/*
 * Copyright (c) 2013, 2019, Red Hat, Inc. All rights reserved.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// #define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>


#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "memory/universe.hpp"

#include "gc/shared/gcArguments.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/memAllocator.hpp"
#include "gc/shared/parallelCleaning.hpp"
#include "gc/shared/plab.hpp"

#include "gc/shenandoah/makoOffsetTable.hpp"
#include "gc/shenandoah/shenandoahAllocTracker.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahMarkCompact.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahMemoryPool.hpp"
#include "gc/shenandoah/shenandoahMetrics.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahPacer.inline.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.inline.hpp"
#include "gc/shenandoah/shenandoahStringDedup.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"
#include "gc/shenandoah/shenandoahCodeRoots.hpp"
#include "gc/shenandoah/shenandoahVMOperations.hpp"
#include "gc/shenandoah/shenandoahWorkGroup.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahAggressiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahCompactHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahPassiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahStaticHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahTraversalHeuristics.hpp"
#if INCLUDE_JFR
#include "gc/shenandoah/shenandoahJfrSupport.hpp"
#endif

#include "memory/metaspace.hpp"
#include "oops/compressedOops.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/vmThread.hpp"
#include "services/mallocTracker.hpp"

// Modified by Haoran
#include "gc/shared/rdmaStructure.inline.hpp"

#include <x86intrin.h>


#ifdef ASSERT
template <class T>
void ShenandoahAssertToSpaceClosure::do_oop_work(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (! CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    shenandoah_assert_not_forwarded(p, obj);
  }
}

void ShenandoahAssertToSpaceClosure::do_oop(narrowOop* p) { do_oop_work(p); }
void ShenandoahAssertToSpaceClosure::do_oop(oop* p)       { do_oop_work(p); }
#endif

class ShenandoahPretouchHeapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  const size_t _page_size;
public:
  ShenandoahPretouchHeapTask(size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Heap"),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions.next();
    while (r != NULL) {
      os::pretouch_memory(r->bottom(), r->end(), _page_size);
      r = _regions.next();
    }
  }
};

class ShenandoahPretouchBitmapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  char* _bitmap_base;
  const size_t _bitmap_size;
  const size_t _page_size;
public:
  ShenandoahPretouchBitmapTask(char* bitmap_base, size_t bitmap_size, size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Bitmap"),
    _bitmap_base(bitmap_base),
    _bitmap_size(bitmap_size),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions.next();
    while (r != NULL) {
      size_t start = r->region_number()       * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      size_t end   = (r->region_number() + 1) * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      assert (end <= _bitmap_size, "end is sane: " SIZE_FORMAT " < " SIZE_FORMAT, end, _bitmap_size);

      os::pretouch_memory(_bitmap_base + start, _bitmap_base + end, _page_size);

      r = _regions.next();
    }
  }
};

class ShenandoahInitializeTask : public AbstractGangTask {
private:
  ShenandoahHeap* _heap;
  int* _memory_index;
  size_t _st_address;
public:
  ShenandoahInitializeTask(ShenandoahHeap* heap, int* memory_index, size_t st_address) :
    AbstractGangTask("Shenandoah Initialize Semeru Heap"),
    _heap(heap),
    _memory_index(memory_index),
    _st_address(st_address) {}

  virtual void work(uint worker_id) {
    
    int new_memory_index = Atomic::add(-1, _memory_index);
    while(new_memory_index >= 0) {
      memset((char *)(_st_address + new_memory_index * ONE_MB * 128), 0, 128 * ONE_MB);
      new_memory_index = Atomic::add(-1, _memory_index);
    }
  }
};

// class ShenandoahEvictTask : public AbstractGangTask {
// private:
//   ShenandoahHeap* _heap;
//   int* _region_index;
// public:
//   ShenandoahEvictTask(ShenandoahHeap* heap, int* region_index) :
//     AbstractGangTask("Shenandoah Prefetch Semeru Heap"),
//     _heap(heap),
//     _region_index(region_index){}

//   virtual void work(uint worker_id) {
    
//     int new_region_index = Atomic::add(1, _region_index);
//     int num_regions = _heap->num_regions();
//     ShenandoahCollectionSet* cset = _heap->collection_set();
//     while(new_region_index <= num_regions) {
//       new_region_index -= 1;
//       if(!cset->is_in_update_set(new_region_index)) {
//         new_region_index = Atomic::add(1, _region_index);
//         continue;
//       }
//       ShenandoahHeapRegion* corr_region = _heap->get_corr_region(new_region_index);
//       ShenandoahHeapRegion* region = _heap->get_region(new_region_index);
//       assert(!region->is_humongous_continuation(), "Invariant!");
//       if(cset->is_in_evac_set(corr_region->region_number()) && !cset->is_in_local_update_set(corr_region)) {
//         assert(!corr_region->is_humongous(), "Cannot be!");
//         _heap->rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//       }
//       else if(!cset->is_in_local_update_set(new_region_index)) {
//         if(region->is_humongous_start()) {
//           oop h_obj = oop(region->bottom());
//           size_t h_size = (h_obj->size()) * 8;
//           size_t aligned_size = ((h_size - 1)/4096 + 1) * 4096;
//           _heap->rdma_evict(0, (char*)region->bottom(), aligned_size);
//         }
//         else {
//           assert(!region->is_humongous(), "invariant!");
//           if(cset->is_in_evac_set(new_region_index)){
//             // _heap->rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//           }
//           else {
//             _heap->rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//           }
//         }

//       }
//       new_region_index = Atomic::add(1, _region_index);
//     }
//   }
// };

class ShenandoahPrefetchTask : public AbstractGangTask {
private:
  ShenandoahHeap* _heap;
  int* _memory_index;
public:
  ShenandoahPrefetchTask(ShenandoahHeap* heap, int* memory_index) :
    AbstractGangTask("Shenandoah Prefetch Semeru Heap"),
    _heap(heap),
    _memory_index(memory_index){}

  virtual void work(uint worker_id) {
    
    int new_memory_index = Atomic::add(1, _memory_index);
    size_t num_regions = _heap->num_regions();
    while(new_memory_index <= 7*1024) {
      new_memory_index -= 1;
      ShenandoahHeapRegion* region = _heap->get_region(new_memory_index);
      ShenandoahHeapRegion* corr_region = _heap->get_region(new_memory_index + num_regions/2);
      if(region->is_empty() || region->is_regular()) {
        corr_region = region;
      }
      else if(corr_region->is_empty() || corr_region->is_regular()) {
        region = corr_region;
      }
      else {
        continue;
      }
      HeapWord* current_pos = region->bottom();
      while (current_pos < region->top()) {
        oop obj = oop(current_pos);
        int size = obj->size();
        current_pos += size;
      }
      if(current_pos != (region->top())) {
        ShouldNotReachHere();
      }

      size_t avail_bytes = (size_t)(region->end())-(size_t)(region->top());
      memset((char*)region->top(), 0, avail_bytes);
      new_memory_index = Atomic::add(1, _memory_index);
    }
  }
};

jint ShenandoahHeap::initialize() {
  initialize_heuristics();


  // Modified by Haoran
  _assure_read_monitor = AssureRead_lock;

  //
  // Figure out heap sizing
  //

  size_t init_byte_size = InitialHeapSize;
  size_t min_byte_size  = MinHeapSize;
  size_t max_byte_size  = MaxHeapSize;
  size_t heap_alignment = HeapAlignment;

  syscall(333, 0xb, 0, 0, 1);
  syscall(SYS_SWAP_STAT_RESET, (char*)(RDMA_DATA_SPACE_START_ADDR), max_byte_size);

  size_t reg_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  if (ShenandoahAlwaysPreTouch) {
    // Enabled pre-touch means the entire heap is committed right away.
    init_byte_size = max_byte_size;
  }

  Universe::check_alignment(max_byte_size,  reg_size_bytes, "Shenandoah heap");
  Universe::check_alignment(init_byte_size, reg_size_bytes, "Shenandoah heap");

  _num_regions = ShenandoahHeapRegion::region_count();

  size_t num_committed_regions = init_byte_size / reg_size_bytes;
  num_committed_regions = MIN2(num_committed_regions, _num_regions);
  assert(num_committed_regions <= _num_regions, "sanity");
  _initial_size = num_committed_regions * reg_size_bytes;

  size_t num_min_regions = min_byte_size / reg_size_bytes;
  num_min_regions = MIN2(num_min_regions, _num_regions);
  assert(num_min_regions <= _num_regions, "sanity");
  _minimum_size = num_min_regions * reg_size_bytes;

  _committed = _initial_size;

  size_t heap_page_size   = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  size_t bitmap_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();

  //
  // Reserve and commit memory for heap
  //

  // Semeru - Allocate Heap at fixed address. Reserve space for RDMA data.
  // Modified by Haoran
  ReservedSpace all_rs;
  ReservedSpace rdma_rs; 
  ReservedSpace heap_rs;
  if(SemeruEnableMemPool && UseCompressedOops==false){
    size_t reserved_for_rdma_data = RDMA_STRUCTURE_SPACE_SIZE;
    all_rs = Universe::reserve_semeru_memory_pool(max_byte_size + reserved_for_rdma_data, heap_alignment);
    initialize_reserved_region((HeapWord*)all_rs.base()+reserved_for_rdma_data/HeapWordSize, (HeapWord*)(all_rs.base() + all_rs.size()));
    rdma_rs = all_rs.first_part(reserved_for_rdma_data);
    heap_rs = all_rs.last_part(reserved_for_rdma_data);   

    os::commit_memory_or_exit(heap_rs.base(), max_byte_size, heap_alignment, false,
                          "Cannot commit heap memory");
    // memset((char *)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE), 0, max_byte_size);
    int memory_index = RDMA_DATA_REGION_NUM * 4 * 8;
    ShenandoahInitializeTask it(this, &memory_index, SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE);
    uint old_num = _workers->active_workers();
    _workers->update_active_workers(16);
    _workers->run_task(&it);
    _workers->update_active_workers(old_num);
    // _workers->update_active_workers(old_num);
    // for(size_t i_th = RDMA_DATA_REGION_NUM * 4; i_th > 0; i_th --) {
    //   memset((char *)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE + (i_th - 1) * ONE_GB), 0, ONE_GB);
    // }
    
    // Haoran: TODO
    //os::commit_memory_or_exit((char*)RDMA_RESERVE_PADDING_OFFSET, RDMA_RESERVE_PADDING_SIZE, false, "Allocator (commit)");  // Commit the space.
    // os::commit_memory_or_exit((char*)SEMERU_START_ADDR,  OFFSET_TABLE_START_ADDR - SEMERU_START_ADDR, false, "Allocator (commit)");  // Commit the space.
  
    memory_index = (int)(((ALIVE_TABLE_OFFSET + ALIVE_TABLE_MAX_SIZE) - 1) / (128 * ONE_MB) + 1);
    size_t meta_data_size = memory_index * 128 * ONE_MB;
    os::commit_memory_or_exit((char*)SEMERU_START_ADDR, meta_data_size, false, "Allocator (commit)");  // Commit the space.
    ShenandoahInitializeTask itm(this, &memory_index, SEMERU_START_ADDR);
    _workers->run_task(&itm);
    _workers->update_active_workers(old_num);

    // memset((char*)SEMERU_START_ADDR, 0, OFFSET_TABLE_START_ADDR - SEMERU_START_ADDR);
    log_debug(semeru)("after commit and memset all rdma structure space, size: 0x%lx",  (size_t)(meta_data_size));

    // os::commit_memory_or_exit(heap_rs.base(), max_byte_size, heap_alignment, false,
    //                   "Cannot commit heap memory");
    // memset((char *)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE), 0, max_byte_size);



    _recv_mem_server_cset 	= new(MEMORY_SERVER_CSET_SIZE, rdma_rs.base() + MEMORY_SERVER_CSET_OFFSET) received_memory_server_cset();
	  _cpu_server_flags				=	new(FLAGS_OF_CPU_SERVER_STATE_SIZE, rdma_rs.base() + FLAGS_OF_CPU_SERVER_STATE_OFFSET) flags_of_cpu_server_state();
    _mem_server_flags       =	new(FLAGS_OF_MEM_SERVER_STATE_SIZE, rdma_rs.base() + FLAGS_OF_MEM_SERVER_STATE_OFFSET) flags_of_mem_server_state();
    // _rdma_write_check_flags = new(FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT, rdma_rs.base() + FLAGS_OF_CPU_WRITE_CHECK_OFFSET) flags_of_rdma_write_check(rdma_rs.base() + FLAGS_OF_CPU_WRITE_CHECK_OFFSET, FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT, sizeof(uint32_t));
    _satb_metadata_mem_server    = new(SATB_METADATA_MEM_SERVER_SIZE, rdma_rs.base() + SATB_METADATA_MEM_SERVER_OFFSET) SATBMetadataMemServer();
    _satb_metadata_cpu_server    = new(SATB_METADATA_CPU_SERVER_SIZE, rdma_rs.base() + SATB_METADATA_CPU_SERVER_OFFSET) SATBMetadataCPUServer();
    
    for(int i = 0; i < NUM_OF_MEMORY_SERVER; i ++) {
      _mem_ref_queues[i] = NULL;
      // _mem_ref_queues[i] =  new (MEM_REF_Q_LEN, i)MemRefQueue();
      // memset((char*)_mem_ref_queues[i], 0, MEM_REF_Q_PER_SIZE);
      // _mem_ref_queues[i]->initialize(i, (HeapWord*)(heap_rs.base() + max_byte_size / NUM_OF_MEMORY_SERVER * i ));
    }

    _mem_ref_current_queue_has_data = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET) RDMAByteFlag(false);
    _mem_ref_current_queue_all_here = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);
    _mem_ref_other_queue_has_data = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + 2 * RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);
    _tracing_current_cycle_processing = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + 3 * RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);

    _assure_read_flags = NEW_C_HEAP_ARRAY(RDMAByteFlag*, _num_regions + 3, mtGC);


    
    size_t index = 0;
    _root_object_queue = new (CROSS_REGION_REF_UPDATE_Q_LEN, index)RootObjectQueue();
    _root_object_queue->initialize(0, (HeapWord*)heap_rs.base());

    _root_object_update_queue = new RootObjectUpdateQueue(64 * 1024, ParallelGCThreads);

    #ifdef ASSERT
		log_debug(semeru, alloc)("%s, Meta data allocation Start\n", __func__);
		log_debug(semeru, alloc)("	received_memory_server_cset 0x%lx, flexible array 0x%lx",  
																							(size_t)_recv_mem_server_cset, (size_t)_recv_mem_server_cset->_region_cset  );
		log_debug(semeru, alloc)("	flags_of_cpu_server_state  0x%lx, flexible array 0x%lx",  
																							(size_t)_cpu_server_flags, (size_t)0 );
    log_debug(semeru, alloc)("	flags_of_mem_server_state  0x%lx, flexible array 0x%lx",  
																							(size_t)_mem_server_flags, (size_t)0 );

		log_debug(semeru, alloc)("%s, Meta data allocation End \n", __func__);
		#endif
  } else {
    heap_rs = Universe::reserve_heap(max_byte_size, heap_alignment);
    initialize_reserved_region((HeapWord*)heap_rs.base(), (HeapWord*) (heap_rs.base() + heap_rs.size()));
  }
  // ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size, heap_alignment);
  // initialize_reserved_region((HeapWord*)heap_rs.base(), (HeapWord*) (heap_rs.base() + heap_rs.size()));
  _heap_region = MemRegion((HeapWord*)heap_rs.base(), heap_rs.size() / HeapWordSize);


  // Modified by Shi
  tty->print("range of heap: 0x%lx to 0x%lx\n", (long)(_heap_region.start()), (long)(_heap_region.end()));


  _heap_region_special = heap_rs.special();

  assert((((size_t) base()) & ShenandoahHeapRegion::region_size_bytes_mask()) == 0,
         "Misaligned heap: " PTR_FORMAT, p2i(base()));

#if SHENANDOAH_OPTIMIZED_OBJTASK
  // The optimized ObjArrayChunkedTask takes some bits away from the full object bits.
  // Fail if we ever attempt to address more than we can.
  if ((uintptr_t)heap_rs.end() >= ObjArrayChunkedTask::max_addressable()) {
    FormatBuffer<512> buf("Shenandoah reserved [" PTR_FORMAT ", " PTR_FORMAT") for the heap, \n"
                          "but max object address is " PTR_FORMAT ". Try to reduce heap size, or try other \n"
                          "VM options that allocate heap at lower addresses (HeapBaseMinAddress, AllocateHeapAt, etc).",
                p2i(heap_rs.base()), p2i(heap_rs.end()), ObjArrayChunkedTask::max_addressable());
    vm_exit_during_initialization("Fatal Error", buf);
  }
#endif

  ReservedSpace sh_rs = heap_rs.first_part(max_byte_size);
  if (!_heap_region_special) {
    // os::commit_memory_or_exit(sh_rs.base(), _initial_size, heap_alignment, false,
    //                           "Cannot commit heap memory");
    // memset((char *)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE), 0, RDMA_STRUCTURE_SPACE_SIZE * 2);

  }

  //
  // Reserve and commit memory for bitmap(s)
  //

  _bitmap_size = MarkBitMap::compute_size(heap_rs.size());
  _bitmap_size = align_up(_bitmap_size, bitmap_page_size);

  size_t bitmap_bytes_per_region = reg_size_bytes / MarkBitMap::heap_map_factor();

  guarantee(bitmap_bytes_per_region != 0,
            "Bitmap bytes per region should not be zero");
  guarantee(is_power_of_2(bitmap_bytes_per_region),
            "Bitmap bytes per region should be power of two: " SIZE_FORMAT, bitmap_bytes_per_region);

  if (bitmap_page_size > bitmap_bytes_per_region) {
    _bitmap_regions_per_slice = bitmap_page_size / bitmap_bytes_per_region;
    _bitmap_bytes_per_slice = bitmap_page_size;
  } else {
    _bitmap_regions_per_slice = 1;
    _bitmap_bytes_per_slice = bitmap_bytes_per_region;
  }

  guarantee(_bitmap_regions_per_slice >= 1,
            "Should have at least one region per slice: " SIZE_FORMAT,
            _bitmap_regions_per_slice);

  guarantee(((_bitmap_bytes_per_slice) % bitmap_page_size) == 0,
            "Bitmap slices should be page-granular: bps = " SIZE_FORMAT ", page size = " SIZE_FORMAT,
            _bitmap_bytes_per_slice, bitmap_page_size);

  // Modified by Haoran
  // ReservedSpace bitmap(_bitmap_size, bitmap_page_size);
  // MemTracker::record_virtual_memory_type(bitmap.base(), mtGC);
  // _bitmap_region = MemRegion((HeapWord*) bitmap.base(), bitmap.size() / HeapWordSize);
  // _bitmap_region_special = bitmap.special();

  // size_t bitmap_init_commit = _bitmap_bytes_per_slice *
  //                             align_up(num_committed_regions, _bitmap_regions_per_slice) / _bitmap_regions_per_slice;
  // bitmap_init_commit = MIN2(_bitmap_size, bitmap_init_commit);
  // if (!_bitmap_region_special) {
  //   os::commit_memory_or_exit((char *) _bitmap_region.start(), bitmap_init_commit, bitmap_page_size, false,
  //                             "Cannot commit bitmap memory");
  // }

  char* bitmap_base_addr = rdma_rs.base() + ALIVE_BITMAP_OFFSET;
  MemTracker::record_virtual_memory_type(bitmap_base_addr, mtGC);
  _bitmap_region = MemRegion((HeapWord*) bitmap_base_addr, ALIVE_BITMAP_SIZE / HeapWordSize);
  // _bitmap_region_special = bitmap.special();
  size_t bitmap_init_commit = _bitmap_bytes_per_slice * align_up(num_committed_regions, _bitmap_regions_per_slice) / _bitmap_regions_per_slice;
  bitmap_init_commit = MIN2(_bitmap_size, bitmap_init_commit);
  // if (!_bitmap_region_special) {
  // os::commit_memory_or_exit((char *) _bitmap_region.start(), _bitmap_region.byte_size(), bitmap_page_size, false,
  //                           "Cannot commit bitmap memory");
  
  _marking_context = new ShenandoahMarkingContext(_heap_region, _bitmap_region, _num_regions);
  // memset(_bitmap_region.start(), 0, _bitmap_region.byte_size());

  log_debug(semeru,rdma)("Reset bitmap server 0: 0x%lx, byte_size: 0x%lx", 	(size_t)_bitmap_region.start(), _bitmap_region.byte_size());
  
  // os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET), ALIVE_TABLE_MAX_SIZE, false, "Cannot commit alive map memory");
  // memset((char*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET), 0, ALIVE_TABLE_MAX_SIZE);
  _alive_table = new ShenandoahAliveTable(this, (HeapWord*)heap_rs.base(), MAX_HEAP_SIZE/HeapWordSize, reg_size_bytes/HeapWordSize, ALIVE_TABLE_PAGE_SIZE, LOG_ALIVE_TABLE_PAGE_SIZE, (uint*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET));





  // for(int i = 0; i >= 0; i++) {
  //   log_debug(semeru,rdma)("Read bitmap server 0: 0x%lx, byte_size: 0x%lx", 	(size_t)_bitmap_region.start(), (size_t)0x1e800000 + i * 4096);
  //   // rdma_read(0, (char*)_bitmap_region.start() , _bitmap_region.byte_size()-i*1024*1024);
  //   rdma_read(0, (char*)_bitmap_region.start() , (size_t)0x1e800000 + i * 4096);
    
  // }
  // for(size_t i = 0x40007e7ff000; i; i+=4096) {
  //   log_debug(semeru,rdma)("Read bitmap server 0: 0x%lx, byte_size: 0x%lx", 	((size_t)i) , (size_t)4096);
  //   // rdma_read(0, (char*)_bitmap_region.start() , _bitmap_region.byte_size()-i*1024*1024);
  //   rdma_read(0, (char*)i , (size_t)4*1024);
    
  // }


  
  // log_debug(semeru,rdma)("Read bitmap: 0x%lx, byte_size: 0x%lx", 	(size_t)_bitmap_region.start(), _bitmap_region.byte_size());
   
  // Haoran: debug
  // rdma_read(1, (char*)_bitmap_region.start() , _bitmap_region.byte_size());
   
  // rdma_read(1, (char*)(((size_t)_bitmap_region.start()) + 1 * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER)), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER-4096);
  // log_debug(semeru,rdma)("Read bitmap: 0x%lx, byte_size: 0x%lx", 	(size_t)((char*)(((size_t)_bitmap_region.start()) + 1 * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER))), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER-4096);
    

  // os::commit_memory_or_exit((char *)(SEMERU_START_ADDR + SATB_BUFFER_OFFSET), SATB_BUFFER_SIZE_LIMIT, false,
  //                           "Cannot commit satb queue memory");
  // memset((char *)(SEMERU_START_ADDR + SATB_BUFFER_OFFSET), 0, SATB_BUFFER_SIZE_LIMIT);

  // mhrcr: useless normal              disabled by default
  if (ShenandoahVerify) {
    ReservedSpace verify_bitmap(_bitmap_size, bitmap_page_size);
    if (!verify_bitmap.special()) {
      os::commit_memory_or_exit(verify_bitmap.base(), verify_bitmap.size(), bitmap_page_size, false,
                                "Cannot commit verification bitmap memory");
    }
    MemTracker::record_virtual_memory_type(verify_bitmap.base(), mtGC);
    MemRegion verify_bitmap_region = MemRegion((HeapWord *) verify_bitmap.base(), verify_bitmap.size() / HeapWordSize);
    _verification_bit_map.initialize(_heap_region, verify_bitmap_region);
    _verifier = new ShenandoahVerifier(this, &_verification_bit_map);
  }

  // Reserve aux bitmap for use in object_iterate(). We don't commit it here.
  ReservedSpace aux_bitmap(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(aux_bitmap.base(), mtGC);
  _aux_bitmap_region = MemRegion((HeapWord*) aux_bitmap.base(), aux_bitmap.size() / HeapWordSize);
  _aux_bitmap_region_special = aux_bitmap.special();
  _aux_bit_map.initialize(_heap_region, _aux_bitmap_region);

  //
  // Create regions and region sets
  //

  _regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _num_regions, mtGC);
  _free_set = new ShenandoahFreeSet(this, _num_regions);

  _taken = NEW_C_HEAP_ARRAY(char, _num_regions+10, mtGC);


  // Modified by Haoran for sync
  // _collection_set = new ShenandoahCollectionSet(this, sh_rs.base(), sh_rs.size());
  // os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + CSET_SYNC_MAP_OFFSET), CSET_SYNC_MAP_SIZE, false, "Allocator (commit)");
  // os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + COMPACTED_MAP_OFFSET), COMPACTED_MAP_SIZE, false, "Allocator (commit)");
  
  _collection_set = new ShenandoahCollectionSet(this, sh_rs.base(), sh_rs.size(), (char*)(SEMERU_START_ADDR + CSET_SYNC_MAP_OFFSET), (char*)(SEMERU_START_ADDR + COMPACTED_MAP_OFFSET));


  {
    ShenandoahHeapLocker locker(lock());

    size_t size_words = ShenandoahHeapRegion::region_size_words();

    for (size_t i = 0; i < _num_regions; i++) {
      HeapWord* start = (HeapWord*)sh_rs.base() + size_words * i;
      bool is_committed = i < num_committed_regions;
      ShenandoahHeapRegion* r = new ShenandoahHeapRegion(this, start, size_words, i, is_committed);
      _marking_context->initialize_top_at_mark_start(r);
      _regions[i] = r;
      assert(!collection_set()->is_in_update_set(i), "New region should not be in collection set");
    }

    // Initialize to complete
    _marking_context->mark_complete();

    _free_set->rebuild();
  }

  if (ShenandoahAlwaysPreTouch) {
    assert(!AlwaysPreTouch, "Should have been overridden");

    // For NUMA, it is important to pre-touch the storage under bitmaps with worker threads,
    // before initialize() below zeroes it with initializing thread. For any given region,
    // we touch the region and the corresponding bitmaps from the same thread.
    ShenandoahPushWorkerScope scope(workers(), _max_workers, false);

    size_t pretouch_heap_page_size = heap_page_size;
    size_t pretouch_bitmap_page_size = bitmap_page_size;

#ifdef LINUX
    // UseTransparentHugePages would madvise that backing memory can be coalesced into huge
    // pages. But, the kernel needs to know that every small page is used, in order to coalesce
    // them into huge one. Therefore, we need to pretouch with smaller pages.
    if (UseTransparentHugePages) {
      pretouch_heap_page_size = (size_t)os::vm_page_size();
      pretouch_bitmap_page_size = (size_t)os::vm_page_size();
    }
#endif

    // OS memory managers may want to coalesce back-to-back pages. Make their jobs
    // simpler by pre-touching continuous spaces (heap and bitmap) separately.

    // log_info(gc, init)("Pretouch bitmap: " SIZE_FORMAT " regions, " SIZE_FORMAT " bytes page",
                      //  _num_regions, pretouch_bitmap_page_size);
    // Modified by Haoran2
    // ShenandoahPretouchBitmapTask bcl(bitmap.base(), _bitmap_size, pretouch_bitmap_page_size);
    // ShenandoahPretouchBitmapTask bcl(bitmap_base_addr, _bitmap_size, pretouch_bitmap_page_size);
    // _workers->run_task(&bcl);

    log_info(gc, init)("Pretouch heap: " SIZE_FORMAT " regions, " SIZE_FORMAT " bytes page",
                       _num_regions, pretouch_heap_page_size);
    // ShenandoahPretouchHeapTask hcl(pretouch_heap_page_size);
    // _workers->run_task(&hcl);
  }

  //
  // Initialize the rest of GC subsystems
  //

  _liveness_cache = NEW_C_HEAP_ARRAY(jushort*, _max_workers, mtGC);
  for (uint worker = 0; worker < _max_workers; worker++) {
    _liveness_cache[worker] = NEW_C_HEAP_ARRAY(jushort, _num_regions, mtGC);
    Copy::fill_to_bytes(_liveness_cache[worker], _num_regions * sizeof(jushort));
  }

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  ShenandoahBarrierSet::satb_mark_queue_set().initialize(this,
                                                         SATB_Q_CBL_mon,
                                                         20 /* G1SATBProcessCompletedThreshold */,
                                                         60 /* G1SATBBufferEnqueueingThresholdPercent */);

  _monitoring_support = new ShenandoahMonitoringSupport(this);
  _phase_timings = new ShenandoahPhaseTimings();
  ShenandoahStringDedup::initialize();
  ShenandoahCodeRoots::initialize();

  if (ShenandoahAllocationTrace) {
    _alloc_tracker = new ShenandoahAllocTracker();
  }

  if (ShenandoahPacing) {
    _pacer = new ShenandoahPacer(this);
    _pacer->setup_for_idle();
  } else {
    _pacer = NULL;
  }

  _traversal_gc = heuristics()->can_do_traversal_gc() ?
                  new ShenandoahTraversalGC(this, _num_regions) :
                  NULL;

  _control_thread = new ShenandoahControlThread();

  log_info(gc, init)("Initialize Shenandoah heap: " SIZE_FORMAT "%s initial, " SIZE_FORMAT "%s min, " SIZE_FORMAT "%s max",
                     byte_size_in_proper_unit(_initial_size),  proper_unit_for_byte_size(_initial_size),
                     byte_size_in_proper_unit(_minimum_size),  proper_unit_for_byte_size(_minimum_size),
                     byte_size_in_proper_unit(max_capacity()), proper_unit_for_byte_size(max_capacity())
  );

  log_info(gc, init)("Safepointing mechanism: %s",
                     SafepointMechanism::uses_thread_local_poll() ? "thread-local poll" :
                     (SafepointMechanism::uses_global_page_poll() ? "global-page poll" : "unknown"));

  return JNI_OK;
}

void ShenandoahHeap::initialize_heuristics() {
  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      _heuristics = new ShenandoahAggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "static") == 0) {
      _heuristics = new ShenandoahStaticHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      _heuristics = new ShenandoahAdaptiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "passive") == 0) {
      _heuristics = new ShenandoahPassiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "compact") == 0) {
      _heuristics = new ShenandoahCompactHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "traversal") == 0) {
      _heuristics = new ShenandoahTraversalHeuristics();
    } else {
      vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option");
    }

    if (_heuristics->is_diagnostic() && !UnlockDiagnosticVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                      _heuristics->name()));
    }
    if (_heuristics->is_experimental() && !UnlockExperimentalVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                      _heuristics->name()));
    }
    log_info(gc, init)("Shenandoah heuristics: %s",
                       _heuristics->name());
  } else {
      ShouldNotReachHere();
  }

}

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) :
  CollectedHeap(),
  _initial_size(0),
  _used(0),
  _committed(0),
  _bytes_allocated_since_gc_start(0),
  _max_workers(MAX2(ConcGCThreads, ParallelGCThreads)),
  _workers(NULL),
  _safepoint_workers(NULL),
  _heap_region_special(false),
  _num_regions(0),
  _regions(NULL),
  _update_refs_iterator(this),
  _control_thread(NULL),
  _shenandoah_policy(policy),
  _heuristics(NULL),
  _free_set(NULL),
  _scm(new ShenandoahConcurrentMark()),
  _traversal_gc(NULL),
  _full_gc(new ShenandoahMarkCompact()),
  _pacer(NULL),
  _verifier(NULL),
  _alloc_tracker(NULL),
  _phase_timings(NULL),
  _monitoring_support(NULL),
  _memory_pool(NULL),
  _stw_memory_manager("Shenandoah Pauses", "end of GC pause"),
  _cycle_memory_manager("Shenandoah Cycles", "end of GC cycle"),
  _gc_timer(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
  _soft_ref_policy(),
  _log_min_obj_alignment_in_bytes(LogMinObjAlignmentInBytes),
  _ref_processor(NULL),
  _marking_context(NULL),
  _bitmap_size(0),
  _bitmap_regions_per_slice(0),
  _bitmap_bytes_per_slice(0),
  _bitmap_region_special(false),
  _aux_bitmap_region_special(false),
  _liveness_cache(NULL),
  _collection_set(NULL),
  _alive_table(NULL),
  pretouch_time(0),
  gc_start_threshold(0),
  is_doing_flush(false)
{
  log_info(gc, init)("GC threads: " UINT32_FORMAT " parallel, " UINT32_FORMAT " concurrent", ParallelGCThreads, ConcGCThreads);
  log_info(gc, init)("Reference processing: %s", ParallelRefProcEnabled ? "parallel" : "serial");

  BarrierSet::set_barrier_set(new ShenandoahBarrierSet(this));

  _max_workers = MAX2(_max_workers, 1U);
  _workers = new ShenandoahWorkGang("Shenandoah GC Threads", _max_workers,
                            /* are_GC_task_threads */ true,
                            /* are_ConcurrentGC_threads */ true);
  if (_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _workers->initialize_workers();
  }

  if (ShenandoahParallelSafepointThreads > 1) {
    _safepoint_workers = new ShenandoahWorkGang("Safepoint Cleanup Thread",
                                                ShenandoahParallelSafepointThreads,
                      /* are_GC_task_threads */ false,
                 /* are_ConcurrentGC_threads */ false);
    _safepoint_workers->initialize_workers();
  }
}

#ifdef _MSC_VER
#pragma warning( pop )
#endif

class ShenandoahResetBitmapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;

public:
  ShenandoahResetBitmapTask() :
    AbstractGangTask("Parallel Reset Bitmap Task") {}

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions.next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahMarkingContext* const ctx = heap->marking_context();
    while (region != NULL) {
      if (heap->is_bitmap_slice_committed(region)) {
        ctx->clear_bitmap(region);
      }
      // if(region->offset_table()->region_id() == region->region_number()) {
      //   region->offset_table()->clear();
      // }
      region->sync_between_mem_and_cpu()->_evac_start = NULL;
      region->_selected_to = false;
      region = _regions.next();
    }
  }
};

void ShenandoahHeap::reset_mark_bitmap() {
  assert_gc_workers(_workers->active_workers());
  mark_incomplete_marking_context();

  ShenandoahResetBitmapTask task;
  _workers->run_task(&task);
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print_cr("Shenandoah Heap");
  st->print_cr(" " SIZE_FORMAT "K total, " SIZE_FORMAT "K committed, " SIZE_FORMAT "K used",
               max_capacity() / K, committed() / K, used() / K);
  st->print_cr(" " SIZE_FORMAT " x " SIZE_FORMAT"K regions",
               num_regions(), ShenandoahHeapRegion::region_size_bytes() / K);

  st->print("Status: ");
  if (has_forwarded_objects())               st->print("has forwarded objects, ");
  if (is_concurrent_mark_in_progress())      st->print("marking, ");
  if (is_evacuation_in_progress())           st->print("evacuating, ");
  if (is_update_refs_in_progress())          st->print("updating refs, ");
  if (is_concurrent_traversal_in_progress()) st->print("traversal, ");
  if (is_degenerated_gc_in_progress())       st->print("degenerated gc, ");
  if (is_full_gc_in_progress())              st->print("full gc, ");
  if (is_full_gc_move_in_progress())         st->print("full gc move, ");

  if (cancelled_gc()) {
    st->print("cancelled");
  } else {
    st->print("not cancelled");
  }
  st->cr();

  st->print_cr("Reserved region:");
  st->print_cr(" - [" PTR_FORMAT ", " PTR_FORMAT ") ",
               p2i(reserved_region().start()),
               p2i(reserved_region().end()));

  ShenandoahCollectionSet* cset = collection_set();
  st->print_cr("Collection set:");
  if (cset != NULL) {
    st->print_cr(" - map (vanilla): " PTR_FORMAT, p2i(cset->map_address()));
    st->print_cr(" - map (biased):  " PTR_FORMAT, p2i(cset->biased_map_address()));
  } else {
    st->print_cr(" (NULL)");
  }

  st->cr();
  MetaspaceUtils::print_on(st);

  if (Verbose) {
    print_heap_regions_on(st);
  }
}

class ShenandoahInitWorkerGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread != NULL, "Sanity");
    assert(thread->is_Worker_thread(), "Only worker thread expected");
    ShenandoahThreadLocalData::initialize_gclab(thread);
  }
};

void ShenandoahHeap::post_initialize() {
  CollectedHeap::post_initialize();
  MutexLocker ml(Threads_lock);

  ShenandoahInitWorkerGCLABClosure init_gclabs;
  _workers->threads_do(&init_gclabs);

  // gclab can not be initialized early during VM startup, as it can not determinate its max_size.
  // Now, we will let WorkGang to initialize gclab when new worker is created.
  _workers->set_initialize_gclab();

  _scm->initialize(_max_workers);
  _full_gc->initialize(_gc_timer);

  ref_processing_init();

  _heuristics->initialize();

  JFR_ONLY(ShenandoahJFRSupport::register_jfr_type_serializers());
}

size_t ShenandoahHeap::used() const {
  return OrderAccess::load_acquire(&_used);
}

size_t ShenandoahHeap::committed() const {
  OrderAccess::acquire();
  return _committed;
}

void ShenandoahHeap::increase_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed += bytes;
}

void ShenandoahHeap::decrease_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed -= bytes;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  Atomic::add(bytes, &_used);
}

void ShenandoahHeap::set_used(size_t bytes) {
  OrderAccess::release_store_fence(&_used, bytes);
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(used() >= bytes, "never decrease heap size by more than we've left");
  Atomic::sub(bytes, &_used);
}

void ShenandoahHeap::increase_allocated(size_t bytes) {
  Atomic::add(bytes, &_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::notify_mutator_alloc_words(size_t words, bool waste) {
  size_t bytes = words * HeapWordSize;
  if (!waste) {
    increase_used(bytes);
  }
  increase_allocated(bytes);
  if (ShenandoahPacing) {
    control_thread()->pacing_notify_alloc(words);
    if (waste) {
      pacer()->claim_for_alloc(words, true);
    }
  }
}

size_t ShenandoahHeap::capacity() const {
  return committed();
}

size_t ShenandoahHeap::max_capacity() const {
  return _num_regions * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahHeap::min_capacity() const {
  return _minimum_size;
}

size_t ShenandoahHeap::initial_capacity() const {
  return _initial_size;
}

bool ShenandoahHeap::is_in(const void* p) const {
  HeapWord* heap_base = (HeapWord*) base();
  HeapWord* last_region_end = heap_base + ShenandoahHeapRegion::region_size_words() * num_regions();
  return p >= heap_base && p < last_region_end;
}

void ShenandoahHeap::op_uncommit(double shrink_before) {
  assert (ShenandoahUncommit, "should be enabled");

  // Application allocates from the beginning of the heap, and GC allocates at
  // the end of it. It is more efficient to uncommit from the end, so that applications
  // could enjoy the near committed regions. GC allocations are much less frequent,
  // and therefore can accept the committing costs.

  size_t count = 0;
  for (size_t i = num_regions(); i > 0; i--) { // care about size_t underflow
    ShenandoahHeapRegion* r = get_region(i - 1);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      ShenandoahHeapLocker locker(lock());
      if (r->is_empty_committed()) {
        // Do not uncommit below minimal capacity
        if (committed() < min_capacity() + ShenandoahHeapRegion::region_size_bytes()) {
          break;
        }

        r->make_uncommitted();
        count++;
      }
    }
    SpinPause(); // allow allocators to take the lock
  }

  if (count > 0) {
    control_thread()->notify_heap_changed();
  }
}

HeapWord* ShenandoahHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // New object should fit the GCLAB size
  size_t min_size = MAX2(size, PLAB::min_size());

  // Figure out size of new GCLAB, looking back at heuristics. Expand aggressively.
  size_t new_size = ShenandoahThreadLocalData::gclab_size(thread) * 2;
  new_size = MIN2(new_size, PLAB::max_size());
  new_size = MAX2(new_size, PLAB::min_size());

  new_size = ShenandoahGCLABSize;

  // Record new heuristic value even if we take any shortcut. This captures
  // the case when moderately-sized objects always take a shortcut. At some point,
  // heuristics should catch up with them.
  ShenandoahThreadLocalData::set_gclab_size(thread, new_size);

  if (new_size < size) {
    // New size still does not fit the object. Fall back to shared allocation.
    // This avoids retiring perfectly good GCLABs, when we encounter a large object.
    return NULL;
  }

  // Retire current GCLAB, and allocate a new one.
  PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
  gclab->retire();

  size_t actual_size = 0;
  HeapWord* gclab_buf = allocate_new_gclab(min_size, new_size, &actual_size);
  if (gclab_buf == NULL) {
    return NULL;
  }

  assert (size <= actual_size, "allocation should fit");

  if (ZeroTLAB) {
    // ..and clear it.
    Copy::zero_to_words(gclab_buf, actual_size);
  } else {
    // ...and zap just allocated object.
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(gclab_buf + hdr_size, actual_size - hdr_size, badHeapWordVal);
#endif // ASSERT
  }
  gclab->set_buf(gclab_buf, actual_size);
  return gclab->allocate(size);
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t min_size,
                                            size_t requested_size,
                                            size_t* actual_size) {
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_tlab(min_size, requested_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t min_size,
                                             size_t word_size,
                                             size_t* actual_size) {
  // Haoran: debug
  // ShouldNotReachHere();

  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_gclab(min_size, word_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

ShenandoahHeap* ShenandoahHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(heap->kind() == CollectedHeap::Shenandoah, "not a shenandoah heap");
  return (ShenandoahHeap*) heap;
}

ShenandoahHeap* ShenandoahHeap::heap_no_check() {
  CollectedHeap* heap = Universe::heap();
  return (ShenandoahHeap*) heap;
}

HeapWord* ShenandoahHeap::allocate_memory(ShenandoahAllocRequest& req) {
  ShenandoahAllocTrace trace_alloc(req.size(), req.type());

  intptr_t pacer_epoch = 0;
  bool in_new_region = false;
  HeapWord* result = NULL;

  if (req.is_mutator_alloc()) {
    if (ShenandoahPacing) {
      pacer()->pace_for_alloc(req.size());
      pacer_epoch = pacer()->epoch();
    }
    if (ShenandoahSemeruPacing && (is_concurrent_mark_in_progress() || is_evacuation_in_progress())) {

      size_t max = ShenandoahPacingMaxDelay;
      double start = os::elapsedTime();
      size_t total = 0;
      size_t cur = 0;
      while (true) {
        cur = cur * 2;
        if (total + cur > max) {
          cur = (max > total) ? (max - total) : 0;
        }
        cur = MAX2<size_t>(1, cur);
        os::sleep(Thread::current(), cur, true);
        double end = os::elapsedTime();
        total = (size_t)((end - start) * 1000);
        if (total > max) {
          break;
        }
      }
    }

    if (!ShenandoahAllocFailureALot || !should_inject_alloc_failure()) {
      result = allocate_memory_under_lock(req, in_new_region);
    }

    // Allocation failed, block until control thread reacted, then retry allocation.
    //
    // It might happen that one of the threads requesting allocation would unblock
    // way later after GC happened, only to fail the second allocation, because
    // other threads have already depleted the free storage. In this case, a better
    // strategy is to try again, as long as GC makes progress.
    //
    // Then, we need to make sure the allocation was retried after at least one
    // Full GC, which means we want to try more than ShenandoahFullGCThreshold times.

    size_t tries = 0;

    while (result == NULL && _progress_last_gc.is_set()) {
      tries++;
      control_thread()->handle_alloc_failure(req.size());
      result = allocate_memory_under_lock(req, in_new_region);
    }

    while (result == NULL && tries <= ShenandoahFullGCThreshold) {
      tries++;
      control_thread()->handle_alloc_failure(req.size());
      result = allocate_memory_under_lock(req, in_new_region);
    }

  } else {
    assert(req.is_gc_alloc(), "Can only accept GC allocs here");
    result = allocate_memory_under_lock(req, in_new_region);
    // Do not call handle_alloc_failure() here, because we cannot block.
    // The allocation failure would be handled by the LRB slowpath with handle_alloc_failure_evac().
  }

  if (in_new_region) {
    control_thread()->notify_heap_changed();
  }

  if (result != NULL) {
    size_t requested = req.size();
    size_t actual = req.actual_size();

    assert (req.is_lab_alloc() || (requested == actual),
            "Only LAB allocations are elastic: %s, requested = " SIZE_FORMAT ", actual = " SIZE_FORMAT,
            ShenandoahAllocRequest::alloc_type_to_string(req.type()), requested, actual);

    if (req.is_mutator_alloc()) {
      notify_mutator_alloc_words(actual, false);

      // If we requested more than we were granted, give the rest back to pacer.
      // This only matters if we are in the same pacing epoch: do not try to unpace
      // over the budget for the other phase.
      if (ShenandoahPacing && (pacer_epoch > 0) && (requested > actual)) {
        pacer()->unpace_for_alloc(pacer_epoch, requested - actual);
      }
    } else {
      increase_used(actual*HeapWordSize);
    }
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_in_safepoint(ShenandoahAllocRequest& req) {
  ShenandoahAllocTrace trace_alloc(req.size(), req.type());

  intptr_t pacer_epoch = 0;
  bool in_new_region = false;
  HeapWord* result = NULL;

  if (req.is_mutator_alloc()) {
    ShouldNotReachHere();
  } else {
    assert(req.is_gc_alloc(), "Can only accept GC allocs here");
    assert_heaplock_owned_by_current_thread();
    result = _free_set->allocate(req, in_new_region);
  }

  // if (in_new_region) {
  //   control_thread()->notify_heap_changed();
  // }

  if (result != NULL) {
    size_t requested = req.size();
    size_t actual = req.actual_size();

    assert (req.is_lab_alloc() || (requested == actual),
            "Only LAB allocations are elastic: %s, requested = " SIZE_FORMAT ", actual = " SIZE_FORMAT,
            ShenandoahAllocRequest::alloc_type_to_string(req.type()), requested, actual);

    increase_used(actual*HeapWordSize);
  }
  else {
    return NULL;
    // ShouldNotReachHere();
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_under_lock(ShenandoahAllocRequest& req, bool& in_new_region) {
  ShenandoahHeapLocker locker(lock());
  HeapWord* result = _free_set->allocate(req, in_new_region);
  // if(in_new_region) {

  //   ShenandoahHeapRegion* region = heap_region_containing(result);
  //   ShenandoahHeapRegion* corr_region = get_corr_region(region->region_number());
  //   if(!region->pretouched) {
  //     if(result != region->bottom() || corr_region->pretouched) {
  //       ShouldNotReachHere();
  //     }
      
  //     memset((char*)region->bottom(), 0, region->region_size_bytes());
  //     memset((char*)corr_region->bottom(), 0, region->region_size_bytes());
  //     region->pretouched = true;
  //     corr_region->pretouched = true;
  //   }
  // }

  return result;
}

HeapWord* ShenandoahHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared(size);
  return allocate_memory(req);
}

MetaWord* ShenandoahHeap::satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
                                                             size_t size,
                                                             Metaspace::MetadataType mdtype) {
  MetaWord* result;

  // Inform metaspace OOM to GC heuristics if class unloading is possible.
  if (heuristics()->can_unload_classes()) {
    ShenandoahHeuristics* h = heuristics();
    h->record_metaspace_oom();
  }

  // Expand and retry allocation
  result = loader_data->metaspace_non_null()->expand_and_allocate(size, mdtype);
  if (result != NULL) {
    return result;
  }

  // Start full GC
  collect(GCCause::_metadata_GC_clear_soft_refs);

  // Retry allocation
  result = loader_data->metaspace_non_null()->allocate(size, mdtype);
  if (result != NULL) {
    return result;
  }

  // Expand and retry allocation
  result = loader_data->metaspace_non_null()->expand_and_allocate(size, mdtype);
  if (result != NULL) {
    return result;
  }

  // Out of memory
  return NULL;
}

class ShenandoahConcurrentEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* const _heap;
  Thread* const _thread;
public:
  ShenandoahConcurrentEvacuateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) {
    shenandoah_assert_marked(NULL, p);
    // if (oopDesc::equals_raw(p, ShenandoahBarrierSet::resolve_forwarded_not_null(p))) {
    
    if(ShenandoahForwarding::is_forwarded(p)) {
      // Have been evacuated by roots
      size_t size = (size_t) p->size();
      oop orig_target_obj = _heap->alive_table()->get_target_address(p);
      assert(orig_target_obj != ShenandoahForwarding::get_forwardee(p), "Invariant!");
#ifdef RELEASE_CHECK
      if(orig_target_obj == ShenandoahForwarding::get_forwardee(p)) {
        ShouldNotReachHere();
      }
#endif
      // _heap->fill_with_dummy_object((HeapWord*)orig_target_obj, ((HeapWord*)orig_target_obj) + size);
    }
    else {
      assert(!ShenandoahForwarding::is_forwarded(p), "Invariant!");
      // if (!p->is_forwarded()) {
      oop new_p = _heap->evacuate_object(p, _thread);
    }
    
  }
};

class ShenandoahConcurrentUpdateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* const _heap;
  Thread* const _thread;
public:
  ShenandoahConcurrentUpdateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) {
    shenandoah_assert_marked(NULL, p);
    _heap->update_object(p);
  }
};


inline void clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}

void ShenandoahHeap::flush_cache_all() {
  // syscall(INVD_CACHE, 0, (char*)0, 0);
}

void ShenandoahHeap::flush_cache_for_region(ShenandoahHeapRegion* r) {
  ShouldNotCallThis();
  // _mm_mfence();
  // IndirectionTable* table = Universe::table(r->region_number());
  // for(size_t i = table->table_base; i < table->table_end; i += 64) {
  //   clflush((char*)i);
  // }
  // ShenandoahHeapRegion* corr_region = get_corr_region(r->region_number());
  // for(size_t i = (size_t)corr_region->bottom(); i < (size_t)corr_region->top(); i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

void ShenandoahHeap::flush_cache_bitmap() {
  ShouldNotCallThis();
  // _mm_mfence();
  // for(size_t i = SEMERU_START_ADDR + ALIVE_BITMAP_OFFSET; i < SEMERU_START_ADDR + ALIVE_BITMAP_OFFSET + ALIVE_BITMAP_SIZE; i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

void ShenandoahHeap::flush_cache_range(size_t start, size_t end) {
  ShouldNotCallThis();
  // _mm_mfence();
  // for(size_t i = start/64*64; i < end; i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

// class ShenandoahUpdateTask : public AbstractGangTask {
// private:
//   ShenandoahHeap* const _sh;
//   ShenandoahCollectionSet* const _cs;
// public:
//   ShenandoahUpdateTask(ShenandoahHeap* sh,
//                            ShenandoahCollectionSet* cs) :
//     AbstractGangTask("Parallel Update Task"),
//     _sh(sh),
//     _cs(cs)
//   {}

//   void work(uint worker_id) {
//     ShenandoahConcurrentUpdateRegionObjectClosure update_cl(_sh);
//     ShenandoahHeapRegion* r;
//     // Modified by Haoran for remote compaction
//     while ((r =_cs->claim_next()) != NULL) {
//       if(_cs->is_in_local_update_set(r)) {
//         assert(r->top() > r->bottom(), "all-garbage regions are reclaimed early");
//         _sh->marked_object_iterate(r, &update_cl);
//         // log_debug(semeru)("Finish updating for region %lx", r->region_number());
//       }
//     }
//   }
// };

// class ShenandoahEvacuationTask : public AbstractGangTask {
// private:
//   ShenandoahHeap* const _sh;
//   ShenandoahCollectionSet* const _cs;
//   bool _concurrent;
// public:
//   ShenandoahEvacuationTask(ShenandoahHeap* sh,
//                            ShenandoahCollectionSet* cs,
//                            bool concurrent) :
//     AbstractGangTask("Parallel Evacuation Task"),
//     _sh(sh),
//     _cs(cs),
//     _concurrent(concurrent)
//   {}

//   void work(uint worker_id) {
//     if (_concurrent) {
//       ShenandoahConcurrentWorkerSession worker_session(worker_id);
//       ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
//       ShenandoahEvacOOMScope oom_evac_scope;
//       do_work();
//     } else {
//       ShenandoahParallelWorkerSession worker_session(worker_id);
//       ShenandoahEvacOOMScope oom_evac_scope;
//       do_work();
//     }
//   }

// private:
//   void do_work() {
//     ShenandoahConcurrentEvacuateRegionObjectClosure cl(_sh);
//     ShenandoahConcurrentUpdateRegionObjectClosure update_cl(_sh);
//     ShenandoahHeapRegion* r;
//     while ((r =_cs->claim_next()) != NULL) {
//       // assert(r->has_live(), "all-garbage regions are reclaimed early");
//       assert(_cs->is_in_local_update_set(r), "Invariant!");
//       if(_cs->is_in_evac_set(r)) {
//         assert(r->has_live(), "all-garbage regions are reclaimed early");
//         assert(_sh->marking_context()->top_at_mark_start(r) == r->top(), "Do not evacuate regions that have unmarked objects!");
//         r->_evac_top = r->bottom();
//         _sh->marked_object_iterate(r, &cl);
//         // _sh->fill_with_dummy_object(r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words(), r->sync_between_mem_and_cpu()->_evac_start + align_up(r->get_live_data_words(), 512), true);
// #ifdef RELEASE_CHECK
//         log_debug(semeru)("Finish compaction updating for region %lu", r->region_number());
// #endif
//       }
//       else if(_cs->is_in_update_set(r) && !r->_selected_to && !r->is_humongous_continuation()) {
//         assert(!_cs->is_in_evac_set(r->region_number()), "Invariant!");
//         assert(r->has_live(), "all-garbage regions are reclaimed early");
//         assert(!r->is_humongous_continuation(), "Invariant!");
//         assert(!r->is_cset(), "Invariant!");
//         _sh->marked_object_iterate(r, &update_cl);
// #ifdef RELEASE_CHECK
//         log_debug(semeru)("Finish updating for region %lu", r->region_number());
// #endif
//       }

//       if (ShenandoahPacing) {
//         _sh->pacer()->report_evac(r->used() >> LogHeapWordSize);
//       }

//       if (_sh->check_cancelled_gc_and_yield(_concurrent)) {
//         break;
//       }
//     }
//   }
// };


class ShenandoahEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* const _sh;
  ShenandoahCollectionSet* const _cs;
  bool _concurrent;
public:
  ShenandoahEvacuationTask(ShenandoahHeap* sh,
                           ShenandoahCollectionSet* cs,
                           bool concurrent) :
    AbstractGangTask("Parallel Evacuation Task"),
    _sh(sh),
    _cs(cs),
    _concurrent(concurrent)
  {}

  void work(uint worker_id) {
    if (_concurrent) {
      ShenandoahConcurrentWorkerSession worker_session(worker_id);
      ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
      ShenandoahEvacOOMScope oom_evac_scope;
      do_work();
    } else {
      ShenandoahParallelWorkerSession worker_session(worker_id);
      ShenandoahEvacOOMScope oom_evac_scope;
      do_work();
    }
  }

private:
  void do_work() {
    ShenandoahConcurrentEvacuateRegionObjectClosure cl(_sh);
    ShenandoahConcurrentUpdateRegionObjectClosure update_cl(_sh);
    ShenandoahHeapRegion* r;
    while ((r =_cs->claim_next()) != NULL) {
      // assert(r->has_live(), "all-garbage regions are reclaimed early");
      assert(_cs->is_in_local_update_set(r), "Invariant!");
      if(_cs->is_in_evac_set(r)) {
        assert(r->has_live(), "all-garbage regions are reclaimed early");
        assert(_sh->marking_context()->top_at_mark_start(r) == r->top(), "Do not evacuate regions that have unmarked objects!");
        r->_evac_top = r->bottom();
        _sh->marked_object_iterate(r, &cl);
        // _sh->fill_with_dummy_object(r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words(), r->sync_between_mem_and_cpu()->_evac_start + align_up(r->get_live_data_words(), 512), true);
#ifdef RELEASE_CHECK
        log_debug(semeru)("Finish compaction updating for region %lu", r->region_number());
#endif
      }

      if (ShenandoahPacing) {
        _sh->pacer()->report_evac(r->used() >> LogHeapWordSize);
      }

      if (_sh->check_cancelled_gc_and_yield(_concurrent)) {
        break;
      }
    }
  }
};

void ShenandoahHeap::trash_cset_regions() {
  ShenandoahHeapLocker locker(lock());

  ShenandoahCollectionSet* set = collection_set();
  ShenandoahHeapRegion* r;
  set->clear_current_index();
  while ((r = set->next_evac_set()) != NULL) {
    r->make_trash();
  }
  collection_set()->clear();
}

void ShenandoahHeap::print_heap_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions:");
  st->print_cr("EU=empty-uncommitted, EC=empty-committed, R=regular, H=humongous start, HC=humongous continuation, CS=collection set, T=trash, P=pinned");
  st->print_cr("BTE=bottom/top/end, U=used, T=TLAB allocs, G=GCLAB allocs, S=shared allocs, L=live data");
  st->print_cr("R=root, CP=critical pins, TAMS=top-at-mark-start (previous, next)");
  st->print_cr("SN=alloc sequence numbers (first mutator, last mutator, first gc, last gc)");

  for (size_t i = 0; i < num_regions(); i++) {
    get_region(i)->print_on(st);
  }
}

void ShenandoahHeap::trash_humongous_region_at(ShenandoahHeapRegion* start) {
  assert(start->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = oop(start->bottom());
  size_t size = humongous_obj->size();
  size_t required_regions = ShenandoahHeapRegion::required_regions(size * HeapWordSize);
  size_t index = start->region_number() + required_regions - 1;

  assert(!start->has_live(), "liveness must be zero");

  for(size_t i = 0; i < required_regions; i++) {
    // Reclaim from tail. Otherwise, assertion fails when printing region to trace log,
    // as it expects that every region belongs to a humongous region starting with a humongous start region.
    ShenandoahHeapRegion* region = get_region(index --);

    assert(region->is_humongous(), "expect correct humongous start or continuation");
    assert(!region->is_cset(), "Humongous region should not be in collection set");

    region->make_trash_immediate();
  }
}

class ShenandoahRetireGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
    assert(gclab != NULL, "GCLAB should be initialized for %s", thread->name());
    gclab->retire();
  }
};

void ShenandoahHeap::make_parsable(bool retire_tlabs) { // mhrcr: basically always true
  if (UseTLAB) {
    CollectedHeap::ensure_parsability(retire_tlabs);
  }
  ShenandoahRetireGCLABClosure cl;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    cl.do_thread(t);
  }
  workers()->threads_do(&cl);
}

void ShenandoahHeap::resize_tlabs() {
  CollectedHeap::resize_all_tlabs();
}


class ShenandoahSemeruEvacuateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootEvacuator* _rp;

public:
  ShenandoahSemeruEvacuateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah mako evacuate roots"),
    _rp(rp) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahSemeruEvacuateRootsClosure cl(worker_id);
    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
    _rp->roots_do(worker_id, &cl);
  }
};


class ShenandoahEvacuateUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootEvacuator* _rp;

public:
  ShenandoahEvacuateUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahEvacuateUpdateRootsClosure cl(worker_id);
    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
    _rp->roots_do(worker_id, &cl);
  }
};

class ShenandoahSemeruUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootEvacuator* _rp;

public:
  ShenandoahSemeruUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    {
      ShenandoahSemeruUpdateRootsClosure cl;
      MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
      _rp->roots_do(worker_id, &cl);
    }
  }
};

class ShenandoahUpdateRootObjTask : public AbstractGangTask {
private:
  ShenandoahHeap* _heap;
  RootObjectUpdateQueue* _q;
public:
  ShenandoahUpdateRootObjTask(RootObjectUpdateQueue* q) :
    AbstractGangTask("Shenandoah Update Root Reacheable Objects"),
    _heap(ShenandoahHeap::heap()),
    _q(q) {}

  virtual void work(uint worker_id) {
    size_t j = worker_id;
    size_t length = _q->length(j);
    for(size_t i = 0; i < length; i ++) {
      HeapWord* p = _q->retrieve_item(i, j);
      oop obj = oop(p);
      if(_heap->in_evac_set(obj)) {
        size_t size = obj->size();
        oop orig_target_obj = _heap->alive_table()->get_target_address(obj);
        _heap->fill_with_dummy_object((HeapWord*)orig_target_obj, ((HeapWord*)orig_target_obj) + size, true);
        oop to_oop = ShenandoahForwarding::get_forwardee(obj);
        _heap->update_object(to_oop);
      }   
      else {
        _heap->update_object(obj);
      }
    }
  }
};

oop ShenandoahHeap::process_region(oop src, Thread* thread) {
  size_t index = heap_region_index_containing((HeapWord*)src);
  // log_debug(semeru)("Thread 0x%lx waiting for region %lu for object 0x%lx", (size_t)thread, index, (size_t)src);

  if (is_evacuation_in_progress()) {
    if (_cpu_server_flags->_should_start_update == false) {
      if (in_evac_set(src)) {
        if (collection_set()->is_in_local_update_set(index)) {
          oop fwd = ShenandoahForwarding::get_forwardee(src);
          if (oopDesc::equals_raw(src, fwd)) {
            return barrier_evacuate(src, thread);
          }
          return fwd;
        }
        else {
          while(!collection_set()->is_evac_finished(index)) {
            os::naked_short_sleep(5);
          }
        }
      }
    } else {
      if (in_evac_set(src)) {
        ShouldNotReachHere();
      }
      if (collection_set()->is_in_local_update_set(index)) {
        update_object(src);
      }
      else {
        while(!collection_set()->is_update_finished(index)) {
          os::naked_short_sleep(5);
        }
      }
    }
  }
  // Haoran: debug this!!
#ifdef RELEASE_CHECK
  if(in_evac_set(src)) {
    ShouldNotReachHere();
    return ShenandoahForwarding::get_forwardee(src);
  }
#endif
  return src;
}

void ShenandoahHeap::evacuate_and_update_roots() {
  ShouldNotCallThis();
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  {
    ShenandoahRootEvacuator rp(workers()->active_workers(), ShenandoahPhaseTimings::init_evac);
    ShenandoahEvacuateUpdateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

void ShenandoahHeap::evacuate_roots() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  {
    ShenandoahRootEvacuator rp(workers()->active_workers(), ShenandoahPhaseTimings::init_evac);
    ShenandoahSemeruEvacuateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

void ShenandoahHeap::update_roots() {
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::clear();
#endif
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  {

    ShenandoahRootEvacuator rp(workers()->active_workers(), ShenandoahPhaseTimings::update_roots);
    ShenandoahSemeruUpdateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }

#if COMPILER2_OR_JVMCI
  DerivedPointerTable::update_pointers();
#endif
}

// Returns size in bytes
size_t ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  if (ShenandoahElasticTLAB) {
    // With Elastic TLABs, return the max allowed size, and let the allocation path
    // figure out the safe size for current allocation.
    return ShenandoahHeapRegion::max_tlab_size_bytes();
  } else {
    return MIN2(_free_set->unsafe_peek_free(), ShenandoahHeapRegion::max_tlab_size_bytes());
  }
}

size_t ShenandoahHeap::max_tlab_size() const {
  // Returns size in words
  return ShenandoahHeapRegion::max_tlab_size_words();
}

class ShenandoahRetireAndResetGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
    gclab->retire();
    if (ShenandoahThreadLocalData::gclab_size(thread) > 0) {
      ShenandoahThreadLocalData::set_gclab_size(thread, 0);
    }
  }
};

void ShenandoahHeap::retire_and_reset_gclabs() {
  ShenandoahRetireAndResetGCLABClosure cl;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    cl.do_thread(t);
  }
  workers()->threads_do(&cl);
}

void ShenandoahHeap::collect(GCCause::Cause cause) {
  control_thread()->request_gc(cause);
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  Space* sp = heap_region_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  return sp->block_is_obj(addr);
}

jlong ShenandoahHeap::millis_since_last_gc() {
  double v = heuristics()->time_since_last_gc() * 1000;
  assert(0 <= v && v <= max_jlong, "value should fit: %f", v);
  return (jlong)v;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    make_parsable(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::print_worker_threads_on(st);
  }
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
  if (_safepoint_workers != NULL) {
    _safepoint_workers->threads_do(tcl);
  }
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::threads_do(tcl);
  }
}

void ShenandoahHeap::print_tracing_info() const {
  LogTarget(Info, gc, stats) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);

    phase_timings()->print_on(&ls);

    ls.cr();
    ls.cr();

    shenandoah_policy()->print_gc_stats(&ls);

    ls.cr();
    ls.cr();

    if (ShenandoahPacing) {
      pacer()->print_on(&ls);
    }

    ls.cr();
    ls.cr();

    if (ShenandoahAllocationTrace) {
      assert(alloc_tracker() != NULL, "Must be");
      alloc_tracker()->print_on(&ls);
    } else {
      ls.print_cr("  Allocation tracing is disabled, use -XX:+ShenandoahAllocationTrace to enable.");
    }
  }
}

void ShenandoahHeap::verify(VerifyOption vo) {
  if (ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    if (ShenandoahVerify) {
      verifier()->verify_generic(vo);
    } else {
      // TODO: Consider allocating verification bitmaps on demand,
      // and turn this on unconditionally.
    }
  }
}
size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return _free_set->capacity();
}

class ObjectIterateScanRootClosure : public BasicOopIterateClosure {
private:
  MarkBitMap* _bitmap;
  Stack<oop,mtGC>* _oop_stack;

  template <class T>
  void do_oop_work(T* p) {
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);
      obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      assert(oopDesc::is_oop(obj), "must be a valid oop");
      if (!_bitmap->is_marked((HeapWord*) obj)) {
        _bitmap->mark((HeapWord*) obj);
        _oop_stack->push(obj);
      }
    }
  }
public:
  ObjectIterateScanRootClosure(MarkBitMap* bitmap, Stack<oop,mtGC>* oop_stack) :
    _bitmap(bitmap), _oop_stack(oop_stack) {}
  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

/*
 * This is public API, used in preparation of object_iterate().
 * Since we don't do linear scan of heap in object_iterate() (see comment below), we don't
 * need to make the heap parsable. For Shenandoah-internal linear heap scans that we can
 * control, we call SH::make_tlabs_parsable().
 */
void ShenandoahHeap::ensure_parsability(bool retire_tlabs) {
  // No-op.
}

/*
 * Iterates objects in the heap. This is public API, used for, e.g., heap dumping.
 *
 * We cannot safely iterate objects by doing a linear scan at random points in time. Linear
 * scanning needs to deal with dead objects, which may have dead Klass* pointers (e.g.
 * calling oopDesc::size() would crash) or dangling reference fields (crashes) etc. Linear
 * scanning therefore depends on having a valid marking bitmap to support it. However, we only
 * have a valid marking bitmap after successful marking. In particular, we *don't* have a valid
 * marking bitmap during marking, after aborted marking or during/after cleanup (when we just
 * wiped the bitmap in preparation for next marking).
 *
 * For all those reasons, we implement object iteration as a single marking traversal, reporting
 * objects as we mark+traverse through the heap, starting from GC roots. JVMTI IterateThroughHeap
 * is allowed to report dead objects, but is not required to do so.
 */
void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  if (!_aux_bitmap_region_special && !os::commit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size(), false)) {
    log_warning(gc)("Could not commit native memory for auxiliary marking bitmap for heap iteration");
    return;
  }

  // Reset bitmap
  _aux_bit_map.clear();

  Stack<oop,mtGC> oop_stack;

  // First, we process all GC roots. This populates the work stack with initial objects.
  ShenandoahAllRootScanner rp(1, ShenandoahPhaseTimings::_num_phases);
  ObjectIterateScanRootClosure oops(&_aux_bit_map, &oop_stack);
  rp.roots_do_unchecked(&oops);

  // Work through the oop stack to traverse heap.
  while (! oop_stack.is_empty()) {
    oop obj = oop_stack.pop();
    assert(oopDesc::is_oop(obj), "must be a valid oop");
    cl->do_object(obj);
    obj->oop_iterate(&oops);
  }

  assert(oop_stack.is_empty(), "should be empty");

  if (!_aux_bitmap_region_special && !os::uncommit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size())) {
    log_warning(gc)("Could not uncommit native memory for auxiliary marking bitmap for heap iteration");
  }
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  object_iterate(cl);
}

void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk) const {
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* current = get_region(i);
    blk->heap_region_do(current);
  }
}

class ShenandoahParallelHeapRegionTask : public AbstractGangTask {
private:
  ShenandoahHeap* const _heap;
  ShenandoahHeapRegionClosure* const _blk;

  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(volatile size_t));
  volatile size_t _index;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, 0);

public:
  ShenandoahParallelHeapRegionTask(ShenandoahHeapRegionClosure* blk) :
          AbstractGangTask("Parallel Region Task"),
          _heap(ShenandoahHeap::heap()), _blk(blk), _index(0) {}

  void work(uint worker_id) {
    size_t stride = ShenandoahParallelRegionStride;

    size_t max = _heap->num_regions();
    while (_index < max) {
      size_t cur = Atomic::add(stride, &_index) - stride;
      size_t start = cur;
      size_t end = MIN2(cur + stride, max);
      if (start >= max) break;

      for (size_t i = cur; i < end; i++) {
        ShenandoahHeapRegion* current = _heap->get_region(i);
        _blk->heap_region_do(current);
      }
    }
  }
};

void ShenandoahHeap::parallel_heap_region_iterate(ShenandoahHeapRegionClosure* blk) const {
  assert(blk->is_thread_safe(), "Only thread-safe closures here");
  if (num_regions() > ShenandoahParallelRegionStride) {
    ShenandoahParallelHeapRegionTask task(blk);
    workers()->run_task(&task);
  } else {
    heap_region_iterate(blk);
  }
}

class ShenandoahClearLivenessClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahClearLivenessClosure() : _ctx(ShenandoahHeap::heap()->marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    if (r->is_active()) {
      r->clear_live_data();
      _ctx->capture_top_at_mark_start(r);
      // r->acquire_offset_table();
    } else {
      assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->region_number());
      assert(_ctx->top_at_mark_start(r) == r->top(),
             "Region " SIZE_FORMAT " should already have correct TAMS", r->region_number());
    }

    // Modified by Haoran
    r->sync_between_mem_and_cpu()->_state = (size_t)r->semeru_state_ordinal();
  }

  bool is_thread_safe() { return true; }
};

// class ShenandoahEvictClosure : public ShenandoahHeapRegionClosure {
// private:
//   ShenandoahHeap* const _heap;
//   ShenandoahCollectionSet* const _cset;
// public:
//   ShenandoahEvictClosure() : _heap(ShenandoahHeap::heap()), _cset(ShenandoahHeap::heap()->collection_set()) {}

//   void heap_region_do(ShenandoahHeapRegion* region) {
//     size_t i = region->region_number();
//     if(!_cset->is_in_update_set(i)) return;
//     ShenandoahHeapRegion* corr_region = _heap->get_corr_region(i);
//     assert(!region->is_humongous_continuation(), "Invariant!");
//     if(_cset->is_in_evac_set(corr_region->region_number()) && !_cset->is_in_local_update_set(corr_region)) {
//       _heap->rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//     }
//     else if(!_cset->is_in_local_update_set(i)) {
//       if(region->is_humongous_start()) {
//         oop h_obj = oop(region->bottom());
//         size_t h_size = (h_obj->size()) * 8;
//         size_t aligned_size = ((h_size - 1)/4096 + 1) * 4096;
//         _heap->rdma_evict(0, (char*)region->bottom(), aligned_size);
//       }
//       else {
//         assert(!region->is_humongous(), "invariant!");
//         if(!_cset->is_in_evac_set(i)){
//           _heap->rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//         }
//       }
//     }
//   }

//   bool is_thread_safe() { return true; }
// };

// class ShenandoahFinalFlushClosure : public ShenandoahHeapRegionClosure {
// private:
//   ShenandoahHeap* const _heap;
//   ShenandoahCollectionSet* const _cset;
// public:
//   ShenandoahFinalFlushClosure() : _heap(ShenandoahHeap::heap()), _cset(ShenandoahHeap::heap()->collection_set()) {}

//   void heap_region_do(ShenandoahHeapRegion* region) {
//     size_t i = region->region_number();
//     if(!_cset->is_in_update_set(i)) return;
//     ShenandoahHeapRegion* corr_region = _heap->get_corr_region(i);
//     assert(!region->is_humongous_continuation(), "Invariant!");
//     if(_cset->is_in_evac_set(i)) {
//       assert(region->is_cset(), "Invariant!");
//       _heap->rdma_write_offset_table(i);
//     }
//     if(_cset->is_in_evac_set(corr_region->region_number()) && !_cset->is_in_local_update_set(corr_region)) {
//       return;
//     }
//     else if(!_cset->is_in_local_update_set(i)) {
//       if(!region->is_humongous_start()) {
//         assert(!region->is_humongous(), "invariant!");
//         if(_cset->is_in_evac_set(i)){
//           _heap->rdma_write(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
//         }
//       }
//     }
//   }

//   bool is_thread_safe() { return true; }
// };

void ShenandoahHeap::flush_heap() {
  int core_id = 1;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pid_t tid = syscall(SYS_gettid);
  sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

  RootObjectUpdateQueue* q = root_object_update_queue();
  for(size_t j = 0; j < ParallelGCThreads; j ++) {
    size_t length = q->length(j);
    for(size_t i = 0; i < length; i ++) {
      HeapWord* p = q->retrieve_item(i, j);
      _root_object_queue->push(oop(p));
    }
  }
  rdma_write_all((char*)(SEMERU_START_ADDR+KLASS_INSTANCE_OFFSET), KLASS_INSTANCE_OFFSET_SIZE_LIMIT);


  ShenandoahRegionIterator regions;
  ShenandoahHeapRegion* region;
  regions.reset();
  region = regions.next();
  // syscall(ENTER_EVICT, 0, NULL, 0);
  // syscall(INIT_CHECK_STATUS, 0, NULL, 0);

  while(region != NULL) {
    // if(region->top() != region->bottom()) {
      rdma_write(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
    // }
    region = regions.next();         
  }
  // syscall(LEAVE_EVICT, 0, NULL, 0);

  log_debug(semeru,rdma)("Pre Flushed all regions to mem servers.");

  rdma_write(0, (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET), CPU_TO_MEMORY_INIT_SIZE_LIMIT);
  rdma_write(0, (char*)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET), SYNC_MEMORY_AND_CPU_SIZE_LIMIT);

  rdma_write(0, (char*)_tracing_current_cycle_processing, RDMA_FLAG_PER_SIZE);
  rdma_write(0, (char*)_root_object_queue, CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT);
  log_debug(semeru,rdma)("rootobjqueue start_addr: 0x%lx, size: 0x%lx, root_object_queue: 0x%lx", 	(size_t)_root_object_queue, CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT, (size_t)_root_object_queue->_queue);
  log_debug(semeru)("Root Object Queue Len: %lu", _root_object_queue->length());

  
  rdma_write(0, (char*)_cpu_server_flags, sizeof(flags_of_cpu_server_state));
  is_doing_flush = false;
}

void ShenandoahHeap::op_heap_flush() {
  flush_heap();
}


class ShenandoahDupSATBThreadsClosure : public ThreadClosure {
private:
  ShenandoahSATBBufferClosure* _satb_cl;
  uintx _claim_token;

public:
  ShenandoahDupSATBThreadsClosure(ShenandoahSATBBufferClosure* satb_cl) :
    _satb_cl(satb_cl),
    _claim_token(Threads::thread_claim_token()) {}

  void do_thread(Thread* thread) {
    if (thread->claim_threads_do(true, _claim_token)) {
      ShenandoahThreadLocalData::satb_mark_queue(thread).apply_closure_and_empty(_satb_cl);
    }
  }
};

class ShenandoahSATBFlushTask : public AbstractGangTask {

public:
  ShenandoahSATBFlushTask() :
    AbstractGangTask("Shenandoah SATB Flush"){
  }

  void work(uint worker_id) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahObjToScanQueue* q = heap->concurrent_mark()->get_queue(worker_id);
    ShenandoahSATBBufferClosure cl(q);
    ShenandoahDupSATBThreadsClosure tc(&cl);
    Threads::threads_do(&tc);
  }
};

void ShenandoahHeap::op_satb_flush() {

  ShenandoahSATBFlushTask task;
  workers()->run_task(&task);
  
}


void ShenandoahHeap::op_init_mark() {
  int core_id = 1;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pid_t tid = syscall(SYS_gettid);
  sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);


  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");

  assert(marking_context()->is_bitmap_clear(), "need clear marking bitmap");
  assert(!marking_context()->is_complete(), "should not be complete");

  if (ShenandoahVerify) {
    verifier()->verify_before_concmark();
  }
  if (VerifyBeforeGC) {
    Universe::verify();
  }
  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::make_parsable);
    make_parsable(true);
  }
  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::clear_liveness);
    ShenandoahClearLivenessClosure clc;
    parallel_heap_region_iterate(&clc);
  }

  // Make above changes visible to worker threads
  OrderAccess::fence();
  
  double final_pause_time = 0;
  double st_time = os::elapsedTime();

  
  concurrent_mark()->mark_roots(ShenandoahPhaseTimings::scan_roots);
  final_pause_time += os::elapsedTime() - st_time;
  log_debug(semeru)("Pause Time for mark roots: %lf ms", (os::elapsedTime() - st_time) * 1000);
  st_time = os::elapsedTime();

  // RootObjectUpdateQueue* q = root_object_update_queue();
  // for(size_t j = 0; j < ParallelGCThreads; j ++) {
  //   size_t length = q->length(j);
  //   for(size_t i = 0; i < length; i ++) {
  //     HeapWord* p = q->retrieve_item(i, j);
  //     _root_object_queue->push(oop(p));
  //   }
  // }

  // flush_cache_all();

  // rdma_write_all((char*)(SEMERU_START_ADDR+KLASS_INSTANCE_OFFSET), KLASS_INSTANCE_OFFSET_SIZE_LIMIT);

  // Modified by Haoran 
  ShenandoahRegionIterator regions;
  ShenandoahHeapRegion* region;



  regions.reset();
  region = regions.next();
  // syscall(ENTER_EVICT, 0, NULL, 0);
  // syscall(INIT_CHECK_STATUS, 0, NULL, 0);
  // // TODO: make this parallel
  // while(region != NULL) {
  //   // Modified by Haoran New
  //   // TODO: optimization
  //   if(region->top() != region->bottom()) {
  //     rdma_write(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
  //     // rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
  //     // syscall(CHECK_STATUS, 0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
  //     // log_debug(semeru,rdma)("flushed region [0x%lx], start: 0x%lx, size: 0x%lx, top: 0x%lx", region->region_number(), (size_t)region->bottom(), ShenandoahHeapRegion::region_size_bytes(), (size_t)region->top());
    
  //   }
    
  //   region = regions.next();
  //   // rdma_write(0, (char*)region->cpu_to_mem_at_init(), sizeof(ShenandoahCPUToMemoryAtInit));
  //   // region->sync_between_mem_and_cpu()->_top = region->top();
  //   // rdma_write(0, (char*)region->sync_between_mem_and_cpu(), sizeof(ShenandoahSyncBetweenMemoryAndCPU));
  //   // region = regions.next();               
  // }
  // syscall(LEAVE_EVICT, 0, NULL, 0);
  // log_debug(semeru,rdma)("Flushed regions structures to mem servers.");


  regions.reset();
  region = regions.next();
  while(region != NULL) {
    // rdma_write(0, (char*)region->cpu_to_mem_at_init(), sizeof(ShenandoahCPUToMemoryAtInit));
    region->sync_between_mem_and_cpu()->_top = region->top();
    // rdma_write(0, (char*)region->sync_between_mem_and_cpu(), sizeof(ShenandoahSyncBetweenMemoryAndCPU));
    region = regions.next();               
  }
  // rdma_write(0, (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET), CPU_TO_MEMORY_INIT_SIZE_LIMIT);
  // rdma_write(0, (char*)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET), SYNC_MEMORY_AND_CPU_SIZE_LIMIT);


  _tracing_current_cycle_processing->set_byte_flag(true);
  // rdma_write(0, (char*)_tracing_current_cycle_processing, RDMA_FLAG_PER_SIZE);
  // rdma_write(0, (char*)_root_object_queue, CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT);
  // log_debug(semeru,rdma)("rootobjqueue start_addr: 0x%lx, size: 0x%lx, root_object_queue: 0x%lx", 	(size_t)_root_object_queue, CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT, (size_t)_root_object_queue->_queue);
  // log_debug(semeru)("Root Object Queue Len: %lu", _root_object_queue->length());

  _cpu_server_flags->_should_start_tracing = true;
  log_debug(semeru,rdma)("cpu server flags _should_start_tracing %d", _cpu_server_flags->_should_start_tracing);
  _cpu_server_flags->_tracing_all_finished = false;
  log_debug(semeru,rdma)("cpu server flags _tracing_all_finished %d", _cpu_server_flags->_tracing_all_finished);
  // Modified by Haoran for remote compaction
  _cpu_server_flags->_should_start_evacuation = false;
  _cpu_server_flags->_evacuation_all_finished = false;
  _cpu_server_flags->_should_start_update = false;
  _cpu_server_flags->_update_all_finished = false;

  // Haoran: TODO: ensure all data has been written
  // os::naked_short_sleep(100);
  // os::sleep(Thread::current(), 100, false);

  // rdma_signal_flag(0, (char*)_cpu_server_flags, sizeof(flags_of_cpu_server_state));
  // rdma_write(0, (char*)_cpu_server_flags, sizeof(flags_of_cpu_server_state));

  
  log_debug(semeru,rdma)("cpu server flags start_addr: 0x%lx, size: 0x%lx", (size_t)_cpu_server_flags, (size_t)sizeof(flags_of_cpu_server_state) );
  log_debug(semeru,rdma)("cpu server flags _tracing_all_finished %d", _cpu_server_flags->_tracing_all_finished);


  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::resize_tlabs);
    resize_tlabs();
  }

  if (ShenandoahPacing) {
    pacer()->setup_for_mark();
  }


  // while(_mem_server_flags->_tracing_finished == false) {
  //   os::naked_short_sleep(10);
  //   syscall(RDMA_READ, 0, _mem_server_flags, FLAGS_OF_MEM_SERVER_STATE_SIZE);
  // }
  // log_debug(semeru)("Memory Server Finishes Marking!");

  // stop_concurrent_marking();

  is_doing_flush = true;

  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    SATBMarkQueue* q = &(ShenandoahThreadLocalData::satb_mark_queue(t));
    if(q->index() != 0) {
      q->set_index(0);
      if(q->_buf != NULL) {
        memset(q->_buf, 0, q->capacity_in_bytes());
      }
    }
    
  }

}

void ShenandoahHeap::op_mark() {
  concurrent_mark()->mark_from_roots();
}

class ShenandoahCompleteLivenessClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahCompleteLivenessClosure() : _ctx(ShenandoahHeap::heap()->complete_marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    if (r->is_active()) {
      HeapWord *tams = _ctx->top_at_mark_start(r);
      HeapWord *top = r->top();
      if (top > tams) {
        // Modified by Haoran
        log_debug(semeru)("increase region: 0x%lx words: 0x%lx", r->region_number(), pointer_delta(top, tams));
        r->increase_live_data_alloc_words(pointer_delta(top, tams));
      }
    } else {
      assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->region_number());
      assert(_ctx->top_at_mark_start(r) == r->top(),
             "Region " SIZE_FORMAT " should have correct TAMS", r->region_number());
    }
  }

  bool is_thread_safe() { return true; }
};


// Modified by Haoran
void ShenandoahHeap::rdma_read_bitmap(size_t server_id) {
  log_debug(semeru,rdma)("Read server: %lu bitmap: 0x%lx, byte_size: 0x%lx", 	server_id, (size_t)((char*)(((size_t)_bitmap_region.start()) + server_id * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER))), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER);
  rdma_read(server_id, (char*)(((size_t)_bitmap_region.start()) + server_id * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER)), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER);
}

// Modified by Haoran
// void ShenandoahHeap::rdma_read_offset_table(size_t region_index) {
//   //TODO: implement();
//   // ShouldNotCallThis();
//   ShenandoahHeapRegion* r = get_region(region_index);
//   OffsetTable* ot = r->offset_table();
//   // log_debug(semeru,rdma)("Read server: %d offset_table: 0x%lx, byte_size: 0x%lx", 	0, (size_t)(ot->base()), ot->table_byte_size());
//   rdma_read(0, ot->base(), ot->table_byte_size());
// }

// // Modified by Haoran
// void ShenandoahHeap::rdma_write_offset_table(size_t region_index) {
//   //TODO: implement();
//   ShenandoahHeapRegion* r = get_region(region_index);
//   OffsetTable* ot = r->offset_table();
//   // log_debug(semeru,rdma)("Write server: %d offset_table: 0x%lx, byte_size: 0x%lx", 0, (size_t)(ot->base()), ot->table_byte_size());
//   rdma_write(0, ot->base(), ot->table_byte_size());
// }

size_t ShenandoahHeap::cache_size_in_bytes(size_t region_index) {
  ShenandoahHeapRegion* region = ShenandoahHeap::heap()->get_region(region_index);
  size_t num_pages = syscall(SYS_NUM_SWAP_OUT_PAGES, region->bottom(), ShenandoahHeapRegion::region_size_bytes());
  return ShenandoahHeapRegion::region_size_bytes() - num_pages * 4096;
}

size_t ShenandoahHeap::cache_ratio(size_t region_index) {
  size_t cache_size = cache_size_in_bytes(region_index);
  return cache_size * 100 / ShenandoahHeapRegion::region_size_bytes();
}

void ShenandoahHeap::rdma_read_bitmap_all() {
  for(size_t i = 0; i < NUM_OF_MEMORY_SERVER; i ++) {
      rdma_read_bitmap(i);
      // assure_read(i);
  }
    
}

void ShenandoahHeap::rdma_write_bitmap(size_t server_id) {
  log_debug(semeru,rdma)("Write server: %lu bitmap: 0x%lx, byte_size: 0x%lx", 	server_id, (size_t)((char*)(((size_t)_bitmap_region.start()) + server_id * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER))), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER);
  rdma_write(server_id, (char*)(((size_t)_bitmap_region.start()) + server_id * (_bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER)), _bitmap_region.byte_size()/NUM_OF_MEMORY_SERVER);
}

void ShenandoahHeap::rdma_write_bitmap_all() {
  for(size_t i = 0; i < NUM_OF_MEMORY_SERVER; i ++) {
      rdma_write_bitmap(i);
      // assure_write(i);
  }
    
}

void ShenandoahHeap::rdma_read_alive_table(size_t server_id) {
  log_debug(semeru,rdma)("Read server: %lu alive_table: 0x%lx, byte_size: 0x%lx", 	server_id, (size_t)(char*)(((size_t)(ALIVE_TABLE_START_ADDR)) + server_id * (ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER)), ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER);
  rdma_read(server_id, (char*)(((size_t)(ALIVE_TABLE_START_ADDR)) + server_id * (ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER)), ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER);
}


void ShenandoahHeap::rdma_write_alive_table(size_t server_id) {
  log_debug(semeru,rdma)("Write server: %lu alive_table: 0x%lx, byte_size: 0x%lx", 	server_id, (size_t)(char*)(((size_t)(ALIVE_TABLE_START_ADDR)) + server_id * (ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER)), ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER);
  rdma_write(server_id, (char*)(((size_t)(ALIVE_TABLE_START_ADDR)) + server_id * (ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER)), ALIVE_TABLE_MAX_SIZE/NUM_OF_MEMORY_SERVER);
}



// class ShenandoahCheckHeapMarkClosure : public BasicOopIterateClosure {
// private:
//   ShenandoahHeap* _heap;

//   template <class T>
//   void do_oop_work(T* p) {
//     T o = RawAccess<>::oop_load(p);
    
//     if (!CompressedOops::is_null(o)) {
//       oop* obj_entry = (oop*)(CompressedOops::decode_not_null(o));
//       oop obj = IndirectionTable::resolve(obj_entry);
//       if(!_heap->marking_context()->is_marked(obj)) {
//         log_debug(semeru)("Something wrong is with marking! p: 0x%lx, obj_entry: 0x%lx, obj: 0x%lx", (size_t)p, (size_t)obj_entry, (size_t)obj);
//       }
//       // return maybe_update_with_forwarded_not_null(p, obj);
//     } else {
//       return;
//     }
//   }

// public:
//   ShenandoahCheckHeapMarkClosure() :
//     _heap(ShenandoahHeap::heap()) {}

//   virtual void do_oop(narrowOop* p) { do_oop_work(p); }
//   virtual void do_oop(oop* p)       { do_oop_work(p); }
// };

void ShenandoahHeap::push_to_root_object_update_queue(oop p, size_t worker_id) {
  _root_object_update_queue->push(p, worker_id);
}

void ShenandoahHeap::update_root_objects() {
  // RootObjectUpdateQueue* q = root_object_update_queue();
  // for(size_t j = 0; j < ParallelGCThreads; j ++) {
  //   size_t length = q->length(j);
  //   for(size_t i = 0; i < length; i ++) {
  //     HeapWord* p = q->retrieve_item(i, j);
  //     oop obj = oop(p);
  //     if(in_evac_set(obj)) {
  //       size_t size = obj->size();
  //       oop orig_target_obj = _alive_table->get_target_address(obj);
  //       fill_with_dummy_object((HeapWord*)orig_target_obj, ((HeapWord*)orig_target_obj) + size, true);
  //       oop to_oop = ShenandoahForwarding::get_forwardee(obj);
  //       update_object(to_oop);
  //     }
          
  //     else {
  //       update_object(obj);
  //     }
  //   }
  // }


  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");
  {
    RootObjectUpdateQueue* q = root_object_update_queue();
    ShenandoahUpdateRootObjTask roots_task(q);
    workers()->run_task(&roots_task);
  }

}

void ShenandoahHeap::read_data_pre_final_mark() {
  rdma_read_bitmap(0);
  rdma_read(0, (char*)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET), MEMORY_TO_CPU_GC_SIZE_LIMIT);
  rdma_read_alive_table(0);
}

void ShenandoahHeap::write_data_after_final_mark() {
  int core_id = 1;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pid_t tid = syscall(SYS_gettid);
  sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

  rdma_write_all((char*)(SEMERU_START_ADDR+KLASS_INSTANCE_OFFSET), KLASS_INSTANCE_OFFSET_SIZE_LIMIT);
  rdma_write(0, (char*)collection_set()->_sync_map, CSET_SYNC_MAP_SIZE);
  os::naked_short_sleep(10);
  log_debug(semeru,rdma)("Flushed collection set to mem servers.");
  {

    syscall(ENTER_EVICT, 0, NULL, 0);
    syscall(INIT_CHECK_STATUS, 0, NULL, 0);
    bool has_remote_update_set = true;
    if(has_remote_update_set) {
      for(size_t i = 0; i < _num_regions; i++) {
        ShenandoahHeapRegion* region = get_region(i);
        
        // if(!collection_set()->is_in_update_set(i) && !region->_selected_to) continue;
        if(!collection_set()->is_in_update_set(i) || region->_selected_to) continue;
        if(region->is_humongous_continuation()) continue;
        assert(!region->is_humongous_continuation(), "Invariant!");

        if(collection_set()->is_in_evac_set(i)) {
          assert(region->is_cset(), "Invariant!");
          // rdma_write_offset_table(i);
          rdma_write(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
          if(!collection_set()->is_in_local_update_set(i)) {
            rdma_evict(0, (char*)region->sync_between_mem_and_cpu()->_evac_start, align_up(region->get_live_data_bytes(), 4096));
          }
        }
        // else if(region->_selected_to){
        //   assert(!region->is_cset(), "invariant!");
        //   rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
        // }
        else if(!collection_set()->is_in_local_update_set(i)) {
          if(region->is_humongous_start()) {
            oop h_obj = oop(region->bottom());
            size_t h_size = (h_obj->size()) * 8;
            size_t aligned_size = ((h_size - 1)/4096 + 1) * 4096;
            rdma_evict(0, (char*)region->bottom(), aligned_size);
            // log_debug(semeru,rdma)("humongous_region[0x%lx], bottom: 0x%lx, size: 0x%lx", region->region_number(), (size_t)region->bottom(), aligned_size);
          }
          else {
            assert(!region->is_humongous(), "invariant!");
            rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
            // log_debug(semeru,rdma)("_region[0x%lx], bottom: 0x%lx, top: 0x%lx, size: 0x%lx", region->region_number(), (size_t)region->bottom(), (size_t)region->top(),ShenandoahHeapRegion::region_size_bytes() );
          }

        }
      }
    }
    syscall(LEAVE_EVICT, 0, NULL, 0);
    rdma_write(0, (char*)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET), SYNC_MEMORY_AND_CPU_SIZE_LIMIT);
  }

  log_debug(semeru,rdma)("Flushed regions in collection set to mem servers.");
  rdma_write_bitmap(0);
  rdma_write_alive_table(0);
  // Haoran: TODO, need to sync here to wait for other data to be transmitted.
  // os::naked_short_sleep(10);
  rdma_write(0, (char*)_cpu_server_flags, sizeof(flags_of_cpu_server_state));
  log_debug(semeru,rdma)("cpu server flags start_addr: 0x%lx, size: 0x%lx", (size_t)_cpu_server_flags, (size_t)PAGE_SIZE );
}

void ShenandoahHeap::write_data_after_init_update() {
  int core_id = 1;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pid_t tid = syscall(SYS_gettid);
  sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

  rdma_write_all((char*)(SEMERU_START_ADDR+KLASS_INSTANCE_OFFSET), KLASS_INSTANCE_OFFSET_SIZE_LIMIT);
  rdma_write(0, (char*)collection_set()->_sync_map, CSET_SYNC_MAP_SIZE);
  os::naked_short_sleep(10);
  log_debug(semeru,rdma)("Flushed collection set to mem servers.");
  {

    syscall(ENTER_EVICT, 0, NULL, 0);
    syscall(INIT_CHECK_STATUS, 0, NULL, 0);
    bool has_remote_update_set = true;
    if(has_remote_update_set) {
      for(size_t i = 0; i < _num_regions; i++) {
        ShenandoahHeapRegion* region = get_region(i);
        
        // if(!collection_set()->is_in_update_set(i) && !region->_selected_to) continue;
        if(!collection_set()->is_in_update_set(i) || region->_selected_to) continue;
        if(region->is_humongous_continuation()) continue;
        assert(!region->is_humongous_continuation(), "Invariant!");

        if(collection_set()->is_in_evac_set(i)) {
          assert(region->is_cset(), "Invariant!");
          // rdma_write_offset_table(i);
          rdma_write(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
          if(!collection_set()->is_in_local_update_set(i)) {
            rdma_evict(0, (char*)region->sync_between_mem_and_cpu()->_evac_start, align_up(region->get_live_data_bytes(), 4096));
          }
        }
        // else if(region->_selected_to){
        //   assert(!region->is_cset(), "invariant!");
        //   rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
        // }
        else if(!collection_set()->is_in_local_update_set(i)) {
          if(region->is_humongous_start()) {
            oop h_obj = oop(region->bottom());
            size_t h_size = (h_obj->size()) * 8;
            size_t aligned_size = ((h_size - 1)/4096 + 1) * 4096;
            rdma_evict(0, (char*)region->bottom(), aligned_size);
            // log_debug(semeru,rdma)("humongous_region[0x%lx], bottom: 0x%lx, size: 0x%lx", region->region_number(), (size_t)region->bottom(), aligned_size);
          }
          else {
            assert(!region->is_humongous(), "invariant!");
            rdma_evict(0, (char*)region->bottom(), ShenandoahHeapRegion::region_size_bytes());
            // log_debug(semeru,rdma)("_region[0x%lx], bottom: 0x%lx, top: 0x%lx, size: 0x%lx", region->region_number(), (size_t)region->bottom(), (size_t)region->top(),ShenandoahHeapRegion::region_size_bytes() );
          }

        }
      }
    }
    syscall(LEAVE_EVICT, 0, NULL, 0);
    rdma_write(0, (char*)(SEMERU_START_ADDR + SYNC_MEMORY_AND_CPU_OFFSET), SYNC_MEMORY_AND_CPU_SIZE_LIMIT);
  }

  log_debug(semeru,rdma)("Flushed regions in collection set to mem servers.");
  rdma_write_bitmap(0);
  rdma_write_alive_table(0);
  // Haoran: TODO, need to sync here to wait for other data to be transmitted.
  // os::naked_short_sleep(10);
  rdma_write(0, (char*)_cpu_server_flags, sizeof(flags_of_cpu_server_state));
  log_debug(semeru,rdma)("cpu server flags start_addr: 0x%lx, size: 0x%lx", (size_t)_cpu_server_flags, (size_t)PAGE_SIZE );
}

class ShenandoahSemeruCalcOffsetTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  ShenandoahHeap* const _sh;
public:
  ShenandoahSemeruCalcOffsetTask(ShenandoahHeap* sh) :
    AbstractGangTask("Parallel Calculate Offset Task"),
    _sh(sh)
  {}

  void work(uint worker_id) {
    do_work();
  }

private:
  void do_work() {
    ShenandoahHeapRegion* region = _regions.next();
    ShenandoahMarkingContext* const ctx = _sh->marking_context();
    while (region != NULL) {
      if(region->has_live() && !region->is_humongous_continuation()) {
        // _sh->calc_offset(region);
        _sh->alive_table()->accumulate_table(region->region_number());
      }
      region = _regions.next();
    }
  }
};

void ShenandoahHeap::op_final_mark() {
  double final_pause_time = 0;
  double st_time = os::elapsedTime();
  // int core_id = 1;
  // cpu_set_t cpuset;
  // CPU_ZERO(&cpuset);
  // CPU_SET(core_id, &cpuset);

  // pid_t tid = syscall(SYS_gettid);
  // sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.

  if (!cancelled_gc()) {

    ShenandoahRegionIterator regions;
    ShenandoahHeapRegion* region;

    // clear buffers that has been processed
    
    final_pause_time += os::elapsedTime() - st_time;
    log_debug(semeru)("Pause Time for clear trace buffer: %lf ms", (os::elapsedTime() - st_time) * 1000);
    st_time = os::elapsedTime();

    log_debug(semeru,rdma)("after clear traced buffer");
    
    concurrent_mark()->finish_mark_from_roots(/* full_gc = */ false);
    log_debug(semeru)("CPU Server Finishes All Marking!");
    stop_concurrent_marking();
    ShenandoahSemeruCalcOffsetTask task(this);
    workers()->run_task(&task);

    final_pause_time += os::elapsedTime() - st_time;
    log_debug(semeru)("Pause Time for finish marking: %lf ms", (os::elapsedTime() - st_time) * 1000);
    st_time = os::elapsedTime();

    if (has_forwarded_objects()) {
      ShouldNotReachHere();
      // Degen may be caused by failed evacuation of roots
      if (is_degenerated_gc_in_progress()) {
        concurrent_mark()->update_roots(ShenandoahPhaseTimings::degen_gc_update_roots);
      } else {
        concurrent_mark()->update_thread_roots(ShenandoahPhaseTimings::update_roots);
      }
    }

    if (ShenandoahVerify) {
      verifier()->verify_roots_no_forwarded();
    }

    // stop_concurrent_marking();

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::complete_liveness);

      // All allocations past TAMS are implicitly live, adjust the region data.
      // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
      ShenandoahCompleteLivenessClosure cl;
      parallel_heap_region_iterate(&cl);
    }
    final_pause_time += os::elapsedTime() - st_time;
    log_debug(semeru)("Pause Time for complete liveness: %lf ms", (os::elapsedTime() - st_time) * 1000);
    st_time = os::elapsedTime();

    {
      ShenandoahGCPhase prepare_evac(ShenandoahPhaseTimings::prepare_evac);

      make_parsable(true);
      final_pause_time += os::elapsedTime() - st_time;
      log_debug(semeru)("Pause Time for retire labs: %lf ms", (os::elapsedTime() - st_time) * 1000);
      st_time = os::elapsedTime();

      // Modified by Haoran
      assert(collection_set()->is_empty(), "Invariant here, cset should be set to empty at final update refs");
      // trash_cset_regions();

      {
        ShenandoahHeapLocker locker(lock());
        _collection_set->clear();
        _free_set->clear();
        _free_set->rebuild();
        final_pause_time += os::elapsedTime() - st_time;
        log_debug(semeru)("Pause Time for rebuild free set: %lf ms", (os::elapsedTime() - st_time) * 1000);
        st_time = os::elapsedTime();

        // Haoran: for region with no live objects, will make them trash in this function.
        heuristics()->choose_collection_set(_collection_set);
        final_pause_time += os::elapsedTime() - st_time;
        log_debug(semeru)("Pause Time for choose cset: %lf ms", (os::elapsedTime() - st_time) * 1000);
        st_time = os::elapsedTime();


        regions.reset();
        region = regions.next();
        while(region != NULL) {
          if(region->is_humongous()) {

          }
          if(region->bottom() != region->top() && region->get_live_data_words() > 0 && !region->is_cset() && !region->_selected_to) {
            if(!region->is_humongous_continuation()) {
              collection_set()->add_region_to_update(region);
            }
          }
          region = regions.next();               
        }
        final_pause_time += os::elapsedTime() - st_time;
        log_debug(semeru)("Pause Time for add collectionset: %lf ms", (os::elapsedTime() - st_time) * 1000);
        st_time = os::elapsedTime();
      }
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!collection_set()->is_empty()) {
      ShenandoahGCPhase init_evac(ShenandoahPhaseTimings::init_evac);

      if (ShenandoahVerify) {
        verifier()->verify_before_evacuation();
      }

      set_evacuation_in_progress(true);
      // From here on, we need to update references.
      set_has_forwarded_objects(true);
      final_pause_time += os::elapsedTime() - st_time;
      log_debug(semeru)("Pause Time before evacuate roots: %lf ms", (os::elapsedTime() - st_time) * 1000);
      st_time = os::elapsedTime();
      // evacuate_and_update_roots();
      evacuate_roots();
      final_pause_time += os::elapsedTime() - st_time;
      log_debug(semeru)("Pause Time during evacuate roots: %lf ms", (os::elapsedTime() - st_time) * 1000);
      st_time = os::elapsedTime();

      

      if (ShenandoahPacing) {
        pacer()->setup_for_evac();
      }
      
      // Modified by Haoran for remote compaction
      
      // if(gc_start_threshold > 23 * ONE_GB || gc_start_threshold == 0)
      bool has_remote_update_set = collection_set()->select_local_process_regions();
      
      tty->print("has_remote: %d\n", has_remote_update_set);

      collection_set()->copy_cset_to_sync();

      for(size_t i = 0; i < num_regions(); i++){
        ShenandoahHeapRegion* r = get_region(i);
        if(r->_selected_to) {
          collection_set()->add_region_to_update(r);
        }
        else if(r->is_humongous_continuation()) {
          collection_set()->add_region_to_update(r);
        }

        if(collection_set()->is_in_evac_set(i)) {
          if(collection_set()->is_in_local_update_set(i)) {
            tty->print("region: %lu is in local update set\n", i);
          } else {
            tty->print("region: %lu is in remote update set\n", i);
          }
        }
      }
      
      {
        ShenandoahHeapLocker locker(lock());

        for(size_t i = 0; i < _num_regions; i++) {
          region = get_region(i);
          region->sync_between_mem_and_cpu()->_top = region->top();
          region->sync_between_mem_and_cpu()->_state = region->semeru_state_ordinal();
        }
      }

      _cpu_server_flags->_should_start_tracing = false;
      _cpu_server_flags->_tracing_all_finished = true;
      _cpu_server_flags->_should_start_evacuation = true;
      _cpu_server_flags->_evacuation_all_finished = false;
      _cpu_server_flags->_should_start_update = false;
      _cpu_server_flags->_update_all_finished = false;
      final_pause_time += os::elapsedTime() - st_time;
      log_debug(semeru)("Pause Time after evacuate roots: %lf ms", (os::elapsedTime() - st_time) * 1000);
      st_time = os::elapsedTime();
      log_debug(semeru)("Total Pause Time: %lf ms", final_pause_time * 1000);

      // update_root_objects();
      log_debug(semeru)("Pause Time for update roots: %lf ms", (os::elapsedTime() - st_time) * 1000);


      // ShenandoahEvacuationTask task(this, _collection_set, false);
      // workers()->run_task(&task);
      log_debug(semeru)("Finish update roots!");
      {
        ShenandoahHeapLocker locker(lock());
        _free_set->rebuild();
      }
      // write_data_after_final_mark();
      return;
    } else {
      // ShouldNotReachHere();
      log_debug(semeru)("No collection set! Skip!");
      if (ShenandoahVerify) {
        verifier()->verify_after_concmark();
      }

      if (VerifyAfterGC) {
        Universe::verify();
      }
      // Modified by Haoran for remote
      _cpu_server_flags->_should_start_tracing = false;
      _cpu_server_flags->_tracing_all_finished = true;
      _cpu_server_flags->_should_start_evacuation = false;
      _cpu_server_flags->_evacuation_all_finished = true;
      _cpu_server_flags->_should_start_update = false;
      _cpu_server_flags->_update_all_finished = true;
      rdma_write_all((char*)_cpu_server_flags, PAGE_SIZE);
      // syscall(RDMA_WRITE, 0, _cpu_server_flags, sizeof(flags_of_cpu_server_state));
      log_debug(semeru,rdma)("cpu server flags start_addr: 0x%lx, size: 0x%lx", (size_t)_cpu_server_flags, (size_t)PAGE_SIZE);
    }

  } else {
    // Modified by Haoran for remote compaction
    ShouldNotReachHere();

    concurrent_mark()->cancel();
    stop_concurrent_marking();

    if (process_references()) {
      // Abandon reference processing right away: pre-cleaning must have failed.
      ReferenceProcessor *rp = ref_processor();
      rp->disable_discovery();
      rp->abandon_partial_discovery();
      rp->verify_no_references_recorded();
    }
  }
}

void ShenandoahHeap::op_final_evac() {

  op_final_sync(0);
  trash_cset_regions();
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");
  set_evacuation_in_progress(false);
  retire_and_reset_gclabs();
  make_parsable(true);
  size_t current_alive = 0;
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    r->set_concurrent_iteration_safe_limit(r->top());

    if(!r->is_trash()) {
      current_alive += (size_t)r->top() - (size_t)r->bottom();
    }

  }
  current_alive = current_alive/1024/1024;
  log_debug(semeru)("WholeHeapAlive: %lu MB", current_alive);
  if(current_alive <= max_capacity()/1024/1024 * ShenandoahLocalProcessingRatio/100 / 2) {
    gc_start_threshold = max_capacity() * (ShenandoahInitFreeThreshold) / 100;
  }
  else {
    gc_start_threshold = max_capacity() * ShenandoahMinFreeThreshold / 100;
  }
  set_has_forwarded_objects(false);
  {
    ShenandoahHeapLocker locker(lock());
    _free_set->rebuild();
  }

  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  // set_evacuation_in_progress(false);

  // retire_and_reset_gclabs();

  // if (ShenandoahVerify) {
  //   verifier()->verify_after_evacuation();
  // }

  // if (VerifyAfterGC) {
  //   Universe::verify();
  // }
}

void ShenandoahHeap::op_conc_evac() {
  ShenandoahEvacuationTask task(this, _collection_set, true);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_stw_evac() {
  ShenandoahEvacuationTask task(this, _collection_set, false);
  workers()->run_task(&task);
}


class ShenandoahSemeruUpdateHeapRefsTask : public AbstractGangTask {
private:
  ShenandoahHeap* _sh;
  ShenandoahCollectionSet* const _cs;
  bool _concurrent;
public:
  ShenandoahSemeruUpdateHeapRefsTask(ShenandoahHeap* sh,
                           ShenandoahCollectionSet* cs,
                           bool concurrent) :
    AbstractGangTask("Concurrent Update References Task"),
    _sh(sh),
    _cs(cs),
    _concurrent(concurrent)
  {}

  void work(uint worker_id) {
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
    do_work();
  }

private:
  void do_work() {
    ShenandoahConcurrentUpdateRegionObjectClosure update_cl(_sh);
    ShenandoahHeapRegion* r;
    while ((r =_cs->claim_next()) != NULL) {
      if(_cs->is_in_update_set(r) && !r->is_humongous_continuation()) {
        assert(!_cs->is_in_evac_set(r->region_number()), "Invariant!");
        assert(r->has_live(), "all-garbage regions are reclaimed early");
        assert(!r->is_humongous_continuation(), "Invariant!");
        assert(!r->is_cset(), "Invariant!");
        HeapWord* top_at_start_ur = r->concurrent_iteration_safe_limit();
        _sh->marked_object_iterate(r, &update_cl, top_at_start_ur);
#ifdef RELEASE_CHECK
        log_debug(semeru)("Finish updating for region %lu", r->region_number());
#endif
      }
      if (ShenandoahPacing) {
        _sh->pacer()->report_evac(r->used() >> LogHeapWordSize);
      }
      if (_sh->check_cancelled_gc_and_yield(_concurrent)) {
        break;
      }
    }
  }
};


void ShenandoahHeap::op_updaterefs() {
  ShenandoahSemeruUpdateHeapRefsTask task(this, _collection_set, true);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_cleanup() {
  free_set()->recycle_trash();
}

void ShenandoahHeap::op_reset() {
  reset_mark_bitmap();
}

void ShenandoahHeap::op_preclean() {
  concurrent_mark()->preclean_weak_refs();
}

void ShenandoahHeap::op_init_traversal() {
  traversal_gc()->init_traversal_collection();
}

void ShenandoahHeap::op_traversal() {
  traversal_gc()->concurrent_traversal_collection();
}

void ShenandoahHeap::op_final_traversal() {
  traversal_gc()->final_traversal_collection();
}

void ShenandoahHeap::op_full(GCCause::Cause cause) {
  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  full_gc()->do_it(cause);
  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_resize_tlabs);
    resize_all_tlabs();
  }

  metrics.snap_after();

  if (metrics.is_good_progress()) {
    _progress_last_gc.set();
  } else {
    // Nothing to do. Tell the allocation path that we have failed to make
    // progress, and it can finally fail.
    _progress_last_gc.unset();
  }
}

void ShenandoahHeap::op_degenerated(ShenandoahDegenPoint point) {
  // Degenerated GC is STW, but it can also fail. Current mechanics communicates
  // GC failure via cancelled_concgc() flag. So, if we detect the failure after
  // some phase, we have to upgrade the Degenerate GC to Full GC.

  clear_cancelled_gc();

  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  switch (point) {
    case _degenerated_traversal:
      {
        // Drop the collection set. Note: this leaves some already forwarded objects
        // behind, which may be problematic, see comments for ShenandoahEvacAssist
        // workarounds in ShenandoahTraversalHeuristics.

        ShenandoahHeapLocker locker(lock());
        collection_set()->clear_current_index();
        for (size_t i = 0; i < collection_set()->count(); i++) {
          ShenandoahHeapRegion* r = collection_set()->next();
          r->make_regular_bypass();
        }
        collection_set()->clear();
      }
      op_final_traversal();
      op_cleanup();
      return;

    // The cases below form the Duff's-like device: it describes the actual GC cycle,
    // but enters it at different points, depending on which concurrent phase had
    // degenerated.

    case _degenerated_outside_cycle:
      // We have degenerated from outside the cycle, which means something is bad with
      // the heap, most probably heavy humongous fragmentation, or we are very low on free
      // space. It makes little sense to wait for Full GC to reclaim as much as it can, when
      // we can do the most aggressive degen cycle, which includes processing references and
      // class unloading, unless those features are explicitly disabled.
      //
      // Note that we can only do this for "outside-cycle" degens, otherwise we would risk
      // changing the cycle parameters mid-cycle during concurrent -> degenerated handover.
      set_process_references(heuristics()->can_process_references());
      set_unload_classes(heuristics()->can_unload_classes());

      if (heuristics()->can_do_traversal_gc()) {
        // Not possible to degenerate from here, upgrade to Full GC right away.
        cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
        op_degenerated_fail();
        return;
      }

      op_reset();

      op_init_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

    case _degenerated_mark:
      op_final_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

      op_cleanup();

    case _degenerated_evac:
      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we can do evacuation. Otherwise, it would be the shortcut cycle.
      if (is_evacuation_in_progress()) {

        // Degeneration under oom-evac protocol might have left some objects in
        // collection set un-evacuated. Restart evacuation from the beginning to
        // capture all objects. For all the objects that are already evacuated,
        // it would be a simple check, which is supposed to be fast. This is also
        // safe to do even without degeneration, as CSet iterator is at beginning
        // in preparation for evacuation anyway.
        //
        // Before doing that, we need to make sure we never had any cset-pinned
        // regions. This may happen if allocation failure happened when evacuating
        // the about-to-be-pinned object, oom-evac protocol left the object in
        // the collection set, and then the pin reached the cset region. If we continue
        // the cycle here, we would trash the cset and alive objects in it. To avoid
        // it, we fail degeneration right away and slide into Full GC to recover.

        {
          collection_set()->clear_current_index();

          ShenandoahHeapRegion* r;
          while ((r = collection_set()->next()) != NULL) {
            if (r->is_pinned()) {
              cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
              op_degenerated_fail();
              return;
            }
          }

          collection_set()->clear_current_index();
        }

        op_stw_evac();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we need to do update-refs. Otherwise, it would be the shortcut cycle.
      if (has_forwarded_objects()) {
        op_init_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

    case _degenerated_updaterefs:
      if (has_forwarded_objects()) {
        op_final_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      op_cleanup();
      break;

    default:
      ShouldNotReachHere();
  }

  if (ShenandoahVerify) {
    verifier()->verify_after_degenerated();
  }

  if (VerifyAfterGC) {
    Universe::verify();
  }

  metrics.snap_after();

  // Check for futility and fail. There is no reason to do several back-to-back Degenerated cycles,
  // because that probably means the heap is overloaded and/or fragmented.
  if (!metrics.is_good_progress()) {
    _progress_last_gc.unset();
    cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
    op_degenerated_futile();
  } else {
    _progress_last_gc.set();
  }
}

void ShenandoahHeap::op_degenerated_fail() {
  log_info(gc)("Cannot finish degeneration, upgrading to Full GC");
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::op_degenerated_futile() {
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(is_concurrent_mark_in_progress(), "How else could we get here?");
  set_concurrent_mark_in_progress(false);
  if (!cancelled_gc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_has_forwarded_objects(false);
    mark_complete_marking_context();
  }
}

void ShenandoahHeap::force_satb_flush_all_threads() {
  if (!is_concurrent_mark_in_progress() && !is_concurrent_traversal_in_progress()) {
    // No need to flush SATBs
    return;
  }

  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    ShenandoahThreadLocalData::set_force_satb_flush(t, true);
  }
  // The threads are not "acquiring" their thread-local data, but it does not
  // hurt to "release" the updates here anyway.
  OrderAccess::fence();
}

void ShenandoahHeap::set_gc_state_all_threads(char state) {
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    ShenandoahThreadLocalData::set_gc_state(t, state);
  }
}

void ShenandoahHeap::set_gc_state_mask(uint mask, bool value) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should really be Shenandoah safepoint");
  _gc_state.set_cond(mask, value);
  set_gc_state_all_threads(_gc_state.raw_value());
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  if (has_forwarded_objects()) {
    set_gc_state_mask(MARKING | UPDATEREFS, in_progress);
  } else {
    set_gc_state_mask(MARKING, in_progress);
  }
  ShenandoahBarrierSet::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahHeap::set_concurrent_traversal_in_progress(bool in_progress) {
   set_gc_state_mask(TRAVERSAL | HAS_FORWARDED | UPDATEREFS, in_progress);
   ShenandoahBarrierSet::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only call this at safepoint");
  set_gc_state_mask(EVACUATION, in_progress);
}

void ShenandoahHeap::ref_processing_init() {
  assert(_max_workers > 0, "Sanity");

  _ref_processor =
    new ReferenceProcessor(&_subject_to_discovery,  // is_subject_to_discovery
                           ParallelRefProcEnabled,  // MT processing
                           _max_workers,            // Degree of MT processing
                           true,                    // MT discovery
                           _max_workers,            // Degree of MT discovery
                           false,                   // Reference discovery is not atomic
                           NULL,                    // No closure, should be installed before use
                           true);                   // Scale worker threads

  shenandoah_assert_rp_isalive_not_installed();
}

GCTracer* ShenandoahHeap::tracer() {
  return shenandoah_policy()->tracer();
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  return _free_set->used();
}

bool ShenandoahHeap::try_cancel_gc() {
  while (true) {
    jbyte prev = _cancelled_gc.cmpxchg(CANCELLED, CANCELLABLE);
    if (prev == CANCELLABLE) return true;
    else if (prev == CANCELLED) return false;
    assert(ShenandoahSuspendibleWorkers, "should not get here when not using suspendible workers");
    assert(prev == NOT_CANCELLED, "must be NOT_CANCELLED");
    {
      // We need to provide a safepoint here, otherwise we might
      // spin forever if a SP is pending.
      ThreadBlockInVM sp(JavaThread::current());
      SpinPause();
    }
  }
}

void ShenandoahHeap::cancel_gc(GCCause::Cause cause) {
  if (try_cancel_gc()) {
    FormatBuffer<> msg("Cancelling GC: %s", GCCause::to_string(cause));
    log_info(gc)("%s", msg.buffer());
    Events::log(Thread::current(), "%s", msg.buffer());
  }
}

uint ShenandoahHeap::max_workers() {
  return _max_workers;
}

void ShenandoahHeap::stop() {
  // The shutdown sequence should be able to terminate when GC is running.

  // Step 0. Notify policy to disable event recording.
  _shenandoah_policy->record_shutdown();

  // Step 1. Notify control thread that we are in shutdown.
  // Note that we cannot do that with stop(), because stop() is blocking and waits for the actual shutdown.
  // Doing stop() here would wait for the normal GC cycle to complete, never falling through to cancel below.
  control_thread()->prepare_for_graceful_shutdown();

  // Step 2. Notify GC workers that we are cancelling GC.
  cancel_gc(GCCause::_shenandoah_stop_vm);

  // Step 3. Wait until GC worker exits normally.
  control_thread()->stop();

  // Step 4. Stop String Dedup thread if it is active
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::stop();
  }
}

void ShenandoahHeap::unload_classes_and_cleanup_tables(bool full_gc) {
  assert(heuristics()->can_unload_classes(), "Class unloading should be enabled");

  ShenandoahGCPhase root_phase(full_gc ?
                               ShenandoahPhaseTimings::full_gc_purge :
                               ShenandoahPhaseTimings::purge);

  ShenandoahIsAliveSelector alive;
  BoolObjectClosure* is_alive = alive.is_alive_closure();

  bool purged_class;

  // Unload classes and purge SystemDictionary.
  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_class_unload :
                            ShenandoahPhaseTimings::purge_class_unload);
    purged_class = SystemDictionary::do_unloading(gc_timer());
  }

  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_par :
                            ShenandoahPhaseTimings::purge_par);
    uint active = _workers->active_workers();
    ParallelCleaningTask unlink_task(is_alive, active, purged_class, true);
    _workers->run_task(&unlink_task);
  }

  {
    ShenandoahGCPhase phase(full_gc ?
                      ShenandoahPhaseTimings::full_gc_purge_cldg :
                      ShenandoahPhaseTimings::purge_cldg);
    ClassLoaderDataGraph::purge();
  }
}

void ShenandoahHeap::set_has_forwarded_objects(bool cond) {
  set_gc_state_mask(HAS_FORWARDED, cond);
}

void ShenandoahHeap::set_process_references(bool pr) {
  _process_references.set_cond(pr);
}

void ShenandoahHeap::set_unload_classes(bool uc) {
  _unload_classes.set_cond(uc);
}

bool ShenandoahHeap::process_references() const {
  return _process_references.is_set();
}

bool ShenandoahHeap::unload_classes() const {
  return _unload_classes.is_set();
}

address ShenandoahHeap::in_cset_fast_test_addr() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  assert(heap->collection_set() != NULL, "Sanity");
  return (address) heap->collection_set()->biased_map_address();
}

address ShenandoahHeap::cancelled_gc_addr() {
  return (address) ShenandoahHeap::heap()->_cancelled_gc.addr_of();
}

address ShenandoahHeap::gc_state_addr() {
  return (address) ShenandoahHeap::heap()->_gc_state.addr_of();
}

size_t ShenandoahHeap::bytes_allocated_since_gc_start() {
  return OrderAccess::load_acquire(&_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::reset_bytes_allocated_since_gc_start() {
  OrderAccess::release_store_fence(&_bytes_allocated_since_gc_start, (size_t)0);
}

void ShenandoahHeap::set_degenerated_gc_in_progress(bool in_progress) {
  _degenerated_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_in_progress(bool in_progress) {
  _full_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_move_in_progress(bool in_progress) {
  assert (is_full_gc_in_progress(), "should be");
  _full_gc_move_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_update_refs_in_progress(bool in_progress) {
  set_gc_state_mask(UPDATEREFS, in_progress);
}

void ShenandoahHeap::register_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::add_nmethod(nm);
}

void ShenandoahHeap::unregister_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::remove_nmethod(nm);
}

oop ShenandoahHeap::pin_object(JavaThread* thr, oop o) {
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_pinned();
  return o;
}

void ShenandoahHeap::unpin_object(JavaThread* thr, oop o) {
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_unpinned();
}

GCTimer* ShenandoahHeap::gc_timer() const {
  return _gc_timer;
}

#ifdef ASSERT
void ShenandoahHeap::assert_gc_workers(uint nworkers) {
  assert(nworkers > 0 && nworkers <= max_workers(), "Sanity");

  if (ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ParallelGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ParallelGCThreads, "Cannot use more than it has");
    } else {
      // Use ParallelGCThreads inside safepoints
      assert(nworkers == ParallelGCThreads, "Use ParalleGCThreads within safepoints");
    }
  } else {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ConcGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ConcGCThreads, "Cannot use more than it has");
    } else {
      // Use ConcGCThreads outside safepoints
      assert(nworkers == ConcGCThreads, "Use ConcGCThreads outside safepoints");
    }
  }
}
#endif

ShenandoahVerifier* ShenandoahHeap::verifier() {
  guarantee(ShenandoahVerify, "Should be enabled");
  assert (_verifier != NULL, "sanity");
  return _verifier;
}

template<class T>
class ShenandoahUpdateHeapRefsTask : public AbstractGangTask {
private:
  T cl;
  ShenandoahHeap* _heap;
  ShenandoahRegionIterator* _regions;
  bool _concurrent;
public:
  ShenandoahUpdateHeapRefsTask(ShenandoahRegionIterator* regions, bool concurrent) :
    AbstractGangTask("Concurrent Update References Task"),
    cl(T()),
    _heap(ShenandoahHeap::heap()),
    _regions(regions),
    _concurrent(concurrent) {
  }

  void work(uint worker_id) {
    if (_concurrent) {
      ShenandoahConcurrentWorkerSession worker_session(worker_id);
      ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
      do_work();
    } else {
      ShenandoahParallelWorkerSession worker_session(worker_id);
      do_work();
    }
  }

private:
  void do_work() {
    ShenandoahHeapRegion* r = _regions->next();
    ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
    while (r != NULL) {
      HeapWord* top_at_start_ur = r->concurrent_iteration_safe_limit();
      assert (top_at_start_ur >= r->bottom(), "sanity");
      if (r->is_active() && !r->is_cset()) {
        _heap->marked_object_oop_iterate(r, &cl, top_at_start_ur);
      }
      if (ShenandoahPacing) {
        _heap->pacer()->report_updaterefs(pointer_delta(top_at_start_ur, r->bottom()));
      }
      if (_heap->check_cancelled_gc_and_yield(_concurrent)) {
        return;
      }
      r = _regions->next();
    }
  }
};




void ShenandoahHeap::update_heap_references(bool concurrent) {
  ShenandoahUpdateHeapRefsTask<ShenandoahUpdateHeapRefsClosure> task(&_update_refs_iterator, concurrent);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_init_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");
  retire_and_reset_gclabs();
  make_parsable(true);
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    r->set_concurrent_iteration_safe_limit(r->top());
  }



  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    if (r->is_empty()) continue;
    collection_set()->add_region_to_update(r);
  }
  bool has_remote_update_set = collection_set()->select_local_update_regions();
  // Reset iterator.
  _update_refs_iterator.reset();
  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::update_roots);
    update_roots();
  }
  collection_set()->copy_cset_to_sync();
 
  ShenandoahHeapLocker locker(lock());

  for(size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* region = get_region(i);
    region->sync_between_mem_and_cpu()->_top = region->top();
    region->sync_between_mem_and_cpu()->_state = region->semeru_state_ordinal();
  }

  _cpu_server_flags->_should_start_tracing = false;
  _cpu_server_flags->_tracing_all_finished = true;
  _cpu_server_flags->_should_start_evacuation = false;
  _cpu_server_flags->_evacuation_all_finished = true;
  _cpu_server_flags->_should_start_update = true;
  _cpu_server_flags->_update_all_finished = false;


  set_update_refs_in_progress(true);

  log_debug(semeru)("Finish init update roots!");
}



void ShenandoahHeap::op_final_sync(size_t max_region_num_for_record) {
  // Modified by Haoran
  // Moved from the init_update_refs
  _cpu_server_flags->_should_start_evacuation = false;
  _cpu_server_flags->_evacuation_all_finished = true;
  _cpu_server_flags->_should_start_update = false;
  _cpu_server_flags->_update_all_finished = true;
  _cpu_server_flags-> _is_cpu_server_in_stw = false;
  _cpu_server_flags->_cpu_server_data_sent = false;
  _cpu_server_flags->_should_start_tracing = false;
  _cpu_server_flags->_tracing_all_finished = true;

  ShenandoahRegionIterator regions;
  ShenandoahHeapRegion* region;


  size_t calc_used = 0;
  regions.reset();
  region = regions.next();
  while(region != NULL) {
    calc_used += (size_t)region->top() - (size_t)region->bottom();
    region = regions.next();   
  } 
  tty->print("HEAP used: 0x%lx, calc_used: 0x%lx\n", used(), calc_used);
}

void ShenandoahHeap::op_do_updaterefs(){
  // WorkGang* workers = workers();
  uint nworkers = _workers->active_workers();
  ShenandoahUpdateHeapRefsClosure cl;
  ShenandoahSemeruUpdateHeapRefsTask task(this, _collection_set, true);
  _workers->run_task(&task);
}

void ShenandoahHeap::op_final_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");


 
  // FREE_C_HEAP_ARRAY(size_t, pre_top);

  // rdma_write_bitmap_all();






  // Modified by Haoran
  // Check if there is left-over work, and finish it
  // if (_update_refs_iterator.has_next()) {
  //   ShenandoahGCPhase final_work(ShenandoahPhaseTimings::final_update_refs_finish_work);

  //   // Finish updating references where we left off.
  //   clear_cancelled_gc();
  //   update_heap_references(false);
  // }

  // Clear cancelled GC, if set. On cancellation path, the block before would handle
  // everything. On degenerated paths, cancelled gc would not be set anyway.
  if (cancelled_gc()) {
    clear_cancelled_gc();
  }
  assert(!cancelled_gc(), "Should have been done right before");

  // Haoran: TODO restore verifiers.
  // if (ShenandoahVerify && !is_degenerated_gc_in_progress()) {
  //   verifier()->verify_roots_no_forwarded_except(ShenandoahRootVerifier::ThreadRoots);
  // }

  if (is_degenerated_gc_in_progress()) {
    concurrent_mark()->update_roots(ShenandoahPhaseTimings::degen_gc_update_roots);
  } else {
    concurrent_mark()->update_thread_roots(ShenandoahPhaseTimings::final_update_refs_roots);
  }



  ShenandoahGCPhase final_update_refs(ShenandoahPhaseTimings::final_update_refs_recycle);

  trash_cset_regions();
  set_has_forwarded_objects(false);
  set_update_refs_in_progress(false);

  if (ShenandoahVerify) {
    verifier()->verify_roots_no_forwarded();
    verifier()->verify_after_updaterefs();
  }

  if (VerifyAfterGC) {
    Universe::verify();
  }

  {
    ShenandoahHeapLocker locker(lock());
    _free_set->rebuild();
  }
}

#ifdef ASSERT
void ShenandoahHeap::assert_heaplock_owned_by_current_thread() {
  _lock.assert_owned_by_current_thread();
}

void ShenandoahHeap::assert_heaplock_not_owned_by_current_thread() {
  _lock.assert_not_owned_by_current_thread();
}

void ShenandoahHeap::assert_heaplock_or_safepoint() {
  _lock.assert_owned_by_current_thread_or_safepoint();
}
#endif

void ShenandoahHeap::print_extended_on(outputStream *st) const {
  print_on(st);
  print_heap_regions_on(st);
}

bool ShenandoahHeap::is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self) {
  size_t slice = r->region_number() / _bitmap_regions_per_slice;

  size_t regions_from = _bitmap_regions_per_slice * slice;
  size_t regions_to   = MIN2(num_regions(), _bitmap_regions_per_slice * (slice + 1));
  for (size_t g = regions_from; g < regions_to; g++) {
    assert (g / _bitmap_regions_per_slice == slice, "same slice");
    if (skip_self && g == r->region_number()) continue;
    if (get_region(g)->is_committed()) {
      return true;
    }
  }
  return false;
}

bool ShenandoahHeap::commit_bitmap_slice(ShenandoahHeapRegion* r) {
  assert_heaplock_owned_by_current_thread();

  // Bitmaps in special regions do not need commits
  if (_bitmap_region_special) {
    return true;
  }

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is already committed, meaning the bitmap
    // slice is already committed, we exit right away.
    return true;
  }

  // Commit the bitmap slice:
  size_t slice = r->region_number() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  if (!os::commit_memory((char*)_bitmap_region.start() + off, len, false)) {
    return false;
  }
  return true;
}

bool ShenandoahHeap::uncommit_bitmap_slice(ShenandoahHeapRegion *r) {
  assert_heaplock_owned_by_current_thread();

  // Bitmaps in special regions do not need uncommits
  if (_bitmap_region_special) {
    return true;
  }

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is still committed, meaning the bitmap
    // slice is should stay committed, exit right away.
    return true;
  }

  // Uncommit the bitmap slice:
  size_t slice = r->region_number() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  if (!os::uncommit_memory((char*)_bitmap_region.start() + off, len)) {
    return false;
  }
  return true;
}

void ShenandoahHeap::safepoint_synchronize_begin() {
  if (ShenandoahSuspendibleWorkers || UseStringDeduplication) {
    SuspendibleThreadSet::synchronize();
  }
}

void ShenandoahHeap::safepoint_synchronize_end() {
  if (ShenandoahSuspendibleWorkers || UseStringDeduplication) {
    SuspendibleThreadSet::desynchronize();
  }
}

void ShenandoahHeap::vmop_entry_init_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitMark op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahHeap::vmop_entry_heap_flush() {
  // TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  // ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  // ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  VM_ShenandoahHeapFlush op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahHeap::vmop_entry_satb_flush() {
  // TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  // ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  // ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  VM_ShenandoahSATBFlush op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahHeap::vmop_entry_final_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalMarkStartEvac op;
  VMThread::execute(&op); // jump to entry_final_mark under safepoint
}

void ShenandoahHeap::vmop_entry_final_evac() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac_gross);

  VM_ShenandoahFinalEvac op;
  VMThread::execute(&op); // jump to entry_final_evac under safepoint
}

void ShenandoahHeap::vmop_entry_init_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_final_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_init_traversal() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_traversal_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitTraversalGC op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_final_traversal() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_traversal_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalTraversalGC op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_full(GCCause::Cause cause) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFullGC op(cause);
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_degenerated(ShenandoahDegenPoint point) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc_gross);

  VM_ShenandoahDegeneratedGC degenerated_gc((int)point);
  VMThread::execute(&degenerated_gc);
}

void ShenandoahHeap::entry_init_mark() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark);
  const char* msg = init_mark_event_message();
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_init_marking(),
                              "init marking");

  op_init_mark();
}

void ShenandoahHeap::entry_heap_flush() {
  // ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  // ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark);
  // const char* msg = init_mark_event_message();
  // GCTraceTime(Info, gc) time(msg, gc_timer());
  // EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_init_marking(),
                              "init marking");

  op_heap_flush();
}

void ShenandoahHeap::entry_satb_flush() {
  // ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  // ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark);
  // const char* msg = init_mark_event_message();
  // GCTraceTime(Info, gc) time(msg, gc_timer());
  // EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_init_marking(),
                              "init marking");

  op_satb_flush();
}

void ShenandoahHeap::entry_final_mark() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark);
  const char* msg = final_mark_event_message();
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_marking(),
                              "final marking");

  op_final_mark();
}

void ShenandoahHeap::entry_final_evac() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac);
  static const char* msg = "Pause Final Evac";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  op_final_evac();
}

void ShenandoahHeap::entry_init_updaterefs() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs);

  static const char* msg = "Pause Init Update Refs";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  // No workers used in this phase, no setup required

  op_init_updaterefs();
}

void ShenandoahHeap::entry_final_updaterefs() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs);

  static const char* msg = "Pause Final Update Refs";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_update_ref(),
                              "final reference update");

  op_final_updaterefs();
}

void ShenandoahHeap::entry_init_traversal() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_traversal_gc);

  static const char* msg = "Pause Init Traversal";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_stw_traversal(),
                              "init traversal");

  op_init_traversal();
}

void ShenandoahHeap::entry_final_traversal() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_traversal_gc);

  static const char* msg = "Pause Final Traversal";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_stw_traversal(),
                              "final traversal");

  op_final_traversal();
}

void ShenandoahHeap::entry_full(GCCause::Cause cause) {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc);

  static const char* msg = "Pause Full";
  GCTraceTime(Info, gc) time(msg, gc_timer(), cause, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_fullgc(),
                              "full gc");

  op_full(cause);
}

void ShenandoahHeap::entry_degenerated(int point) {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc);

  ShenandoahDegenPoint dpoint = (ShenandoahDegenPoint)point;
  const char* msg = degen_event_message(dpoint);
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_stw_degenerated(),
                              "stw degenerated gc");

  set_degenerated_gc_in_progress(true);
  op_degenerated(dpoint);
  set_degenerated_gc_in_progress(false);
}

void ShenandoahHeap::entry_mark() {
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  const char* msg = conc_mark_event_message();
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent marking");

  try_inject_alloc_failure();
  op_mark();
}

void ShenandoahHeap::entry_evac() {
  ShenandoahGCPhase conc_evac_phase(ShenandoahPhaseTimings::conc_evac);
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  static const char* msg = "Concurrent evacuation";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_evac(),
                              "concurrent evacuation");

  try_inject_alloc_failure();
  op_conc_evac();
}

void ShenandoahHeap::entry_updaterefs() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_update_refs);

  static const char* msg = "Concurrent update references";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_update_ref(),
                              "concurrent reference update");

  try_inject_alloc_failure();
  op_updaterefs();
}
void ShenandoahHeap::entry_cleanup() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  // This phase does not use workers, no need for setup

  try_inject_alloc_failure();
  op_cleanup();
}

void ShenandoahHeap::entry_reset() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_reset);

  static const char* msg = "Concurrent reset";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_reset(),
                              "concurrent reset");

  try_inject_alloc_failure();
  op_reset();
}

void ShenandoahHeap::entry_preclean() {
  if (ShenandoahPreclean && process_references()) {
    static const char* msg = "Concurrent precleaning";
    GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
    EventMark em("%s", msg);

    ShenandoahGCPhase conc_preclean(ShenandoahPhaseTimings::conc_preclean);

    ShenandoahWorkerScope scope(workers(),
                                ShenandoahWorkerPolicy::calc_workers_for_conc_preclean(),
                                "concurrent preclean",
                                /* check_workers = */ false);

    try_inject_alloc_failure();
    op_preclean();
  }
}

void ShenandoahHeap::entry_traversal() {
  static const char* msg = "Concurrent traversal";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_traversal(),
                              "concurrent traversal");

  try_inject_alloc_failure();
  op_traversal();
}

void ShenandoahHeap::entry_uncommit(double shrink_before) {
  static const char *msg = "Concurrent uncommit";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_uncommit);

  op_uncommit(shrink_before);
}

void ShenandoahHeap::try_inject_alloc_failure() {
  if (ShenandoahAllocFailureALot && !cancelled_gc() && ((os::random() % 1000) > 950)) {
    _inject_alloc_failure.set();
    os::naked_short_sleep(1);
    if (cancelled_gc()) {
      log_info(gc)("Allocation failure was successfully injected");
    }
  }
}

bool ShenandoahHeap::should_inject_alloc_failure() {
  return _inject_alloc_failure.is_set() && _inject_alloc_failure.try_unset();
}

void ShenandoahHeap::initialize_serviceability() {
  _memory_pool = new ShenandoahMemoryPool(this);
  _cycle_memory_manager.add_pool(_memory_pool);
  _stw_memory_manager.add_pool(_memory_pool);
}

// Modified by Haoran
size_t ShenandoahHeap::get_server_id(HeapWord* addr) {
  size_t addr_num = (size_t)addr;
  // Modified by Haoran2
  size_t per_bytes = MaxHeapSize / NUM_OF_MEMORY_SERVER;
  // size_t per_bytes = _shenandoah_policy->max_heap_byte_size() / NUM_OF_MEMORY_SERVER;
  return (addr_num - (size_t)base())/per_bytes;
}

GrowableArray<GCMemoryManager*> ShenandoahHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(2);
  memory_managers.append(&_cycle_memory_manager);
  memory_managers.append(&_stw_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> ShenandoahHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_memory_pool);
  return memory_pools;
}

MemoryUsage ShenandoahHeap::memory_usage() {
  return _memory_pool->get_memory_usage();
}

void ShenandoahHeap::enter_evacuation() {
  _oom_evac_handler.enter_evacuation();
}

void ShenandoahHeap::leave_evacuation() {
  _oom_evac_handler.leave_evacuation();
}

ShenandoahRegionIterator::ShenandoahRegionIterator() :
  _heap(ShenandoahHeap::heap()),
  _index(0) {}

ShenandoahRegionIterator::ShenandoahRegionIterator(ShenandoahHeap* heap) :
  _heap(heap),
  _index(0) {}

void ShenandoahRegionIterator::reset() {
  _index = 0;
}

bool ShenandoahRegionIterator::has_next() const {
  return _index < _heap->num_regions();
}

void ShenandoahHeap::assure_read_for_region_index_no_sleep(size_t region_index) {
  // // Haoran: TODO, port to multi servers.
  // size_t server_id = 0; // get_server_id()
  // _assure_read_flags[region_index]->set_byte_flag(false);
  // syscall(RDMA_READ, server_id, _assure_read_flags[region_index], RDMA_FLAG_PER_SIZE);
  // while(1) {
  //   if(_assure_read_flags[region_index]->get_byte_flag() == 1){
  //     break;
  //   }
  // }
  // _assure_read_flags[region_index]->set_byte_flag(false);
}

// Modified by Haoran
void ShenandoahHeap::assure_write(size_t server_id) {
  // MutexLocker x(_assure_read_monitor, Mutex::_no_safepoint_check_flag);
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
  syscall(RDMA_READ, server_id, _recv_mem_server_cset, MEMORY_SERVER_CSET_SIZE);
  while(1) {
    if(_recv_mem_server_cset->_write_check_bit_shenandoah == 1){
      break;
    }    

    os::naked_short_sleep(50);
  }
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
}

void ShenandoahHeap::assure_read(size_t server_id) {
  // MutexLocker x(_assure_read_monitor, Mutex::_no_safepoint_check_flag);
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
  syscall(RDMA_READ, server_id, _recv_mem_server_cset, MEMORY_SERVER_CSET_SIZE);
  while(1) {
    if(_recv_mem_server_cset->_write_check_bit_shenandoah == 1){
      break;
    }
    os::naked_short_sleep(50);
  }
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
}

void ShenandoahHeap::assure_read_no_wait(size_t server_id) {
  // MutexLocker x(_assure_read_monitor, Mutex::_no_safepoint_check_flag);
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
  syscall(RDMA_READ, server_id, _recv_mem_server_cset, MEMORY_SERVER_CSET_SIZE);
  while(1) {
    if(_recv_mem_server_cset->_write_check_bit_shenandoah == 1){
      break;
    }
  }
  _recv_mem_server_cset->_write_check_bit_shenandoah = 0;
}

char ShenandoahHeap::gc_state() const {
  return _gc_state.raw_value();
}

void ShenandoahHeap::deduplicate_string(oop str) {
  assert(java_lang_String::is_instance(str), "invariant");

  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::deduplicate(str);
  }
}

const char* ShenandoahHeap::init_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Pause Init Mark (update refs) (process weakrefs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Pause Init Mark (update refs) (process weakrefs)";
  } else if (update_refs && unload_cls) {
    return "Pause Init Mark (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Pause Init Mark (process weakrefs) (unload classes)";
  } else if (update_refs) {
    return "Pause Init Mark (update refs)";
  } else if (proc_refs) {
    return "Pause Init Mark (process weakrefs)";
  } else if (unload_cls) {
    return "Pause Init Mark (unload classes)";
  } else {
    return "Pause Init Mark";
  }
}

const char* ShenandoahHeap::final_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Pause Final Mark (update refs) (process weakrefs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Pause Final Mark (update refs) (process weakrefs)";
  } else if (update_refs && unload_cls) {
    return "Pause Final Mark (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Pause Final Mark (process weakrefs) (unload classes)";
  } else if (update_refs) {
    return "Pause Final Mark (update refs)";
  } else if (proc_refs) {
    return "Pause Final Mark (process weakrefs)";
  } else if (unload_cls) {
    return "Pause Final Mark (unload classes)";
  } else {
    return "Pause Final Mark";
  }
}

const char* ShenandoahHeap::conc_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Concurrent marking (update refs) (process weakrefs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Concurrent marking (update refs) (process weakrefs)";
  } else if (update_refs && unload_cls) {
    return "Concurrent marking (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Concurrent marking (process weakrefs) (unload classes)";
  } else if (update_refs) {
    return "Concurrent marking (update refs)";
  } else if (proc_refs) {
    return "Concurrent marking (process weakrefs)";
  } else if (unload_cls) {
    return "Concurrent marking (unload classes)";
  } else {
    return "Concurrent marking";
  }
}

const char* ShenandoahHeap::degen_event_message(ShenandoahDegenPoint point) const {
  switch (point) {
    case _degenerated_unset:
      return "Pause Degenerated GC (<UNSET>)";
    case _degenerated_traversal:
      return "Pause Degenerated GC (Traversal)";
    case _degenerated_outside_cycle:
      return "Pause Degenerated GC (Outside of Cycle)";
    case _degenerated_mark:
      return "Pause Degenerated GC (Mark)";
    case _degenerated_evac:
      return "Pause Degenerated GC (Evacuation)";
    case _degenerated_updaterefs:
      return "Pause Degenerated GC (Update Refs)";
    default:
      ShouldNotReachHere();
      return "ERROR";
  }
}

jushort* ShenandoahHeap::get_liveness_cache(uint worker_id) {
#ifdef ASSERT
  assert(_liveness_cache != NULL, "sanity");
  assert(worker_id < _max_workers, "sanity");
  for (uint i = 0; i < num_regions(); i++) {
    assert(_liveness_cache[worker_id][i] == 0, "liveness cache should be empty");
  }
#endif
  return _liveness_cache[worker_id];
}

void ShenandoahHeap::flush_liveness_cache(uint worker_id) {
  assert(worker_id < _max_workers, "sanity");
  assert(_liveness_cache != NULL, "sanity");
  jushort* ld = _liveness_cache[worker_id];
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    jushort live = ld[i];
    if (live > 0) {
      r->increase_live_data_gc_words(live);
      ld[i] = 0;
    }
  }
}
