/*
 * Copyright (c) 2016, 2018, Red Hat, Inc. All rights reserved.
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

#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "runtime/atomic.hpp"
#include "services/memTracker.hpp"
#include "utilities/copy.hpp"

#include "utilities/quickSort.hpp"

// Modified by Haoran for sync
ShenandoahCollectionSet::ShenandoahCollectionSet(ShenandoahHeap* heap, char* heap_base, size_t size, char* sync_map_base, char* compacted_map_base) :
  _map_size(heap->num_regions()),
  _region_size_bytes_shift(ShenandoahHeapRegion::region_size_bytes_shift()),
  _map_space(align_up(((uintx)heap_base + size) >> _region_size_bytes_shift, os::vm_allocation_granularity())),
  _cset_map(_map_space.base() + ((uintx)heap_base >> _region_size_bytes_shift)),
  _biased_cset_map(_map_space.base()),
  _region_data(NULL),
  _sync_map(sync_map_base), //Modified by Haoran for sync
  _compacted_map(compacted_map_base), //Modified by Haoran for sync
  _heap(heap),
  _garbage(0),
  _live_data(0),
  _used(0),
  _region_count(0),
  _local_pages(0),
  _current_index(0) {

  // The collection set map is reserved to cover the entire heap *and* zero addresses.
  // This is needed to accept in-cset checks for both heap oops and NULLs, freeing
  // high-performance code from checking for NULL first.
  //
  // Since heap_base can be far away, committing the entire map would waste memory.
  // Therefore, we only commit the parts that are needed to operate: the heap view,
  // and the zero page.
  //
  // Note: we could instead commit the entire map, and piggyback on OS virtual memory
  // subsystem for mapping not-yet-written-to pages to a single physical backing page,
  // but this is not guaranteed, and would confuse NMT and other memory accounting tools.

  assert(_map_size <= CSET_SYNC_MAP_SIZE, "Exceed allocated space for CSET_SYNC_MAP!");

  MemTracker::record_virtual_memory_type(_map_space.base(), mtGC);

  size_t page_size = (size_t)os::vm_page_size();

  if (!_map_space.special()) {
    // Commit entire pages that cover the heap cset map.
    char* bot_addr = align_down(_cset_map, page_size);
    char* top_addr = align_up(_cset_map + _map_size, page_size);
    os::commit_memory_or_exit(bot_addr, pointer_delta(top_addr, bot_addr, 1), false,
                              "Unable to commit collection set bitmap: heap");

    // Commit the zero page, if not yet covered by heap cset map.
    if (bot_addr != _biased_cset_map) {
      os::commit_memory_or_exit(_biased_cset_map, page_size, false,
                                "Unable to commit collection set bitmap: zero page");
    }
  }

  _region_data = NEW_C_HEAP_ARRAY(RegionCacheData, _heap->num_regions(), mtGC);


  Copy::zero_to_bytes(_cset_map, _map_size);
  Copy::zero_to_bytes(_biased_cset_map, page_size);

  // Modified by Haoran for sync
  Copy::zero_to_bytes(_sync_map, _map_size);
}

void ShenandoahCollectionSet::add_region(ShenandoahHeapRegion* r) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  assert(!is_in_update_set(r), "Already in collection set");
#ifdef RELEASE_CHECK
  tty->print("Add region %lu to cset\n", r->region_number());
#endif
  // _cset_map[r->region_number()] = 1;
  _cset_map[r->region_number()] = 3;
  // _cset_map[_heap->get_corr_region(r->region_number())->region_number()] = 1;
  _region_count ++;
  _garbage += r->garbage();
  _live_data += r->get_live_data_bytes();
  _used += r->used();
}

void ShenandoahCollectionSet::add_region_to_update(ShenandoahHeapRegion* r) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  assert(!is_in_update_set(r), "Already in collection set");
  // _cset_map[r->region_number()] = 1;
  _cset_map[r->region_number()] = 1;
}

bool ShenandoahCollectionSet::add_region_check_for_duplicates(ShenandoahHeapRegion* r) {
  if (!is_in_update_set(r)) {
    add_region(r);
    return true;
  } else {
    return false;
  }
}

void ShenandoahCollectionSet::remove_region(ShenandoahHeapRegion* r) {
  ShouldNotCallThis();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  // assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  // assert(is_in(r), "Not in collection set");
  // _cset_map[r->region_number()] = 0;
  // _region_count --;
}

void ShenandoahCollectionSet::update_region_status() {
  for (size_t index = 0; index < _heap->num_regions(); index ++) {
    ShenandoahHeapRegion* r = _heap->get_region(index);
    if (is_in_evac_set(r)) {
      r->make_cset();
    } else {
      assert (!r->is_cset(), "should not be cset");
    }
  }
}

void ShenandoahCollectionSet::clear() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  Copy::zero_to_bytes(_cset_map, _map_size);

  // Modified by Haoran for remote evcuation
  Copy::zero_to_bytes(_compacted_map, _map_size);

#ifdef ASSERT
  for (size_t index = 0; index < _heap->num_regions(); index ++) {
    assert (!_heap->get_region(index)->is_cset(), "should have been cleared before");
  }
#endif

  _garbage = 0;
  _live_data = 0;
  _used = 0;

  _region_count = 0;
  _current_index = 0;
}

// Haoran: used on memory server to get the info from cpu server
void ShenandoahCollectionSet::copy_sync_to_cset() {
  ShouldNotReachHere();
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  clear();


  size_t num_regions = _heap->num_regions();
  size_t index = (size_t)0;

  while(index < num_regions) {
    if (_sync_map[index]) {
      add_region(_heap->get_region(index));
    }
  }

  Copy::zero_to_bytes(_sync_map, _map_size);
}

// Haoran: used on cpu server before send sync cset
void ShenandoahCollectionSet::copy_cset_to_sync() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  Copy::conjoint_jbytes(_cset_map, _sync_map, _map_size);
}

int ShenandoahCollectionSet::compare_by_cache(RegionCacheData a, RegionCacheData b) {
  if (a._cache_ratio > b._cache_ratio)
    return -1;
  else if (a._cache_ratio < b._cache_ratio)
    return 1;
  else if (a._cache_pages > b._cache_pages)
    return -1;
  else if (a._cache_pages < b._cache_pages)
    return 1;
  return 0;
}

// void ShenandoahCollectionSet::select_local_process_regions() {
//   assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  
//   size_t num_regions = _heap->num_regions();
//   size_t cand_idx = 0;

//   for (size_t i = 0; i < num_regions; i++) {
//     if(!is_in_update_set(i)) continue;
//     ShenandoahHeapRegion* region = _heap->get_region(i);
//     _cset_map[i] |= 4;
//   }
// }


// Haoran: decide which regions are processed by the CPU server and which regions are processed by the memory server
bool ShenandoahCollectionSet::select_local_process_regions() {

  if(_heap->gc_start_threshold > _heap->max_capacity() * (ShenandoahInitFreeThreshold - 1) / 100) {
    size_t num_regions = _heap->num_regions();
    for (size_t i = 0; i < num_regions; i++) {
      if(!is_in_update_set(i)) continue;
      ShenandoahHeapRegion* region = _heap->get_region(i);
      _cset_map[i] |= 4;
    }
    return false;
  }
  return true;

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  size_t num_regions = _heap->num_regions();
  size_t cand_idx = 0;

  ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();

  for (size_t i = 0; i < num_regions; i++) {
    if(!is_in_update_set(i)) continue;
    if(is_in_local_update_set(i)) continue;
    ShenandoahHeapRegion* region = _heap->get_region(i);
    if (region->is_humongous_start()) {
      oop obj = (oop)(region->bottom());
      assert(_heap->marking_context()->is_marked(obj), "Must be alive!");
      _region_data[cand_idx]._region = region;
      size_t obj_size_in_bytes = (obj->size()) * HeapWordSize;
      size_t num_regions = ShenandoahHeapRegion::required_regions(obj_size_in_bytes);
      _region_data[cand_idx]._cache_pages = (uint)(_heap->cache_size_in_bytes(region->bottom(), num_regions * ShenandoahHeapRegion::region_size_bytes())/4096);
      _region_data[cand_idx]._total_pages = (uint)(num_regions * (ShenandoahHeapRegion::region_size_bytes()/4096));
      _region_data[cand_idx]._cache_ratio = _region_data[cand_idx]._cache_pages/_region_data[cand_idx]._total_pages;
      cand_idx ++;
    } else if(!region->is_humongous_continuation()) {
      assert(region->has_live(), "Otherwise should not be in update set!");
      _region_data[cand_idx]._region = region;
      _region_data[cand_idx]._cache_pages = (uint)(_heap->cache_size_in_bytes(i)/4096);
      _region_data[cand_idx]._total_pages = (uint)(ShenandoahHeapRegion::region_size_bytes()/4096);
      _region_data[cand_idx]._cache_ratio = _region_data[cand_idx]._cache_pages/_region_data[cand_idx]._total_pages;
      cand_idx ++;
    } else {
      assert(region->is_humongous_continuation(), "Test!");
    }
  }

  // Better select cache-first regions
  QuickSort::sort<RegionCacheData>(_region_data, (int)cand_idx, compare_by_cache, false);

  size_t capacity    = ShenandoahHeap::heap()->max_capacity();
  size_t target = capacity / 100 * ShenandoahLocalProcessingRatio / 4096;

  size_t cur_pages = _local_pages;

  for (size_t idx = 0; idx < cand_idx; idx++) {
    ShenandoahHeapRegion* r = _region_data[idx]._region;
    size_t region_index = r->region_number();


    size_t new_pages = cur_pages;
    if(is_in_evac_set(region_index))
      new_pages += _region_data[idx]._total_pages + align_up(r->get_live_data_bytes(), 4096)/4096;
    else
      new_pages += _region_data[idx]._total_pages;

    if(new_pages > target){
      if(new_pages - target < target - cur_pages) {
        _sync_map[region_index] |= 4;
        _cset_map[region_index] |= 4;
      }
      return true;
    }
    _sync_map[region_index] |= 4;
    _cset_map[region_index] |= 4;
    cur_pages = new_pages;
  }
  return true;
}

void ShenandoahCollectionSet::add_region_to_local_set(size_t region_index) {
  size_t i = region_index;
  assert(is_in_update_set(i) && !is_in_evac_set(i), "Invariant!");
#ifdef RELEASE_CHECK
  if(!is_in_update_set(i) || is_in_evac_set(i)) {
    ShouldNotReachHere();
  }
  if(_heap->get_region(i)->is_humongous_continuation()) {
    ShouldNotReachHere();
  }
#endif
  _cset_map[i] |= 4;

  ShenandoahHeapRegion* region = _heap->get_region(i);
  assert(!region->is_humongous_continuation(), "Invariant!");
  if(region->is_humongous_start()) {
    oop obj = (oop)(region->bottom());
    _local_pages += align_up(obj->size(), 4096) / 4096;
  }
  else {
    _local_pages += ShenandoahHeapRegion::region_size_bytes() / 4096;
  }
}

// Haoran: used on cpu server to get compacted data
// void ShenandoahCollectionSet::copy_compacted_to_cset() {
//   for(size_t i = 0; i < _map_size; i++) {
//     if(_compacted_map[i] && ((_cset_map[i] & 4) == 0)) {
//       Atomic::store(4, &_cset_map[i]);
//     }
//   }
// }



// ShenandoahHeapRegion* ShenandoahCollectionSet::claim_next() {
//   size_t num_regions = _heap->num_regions();
//   if (_current_index >= (jint)num_regions) {
//     return NULL;
//   }

//   jint saved_current = _current_index;
//   size_t index = (size_t)saved_current;

//   while(index < num_regions) {
//     if (is_in(index)) {
//       jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
//       assert(cur >= (jint)saved_current, "Must move forward");
//       if (cur == saved_current) {
//         assert(is_in(index), "Invariant");
//         return _heap->get_region(index);
//       } else {
//         index = (size_t)cur;
//         saved_current = cur;
//       }
//     } else {
//       index ++;
//     }
//   }
//   return NULL;
// }
ShenandoahHeapRegion* ShenandoahCollectionSet::claim_next() {
  size_t num_regions = _heap->num_regions();
  if (_current_index >= (jint)num_regions) {
    return NULL;
  }

  jint saved_current = _current_index;
  size_t index = (size_t)saved_current;

  while(index < num_regions) {
    if (is_in_local_update_set(index)) {
      jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
      assert(cur >= (jint)saved_current, "Must move forward");
      if (cur == saved_current) {
        assert(is_in_local_update_set(index), "Invariant");
        return _heap->get_region(index);
      } else {
        index = (size_t)cur;
        saved_current = cur;
      }
    } else {
      index ++;
    }
  }
  return NULL;
}

ShenandoahHeapRegion* ShenandoahCollectionSet::next() {
  ShouldNotCallThis();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  // assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  // size_t num_regions = _heap->num_regions();
  // for (size_t index = (size_t)_current_index; index < num_regions; index ++) {
  //   if (is_in(index)) {
  //     _current_index = (jint)(index + 1);
  //     return _heap->get_region(index);
  //   }
  // }

  return NULL;
}

ShenandoahHeapRegion* ShenandoahCollectionSet::next_evac_set() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  size_t num_regions = _heap->num_regions();
  for (size_t index = (size_t)_current_index; index < num_regions; index ++) {
    if (is_in_evac_set(index)) {
      _current_index = (jint)(index + 1);
      return _heap->get_region(index);
    }
  }
  return NULL;
}

void ShenandoahCollectionSet::print_on(outputStream* out) const {
  out->print_cr("Collection Set : " SIZE_FORMAT "", count());

  debug_only(size_t regions = 0;)
  for (size_t index = 0; index < _heap->num_regions(); index ++) {
    if (is_in_evac_set(index)) {
      _heap->get_region(index)->print_on(out);
      debug_only(regions ++;)
    }
  }
  assert(regions == count(), "Must match");
}




// Modified by Haoran for remote compaction
// Haoran: used on memory server to get a region to compact
// ShenandoahHeapRegion* ShenandoahCollectionSet::claim_next_for_compaction() {

//   MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);
//   if(_prioritized_queue_st < _prioritized_queue_ed){
//     ShenandoahHeapRegion* region = _heap->get_region(_prioritized_queue[_prioritized_queue_st]);
//     _prioritized_queue_st ++;
//     return region;
//   }
//   else {
//     size_t num_regions = _heap->num_regions();
//     if (_current_index >= (jint)num_regions) {
//       return NULL;
//     }

//     jint saved_current = _current_index;
//     size_t index = (size_t)saved_current;

//     while(index < num_regions) {
//       if (in_not_compacted_cset(index)) {
//         jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
//         assert(cur >= (jint)saved_current, "Must move forward");
//         if (cur == saved_current) {
//           assert(is_in(index), "Invariant");
//           return _heap->get_region(index);
//         } else {
//           index = (size_t)cur;
//           saved_current = cur;
//         }
//       } else {
//         index ++;
//       }
//     }
//     return NULL;

//   }
  
// }
// Modified by Haoran for remote compaction
ShenandoahHeapRegion* ShenandoahCollectionSet::claim_next_for_compaction() {
  ShouldNotCallThis();
  return NULL;
}



HeapWord* ShenandoahCollectionSet::alloc_space_and_update_start(ShenandoahHeapRegion* r) {
  size_t word_size = align_up(r->get_live_data_words(), 512);
  if(word_size - r->get_live_data_words() == 1){
    word_size += 512;
  }
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared_gc(word_size);
  // HeapWord* filler = _heap->allocate_memory(req);
  HeapWord* filler = _heap->allocate_memory_in_safepoint(req);
  if(filler == NULL) {
    return NULL;
  }
  assert(is_aligned(filler, 4096), "Must be aligned with pages");
#ifdef RELEASE_CHECK
  if(!is_aligned(filler, 4096)) {
    ShouldNotReachHere();
  }
#endif
  r->sync_between_mem_and_cpu()->_evac_start = filler;

  ShenandoahHeapRegion* new_r = _heap->heap_region_containing(filler);

  // tty->print("region_id: 0x%lx, sync_address: 0x%lx, evac_start 0x%lx, live_data_words: %lu, word_size: %lu", r->region_number(), (size_t)r->sync_between_mem_and_cpu(), (size_t)r->sync_between_mem_and_cpu()->_evac_start, r->get_live_data_words(), word_size);
  // if(r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words() < new_r->bottom() || r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words() > new_r->top() ||  r->sync_between_mem_and_cpu()->_evac_start + word_size > new_r->top()) {
  //   tty->print("region_id: 0x%lx, sync_address: 0x%lx, evac_start 0x%lx, live_data_words: %lu, word_size: %lu", r->region_number(), (size_t)r->sync_between_mem_and_cpu(), (size_t)r->sync_between_mem_and_cpu()->_evac_start, r->get_live_data_words(), word_size);
  //   ShouldNotReachHere();
  // }
  if(word_size > r->get_live_data_words()) {
    _heap->fill_with_dummy_object(r->sync_between_mem_and_cpu()->_evac_start + r->get_live_data_words(), r->sync_between_mem_and_cpu()->_evac_start + word_size, true);
#ifdef RELEASE_CHECK
    if(word_size - r->get_live_data_words() < 2) {
      ShouldNotReachHere();
    }
#endif
  }


  _heap->heap_region_containing(filler)->_selected_to = true;

  return filler;
}


bool ShenandoahCollectionSet::is_update_finished(size_t region_number) const {
  assert(region_number < _heap->num_regions(), "Sanity");
  return (Atomic::load(&_cset_map[region_number]) & 8);
}

void ShenandoahCollectionSet::set_update_finished(size_t region_number) {
  assert(region_number < _heap->num_regions(), "Sanity");
  char v = Atomic::load(&_cset_map[region_number]);
  Atomic::store((char)(v | 8), &_cset_map[region_number]);
}