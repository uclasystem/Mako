// Modified by Haoran
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

#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"

#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/referenceProcessorPhaseTimes.hpp"

#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahMarkCompact.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahTimingTracker.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"

#include "memory/iterator.inline.hpp"
#include "memory/metaspace.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"

template<UpdateRefsMode UPDATE_REFS>
class ShenandoahSemeruInitMarkRootsClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahSemeruHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    // Modified by Haoran
    ShouldNotReachHere();
    ShenandoahSemeruConcurrentMark::mark_through_ref<T, UPDATE_REFS, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahSemeruInitMarkRootsClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()) {};

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
  void semeru_ms_do_oop(oop obj, narrowOop* p) { do_oop_work(p); }
  void semeru_ms_do_oop(oop obj,       oop* p) { do_oop_work(p); }
};

ShenandoahSemeruMarkRefsSuperClosure::ShenandoahSemeruMarkRefsSuperClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
  MetadataVisitingOopIterateClosure(rp),
  _queue(q),
  _heap(ShenandoahSemeruHeap::heap()),
  _mark_context(_heap->marking_context())
{ }

template<UpdateRefsMode UPDATE_REFS>
class ShenandoahSemeruInitMarkRootsTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;
  bool _process_refs;
public:
  ShenandoahSemeruInitMarkRootsTask(ShenandoahRootProcessor* rp, bool process_refs) :
    AbstractGangTask("Shenandoah init mark roots task"),
    _rp(rp),
    _process_refs(process_refs) {
  }

  void work(uint worker_id) {
    ShouldNotReachHere();
    // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
    // ShenandoahParallelWorkerSession worker_session(worker_id);

    // ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
    // ShenandoahObjToScanQueueSet* queues = heap->concurrent_mark()->task_queues();
    // assert(queues->get_reserved() > worker_id, "Queue has not been reserved for worker id: %d", worker_id);

    // ShenandoahObjToScanQueue* q = queues->queue(worker_id);

    // ShenandoahInitMarkRootsClosure<UPDATE_REFS> mark_cl(q);
    // do_work(heap, &mark_cl, worker_id);
  }

private:
  void do_work(ShenandoahSemeruHeap* heap, OopClosure* oops, uint worker_id) {
    // The rationale for selecting the roots to scan is as follows:
    //   a. With unload_classes = true, we only want to scan the actual strong roots from the
    //      code cache. This will allow us to identify the dead classes, unload them, *and*
    //      invalidate the relevant code cache blobs. This could be only done together with
    //      class unloading.
    //   b. With unload_classes = false, we have to nominally retain all the references from code
    //      cache, because there could be the case of embedded class/oop in the generated code,
    //      which we will never visit during mark. Without code cache invalidation, as in (a),
    //      we risk executing that code cache blob, and crashing.
    //   c. With ShenandoahConcurrentScanCodeRoots, we avoid scanning the entire code cache here,
    //      and instead do that in concurrent phase under the relevant lock. This saves init mark
    //      pause time.

    // Modified by Haoran2
    ShouldNotReachHere();
//     CLDToOopClosure clds_cl(oops, ClassLoaderData::_claim_strong);
//     MarkingCodeBlobClosure blobs_cl(oops, ! CodeBlobToOopClosure::FixRelocations);
//     OopClosure* weak_oops = _process_refs ? NULL : oops;

//     ResourceMark m;
//     if (heap->unload_classes()) {
//       _rp->process_strong_roots(oops, weak_oops, &clds_cl, NULL, &blobs_cl, NULL, worker_id);
//     } else {
//       if (ShenandoahConcurrentScanCodeRoots) {
//         CodeBlobClosure* code_blobs = NULL;
// #ifdef ASSERT
//         ShenandoahAssertToSpaceClosure assert_to_space_oops;
//         CodeBlobToOopClosure assert_to_space(&assert_to_space_oops, !CodeBlobToOopClosure::FixRelocations);
//         // If conc code cache evac is disabled, code cache should have only to-space ptrs.
//         // Otherwise, it should have to-space ptrs only if mark does not update refs.
//         if (!heap->has_forwarded_objects()) {
//           code_blobs = &assert_to_space;
//         }
// #endif
//         _rp->process_all_roots(oops, weak_oops, &clds_cl, code_blobs, NULL, worker_id);
//       } else {
//         _rp->process_all_roots(oops, weak_oops, &clds_cl, &blobs_cl, NULL, worker_id);
//       }
//     }
  }
};

class ShenandoahSemeruUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;
  const bool _update_code_cache;
public:
  ShenandoahSemeruUpdateRootsTask(ShenandoahRootProcessor* rp, bool update_code_cache) :
    AbstractGangTask("Shenandoah update roots task"),
    _rp(rp),
    _update_code_cache(update_code_cache) {
  }

  void work(uint worker_id) {
    // Modified by Haoran2
    ShouldNotReachHere();
//     assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
//     ShenandoahParallelWorkerSession worker_session(worker_id);

//     ShenandoahHeap* heap = ShenandoahHeap::heap();
//     ShenandoahUpdateRefsClosure cl;
//     CLDToOopClosure cldCl(&cl, ClassLoaderData::_claim_strong);

//     CodeBlobClosure* code_blobs;
//     CodeBlobToOopClosure update_blobs(&cl, CodeBlobToOopClosure::FixRelocations);
// #ifdef ASSERT
//     ShenandoahAssertToSpaceClosure assert_to_space_oops;
//     CodeBlobToOopClosure assert_to_space(&assert_to_space_oops, !CodeBlobToOopClosure::FixRelocations);
// #endif
//     if (_update_code_cache) {
//       code_blobs = &update_blobs;
//     } else {
//       code_blobs =
//         DEBUG_ONLY(&assert_to_space)
//         NOT_DEBUG(NULL);
//     }
//     _rp->process_all_roots(&cl, &cl, &cldCl, code_blobs, NULL, worker_id);
  }
};

class ShenandoahSemeruConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahSemeruConcurrentMark* _cm;
  ShenandoahTaskTerminator* _terminator;

public:
  ShenandoahSemeruConcurrentMarkingTask(ShenandoahSemeruConcurrentMark* cm, ShenandoahTaskTerminator* terminator) :
    AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahSuspendibleThreadSetJoiner stsj(ShenandoahSuspendibleWorkers);
    ShenandoahObjToScanQueue* q = _cm->get_queue(worker_id);
    ReferenceProcessor* rp;
    if (heap->process_references()) {
      rp = heap->ref_processor();
      shenandoah_assert_rp_isalive_installed();
    } else {
      rp = NULL;
    }

    _cm->concurrent_scan_code_roots(worker_id, rp);
    _cm->mark_loop(worker_id, _terminator, rp,
                   true, // cancellable
                   ShenandoahStringDedup::is_enabled()); // perform string dedup
  }
};

class ShenandoahSemeruSATBThreadsClosure : public ThreadClosure {
private:
  ShenandoahSemeruSATBBufferClosure* _satb_cl;
  int _thread_parity;

public:
  ShenandoahSemeruSATBThreadsClosure(ShenandoahSemeruSATBBufferClosure* satb_cl) :
    _satb_cl(satb_cl),
    // Modified by Haoran2
    // _thread_parity(Threads::thread_claim_parity()) {}
    _thread_parity(Threads::thread_claim_token()) {}
  void do_thread(Thread* thread) {
    // Modified by Haoran2
    if (thread->claim_threads_do(true, _thread_parity)) {
      ShenandoahThreadLocalData::satb_mark_queue(thread).apply_closure_and_empty(_satb_cl);
    }
    // if (thread->is_Java_thread()) {
    //   if (thread->claim_oops_do(true, _thread_parity)) {
    //     JavaThread* jt = (JavaThread*)thread;
    //     ShenandoahThreadLocalData::satb_mark_queue(jt).apply_closure_and_empty(_satb_cl);
    //   }
    // } else if (thread->is_VM_thread()) {
    //   if (thread->claim_oops_do(true, _thread_parity)) {
    //     ShenandoahBarrierSet::satb_mark_queue_set().shared_satb_queue()->apply_closure_and_empty(_satb_cl);
    //   }
    // }
  }
};

class ShenandoahSemeruFinalMarkingTask : public AbstractGangTask {
private:
  ShenandoahSemeruConcurrentMark* _cm;
  ShenandoahTaskTerminator* _terminator;
  bool _dedup_string;

public:
  ShenandoahSemeruFinalMarkingTask(ShenandoahSemeruConcurrentMark* cm, ShenandoahTaskTerminator* terminator, bool dedup_string) :
    AbstractGangTask("Shenandoah Final Marking"), _cm(cm), _terminator(terminator), _dedup_string(dedup_string) {
  }

  void work(uint worker_id) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();

    ShenandoahParallelWorkerSession worker_session(worker_id);
    // First drain remaining SATB buffers.
    // Notice that this is not strictly necessary for mark-compact. But since
    // it requires a StrongRootsScope around the task, we need to claim the
    // threads, and performance-wise it doesn't really matter. Adds about 1ms to
    // full-gc.
    {
      ShenandoahObjToScanQueue* q = _cm->get_queue(worker_id);
      ShenandoahSemeruSATBBufferClosure cl(q);
      SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();
      while (satb_mq_set.apply_closure_to_completed_buffer(&cl));
      ShenandoahSemeruSATBThreadsClosure tc(&cl);
      Threads::threads_do(&tc);
    }

    ReferenceProcessor* rp;
    if (heap->process_references()) {
      rp = heap->ref_processor();
      shenandoah_assert_rp_isalive_installed();
    } else {
      rp = NULL;
    }

    // Degenerated cycle may bypass concurrent cycle, so code roots might not be scanned,
    // let's check here.
    _cm->concurrent_scan_code_roots(worker_id, rp);
    _cm->mark_loop(worker_id, _terminator, rp,
                   false, // not cancellable
                   _dedup_string);

    assert(_cm->task_queues()->is_empty(), "Should be empty");
  }
};

void ShenandoahSemeruConcurrentMark::mark_roots(ShenandoahPhaseTimings::Phase root_phase) {
  // assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  ShenandoahGCPhase phase(root_phase);

  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  RootObjectQueue* roq = heap->root_object_queue(); // Haoran: TODO  build root obj queue
  oop obj;  // For semeru, we only yse non compressed oop.


  WorkGang* workers = heap->workers();
  uint nworkers = workers->active_workers();
  assert(nworkers <= task_queues()->size(), "Just check");

  // ShenandoahRootProcessor root_proc(heap, nworkers, root_phase);
  TASKQUEUE_STATS_ONLY(task_queues()->reset_taskqueue_stats());
  task_queues()->reserve(nworkers);


  // ShenandoahObjToScanQueue* q = task_queues()->queue(0);
  ShenandoahMarkingContext* mark_context = heap->marking_context();

  log_debug(semeru)("Root Object Queue Len: %lu", roq->length());
  size_t num_of_pushed_objs = 0;
  for(size_t i = 0; i < roq->length(); i ++) {

    if((size_t)roq->retrieve_item(i) == 0xffffffff) {
      ShouldNotReachHere();
      continue;
    }

    obj = (oop)(size_t)roq->retrieve_item(i);  // target object, addr before compaction
    //assert(_curr_region->is_in(obj) , "Wrong obj in Region[0x%lx]'s cross region ref queue.", (size_t)obj );
    // if(_curr_region->is_in(obj) == false){
    //   log_info(semeru,mem_trace)("Warning in %s, obj 0x%lx is NOT in current Region[0x%lx]", __func__, (size_t)(HeapWord*)obj, cross_region_ref_q->_region_index);
    //   return;
    // }
    //log_trace(semeru,mem_trace)("%s, get an obj 0x%lx from Region[0x%lx]->corss_region_ref_q ", __func__, (size_t)(HeapWord*)obj, cross_region_ref_q->_region_index );

    if(heap->get_server_id((HeapWord*)obj) != CUR_MEMORY_SERVER_ID) {
      ShouldNotReachHere();
      continue;
    }

    // if (!heap->is_in(obj)) {
    // log_debug(semeru)("Obj not in heap： 0x%lx", (size_t)obj);
    // ShouldNotReachHere();
    // }




    if (mark_context->mark(obj)) {
      assert(heap->is_in(obj), "Invariant!");
      // if(!heap->is_in(obj)) {
      //   ShouldNotReachHere();
      //   log_debug(semeru)("Obj not in heap: 0x%lx", (size_t)obj);
      // }
      Klass* obj_klass = obj->klass_or_null();
      // if (obj_klass == NULL || (size_t)obj_klass == 0xdeafbabedeafbabeULL) {
      //   log_debug(semeru)("Obj klass not right： 0x%lx, 0x%lx", (size_t)obj, (size_t)obj_klass);
      //   ShouldNotReachHere();
      // }
      ShenandoahObjToScanQueue* q = task_queues()->queue(i % nworkers);

      bool pushed = q->push(ShenandoahMarkTask(obj));
      assert(pushed, "overflow queue should always succeed pushing");
      num_of_pushed_objs++;
    }
    else {
      tty->print("%lu-th obj: 0x%lx, allocated after mark start: %d, tams: 0x%lx, is_markedL: %d, region_top: 0x%lx\n", i, (size_t)obj, mark_context->allocated_after_mark_start((HeapWord*)obj), (size_t)mark_context->semeru_top_at_mark_start(heap->heap_region_containing(obj)), mark_context->is_marked(obj), (size_t)heap->heap_region_containing(obj)->top());
      ShouldNotReachHere();
    }
  }
  log_debug(semeru)("Pushed Objs: %lu", num_of_pushed_objs);
  if(num_of_pushed_objs == 0) heap->_tracing_current_cycle_processing->set_byte_flag(false);
  log_debug(semeru)("after set byte flag Pushed Objs: %lu", num_of_pushed_objs);


  // if (heap->has_forwarded_objects()) {
  //   ShenandoahInitMarkRootsTask<RESOLVE> mark_roots(&root_proc, _heap->process_references());
  //   workers->run_task(&mark_roots);
  // } else {
  //   // No need to update references, which means the heap is stable.
  //   // Can save time not walking through forwarding pointers.
  //   ShenandoahInitMarkRootsTask<NONE> mark_roots(&root_proc, _heap->process_references());
  //   workers->run_task(&mark_roots);
  // }

  // if (ShenandoahConcurrentScanCodeRoots) {
  //   clear_claim_codecache();
  // }
}

void ShenandoahSemeruConcurrentMark::update_roots(ShenandoahPhaseTimings::Phase root_phase) {
  ShouldNotReachHere();
//   assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

//   bool update_code_cache = true; // initialize to safer value
//   switch (root_phase) {
//     case ShenandoahPhaseTimings::update_roots:
//     case ShenandoahPhaseTimings::final_update_refs_roots:
//       update_code_cache = false;
//       break;
//     case ShenandoahPhaseTimings::full_gc_roots:
//     case ShenandoahPhaseTimings::degen_gc_update_roots:
//       update_code_cache = true;
//       break;
//     default:
//       ShouldNotReachHere();
//   }

//   ShenandoahGCPhase phase(root_phase);

// #if defined(COMPILER2) || INCLUDE_JVMCI
//   DerivedPointerTable::clear();
// #endif

//   uint nworkers = _heap->workers()->active_workers();

//   ShenandoahRootProcessor root_proc(_heap, nworkers, root_phase);
//   ShenandoahUpdateRootsTask update_roots(&root_proc, update_code_cache);
//   _heap->workers()->run_task(&update_roots);

// #if defined(COMPILER2) || INCLUDE_JVMCI
//   DerivedPointerTable::update_pointers();
// #endif
}

void ShenandoahSemeruConcurrentMark::mark_mem_queue() {
  // assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
  ShouldNotCallThis();

  // ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();

  // for(int i = 0; i < NUM_OF_MEMORY_SERVER; i++) {
  //   if(i == CUR_MEMORY_SERVER_ID) {
  //     int num = 0;
  //     int obj_not_pushed = 0;
  //     MemRefQueue* mrq = heap->_mem_ref_queues[i];
  //     oop obj = NULL;  // For semeru, we only yse non compressed oop.

  //     WorkGang* workers = heap->workers();
  //     uint nworkers = workers->active_workers();
  //     assert(nworkers <= task_queues()->size(), "Just check");


  //     ShenandoahMarkingContext* mark_context = heap->marking_context();

  //     for(size_t i = 0; i < mrq->length(); i ++) {

  //       if((size_t)mrq->retrieve_item(i) == 0xffffffff) {
  //         ShouldNotReachHere();
  //         continue;
  //       }

  //       obj = (oop)(size_t)mrq->retrieve_item(i);  // target object, addr before compaction
  //       assert(_heap->get_server_id((HeapWord*)obj) == CUR_MEMORY_SERVER_ID , "server id: 0x%lx, cur_server: 0x%lx", _heap->get_server_id((HeapWord*)obj), CUR_MEMORY_SERVER_ID );
        
  //       // if(!heap->is_in(obj)) {
  //       //   ShouldNotReachHere();
  //       //   log_debug(semeru)("Obj not in heap： 0x%lx", (size_t)obj);
  //       // }
  //       // Klass* obj_klass = obj->klass_or_null();
  //       // if (obj_klass == NULL || (size_t)obj_klass == 0xdeafbabedeafbabeULL) {
  //       //   log_debug(semeru)("Obj klass not right： 0x%lx, 0x%lx", (size_t)obj, (size_t)obj_klass);
  //       //   ShouldNotReachHere();
  //       // }


  //       ShenandoahObjToScanQueue* q = task_queues()->queue(0);// i % (size_t)nworkers);
  //       if (mark_context->mark(obj)) {
  //         bool pushed = q->push(ShenandoahMarkTask(obj));
  //         num ++;
  //         assert(pushed, "overflow queue should always succeed pushing");
  //       }
  //       else {
  //         obj_not_pushed ++;
  //       }
  //     }
  //     log_debug(semeru)("mem ref queue total: %lu, pushed: %d, has marked: %d", mrq->length(), num, obj_not_pushed);

  //   }
  // }
  
}

void ShenandoahSemeruConcurrentMark::initialize(uint workers) {
  _heap = ShenandoahSemeruHeap::heap();

  uint num_queues = MAX2(workers, 1U);

  _task_queues = new ShenandoahObjToScanQueueSet((int) num_queues);

  for (uint i = 0; i < num_queues; ++i) {
    ShenandoahObjToScanQueue* task_queue = new ShenandoahObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
}

void ShenandoahSemeruConcurrentMark::concurrent_scan_code_roots(uint worker_id, ReferenceProcessor* rp) {
  if (ShenandoahConcurrentScanCodeRoots && claim_codecache()) {
    ShenandoahObjToScanQueue* q = task_queues()->queue(worker_id);
    if (!_heap->unload_classes()) {
      // Modified by Haoran2
      MutexLocker mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
      // TODO: We can not honor StringDeduplication here, due to lock ranking
      // inversion. So, we may miss some deduplication candidates.
      if (_heap->has_forwarded_objects()) {
        ShenandoahMarkResolveRefsClosure cl(q, rp);
        CodeBlobToOopClosure blobs(&cl, !CodeBlobToOopClosure::FixRelocations);
        CodeCache::blobs_do(&blobs);
      } else {
        ShenandoahMarkRefsClosure cl(q, rp);
        CodeBlobToOopClosure blobs(&cl, !CodeBlobToOopClosure::FixRelocations);
        CodeCache::blobs_do(&blobs);
      }
    }
  }
}

void ShenandoahSemeruConcurrentMark::mark_from_roots() {

  tty->print("ReadBitCheck: 0x%lx", _heap->_recv_mem_server_cset->_write_check_bit_shenandoah);
  // if(_heap->_recv_mem_server_cset->_write_check_bit_shenandoah == 0) {
  //   _heap->_recv_mem_server_cset->_write_check_bit_shenandoah = 1;
  // }

  WorkGang* workers = _heap->workers();
  uint nworkers = workers->active_workers();

  ShenandoahGCPhase conc_mark_phase(ShenandoahPhaseTimings::conc_mark);

  if (_heap->process_references()) {
    ReferenceProcessor* rp = _heap->ref_processor();
    rp->set_active_mt_degree(nworkers);

    // enable ("weak") refs discovery
    rp->enable_discovery(true /*verify_no_refs*/);
    rp->setup_policy(_heap->soft_ref_policy()->should_clear_all_soft_refs());
  }

  shenandoah_assert_rp_isalive_not_installed();
  ShenandoahIsAliveSelector is_alive;
  ReferenceProcessorIsAliveMutator fix_isalive(_heap->ref_processor(), is_alive.is_alive_closure());

  task_queues()->reserve(nworkers);
  log_debug(semeru)("Start Marking!");
  {
    ShenandoahTerminationTracker term(ShenandoahPhaseTimings::conc_termination);
    ShenandoahTaskTerminator terminator(nworkers, task_queues());
    ShenandoahSemeruConcurrentMarkingTask task(this, &terminator);
    workers->run_task(&task);
  }
  log_debug(semeru)("Finish Marking!");

  assert(task_queues()->is_empty() || _heap->cancelled_gc(), "Should be empty when not cancelled");
}

void ShenandoahSemeruConcurrentMark::finish_mark_from_roots(bool full_gc) {
  // Modified by Haoran for compaction
  ShouldNotReachHere();
  
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

  uint nworkers = _heap->workers()->active_workers();

  // Finally mark everything else we've got in our queues during the previous steps.
  // It does two different things for concurrent vs. mark-compact GC:
  // - For concurrent GC, it starts with empty task queues, drains the remaining
  //   SATB buffers, and then completes the marking closure.
  // - For mark-compact GC, it starts out with the task queues seeded by initial
  //   root scan, and completes the closure, thus marking through all live objects
  // The implementation is the same, so it's shared here.
  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_mark_finish_queues :
                            ShenandoahPhaseTimings::finish_queues);
    task_queues()->reserve(nworkers);

    shenandoah_assert_rp_isalive_not_installed();
    ShenandoahIsAliveSelector is_alive;
    ReferenceProcessorIsAliveMutator fix_isalive(_heap->ref_processor(), is_alive.is_alive_closure());

    ShenandoahTerminationTracker termination_tracker(full_gc ?
                                                     ShenandoahPhaseTimings::full_gc_mark_termination :
                                                     ShenandoahPhaseTimings::termination);

    StrongRootsScope scope(nworkers);
    ShenandoahTaskTerminator terminator(nworkers, task_queues());
    // Haoran: TODO
    // Do the final marking
    // ShenandoahFinalMarkingTask task(this, &terminator, ShenandoahStringDedup::is_enabled());
    // _heap->workers()->run_task(&task);
  }

  assert(task_queues()->is_empty(), "Should be empty");

  // When we're done marking everything, we process weak references.
  if (_heap->process_references()) {
    weak_refs_work(full_gc);
  }

  // And finally finish class unloading
  if (_heap->unload_classes()) {
    _heap->unload_classes_and_cleanup_tables(full_gc);
  }

  assert(task_queues()->is_empty(), "Should be empty");
  TASKQUEUE_STATS_ONLY(task_queues()->print_taskqueue_stats());
  TASKQUEUE_STATS_ONLY(task_queues()->reset_taskqueue_stats());

  // Resize Metaspace
  MetaspaceGC::compute_new_size();
}

// Weak Reference Closures
class ShenandoahSemeruCMDrainMarkingStackClosure: public VoidClosure {
  uint _worker_id;
  ShenandoahTaskTerminator* _terminator;
  bool _reset_terminator;

public:
  ShenandoahSemeruCMDrainMarkingStackClosure(uint worker_id, ShenandoahTaskTerminator* t, bool reset_terminator = false):
    _worker_id(worker_id),
    _terminator(t),
    _reset_terminator(reset_terminator) {
  }

  void do_void() {
    ShouldNotReachHere();
    // assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

    // ShenandoahSemeruHeap* sh = ShenandoahSemeruHeap::heap();
    // ShenandoahSemeruConcurrentMark* scm = sh->concurrent_mark();
    // assert(sh->process_references(), "why else would we be here?");
    // ReferenceProcessor* rp = sh->ref_processor();

    // shenandoah_assert_rp_isalive_installed();

    // scm->mark_loop(_worker_id, _terminator, rp,
    //                false,   // not cancellable
    //                false);  // do not do strdedup

    // if (_reset_terminator) {
    //   _terminator->reset_for_reuse();
    // }
  }
};

class ShenandoahSemeruCMKeepAliveClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahSemeruHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahSemeruConcurrentMark::mark_through_ref<T, NONE, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahSemeruCMKeepAliveClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahSemeruHeap::heap()),
    _mark_context(_heap->marking_context()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
  inline void semeru_ms_do_oop(oop obj, oop* p){ do_oop(p); };
  inline void semeru_ms_do_oop(oop obj, narrowOop* p){ do_oop(p); };

};

class ShenandoahSemeruCMKeepAliveUpdateClosure : public OopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahSemeruHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

  template <class T>
  inline void do_oop_work(T* p) {
    ShenandoahSemeruConcurrentMark::mark_through_ref<T, SIMPLE, NO_DEDUP>(p, _heap, _queue, _mark_context);
  }

public:
  ShenandoahSemeruCMKeepAliveUpdateClosure(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahSemeruHeap::heap()),
    _mark_context(_heap->marking_context()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }

  inline void semeru_ms_do_oop(oop obj, oop* p){ do_oop(p); };
  inline void semeru_ms_do_oop(oop obj, narrowOop* p){ do_oop(p); };

};

class ShenandoahWeakUpdateClosure : public OopClosure {
private:
  ShenandoahHeap* const _heap;

  template <class T>
  inline void do_oop_work(T* p) {
    oop o = _heap->maybe_update_with_forwarded(p);
    shenandoah_assert_marked_except(p, o, o == NULL);
  }

public:
  ShenandoahWeakUpdateClosure() : _heap(ShenandoahHeap::heap()) {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }
void semeru_ms_do_oop(oop obj, narrowOop* p) { do_oop_work(p); }
void semeru_ms_do_oop(oop obj,       oop* p) { do_oop_work(p); }

};

class ShenandoahWeakAssertNotForwardedClosure : public OopClosure {
private:
  template <class T>
  inline void do_oop_work(T* p) {
    T o = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(o)) {
      oop obj = CompressedOops::decode_not_null(o);
      shenandoah_assert_not_forwarded(p, obj);
    }
  }

public:
  ShenandoahWeakAssertNotForwardedClosure() {}

  void do_oop(narrowOop* p) { do_oop_work(p); }
  void do_oop(oop* p)       { do_oop_work(p); }

  void semeru_ms_do_oop(oop obj, narrowOop* p) { do_oop_work(p); }
  void semeru_ms_do_oop(oop obj,       oop* p) { do_oop_work(p); }
};

// class ShenandoahRefProcTaskProxy : public AbstractGangTask {
// private:
//   AbstractRefProcTaskExecutor::ProcessTask& _proc_task;
//   ShenandoahTaskTerminator* _terminator;

// public:
//   ShenandoahRefProcTaskProxy(AbstractRefProcTaskExecutor::ProcessTask& proc_task,
//                              ShenandoahTaskTerminator* t) :
//     AbstractGangTask("Process reference objects in parallel"),
//     _proc_task(proc_task),
//     _terminator(t) {
//   }

//   void work(uint worker_id) {
//     ResourceMark rm;
//     HandleMark hm;
//     assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");
//     ShenandoahHeap* heap = ShenandoahHeap::heap();
//     ShenandoahCMDrainMarkingStackClosure complete_gc(worker_id, _terminator);
//     if (heap->has_forwarded_objects()) {
//       ShenandoahForwardedIsAliveClosure is_alive;
//       ShenandoahCMKeepAliveUpdateClosure keep_alive(heap->concurrent_mark()->get_queue(worker_id));
//       _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
//     } else {
//       ShenandoahIsAliveClosure is_alive;
//       ShenandoahCMKeepAliveClosure keep_alive(heap->concurrent_mark()->get_queue(worker_id));
//       _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
//     }
//   }
// };

// class ShenandoahRefProcTaskExecutor : public AbstractRefProcTaskExecutor {
// private:
//   WorkGang* _workers;

// public:
//   ShenandoahRefProcTaskExecutor(WorkGang* workers) :
//     _workers(workers) {
//   }

//   // Executes a task using worker threads.
//   void execute(ProcessTask& task, uint ergo_workers) {
//     assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a safepoint");

//     ShenandoahHeap* heap = ShenandoahHeap::heap();
//     ShenandoahSemeruConcurrentMark* cm = heap->concurrent_mark();
//     ShenandoahPushWorkerQueuesScope scope(_workers, cm->task_queues(),
//                                           ergo_workers,
//                                           /* do_check = */ false);
//     uint nworkers = _workers->active_workers();
//     cm->task_queues()->reserve(nworkers);
//     ShenandoahTaskTerminator terminator(nworkers, cm->task_queues());
//     ShenandoahRefProcTaskProxy proc_task_proxy(task, &terminator);
//     _workers->run_task(&proc_task_proxy);
//   }
// };

void ShenandoahSemeruConcurrentMark::weak_refs_work(bool full_gc) {
  ShouldNotCallThis();
  // assert(_heap->process_references(), "sanity");

  // ShenandoahPhaseTimings::Phase phase_root =
  //         full_gc ?
  //         ShenandoahPhaseTimings::full_gc_weakrefs :
  //         ShenandoahPhaseTimings::weakrefs;

  // ShenandoahGCPhase phase(phase_root);

  // ReferenceProcessor* rp = _heap->ref_processor();

  // // NOTE: We cannot shortcut on has_discovered_references() here, because
  // // we will miss marking JNI Weak refs then, see implementation in
  // // ReferenceProcessor::process_discovered_references.
  // weak_refs_work_doit(full_gc);

  // rp->verify_no_references_recorded();
  // assert(!rp->discovery_enabled(), "Post condition");

}

void ShenandoahSemeruConcurrentMark::weak_refs_work_doit(bool full_gc) {
  ShouldNotCallThis();
  // ReferenceProcessor* rp = _heap->ref_processor();

  // ShenandoahPhaseTimings::Phase phase_process =
  //         full_gc ?
  //         ShenandoahPhaseTimings::full_gc_weakrefs_process :
  //         ShenandoahPhaseTimings::weakrefs_process;

  // ShenandoahPhaseTimings::Phase phase_process_termination =
  //         full_gc ?
  //         ShenandoahPhaseTimings::full_gc_weakrefs_termination :
  //         ShenandoahPhaseTimings::weakrefs_termination;

  // shenandoah_assert_rp_isalive_not_installed();
  // ShenandoahIsAliveSelector is_alive;
  // ReferenceProcessorIsAliveMutator fix_isalive(rp, is_alive.is_alive_closure());

  // WorkGang* workers = _heap->workers();
  // uint nworkers = workers->active_workers();

  // rp->setup_policy(_heap->soft_ref_policy()->should_clear_all_soft_refs());
  // rp->set_active_mt_degree(nworkers);

  // assert(task_queues()->is_empty(), "Should be empty");

  // // complete_gc and keep_alive closures instantiated here are only needed for
  // // single-threaded path in RP. They share the queue 0 for tracking work, which
  // // simplifies implementation. Since RP may decide to call complete_gc several
  // // times, we need to be able to reuse the terminator.
  // uint serial_worker_id = 0;
  // ShenandoahTaskTerminator terminator(1, task_queues());
  // ShenandoahCMDrainMarkingStackClosure complete_gc(serial_worker_id, &terminator, /* reset_terminator = */ true);

  // ShenandoahRefProcTaskExecutor executor(workers);

  // ReferenceProcessorPhaseTimes pt(_heap->gc_timer(), rp->num_queues());

  // {
  //   ShenandoahGCPhase phase(phase_process);
  //   ShenandoahTerminationTracker phase_term(phase_process_termination);

  //   // Process leftover weak oops: update them, if needed (using parallel version),
  //   // or assert they do not need updating (using serial version) otherwise.
  //   // Weak processor API requires us to visit the oops, even if we are not doing
  //   // anything to them.
  //   if (_heap->has_forwarded_objects()) {
  //     ShenandoahCMKeepAliveUpdateClosure keep_alive(get_queue(serial_worker_id));
  //     rp->process_discovered_references(is_alive.is_alive_closure(), &keep_alive,
  //                                       &complete_gc, &executor,
  //                                       &pt);

  //     ShenandoahWeakUpdateClosure cl;
  //     WeakProcessor::weak_oops_do(workers, is_alive.is_alive_closure(), &cl, 1);
  //   } else {
  //     ShenandoahCMKeepAliveClosure keep_alive(get_queue(serial_worker_id));
  //     rp->process_discovered_references(is_alive.is_alive_closure(), &keep_alive,
  //                                       &complete_gc, &executor,
  //                                       &pt);

  //     ShenandoahWeakAssertNotForwardedClosure cl;
  //     WeakProcessor::weak_oops_do(is_alive.is_alive_closure(), &cl);
  //   }

  //   pt.print_all_references();

  //   assert(task_queues()->is_empty(), "Should be empty");
  // }
}

// class ShenandoahCancelledGCYieldClosure : public YieldClosure {
// private:
//   ShenandoahHeap* const _heap;
// public:
//   ShenandoahCancelledGCYieldClosure() : _heap(ShenandoahHeap::heap()) {};
//   virtual bool should_return() { return _heap->cancelled_gc(); }
// };

// class ShenandoahPrecleanCompleteGCClosure : public VoidClosure {
// public:
//   void do_void() {
//     ShenandoahHeap* sh = ShenandoahHeap::heap();
//     ShenandoahSemeruConcurrentMark* scm = sh->concurrent_mark();
//     assert(sh->process_references(), "why else would we be here?");
//     ShenandoahTaskTerminator terminator(1, scm->task_queues());

//     ReferenceProcessor* rp = sh->ref_processor();
//     shenandoah_assert_rp_isalive_installed();

//     scm->mark_loop(0, &terminator, rp,
//                    false, // not cancellable
//                    false); // do not do strdedup
//   }
// };

// class ShenandoahPrecleanKeepAliveUpdateClosure : public OopClosure {
// private:
//   ShenandoahObjToScanQueue* _queue;
//   ShenandoahHeap* _heap;
//   ShenandoahMarkingContext* const _mark_context;

//   template <class T>
//   inline void do_oop_work(T* p) {
//     ShenandoahSemeruConcurrentMark::mark_through_ref<T, CONCURRENT, NO_DEDUP>(p, _heap, _queue, _mark_context);
//   }

// public:
//   ShenandoahPrecleanKeepAliveUpdateClosure(ShenandoahObjToScanQueue* q) :
//     _queue(q),
//     _heap(ShenandoahHeap::heap()),
//     _mark_context(_heap->marking_context()) {}

//   void do_oop(narrowOop* p) { do_oop_work(p); }
//   void do_oop(oop* p)       { do_oop_work(p); }

//   void semeru_ms_do_oop(oop obj, narrowOop* p) { do_oop_work(p); }
//   void semeru_ms_do_oop(oop obj,       oop* p) { do_oop_work(p); }
// };

// class ShenandoahPrecleanTask : public AbstractGangTask {
// private:
//   ReferenceProcessor* _rp;

// public:
//   ShenandoahPrecleanTask(ReferenceProcessor* rp) :
//           AbstractGangTask("Precleaning task"),
//           _rp(rp) {}

//   void work(uint worker_id) {
//     assert(worker_id == 0, "The code below is single-threaded, only one worker is expected");
//     ShenandoahParallelWorkerSession worker_session(worker_id);

//     ShenandoahHeap* sh = ShenandoahHeap::heap();

//     ShenandoahObjToScanQueue* q = sh->concurrent_mark()->get_queue(worker_id);

//     ShenandoahCancelledGCYieldClosure yield;
//     ShenandoahPrecleanCompleteGCClosure complete_gc;

//     if (sh->has_forwarded_objects()) {
//       ShenandoahForwardedIsAliveClosure is_alive;
//       ShenandoahPrecleanKeepAliveUpdateClosure keep_alive(q);
//       ResourceMark rm;
//       _rp->preclean_discovered_references(&is_alive, &keep_alive,
//                                           &complete_gc, &yield,
//                                           NULL);
//     } else {
//       ShenandoahIsAliveClosure is_alive;
//       ShenandoahCMKeepAliveClosure keep_alive(q);
//       ResourceMark rm;
//       _rp->preclean_discovered_references(&is_alive, &keep_alive,
//                                           &complete_gc, &yield,
//                                           NULL);
//     }
//   }
// };

void ShenandoahSemeruConcurrentMark::preclean_weak_refs() {
  ShouldNotCallThis();
  // // Pre-cleaning weak references before diving into STW makes sense at the
  // // end of concurrent mark. This will filter out the references which referents
  // // are alive. Note that ReferenceProcessor already filters out these on reference
  // // discovery, and the bulk of work is done here. This phase processes leftovers
  // // that missed the initial filtering, i.e. when referent was marked alive after
  // // reference was discovered by RP.

  // assert(_heap->process_references(), "sanity");

  // // Shortcut if no references were discovered to avoid winding up threads.
  // ReferenceProcessor* rp = _heap->ref_processor();
  // if (!rp->has_discovered_references()) {
  //   return;
  // }

  // assert(task_queues()->is_empty(), "Should be empty");

  // ReferenceProcessorMTDiscoveryMutator fix_mt_discovery(rp, false);

  // shenandoah_assert_rp_isalive_not_installed();
  // ShenandoahIsAliveSelector is_alive;
  // ReferenceProcessorIsAliveMutator fix_isalive(rp, is_alive.is_alive_closure());

  // // Execute precleaning in the worker thread: it will give us GCLABs, String dedup
  // // queues and other goodies. When upstream ReferenceProcessor starts supporting
  // // parallel precleans, we can extend this to more threads.
  // WorkGang* workers = _heap->workers();
  // uint nworkers = workers->active_workers();
  // assert(nworkers == 1, "This code uses only a single worker");
  // task_queues()->reserve(nworkers);

  // ShenandoahPrecleanTask task(rp);
  // workers->run_task(&task);

  // assert(task_queues()->is_empty(), "Should be empty");
}

void ShenandoahSemeruConcurrentMark::cancel() {
  // Clean up marking stacks.
  ShenandoahObjToScanQueueSet* queues = task_queues();
  queues->clear();

  // Cancel SATB buffers.
  ShenandoahBarrierSet::satb_mark_queue_set().abandon_partial_marking();
}

ShenandoahObjToScanQueue* ShenandoahSemeruConcurrentMark::get_queue(uint worker_id) {
  assert(task_queues()->get_reserved() > worker_id, "No reserved queue for worker id: %d", worker_id);
  return _task_queues->queue(worker_id);
}

template <bool CANCELLABLE>
void ShenandoahSemeruConcurrentMark::mark_loop_prework(uint w, ShenandoahTaskTerminator *t, ReferenceProcessor *rp,
                                                 bool strdedup) {
  ShenandoahObjToScanQueue* q = get_queue(w);

  jushort* ld = _heap->get_liveness_cache(w);

  // TODO: We can clean up this if we figure out how to do templated oop closures that
  // play nice with specialized_oop_iterators.
  if (_heap->unload_classes()) {
    if (_heap->has_forwarded_objects()) {
      if (strdedup) {
        ShenandoahMarkUpdateRefsMetadataDedupClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsMetadataDedupClosure, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkUpdateRefsMetadataClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsMetadataClosure, CANCELLABLE>(&cl, ld, w, t);
      }
    } else {
      if (strdedup) {
        ShenandoahMarkRefsMetadataDedupClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsMetadataDedupClosure, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkRefsMetadataClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsMetadataClosure, CANCELLABLE>(&cl, ld, w, t);
      }
    }
  } else {
    if (_heap->has_forwarded_objects()) {
      if (strdedup) {
        ShenandoahMarkUpdateRefsDedupClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsDedupClosure, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahMarkUpdateRefsClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkUpdateRefsClosure, CANCELLABLE>(&cl, ld, w, t);
      }
    } else {
      if (strdedup) {
        ShenandoahMarkRefsDedupClosure cl(q, rp);
        mark_loop_work<ShenandoahMarkRefsDedupClosure, CANCELLABLE>(&cl, ld, w, t);
      } else {
        ShenandoahSemeruMarkRefsClosure cl(q, rp);
        mark_loop_work<ShenandoahSemeruMarkRefsClosure, CANCELLABLE>(&cl, ld, w, t);
      }
    }
  }

  _heap->flush_liveness_cache(w);
}

template <class T, bool CANCELLABLE>
void ShenandoahSemeruConcurrentMark::mark_loop_work(T* cl, jushort* live_data, uint worker_id, ShenandoahTaskTerminator *terminator) {
  uintx stride = ShenandoahMarkLoopStride;

  // Haoran: TODO // Why we need this heap
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahObjToScanQueueSet* queues = task_queues();
  ShenandoahObjToScanQueue* q;
  ShenandoahMarkTask t;

  /*
   * Process outstanding queues, if any.
   *
   * There can be more queues than workers. To deal with the imbalance, we claim
   * extra queues first. Since marking can push new tasks into the queue associated
   * with this worker id, we come back to process this queue in the normal loop.
   */
  assert(queues->get_reserved() == heap->workers()->active_workers(),
         "Need to reserve proper number of queues: reserved: %u, active: %u", queues->get_reserved(), heap->workers()->active_workers());

  q = queues->claim_next();
  while (q != NULL) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (q->pop(t)) {
        do_task<T>(q, cl, live_data, &t);
      } else {
        assert(q->is_empty(), "Must be empty");
        q = queues->claim_next();
        break;
      }
    }
  }
  log_debug(semeru,mem_trace)("%lu Finish marking and before SATB!",(size_t)worker_id);

  q = get_queue(worker_id);

  // Modified by Haoran
  ShenandoahSemeruSATBBufferClosure drain_satb(q);
  SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();

  /*
   * Normal marking loop:
   */
  while (true) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    // Modified by Haoran
    satb_mq_set.semeru_apply_closure_to_completed_buffer(&drain_satb);
    while (satb_mq_set.semeru_not_empty()) {
      satb_mq_set.semeru_apply_closure_to_completed_buffer(&drain_satb);
    }

    uint work = 0;
    for (uint i = 0; i < stride; i++) {
      if (q->pop(t) ||
          queues->steal(worker_id, t)) {
        do_task<T>(q, cl, live_data, &t);
        work++;
      } else {
        break;
      }
    }
    // log_debug(semeru,mem_trace)("%lu After SATB!",(size_t)worker_id);
    if (work == 0) {
      // log_debug(semeru)("How much work has done? %u", work);
      // No work encountered in current stride, try to terminate.
      // Need to leave the STS here otherwise it might block safepoints.
      // What is this terminator doing
      ShenandoahSuspendibleThreadSetLeaver stsl(CANCELLABLE && ShenandoahSuspendibleWorkers);
      ShenandoahTerminationTimingsTracker term_tracker(worker_id);
      ShenandoahTerminatorTerminator tt(heap);
      if (terminator->offer_termination(&tt)) return;
      // mhrcr: single worker will directly be done
    }
  }
}

bool ShenandoahSemeruConcurrentMark::claim_codecache() {
  assert(ShenandoahConcurrentScanCodeRoots, "must not be called otherwise");
  return _claimed_codecache.try_set();
}

void ShenandoahSemeruConcurrentMark::clear_claim_codecache() {
  assert(ShenandoahConcurrentScanCodeRoots, "must not be called otherwise");
  _claimed_codecache.unset();
}
