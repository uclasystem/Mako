/*
 * Copyright (c) 2017, 2018, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_INLINE_HPP

#include "gc/shenandoah/shenandoahCollectionSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"

bool ShenandoahCollectionSet::is_in_local_update_set(size_t region_number) const {
  assert(region_number < _heap->num_regions(), "Sanity");
  return (_cset_map[region_number] & 5) == 5;
}

bool ShenandoahCollectionSet::is_in_local_update_set(ShenandoahHeapRegion* r) const {
  return is_in_local_update_set(r->region_number());
}

bool ShenandoahCollectionSet::is_in_update_set(size_t region_number) const {
  assert(region_number < _heap->num_regions(), "Sanity");
  return (_cset_map[region_number] & 1);
}

bool ShenandoahCollectionSet::is_in_update_set(ShenandoahHeapRegion* r) const {
  return is_in_update_set(r->region_number());
}

bool ShenandoahCollectionSet::is_in_update_set(HeapWord* p) const {
  assert(_heap->is_in(p), "Must be in the heap");
  uintx index = ((uintx) p) >> _region_size_bytes_shift;
  // no need to subtract the bottom of the heap from p,
  // _biased_cset_map is biased
  return (_biased_cset_map[index] & 1);
}

bool ShenandoahCollectionSet::is_in_evac_set(size_t region_number) const {
  assert(region_number < _heap->num_regions(), "Sanity");
  return (_cset_map[region_number] & 2);
}

bool ShenandoahCollectionSet::is_in_evac_set(ShenandoahHeapRegion* r) const {
  return is_in_evac_set(r->region_number());
}

bool ShenandoahCollectionSet::is_in_evac_set(HeapWord* p) const {
  assert(_heap->is_in(p), "Must be in the heap");
  uintx index = ((uintx) p) >> _region_size_bytes_shift;
  // no need to subtract the bottom of the heap from p,
  // _biased_cset_map is biased
  return (_biased_cset_map[index] & 2);
}

bool ShenandoahCollectionSet::atomic_is_in_evac_set(HeapWord* p) const {
  assert(_heap->is_in(p), "Must be in the heap");
  uintx index = ((uintx) p) >> _region_size_bytes_shift;
  // no need to subtract the bottom of the heap from p,
  // _biased_cset_map is biased
  return (Atomic::load(&_biased_cset_map[index]) & 2);
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCOLLECTIONSET_INLINE_HPP
