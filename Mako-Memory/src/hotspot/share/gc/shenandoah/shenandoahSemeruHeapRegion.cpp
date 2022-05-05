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
#include "memory/allocation.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.inline.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahSemeruHeapRegion.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahTraversalGC.hpp"
#include "gc/shared/space.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"



// Semeru

SemeruContiguousSpace::SemeruContiguousSpace(): CompactibleSpace() {
}

SemeruContiguousSpace::~SemeruContiguousSpace() {
}

void SemeruContiguousSpace::initialize(MemRegion mr,
                                 bool clear_space,
                                 bool mangle_space)
{
  CompactibleSpace::initialize(mr, clear_space, mangle_space);
  // set_concurrent_iteration_safe_limit(top());
}

void SemeruContiguousSpace::clear(bool mangle_space) {
  // set_top(bottom());
  // set_saved_mark();
  CompactibleSpace::clear(mangle_space);
}

void SemeruContiguousSpace::mangle_unused_area() {
  ShouldNotReachHere();
}

void SemeruContiguousSpace::mangle_unused_area_complete() {
  ShouldNotReachHere();
}

// void SemeruContiguousSpace::is_free_block() const{
//   ShouldNotReachHere();
// }






size_t ShenandoahSemeruHeapRegion::RegionCount = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeBytes = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeWords = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeBytesShift = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeWordsShift = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeBytesMask = 0;
size_t ShenandoahSemeruHeapRegion::RegionSizeWordsMask = 0;
size_t ShenandoahSemeruHeapRegion::HumongousThresholdBytes = 0;
size_t ShenandoahSemeruHeapRegion::HumongousThresholdWords = 0;
size_t ShenandoahSemeruHeapRegion::MaxTLABSizeBytes = 0;
size_t ShenandoahSemeruHeapRegion::MaxTLABSizeWords = 0;

//
// Semeru heap control.
//
int    ShenandoahSemeruHeapRegion::SemeruLogOfHRGrainBytes = 0;
int    ShenandoahSemeruHeapRegion::SemeruLogOfHRGrainWords = 0;
size_t ShenandoahSemeruHeapRegion::SemeruGrainBytes        = 0;   // Semeru allocation alignment && Region size.
size_t ShenandoahSemeruHeapRegion::SemeruGrainWords        = 0; 

ShenandoahSemeruHeapRegion::PaddedAllocSeqNum ShenandoahSemeruHeapRegion::_alloc_seq_num;

ShenandoahSemeruHeapRegion::ShenandoahSemeruHeapRegion(ShenandoahSemeruHeap* heap, HeapWord* start,
                                           size_t size_words, size_t index, bool committed) :
  _heap(heap),
  _reserved(MemRegion(start, size_words)),
  /*_region_number(index),*/
  _new_top(NULL),
  _critical_pins(0),
  _empty_time(os::elapsedTime()),
  _state(committed ? _empty_committed : _empty_uncommitted),
  _tlab_allocs(0),
  _gclab_allocs(0),
  _shared_allocs(0),
  _seqnum_first_alloc_mutator(0),
  _seqnum_first_alloc_gc(0),
  _seqnum_last_alloc_mutator(0),
  _seqnum_last_alloc_gc(0),
  _evac_top(NULL)
  {

  SemeruContiguousSpace::initialize(_reserved, true, committed);
  _cpu_to_mem_at_init = new(index) ShenandoahCPUToMemoryAtInit(index);
  _sync_between_mem_and_cpu = new(index) ShenandoahSyncBetweenMemoryAndCPU(index);
  // _cpu_to_mem_at_gc = new(index) ShenandoahCPUToMemoryAtGC();
  _mem_to_cpu_at_gc = new(index) ShenandoahMemoryToCPUAtGC(index);
}

size_t ShenandoahSemeruHeapRegion::region_number() const {
  return cpu_to_mem_at_init()->_region_number;
  //return _region_number;
}

void ShenandoahSemeruHeapRegion::report_illegal_transition(const char *method) {
  ResourceMark rm;
  stringStream ss;
  ss.print("Illegal region state transition from \"%s\", at %s\n  ", region_state_to_string(_state), method);
  print_on(&ss);
  fatal("%s", ss.as_string());
}

void ShenandoahSemeruHeapRegion::make_regular_allocation() {
  _heap->assert_heaplock_owned_by_current_thread();

  switch (_state) {
    case _empty_uncommitted:
      do_commit();
    case _empty_committed:
      _state = _regular;
    case _regular:
    case _pinned:
      return;
    default:
      report_illegal_transition("regular allocation");
  }
}

void ShenandoahSemeruHeapRegion::make_regular_bypass() {
  // Modified by Haoran for compaction
  // Haoran: debug
  ShouldNotReachHere();
  

  _heap->assert_heaplock_owned_by_current_thread();
  assert (_heap->is_full_gc_in_progress() || _heap->is_degenerated_gc_in_progress(),
          "only for full or degen GC");

  switch (_state) {
    case _empty_uncommitted:
      do_commit();
    case _empty_committed:
    case _cset:
    case _humongous_start:
    case _humongous_cont:
      _state = _regular;
      return;
    case _pinned_cset:
      _state = _pinned;
      return;
    case _regular:
    case _pinned:
      return;
    default:
      report_illegal_transition("regular bypass");
  }
}

void ShenandoahSemeruHeapRegion::make_humongous_start() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _empty_uncommitted:
      do_commit();
    case _empty_committed:
      _state = _humongous_start;
      return;
    default:
      report_illegal_transition("humongous start allocation");
  }
}

void ShenandoahSemeruHeapRegion::make_humongous_start_bypass() {
  _heap->assert_heaplock_owned_by_current_thread();
  assert (_heap->is_full_gc_in_progress(), "only for full GC");

  switch (_state) {
    case _empty_committed:
    case _regular:
    case _humongous_start:
    case _humongous_cont:
      _state = _humongous_start;
      return;
    default:
      report_illegal_transition("humongous start bypass");
  }
}

void ShenandoahSemeruHeapRegion::make_humongous_cont() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _empty_uncommitted:
      do_commit();
    case _empty_committed:
      _state = _humongous_cont;
      return;
    default:
      report_illegal_transition("humongous continuation allocation");
  }
}

void ShenandoahSemeruHeapRegion::make_humongous_cont_bypass() {
  _heap->assert_heaplock_owned_by_current_thread();
  assert (_heap->is_full_gc_in_progress(), "only for full GC");

  switch (_state) {
    case _empty_committed:
    case _regular:
    case _humongous_start:
    case _humongous_cont:
      _state = _humongous_cont;
      return;
    default:
      report_illegal_transition("humongous continuation bypass");
  }
}

void ShenandoahSemeruHeapRegion::make_pinned() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _regular:
      assert (_critical_pins == 0, "sanity");
      _state = _pinned;
    case _pinned_cset:
    case _pinned:
      _critical_pins++;
      return;
    case _humongous_start:
      assert (_critical_pins == 0, "sanity");
      _state = _pinned_humongous_start;
    case _pinned_humongous_start:
      _critical_pins++;
      return;
    case _cset:
      guarantee(_heap->cancelled_gc(), "only valid when evac has been cancelled");
      assert (_critical_pins == 0, "sanity");
      _state = _pinned_cset;
      _critical_pins++;
      return;
    default:
      report_illegal_transition("pinning");
  }
}

void ShenandoahSemeruHeapRegion::make_unpinned() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _pinned:
      assert (_critical_pins > 0, "sanity");
      _critical_pins--;
      if (_critical_pins == 0) {
        _state = _regular;
      }
      return;
    case _regular:
    case _humongous_start:
      assert (_critical_pins == 0, "sanity");
      return;
    case _pinned_cset:
      guarantee(_heap->cancelled_gc(), "only valid when evac has been cancelled");
      assert (_critical_pins > 0, "sanity");
      _critical_pins--;
      if (_critical_pins == 0) {
        _state = _cset;
      }
      return;
    case _pinned_humongous_start:
      assert (_critical_pins > 0, "sanity");
      _critical_pins--;
      if (_critical_pins == 0) {
        _state = _humongous_start;
      }
      return;
    default:
      report_illegal_transition("unpinning");
  }
}

void ShenandoahSemeruHeapRegion::make_cset() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _regular:
      _state = _cset;
    case _cset:
      return;
    default:
      report_illegal_transition("cset");
  }
}

void ShenandoahSemeruHeapRegion::make_trash() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _cset:
      // Reclaiming cset regions
    case _humongous_start:
    case _humongous_cont:
      // Reclaiming humongous regions
    case _regular:
      // Immediate region reclaim
      _state = _trash;
      return;
    default:
      report_illegal_transition("trashing");
  }
}

void ShenandoahSemeruHeapRegion::make_trash_immediate() {
  make_trash();

  // On this path, we know there are no marked objects in the region,
  // tell marking context about it to bypass bitmap resets.
  _heap->complete_marking_context()->semeru_reset_top_bitmap(this);
}

void ShenandoahSemeruHeapRegion::make_empty() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _trash:
      _state = _empty_committed;
      _empty_time = os::elapsedTime();
      return;
    default:
      report_illegal_transition("emptying");
  }
}

void ShenandoahSemeruHeapRegion::make_uncommitted() {
  _heap->assert_heaplock_owned_by_current_thread();
  switch (_state) {
    case _empty_committed:
      do_uncommit();
      _state = _empty_uncommitted;
      return;
    default:
      report_illegal_transition("uncommiting");
  }
}

void ShenandoahSemeruHeapRegion::make_committed_bypass() {
  _heap->assert_heaplock_owned_by_current_thread();
  assert (_heap->is_full_gc_in_progress(), "only for full GC");

  switch (_state) {
    case _empty_uncommitted:
      do_commit();
      _state = _empty_committed;
      return;
    default:
      report_illegal_transition("commit bypass");
  }
}

void ShenandoahSemeruHeapRegion::clear_live_data() {
  OrderAccess::release_store_fence<size_t>(&(_mem_to_cpu_at_gc->_live_data), 0);
}

void ShenandoahSemeruHeapRegion::reset_alloc_metadata() {
  _tlab_allocs = 0;
  _gclab_allocs = 0;
  _shared_allocs = 0;
  _seqnum_first_alloc_mutator = 0;
  _seqnum_last_alloc_mutator = 0;
  _seqnum_first_alloc_gc = 0;
  _seqnum_last_alloc_gc = 0;
}

void ShenandoahSemeruHeapRegion::reset_alloc_metadata_to_shared() {
  if (used() > 0) {
    _tlab_allocs = 0;
    _gclab_allocs = 0;
    _shared_allocs = used() >> LogHeapWordSize;
    uint64_t next = _alloc_seq_num.value++;
    _seqnum_first_alloc_mutator = next;
    _seqnum_last_alloc_mutator = next;
    _seqnum_first_alloc_gc = 0;
    _seqnum_last_alloc_gc = 0;
  } else {
    reset_alloc_metadata();
  }
}

size_t ShenandoahSemeruHeapRegion::get_shared_allocs() const {
  return _shared_allocs * HeapWordSize;
}

size_t ShenandoahSemeruHeapRegion::get_tlab_allocs() const {
  return _tlab_allocs * HeapWordSize;
}

size_t ShenandoahSemeruHeapRegion::get_gclab_allocs() const {
  return _gclab_allocs * HeapWordSize;
}

void ShenandoahSemeruHeapRegion::set_live_data(size_t s) {
  assert(Thread::current()->is_VM_thread(), "by VM thread");
  _mem_to_cpu_at_gc->_live_data = (s >> LogHeapWordSize);
}

size_t ShenandoahSemeruHeapRegion::get_live_data_words() const {
  return OrderAccess::load_acquire(&(_mem_to_cpu_at_gc->_live_data));
}

size_t ShenandoahSemeruHeapRegion::get_live_data_bytes() const {
  return get_live_data_words() * HeapWordSize;
}

bool ShenandoahSemeruHeapRegion::has_live() const {
  return get_live_data_words() != 0;
}

size_t ShenandoahSemeruHeapRegion::garbage() const {
  assert(used() >= get_live_data_bytes(), "Live Data must be a subset of used() live: " SIZE_FORMAT " used: " SIZE_FORMAT,
         get_live_data_bytes(), used());

  size_t result = used() - get_live_data_bytes();
  return result;
}

void ShenandoahSemeruHeapRegion::print_on(outputStream* st) const {
  st->print("|");
  st->print(SIZE_FORMAT_W(5), this->region_number());

  switch (_state) {
    case _empty_uncommitted:
      st->print("|EU ");
      break;
    case _empty_committed:
      st->print("|EC ");
      break;
    case _regular:
      st->print("|R  ");
      break;
    case _humongous_start:
      st->print("|H  ");
      break;
    case _pinned_humongous_start:
      st->print("|HP ");
      break;
    case _humongous_cont:
      st->print("|HC ");
      break;
    case _cset:
      st->print("|CS ");
      break;
    case _trash:
      st->print("|T  ");
      break;
    case _pinned:
      st->print("|P  ");
      break;
    case _pinned_cset:
      st->print("|CSP");
      break;
    default:
      ShouldNotReachHere();
  }
  st->print("|BTE " INTPTR_FORMAT_W(12) ", " INTPTR_FORMAT_W(12) ", " INTPTR_FORMAT_W(12),
            p2i(bottom()), p2i(top()), p2i(end()));
  st->print("|TAMS " INTPTR_FORMAT_W(12),
            p2i(_heap->marking_context()->semeru_top_at_mark_start(const_cast<ShenandoahSemeruHeapRegion*>(this))));
  st->print("|U " SIZE_FORMAT_W(5) "%1s", byte_size_in_proper_unit(used()),                proper_unit_for_byte_size(used()));
  st->print("|T " SIZE_FORMAT_W(5) "%1s", byte_size_in_proper_unit(get_tlab_allocs()),     proper_unit_for_byte_size(get_tlab_allocs()));
  st->print("|G " SIZE_FORMAT_W(5) "%1s", byte_size_in_proper_unit(get_gclab_allocs()),    proper_unit_for_byte_size(get_gclab_allocs()));
  st->print("|S " SIZE_FORMAT_W(5) "%1s", byte_size_in_proper_unit(get_shared_allocs()),   proper_unit_for_byte_size(get_shared_allocs()));
  st->print("|L " SIZE_FORMAT_W(5) "%1s", byte_size_in_proper_unit(get_live_data_bytes()), proper_unit_for_byte_size(get_live_data_bytes()));
  st->print("|CP " SIZE_FORMAT_W(3), _critical_pins);
  st->print("|SN " UINT64_FORMAT_X_W(12) ", " UINT64_FORMAT_X_W(8) ", " UINT64_FORMAT_X_W(8) ", " UINT64_FORMAT_X_W(8),
            seqnum_first_alloc_mutator(), seqnum_last_alloc_mutator(),
            seqnum_first_alloc_gc(), seqnum_last_alloc_gc());
  st->cr();
}

void ShenandoahSemeruHeapRegion::oop_iterate(OopIterateClosure* blk) {
  if (!is_active()) return;
  if (is_humongous()) {
    oop_iterate_humongous(blk);
  } else {
    oop_iterate_objects(blk);
  }
}

void ShenandoahSemeruHeapRegion::oop_iterate_objects(OopIterateClosure* blk) {
  assert(! is_humongous(), "no humongous region here");
  // Modified by Haoran2
  // HeapWord* obj_addr = bottom() + ShenandoahBrooksPointer::word_size();
  HeapWord* obj_addr = bottom();
  HeapWord* t = top();
  // Could call objects iterate, but this is easier.
  while (obj_addr < t) {
    oop obj = oop(obj_addr);
    // Modified by Haoran2
    // obj_addr += obj->oop_iterate_size(blk) + ShenandoahBrooksPointer::word_size();
    obj_addr += obj->oop_iterate_size(blk);
  }
}

void ShenandoahSemeruHeapRegion::oop_iterate_humongous(OopIterateClosure* blk) {
  assert(is_humongous(), "only humongous region here");
  // Find head.
  ShenandoahSemeruHeapRegion* r = humongous_start_region();
  assert(r->is_humongous_start(), "need humongous head here");
  // Modified by Haoran2
  // oop obj = oop(r->bottom() + ShenandoahBrooksPointer::word_size());
  oop obj = oop(r->bottom());
  obj->oop_iterate(blk, MemRegion(bottom(), top()));
}

ShenandoahSemeruHeapRegion* ShenandoahSemeruHeapRegion::humongous_start_region() const {
  assert(is_humongous(), "Must be a part of the humongous region");
  size_t reg_num = region_number();
  ShenandoahSemeruHeapRegion* r = const_cast<ShenandoahSemeruHeapRegion*>(this);
  while (!r->is_humongous_start()) {
    assert(reg_num > 0, "Sanity");
    reg_num --;
    r = _heap->get_region(reg_num);
    assert(r->is_humongous(), "Must be a part of the humongous region");
  }
  assert(r->is_humongous_start(), "Must be");
  return r;
}

void ShenandoahSemeruHeapRegion::recycle() {
  SemeruContiguousSpace::clear(false);
  if (ZapUnusedHeapArea) {
    // Semeru
    // Haoran: TODO check if this function is userful
    // SemeruContiguousSpace::mangle_unused_area_complete();
  }
  clear_live_data();

  reset_alloc_metadata();

  _heap->marking_context()->semeru_reset_top_at_mark_start(this);

  make_empty();
}

HeapWord* ShenandoahSemeruHeapRegion::block_start_const(const void* p) const {
  assert(MemRegion(bottom(), end()).contains(p),
         "p (" PTR_FORMAT ") not in space [" PTR_FORMAT ", " PTR_FORMAT ")",
         p2i(p), p2i(bottom()), p2i(end()));
  if (p >= top()) {
    return top();
  } else {
    // Modified by Haoran2
    // HeapWord* last = bottom() + ShenandoahBrooksPointer::word_size();
    HeapWord* last = bottom();
    HeapWord* cur = last;
    while (cur <= p) {
      last = cur;
      // Modified by Haoran2
      // cur += oop(cur)->size() + ShenandoahBrooksPointer::word_size();
      cur += oop(cur)->size();
    }
    shenandoah_assert_correct(NULL, oop(last));
    return last;
  }
}

void ShenandoahSemeruHeapRegion::setup_sizes(size_t max_heap_size) {

  ShouldNotReachHere();
}

void ShenandoahSemeruHeapRegion::do_commit() {
  if (!_heap->is_heap_region_special() && !os::commit_memory((char *) _reserved.start(), _reserved.byte_size(), false)) {
    report_java_out_of_memory("Unable to commit region");
  }
  if (!_heap->commit_bitmap_slice(this)) {
    report_java_out_of_memory("Unable to commit bitmaps for region");
  }
  _heap->increase_committed(ShenandoahSemeruHeapRegion::region_size_bytes());
}

void ShenandoahSemeruHeapRegion::do_uncommit() {
  if (!_heap->is_heap_region_special() && !os::uncommit_memory((char *) _reserved.start(), _reserved.byte_size())) {
    report_java_out_of_memory("Unable to uncommit region");
  }
  if (!_heap->uncommit_bitmap_slice(this)) {
    report_java_out_of_memory("Unable to uncommit bitmaps for region");
  }
  _heap->decrease_committed(ShenandoahSemeruHeapRegion::region_size_bytes());
}
