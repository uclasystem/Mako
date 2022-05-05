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

#ifndef SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAP_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAP_INLINE_HPP

#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/threadLocalAllocBuffer.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
// Modified by Haoran2
// #include "gc/shenandoah/shenandoahBrooksPointer.inline.hpp"
#include "gc/shenandoah/shenandoahForwarding.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruCollectionSet.hpp"
#include "gc/shenandoah/shenandoahSemeruCollectionSet.inline.hpp"
#include "gc/shenandoah/shenandoahWorkGroup.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruControlThread.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"

#include "gc/shenandoah/shenandoahOopClosures.hpp"

#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/prefetch.hpp"
#include "runtime/prefetch.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/globalDefinitions.hpp"


inline ShenandoahSemeruHeapRegion* ShenandoahSemeruRegionIterator::next() {
  size_t new_index = Atomic::add((size_t) 1, &_index);
  // get_region() provides the bounds-check and returns NULL on OOB.
  return _heap->get_region(new_index - 1);
}

inline bool ShenandoahSemeruHeap::has_forwarded_objects() const {
  return _gc_state.is_set(HAS_FORWARDED);
}

inline WorkGang* ShenandoahSemeruHeap::workers() const {
  return _workers;
}

inline WorkGang* ShenandoahSemeruHeap::get_safepoint_workers() {
  return _safepoint_workers;
}

inline size_t ShenandoahSemeruHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) semeru_base()) >> ShenandoahSemeruHeapRegion::region_size_bytes_shift();
  assert(index < num_regions(), "Region index is in bounds: " PTR_FORMAT, p2i(addr));
  return index;
}

inline ShenandoahSemeruHeapRegion* const ShenandoahSemeruHeap::heap_region_containing(const void* addr) const {
  size_t index = heap_region_index_containing(addr);
  ShenandoahSemeruHeapRegion* const result = get_region(index);
  assert(addr >= result->bottom() && addr < result->end(), "Heap region contains the address: " PTR_FORMAT, p2i(addr));
  return result;
}

template <class T>
inline oop ShenandoahSemeruHeap::update_with_forwarded_not_null(T* p, oop obj) {
  ShouldNotCallThis();
//   if (in_collection_set(obj)) {
//     shenandoah_assert_forwarded_except(p, obj, is_full_gc_in_progress() || cancelled_gc() || is_degenerated_gc_in_progress());


//     // Modified by Haoran
//     // Haoran: debug
//     ShouldNotReachHere();
//     obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
//     RawAccess<IS_NOT_NULL>::oop_store(p, obj);
//   }
// #ifdef ASSERT
//   else {
//     shenandoah_assert_not_forwarded(p, obj);
//   }
// #endif
  return obj;
}

template <class T>
inline oop ShenandoahSemeruHeap::maybe_update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    return maybe_update_with_forwarded_not_null(p, obj);
  } else {
    return NULL;
  }
}

template <class T>
inline void ShenandoahSemeruHeap::semeru_maybe_update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);

    if (collection_set()->is_in_evac_set((HeapWord*)obj)) {
      oop new_obj = ShenandoahForwarding::get_forwardee(obj);
      if(new_obj == obj) {
        new_obj = _alive_table->get_target_address(obj);
      }
      // assert(ShenandoahForwarding::is_forwarded(obj), "Invariant");
      assert(heap_region_containing(new_obj) != heap_region_containing(obj), "Invariant!");
      assert(!collection_set()->is_in_evac_set((HeapWord*)new_obj), "Invariant!");
      // assert(new_obj <= obj, "Invariant!");
      oop result = cas_oop(new_obj, p, obj);
      if (!oopDesc::equals_raw(result, obj)) {
        assert(CompressedOops::is_null(result) || !collection_set()->is_in_evac_set((HeapWord*)result),
              "expect not in cset");
#ifdef RELEASE_CHECK
        if(!CompressedOops::is_null(result) && collection_set()->is_in_evac_set((HeapWord*)result)){
          ShouldNotReachHere();
        }
#endif    
      }


      // RawAccess<>::oop_store(p, new_obj);
    }
  }
}

template <class T>
inline oop ShenandoahSemeruHeap::evac_update_with_forwarded(T* p) {
  ShouldNotCallThis();
  return NULL;
  // T o = RawAccess<>::oop_load(p);
  // if (!CompressedOops::is_null(o)) {
  //   oop heap_oop = CompressedOops::decode_not_null(o);
  //   if (in_collection_set(heap_oop)) {
  //     oop forwarded_oop = ShenandoahBarrierSet::resolve_forwarded_not_null(heap_oop);
  //     if (oopDesc::equals_raw(forwarded_oop, heap_oop)) {
  //       forwarded_oop = evacuate_object(heap_oop, Thread::current());
  //     }
  //     oop prev = atomic_compare_exchange_oop(forwarded_oop, p, heap_oop);
  //     if (oopDesc::equals_raw(prev, heap_oop)) {
  //       return forwarded_oop;
  //     } else {
  //       return NULL;
  //     }
  //   }
  //   return heap_oop;
  // } else {
  //   return NULL;
  // }
}

inline oop ShenandoahSemeruHeap::cas_oop(oop n, oop* addr, oop c) {
  return (oop) Atomic::cmpxchg(n, addr, c);
}

inline oop ShenandoahSemeruHeap::cas_oop(oop n, narrowOop* addr, oop c) {
  narrowOop cmp = CompressedOops::encode(c);
  narrowOop val = CompressedOops::encode(n);
  return CompressedOops::decode((narrowOop) Atomic::cmpxchg(val, addr, cmp));
}

template <class T>
inline oop ShenandoahSemeruHeap::maybe_update_with_forwarded_not_null(T* p, oop heap_oop) {
  ShouldNotCallThis();
  return NULL;
  // shenandoah_assert_not_in_cset_loc_except(p, !is_in(p) || is_full_gc_in_progress() || is_degenerated_gc_in_progress());
  // shenandoah_assert_correct(p, heap_oop);

  // if (in_collection_set(heap_oop)) {
  //   oop forwarded_oop = ShenandoahBarrierSet::resolve_forwarded_not_null(heap_oop);
  //   if (oopDesc::equals_raw(forwarded_oop, heap_oop)) {
  //     // E.g. during evacuation.
  //     return forwarded_oop;
  //   }

  //   shenandoah_assert_forwarded_except(p, heap_oop, is_full_gc_in_progress() || is_degenerated_gc_in_progress());
  //   shenandoah_assert_not_in_cset_except(p, forwarded_oop, cancelled_gc());

  //   // If this fails, another thread wrote to p before us, it will be logged in SATB and the
  //   // reference be updated later.
  //   oop result = atomic_compare_exchange_oop(forwarded_oop, p, heap_oop);

  //   if (oopDesc::equals_raw(result, heap_oop)) { // CAS successful.
  //     return forwarded_oop;
  //   } else {
  //     // Note: we used to assert the following here. This doesn't work because sometimes, during
  //     // marking/updating-refs, it can happen that a Java thread beats us with an arraycopy,
  //     // which first copies the array, which potentially contains from-space refs, and only afterwards
  //     // updates all from-space refs to to-space refs, which leaves a short window where the new array
  //     // elements can be from-space.
  //     // assert(CompressedOops::is_null(result) ||
  //     //        oopDesc::equals_raw(result, ShenandoahBarrierSet::resolve_oop_static_not_null(result)),
  //     //       "expect not forwarded");
  //     return NULL;
  //   }
  // } else {
  //   shenandoah_assert_not_forwarded(p, heap_oop);
  //   return heap_oop;
  // }
}

// inline bool ShenandoahSemeruHeap::cancelled_gc() const {
//   return _cancelled_gc.get() == CANCELLED;
// }

inline bool ShenandoahSemeruHeap::check_cancelled_gc_and_yield(bool sts_active) {
  if (! (sts_active && ShenandoahSuspendibleWorkers)) {
    return cancelled_gc();
  }

  jbyte prev = _cancelled_gc.cmpxchg(NOT_CANCELLED, CANCELLABLE);
  if (prev == CANCELLABLE || prev == NOT_CANCELLED) {
    if (SuspendibleThreadSet::should_yield()) {
      SuspendibleThreadSet::yield();
    }

    // Back to CANCELLABLE. The thread that poked NOT_CANCELLED first gets
    // to restore to CANCELLABLE.
    if (prev == CANCELLABLE) {
      _cancelled_gc.set(CANCELLABLE);
    }
    return false;
  } else {
    return true;
  }
}

inline bool ShenandoahSemeruHeap::try_cancel_gc() {
  while (true) {
    jbyte prev = _cancelled_gc.cmpxchg(CANCELLED, CANCELLABLE);
    if (prev == CANCELLABLE) return true;
    else if (prev == CANCELLED) return false;
    assert(ShenandoahSuspendibleWorkers, "should not get here when not using suspendible workers");
    assert(prev == NOT_CANCELLED, "must be NOT_CANCELLED");
    {
      // We need to provide a safepoint here, otherwise we might
      // spin forever if a SP is pending.
      ThreadBlockInVM sp(JavaThread::current());
      SpinPause();
    }
  }
}

inline void ShenandoahSemeruHeap::clear_cancelled_gc() {
  _cancelled_gc.set(CANCELLABLE);
  _oom_evac_handler.clear();
}

inline HeapWord* ShenandoahSemeruHeap::allocate_from_gclab(Thread* thread, size_t size) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
  if (gclab == NULL) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(),
           "Performance: thread should have GCLAB: %s", thread->name());
    // No GCLABs in this thread, fallback to shared allocation
    return NULL;
  }
  HeapWord* obj = gclab->allocate(size);
  if (obj != NULL) {
    return obj;
  }
  // Otherwise...
  return allocate_from_gclab_slow(thread, size);
}

inline oop ShenandoahSemeruHeap::evacuate_object(oop p, Thread* thread) {

  // Modified by Shi
  shenandoah_assert_in_heap(NULL, p);

  if (ShenandoahThreadLocalData::is_oom_during_evac(Thread::current())) {
    ShouldNotReachHere();
  }

  assert(ShenandoahThreadLocalData::is_evac_allowed(thread), "must be enclosed in oom-evac scope");

  size_t size = (size_t) p->size();

  assert(!heap_region_containing(p)->is_humongous(), "never evacuate humongous objects");

  // Modified by Haoran for evacuaion
  ShenandoahSemeruHeapRegion* region = heap_region_containing(p);

  // HeapWord* copy = region->offset_table()->get((HeapWord*)p);
  // HeapWord* copy = region->_evac_top;
  // region->_evac_top += size;
  HeapWord* copy = (HeapWord*)(alive_table()->get_target_address(p));
  
  assert(heap_region_index_containing(copy) != region->region_number(), "Invariant!");
  // assert(copy <= (HeapWord*)p, "Invariant!");

  assert(copy != NULL, "Invariant!");
#ifdef RELEASE_DEBUG
  if (copy == NULL) {
    // Modified by Shi
    ShouldNotReachHere();
  }
#endif

  // Copy the object and initialize its forwarding ptr:
  // Modified by Haoran2
  // HeapWord* copy = filler + ShenandoahBrooksPointer::word_size();
  oop copy_val = oop(copy);

  Copy::aligned_disjoint_words((HeapWord*) p, copy, size);
  oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  assert(oopDesc::equals_raw(result, copy_val), "Invariant!");

  // if(copy + size <= (HeapWord*)p) {
  //   Copy::aligned_disjoint_words((HeapWord*) p, copy, size);
  // }
  // else if(copy != (HeapWord*)p) {
  //   Copy::aligned_conjoint_words((HeapWord*) p, copy, size);
  // }

  // Try to install the new forwarding pointer.
  // oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  // assert(oopDesc::equals_raw(result, copy_val), "Invariant!");
  return copy_val;
}

inline void ShenandoahSemeruHeap::update_object(oop obj) {
  ShenandoahSemeruUpdateHeapRefsClosure update_cl(this);
  // Modified by Shi
  shenandoah_assert_in_heap(NULL, obj);
  if (obj->is_instance()) {
    obj->oop_iterate(&update_cl);
  } else if (obj->is_objArray()) {
    obj->oop_iterate(&update_cl);
  } else {
    assert (obj->is_typeArray(), "should be type array");
  }
}

template<bool RESOLVE>
inline bool ShenandoahSemeruHeap::requires_marking(const void* entry) const {
  oop obj = oop(entry);
  if (RESOLVE) {
    obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
  }
  return !_marking_context->is_marked(obj);
}

template <class T>
inline bool ShenandoahSemeruHeap::in_update_set(T p) const {
  HeapWord* obj = (HeapWord*) p;
  assert(collection_set() != NULL, "Sanity");
  assert(is_in(obj), "should be in heap");

  return collection_set()->is_in_update_set(obj);
}

// template <class T>
// inline bool ShenandoahSemeruHeap::in_collection_set(T p) const {
//   HeapWord* obj = (HeapWord*) p;
//   assert(collection_set() != NULL, "Sanity");
//   assert(is_in(obj), "should be in heap");

//   return collection_set()->is_in(obj);
// }

inline bool ShenandoahSemeruHeap::is_stable() const {
  return _gc_state.is_clear();
}

inline bool ShenandoahSemeruHeap::is_idle() const {
  return _gc_state.is_unset(MARKING | EVACUATION | UPDATEREFS | TRAVERSAL);
}

inline bool ShenandoahSemeruHeap::is_concurrent_mark_in_progress() const {
  return _gc_state.is_set(MARKING);
}

inline bool ShenandoahSemeruHeap::is_concurrent_traversal_in_progress() const {
  return _gc_state.is_set(TRAVERSAL);
}

inline bool ShenandoahSemeruHeap::is_evacuation_in_progress() const {
  return _gc_state.is_set(EVACUATION);
}

inline bool ShenandoahSemeruHeap::is_gc_in_progress_mask(uint mask) const {
  return _gc_state.is_set(mask);
}

// inline bool ShenandoahSemeruHeap::is_degenerated_gc_in_progress() const {
//   return _degenerated_gc_in_progress.is_set();
// }

// inline bool ShenandoahSemeruHeap::is_full_gc_in_progress() const {
//   return _full_gc_in_progress.is_set();
// }

inline bool ShenandoahSemeruHeap::is_full_gc_move_in_progress() const {
  return _full_gc_move_in_progress.is_set();
}

inline bool ShenandoahSemeruHeap::is_update_refs_in_progress() const {
  return _gc_state.is_set(UPDATEREFS);
}

template<class T>
inline void ShenandoahSemeruHeap::marked_object_iterate(ShenandoahSemeruHeapRegion* region, T* cl) {
  marked_object_iterate(region, cl, region->top());
}

template<class T>
inline void ShenandoahSemeruHeap::marked_object_iterate(ShenandoahSemeruHeapRegion* region, T* cl, HeapWord* limit) {
  assert(! region->is_humongous_continuation(), "no humongous continuation regions here");

  ShenandoahMarkingContext* const ctx = complete_marking_context();
  // Modified by Haoran for compaction
  // assert(ctx->is_complete(), "sanity");

  MarkBitMap* mark_bit_map = ctx->mark_bit_map();
  HeapWord* tams = ctx->semeru_top_at_mark_start(region);

  // Modified by Haoran2
  // size_t skip_bitmap_delta = ShenandoahBrooksPointer::word_size() + 1;
  // size_t skip_objsize_delta = ShenandoahBrooksPointer::word_size() /* + actual obj.size() below */;
  // HeapWord* start = region->bottom() + ShenandoahBrooksPointer::word_size();
  // HeapWord* end = MIN2(tams + ShenandoahBrooksPointer::word_size(), region->end());
  size_t skip_bitmap_delta = 1;
  HeapWord* start = region->bottom();
  HeapWord* end = MIN2(tams, region->end());

  // Step 1. Scan below the TAMS based on bitmap data.
  HeapWord* limit_bitmap = MIN2(limit, tams);

  // Try to scan the initial candidate. If the candidate is above the TAMS, it would
  // fail the subsequent "< limit_bitmap" checks, and fall through to Step 2.
  HeapWord* cb = mark_bit_map->get_next_marked_addr(start, end);

  intx dist = ShenandoahMarkScanPrefetch;
  if (dist > 0) {
    // Batched scan that prefetches the oop data, anticipating the access to
    // either header, oop field, or forwarding pointer. Not that we cannot
    // touch anything in oop, while it still being prefetched to get enough
    // time for prefetch to work. This is why we try to scan the bitmap linearly,
    // disregarding the object size. However, since we know forwarding pointer
    // preceeds the object, we can skip over it. Once we cannot trust the bitmap,
    // there is no point for prefetching the oop contents, as oop->size() will
    // touch it prematurely.

    // No variable-length arrays in standard C++, have enough slots to fit
    // the prefetch distance.
    static const int SLOT_COUNT = 256;
    guarantee(dist <= SLOT_COUNT, "adjust slot count");
    HeapWord* slots[SLOT_COUNT];

    int avail;
    do {
      avail = 0;
      for (int c = 0; (c < dist) && (cb < limit_bitmap); c++) {
        // Modified by Haoran2
        // Prefetch::read(cb, ShenandoahBrooksPointer::byte_offset());
        Prefetch::read(cb, oopDesc::mark_offset_in_bytes());
        slots[avail++] = cb;
        cb += skip_bitmap_delta;
        if (cb < limit_bitmap) {
          cb = mark_bit_map->get_next_marked_addr(cb, limit_bitmap);
        }
      }

      for (int c = 0; c < avail; c++) {
        assert (slots[c] < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(tams));
        assert (slots[c] < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(limit));
        oop obj = oop(slots[c]);
        // Modified by Haoran for compaction
        // assert(oopDesc::is_oop(obj), "sanity");
        assert(is_in(obj), "sanity");
        assert(ctx->is_marked(obj), "object expected to be marked");
        // Modified by Haoran
        // Haoran: warning, may cause bugs
        assert(ShenandoahForwarding::get_forwardee_raw(obj) != NULL, "CHECK THIS!");
        cl->do_object(obj);
      }
    } while (avail > 0);
  } else {
    while (cb < limit_bitmap) {
      assert (cb < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(tams));
      assert (cb < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(limit));
      oop obj = oop(cb);
      // Modified by Haoran for compaction
      // assert(oopDesc::is_oop(obj), "sanity");
      assert(is_in(obj), "sanity");
      assert(ctx->is_marked(obj), "object expected to be marked");
      cl->do_object(obj);
      cb += skip_bitmap_delta;
      if (cb < limit_bitmap) {
        cb = mark_bit_map->get_next_marked_addr(cb, limit_bitmap);
      }
    }
  }

  // Step 2. Accurate size-based traversal, happens past the TAMS.
  // This restarts the scan at TAMS, which makes sure we traverse all objects,
  // regardless of what happened at Step 1.
  // Modified by Haoran2
  // HeapWord* cs = tams + ShenandoahBrooksPointer::word_size();
  HeapWord* cs = tams;
  while (cs < limit) {
    assert (cs >= tams,  "only objects past TAMS here: "   PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(tams));
    assert (cs < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(limit));
    oop obj = oop(cs);
    // Modified by Haoran for compaction
    // assert(oopDesc::is_oop(obj), "sanity");
    assert(is_in(obj), "sanity");

    assert(ctx->is_marked(obj), "object expected to be marked");
    int size = obj->size();
    cl->do_object(obj);
    cs += size;
  }
}

template <class T>
class ShenandoahSemeruObjectToOopClosure : public ObjectClosure {
  T* _cl;
public:
  ShenandoahSemeruObjectToOopClosure(T* cl) : _cl(cl) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl);
  }
};

template <class T>
class ShenandoahSemeruObjectToOopBoundedClosure : public ObjectClosure {
  T* _cl;
  MemRegion _bounds;
public:
  ShenandoahSemeruObjectToOopBoundedClosure(T* cl, HeapWord* bottom, HeapWord* top) :
    _cl(cl), _bounds(bottom, top) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl, _bounds);
  }
};

template<class T>
inline void ShenandoahSemeruHeap::marked_object_oop_iterate(ShenandoahSemeruHeapRegion* region, T* cl, HeapWord* top) {
  if (region->is_humongous()) {
    HeapWord* bottom = region->bottom();
    if (top > bottom) {
      region = region->humongous_start_region();
      ShenandoahSemeruObjectToOopBoundedClosure<T> objs(cl, bottom, top);
      marked_object_iterate(region, &objs);
    }
  } else {
    ShenandoahSemeruObjectToOopClosure<T> objs(cl);
    marked_object_iterate(region, &objs, top);
  }
}

// inline ShenandoahSemeruHeapRegion* const ShenandoahSemeruHeap::get_region(size_t region_idx) const {
//   if (region_idx < _num_regions) {
//     return _regions[region_idx];
//   } else {
//     return NULL;
//   }
// }

inline void ShenandoahSemeruHeap::mark_complete_marking_context() {
  _marking_context->mark_complete();
}

inline void ShenandoahSemeruHeap::mark_incomplete_marking_context() {
  _marking_context->mark_incomplete();
}

// inline ShenandoahMarkingContext* ShenandoahSemeruHeap::complete_marking_context() const {
//   assert (_marking_context->is_complete()," sanity");
//   return _marking_context;
// }

// inline ShenandoahMarkingContext* ShenandoahSemeruHeap::marking_context() const {
//   return _marking_context;
// }

#endif // SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAP_INLINE_HPP
