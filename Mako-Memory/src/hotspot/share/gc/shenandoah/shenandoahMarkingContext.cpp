/*
 * Copyright (c) 2018, Red Hat, Inc. All rights reserved.
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
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
// Modified by Haoran
#include "gc/shenandoah/shenandoahSemeruHeap.hpp"
#include "gc/shenandoah/shenandoahSemeruHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.hpp"

ShenandoahMarkingContext::ShenandoahMarkingContext(MemRegion heap_region, MemRegion bitmap_region, size_t num_regions) :
  _top_bitmaps(NEW_C_HEAP_ARRAY(HeapWord*, num_regions, mtGC)),
  _top_at_mark_starts_base(NEW_C_HEAP_ARRAY(HeapWord*, num_regions, mtGC)),
  // _top_at_mark_starts(_top_at_mark_starts_base -
                      // ((uintx) heap_region.start() >> ShenandoahHeapRegion::region_size_bytes_shift())) 
  // Modified by Haoran
  _top_at_mark_starts(_top_at_mark_starts_base -
                      ((uintx) heap_region.start() >> ShenandoahSemeruHeapRegion::region_size_bytes_shift())) {

  // Modified by Haoran
  log_debug(semeru)("_top_bitmaps: 0x%lx, _top_at_mark_starts_base: 0x%lx, _top_at_mark_starts: 0x%lx, _top_at_mark_starts_addr: 0x%lx, index_offset: 0x%lx", 
    (size_t)_top_bitmaps, (size_t)_top_at_mark_starts_base, (size_t)_top_at_mark_starts, (size_t)(&_top_at_mark_starts), (size_t)((uintx) heap_region.start() >> ShenandoahSemeruHeapRegion::region_size_bytes_shift()));
  _mark_bit_map.initialize(heap_region, bitmap_region);
}

bool ShenandoahMarkingContext::is_bitmap_clear() const {
  // ShenandoahHeap* heap = ShenandoahHeap::heap();
  // Modified by Haoran
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  size_t num_regions = heap->num_regions();
  for (size_t idx = 0; idx < num_regions; idx++) {
    // ShenandoahHeapRegion* r = heap->get_region(idx);
    ShenandoahSemeruHeapRegion* r = heap->get_region(idx);
    if (heap->is_bitmap_slice_committed(r) && !is_bitmap_clear_range(r->bottom(), r->end())) {
      return false;
    }
  }
  return true;
}

bool ShenandoahMarkingContext::is_bitmap_clear_range(HeapWord* start, HeapWord* end) const {
  return _mark_bit_map.get_next_marked_addr(start, end) == end;
}

void ShenandoahMarkingContext::initialize_top_at_mark_start(ShenandoahHeapRegion* r) {
  size_t idx = r->region_number();
  HeapWord *bottom = r->bottom();
  _top_at_mark_starts_base[idx] = bottom;
  _top_bitmaps[idx] = bottom;
}

void ShenandoahMarkingContext::capture_top_at_mark_start(ShenandoahHeapRegion *r) {
  size_t region_number = r->region_number();
  HeapWord* old_tams = _top_at_mark_starts_base[region_number];
  HeapWord* new_tams = r->top();

  assert(new_tams >= old_tams,
         "Region " SIZE_FORMAT", TAMS updates should be monotonic: " PTR_FORMAT " -> " PTR_FORMAT,
         region_number, p2i(old_tams), p2i(new_tams));
  assert(is_bitmap_clear_range(old_tams, new_tams),
         "Region " SIZE_FORMAT ", bitmap should be clear while adjusting TAMS: " PTR_FORMAT " -> " PTR_FORMAT,
         region_number, p2i(old_tams), p2i(new_tams));

  _top_at_mark_starts_base[region_number] = new_tams;
  _top_bitmaps[region_number] = new_tams;
}

void ShenandoahMarkingContext::reset_top_at_mark_start(ShenandoahHeapRegion* r) {
  _top_at_mark_starts_base[r->region_number()] = r->bottom();
}


// Modified by Haoran
void ShenandoahMarkingContext::semeru_initialize_top_at_mark_start(ShenandoahSemeruHeapRegion* r) {
  size_t idx = r->region_number();
  HeapWord *bottom = r->bottom();
  _top_at_mark_starts_base[idx] = bottom;
  _top_bitmaps[idx] = bottom;
}

void ShenandoahMarkingContext::semeru_capture_top_at_mark_start(ShenandoahSemeruHeapRegion *r) {
  size_t region_number = r->region_number();
  HeapWord* old_tams = _top_at_mark_starts_base[region_number];
  HeapWord* new_tams = r->top();

  // assert(new_tams >= old_tams,
  //        "Region " SIZE_FORMAT", TAMS updates should be monotonic: " PTR_FORMAT " -> " PTR_FORMAT,
  //        region_number, p2i(old_tams), p2i(new_tams));
  assert(is_bitmap_clear_range(r->bottom(), new_tams),
         "Region " SIZE_FORMAT ", bitmap should be clear while adjusting TAMS: " PTR_FORMAT " -> " PTR_FORMAT,
         region_number, p2i(old_tams), p2i(new_tams));

  _top_at_mark_starts_base[region_number] = new_tams;
  _top_bitmaps[region_number] = new_tams;
}

void ShenandoahMarkingContext::semeru_reset_top_at_mark_start(ShenandoahSemeruHeapRegion* r) {
  _top_at_mark_starts_base[r->region_number()] = r->bottom();
}

HeapWord* ShenandoahMarkingContext::top_at_mark_start(ShenandoahHeapRegion* r) const {
  return _top_at_mark_starts_base[r->region_number()];
}

// Modified by Haoran
HeapWord* ShenandoahMarkingContext::semeru_top_at_mark_start(ShenandoahSemeruHeapRegion* r) const {
  return _top_at_mark_starts_base[r->region_number()];
}

void ShenandoahMarkingContext::reset_top_bitmap(ShenandoahHeapRegion* r) {
  assert(is_bitmap_clear_range(r->bottom(), r->end()),
         "Region " SIZE_FORMAT " should have no marks in bitmap", r->region_number());
  _top_bitmaps[r->region_number()] = r->bottom();
}

// Modified by Haoran
void ShenandoahMarkingContext::semeru_reset_top_bitmap(ShenandoahSemeruHeapRegion* r) {
  assert(is_bitmap_clear_range(r->bottom(), r->end()),
         "Region " SIZE_FORMAT " should have no marks in bitmap", r->region_number());
  _top_bitmaps[r->region_number()] = r->bottom();
}

void ShenandoahMarkingContext::clear_bitmap(ShenandoahHeapRegion* r) {
  HeapWord* bottom = r->bottom();
  HeapWord* top_bitmap = _top_bitmaps[r->region_number()];
  if (top_bitmap > bottom) {
    _mark_bit_map.clear_range_large(MemRegion(bottom, top_bitmap));
    _top_bitmaps[r->region_number()] = bottom;
  }
  assert(is_bitmap_clear_range(bottom, r->end()),
         "Region " SIZE_FORMAT " should have no marks in bitmap", r->region_number());
}

// Modified by Haoran
void ShenandoahMarkingContext::semeru_clear_bitmap(ShenandoahSemeruHeapRegion* r) {
  // tty->print("clear bitmap for range [0x%lx, 0x%lx)\n", (size_t)r->bottom(), (size_t)r->end());
  HeapWord* bottom = r->bottom();
  HeapWord* top_bitmap = _top_bitmaps[r->region_number()];
  
  // Modified by Haoran
  _mark_bit_map.clear_range_large(MemRegion(bottom, r->end()));

  if (top_bitmap > bottom) {
    //_mark_bit_map.clear_range_large(MemRegion(bottom, top_bitmap));
    

    _top_bitmaps[r->region_number()] = bottom;
  }
  assert(is_bitmap_clear_range(bottom, r->end()),
         "Region " SIZE_FORMAT " should have no marks in bitmap", r->region_number());
}

bool ShenandoahMarkingContext::is_complete() {
  return _is_complete.is_set();
}

void ShenandoahMarkingContext::mark_complete() {
  _is_complete.set();
}

void ShenandoahMarkingContext::mark_incomplete() {
  _is_complete.unset();
}


// Modified by Haoran
bool ShenandoahMarkingContext::allocated_after_mark_start(HeapWord* addr) const {
  // uintx index = ((uintx) addr) >> ShenandoahHeapRegion::region_size_bytes_shift();
  // HeapWord* top_at_mark_start = _top_at_mark_starts[index];
  // bool alloc_after_mark_start = addr >= top_at_mark_start;
  // return alloc_after_mark_start;
  // Modified by Haoran

  // Haoran: TODO
  uintx index = ((uintx) addr) >> ShenandoahSemeruHeapRegion::region_size_bytes_shift(); //Should be ShenandoahSemeruHeapRegion
  HeapWord* top_at_mark_start = _top_at_mark_starts[index];
  bool alloc_after_mark_start = addr >= top_at_mark_start;
  return alloc_after_mark_start;
}





void ShenandoahAliveTable::reset_alive_table(size_t region_index) {
  uint* st = _alive_table + ((_region_size_in_words * region_index) >> _log_page_size_in_words);
  size_t byte_size = (_region_size_in_words >> _log_page_size_in_words) * sizeof(uint);
  memset((char*)st, 0, byte_size);
}
void ShenandoahAliveTable::accumulate_table(size_t region_index) {
  size_t st_index = ((_region_size_in_words * region_index) >> _log_page_size_in_words);
  size_t ed_index  = st_index + (_region_size_in_words >> _log_page_size_in_words);
  for(size_t i = ed_index - 1; i > st_index; i--) {
    _alive_table[i] = _alive_table[i - 1];
  }
  _alive_table[st_index] = 0;
  for(size_t i = st_index + 1; i < ed_index; i++) {
    _alive_table[i] += _alive_table[i - 1];
  }
}

void ShenandoahAliveTable::increase_live_data(oop obj, size_t word_size) {
  size_t word_offset = (HeapWord*)obj - _heap_start;
  size_t page_index = word_offset >> _log_page_size_in_words;
  Atomic::add((uint)word_size, &_alive_table[page_index]);
}

oop ShenandoahAliveTable::get_target_address(oop obj) {
  ShenandoahSemeruHeapRegion* region = _heap->heap_region_containing(obj);
  ShenandoahMarkingContext* ctx = _heap->complete_marking_context();
  MarkBitMap* mark_bit_map = ctx->mark_bit_map();

  HeapWord* to_space_start = region->sync_between_mem_and_cpu()->_evac_start;
  size_t page_index = (((HeapWord*)obj - _heap_start) >> _log_page_size_in_words);
  uint alive_data = _alive_table[page_index];
  
  HeapWord* start = _heap_start + (page_index << _log_page_size_in_words);
  HeapWord* end = (HeapWord*)obj;
  HeapWord* cb = mark_bit_map->get_next_marked_addr(start, end);
  size_t skip_bitmap_delta = 1;
  while (cb < end) {
    oop prev_obj = oop(cb);
    assert(_heap->is_in(prev_obj), "sanity");
    assert(ctx->is_marked(prev_obj), "object expected to be marked");
    int size = prev_obj->size();
    alive_data += (uint)size;
    cb += skip_bitmap_delta;
    if (cb < end) {
      cb = mark_bit_map->get_next_marked_addr(cb, end);
    }
  }
  return (oop)(to_space_start + alive_data);
}