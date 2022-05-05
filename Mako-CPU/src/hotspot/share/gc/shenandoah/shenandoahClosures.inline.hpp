/*
 * Copyright (c) 2019, Red Hat, Inc. All rights reserved.
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
#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHCLOSURES_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHCLOSURES_INLINE_HPP

#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahClosures.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "runtime/thread.hpp"

ShenandoahForwardedIsAliveClosure::ShenandoahForwardedIsAliveClosure() :
  _mark_context(ShenandoahHeap::heap()->marking_context()) {
}

bool ShenandoahForwardedIsAliveClosure::do_object_b(oop obj) {
  if (CompressedOops::is_null(obj)) {
    return false;
  }
  obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
  shenandoah_assert_not_forwarded_if(NULL, obj,
                                     (ShenandoahHeap::heap()->is_concurrent_mark_in_progress() ||
                                     ShenandoahHeap::heap()->is_concurrent_traversal_in_progress()));
  return _mark_context->is_marked(obj);
}

ShenandoahIsAliveClosure::ShenandoahIsAliveClosure() :
  _mark_context(ShenandoahHeap::heap()->marking_context()) {
}

bool ShenandoahIsAliveClosure::do_object_b(oop obj) {
  if (CompressedOops::is_null(obj)) {
    return false;
  }
  shenandoah_assert_not_forwarded(NULL, obj);
  return _mark_context->is_marked(obj);
}

BoolObjectClosure* ShenandoahIsAliveSelector::is_alive_closure() {
  return ShenandoahHeap::heap()->has_forwarded_objects() ?
         reinterpret_cast<BoolObjectClosure*>(&_fwd_alive_cl) :
         reinterpret_cast<BoolObjectClosure*>(&_alive_cl);
}

ShenandoahUpdateRefsClosure::ShenandoahUpdateRefsClosure() :
  _heap(ShenandoahHeap::heap()) {
}

template <class T>
void ShenandoahUpdateRefsClosure::do_oop_work(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    _heap->update_with_forwarded_not_null(p, obj);
  }
}

void ShenandoahUpdateRefsClosure::do_oop(oop* p)       { do_oop_work(p); }
void ShenandoahUpdateRefsClosure::do_oop(narrowOop* p) { do_oop_work(p); }

ShenandoahEvacuateUpdateRootsClosure::ShenandoahEvacuateUpdateRootsClosure(size_t worker_id) :
  _heap(ShenandoahHeap::heap()), _thread(Thread::current()), _worker_id(worker_id) {
}

template <class T>
void ShenandoahEvacuateUpdateRootsClosure::do_oop_work(T* p) {
  assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

  T o = RawAccess<>::oop_load(p);
  if (! CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    assert(Universe::heap()->is_in(obj), "wrong invariant");

    // if (_heap->in_collection_set(obj)) 
    if (_heap->in_evac_set(obj)) {
      shenandoah_assert_marked(p, obj);
      // if(!_heap->marking_context()->is_marked(obj)) {
      //   for(size_t id = 0; id < _heap->root_object_queue()->length(); id++) {
      //     if((HeapWord*)obj == _heap->root_object_queue()->retrieve_item(id)) {
      //       tty->print("Found obj in root object queue: 0x%lx not marked!\n", (size_t)obj);
      //       break;
      //     }
      //   }
      //   tty->print("Obj: 0x%lx not marked!\n", (size_t)obj);
      // }
#ifdef RELEASE_CHECK
      if(!_heap->marking_context()->is_marked(obj)) {
        tty->print("Obj: 0x%lx not marked!\n", (size_t)obj);
        // ShouldNotReachHere();
      }
#endif
      // if()
      // oop resolved = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      oop resolved = ShenandoahForwarding::get_forwardee(obj);
      if (oopDesc::equals_raw(resolved, obj)) {
        resolved = _heap->evacuate_root(obj, _thread, _worker_id);

        // _heap->root_object_queue()->push(resolved);
        _heap->heap_region_containing(resolved)->_selected_to = true;
      }
      assert(Universe::heap()->is_in(resolved), "wrong invariant");

      assert(resolved != NULL, "Invariant!");
      assert(_heap->heap_region_containing(resolved) != _heap->heap_region_containing(obj)  , "Invariant!");

      RawAccess<IS_NOT_NULL>::oop_store(p, resolved);
      
    }
    else {
      _heap->root_object_update_queue()->push(obj, _worker_id);

      _heap->collection_set()->add_region_to_local_set(_heap->heap_region_index_containing(obj));
    }
  }
}
void ShenandoahEvacuateUpdateRootsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

void ShenandoahEvacuateUpdateRootsClosure::do_oop(narrowOop* p) {
  do_oop_work(p);
}


ShenandoahSemeruUpdateRootsClosure::ShenandoahSemeruUpdateRootsClosure() :
  _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
}

template <class T>
void ShenandoahSemeruUpdateRootsClosure::do_oop_work(T* p) {
  // ShouldNotCallThis();
  assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

  T o = RawAccess<>::oop_load(p);
  if (! CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    assert(Universe::heap()->is_in(obj), "wrong invariant");
    
    // shenandoah_assert_marked(p, obj);
    assert(!_heap->in_evac_set(obj), "Should have been evacuated and updated!");
#ifdef RELEASE_CHECK
    if(_heap->in_evac_set(obj)) {
      ShouldNotReachHere();
    }
#endif
    _heap->update_object(obj);
    
    // if(!r->_selected_to && !_heap->collection_set()->is_in_local_update_set(r->region_number())) {
    //   _heap->collection_set()->add_region_to_local_set(r->region_number());
    // }
    // HeapWord* to = _heap->heap_region_containing(obj)->offset_table()->get((HeapWord*)obj);
    // HeapWord* to = (HeapWord*)_heap->alive_table()->get_target_address(obj);
    // assert(to != NULL, "Invariant!");
    // assert(_heap->heap_region_containing(to) != _heap->heap_region_containing(obj)  , "Invariant!");
    // assert((HeapWord*)to <= (HeapWord*)obj, "Invariant!!!");
    // RawAccess<IS_NOT_NULL>::oop_store(p, (oop)to);
    
    
    // oop forwarded_oop = ShenandoahForwarding::get_forwardee(obj);
    // if(forwarded_oop != obj){
    //   RawAccess<IS_NOT_NULL>::oop_store(p, forwarded_oop);
    //   _heap->heap_region_containing(obj)->offset_table()->subtract_root_offset((HeapWord*)obj); // Might be multithreaded.
    // }
    // size_t old_mark = (size_t)forwarded_oop->mark_raw();
    // size_t new_mark = (old_mark | (1<<7));
    // if(old_mark != new_mark) {
    //   size_t result = Atomic::cmpxchg(new_mark, (size_t*)(forwarded_oop->mark_addr_raw()), old_mark);
    //   if(result == old_mark) {
    //     _heap->update_root(forwarded_oop);
    //   }
    // }
  }
}
void ShenandoahSemeruUpdateRootsClosure::do_oop(oop* p) {
  do_oop_work(p);
}

void ShenandoahSemeruUpdateRootsClosure::do_oop(narrowOop* p) {
  do_oop_work(p);
}


ShenandoahSemeruClearBitClosure::ShenandoahSemeruClearBitClosure() :
  _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
}

template <class T>
void ShenandoahSemeruClearBitClosure::do_oop_work(T* p) {
  assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

  T o = RawAccess<>::oop_load(p);
  if (! CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    assert(Universe::heap()->is_in(obj), "wrong invariant");
    assert(_heap->in_update_set(obj), "Invariant, otherwise no need to update!");
    
    shenandoah_assert_marked(p, obj);
    
    size_t old_mark = (size_t)obj->mark_raw();
    size_t new_mark = (old_mark & (~(1<<7)));
    if(old_mark != new_mark) {
      obj->set_mark_raw((markOop)new_mark);
    }
  }
}
void ShenandoahSemeruClearBitClosure::do_oop(oop* p) {
  do_oop_work(p);
}

void ShenandoahSemeruClearBitClosure::do_oop(narrowOop* p) {
  do_oop_work(p);
}

#ifdef ASSERT
template <class T>
void ShenandoahAssertNotForwardedClosure::do_oop_work(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    shenandoah_assert_not_forwarded(p, obj);
  }
}

void ShenandoahAssertNotForwardedClosure::do_oop(narrowOop* p) { do_oop_work(p); }
void ShenandoahAssertNotForwardedClosure::do_oop(oop* p)       { do_oop_work(p); }
#endif

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHCLOSURES_HPP
