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

#include "gc/shenandoah/shenandoahSemeruFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahTraversalGC.hpp"
#include "logging/logStream.hpp"

// Modified by Haoran
ShenandoahSemeruFreeSet::ShenandoahSemeruFreeSet(ShenandoahSemeruHeap* semeru_heap, size_t max_regions) :
  _semeru_heap(semeru_heap),
  _mutator_free_bitmap(max_regions, mtGC),
  _collector_free_bitmap(max_regions, mtGC),
  _max(max_regions)
{
  clear_internal();
}

void ShenandoahSemeruFreeSet::increase_used(size_t num_bytes) {
  assert_heaplock_owned_by_current_thread();
  _used += num_bytes;

  assert(_used <= _capacity, "must not use more than we have: used: " SIZE_FORMAT
         ", capacity: " SIZE_FORMAT ", num_bytes: " SIZE_FORMAT, _used, _capacity, num_bytes);
}

bool ShenandoahSemeruFreeSet::is_mutator_free(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _mutator_leftmost, _mutator_rightmost);
  return _mutator_free_bitmap.at(idx);
}

bool ShenandoahSemeruFreeSet::is_collector_free(size_t idx) const {
  assert (idx < _max, "index is sane: " SIZE_FORMAT " < " SIZE_FORMAT " (left: " SIZE_FORMAT ", right: " SIZE_FORMAT ")",
          idx, _max, _collector_leftmost, _collector_rightmost);
  return _collector_free_bitmap.at(idx);
}

HeapWord* ShenandoahSemeruFreeSet::allocate_single(ShenandoahSemeruAllocRequest& req, bool& in_new_region) {
  // Scan the bitmap looking for a first fit.
  //
  // Leftmost and rightmost bounds provide enough caching to walk bitmap efficiently. Normally,
  // we would find the region to allocate at right away.
  //
  // Allocations are biased: new application allocs go to beginning of the heap, and GC allocs
  // go to the end. This makes application allocation faster, because we would clear lots
  // of regions from the beginning most of the time.
  //
  // Free set maintains mutator and collector views, and normally they allocate in their views only,
  // unless we special cases for stealing and mixed allocations.

  assert(req.type() == ShenandoahSemeruAllocRequest::_alloc_shared_gc, "Only support this allocation type on memory servers");

  size_t idx = req.region_index();
  // assert(is_collector_free(idx), "The region to be able to allocate gc object/lab must be collector free!");
  HeapWord* result = try_allocate_in(_semeru_heap->get_region(idx), req, in_new_region);
  if (result != NULL) {
    return result;
  }

  return NULL;
}

HeapWord* ShenandoahSemeruFreeSet::try_allocate_in(ShenandoahSemeruHeapRegion* r, ShenandoahSemeruAllocRequest& req, bool& in_new_region) {
  
  HeapWord* result = NULL;
  size_t size = req.size();
  result = r->allocate(size, req.type());
  
  if (result != NULL) {
    req.set_actual_size(size);
  }

  return result;
}

bool ShenandoahSemeruFreeSet::touches_bounds(size_t num) const {
  return num == _collector_leftmost || num == _collector_rightmost || num == _mutator_leftmost || num == _mutator_rightmost;
}

void ShenandoahSemeruFreeSet::recompute_bounds() {
  ShouldNotReachHere();
  // Reset to the most pessimistic case:
  _mutator_rightmost = _max - 1;
  _mutator_leftmost = 0;
  _collector_rightmost = _max - 1;
  _collector_leftmost = 0;

  // ...and adjust from there
  adjust_bounds();
}

void ShenandoahSemeruFreeSet::adjust_bounds() {
  
  ShouldNotReachHere();

  // Rewind both mutator bounds until the next bit.
  while (_mutator_leftmost < _max && !is_mutator_free(_mutator_leftmost)) {
    _mutator_leftmost++;
  }
  while (_mutator_rightmost > 0 && !is_mutator_free(_mutator_rightmost)) {
    _mutator_rightmost--;
  }
  // Rewind both collector bounds until the next bit.
  while (_collector_leftmost < _max && !is_collector_free(_collector_leftmost)) {
    _collector_leftmost++;
  }
  while (_collector_rightmost > 0 && !is_collector_free(_collector_rightmost)) {
    _collector_rightmost--;
  }
}

HeapWord* ShenandoahSemeruFreeSet::allocate_contiguous(ShenandoahSemeruAllocRequest& req) {
  
  ShouldNotReachHere();

  assert_heaplock_owned_by_current_thread();

  size_t words_size = req.size();
  size_t num = ShenandoahSemeruHeapRegion::required_regions(words_size * HeapWordSize);

  // No regions left to satisfy allocation, bye.
  if (num > mutator_count()) {
    return NULL;
  }

  // Find the continuous interval of $num regions, starting from $beg and ending in $end,
  // inclusive. Contiguous allocations are biased to the beginning.

  size_t beg = _mutator_leftmost;
  size_t end = beg;

  while (true) {
    if (end >= _max) {
      // Hit the end, goodbye
      return NULL;
    }

    // If regions are not adjacent, then current [beg; end] is useless, and we may fast-forward.
    // If region is not completely free, the current [beg; end] is useless, and we may fast-forward.
    if (!is_mutator_free(end) || !is_empty_or_trash(_semeru_heap->get_region(end))) {
      end++;
      beg = end;
      continue;
    }

    if ((end - beg + 1) == num) {
      // found the match
      break;
    }

    end++;
  };

  size_t remainder = words_size & ShenandoahSemeruHeapRegion::region_size_words_mask();

  // Initialize regions:
  for (size_t i = beg; i <= end; i++) {
    ShenandoahSemeruHeapRegion* r = _semeru_heap->get_region(i);
    try_recycle_trashed(r);

    assert(i == beg || _semeru_heap->get_region(i-1)->region_number() + 1 == r->region_number(), "Should be contiguous");
    assert(r->is_empty(), "Should be empty");

    if (i == beg) {
      r->make_humongous_start();
    } else {
      r->make_humongous_cont();
    }

    // Trailing region may be non-full, record the remainder there
    size_t used_words;
    if ((i == end) && (remainder != 0)) {
      used_words = remainder;
    } else {
      used_words = ShenandoahSemeruHeapRegion::region_size_words();
    }

    r->set_top(r->bottom() + used_words);
    r->reset_alloc_metadata_to_shared();

    _mutator_free_bitmap.clear_bit(r->region_number());
  }

  // While individual regions report their true use, all humongous regions are
  // marked used in the free set.
  increase_used(ShenandoahSemeruHeapRegion::region_size_bytes() * num);

  if (remainder != 0) {
    // Record this remainder as allocation waste
    _semeru_heap->notify_mutator_alloc_words(ShenandoahSemeruHeapRegion::region_size_words() - remainder, true);
  }

  // Allocated at left/rightmost? Move the bounds appropriately.
  if (beg == _mutator_leftmost || end == _mutator_rightmost) {
    adjust_bounds();
  }
  assert_bounds();

  req.set_actual_size(words_size);
  return _semeru_heap->get_region(beg)->bottom();
}

bool ShenandoahSemeruFreeSet::is_empty_or_trash(ShenandoahSemeruHeapRegion *r) {
  return r->is_empty() || r->is_trash();
}

size_t ShenandoahSemeruFreeSet::alloc_capacity(ShenandoahSemeruHeapRegion *r) {
  if (r->is_trash()) {
    // This would be recycled on allocation path
    return ShenandoahSemeruHeapRegion::region_size_bytes();
  } else {
    return r->free();
  }
}

bool ShenandoahSemeruFreeSet::has_no_alloc_capacity(ShenandoahSemeruHeapRegion *r) {
  return alloc_capacity(r) == 0;
}

void ShenandoahSemeruFreeSet::try_recycle_trashed(ShenandoahSemeruHeapRegion *r) {
  
  ShouldNotReachHere();


  if (r->is_trash()) {
    _semeru_heap->decrease_used(r->used());
    r->recycle();
  }
}

void ShenandoahSemeruFreeSet::recycle_trash() {
  
  ShouldNotReachHere();

  // lock is not reentrable, check we don't have it
  assert_heaplock_not_owned_by_current_thread();

  for (size_t i = 0; i < _semeru_heap->num_regions(); i++) {
    ShenandoahSemeruHeapRegion* r = _semeru_heap->get_region(i);
    if (r->is_trash()) {
      ShenandoahHeapLocker locker(_semeru_heap->lock());
      try_recycle_trashed(r);
    }
    SpinPause(); // allow allocators to take the lock
  }
}

void ShenandoahSemeruFreeSet::flip_to_gc(ShenandoahSemeruHeapRegion* r) {
  
  ShouldNotReachHere();


  size_t idx = r->region_number();

  assert(_mutator_free_bitmap.at(idx), "Should be in mutator view");
  assert(is_empty_or_trash(r), "Should not be allocated");

  _mutator_free_bitmap.clear_bit(idx);
  _collector_free_bitmap.set_bit(idx);
  _collector_leftmost = MIN2(idx, _collector_leftmost);
  _collector_rightmost = MAX2(idx, _collector_rightmost);

  _capacity -= alloc_capacity(r);

  if (touches_bounds(idx)) {
    adjust_bounds();
  }
  assert_bounds();
}

void ShenandoahSemeruFreeSet::clear() {
  assert_heaplock_owned_by_current_thread();
  clear_internal();
}

void ShenandoahSemeruFreeSet::clear_internal() {
  _mutator_free_bitmap.clear();
  _collector_free_bitmap.clear();
  _mutator_leftmost = _max;
  _mutator_rightmost = 0;
  _collector_leftmost = _max;
  _collector_rightmost = 0;
  _capacity = 0;
  _used = 0;
}

void ShenandoahSemeruFreeSet::rebuild() {
  ShouldNotReachHere();
  assert_heaplock_owned_by_current_thread();
  clear();

  for (size_t idx = 0; idx < _semeru_heap->num_regions(); idx++) {
    ShenandoahSemeruHeapRegion* region = _semeru_heap->get_region(idx);
    if (region->is_alloc_allowed() || region->is_trash()) {
      assert(!region->is_cset(), "Shouldn't be adding those to the free set");

      // Do not add regions that would surely fail allocation
      if (has_no_alloc_capacity(region)) continue;

      _capacity += alloc_capacity(region);
      assert(_used <= _capacity, "must not use more than we have");

      assert(!is_mutator_free(idx), "We are about to add it, it shouldn't be there already");
      _mutator_free_bitmap.set_bit(idx);
    }
  }

  // Evac reserve: reserve trailing space for evacuations
  size_t to_reserve = _semeru_heap->max_capacity() / 100 * ShenandoahEvacReserve;
  size_t reserved = 0;

  for (size_t idx = _semeru_heap->num_regions() - 1; idx > 0; idx--) {
    if (reserved >= to_reserve) break;

    ShenandoahSemeruHeapRegion* region = _semeru_heap->get_region(idx);
    if (_mutator_free_bitmap.at(idx) && is_empty_or_trash(region)) {
      _mutator_free_bitmap.clear_bit(idx);
      _collector_free_bitmap.set_bit(idx);
      size_t ac = alloc_capacity(region);
      _capacity -= ac;
      reserved += ac;
    }
  }

  recompute_bounds();
  assert_bounds();
}

void ShenandoahSemeruFreeSet::log_status() {
  ShouldNotReachHere();

  assert_heaplock_owned_by_current_thread();

  LogTarget(Info, gc, ergo) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);

    {
      size_t last_idx = 0;
      size_t max = 0;
      size_t max_contig = 0;
      size_t empty_contig = 0;

      size_t total_used = 0;
      size_t total_free = 0;

      for (size_t idx = _mutator_leftmost; idx <= _mutator_rightmost; idx++) {
        if (is_mutator_free(idx)) {
          ShenandoahSemeruHeapRegion *r = _semeru_heap->get_region(idx);
          size_t free = alloc_capacity(r);

          max = MAX2(max, free);

          if (r->is_empty() && (last_idx + 1 == idx)) {
            empty_contig++;
          } else {
            empty_contig = 0;
          }

          total_used += r->used();
          total_free += free;

          max_contig = MAX2(max_contig, empty_contig);
          last_idx = idx;
        }
      }

      size_t max_humongous = max_contig * ShenandoahSemeruHeapRegion::region_size_bytes();
      size_t free = capacity() - used();

      ls.print("Free: " SIZE_FORMAT "M (" SIZE_FORMAT " regions), Max regular: " SIZE_FORMAT "K, Max humongous: " SIZE_FORMAT "K, ",
               total_free / M, mutator_count(), max / K, max_humongous / K);

      size_t frag_ext;
      if (free > 0) {
        frag_ext = 100 - (100 * max_humongous / free);
      } else {
        frag_ext = 0;
      }
      ls.print("External frag: " SIZE_FORMAT "%%, ", frag_ext);

      size_t frag_int;
      if (mutator_count() > 0) {
        frag_int = (100 * (total_used / mutator_count()) / ShenandoahSemeruHeapRegion::region_size_bytes());
      } else {
        frag_int = 0;
      }
      ls.print("Internal frag: " SIZE_FORMAT "%%", frag_int);
      ls.cr();
    }

    {
      size_t max = 0;
      size_t total_free = 0;

      for (size_t idx = _collector_leftmost; idx <= _collector_rightmost; idx++) {
        if (is_collector_free(idx)) {
          ShenandoahSemeruHeapRegion *r = _semeru_heap->get_region(idx);
          size_t free = alloc_capacity(r);
          max = MAX2(max, free);
          total_free += free;
        }
      }

      ls.print_cr("Evacuation Reserve: " SIZE_FORMAT "M (" SIZE_FORMAT " regions), Max regular: " SIZE_FORMAT "K",
                  total_free / M, collector_count(), max / K);
    }
  }
}

HeapWord* ShenandoahSemeruFreeSet::allocate(ShenandoahSemeruAllocRequest& req, bool& in_new_region) {

  if (req.size() > ShenandoahSemeruHeapRegion::humongous_threshold_words()) {
    ShouldNotReachHere();
    return NULL;
  } else {
    return allocate_single(req, in_new_region);
  }
}

size_t ShenandoahSemeruFreeSet::unsafe_peek_free() const {
  ShouldNotReachHere();
  
  // Deliberately not locked, this method is unsafe when free set is modified.

  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (index < _max && is_mutator_free(index)) {
      ShenandoahSemeruHeapRegion* r = _semeru_heap->get_region(index);
      if (r->free() >= MinTLABSize) {
        return r->free();
      }
    }
  }

  // It appears that no regions left
  return 0;
}

void ShenandoahSemeruFreeSet::print_on(outputStream* out) const {
  out->print_cr("Mutator Free Set: " SIZE_FORMAT "", mutator_count());
  for (size_t index = _mutator_leftmost; index <= _mutator_rightmost; index++) {
    if (is_mutator_free(index)) {
      _semeru_heap->get_region(index)->print_on(out);
    }
  }
  out->print_cr("Collector Free Set: " SIZE_FORMAT "", collector_count());
  for (size_t index = _collector_leftmost; index <= _collector_rightmost; index++) {
    if (is_collector_free(index)) {
      _semeru_heap->get_region(index)->print_on(out);
    }
  }
}

#ifdef ASSERT
void ShenandoahSemeruFreeSet::assert_heaplock_owned_by_current_thread() const {
  _semeru_heap->assert_heaplock_owned_by_current_thread();
}

void ShenandoahSemeruFreeSet::assert_heaplock_not_owned_by_current_thread() const {
  _semeru_heap->assert_heaplock_not_owned_by_current_thread();
}

void ShenandoahSemeruFreeSet::assert_bounds() const {
  // Performance invariants. Failing these would not break the free set, but performance
  // would suffer.
  assert (_mutator_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _mutator_leftmost,  _max);
  assert (_mutator_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _mutator_rightmost, _max);

  assert (_mutator_leftmost == _max || is_mutator_free(_mutator_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _mutator_leftmost);
  assert (_mutator_rightmost == 0   || is_mutator_free(_mutator_rightmost), "rightmost region should be free: " SIZE_FORMAT, _mutator_rightmost);

  size_t beg_off = _mutator_free_bitmap.get_next_one_offset(0);
  size_t end_off = _mutator_free_bitmap.get_next_one_offset(_mutator_rightmost + 1);
  assert (beg_off >= _mutator_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _mutator_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _mutator_rightmost);

  assert (_collector_leftmost <= _max, "leftmost in bounds: "  SIZE_FORMAT " < " SIZE_FORMAT, _collector_leftmost,  _max);
  assert (_collector_rightmost < _max, "rightmost in bounds: " SIZE_FORMAT " < " SIZE_FORMAT, _collector_rightmost, _max);

  assert (_collector_leftmost == _max || is_collector_free(_collector_leftmost),  "leftmost region should be free: " SIZE_FORMAT,  _collector_leftmost);
  assert (_collector_rightmost == 0   || is_collector_free(_collector_rightmost), "rightmost region should be free: " SIZE_FORMAT, _collector_rightmost);

  beg_off = _collector_free_bitmap.get_next_one_offset(0);
  end_off = _collector_free_bitmap.get_next_one_offset(_collector_rightmost + 1);
  assert (beg_off >= _collector_leftmost, "free regions before the leftmost: " SIZE_FORMAT ", bound " SIZE_FORMAT, beg_off, _collector_leftmost);
  assert (end_off == _max,      "free regions past the rightmost: " SIZE_FORMAT ", bound " SIZE_FORMAT,  end_off, _collector_rightmost);
}
#endif
