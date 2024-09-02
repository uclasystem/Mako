/*
 * Copyright (c) 2013, 2019, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahBarrierSetAssembler.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahTraversalGC.hpp"
#include "memory/iterator.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#ifdef COMPILER1
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/shenandoah/c2/shenandoahBarrierSetC2.hpp"
#endif

#include "gc/shenandoah/shenandoahUtils.hpp"

class ShenandoahBarrierSetC1;
class ShenandoahBarrierSetC2;

template <bool STOREVAL_EVAC_BARRIER>
class ShenandoahUpdateRefsForOopClosure: public BasicOopIterateClosure {
private:
  ShenandoahHeap* _heap;
  ShenandoahBarrierSet* _bs;

  template <class T>
  inline void do_oop_work(T* p) {
    oop o;
    if (STOREVAL_EVAC_BARRIER) {
      // Haoran: debug
      ShouldNotReachHere();
      o = _heap->evac_update_with_forwarded(p);
      if (!CompressedOops::is_null(o)) {
        _bs->enqueue(o);
      }
    } else {

      T o = RawAccess<>::oop_load(p);
      if (!CompressedOops::is_null(o)) {
        oop obj = CompressedOops::decode_not_null(o);
        if(_heap->_cpu_server_flags->_should_start_update == true) {
          assert(!_heap->in_evac_set((HeapWord*)obj), "wrong invariant");
#ifdef RELEASE_CHECK
          if(_heap->in_evac_set((HeapWord*)obj)) {
            log_debug(semeru)("cloned during conc update and has ref point to evac set obj: 0x%lx", (size_t)obj);
            ShouldNotReachHere();
          }
#endif
        } else {

          if(_heap->in_evac_set((HeapWord*)obj)) {
            _heap->wait_region(obj, Thread::current());
            log_debug(semeru)("cloned during evac and has ref point to evac set obj: 0x%lx", (size_t)obj);
            _heap->semeru_maybe_update_with_forwarded(p);
          }
          // _heap->maybe_update_with_forwarded(p);
        }
      }
    }
  }
public:
  ShenandoahUpdateRefsForOopClosure() : _heap(ShenandoahHeap::heap()), _bs(ShenandoahBarrierSet::barrier_set()) {
    assert(UseShenandoahGC && ShenandoahCloneBarrier, "should be enabled");
  }

  virtual void do_oop(oop* p)       { do_oop_work(p); }
  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
};

ShenandoahBarrierSet::ShenandoahBarrierSet(ShenandoahHeap* heap) :
  BarrierSet(make_barrier_set_assembler<ShenandoahBarrierSetAssembler>(),
             make_barrier_set_c1<ShenandoahBarrierSetC1>(),
             make_barrier_set_c2<ShenandoahBarrierSetC2>(),
             NULL /* barrier_set_nmethod */,
             BarrierSet::FakeRtti(BarrierSet::ShenandoahBarrierSet)),
  _heap(heap),
  _satb_mark_queue_set()
{
}

ShenandoahBarrierSetAssembler* ShenandoahBarrierSet::assembler() {
  BarrierSetAssembler* const bsa = BarrierSet::barrier_set()->barrier_set_assembler();
  return reinterpret_cast<ShenandoahBarrierSetAssembler*>(bsa);
}

void ShenandoahBarrierSet::print_on(outputStream* st) const {
  st->print("ShenandoahBarrierSet");
}

bool ShenandoahBarrierSet::is_a(BarrierSet::Name bsn) {
  return bsn == BarrierSet::ShenandoahBarrierSet;
}

bool ShenandoahBarrierSet::is_aligned(HeapWord* hw) {
  return true;
}

template <class T, bool STOREVAL_EVAC_BARRIER>
void ShenandoahBarrierSet::write_ref_array_loop(HeapWord* start, size_t count) {
  ShouldNotCallThis();
  assert(UseShenandoahGC && ShenandoahCloneBarrier, "should be enabled");
  ShenandoahUpdateRefsForOopClosure<STOREVAL_EVAC_BARRIER> cl;
  T* dst = (T*) start;
  for (size_t i = 0; i < count; i++) {
    cl.do_oop(dst++);
  }
}

void ShenandoahBarrierSet::write_ref_array(HeapWord* start, size_t count) {
  ShouldNotCallThis();
  assert(_heap->is_update_refs_in_progress(), "should not be here otherwise");
  assert(count > 0, "Should have been filtered before");

  if (_heap->is_concurrent_traversal_in_progress()) {
    ShouldNotReachHere();
    ShenandoahEvacOOMScope oom_evac_scope;
    if (UseCompressedOops) {
      write_ref_array_loop<narrowOop, /* evac = */ true>(start, count);
    } else {
      write_ref_array_loop<oop,       /* evac = */ true>(start, count);
    }
  } else {
    if (UseCompressedOops) {
      write_ref_array_loop<narrowOop, /* evac = */ false>(start, count);
    } else {
      write_ref_array_loop<oop,       /* evac = */ false>(start, count);
    }
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  shenandoah_assert_not_in_cset_loc_except(dst, _heap->cancelled_gc());
  assert(ShenandoahThreadLocalData::satb_mark_queue(Thread::current()).is_active(), "Shouldn't be here otherwise");
  assert(ShenandoahSATBBarrier, "Shouldn't be here otherwise");
  assert(count > 0, "Should have been filtered before");

  Thread* thread = Thread::current();
  ShenandoahMarkingContext* ctx = _heap->marking_context();
  bool has_forwarded = _heap->has_forwarded_objects();
  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      if (has_forwarded) {
        ShouldNotReachHere();
        obj = resolve_forwarded_not_null(obj);
      }
      if (!ctx->is_marked(obj)) {
        ShenandoahThreadLocalData::satb_mark_queue(thread).enqueue_known_active(obj);
      }
    }
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

template <class T>
inline void ShenandoahBarrierSet::inline_write_ref_field_pre(T* field, oop new_val) {
  shenandoah_assert_not_in_cset_loc_except(field, _heap->cancelled_gc());
  if (_heap->is_concurrent_mark_in_progress()) {
    T heap_oop = RawAccess<>::oop_load(field);
    if (!CompressedOops::is_null(heap_oop)) {
      enqueue(CompressedOops::decode(heap_oop));
    }
  }
}

// These are the more general virtual versions.
void ShenandoahBarrierSet::write_ref_field_pre_work(oop* field, oop new_val) {
  inline_write_ref_field_pre(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(narrowOop* field, oop new_val) {
  inline_write_ref_field_pre(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(void* field, oop new_val) {
  guarantee(false, "Not needed");
}

void ShenandoahBarrierSet::write_ref_field_work(void* v, oop o, bool release) {
  shenandoah_assert_not_in_cset_loc_except(v, _heap->cancelled_gc());
  shenandoah_assert_not_forwarded_except  (v, o, o == NULL || _heap->cancelled_gc() || !_heap->is_concurrent_mark_in_progress());
  shenandoah_assert_not_in_cset_except    (v, o, o == NULL || _heap->cancelled_gc() || !_heap->is_concurrent_mark_in_progress());
}

void ShenandoahBarrierSet::write_region(MemRegion mr) {
  if (!ShenandoahCloneBarrier) return;
  // if (!_heap->is_update_refs_in_progress()) return;
  if (!_heap->has_forwarded_objects()) return;

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  oop obj = oop(mr.start());
#ifdef RELEASE_CHECK
  shenandoah_assert_correct(NULL, obj);
#endif


  ShenandoahHeap::heap()->wait_region(obj, Thread::current());

  ShenandoahUpdateRefsForOopClosure</* evac = */ false> cl;
  obj->oop_iterate(&cl);
}

oop ShenandoahBarrierSet::load_reference_barrier_not_null(oop obj) {
  if (ShenandoahLoadRefBarrier && _heap->has_forwarded_objects()) {
    return load_reference_barrier_impl(obj);
  } else {
    return obj;
  }
}

oop ShenandoahBarrierSet::load_reference_barrier(oop obj) {
  if (obj != NULL) {
    return load_reference_barrier_not_null(obj);
  } else {
    return obj;
  }
}


oop ShenandoahBarrierSet::load_reference_barrier_mutator(oop obj) {
  assert(ShenandoahLoadRefBarrier, "should be enabled");
  assert(_heap->is_gc_in_progress_mask(ShenandoahHeap::EVACUATION | ShenandoahHeap::TRAVERSAL), "evac should be in progress");
  // shenandoah_assert_in_cset(NULL, obj);

  // oop fwd = resolve_forwarded_not_null(obj);
  // if (oopDesc::equals_raw(obj, fwd)) {
  // ShenandoahEvacOOMScope oom_evac_scope;

  Thread* thread = Thread::current();
  // oop res_oop = _heap->evacuate_object(obj, thread);
  oop new_obj = _heap->process_region(obj, thread);
  // oop new_obj = _heap->wait_region(obj, thread);

  // assert(new_obj == obj, "Invariant!");
  assert(!_heap->in_evac_set(new_obj), "Invariant!");
#ifdef RELEASE_CHECK
  if(_heap->atomic_in_evac_set(new_obj)) {
    tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
    ShouldNotReachHere();
  }
  // if(new_obj != obj) {
  //   log_debug(semeru)("Alert!!!!!!!!!! new obj: 0x%lx obj: 0x%lx", (size_t)new_obj, (size_t)obj);
  //   tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
  //   ShouldNotReachHere();
  // } 
#endif
  return new_obj;
}

void ShenandoahBarrierSet::load_reference_barrier_array_copy(oop* p) {
  // return;
  assert(ShenandoahLoadRefBarrier, "should be enabled");
  assert(_heap->is_gc_in_progress_mask(ShenandoahHeap::EVACUATION | ShenandoahHeap::TRAVERSAL), "evac should be in progress");
  oop obj = oop(p);
  _heap->wait_region(obj, Thread::current());
  // if(!_heap->is_in((HeapWord*)obj)) {
  //   size_t index = 0;
  //   if(!_heap->is_evacuation_in_progress()) return;
  //   if(_heap->_cpu_server_flags->_should_start_update == false) {

  //     while(index < _heap->num_regions()) {
  //       if(_heap->collection_set()->is_in_evac_set(index)) {
  //         while(!_heap->collection_set()->is_evac_finished(index)) {
  //           os::naked_short_sleep(5);
  //         }
  //       }
  //       index ++;
  //     }
  //     return;
  //   } else {
  //     while(index < _heap->num_regions()) {
  //       if(_heap->collection_set()->is_in_update_set(index)) {
  //         while(!_heap->collection_set()->is_update_finished(index)) {
  //           os::naked_short_sleep(5);
  //         }
  //       }
  //       index ++;
  //     }
  //     return;
  //   }
  // } else{
  //   size_t index = _heap->heap_region_index_containing((HeapWord*)obj);
  //   if(!_heap->is_evacuation_in_progress()) return;
  //   if(_heap->_cpu_server_flags->_should_start_update == false) {
  //     if(!_heap->collection_set()->is_in_evac_set(index)) {
  //       return;
  //     }
  //     // ShouldNotReachHere();
  //     log_debug(semeru)("Array copy src/dst is in evac set! obj: 0x%lx", (size_t)obj);
  //     while(!_heap->collection_set()->is_evac_finished(index)) {
  //       os::naked_short_sleep(5);
  //     }
  //   } else {
  //     if(!_heap->collection_set()->is_in_update_set(index)) return;
  //     while(!_heap->collection_set()->is_update_finished(index)) {
  //       os::naked_short_sleep(5);
  //     }
  //   }
  // }
}

oop ShenandoahBarrierSet::load_reference_barrier_impl(oop obj) {
  assert(ShenandoahLoadRefBarrier, "should be enabled");
  // if (!CompressedOops::is_null(obj)) {
    bool evac_in_progress = _heap->is_gc_in_progress_mask(ShenandoahHeap::EVACUATION | ShenandoahHeap::TRAVERSAL);
    oop new_obj = obj;
    // if (evac_in_progress &&
    //     _heap->in_update_set(obj)) {
    if (evac_in_progress) {
      //     if(!_heap->in_update_set(obj)) {
      //   log_debug(semeru)("Not in update set! obj: 0x%lx", (size_t)obj);
      //   ShouldNotReachHere();
      // }
      Thread *t = Thread::current();
      new_obj = _heap->wait_region(obj, t);
    } 
#ifdef RELEASE_CHECK
    if(_heap->atomic_in_evac_set(new_obj)) {
      tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
      ShouldNotReachHere();
    }
    // if(new_obj != obj) {
    //   log_debug(semeru)("Alert!!!!!!!!!! new obj: 0x%lx obj: 0x%lx", (size_t)new_obj, (size_t)obj);
    //   tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
    //   ShouldNotReachHere();
    // } 
#endif
    return new_obj;     
  // } else {
  //   return obj;
  // }
}

oop ShenandoahBarrierSet::load_reference_barrier_stack_val(oop obj) {
#ifdef RELEASE_CHECK
  if(ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    tty->print("Cannot be at safepoint!");
    ShouldNotReachHere();
  }
#endif
  if (!_heap->has_forwarded_objects() || obj == NULL) {
    return obj;
  }
  oop new_obj = obj;
  assert(ShenandoahLoadRefBarrier, "should be enabled");
  bool evac_in_progress = _heap->is_gc_in_progress_mask(ShenandoahHeap::EVACUATION | ShenandoahHeap::TRAVERSAL);
#ifdef RELEASE_CHECK
  if(!evac_in_progress) {
    tty->print("??????");
    ShouldNotReachHere();
  }
#endif
  Thread *t = Thread::current();
  new_obj = _heap->wait_region(obj, t);
#ifdef RELEASE_CHECK
  if(_heap->atomic_in_evac_set(new_obj)) {
    tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
    ShouldNotReachHere();
  }
  // if(new_obj != obj) {
  //   log_debug(semeru)("Alert!!!!!!!!!! new obj: 0x%lx obj: 0x%lx", (size_t)new_obj, (size_t)obj);
  //   tty->print("new obj: 0x%lx obj: 0x%lx, old_region: %lu, new_region: %lu, in cset: %d", (size_t)new_obj, (size_t)obj, _heap->heap_region_index_containing(obj),_heap->heap_region_index_containing(new_obj), _heap->atomic_in_evac_set(new_obj));
  //   ShouldNotReachHere();
  // } 
#endif
  return new_obj;     
}

void ShenandoahBarrierSet::storeval_barrier(oop obj) {
  if (ShenandoahStoreValEnqueueBarrier && !CompressedOops::is_null(obj) && _heap->is_concurrent_traversal_in_progress()) {
    enqueue(obj);
  }
}

void ShenandoahBarrierSet::keep_alive_barrier(oop obj) {
  if (ShenandoahKeepAliveBarrier && _heap->is_concurrent_mark_in_progress()) {
    enqueue(obj);
  }
}

void ShenandoahBarrierSet::enqueue(oop obj) {
  shenandoah_assert_not_forwarded_if(NULL, obj, _heap->is_concurrent_traversal_in_progress());
  assert(_satb_mark_queue_set.is_active(), "only get here when SATB active");

  // Filter marked objects before hitting the SATB queues. The same predicate would
  // be used by SATBMQ::filter to eliminate already marked objects downstream, but
  // filtering here helps to avoid wasteful SATB queueing work to begin with.
  if (!_heap->requires_marking<false>(obj)) return;

  ShenandoahThreadLocalData::satb_mark_queue(Thread::current()).enqueue_known_active(obj);
}

void ShenandoahBarrierSet::on_thread_create(Thread* thread) {
  // Create thread local data
  ShenandoahThreadLocalData::create(thread);
}

void ShenandoahBarrierSet::on_thread_destroy(Thread* thread) {
  // Destroy thread local data
  ShenandoahThreadLocalData::destroy(thread);
}

void ShenandoahBarrierSet::on_thread_attach(Thread *thread) {
  assert(!thread->is_Java_thread() || !SafepointSynchronize::is_at_safepoint(),
         "We should not be at a safepoint");
  SATBMarkQueue& queue = ShenandoahThreadLocalData::satb_mark_queue(thread);
  assert(!queue.is_active(), "SATB queue should not be active");
  assert( queue.is_empty(),  "SATB queue should be empty");
  queue.set_active(_satb_mark_queue_set.is_active());
  if (thread->is_Java_thread()) {
    ShenandoahThreadLocalData::set_gc_state(thread, _heap->gc_state());
    ShenandoahThreadLocalData::initialize_gclab(thread);
  }
}

void ShenandoahBarrierSet::on_thread_detach(Thread *thread) {
  SATBMarkQueue& queue = ShenandoahThreadLocalData::satb_mark_queue(thread);
  queue.flush();
  if (thread->is_Java_thread()) {
    PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
    if (gclab != NULL) {
      gclab->retire();
    }
  }
}
