/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHMARKINGCONTEXT_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHMARKINGCONTEXT_HPP

#include "gc/shared/markBitMap.hpp"
#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "oops/oopsHierarchy.hpp"

/**
 * Encapsulate a marking bitmap with the top-at-mark-start and top-bitmaps array.
 */
class ShenandoahMarkingContext : public CHeapObj<mtGC> {
private:
  MarkBitMap _mark_bit_map;

  HeapWord** const _top_bitmaps;
  HeapWord** const _top_at_mark_starts_base;
  HeapWord** const _top_at_mark_starts;

  ShenandoahSharedFlag _is_complete;

public:
  ShenandoahMarkingContext(MemRegion heap_region, MemRegion bitmap_region, size_t num_regions);

  /*
   * Marks the object. Returns true if the object has not been marked before and has
   * been marked by this thread. Returns false if the object has already been marked,
   * or if a competing thread succeeded in marking this object.
   */
  inline bool mark(oop obj);

  inline bool is_marked(oop obj) const;

  inline bool allocated_after_mark_start(HeapWord* addr) const;

  inline MarkBitMap* mark_bit_map();

  HeapWord* top_at_mark_start(ShenandoahHeapRegion* r) const;
  void capture_top_at_mark_start(ShenandoahHeapRegion* r);
  void reset_top_at_mark_start(ShenandoahHeapRegion* r);
  void initialize_top_at_mark_start(ShenandoahHeapRegion* r);

  void reset_top_bitmap(ShenandoahHeapRegion *r);
  void clear_bitmap(ShenandoahHeapRegion *r);

  bool is_bitmap_clear() const;
  bool is_bitmap_clear_range(HeapWord* start, HeapWord* end) const;

  bool is_complete();
  void mark_complete();
  void mark_incomplete();

};

class ShenandoahAliveTable : public CHeapObj<mtGC> {
private:
  ShenandoahHeap* _heap;
  HeapWord* _heap_start;
  size_t _heap_size_in_words;
  size_t _region_size_in_words;
  size_t _page_size_in_words;
  size_t _log_page_size_in_words;
  uint* _alive_table;

public:
  ShenandoahAliveTable(ShenandoahHeap* heap, HeapWord* heap_start, size_t heap_size_in_words, size_t region_size_in_words, size_t page_size_in_words, size_t log_page_size_in_words, uint* table_start) :
  _heap(heap),
  _heap_start(heap_start),
  _heap_size_in_words(heap_size_in_words),
  _region_size_in_words(region_size_in_words),
  _page_size_in_words(page_size_in_words),
  _log_page_size_in_words(log_page_size_in_words),
  _alive_table(table_start) {
    log_debug(semeru)("_alive_table: 0x%lx", (size_t)_alive_table);
  } 

  void reset_alive_table(size_t region_index);
  void accumulate_table(size_t region_index);
  void increase_live_data(oop obj, size_t word_size);
  oop get_target_address(oop obj);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHMARKINGCONTEXT_HPP
