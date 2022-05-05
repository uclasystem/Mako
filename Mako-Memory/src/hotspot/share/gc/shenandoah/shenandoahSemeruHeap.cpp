// Modified by Haoran
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

#include "precompiled.hpp"
#include "memory/allocation.hpp"

#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/memAllocator.hpp"
#include "gc/shared/parallelCleaning.hpp"
#include "gc/shared/plab.hpp"

#include "gc/shenandoah/makoOffsetTable.hpp"
#include "gc/shenandoah/shenandoahAllocTracker.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shared/rdmaStructure.inline.hpp"

#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahSemeruCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahSemeruControlThread.hpp"
#include "gc/shenandoah/shenandoahSemeruFreeSet.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahMarkCompact.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahMemoryPool.hpp"
#include "gc/shenandoah/shenandoahMetrics.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahPacer.inline.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.hpp"
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

#include "memory/metaspace.hpp"
#include "runtime/vmThread.hpp"
#include "services/mallocTracker.hpp"

#include <x86intrin.h>

#ifdef ASSERT
template <class T>
void ShenandoahSemeruAssertToSpaceClosure::do_oop_work(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (! CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    shenandoah_assert_not_forwarded(p, obj);
  }
}

void ShenandoahSemeruAssertToSpaceClosure::do_oop(narrowOop* p) { do_oop_work(p); }
void ShenandoahSemeruAssertToSpaceClosure::do_oop(oop* p)       { do_oop_work(p); }
#endif

class ShenandoahSemeruPretouchHeapTask : public AbstractGangTask {
private:
  ShenandoahSemeruRegionIterator _regions;
  const size_t _page_size;
public:
  ShenandoahSemeruPretouchHeapTask(size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Heap"),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahSemeruHeapRegion* r = _regions.next();
    while (r != NULL) {
      os::pretouch_memory(r->bottom(), r->end(), _page_size);
      r = _regions.next();
    }
  }
};

class ShenandoahSemeruPretouchBitmapTask : public AbstractGangTask {
private:
  ShenandoahSemeruRegionIterator _regions;
  char* _bitmap_base;
  const size_t _bitmap_size;
  const size_t _page_size;
public:
  ShenandoahSemeruPretouchBitmapTask(char* bitmap_base, size_t bitmap_size, size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Bitmap"),
    _bitmap_base(bitmap_base),
    _bitmap_size(bitmap_size),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahSemeruHeapRegion* r = _regions.next();
    while (r != NULL) {
      size_t start = r->region_number()       * ShenandoahSemeruHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      size_t end   = (r->region_number() + 1) * ShenandoahSemeruHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      assert (end <= _bitmap_size, "end is sane: " SIZE_FORMAT " < " SIZE_FORMAT, end, _bitmap_size);

      os::pretouch_memory(_bitmap_base + start, _bitmap_base + end, _page_size);

      r = _regions.next();
    }
  }
};

jint ShenandoahSemeruHeap::initialize() {
  log_debug(heap)("Error in %s \n",__func__);
	assert(false, "%s, For SemeruCollectedHeap, Should not reach here.\n",__func__);

	return JNI_OK;
}

/**
 * Semeru
 *  
 */
jint ShenandoahSemeruHeap::initialize_memory_pool(){
    
  //assert(false, "ShenandoahSemeruHeap::%s, Never reach here.",__func__);
  //tty->print("ShenandoahSemeruHeap::%s, Never reach here.",__func__);
  //Could reach here now
	log_debug(heap)("Enter function %s \n", __func__);


  initialize_heuristics();

  //
  // Figure out heap sizing
  //


  // [x] Build the Semeru memory pool policy seperately
  size_t init_byte_size = semeru_collector_policy()->initial_heap_byte_size();
  size_t min_byte_size  = semeru_collector_policy()->min_heap_byte_size(); // mhrcr: useless, always 0
  size_t max_byte_size  = semeru_collector_policy()->max_heap_byte_size(); // mhrcr: always the same as init byte size
  size_t heap_alignment = semeru_collector_policy()->heap_alignment();

  size_t reg_size_bytes = ShenandoahSemeruHeapRegion::region_size_bytes();

  if (ShenandoahAlwaysPreTouch) {
    // Enabled pre-touch means the entire heap is committed right away.
    init_byte_size = max_byte_size;
  }

  // -X:SemeruMemPoolAlignment
	// [x] Shrink the size in product mode. [x]
	size_t reserved_for_rdma_data = RDMA_STRUCTURE_SPACE_SIZE;
  log_info(heap)("%s, init_byte_size : 0x%llx, max_byte_size : 0x%llx, heap_alignment : 0x%llx \n", __func__, (unsigned long long)init_byte_size, (unsigned long long)max_byte_size,(unsigned long long)heap_alignment);


  Universe::check_alignment(max_byte_size,  reg_size_bytes, "Shenandoah Semeru heap");
  Universe::check_alignment(init_byte_size, reg_size_bytes, "Shenandoah Semeru heap");



	// [x] Simplify the design. Always disable the compressed oops.
	//		Assuming the Java heap for big data applications is bigger than 32GB.
	//
	// [x] Reserve some space, reserved_for_rdma_data, for the data transfered by the RDMA.
	// 		 All the space, max_byte_size + reserved_for_rdma_data, will be registered as RDMA space.
	ReservedSpace all_rs = Universe::reserve_semeru_memory_pool( max_byte_size + reserved_for_rdma_data, heap_alignment);

	// Record the whole semer resered space
	set_semeru_reserved_space(&all_rs);
	#ifdef ASSERT
		log_info(heap)("%s, Request memory from OS at specific address passed. \n", __func__);
	#endif
  // |----- RDMA data structure(reserved_for_rdma_data) ----------|---- normal Java heap(max_byte_size)-----------|
	//	Here initialize the second part, max_byte_size, as collectedHeap.
  initialize_semeru_reserved((HeapWord*)all_rs.base() + reserved_for_rdma_data/HeapWordSize, (HeapWord*)(all_rs.base() + all_rs.size()));


 	// Carve out the reserved space for rdma metadata.
	//  [0, TARGET_OBJ_SIZE_BYTE] [alive_bitmap] [dest_bitmap] [reserved] | [Java Heap]
	ReservedSpace rdma_rs = all_rs.first_part(reserved_for_rdma_data);
	#ifdef ASSERT
		tty->print("%s, Reserve space for RDMA data structure, [0x%lx, 0x%lx) \n", __func__, 
																																	(size_t)rdma_rs.base() , 
																																	(size_t)(rdma_rs.base() + rdma_rs.size()) );
		tty->print("%s, current thread is VMThread ? %d, %s, 0x%lx \n", __func__,
																										(int)(Thread::current()->is_VM_thread()),
																										((VMThread*)Thread::current())->name(),
																										(size_t)Thread::current() );
	#endif



  ReservedSpace heap_rs = all_rs.last_part(reserved_for_rdma_data);   
	#ifdef ASSERT
		tty->print("%s, Reserve Shenandoah Java heap, [0x%lx, 0x%lx) \n", __func__, 
																																	(size_t)heap_rs.base() , 
																																	(size_t)(heap_rs.base() + heap_rs.size()) );
	#endif


  //
	// Allocate and intialize the meta data RDMA structure here
	//

	// For the memory_server_cset, we need to allocate space && invoke construction, so it should be operator new().
	// if _mem_server_cset->_num_regions != 0, means new data are sent to Semeru memory server
	//	  the real data is stored in flexible array, _mem_server_cset->_region_cset[]
	// 2) Commit the space directly.

  // Haoran: TODO
	char * area_start;
	size_t area_size;
	
	area_start = rdma_rs.base() + MEMORY_SERVER_CSET_OFFSET;
	area_size	=	MEMORY_SERVER_CSET_SIZE;
	_recv_mem_server_cset 	= new(area_size, area_start) received_memory_server_cset();
	
	area_start = rdma_rs.base() + FLAGS_OF_CPU_SERVER_STATE_OFFSET;
	area_size  = FLAGS_OF_CPU_SERVER_STATE_SIZE;
	_cpu_server_flags				=	new(area_size, area_start) flags_of_cpu_server_state();

	area_start = rdma_rs.base() + FLAGS_OF_MEM_SERVER_STATE_OFFSET;
	area_size  = FLAGS_OF_MEM_SERVER_STATE_SIZE;
	_mem_server_flags				=	new(area_size, area_start) flags_of_mem_server_state();

	area_start =  rdma_rs.base() + FLAGS_OF_CPU_WRITE_CHECK_OFFSET;
	area_size  = FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT;
	_rdma_write_check_flags = new(area_size, area_start) flags_of_rdma_write_check(area_start, area_size, sizeof(uint32_t)); 

  _satb_metadata_mem_server    = new(SATB_METADATA_MEM_SERVER_SIZE, rdma_rs.base() + SATB_METADATA_MEM_SERVER_OFFSET) SATBMetadataMemServer();
  _satb_metadata_cpu_server    = new(SATB_METADATA_CPU_SERVER_SIZE, rdma_rs.base() + SATB_METADATA_CPU_SERVER_OFFSET) SATBMetadataCPUServer();

  // for(int i = 0; i < NUM_OF_MEMORY_SERVER; i ++) {
  //   _mem_ref_queues[i]  = new (MEM_REF_Q_LEN, i)MemRefQueue();
  //   // _mem_ref_queues[i]->initialize(i, (HeapWord*)(heap_rs.base() + max_byte_size / NUM_OF_MEMORY_SERVER * i ));
  // }

  _mem_ref_current_queue_has_data = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET) RDMAByteFlag(false);
  _mem_ref_current_queue_all_here = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);
  _mem_ref_other_queue_has_data = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + 2 * RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);
  _tracing_current_cycle_processing = new(RDMA_FLAG_PER_SIZE, rdma_rs.base() + RDMA_FLAG_OFFSET + 3 * RDMA_FLAG_PER_SIZE) RDMAByteFlag(false);



  size_t index = 0;
  _root_object_queue = new (CROSS_REGION_REF_UPDATE_Q_LEN, index)RootObjectQueue();
  _root_object_queue->initialize(0, (HeapWord*)heap_rs.base());
  // for(size_t ind = 1; ind < ShenandoahSemeruHeapRegion::region_count(); ind ++){
  //   RootObjectQueue* _root_object_queue_useless = new (CROSS_REGION_REF_UPDATE_Q_LEN, ind)RootObjectQueue();
  // }

  #ifdef ASSERT
		log_debug(semeru, alloc)("%s, Meta data allocation Start\n", __func__);
		log_debug(semeru, alloc)("	received_memory_server_cset 0x%lx, flexible array 0x%lx, read bit %lu", 
																							(size_t)_recv_mem_server_cset, (size_t)_recv_mem_server_cset->_region_cset, _recv_mem_server_cset->_write_check_bit_shenandoah);
		log_debug(semeru, alloc)("	flags_of_cpu_server_state  0x%lx, flexible array 0x%lx",  
																							(size_t)_cpu_server_flags, (size_t)0 );
		log_debug(semeru, alloc)("	flags_of_mem_server_state  0x%lx, flexible array 0x%lx",  
																							(size_t)_mem_server_flags, (size_t)0 );
		log_debug(semeru, alloc)("	flags_of_rdma_write_check  0x%lx, flexible array 0x%lx",  
																							(size_t)_rdma_write_check_flags, (size_t)_rdma_write_check_flags->one_sided_rdma_write_check_flags_base );
		log_debug(semeru, alloc)("%s, Meta data allocation End \n", __func__);
	#endif
	// Debug
	// Do padding for the first GB meta data space. Until the start of alive_bitmap.
	// _debug_rdma_padding_flag_variable		= new(RDMA_PADDING_SIZE_LIMIT, rdma_rs.base() + RDMA_PADDING_OFFSET) rdma_padding();
	// tty->print("WARNING in %s, padding data in Meta Region[0x%lx, 0x%lx) for meta flag variables. \n",__func__,
	// 																																								(size_t)(rdma_rs.base() + RDMA_PADDING_OFFSET),
	// 																																								(size_t)RDMA_PADDING_SIZE_LIMIT);

	//
	// End of RDMA structure section
	//





  _num_regions = ShenandoahSemeruHeapRegion::region_count();
  // size_t num_committed_regions = init_byte_size / reg_size_bytes;
  // num_committed_regions = MIN2(num_committed_regions, _num_regions);
  // assert(num_committed_regions <= _num_regions, "sanity");
  _initial_size = _num_regions * reg_size_bytes;

  size_t num_min_regions = min_byte_size / reg_size_bytes;
  num_min_regions = MIN2(num_min_regions, _num_regions);
  assert(num_min_regions <= _num_regions, "sanity");
  _minimum_size = num_min_regions * reg_size_bytes;

  _committed = _initial_size;

  // No large pages in semeru
  size_t heap_page_size   = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  size_t bitmap_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();

  //
  // Reserve and commit memory for heap
  //

  // ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size, heap_alignment);
  // initialize_reserved_region((HeapWord*)heap_rs.base(), (HeapWord*) (heap_rs.base() + heap_rs.size()));
  _heap_region = MemRegion((HeapWord*)heap_rs.base(), heap_rs.size() / HeapWordSize);
  _heap_region_special = heap_rs.special(); //Haoran: TODO what is this

  // assert((((size_t) base()) & ShenandoahSemeruHeapRegion::region_size_bytes_mask()) == 0,
  //        "Misaligned heap: " PTR_FORMAT, p2i(base()));

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

  // ReservedSpace sh_rs = heap_rs.first_part(max_byte_size);
  // if (!_heap_region_special) {
  //   os::commit_memory_or_exit(sh_rs.base(), _initial_size, heap_alignment, false,
  //                             "Cannot commit heap memory");
  // }




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

  char* bitmap_base_addr = rdma_rs.base() + ALIVE_BITMAP_OFFSET;
  MemTracker::record_virtual_memory_type(bitmap_base_addr, mtGC);
  _bitmap_region = MemRegion((HeapWord*) bitmap_base_addr, ALIVE_BITMAP_SIZE / HeapWordSize);
  // _bitmap_region_special = bitmap.special();
  size_t bitmap_init_commit = _bitmap_bytes_per_slice * align_up(_num_regions, _bitmap_regions_per_slice) / _bitmap_regions_per_slice;
  bitmap_init_commit = MIN2(_bitmap_size, bitmap_init_commit);
  // if (!_bitmap_region_special) {
  os::commit_memory_or_exit((char *) _bitmap_region.start(), _bitmap_region.byte_size(), bitmap_page_size, false,
                            "Cannot commit bitmap memory");
  log_debug(semeru, alloc)("base_addr: 0x%lx, commit_size: 0x%lx", (size_t)bitmap_base_addr, bitmap_init_commit);
  memset(_bitmap_region.start(), 0, _bitmap_region.byte_size());
  // // Haoran: debug
  // memset((char *) _bitmap_region.start(), -1,  bitmap_init_commit);
  // log_debug(semeru, alloc)("set bitmap region to all 1: 0x%lx, 0x%lx", (size_t)_bitmap_region.start(), bitmap_init_commit);

  _marking_context = new ShenandoahMarkingContext(_heap_region, _bitmap_region, _num_regions);

  os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET), ALIVE_TABLE_MAX_SIZE, false, "Cannot commit alive map memory");
  memset((char*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET), 0, ALIVE_TABLE_MAX_SIZE);
  _alive_table = new ShenandoahAliveTable(this, (HeapWord*)heap_rs.base(), MAX_HEAP_SIZE/HeapWordSize, reg_size_bytes/HeapWordSize, ALIVE_TABLE_PAGE_SIZE, LOG_ALIVE_TABLE_PAGE_SIZE, (uint*)(SEMERU_START_ADDR + ALIVE_TABLE_OFFSET));


  os::commit_memory_or_exit((char *)(SEMERU_START_ADDR + SATB_BUFFER_OFFSET), SATB_BUFFER_SIZE_LIMIT, false,
                            "Cannot commit satb queue memory");
  memset((char *)(SEMERU_START_ADDR + SATB_BUFFER_OFFSET), 0, SATB_BUFFER_SIZE_LIMIT);

  if (ShenandoahVerify) { // mhrcr: false
    ReservedSpace verify_bitmap(_bitmap_size, bitmap_page_size);
    if (!verify_bitmap.special()) {
      os::commit_memory_or_exit(verify_bitmap.base(), verify_bitmap.size(), bitmap_page_size, false,
                                "Cannot commit verification bitmap memory");
    }
    MemTracker::record_virtual_memory_type(verify_bitmap.base(), mtGC);
    MemRegion verify_bitmap_region = MemRegion((HeapWord *) verify_bitmap.base(), verify_bitmap.size() / HeapWordSize);
    _verification_bit_map.initialize(_heap_region, verify_bitmap_region);
    _verifier = NULL; //new ShenandoahVerifier(this, &_verification_bit_map);
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

  _regions = NEW_C_HEAP_ARRAY(ShenandoahSemeruHeapRegion*, _num_regions, mtGC);
  _free_set = new ShenandoahSemeruFreeSet(this, _num_regions);
  
  // Haoran: commit address space for sync cset
  os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + CSET_SYNC_MAP_OFFSET), CSET_SYNC_MAP_SIZE, false, "Allocator (commit)");
  memset((char*)(SEMERU_START_ADDR + CSET_SYNC_MAP_OFFSET), 0, CSET_SYNC_MAP_SIZE);
  os::commit_memory_or_exit((char*)(SEMERU_START_ADDR + COMPACTED_MAP_OFFSET), COMPACTED_MAP_SIZE, false, "Allocator (commit)");
  memset((char*)(SEMERU_START_ADDR + COMPACTED_MAP_OFFSET), 0, COMPACTED_MAP_SIZE);
  
  _collection_set = new ShenandoahSemeruCollectionSet(this, heap_rs.base(), heap_rs.size(), (char*)(SEMERU_START_ADDR + CSET_SYNC_MAP_OFFSET), (char*)(SEMERU_START_ADDR + COMPACTED_MAP_OFFSET));

  _assure_read_flags = NEW_C_HEAP_ARRAY(RDMAByteFlag*, _num_regions + 3, mtGC);

  {
    ShenandoahHeapLocker locker(lock());

    size_t size_words = ShenandoahSemeruHeapRegion::region_size_words();

    for (size_t i = 0; i < _num_regions; i++) {
      HeapWord* start = (HeapWord*)heap_rs.base() + size_words * i;
      bool is_committed = i < _num_regions;
      
      ShenandoahSemeruHeapRegion* r = new ShenandoahSemeruHeapRegion(this, start, size_words, i, is_committed);
      _marking_context->semeru_initialize_top_at_mark_start(r);
      _regions[i] = r;
      assert(!collection_set()->is_in_update_set(i), "New region should not be in collection set");
    }

    // Initialize to complete
    _marking_context->mark_complete();

    // Haoran: TODO
    // _free_set->rebuild();
  }

  // mhrcr: Semeru should have touched every region

//   if (ShenandoahAlwaysPreTouch) {
//     assert(!AlwaysPreTouch, "Should have been overridden");

//     // For NUMA, it is important to pre-touch the storage under bitmaps with worker threads,
//     // before initialize() below zeroes it with initializing thread. For any given region,
//     // we touch the region and the corresponding bitmaps from the same thread.
//     ShenandoahPushWorkerScope scope(workers(), _max_workers, false);

//     size_t pretouch_heap_page_size = heap_page_size;
//     size_t pretouch_bitmap_page_size = bitmap_page_size;

// #ifdef LINUX
//     // UseTransparentHugePages would madvise that backing memory can be coalesced into huge
//     // pages. But, the kernel needs to know that every small page is used, in order to coalesce
//     // them into huge one. Therefore, we need to pretouch with smaller pages.
//     if (UseTransparentHugePages) {
//       pretouch_heap_page_size = (size_t)os::vm_page_size();
//       pretouch_bitmap_page_size = (size_t)os::vm_page_size();
//     }
// #endif

//     // OS memory managers may want to coalesce back-to-back pages. Make their jobs
//     // simpler by pre-touching continuous spaces (heap and bitmap) separately.

//     log_info(gc, init)("Pretouch bitmap: " SIZE_FORMAT " regions, " SIZE_FORMAT " bytes page",
//                        _num_regions, pretouch_bitmap_page_size);
//     ShenandoahSemeruPretouchBitmapTask bcl(bitmap.base(), _bitmap_size, pretouch_bitmap_page_size);
//     _workers->run_task(&bcl);

//     log_info(gc, init)("Pretouch heap: " SIZE_FORMAT " regions, " SIZE_FORMAT " bytes page",
//                        _num_regions, pretouch_heap_page_size);
//     ShenandoahSemeruPretouchHeapTask hcl(pretouch_heap_page_size);
//     _workers->run_task(&hcl);
//   }

  //
  // Initialize the rest of GC subsystems
  //

  _liveness_cache = NEW_C_HEAP_ARRAY(jushort*, _max_workers, mtGC);
  for (uint worker = 0; worker < _max_workers; worker++) {
    _liveness_cache[worker] = NEW_C_HEAP_ARRAY(jushort, _num_regions, mtGC);
    Copy::fill_to_bytes(_liveness_cache[worker], _num_regions * sizeof(jushort));
  }

  // Haoran: TODO finish the SATB
  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  // ShenandoahBarrierSet::satb_mark_queue_set().initialize(this,
  //                                                        SATB_Q_CBL_mon,
  //                                                        20 /*G1SATBProcessCompletedThreshold */,
  //                                                        60 /* G1SATBBufferEnqueueingThresholdPercent */,
  //                                                        Shared_SATB_Q_lock);

  // _monitoring_support = new ShenandoahMonitoringSupport(this);
  _phase_timings = new ShenandoahPhaseTimings();
  ShenandoahStringDedup::initialize();
  ShenandoahCodeRoots::initialize();

  if (ShenandoahAllocationTrace) {
    _alloc_tracker = new ShenandoahAllocTracker();
  }

  if (ShenandoahPacing) {
    ShouldNotReachHere();
    // _pacer = new ShenandoahPacer(this);
    // _pacer->setup_for_idle();
  } else {
    _pacer = NULL;
  }

  // _traversal_gc = heuristics()->can_do_traversal_gc() ?
  //                 new ShenandoahTraversalGC(this, _num_regions) :
  //                 NULL;

  _control_thread = new ShenandoahSemeruControlThread();

  log_info(gc, init)("Initialize Shenandoah heap: " SIZE_FORMAT "%s initial, " SIZE_FORMAT "%s min, " SIZE_FORMAT "%s max",
                     byte_size_in_proper_unit(_initial_size),  proper_unit_for_byte_size(_initial_size),
                     byte_size_in_proper_unit(_minimum_size),  proper_unit_for_byte_size(_minimum_size),
                     byte_size_in_proper_unit(max_capacity()), proper_unit_for_byte_size(max_capacity())
  );

  log_info(gc, init)("Safepointing mechanism: %s",
                     SafepointMechanism::uses_thread_local_poll() ? "thread-local poll" :
                     (SafepointMechanism::uses_global_page_poll() ? "global-page poll" : "unknown"));

  log_debug(semeru, alloc)("	received_memory_server_cset read bit %lu", 
																							_recv_mem_server_cset->_write_check_bit_shenandoah);


  // Haoran: TODO
  //os::commit_memory_or_exit((char*)RDMA_RESERVE_PADDING_OFFSET, RDMA_RESERVE_PADDING_SIZE, false, "Allocator (commit)");  // Commit the space.
  os::commit_memory_or_exit((char*)SEMERU_START_ADDR, RDMA_STRUCTURE_SPACE_SIZE, false, "Allocator (commit)");  // Commit the space.
  
  // Modified by Haoran for compaction
  memset((char*)SEMERU_START_ADDR, 0, RDMA_STRUCTURE_SPACE_SIZE);


  _recv_mem_server_cset->_write_check_bit_shenandoah = 1;
  
  // for(int i = 0; i < NUM_OF_MEMORY_SERVER; i ++) {
  //   // _mem_ref_queues[i]  = new (MEM_REF_Q_LEN, i)MemRefQueue();
  //   _mem_ref_queues[i]->initialize(i, (HeapWord*)(heap_rs.base() + max_byte_size / NUM_OF_MEMORY_SERVER * i ));
  // }

  log_debug(semeru, alloc)("after commit all	received_memory_server_cset read bit %lu", 
																							_recv_mem_server_cset->_write_check_bit_shenandoah);
  return JNI_OK;
}



void ShenandoahSemeruHeap::initialize_heuristics() {
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

    // Modified by Haoran2
    // if (ShenandoahStoreValEnqueueBarrier && ShenandoahStoreValReadBarrier) {
    //   vm_exit_during_initialization("Cannot use both ShenandoahStoreValEnqueueBarrier and ShenandoahStoreValReadBarrier");
    // }
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

ShenandoahSemeruHeap::ShenandoahSemeruHeap(ShenandoahSemeruCollectorPolicy* policy) :
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
  _scm(new ShenandoahSemeruConcurrentMark()),
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
  _ref_processor(NULL),
  _marking_context(NULL),
  _bitmap_size(0),
  _bitmap_regions_per_slice(0),
  _bitmap_bytes_per_slice(0),
  _bitmap_region_special(false),
  _aux_bitmap_region_special(false),
  _liveness_cache(NULL),
  _collection_set(NULL),
  _alive_table(NULL)
{
  log_info(gc, init)("GC threads: " UINT32_FORMAT " parallel, " UINT32_FORMAT " concurrent", ParallelGCThreads, ConcGCThreads);
  log_info(gc, init)("Reference processing: %s", ParallelRefProcEnabled ? "parallel" : "serial");

  // BarrierSet::set_barrier_set(new ShenandoahBarrierSet(this));

  _max_workers = MAX2(_max_workers, 1U);
  _workers = new ShenandoahWorkGang("Shenandoah GC Threads", _max_workers,
                            /* are_GC_task_threads */true,
                            /* are_ConcurrentGC_threads */false);
  if (_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _workers->initialize_workers();
  }

  if (ShenandoahParallelSafepointThreads > 1) {
    _safepoint_workers = new ShenandoahWorkGang("Safepoint Cleanup Thread",
                                                ShenandoahParallelSafepointThreads,
                                                false, false);
    _safepoint_workers->initialize_workers();
  }
}

#ifdef _MSC_VER
#pragma warning( pop )
#endif

class ShenandoahSemeruResetBitmapTask : public AbstractGangTask {
private:
  ShenandoahSemeruRegionIterator _regions;

public:
  ShenandoahSemeruResetBitmapTask() :
    AbstractGangTask("Parallel Reset Bitmap Task") {}

  void work(uint worker_id) {
    ShenandoahSemeruHeapRegion* region = _regions.next();
    ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
    ShenandoahMarkingContext* const ctx = heap->marking_context();
    ShenandoahAliveTable* const table = heap->alive_table();
    while (region != NULL) {
      // if (heap->is_bitmap_slice_committed(region)) {
      ctx->semeru_clear_bitmap(region);
      table->reset_alive_table(region->region_number());
      region = _regions.next();
    }
  }
};

// class ShenandoahSemeruResetOffsetTableTask : public AbstractGangTask {
// private:
//   ShenandoahSemeruRegionIterator _regions;

// public:
//   ShenandoahSemeruResetOffsetTableTask() :
//     AbstractGangTask("Parallel Reset Offset Table Task") {}

//   void work(uint worker_id) {
//     ShenandoahSemeruHeapRegion* region = _regions.next();
//     ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
//     while (region != NULL) {
//       assert(region->offset_table() != NULL, "Invariant!");
//       region->offset_table()->clear();
//       region = _regions.next();
//     }
//   }
// };

void ShenandoahSemeruHeap::reset_mark_bitmap() {
  assert_gc_workers(_workers->active_workers());
  mark_incomplete_marking_context();

  ShenandoahSemeruResetBitmapTask task;
  _workers->run_task(&task);
}

// void ShenandoahSemeruHeap::switch_offset_table(ShenandoahSemeruHeapRegion* region, ShenandoahSemeruHeapRegion* to_region) {
//   OffsetTable* ot = region->offset_table();
//   ot->set_from_region_start(to_region->bottom());
//   ot->set_to_region_start(region->bottom());
//   ot->set_region_id(to_region->region_number());
//   region->set_offset_table(NULL);
//   to_region->set_offset_table(ot);
// }

// void ShenandoahSemeruHeap::reset_offset_table() {
//   // ShenandoahSemeruRegionIterator regions;
//   // ShenandoahSemeruHeapRegion* region;
//   // region = regions.next();
//   // while(region != NULL) {
//   //   if(!collection_set()->is_in_evac_set(region)) {
//   //     region = regions.next();   
//   //     continue;
//   //   }
//   //   ShenandoahSemeruHeapRegion* corr_region = get_corr_region(region->region_number());
//   //   switch_offset_table(region, corr_region);
//   //   region = regions.next();               
//   // }

//   assert_gc_workers(_workers->active_workers());
//   ShenandoahSemeruResetOffsetTableTask task;
//   _workers->run_task(&task);
// }

void ShenandoahSemeruHeap::print_on(outputStream* st) const {
  st->print_cr("Shenandoah Heap");
  st->print_cr(" " SIZE_FORMAT "K total, " SIZE_FORMAT "K committed, " SIZE_FORMAT "K used",
               max_capacity() / K, committed() / K, used() / K);
  st->print_cr(" " SIZE_FORMAT " x " SIZE_FORMAT"K regions",
               num_regions(), ShenandoahSemeruHeapRegion::region_size_bytes() / K);

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

  ShenandoahSemeruCollectionSet* cset = collection_set();
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

class ShenandoahSemeruInitWorkerGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread != NULL, "Sanity");
    assert(thread->is_Worker_thread(), "Only worker thread expected");
    ShenandoahThreadLocalData::initialize_gclab(thread);
  }
};

void ShenandoahSemeruHeap::post_initialize() {
  CollectedHeap::post_initialize();
  MutexLocker ml(Threads_lock);

  ShenandoahSemeruInitWorkerGCLABClosure init_gclabs;
  _workers->threads_do(&init_gclabs);

  // gclab can not be initialized early during VM startup, as it can not determinate its max_size.
  // Now, we will let WorkGang to initialize gclab when new worker is created.
  _workers->set_initialize_gclab();

  _scm->initialize(_max_workers);
  _full_gc->initialize(_gc_timer);

  ref_processing_init();

  _heuristics->initialize();
}

size_t ShenandoahSemeruHeap::used() const {
  return OrderAccess::load_acquire(&_used);
}

size_t ShenandoahSemeruHeap::committed() const {
  OrderAccess::acquire();
  return _committed;
}

void ShenandoahSemeruHeap::increase_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed += bytes;
}

void ShenandoahSemeruHeap::decrease_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed -= bytes;
}

void ShenandoahSemeruHeap::increase_used(size_t bytes) {
  Atomic::add(bytes, &_used);
}

void ShenandoahSemeruHeap::set_used(size_t bytes) {
  OrderAccess::release_store_fence(&_used, bytes);
}

void ShenandoahSemeruHeap::decrease_used(size_t bytes) {
  assert(used() >= bytes, "never decrease heap size by more than we've left");
  Atomic::sub(bytes, &_used);
}

void ShenandoahSemeruHeap::increase_allocated(size_t bytes) {
  Atomic::add(bytes, &_bytes_allocated_since_gc_start);
}

void ShenandoahSemeruHeap::notify_mutator_alloc_words(size_t words, bool waste) {
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

size_t ShenandoahSemeruHeap::capacity() const {
  return committed();
}

size_t ShenandoahSemeruHeap::max_capacity() const {
  return _num_regions * ShenandoahSemeruHeapRegion::region_size_bytes();
}

size_t ShenandoahSemeruHeap::min_capacity() const {
  return _minimum_size;
}

size_t ShenandoahSemeruHeap::initial_capacity() const {
  return _initial_size;
}

bool ShenandoahSemeruHeap::is_in(const void* p) const {
  HeapWord* heap_base = (HeapWord*) semeru_base();
  HeapWord* last_region_end = heap_base + ShenandoahSemeruHeapRegion::region_size_words() * num_regions();
  return p >= heap_base && p < last_region_end;
}

void ShenandoahSemeruHeap::op_uncommit(double shrink_before) {
  assert (ShenandoahUncommit, "should be enabled");

  // Application allocates from the beginning of the heap, and GC allocates at
  // the end of it. It is more efficient to uncommit from the end, so that applications
  // could enjoy the near committed regions. GC allocations are much less frequent,
  // and therefore can accept the committing costs.

  size_t count = 0;
  for (size_t i = num_regions(); i > 0; i--) { // care about size_t underflow
    ShenandoahSemeruHeapRegion* r = get_region(i - 1);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      ShenandoahHeapLocker locker(lock());
      if (r->is_empty_committed()) {
        // Do not uncommit below minimal capacity
        if (committed() < min_capacity() + ShenandoahSemeruHeapRegion::region_size_bytes()) {
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

HeapWord* ShenandoahSemeruHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // New object should fit the GCLAB size
  size_t min_size = MAX2(size, PLAB::min_size());

  // Figure out size of new GCLAB, looking back at heuristics. Expand aggressively.
  size_t new_size = ShenandoahThreadLocalData::gclab_size(thread) * 2;
  new_size = MIN2(new_size, PLAB::max_size());
  new_size = MAX2(new_size, PLAB::min_size());

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

HeapWord* ShenandoahSemeruHeap::allocate_new_tlab(size_t min_size,
                                            size_t requested_size,
                                            size_t* actual_size) {
  ShenandoahSemeruAllocRequest req = ShenandoahSemeruAllocRequest::for_tlab(min_size, requested_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

HeapWord* ShenandoahSemeruHeap::allocate_new_gclab(size_t min_size,
                                             size_t word_size,
                                             size_t* actual_size) {
  // Haoran: debug
  ShouldNotReachHere();
  
  ShenandoahSemeruAllocRequest req = ShenandoahSemeruAllocRequest::for_gclab(min_size, word_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

ShenandoahSemeruHeap* ShenandoahSemeruHeap::heap() {
  CollectedHeap* heap = Universe::semeru_heap();
  assert(heap != NULL, "Unitialized access to ShenandoahSemeruHeap::heap()");
  assert(heap->kind() == CollectedHeap::Shenandoah, "not a shenandoah heap");
  return (ShenandoahSemeruHeap*) heap;
}

ShenandoahSemeruHeap* ShenandoahSemeruHeap::heap_no_check() {
  CollectedHeap* heap = Universe::semeru_heap();
  return (ShenandoahSemeruHeap*) heap;
}

HeapWord* ShenandoahSemeruHeap::allocate_memory(ShenandoahSemeruAllocRequest& req) {

  // Haoran: TODO: remove this
  // ShenandoahAllocTrace trace_alloc(req.size(), req.type());

  intptr_t pacer_epoch = 0;
  bool in_new_region = false;
  HeapWord* result = NULL;


  
  assert(req.is_gc_alloc(), "Can only accept GC allocs here");
  result = allocate_memory_under_lock(req, in_new_region);

  if (result != NULL) {
    size_t requested = req.size();
    size_t actual = req.actual_size();

    assert (req.is_lab_alloc() || (requested == actual),
            "Only LAB allocations are elastic: %s, requested = " SIZE_FORMAT ", actual = " SIZE_FORMAT,
            ShenandoahSemeruAllocRequest::alloc_type_to_string(req.type()), requested, actual);
  }

  return result;
}

HeapWord* ShenandoahSemeruHeap::allocate_memory_under_lock(ShenandoahSemeruAllocRequest& req, bool& in_new_region) {
  return _free_set->allocate(req, in_new_region);
}


HeapWord* ShenandoahSemeruHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {
  ShenandoahSemeruAllocRequest req = ShenandoahSemeruAllocRequest::for_shared(size);
  return allocate_memory(req);
}

MetaWord* ShenandoahSemeruHeap::satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
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

class ShenandoahSemeruConcurrentEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahSemeruHeap* const _heap;
  Thread* const _thread;
public:
  ShenandoahSemeruConcurrentEvacuateRegionObjectClosure(ShenandoahSemeruHeap* heap) :
    _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) {
    // if (!p->is_forwarded()) {
    if(ShenandoahForwarding::is_forwarded(p)) {
      // Have been evacuated by roots
      size_t size = (size_t) p->size();
      
      assert(_heap->alive_table()->get_target_address(p) != ShenandoahForwarding::get_forwardee(p), "Invariant!");
#ifdef RELEASE_CHECK
      oop orig_target_obj = _heap->alive_table()->get_target_address(p);
      if(orig_target_obj == ShenandoahForwarding::get_forwardee(p)) {
        ShouldNotReachHere();
      }
#endif
      // _heap->fill_with_dummy_object((HeapWord*)orig_target_obj, ((HeapWord*)orig_target_obj) + size);
    }
    else {
      oop new_p = _heap->evacuate_object(p, _thread);
      // _heap->evacuate_object(p, _thread);
      _heap->update_object(new_p);
    }
    // }
  }
};

class ShenandoahSemeruConcurrentUpdateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahSemeruHeap* const _heap;
  
public:
  ShenandoahSemeruConcurrentUpdateRegionObjectClosure(ShenandoahSemeruHeap* heap) :
    _heap(heap) {}

  void do_object(oop p) {
    _heap->update_object(p);
  }
};

inline void clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}

// inline void memory_fence()
// {
//     asm volatile ("clflush (%0)" :: "r"(p));
// }

void ShenandoahSemeruHeap::flush_cache_for_region(ShenandoahSemeruHeapRegion* r) {
  ShouldNotReachHere();
  // _mm_mfence();
  // IndirectionTable* table = Universe::table(r->region_number());
  // for(size_t i = table->table_base; i < table->table_end; i += 64) {
  //   clflush((char*)i);
  // }
  // ShenandoahSemeruHeapRegion* corr_region = NULL;
  // if(r->region_number() >= num_regions()/2) {
  //   corr_region = get_region(r->region_number() - num_regions()/2);
  // }
  // else {
  //   corr_region = get_region(r->region_number() + num_regions()/2);
  // }
  // for(size_t i = (size_t)corr_region->bottom(); i < (size_t)corr_region->top(); i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

void ShenandoahSemeruHeap::flush_cache_bitmap() {
  ShouldNotReachHere();
  // _mm_mfence();
  // for(size_t i = SEMERU_START_ADDR + ALIVE_BITMAP_OFFSET; i < SEMERU_START_ADDR + ALIVE_BITMAP_OFFSET + ALIVE_BITMAP_SIZE; i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

void ShenandoahSemeruHeap::flush_cache_range(size_t start, size_t end) {
  ShouldNotReachHere();
  // _mm_mfence();
  // for(size_t i = start/64*64; i < end; i += 64) {
  //   clflush((char*)i);
  // }
  // _mm_mfence();
}

void ShenandoahSemeruHeap::calc_offset(ShenandoahSemeruHeapRegion* region) {
  ShouldNotCallThis();
  // assert(!region->is_humongous_continuation(), "no humongous continuation regions here");
  // OffsetTable* ot = region->offset_table();
  // if(region->is_humongous_start()) {
  //   ot->set_using_byte_offset(region->bottom(), 0);
  //   return;
  // }

  // ShenandoahMarkingContext* const ctx = complete_marking_context();
  // MarkBitMap* mark_bit_map = ctx->mark_bit_map();
  // HeapWord* tams = ctx->semeru_top_at_mark_start(region);
  // assert(tams == region->top(), "Invariant!");
  // size_t skip_bitmap_delta = 1;
  // HeapWord* start = region->bottom();
  // HeapWord* end = MIN2(tams, region->end());

  // // Step 1. Scan below the TAMS based on bitmap data.
  // HeapWord* limit_bitmap = tams;

  // // Try to scan the initial candidate. If the candidate is above the TAMS, it would
  // // fail the subsequent "< limit_bitmap" checks, and fall through to Step 2.
  // HeapWord* cb = mark_bit_map->get_next_marked_addr(start, end);
  // int current_live_size = 0;

  // while (cb < limit_bitmap) {
  //   assert (cb < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(tams));
  //   // assert (cb < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(limit));
  //   oop obj = oop(cb);
  //   // Modified by Haoran for compaction
  //   // assert(oopDesc::is_oop(obj), "sanity");
  //   assert(is_in(obj), "sanity");
  //   assert(ctx->is_marked(obj), "object expected to be marked");
  //   // cl->do_object(obj);
  //   ot->set_using_byte_offset(cb, current_live_size);
  //   current_live_size += ((obj->size()) << LogHeapWordSize);
  //   cb += skip_bitmap_delta;
  //   if (cb < limit_bitmap) {
  //     cb = mark_bit_map->get_next_marked_addr(cb, limit_bitmap);
  //   }
  // }
}

class ShenandoahSemeruCalcOffsetTask : public AbstractGangTask {
private:
  ShenandoahSemeruRegionIterator _regions;
  ShenandoahSemeruHeap* const _sh;
public:
  ShenandoahSemeruCalcOffsetTask(ShenandoahSemeruHeap* sh) :
    AbstractGangTask("Parallel Calculate Offset Task"),
    _sh(sh)
  {}

  void work(uint worker_id) {
    do_work();
  }

private:
  void do_work() {
    ShenandoahSemeruHeapRegion* region = _regions.next();
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

// class ShenandoahSemeruUpdateTask : public AbstractGangTask {
// private:
//   ShenandoahSemeruHeap* const _sh;
//   ShenandoahSemeruCollectionSet* const _cs;
// public:
//   ShenandoahSemeruUpdateTask(ShenandoahSemeruHeap* sh,
//                            ShenandoahSemeruCollectionSet* cs) :
//     AbstractGangTask("Parallel Update Task"),
//     _sh(sh),
//     _cs(cs)
//   {}

//   void work(uint worker_id) {
//     ShenandoahSemeruConcurrentUpdateRegionObjectClosure update_cl(_sh);
//     ShenandoahSemeruHeapRegion* r;
//     // Modified by Haoran for remote compaction
//     while ((r =_cs->claim_next()) != NULL) {
//       if(_cs->is_in_update_set(r)) {
//         assert(r->top() > r->bottom(), "all-garbage regions are reclaimed early");
//         _sh->marked_object_iterate(r, &update_cl);
//         // log_debug(semeru)("Finish updating for region %lx", r->region_number());
//       }
//     }
//   }
// };


class ShenandoahSemeruEvacuationTask : public AbstractGangTask {
private:
  ShenandoahSemeruHeap* const _sh;
  ShenandoahSemeruCollectionSet* const _cs;
  bool _concurrent;
public:
  ShenandoahSemeruEvacuationTask(ShenandoahSemeruHeap* sh,
                           ShenandoahSemeruCollectionSet* cs,
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
    // Modified by Haoran for compaction
    // ShouldNotCallThis();
    ShenandoahSemeruConcurrentEvacuateRegionObjectClosure cl(_sh);
    ShenandoahSemeruConcurrentUpdateRegionObjectClosure update_cl(_sh);
    ShenandoahSemeruHeapRegion* r;

    // Modified by Haoran for remote compaction
    while ((r =_cs->claim_next()) != NULL) {
      // Modified by Haoran for remote compaction
      // assert(r->has_live(), "all-garbage regions are reclaimed early");
      if(_cs->is_in_evac_set(r)) {
        
        assert(r->top() > r->bottom(), "all-garbage regions are reclaimed early");
        r->_evac_top = r->bottom();
        _sh->marked_object_iterate(r, &cl);

        // _sh->fill_with_dummy_object(r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words(), r->sync_between_mem_and_cpu()->_evac_start + align_up(r->get_live_data_words(), 512), true);
#ifdef DEBUG_PRINT
        log_debug(semeru)("Finish compaction updating for region %lu", r->region_number());
#endif
      }
      else if(_cs->is_in_update_set(r)) {
        assert(!_cs->is_in_evac_set(r->region_number()), "Invariant!");
        assert(r->top() > r->bottom(), "all-garbage regions are reclaimed early");
        // assert(r->is_humongous_continuation(), "Invariant!");
        _sh->marked_object_iterate(r, &update_cl);
#ifdef DEBUG_PRINT
        log_debug(semeru)("Finish updating for region %lu", r->region_number());
#endif
      }
    }
  }
};

void ShenandoahSemeruHeap::trash_cset_regions() {
  ShouldNotCallThis();
  // ShenandoahHeapLocker locker(lock());

  // ShenandoahCollectionSet* set = collection_set();
  // ShenandoahSemeruHeapRegion* r;
  // set->clear_current_index();
  // while ((r = set->next()) != NULL) {
  //   r->make_trash();
  // }
  // collection_set()->clear();
}

void ShenandoahSemeruHeap::print_heap_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions:");
  st->print_cr("EU=empty-uncommitted, EC=empty-committed, R=regular, H=humongous start, HC=humongous continuation, CS=collection set, T=trash, P=pinned");
  st->print_cr("BTE=bottom/top/end, U=used, T=TLAB allocs, G=GCLAB allocs, S=shared allocs, L=live data");
  st->print_cr("R=root, CP=critical pins, TAMS=top-at-mark-start (previous, next)");
  st->print_cr("SN=alloc sequence numbers (first mutator, last mutator, first gc, last gc)");

  for (size_t i = 0; i < num_regions(); i++) {
    get_region(i)->print_on(st);
  }
}

void ShenandoahSemeruHeap::trash_humongous_region_at(ShenandoahSemeruHeapRegion* start) {
  ShouldNotCallThis();
  // assert(start->is_humongous_start(), "reclaim regions starting with the first one");

  // oop humongous_obj = oop(start->bottom() + ShenandoahBrooksPointer::word_size());
  // size_t size = humongous_obj->size() + ShenandoahBrooksPointer::word_size();
  // size_t required_regions = ShenandoahSemeruHeapRegion::required_regions(size * HeapWordSize);
  // size_t index = start->region_number() + required_regions - 1;

  // assert(!start->has_live(), "liveness must be zero");

  // for(size_t i = 0; i < required_regions; i++) {
  //   // Reclaim from tail. Otherwise, assertion fails when printing region to trace log,
  //   // as it expects that every region belongs to a humongous region starting with a humongous start region.
  //   ShenandoahSemeruHeapRegion* region = get_region(index --);

  //   assert(region->is_humongous(), "expect correct humongous start or continuation");
  //   assert(!region->is_cset(), "Humongous region should not be in collection set");

  //   region->make_trash_immediate();
  // }
}

class ShenandoahSemeruRetireGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
    assert(gclab != NULL, "GCLAB should be initialized for %s", thread->name());
    gclab->retire();
  }
};

void ShenandoahSemeruHeap::make_parsable(bool retire_tlabs) {
  if (UseTLAB) {
    CollectedHeap::ensure_parsability(retire_tlabs);
  }
  ShenandoahSemeruRetireGCLABClosure cl;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    cl.do_thread(t);
  }
  workers()->threads_do(&cl);
}

void ShenandoahSemeruHeap::resize_tlabs() {
  CollectedHeap::resize_all_tlabs();
}

class ShenandoahSemeruEvacuateUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootEvacuator* _rp;

public:
  ShenandoahSemeruEvacuateUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahEvacuateUpdateRootsClosure cl;

    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
    // Modified by Haoran2
    // _rp->process_evacuate_roots(&cl, &blobsCl, worker_id);
    _rp->roots_do(worker_id, &cl);
  }
};

void ShenandoahSemeruHeap::evacuate_and_update_roots() {
  ShouldNotCallThis();
// #if defined(COMPILER2) || INCLUDE_JVMCI
//   DerivedPointerTable::clear();
// #endif
//   assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

//   {
//     ShenandoahRootEvacuator rp(this, workers()->active_workers(), ShenandoahPhaseTimings::init_evac);
//     ShenandoahSemeruEvacuateUpdateRootsTask roots_task(&rp);
//     workers()->run_task(&roots_task);
//   }

// #if defined(COMPILER2) || INCLUDE_JVMCI
//   DerivedPointerTable::update_pointers();
// #endif
}

void ShenandoahSemeruHeap::roots_iterate(OopClosure* cl) {
  ShouldNotCallThis();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  // CodeBlobToOopClosure blobsCl(cl, false);
  // CLDToOopClosure cldCl(cl, ClassLoaderData::_claim_strong);

  // ShenandoahRootProcessor rp(this, 1, ShenandoahPhaseTimings::_num_phases);
  // rp.process_all_roots(cl, NULL, &cldCl, &blobsCl, NULL, 0);
}

// Returns size in bytes
size_t ShenandoahSemeruHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  if (ShenandoahElasticTLAB) {
    // With Elastic TLABs, return the max allowed size, and let the allocation path
    // figure out the safe size for current allocation.
    return ShenandoahSemeruHeapRegion::max_tlab_size_bytes();
  } else {
    return MIN2(_free_set->unsafe_peek_free(), ShenandoahSemeruHeapRegion::max_tlab_size_bytes());
  }
}

size_t ShenandoahSemeruHeap::max_tlab_size() const {
  // Returns size in words
  return ShenandoahSemeruHeapRegion::max_tlab_size_words();
}

class ShenandoahSemeruRetireAndResetGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
    gclab->retire();
    if (ShenandoahThreadLocalData::gclab_size(thread) > 0) {
      ShenandoahThreadLocalData::set_gclab_size(thread, 0);
    }
  }
};

void ShenandoahSemeruHeap::retire_and_reset_gclabs() {
  ShenandoahSemeruRetireAndResetGCLABClosure cl;
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    cl.do_thread(t);
  }
  workers()->threads_do(&cl);
}

void ShenandoahSemeruHeap::collect(GCCause::Cause cause) {
  control_thread()->request_gc(cause);
}

void ShenandoahSemeruHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

// Modified by Haoran2
// CollectorPolicy* ShenandoahSemeruHeap::collector_policy() const {
//   return _shenandoah_policy;
// }

ShenandoahSemeruCollectorPolicy* ShenandoahSemeruHeap::semeru_collector_policy() const {
  return _shenandoah_policy;
}

HeapWord* ShenandoahSemeruHeap::block_start(const void* addr) const {
  Space* sp = heap_region_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahSemeruHeap::block_size(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool ShenandoahSemeruHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  return sp->block_is_obj(addr);
}

jlong ShenandoahSemeruHeap::millis_since_last_gc() {
  double v = heuristics()->time_since_last_gc() * 1000;
  assert(0 <= v && v <= max_jlong, "value should fit: %f", v);
  return (jlong)v;
}

void ShenandoahSemeruHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    make_parsable(false);
  }
}

void ShenandoahSemeruHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::print_worker_threads_on(st);
  }
}

void ShenandoahSemeruHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
  if (_safepoint_workers != NULL) {
    _safepoint_workers->threads_do(tcl);
  }
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::threads_do(tcl);
  }
}

void ShenandoahSemeruHeap::print_tracing_info() const {
  ShouldNotCallThis();
  // LogTarget(Info, gc, stats) lt;
  // if (lt.is_enabled()) {
  //   ResourceMark rm;
  //   LogStream ls(lt);

  //   phase_timings()->print_on(&ls);

  //   ls.cr();
  //   ls.cr();

  //   shenandoah_policy()->print_gc_stats(&ls);

  //   ls.cr();
  //   ls.cr();

  //   if (ShenandoahPacing) {
  //     pacer()->print_on(&ls);
  //   }

  //   ls.cr();
  //   ls.cr();

  //   if (ShenandoahAllocationTrace) {
  //     assert(alloc_tracker() != NULL, "Must be");
  //     alloc_tracker()->print_on(&ls);
  //   } else {
  //     ls.print_cr("  Allocation tracing is disabled, use -XX:+ShenandoahAllocationTrace to enable.");
  //   }
  // }
}

void ShenandoahSemeruHeap::verify(VerifyOption vo) {
  if (ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    if (ShenandoahVerify) {
      verifier()->verify_generic(vo);
    } else {
      // TODO: Consider allocating verification bitmaps on demand,
      // and turn this on unconditionally.
    }
  }
}
size_t ShenandoahSemeruHeap::tlab_capacity(Thread *thr) const {
  return _free_set->capacity();
}

class SemeruObjectIterateScanRootClosure : public BasicOopIterateClosure {
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
  SemeruObjectIterateScanRootClosure(MarkBitMap* bitmap, Stack<oop,mtGC>* oop_stack) :
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
void ShenandoahSemeruHeap::ensure_parsability(bool retire_tlabs) {
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
void ShenandoahSemeruHeap::object_iterate(ObjectClosure* cl) {
  ShouldNotCallThis();
  // assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  // if (!_aux_bitmap_region_special && !os::commit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size(), false)) {
  //   log_warning(gc)("Could not commit native memory for auxiliary marking bitmap for heap iteration");
  //   return;
  // }

  // // Reset bitmap
  // _aux_bit_map.clear();

  // Stack<oop,mtGC> oop_stack;

  // // First, we process all GC roots. This populates the work stack with initial objects.
  // ShenandoahRootProcessor rp(this, 1, ShenandoahPhaseTimings::_num_phases);
  // SemeruObjectIterateScanRootClosure oops(&_aux_bit_map, &oop_stack);
  // CLDToOopClosure clds(&oops, ClassLoaderData::_claim_none);
  // CodeBlobToOopClosure blobs(&oops, false);
  // rp.process_all_roots(&oops, &oops, &clds, &blobs, NULL, 0);

  // // Work through the oop stack to traverse heap.
  // while (! oop_stack.is_empty()) {
  //   oop obj = oop_stack.pop();
  //   assert(oopDesc::is_oop(obj), "must be a valid oop");
  //   cl->do_object(obj);
  //   obj->oop_iterate(&oops);
  // }

  // assert(oop_stack.is_empty(), "should be empty");

  // if (!_aux_bitmap_region_special && !os::uncommit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size())) {
  //   log_warning(gc)("Could not uncommit native memory for auxiliary marking bitmap for heap iteration");
  // }
}

void ShenandoahSemeruHeap::safe_object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  object_iterate(cl);
}

void ShenandoahSemeruHeap::heap_region_iterate(ShenandoahSemeruHeapRegionClosure* blk) const {
  for (size_t i = 0; i < num_regions(); i++) {
    if(i >= num_regions()*CUR_MEMORY_SERVER_ID/NUM_OF_MEMORY_SERVER && i < num_regions()*(CUR_MEMORY_SERVER_ID+1)/NUM_OF_MEMORY_SERVER) {
      ShenandoahSemeruHeapRegion* current = get_region(i);
      blk->heap_region_do(current);
    }
  }
}

class ShenandoahSemeruParallelHeapRegionTask : public AbstractGangTask {
private:
  ShenandoahSemeruHeap* const _heap;
  ShenandoahSemeruHeapRegionClosure* const _blk;

  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(volatile size_t));
  volatile size_t _index;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, 0);

public:
  ShenandoahSemeruParallelHeapRegionTask(ShenandoahSemeruHeapRegionClosure* blk) :
          AbstractGangTask("Parallel Region Task"),
          _heap(ShenandoahSemeruHeap::heap()), _blk(blk), _index(0) {}

  void work(uint worker_id) {
    size_t stride = ShenandoahParallelRegionStride;

    size_t max = _heap->num_regions();
    while (_index < max) {
      size_t cur = Atomic::add(stride, &_index) - stride;
      size_t start = cur;
      size_t end = MIN2(cur + stride, max);
      if (start >= max) break;

      for (size_t i = cur; i < end; i++) {
        ShenandoahSemeruHeapRegion* current = _heap->get_region(i);
        // Modified by Haoran
        if(i >= _heap->num_regions()*CUR_MEMORY_SERVER_ID/NUM_OF_MEMORY_SERVER && i < _heap->num_regions()*(CUR_MEMORY_SERVER_ID+1)/NUM_OF_MEMORY_SERVER)
          _blk->heap_region_do(current);
      }
    }
  }
};

void ShenandoahSemeruHeap::parallel_heap_region_iterate(ShenandoahSemeruHeapRegionClosure* blk) const {
  assert(blk->is_thread_safe(), "Only thread-safe closures here");
  if (num_regions() > ShenandoahParallelRegionStride) {
    ShenandoahSemeruParallelHeapRegionTask task(blk);
    workers()->run_task(&task);
  } else {
    heap_region_iterate(blk);
  }
}

class ShenandoahSemeruClearLivenessClosure : public ShenandoahSemeruHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahSemeruClearLivenessClosure() : _ctx(ShenandoahSemeruHeap::heap()->marking_context()) {}

  void heap_region_do(ShenandoahSemeruHeapRegion* r) {
    r->clear_live_data();
    _ctx->semeru_capture_top_at_mark_start(r);
    // if (r->is_active()) {
      
    // } else {
    //   assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->region_number());
    //   assert(_ctx->semeru_top_at_mark_start(r) == r->top(),
    //          "Region " SIZE_FORMAT " should already have correct TAMS", r->region_number());
    // }
    r->set_state(r->sync_between_mem_and_cpu()->_state);
    // if(r->is_active()) {
    //   r->acquire_offset_table();
    // }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahSemeruHeap::op_init_mark() {
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  // assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");

  assert(marking_context()->is_bitmap_clear(), "need clear marking bitmap");
  assert(!marking_context()->is_complete(), "should not be complete");

  if (ShenandoahVerify) {
    verifier()->verify_before_concmark();
  }

  if (VerifyBeforeGC) {
    Universe::verify();
  }

  // set_concurrent_mark_in_progress(true);
  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::make_parsable);

  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::clear_liveness);
    ShenandoahSemeruClearLivenessClosure clc;
    parallel_heap_region_iterate(&clc);
  }

  // Make above changes visible to worker threads
  OrderAccess::fence();

  concurrent_mark()->mark_roots(ShenandoahPhaseTimings::scan_roots);

}

void ShenandoahSemeruHeap::op_mark() {
  concurrent_mark()->mark_from_roots();
  // ShenandoahSemeruCalcOffsetTask task(this);
  // workers()->run_task(&task);
}

class ShenandoahSemeruCompleteLivenessClosure : public ShenandoahSemeruHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahSemeruCompleteLivenessClosure() : _ctx(ShenandoahSemeruHeap::heap()->complete_marking_context()) {}

  void heap_region_do(ShenandoahSemeruHeapRegion* r) {
    if (r->is_active()) {
      HeapWord *tams = _ctx->semeru_top_at_mark_start(r);
      HeapWord *top = r->top();
      if (top > tams) {
        r->increase_live_data_alloc_words(pointer_delta(top, tams));
      }
    } else {
      assert(!r->has_live(), "Region " SIZE_FORMAT " should have no live data", r->region_number());
      assert(_ctx->semeru_top_at_mark_start(r) == r->top(),
             "Region " SIZE_FORMAT " should have correct TAMS", r->region_number());
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahSemeruHeap::op_final_mark() {
  // Modified by Haoran for compaction
  ShouldNotReachHere();

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.

  if (!cancelled_gc()) {
    concurrent_mark()->finish_mark_from_roots(/* full_gc = */ false);

    if (has_forwarded_objects()) {
      concurrent_mark()->update_roots(ShenandoahPhaseTimings::update_roots);
    }

    stop_concurrent_marking();

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::complete_liveness);

      // All allocations past TAMS are implicitly live, adjust the region data.
      // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
      ShenandoahSemeruCompleteLivenessClosure cl;
      parallel_heap_region_iterate(&cl);
    }

    {
      ShenandoahGCPhase prepare_evac(ShenandoahPhaseTimings::prepare_evac);

      make_parsable(true);


      // Modified by Haoran for evacuation
      assert(collection_set()->is_empty(), "Invariant here, cset should be set to empty at final update refs");
      collection_set()->clear();
      // trash_cset_regions();

      {
        ShenandoahHeapLocker locker(lock());
        _collection_set->clear();
        _free_set->clear();

        // Modified by Haoran for sync
        // Haoran: TODO no need to do this
        // Haoran: for region with no live objects, will make them trash in this function.
        // heuristics()->choose_collection_set(_collection_set);

        _free_set->rebuild();
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

      evacuate_and_update_roots();

      if (ShenandoahPacing) {
        pacer()->setup_for_evac();
      }

      if (ShenandoahVerify) {
        verifier()->verify_during_evacuation();
      }
    } else {
      if (ShenandoahVerify) {
        verifier()->verify_after_concmark();
      }

      if (VerifyAfterGC) {
        Universe::verify();
      }
    }

  } else {
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

void ShenandoahSemeruHeap::op_final_evac() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  set_evacuation_in_progress(false);

  retire_and_reset_gclabs();

  if (ShenandoahVerify) {
    verifier()->verify_after_evacuation();
  }

  if (VerifyAfterGC) {
    Universe::verify();
  }
}

void ShenandoahSemeruHeap::op_conc_evac() {
  // ShenandoahSemeruUpdateTask update_task(this, _collection_set);
  // workers()->run_task(&update_task);
  // _collection_set->clear_current_index();
  ShenandoahSemeruEvacuationTask task(this, _collection_set, true);
  workers()->run_task(&task);
}

void ShenandoahSemeruHeap::op_stw_evac() {
  ShenandoahSemeruEvacuationTask task(this, _collection_set, false);
  workers()->run_task(&task);
}

void ShenandoahSemeruHeap::op_updaterefs() {
  update_heap_references(true);
}

void ShenandoahSemeruHeap::op_cleanup() {
  free_set()->recycle_trash();
}

void ShenandoahSemeruHeap::op_reset() {
  reset_mark_bitmap();
}

void ShenandoahSemeruHeap::op_preclean() {
  concurrent_mark()->preclean_weak_refs();
}

void ShenandoahSemeruHeap::op_init_traversal() {
  traversal_gc()->init_traversal_collection();
}

void ShenandoahSemeruHeap::op_traversal() {
  traversal_gc()->concurrent_traversal_collection();
}

void ShenandoahSemeruHeap::op_final_traversal() {
  traversal_gc()->final_traversal_collection();
}

void ShenandoahSemeruHeap::op_full(GCCause::Cause cause) {
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

void ShenandoahSemeruHeap::op_degenerated(ShenandoahDegenPoint point) {
  ShouldNotCallThis();
  // Degenerated GC is STW, but it can also fail. Current mechanics communicates
  // GC failure via cancelled_concgc() flag. So, if we detect the failure after
  // some phase, we have to upgrade the Degenerate GC to Full GC.

  // clear_cancelled_gc();

  // ShenandoahMetricsSnapshot metrics;
  // metrics.snap_before();

  // switch (point) {
  //   case _degenerated_traversal:
  //     {
  //       // Drop the collection set. Note: this leaves some already forwarded objects
  //       // behind, which may be problematic, see comments for ShenandoahEvacAssist
  //       // workarounds in ShenandoahTraversalHeuristics.

  //       ShenandoahHeapLocker locker(lock());
  //       collection_set()->clear_current_index();
  //       for (size_t i = 0; i < collection_set()->count(); i++) {
  //         ShenandoahSemeruHeapRegion* r = collection_set()->next();
  //         r->make_regular_bypass();
  //       }
  //       collection_set()->clear();
  //     }
  //     op_final_traversal();
  //     op_cleanup();
  //     return;

  //   // The cases below form the Duff's-like device: it describes the actual GC cycle,
  //   // but enters it at different points, depending on which concurrent phase had
  //   // degenerated.

  //   case _degenerated_outside_cycle:
  //     // We have degenerated from outside the cycle, which means something is bad with
  //     // the heap, most probably heavy humongous fragmentation, or we are very low on free
  //     // space. It makes little sense to wait for Full GC to reclaim as much as it can, when
  //     // we can do the most aggressive degen cycle, which includes processing references and
  //     // class unloading, unless those features are explicitly disabled.
  //     //
  //     // Note that we can only do this for "outside-cycle" degens, otherwise we would risk
  //     // changing the cycle parameters mid-cycle during concurrent -> degenerated handover.
  //     set_process_references(heuristics()->can_process_references());
  //     set_unload_classes(heuristics()->can_unload_classes());

  //     if (heuristics()->can_do_traversal_gc()) {
  //       // Not possible to degenerate from here, upgrade to Full GC right away.
  //       cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
  //       op_degenerated_fail();
  //       return;
  //     }

  //     op_reset();

  //     op_init_mark();
  //     if (cancelled_gc()) {
  //       op_degenerated_fail();
  //       return;
  //     }

  //   case _degenerated_mark:
  //     op_final_mark();
  //     if (cancelled_gc()) {
  //       op_degenerated_fail();
  //       return;
  //     }

  //     op_cleanup();

  //   case _degenerated_evac:
  //     // If heuristics thinks we should do the cycle, this flag would be set,
  //     // and we can do evacuation. Otherwise, it would be the shortcut cycle.
  //     if (is_evacuation_in_progress()) {

  //       // Degeneration under oom-evac protocol might have left some objects in
  //       // collection set un-evacuated. Restart evacuation from the beginning to
  //       // capture all objects. For all the objects that are already evacuated,
  //       // it would be a simple check, which is supposed to be fast. This is also
  //       // safe to do even without degeneration, as CSet iterator is at beginning
  //       // in preparation for evacuation anyway.
  //       collection_set()->clear_current_index();

  //       op_stw_evac();
  //       if (cancelled_gc()) {
  //         op_degenerated_fail();
  //         return;
  //       }
  //     }

  //     // If heuristics thinks we should do the cycle, this flag would be set,
  //     // and we need to do update-refs. Otherwise, it would be the shortcut cycle.
  //     if (has_forwarded_objects()) {
  //       op_init_updaterefs();
  //       if (cancelled_gc()) {
  //         op_degenerated_fail();
  //         return;
  //       }
  //     }

  //   case _degenerated_updaterefs:
  //     if (has_forwarded_objects()) {
  //       op_final_updaterefs();
  //       if (cancelled_gc()) {
  //         op_degenerated_fail();
  //         return;
  //       }
  //     }

  //     op_cleanup();
  //     break;

  //   default:
  //     ShouldNotReachHere();
  // }

  // if (ShenandoahVerify) {
  //   verifier()->verify_after_degenerated();
  // }

  // if (VerifyAfterGC) {
  //   Universe::verify();
  // }

  // metrics.snap_after();
  // metrics.print();

  // // Check for futility and fail. There is no reason to do several back-to-back Degenerated cycles,
  // // because that probably means the heap is overloaded and/or fragmented.
  // if (!metrics.is_good_progress("Degenerated GC")) {
  //   _progress_last_gc.unset();
  //   cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
  //   op_degenerated_futile();
  // } else {
  //   _progress_last_gc.set();
  // }
}

void ShenandoahSemeruHeap::op_degenerated_fail() {
  log_info(gc)("Cannot finish degeneration, upgrading to Full GC");
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahSemeruHeap::op_degenerated_futile() {
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahSemeruHeap::stop_concurrent_marking() {
  assert(is_concurrent_mark_in_progress(), "How else could we get here?");
  if (!cancelled_gc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_has_forwarded_objects(false);
    mark_complete_marking_context();
  }
  set_concurrent_mark_in_progress(false);
}

void ShenandoahSemeruHeap::force_satb_flush_all_threads() {
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

size_t ShenandoahSemeruHeap::get_server_id(HeapWord* addr) {
  size_t addr_num = (size_t)addr;
  size_t per_bytes = _shenandoah_policy->max_heap_byte_size() / NUM_OF_MEMORY_SERVER;
  return (addr_num - (size_t)(_heap_region.start()))/per_bytes;
}

void ShenandoahSemeruHeap::set_gc_state_all_threads(char state) {
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread *t = jtiwh.next(); ) {
    ShenandoahThreadLocalData::set_gc_state(t, state);
  }
}

void ShenandoahSemeruHeap::set_gc_state_mask(uint mask, bool value) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should really be Shenandoah safepoint");
  _gc_state.set_cond(mask, value);
  set_gc_state_all_threads(_gc_state.raw_value());
}

void ShenandoahSemeruHeap::set_concurrent_mark_in_progress(bool in_progress) {
  set_gc_state_mask(MARKING, in_progress);
  ShenandoahBarrierSet::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahSemeruHeap::set_concurrent_traversal_in_progress(bool in_progress) {
   set_gc_state_mask(TRAVERSAL | HAS_FORWARDED, in_progress);
   ShenandoahBarrierSet::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahSemeruHeap::set_evacuation_in_progress(bool in_progress) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only call this at safepoint");
  set_gc_state_mask(EVACUATION, in_progress);
}



void ShenandoahSemeruHeap::ref_processing_init() {
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

GCTracer* ShenandoahSemeruHeap::tracer() {
  return shenandoah_policy()->tracer();
}

size_t ShenandoahSemeruHeap::tlab_used(Thread* thread) const {
  return _free_set->used();
}

void ShenandoahSemeruHeap::cancel_gc(GCCause::Cause cause) {
  if (try_cancel_gc()) {
    FormatBuffer<> msg("Cancelling GC: %s", GCCause::to_string(cause));
    log_info(gc)("%s", msg.buffer());
    Events::log(Thread::current(), "%s", msg.buffer());
  }
}

uint ShenandoahSemeruHeap::max_workers() {
  return _max_workers;
}

void ShenandoahSemeruHeap::stop() {
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

void ShenandoahSemeruHeap::unload_classes_and_cleanup_tables(bool full_gc) {
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
    
    // Modified by Haoran2
    // StringDedupUnlinkOrOopsDoClosure dedup_cl(is_alive, NULL);
    // ParallelCleaningTask unlink_task(is_alive, &dedup_cl, active, purged_class);
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

void ShenandoahSemeruHeap::set_has_forwarded_objects(bool cond) {
  set_gc_state_mask(HAS_FORWARDED, cond);
}

void ShenandoahSemeruHeap::set_process_references(bool pr) {
  _process_references.set_cond(pr);
}

void ShenandoahSemeruHeap::set_unload_classes(bool uc) {
  _unload_classes.set_cond(uc);
}

bool ShenandoahSemeruHeap::process_references() const {
  return _process_references.is_set();
}

bool ShenandoahSemeruHeap::unload_classes() const {
  return _unload_classes.is_set();
}

address ShenandoahSemeruHeap::in_cset_fast_test_addr() {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  assert(heap->collection_set() != NULL, "Sanity");
  return (address) heap->collection_set()->biased_map_address();
}

address ShenandoahSemeruHeap::cancelled_gc_addr() {
  return (address) ShenandoahSemeruHeap::heap()->_cancelled_gc.addr_of();
}

address ShenandoahSemeruHeap::gc_state_addr() {
  return (address) ShenandoahSemeruHeap::heap()->_gc_state.addr_of();
}

size_t ShenandoahSemeruHeap::bytes_allocated_since_gc_start() {
  return OrderAccess::load_acquire(&_bytes_allocated_since_gc_start);
}

void ShenandoahSemeruHeap::reset_bytes_allocated_since_gc_start() {
  OrderAccess::release_store_fence(&_bytes_allocated_since_gc_start, (size_t)0);
}

void ShenandoahSemeruHeap::set_degenerated_gc_in_progress(bool in_progress) {
  _degenerated_gc_in_progress.set_cond(in_progress);
}

void ShenandoahSemeruHeap::set_full_gc_in_progress(bool in_progress) {
  _full_gc_in_progress.set_cond(in_progress);
}

void ShenandoahSemeruHeap::set_full_gc_move_in_progress(bool in_progress) {
  assert (is_full_gc_in_progress(), "should be");
  _full_gc_move_in_progress.set_cond(in_progress);
}

void ShenandoahSemeruHeap::set_update_refs_in_progress(bool in_progress) {
  set_gc_state_mask(UPDATEREFS, in_progress);
}

void ShenandoahSemeruHeap::register_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::add_nmethod(nm);
}

void ShenandoahSemeruHeap::unregister_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::remove_nmethod(nm);
}

oop ShenandoahSemeruHeap::pin_object(JavaThread* thr, oop o) {
  // Modified by Haoran2
  // o = ShenandoahBarrierSet::barrier_set()->write_barrier(o);
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_pinned();
  return o;
}

void ShenandoahSemeruHeap::unpin_object(JavaThread* thr, oop o) {
  // Modified by Haoran2
  // o = ShenandoahBarrierSet::barrier_set()->read_barrier(o);
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_unpinned();
}

GCTimer* ShenandoahSemeruHeap::gc_timer() const {
  return _gc_timer;
}

#ifdef ASSERT
void ShenandoahSemeruHeap::assert_gc_workers(uint nworkers) {
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

ShenandoahVerifier* ShenandoahSemeruHeap::verifier() {
  guarantee(ShenandoahVerify, "Should be enabled");
  assert (_verifier != NULL, "sanity");
  return _verifier;
}

// template<class T>
// class ShenandoahSemeruUpdateHeapRefsTask : public AbstractGangTask {
// private:
//   T cl;
//   ShenandoahSemeruHeap* _heap;
//   ShenandoahSemeruRegionIterator* _regions;
//   bool _concurrent;
// public:
//   ShenandoahSemeruUpdateHeapRefsTask(ShenandoahSemeruRegionIterator* regions, bool concurrent) :
//     AbstractGangTask("Concurrent Update References Task"),
//     cl(T()),
//     _heap(ShenandoahSemeruHeap::heap()),
//     _regions(regions),
//     _concurrent(concurrent) {
//   }

//   void work(uint worker_id) {
//     if (_concurrent) {
//       ShenandoahConcurrentWorkerSession worker_session(worker_id);
//       ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
//       do_work();
//     } else {
//       ShenandoahParallelWorkerSession worker_session(worker_id);
//       do_work();
//     }
//   }

// private:
//   void do_work() {
//     ShenandoahSemeruHeapRegion* r = _regions->next();
//     ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
//     while (r != NULL) {
//       HeapWord* top_at_start_ur = r->concurrent_iteration_safe_limit();
//       assert (top_at_start_ur >= r->bottom(), "sanity");
//       if (r->is_active() && !r->is_cset()) {
//         _heap->marked_object_oop_iterate(r, &cl, top_at_start_ur);
//       }
//       if (ShenandoahPacing) {
//         _heap->pacer()->report_updaterefs(pointer_delta(top_at_start_ur, r->bottom()));
//       }
//       if (_heap->check_cancelled_gc_and_yield(_concurrent)) {
//         return;
//       }
//       r = _regions->next();
//     }
//   }
// };

void ShenandoahSemeruHeap::update_heap_references(bool concurrent) {
  ShouldNotCallThis();
  // ShenandoahSemeruUpdateHeapRefsTask<ShenandoahUpdateHeapRefsClosure> task(&_update_refs_iterator, concurrent);
  // workers()->run_task(&task);
}

void ShenandoahSemeruHeap::op_init_updaterefs() {
  ShouldNotCallThis();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  // set_evacuation_in_progress(false);

  // retire_and_reset_gclabs();

  // if (ShenandoahVerify) {
  //   verifier()->verify_before_updaterefs();
  // }

  // set_update_refs_in_progress(true);
  // make_parsable(true);
  // for (uint i = 0; i < num_regions(); i++) {
  //   ShenandoahSemeruHeapRegion* r = get_region(i);
  //   r->set_concurrent_iteration_safe_limit(r->top());
  // }

  // // Reset iterator.
  // _update_refs_iterator.reset();

  // if (ShenandoahPacing) {
  //   pacer()->setup_for_updaterefs();
  // }
}

void ShenandoahSemeruHeap::op_final_updaterefs() {
  // Modified by Haoran
  // Haoran: debug
  ShouldNotCallThis();


  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  // // Modified by Haoran
  // // Moved from the init_update_refs
  // set_evacuation_in_progress(false);

  // retire_and_reset_gclabs();



  // // Check if there is left-over work, and finish it
  // if (_update_refs_iterator.has_next()) {
  //   ShenandoahGCPhase final_work(ShenandoahPhaseTimings::final_update_refs_finish_work);

  //   // Finish updating references where we left off.
  //   clear_cancelled_gc();
  //   update_heap_references(false);
  // }

  // // Clear cancelled GC, if set. On cancellation path, the block before would handle
  // // everything. On degenerated paths, cancelled gc would not be set anyway.
  // if (cancelled_gc()) {
  //   clear_cancelled_gc();
  // }
  // assert(!cancelled_gc(), "Should have been done right before");

  // concurrent_mark()->update_roots(is_degenerated_gc_in_progress() ?
  //                                ShenandoahPhaseTimings::degen_gc_update_roots:
  //                                ShenandoahPhaseTimings::final_update_refs_roots);


  // //Universe::indirection_table()->reclaim_dead_entries();
  // reclaim_entries();

  // ShenandoahGCPhase final_update_refs(ShenandoahPhaseTimings::final_update_refs_recycle);

  // trash_cset_regions();
  // set_has_forwarded_objects(false);
  // set_update_refs_in_progress(false);

  // if (ShenandoahVerify) {
  //   verifier()->verify_after_updaterefs();
  // }

  // if (VerifyAfterGC) {
  //   Universe::verify();
  // }

  // {
  //   ShenandoahHeapLocker locker(lock());
  //   _free_set->rebuild();
  // }
}

#ifdef ASSERT
void ShenandoahSemeruHeap::assert_heaplock_owned_by_current_thread() {
  _lock.assert_owned_by_current_thread();
}

void ShenandoahSemeruHeap::assert_heaplock_not_owned_by_current_thread() {
  _lock.assert_not_owned_by_current_thread();
}

void ShenandoahSemeruHeap::assert_heaplock_or_safepoint() {
  _lock.assert_owned_by_current_thread_or_safepoint();
}
#endif

void ShenandoahSemeruHeap::print_extended_on(outputStream *st) const {
  print_on(st);
  print_heap_regions_on(st);
}

bool ShenandoahSemeruHeap::is_bitmap_slice_committed(ShenandoahSemeruHeapRegion* r, bool skip_self) {
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

bool ShenandoahSemeruHeap::commit_bitmap_slice(ShenandoahSemeruHeapRegion* r) {
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

bool ShenandoahSemeruHeap::uncommit_bitmap_slice(ShenandoahSemeruHeapRegion *r) {
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

void ShenandoahSemeruHeap::safepoint_synchronize_begin() {
  if (ShenandoahSuspendibleWorkers || UseStringDeduplication) {
    SuspendibleThreadSet::synchronize();
  }
}

void ShenandoahSemeruHeap::safepoint_synchronize_end() {
  if (ShenandoahSuspendibleWorkers || UseStringDeduplication) {
    SuspendibleThreadSet::desynchronize();
  }
}

void ShenandoahSemeruHeap::vmop_entry_init_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitMark op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahSemeruHeap::vmop_entry_final_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalMarkStartEvac op;
  VMThread::execute(&op); // jump to entry_final_mark under safepoint
}

void ShenandoahSemeruHeap::vmop_entry_final_evac() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac_gross);

  VM_ShenandoahFinalEvac op;
  VMThread::execute(&op); // jump to entry_final_evac under safepoint
}

void ShenandoahSemeruHeap::vmop_entry_init_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahSemeruHeap::vmop_entry_final_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahSemeruHeap::vmop_entry_init_traversal() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_traversal_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitTraversalGC op;
  VMThread::execute(&op);
}

void ShenandoahSemeruHeap::vmop_entry_final_traversal() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_traversal_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalTraversalGC op;
  VMThread::execute(&op);
}

void ShenandoahSemeruHeap::vmop_entry_full(GCCause::Cause cause) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFullGC op(cause);
  VMThread::execute(&op);
}

void ShenandoahSemeruHeap::vmop_degenerated(ShenandoahDegenPoint point) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc_gross);

  VM_ShenandoahDegeneratedGC degenerated_gc((int)point);
  VMThread::execute(&degenerated_gc);
}

void ShenandoahSemeruHeap::entry_init_mark() {
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

void ShenandoahSemeruHeap::entry_final_mark() {
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

void ShenandoahSemeruHeap::entry_final_evac() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac);
  static const char* msg = "Pause Final Evac";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  op_final_evac();
}

void ShenandoahSemeruHeap::entry_init_updaterefs() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs);

  static const char* msg = "Pause Init Update Refs";
  GCTraceTime(Info, gc) time(msg, gc_timer());
  EventMark em("%s", msg);

  // No workers used in this phase, no setup required

  op_init_updaterefs();
}

void ShenandoahSemeruHeap::entry_final_updaterefs() {
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

void ShenandoahSemeruHeap::entry_init_traversal() {
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

void ShenandoahSemeruHeap::entry_final_traversal() {
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

void ShenandoahSemeruHeap::entry_full(GCCause::Cause cause) {
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

void ShenandoahSemeruHeap::entry_degenerated(int point) {
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

void ShenandoahSemeruHeap::entry_mark() {
  // TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  const char* msg = conc_mark_event_message();
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent marking");

  try_inject_alloc_failure();
  op_mark();
}

void ShenandoahSemeruHeap::entry_evac() {
  ShenandoahGCPhase conc_evac_phase(ShenandoahPhaseTimings::conc_evac);
  // TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  static const char* msg = "Concurrent evacuation";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_evac(),
                              "concurrent evacuation");

  try_inject_alloc_failure();
  op_conc_evac();
}

void ShenandoahSemeruHeap::entry_updaterefs() {
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
void ShenandoahSemeruHeap::entry_cleanup() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  // This phase does not use workers, no need for setup

  try_inject_alloc_failure();
  op_cleanup();
}

void ShenandoahSemeruHeap::entry_reset() {
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

void ShenandoahSemeruHeap::entry_preclean() {
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

void ShenandoahSemeruHeap::entry_traversal() {
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

void ShenandoahSemeruHeap::entry_uncommit(double shrink_before) {
  static const char *msg = "Concurrent uncommit";
  GCTraceTime(Info, gc) time(msg, NULL, GCCause::_no_gc, true);
  EventMark em("%s", msg);

  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_uncommit);

  op_uncommit(shrink_before);
}

void ShenandoahSemeruHeap::try_inject_alloc_failure() {
  if (ShenandoahAllocFailureALot && !cancelled_gc() && ((os::random() % 1000) > 950)) {
    _inject_alloc_failure.set();
    os::naked_short_sleep(1);
    if (cancelled_gc()) {
      log_info(gc)("Allocation failure was successfully injected");
    }
  }
}

bool ShenandoahSemeruHeap::should_inject_alloc_failure() {
  return _inject_alloc_failure.is_set() && _inject_alloc_failure.try_unset();
}

void ShenandoahSemeruHeap::initialize_serviceability() {
  _memory_pool = new ShenandoahSemeruMemoryPool(this);
  _cycle_memory_manager.add_pool(_memory_pool);
  _stw_memory_manager.add_pool(_memory_pool);
}

GrowableArray<GCMemoryManager*> ShenandoahSemeruHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(2);
  memory_managers.append(&_cycle_memory_manager);
  memory_managers.append(&_stw_memory_manager);
  return memory_managers;
}

GrowableArray<MemoryPool*> ShenandoahSemeruHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(1);
  memory_pools.append(_memory_pool);
  return memory_pools;
}

MemoryUsage ShenandoahSemeruHeap::memory_usage() {
  return _memory_pool->get_memory_usage();
}

void ShenandoahSemeruHeap::enter_evacuation() {
  _oom_evac_handler.enter_evacuation();
}

void ShenandoahSemeruHeap::leave_evacuation() {
  _oom_evac_handler.leave_evacuation();
}

ShenandoahSemeruRegionIterator::ShenandoahSemeruRegionIterator() :
  _heap(ShenandoahSemeruHeap::heap()),
  _index(0) {}

ShenandoahSemeruRegionIterator::ShenandoahSemeruRegionIterator(ShenandoahSemeruHeap* heap) :
  _heap(heap),
  _index(0) {}

void ShenandoahSemeruRegionIterator::reset() {
  _index = 0;
}

bool ShenandoahSemeruRegionIterator::has_next() const {
  return _index < _heap->num_regions();
}

char ShenandoahSemeruHeap::gc_state() const {
  return _gc_state.raw_value();
}

void ShenandoahSemeruHeap::deduplicate_string(oop str) {
  assert(java_lang_String::is_instance(str), "invariant");

  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::deduplicate(str);
  }
}

const char* ShenandoahSemeruHeap::init_mark_event_message() const {
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

const char* ShenandoahSemeruHeap::final_mark_event_message() const {
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

const char* ShenandoahSemeruHeap::conc_mark_event_message() const {
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

const char* ShenandoahSemeruHeap::degen_event_message(ShenandoahDegenPoint point) const {
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

jushort* ShenandoahSemeruHeap::get_liveness_cache(uint worker_id) {
#ifdef ASSERT
  assert(_liveness_cache != NULL, "sanity");
  assert(worker_id < _max_workers, "sanity");
  for (uint i = 0; i < num_regions(); i++) {
    assert(_liveness_cache[worker_id][i] == 0, "liveness cache should be empty");
  }
#endif
  return _liveness_cache[worker_id];
}

void ShenandoahSemeruHeap::flush_liveness_cache(uint worker_id) {
  assert(worker_id < _max_workers, "sanity");
  assert(_liveness_cache != NULL, "sanity");
  jushort* ld = _liveness_cache[worker_id];
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahSemeruHeapRegion* r = get_region(i);
    jushort live = ld[i];
    if (live > 0) {
      r->increase_live_data_gc_words(live);
      ld[i] = 0;
    }
  }
}

