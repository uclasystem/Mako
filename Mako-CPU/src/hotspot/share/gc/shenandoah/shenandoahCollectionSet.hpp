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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP

#include "memory/allocation.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"

struct RegionCacheData {
  ShenandoahHeapRegion* _region;
  uint _cache_ratio;
  uint _cache_pages;
  uint _total_pages;
};

class ShenandoahCollectionSet : public CHeapObj<mtGC> {
  friend class ShenandoahHeap;
private:
  size_t const          _map_size;
  size_t const          _region_size_bytes_shift;
  ReservedSpace         _map_space;
  char* const           _cset_map;
  // Bias cset map's base address for fast test if an oop is in cset
  char* const           _biased_cset_map;
  RegionCacheData*      _region_data;

public:
  // Modified by Haoran for sync
  char* const           _sync_map;
  char* const           _compacted_map;

private:
  ShenandoahHeap* const _heap;

  size_t                _garbage;
  size_t                _live_data;
  size_t                _used;
  size_t                _region_count;
  size_t                _local_pages;

  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(volatile size_t));
  volatile jint         _current_index;
  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, 0);

public:
  // Modified by Haoran for sync
  // ShenandoahCollectionSet(ShenandoahHeap* heap, char* heap_base, size_t size);
  ShenandoahCollectionSet(ShenandoahHeap* heap, char* heap_base, size_t size, char* sync_map_base, char* compacted_map_base);


  static int compare_by_cache(RegionCacheData a, RegionCacheData b);

  // Add region to collection set
  void add_region(ShenandoahHeapRegion* r);
  void add_region_to_update(ShenandoahHeapRegion* r);
  bool add_region_check_for_duplicates(ShenandoahHeapRegion* r);

  // Bring per-region statuses to consistency with this collection.
  // TODO: This is a transitional interface that bridges the gap between
  // region statuses and this collection. Should go away after we merge them.
  void update_region_status();

  // Remove region from collection set
  void remove_region(ShenandoahHeapRegion* r);

  // MT version
  ShenandoahHeapRegion* claim_next();

  // Single-thread version
  ShenandoahHeapRegion* next();
  ShenandoahHeapRegion* next_evac_set();

  size_t count()  const { return _region_count; }
  bool is_empty() const { return _region_count == 0; }

  void clear_current_index() {
    _current_index = 0;
  }

  inline bool is_in_local_update_set(ShenandoahHeapRegion* r) const;
  inline bool is_in_local_update_set(size_t region_number)    const;
  inline bool is_in_update_set(ShenandoahHeapRegion* r) const;
  inline bool is_in_update_set(size_t region_number)    const;
  inline bool is_in_update_set(HeapWord* p)             const;
  inline bool is_in_evac_set(ShenandoahHeapRegion* r) const;
  inline bool is_in_evac_set(size_t region_number)    const;
  inline bool is_in_evac_set(HeapWord* p)             const;
  inline bool atomic_is_in_evac_set(HeapWord* p)             const;

  bool is_update_finished(size_t region_number)    const;
  void set_update_finished(size_t region_number);
  bool is_evac_finished(size_t region_number)    const;
  void set_evac_finished(size_t region_number);

  void print_on(outputStream* out) const;

  size_t used()      const { return _used; }
  size_t live_data() const { return _live_data; }
  size_t garbage()   const { return _garbage;   }
  void clear();

  // Modified by Haoran for sync
  void copy_sync_to_cset();
  void copy_cset_to_sync();
  // void copy_compacted_to_cset();
  bool select_local_process_regions();
  bool select_local_update_regions();
  void add_region_to_local_set(size_t region_index);


  ShenandoahHeapRegion* claim_next_for_compaction();
  void set_as_compacted(size_t region_index);
  HeapWord* alloc_space_and_update_start(ShenandoahHeapRegion* r);

  void reset_local_pages() {
    _local_pages = 0;
  }

private:
  char* map_address() const {
    return _cset_map;
  }
  char* biased_map_address() const {
    return _biased_cset_map;
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_HPP
