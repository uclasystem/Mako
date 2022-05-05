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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHSEMERUCOLLECTIONSET_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHSEMERUCOLLECTIONSET_HPP

#include "memory/allocation.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.hpp"
#include "gc/shenandoah/shenandoahSemeruHeapRegion.hpp"

class ShenandoahSemeruCollectionSet : public CHeapObj<mtGC> {
  // Modified by Haoran
  friend class ShenandoahSemeruHeap;
private:
  size_t const          _map_size;
  size_t const          _region_size_bytes_shift;
  ReservedSpace         _map_space;
  char* const           _cset_map;
  // Bias cset map's base address for fast test if an oop is in cset
  char* const           _biased_cset_map;

public:
  // Modified by Haoran for sync
  char* const           _sync_map;
  char* const           _compacted_map;

private:

  ShenandoahSemeruHeap* const _semeru_heap;

  size_t                _garbage;
  size_t                _live_data;
  size_t                _used;
  size_t                _region_count;

  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(volatile size_t));
  volatile jint         _current_index;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, 0);

public:
  ShenandoahSemeruCollectionSet(ShenandoahSemeruHeap* semeru_heap, char* semeru_heap_base, size_t size, char* sync_map_base, char* compacted_map_base);

  // Add region to collection set
  void add_region(ShenandoahSemeruHeapRegion* r);
  bool add_region_check_for_duplicates(ShenandoahSemeruHeapRegion* r);

  // Bring per-region statuses to consistency with this collection.
  // TODO: This is a transitional interface that bridges the gap between
  // region statuses and this collection. Should go away after we merge them.
  void update_region_status();

  // Remove region from collection set
  void remove_region(ShenandoahSemeruHeapRegion* r);

  // MT version
  ShenandoahSemeruHeapRegion* claim_next();
  // Modified by Haoran for remote compaction
  ShenandoahSemeruHeapRegion* claim_next_for_compaction();

  // Single-thread version
  ShenandoahSemeruHeapRegion* next();

  size_t count()  const { return _region_count; }
  bool is_empty() const { return _region_count == 0; }

  void clear_current_index() {
    _current_index = 0;
  }
  inline bool is_in_remote_update_set(ShenandoahSemeruHeapRegion* r) const;
  inline bool is_in_remote_update_set(size_t region_number)    const;
  inline bool is_in_update_set(ShenandoahSemeruHeapRegion* r) const;
  inline bool is_in_update_set(size_t region_number)    const;
  inline bool is_in_update_set(HeapWord* p)             const;
  inline bool is_in_evac_set(ShenandoahSemeruHeapRegion* r) const;
  inline bool is_in_evac_set(size_t region_number)    const;
  inline bool is_in_evac_set(HeapWord* p)             const;
  inline bool is_compacted(size_t region_number)  const;

  void print_on(outputStream* out) const;

  size_t used()      const { return _used; }
  size_t live_data() const { return _live_data; }
  size_t garbage()   const { return _garbage;   }
  void clear();

  // Modified by Haoran for sync
  void copy_sync_to_cset();
  void copy_cset_to_sync();
  

private:
  char* map_address() const {
    return _cset_map;
  }
  char* biased_map_address() const {
    return _biased_cset_map;
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP
