// Modified by Haoran
/*
 * Copyright (c) 2015, 2018, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAPREGION_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAPREGION_INLINE_HPP


#include "gc/shenandoah/shenandoahSemeruHeapRegion.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahPacer.inline.hpp"
#include "runtime/atomic.hpp"

HeapWord* ShenandoahSemeruHeapRegion::allocate(size_t size, ShenandoahSemeruAllocRequest::Type type) {
  // Modified by Haoran for compaction
  // _heap->assert_heaplock_or_safepoint();

  assert(is_object_aligned(size), "alloc size breaks alignment: " SIZE_FORMAT, size);

  HeapWord* obj = top();
  if (pointer_delta(end(), obj) >= size) {
    // make_regular_allocation();
    // adjust_alloc_metadata(type, size);

    HeapWord* new_top = obj + size;
    set_top(new_top);

    assert(is_object_aligned(new_top), "new top breaks alignment: " PTR_FORMAT, p2i(new_top));
    assert(is_object_aligned(obj),     "obj is not aligned: "       PTR_FORMAT, p2i(obj));

    return obj;
  } else {
    ShouldNotReachHere();
    return NULL;
  }
}

inline void ShenandoahSemeruHeapRegion::adjust_alloc_metadata(ShenandoahSemeruAllocRequest::Type type, size_t size) {
  bool is_first_alloc = (top() == bottom());

  switch (type) {
    case ShenandoahSemeruAllocRequest::_alloc_shared:
    case ShenandoahSemeruAllocRequest::_alloc_tlab:
      _seqnum_last_alloc_mutator = _alloc_seq_num.value++;
      if (is_first_alloc) {
        assert (_seqnum_first_alloc_mutator == 0, "Region " SIZE_FORMAT " metadata is correct", region_number());
        _seqnum_first_alloc_mutator = _seqnum_last_alloc_mutator;
      }
      break;
    case ShenandoahSemeruAllocRequest::_alloc_shared_gc:
    case ShenandoahSemeruAllocRequest::_alloc_gclab:
      _seqnum_last_alloc_gc = _alloc_seq_num.value++;
      if (is_first_alloc) {
        assert (_seqnum_first_alloc_gc == 0, "Region " SIZE_FORMAT " metadata is correct", region_number());
        _seqnum_first_alloc_gc = _seqnum_last_alloc_gc;
      }
      break;
    default:
      ShouldNotReachHere();
  }

  switch (type) {
    case ShenandoahSemeruAllocRequest::_alloc_shared:
    case ShenandoahSemeruAllocRequest::_alloc_shared_gc:
      _shared_allocs += size;
      break;
    case ShenandoahSemeruAllocRequest::_alloc_tlab:
      _tlab_allocs += size;
      break;
    case ShenandoahSemeruAllocRequest::_alloc_gclab:
      _gclab_allocs += size;
      break;
    default:
      ShouldNotReachHere();
  }
}

inline void ShenandoahSemeruHeapRegion::increase_live_data_alloc_words(size_t s) {
  internal_increase_live_data(s);
}

inline size_t ShenandoahSemeruHeapRegion::increase_live_data_gc_words(size_t s) {
  // internal_increase_live_data(s);
  size_t words = internal_increase_live_data(s);
  if (ShenandoahPacing) {
    _heap->pacer()->report_mark(s);
  }
  return words;
}

inline size_t ShenandoahSemeruHeapRegion::internal_increase_live_data(size_t s) {
  size_t new_live_data = Atomic::add(s, &(_mem_to_cpu_at_gc->_live_data));

#ifdef ASSERT
  size_t live_bytes = new_live_data * HeapWordSize;
  size_t used_bytes = used();
  assert(live_bytes <= used_bytes,
         "can't have more live data than used: " SIZE_FORMAT ", " SIZE_FORMAT, live_bytes, used_bytes);
#endif
  return new_live_data;
}


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_INLINE_HPP
