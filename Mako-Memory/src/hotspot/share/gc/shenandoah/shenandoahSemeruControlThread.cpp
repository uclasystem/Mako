// Modified by Haoran
/*
 * Copyright (c) 2013, 2018, Red Hat, Inc. All rights reserved.
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

#include "gc/shenandoah/shenandoahSemeruConcurrentMark.hpp"
#include "gc/shenandoah/shenandoahSemeruCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahSemeruControlThread.hpp"
#include "gc/shenandoah/shenandoahTraversalGC.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVMOperations.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"

ShenandoahSemeruControlThread::ShenandoahSemeruControlThread() :
  ConcurrentGCThread(),
  _alloc_failure_waiters_lock(Mutex::leaf, "ShenandoahAllocFailureGC_lock", true, Monitor::_safepoint_check_always),
  _gc_waiters_lock(Mutex::leaf, "ShenandoahRequestedGC_lock", true, Monitor::_safepoint_check_always),
  _periodic_task(this),
  _requested_gc_cause(GCCause::_no_cause_specified),
  _degen_point(ShenandoahSemeruHeap::_degenerated_outside_cycle),
  _allocs_seen(0) {

  create_and_start(ShenandoahCriticalControlThreadPriority ? CriticalPriority : NearMaxPriority);
  _periodic_task.enroll();
  _periodic_satb_flush_task.enroll();
}

ShenandoahSemeruControlThread::~ShenandoahSemeruControlThread() {
  // This is here so that super is called.
}

void ShenandoahSemeruPeriodicTask::task() {
  _thread->handle_force_counters_update();
  _thread->handle_counters_update();
}

void ShenandoahSemeruPeriodicSATBFlushTask::task() {
  ShenandoahSemeruHeap::heap()->force_satb_flush_all_threads();
}

void ShenandoahSemeruControlThread::run_service() {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  log_debug(semeru)("read bit %lu", heap->_recv_mem_server_cset->_write_check_bit_shenandoah);

  ShenandoahSemeruRegionIterator regions;
  ShenandoahSemeruHeapRegion* region;
  int sleep = ShenandoahControlIntervalMin;

  double last_shrink_time = os::elapsedTime();
  double last_sleep_adjust_time = os::elapsedTime();

  // Shrink period avoids constantly polling regions for shrinking.
  // Having a period 10x lower than the delay would mean we hit the
  // shrinking with lag of less than 1/10-th of true delay.
  // ShenandoahUncommitDelay is in msecs, but shrink_period is in seconds.
  // double shrink_period = (double)ShenandoahUncommitDelay / 1000 / 10;

  ShenandoahSemeruCollectorPolicy* policy = heap->shenandoah_policy();
  ShenandoahHeuristics* heuristics = heap->heuristics();
  while (!in_graceful_shutdown() && !should_terminate()) {

    log_debug(semeru, alloc)(" heap 0x%lx, policy 0x%lx", (size_t)(heap), (size_t)policy);
    log_debug(semeru, alloc)("	flags_of_cpu_server_state  0x%lx", (size_t)(heap->_cpu_server_flags));
    do {
      os::naked_short_sleep(ShenandoahControlIntervalMin); // Haoran: TODO add a new parameter to control this
    } while(heap->_cpu_server_flags->should_start_tracing() == false);

    // heap->_cpu_server_flags->_should_start_tracing = false;
    log_debug(semeru)("read bit %lu", heap->_recv_mem_server_cset->_write_check_bit_shenandoah);
    // This control loop iteration have seen this much allocations.
    // size_t allocs_seen = Atomic::xchg<size_t>(0, &_allocs_seen);

    // Choose which GC mode to run in. The block below should select a single mode.
    // mhrcr: always use this mode
    GCMode mode = concurrent_normal;
    GCCause::Cause cause = GCCause::_shenandoah_concurrent_gc;
    //ShenandoahSemeruHeap::ShenandoahDegenPoint degen_point = ShenandoahSemeruHeap::_degenerated_unset;

    // Blow all soft references on this cycle, if handling allocation failure,
    // or we are requested to do so unconditionally.
    // Haoran: TODO do we need to process soft refs?
    // if (alloc_failure_pending || ShenandoahAlwaysClearSoftRefs) {
    //   heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);
    // }

    bool gc_requested = (mode != none);
    assert (!gc_requested || cause != GCCause::_last_gc_cause, "GC cause should be set");

    if (gc_requested) {
      heap->reset_bytes_allocated_since_gc_start();

      // If GC was requested, we are sampling the counters even without actual triggers
      // from allocation machinery. This captures GC phases more accurately.
      set_forced_counters_update(true);

      // If GC was requested, we better dump freeset data for performance debugging
      {
        ShenandoahHeapLocker locker(heap->lock());
        // heap->free_set()->log_status();
      }
    }
    log_debug(semeru)("read bit %lu", heap->_recv_mem_server_cset->_write_check_bit_shenandoah);
    service_concurrent_normal_cycle(cause);

    // if (gc_requested) {
    //   // If this was the requested GC cycle, notify waiters about it
    //   // if (explicit_gc_requested || implicit_gc_requested) {
    //   //   notify_gc_waiters();
    //   // }

    //   // If this was the allocation failure GC cycle, notify waiters about it
    //   // if (alloc_failure_pending) {
    //   //   notify_alloc_failure_waiters();
    //   // }

    //   // Report current free set state at the end of cycle, whether
    //   // it is a normal completion, or the abort.
    //   {
    //     ShenandoahHeapLocker locker(heap->lock());
    //     heap->free_set()->log_status();

    //     // Notify Universe about new heap usage. This has implications for
    //     // global soft refs policy, and we better report it every time heap
    //     // usage goes down.
    //     Universe::update_heap_info_at_gc();
    //   }

    //   // Disable forced counters update, and update counters one more time
    //   // to capture the state at the end of GC session.
    //   handle_force_counters_update();
    //   set_forced_counters_update(false);

    //   // Retract forceful part of soft refs policy
    //   heap->soft_ref_policy()->set_should_clear_all_soft_refs(false);

    //   // Clear metaspace oom flag, if current cycle unloaded classes
    //   // mhrcr: never unload classes
    //   // if (heap->unload_classes()) {
    //   //   heuristics->clear_metaspace_oom();
    //   // }

    //   // GC is over, we are at idle now
    //   if (ShenandoahPacing) {
    //     heap->pacer()->setup_for_idle();
    //   }
    // } else {
    //   // Allow allocators to know we have seen this much regions
    //   // if (ShenandoahPacing && (allocs_seen > 0)) {
    //   //   heap->pacer()->report_alloc(allocs_seen);
    //   // }
    // }

    // double current = os::elapsedTime();


    // // mhrcr: Never uncommit on memory servers.

    // // Wait before performing the next action. If allocation happened during this wait,
    // // we exit sooner, to let heuristics re-evaluate new conditions. If we are at idle,
    // // back off exponentially.
    // if (_heap_changed.try_unset()) {
    //   sleep = ShenandoahControlIntervalMin;
    // } else if ((current - last_sleep_adjust_time) * 1000 > ShenandoahControlIntervalAdjustPeriod){
    //   sleep = MIN2<int>(ShenandoahControlIntervalMax, MAX2(1, sleep * 2));
    //   last_sleep_adjust_time = current;
    // }
    os::naked_short_sleep(sleep);
  }

  // Wait for the actual stop(), can't leave run_service() earlier.
  while (!should_terminate()) {
    os::naked_short_sleep(ShenandoahControlIntervalMin);
  }
}

void ShenandoahSemeruControlThread::service_concurrent_traversal_cycle(GCCause::Cause cause) {

    // Modified by Haoran
    ShouldNotReachHere();


  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause);

  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());

  // Reset for upcoming cycle
  heap->entry_reset();

  heap->vmop_entry_init_traversal();

  if (check_cancellation_or_degen(ShenandoahSemeruHeap::_degenerated_traversal)) return;

  heap->entry_traversal();
  if (check_cancellation_or_degen(ShenandoahSemeruHeap::_degenerated_traversal)) return;

  heap->vmop_entry_final_traversal();

  heap->entry_cleanup();

  heap->heuristics()->record_success_concurrent();
  heap->shenandoah_policy()->record_success_concurrent();
}

void ShenandoahSemeruControlThread::service_concurrent_normal_cycle(GCCause::Cause cause) {
  // Normal cycle goes via all concurrent phases. If allocation failure (af) happens during
  // any of the concurrent phases, it first degrades to Degenerated GC and completes GC there.
  // If second allocation failure happens during Degenerated GC cycle (for example, when GC
  // tries to evac something and no memory is available), cycle degrades to Full GC.
  //
  // There are also two shortcuts through the normal cycle: a) immediate garbage shortcut, when
  // heuristics says there are no regions to compact, and all the collection comes from immediately
  // reclaimable regions; b) coalesced UR shortcut, when heuristics decides to coalesce UR with the
  // mark from the next cycle.
  //
  // ................................................................................................
  //
  //                                    (immediate garbage shortcut)                Concurrent GC
  //                             /-------------------------------------------\
  //                             |                       (coalesced UR)      v
  //                             |                  /----------------------->o
  //                             |                  |                        |
  //                             |                  |                        v
  // [START] ----> Conc Mark ----o----> Conc Evac --o--> Conc Update-Refs ---o----> [END]
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    |                 |              |      Degenerated GC
  //                   v                    v                 v              |
  //               STW Mark ----------> STW Evac ----> STW Update-Refs ----->o
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    v                 |              |      Full GC
  //                   \------------------->o<----------------/              |
  //                                        |                                |
  //                                        v                                |
  //                                      Full GC  --------------------------/
  //
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();

  // if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_outside_cycle)) return;

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause);

  // Haoran: TODO
  // What's this
  //TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());

  // Reset for upcoming marking
  heap->entry_reset();
  heap->_mem_server_flags->_tracing_finished = false;
  // Modified by Haoran for remote compaction
  heap->_mem_server_flags->evacuation_finished = false;

  tty->print("tracing all finished before init_mark: %d, tracing_current_cycle_processing %d\n", heap->_cpu_server_flags->_tracing_all_finished, heap->_tracing_current_cycle_processing->get_byte_flag());
  heap->entry_init_mark();
  tty->print("tracing all finished after init mark: %d, tracing_current_cycle_processing %d\n", heap->_cpu_server_flags->_tracing_all_finished, heap->_tracing_current_cycle_processing->get_byte_flag());

  // while(heap->_cpu_server_flags->_tracing_all_finished == false) {
  //   log_debug(semeru)("in loop");
  //   if(heap->_mem_ref_current_queue_all_here->get_byte_flag() == true) {
  //     heap->_tracing_current_cycle_processing->set_byte_flag(true);
  //     log_debug(semeru)("before_mark_queue");
      // heap->concurrent_mark()->mark_mem_queue();
  //     log_debug(semeru)("after_mark_queue");
  //     heap->_mem_ref_current_queue_has_data->set_byte_flag(false);
  //     heap->_mem_ref_current_queue_all_here->set_byte_flag(false);
  //   }
  //   // Continue concurrent mark
  //   if(heap->_tracing_current_cycle_processing->get_byte_flag()) {
  //     log_debug(semeru)("before entry mark");
      
  //     for(int i = 0; i < NUM_OF_MEMORY_SERVER; i++){
  //       if(i != CUR_MEMORY_SERVER_ID)
  //         heap->_mem_ref_queues[i]->reset();
  //     }

      heap->entry_mark();
      log_debug(semeru)("after entry mark");
      log_debug(semeru)("memref_queue: %lu, length: %lu", CUR_MEMORY_SERVER_ID^1, heap->_mem_ref_queues[CUR_MEMORY_SERVER_ID^1]->length());
      ShenandoahSemeruRegionIterator regions;
      ShenandoahSemeruHeapRegion* region;
      // region = regions.next();
      // while(region != NULL) {
      //   //syscall(RDMA_READ, 0, region->mem_to_cpu_at_gc(), sizeof(ShenandoahMemoryToCPUAtGC));
      //   // if(heap->get_server_id(region->bottom()) == CUR_MEMORY_SERVER_ID)
      //     // log_debug(semeru,rdma)("_curr_region[0x%lx], live_data: 0x%lx, is_bitmap_clear: %d, next: 0x%lx", 	region->region_number(), region->mem_to_cpu_at_gc()->_live_data, heap->_marking_context->is_bitmap_clear_range(region->bottom(), region->top()), (size_t)heap->_marking_context->mark_bit_map()->get_next_marked_addr(region->bottom(), region->top()) );
      //   region = regions.next();
      // }
    // }
    


  //   // heap->flush_cache_bitmap();

  //   heap->_tracing_current_cycle_processing->set_byte_flag(false);

    

  //   os::naked_short_sleep(500);
  // }
  // log_debug(semeru)("tracing_finished_is_true");

  
  heap->_mem_server_flags->_tracing_finished = true;
  log_debug(semeru)("finish marking");

  tty->print("tracing all finished after init mark: %d, tracing_current_cycle_processing %d\n", heap->_cpu_server_flags->_tracing_all_finished, heap->_tracing_current_cycle_processing->get_byte_flag());


  //clear flags:
  heap->_cpu_server_flags->_should_start_tracing = false;
  heap->_cpu_server_flags->_tracing_all_finished = false;

  tty->print("tracing all finished after init mark: %d, tracing_current_cycle_processing %d\n", heap->_cpu_server_flags->_tracing_all_finished, heap->_tracing_current_cycle_processing->get_byte_flag());


  while(heap->_cpu_server_flags->_should_start_evacuation == false && heap->_cpu_server_flags->_evacuation_all_finished == false) {
    os::naked_short_sleep(10);
    // continue;
  }

  log_debug(semeru)("Start evacuation!");

  if(heap->_cpu_server_flags->_evacuation_all_finished == false) {
    heap->_mem_server_flags->_tracing_finished = false;


    heap->collection_set()->copy_sync_to_cset();
    
    // Concurrently compact objects
    heap->entry_evac();


    heap->_mem_server_flags->evacuation_finished = true;
    log_debug(semeru)("Finish evacuation!");
  }
  else {
    log_debug(semeru)("No CSET, skip evacuation!");
  }
  heap->_mem_server_flags->_tracing_finished = false;
  // heap->reset_offset_table();
}

bool ShenandoahSemeruControlThread::check_cancellation_or_degen(ShenandoahSemeruHeap::ShenandoahDegenPoint point) {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  if (heap->cancelled_gc()) {
    assert (is_alloc_failure_gc() || in_graceful_shutdown(), "Cancel GC either for alloc failure GC, or gracefully exiting");
    if (!in_graceful_shutdown()) {
      assert (_degen_point == ShenandoahSemeruHeap::_degenerated_outside_cycle,
              "Should not be set yet: %s", ShenandoahSemeruHeap::degen_point_to_string(_degen_point));
      _degen_point = point;
    }
    return true;
  }
  return false;
}

void ShenandoahSemeruControlThread::stop_service() {
  // Nothing to do here.
}

void ShenandoahSemeruControlThread::service_stw_full_cycle(GCCause::Cause cause) {

  // Modified by Haoran
  ShouldNotReachHere();

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause);

  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  heap->vmop_entry_full(cause);

  heap->heuristics()->record_success_full();
  heap->shenandoah_policy()->record_success_full();
}

void ShenandoahSemeruControlThread::service_stw_degenerated_cycle(GCCause::Cause cause, ShenandoahSemeruHeap::ShenandoahDegenPoint point) {
  assert (point != ShenandoahSemeruHeap::_degenerated_unset, "Degenerated point should be set");
    
  // Modified by Haoran
  ShouldNotReachHere();

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause);

  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();
  heap->vmop_degenerated(point);

  heap->heuristics()->record_success_degenerated();
  heap->shenandoah_policy()->record_success_degenerated();
}

void ShenandoahSemeruControlThread::service_uncommit(double shrink_before) {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();

  // Determine if there is work to do. This avoids taking heap lock if there is
  // no work available, avoids spamming logs with superfluous logging messages,
  // and minimises the amount of work while locks are taken.

  if (heap->committed() <= heap->min_capacity()) return;

  bool has_work = false;
  for (size_t i = 0; i < heap->num_regions(); i++) {
    ShenandoahSemeruHeapRegion *r = heap->get_region(i);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      has_work = true;
      break;
    }
  }

  if (has_work) {
    heap->entry_uncommit(shrink_before);
  }
}

bool ShenandoahSemeruControlThread::is_explicit_gc(GCCause::Cause cause) const {
  return GCCause::is_user_requested_gc(cause) ||
         GCCause::is_serviceability_requested_gc(cause);
}

void ShenandoahSemeruControlThread::request_gc(GCCause::Cause cause) {
  assert(GCCause::is_user_requested_gc(cause) ||
         GCCause::is_serviceability_requested_gc(cause) ||
         cause == GCCause::_metadata_GC_clear_soft_refs ||
         cause == GCCause::_full_gc_alot ||
         cause == GCCause::_wb_full_gc ||
         cause == GCCause::_scavenge_alot,
         "only requested GCs here");

  if (is_explicit_gc(cause)) {
    if (!DisableExplicitGC) {
      handle_requested_gc(cause);
    }
  } else {
    handle_requested_gc(cause);
  }
}

void ShenandoahSemeruControlThread::handle_requested_gc(GCCause::Cause cause) {
  _requested_gc_cause = cause;
  _gc_requested.set();
  // Modified by Haoran2
  MonitorLocker ml(&_gc_waiters_lock);
  while (_gc_requested.is_set()) {
    ml.wait();
  }
}

void ShenandoahSemeruControlThread::handle_alloc_failure(size_t words) {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();

  assert(current()->is_Java_thread(), "expect Java thread here");

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate " SIZE_FORMAT "%s",
                 byte_size_in_proper_unit(words * HeapWordSize), proper_unit_for_byte_size(words * HeapWordSize));

    // Now that alloc failure GC is scheduled, we can abort everything else
    heap->cancel_gc(GCCause::_allocation_failure);
  }
  // Modified by Haoran2
  MonitorLocker ml(&_alloc_failure_waiters_lock);
  while (is_alloc_failure_gc()) {
    ml.wait();
  }
}

void ShenandoahSemeruControlThread::handle_alloc_failure_evac(size_t words) {
  ShenandoahSemeruHeap* heap = ShenandoahSemeruHeap::heap();

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate " SIZE_FORMAT "%s for evacuation",
                 byte_size_in_proper_unit(words * HeapWordSize), proper_unit_for_byte_size(words * HeapWordSize));
  }

  // Forcefully report allocation failure
  heap->cancel_gc(GCCause::_shenandoah_allocation_failure_evac);
}

void ShenandoahSemeruControlThread::notify_alloc_failure_waiters() {
  _alloc_failure_gc.unset();
  // Modified by Haoran2
  MonitorLocker ml(&_alloc_failure_waiters_lock);
  ml.notify_all();
}

bool ShenandoahSemeruControlThread::try_set_alloc_failure_gc() {
  return _alloc_failure_gc.try_set();
}

bool ShenandoahSemeruControlThread::is_alloc_failure_gc() {
  return _alloc_failure_gc.is_set();
}

void ShenandoahSemeruControlThread::notify_gc_waiters() {
  _gc_requested.unset();
  // Modified by Haoran2
  MonitorLocker ml(&_gc_waiters_lock);
  ml.notify_all();
}

void ShenandoahSemeruControlThread::handle_counters_update() {
  if (_do_counters_update.is_set()) {
    _do_counters_update.unset();
    ShenandoahSemeruHeap::heap()->monitoring_support()->update_counters();
  }
}

void ShenandoahSemeruControlThread::handle_force_counters_update() {
  // Haoran: TODO
  // What's this?
  // if (_force_counters_update.is_set()) {
  //   _do_counters_update.unset(); // reset these too, we do update now!
  //   ShenandoahSemeruHeap::heap()->monitoring_support()->update_counters();
  // }
}

void ShenandoahSemeruControlThread::notify_heap_changed() {
  // This is called from allocation path, and thus should be fast.

  // Update monitoring counters when we took a new region. This amortizes the
  // update costs on slow path.
  if (_do_counters_update.is_unset()) {
    _do_counters_update.set();
  }
  // Notify that something had changed.
  if (_heap_changed.is_unset()) {
    _heap_changed.set();
  }
}

void ShenandoahSemeruControlThread::pacing_notify_alloc(size_t words) {
  assert(ShenandoahPacing, "should only call when pacing is enabled");
  Atomic::add(words, &_allocs_seen);
}

void ShenandoahSemeruControlThread::set_forced_counters_update(bool value) {
  _force_counters_update.set_cond(value);
}

void ShenandoahSemeruControlThread::print() const {
  print_on(tty);
}

void ShenandoahSemeruControlThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahSemeruControlThread::start() {
  create_and_start();
}

void ShenandoahSemeruControlThread::prepare_for_graceful_shutdown() {
  _graceful_shutdown.set();
}

bool ShenandoahSemeruControlThread::in_graceful_shutdown() {
  return _graceful_shutdown.is_set();
}
