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

#include "gc/shenandoah/shenandoahSemeruCollectionSet.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "runtime/atomic.hpp"
#include "services/memTracker.hpp"
#include "utilities/copy.hpp"

ShenandoahSemeruCollectionSet::ShenandoahSemeruCollectionSet(ShenandoahSemeruHeap* semeru_heap, char* semeru_heap_base, size_t size, char* sync_map_base, char* compacted_map_base) :
  _map_size(semeru_heap->num_regions()),
  _region_size_bytes_shift(ShenandoahSemeruHeapRegion::region_size_bytes_shift()),
  _map_space(align_up(((uintx)semeru_heap_base + size) >> _region_size_bytes_shift, os::vm_allocation_granularity())),
  _cset_map(_map_space.base() + ((uintx)semeru_heap_base >> _region_size_bytes_shift)),
  _biased_cset_map(_map_space.base()),
  _sync_map(sync_map_base),
  _compacted_map(compacted_map_base), //Modified by Haoran for sync
  _semeru_heap(semeru_heap),
  _garbage(0),
  _live_data(0),
  _used(0),
  _region_count(0),
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

  Copy::zero_to_bytes(_cset_map, _map_size);
  Copy::zero_to_bytes(_biased_cset_map, page_size);

  Copy::zero_to_bytes(_sync_map, _map_size);
}

void ShenandoahSemeruCollectionSet::add_region(ShenandoahSemeruHeapRegion* r) {

  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  // assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  assert(!is_in_update_set(r), "Already in collection set");
  _cset_map[r->region_number()] = 3;
  _region_count ++;
  _garbage += r->garbage();
  _live_data += r->get_live_data_bytes();
  _used += r->used();
}

bool ShenandoahSemeruCollectionSet::add_region_check_for_duplicates(ShenandoahSemeruHeapRegion* r) {
  ShouldNotReachHere();
  // if (!is_in(r)) {
  //   add_region(r);
  //   return true;
  // } else {
  //   return false;
  // }
  return false;
}

void ShenandoahSemeruCollectionSet::remove_region(ShenandoahSemeruHeapRegion* r) {
  ShouldNotReachHere();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  // assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  // assert(is_in(r), "Not in collection set");
  // _cset_map[r->region_number()] = 0;
  // _region_count --;
}

void ShenandoahSemeruCollectionSet::update_region_status() {
  ShouldNotReachHere();
  // for (size_t index = 0; index < _semeru_heap->num_regions(); index ++) {
  //   ShenandoahSemeruHeapRegion* r = _semeru_heap->get_region(index);
  //   if (is_in(r)) {
  //     r->make_cset();
  //   } else {
  //     assert (!r->is_cset(), "should not be cset");
  //   }
  // }
}

void ShenandoahSemeruCollectionSet::clear() {
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  Copy::zero_to_bytes(_cset_map, _map_size);

  // Modified by Haoran for remote evcuation
  Copy::zero_to_bytes(_compacted_map, _map_size);


#ifdef ASSERT
  for (size_t index = 0; index < _semeru_heap->num_regions(); index ++) {
    assert (!_semeru_heap->get_region(index)->is_cset(), "should have been cleared before");
  }
#endif

  _garbage = 0;
  _live_data = 0;
  _used = 0;

  _region_count = 0;
  _current_index = 0;
}

// Haoran: used on memory server to get the info from cpu server
void ShenandoahSemeruCollectionSet::copy_sync_to_cset() {
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  clear();


  size_t num_regions = _semeru_heap->num_regions();
  size_t index = (size_t)0;

  while(index < num_regions) {
    ShenandoahSemeruHeapRegion* region = _semeru_heap->get_region(index);
    region->set_state(region->sync_between_mem_and_cpu()->_state);

    if ((_sync_map[index] & 3) == 3) {
      add_region(region);
    }
    _cset_map[index] = _sync_map[index];
    index ++;
  }

  // Copy::zero_to_bytes(_sync_map, _map_size);
}

// Haoran: used on cpu server before send sync cset
void ShenandoahSemeruCollectionSet::copy_cset_to_sync() {
  ShouldNotReachHere();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  Copy::conjoint_jbytes(_cset_map, _sync_map, _map_size);
}

// ShenandoahSemeruHeapRegion* ShenandoahSemeruCollectionSet::claim_next() {
//   size_t num_regions = _semeru_heap->num_regions();
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
//         return _semeru_heap->get_region(index);
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

ShenandoahSemeruHeapRegion* ShenandoahSemeruCollectionSet::claim_next() {
  size_t num_regions = _semeru_heap->num_regions();
  if (_current_index >= (jint)num_regions) {
    return NULL;
  }

  jint saved_current = _current_index;
  size_t index = (size_t)saved_current;

  while(index < num_regions) {
    if (is_in_remote_update_set(index)) {
      jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
      assert(cur >= (jint)saved_current, "Must move forward");
      if (cur == saved_current) {
        assert(is_in_remote_update_set(index), "Invariant");
        return _semeru_heap->get_region(index);
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


// Modified by Haoran for remote compaction
ShenandoahSemeruHeapRegion* ShenandoahSemeruCollectionSet::claim_next_for_compaction() {

  // MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  
  size_t num_regions = _semeru_heap->num_regions();
  if (_current_index >= (jint)num_regions) {
    return NULL;
  }  

  size_t prioritized_index = (size_t)_current_index;
  // Haoran: cset_map: 0001 only in cset   1001 in cset and has been claimed  sync_map: 01 in cset 11 prioritized  compacted_map: 01 compacted by memory servers
  while(prioritized_index < num_regions) {
    if(_cset_map[prioritized_index] == 1 && (_cset_map[prioritized_index]&1) == 1 && ((_sync_map[prioritized_index] & 2) != 0 && !_compacted_map[prioritized_index])){
      char cur = Atomic::cmpxchg((char)3, &_cset_map[prioritized_index], (char)1);
      if(cur == 1) {
        return _semeru_heap->get_region(prioritized_index);
      }
    }
    prioritized_index ++;
  } 


  ShenandoahSemeruHeapRegion* r = NULL;
  // jint saved_current = _current_index;
  size_t index = (size_t)_current_index;;


  while(index < num_regions) {
    if (_cset_map[index] == 1) {
      char cur = Atomic::cmpxchg((char)3, &_cset_map[index], (char)1);
      if(cur == 1) {
        r = _semeru_heap->get_region(index);
        break;
      }
      else {
        index ++;
      }
    } else {
      index ++;
    }
  }

  if(index == num_regions) {
    Atomic::store((jint)index, &_current_index);
  }
  else {
    jint saved_current = _current_index;
    while((size_t)saved_current <= index) {
      jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
      if(cur == saved_current) break;
      saved_current = _current_index;
    }
    assert((size_t)_current_index > index, "Invariant");
  }

  return r;
  
}

ShenandoahSemeruHeapRegion* ShenandoahSemeruCollectionSet::next() {
  ShouldNotCallThis();
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  // assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  // size_t num_regions = _semeru_heap->num_regions();
  // for (size_t index = (size_t)_current_index; index < num_regions; index ++) {
  //   if (is_in(index)) {
  //     _current_index = (jint)(index + 1);
  //     return _semeru_heap->get_region(index);
  //   }
  // }

  return NULL;
}

void ShenandoahSemeruCollectionSet::print_on(outputStream* out) const {
  ShouldNotCallThis();
  // out->print_cr("Collection Set : " SIZE_FORMAT "", count());

  // debug_only(size_t regions = 0;)
  // for (size_t index = 0; index < _semeru_heap->num_regions(); index ++) {
  //   if (is_in(index)) {
  //     _semeru_heap->get_region(index)->print_on(out);
  //     debug_only(regions ++;)
  //   }
  // }
  // assert(regions == count(), "Must match");
}