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
// Modified by Haoran
#include "precompiled.hpp"

#include "gc/shenandoah/shenandoahSemeruCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.inline.hpp"
#include "runtime/os.hpp"

ShenandoahSemeruCollectorPolicy::ShenandoahSemeruCollectorPolicy():
  // Modified by Haoran2
  // In semeru we assume min heap size is initial
  _min_heap_byte_size(InitialHeapSize),
  _space_alignment(0),
  _success_concurrent_gcs(0),
  _success_degenerated_gcs(0),
  _success_full_gcs(0),
  _alloc_failure_degenerated(0),
  _alloc_failure_degenerated_upgrade_to_full(0),
  _alloc_failure_full(0),
  _explicit_concurrent(0),
  _explicit_full(0),
  _implicit_concurrent(0),
  _implicit_full(0),
  _cycle_counter(0),
  _semeru_max_heap_byte_size(SemeruMemPoolMaxSize),
  _semeru_initial_heap_byte_size(SemeruMemPoolInitialSize),
  _semeru_heap_alignment(0) {

//   Copy::zero_to_bytes(_degen_points, sizeof(size_t) * ShenandoahHeap::_DEGENERATED_LIMIT);

//   ShenandoahHeapRegion::setup_sizes(max_heap_byte_size());

//   initialize_all();

//   _tracer = new (ResourceObj::C_HEAP, mtGC) ShenandoahTracer();


  Copy::zero_to_bytes(_degen_points, sizeof(size_t) * ShenandoahSemeruHeap::_DEGENERATED_LIMIT);


  ShenandoahSemeruHeapRegion::setup_semeru_heap_region_size(SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);
  //ShenandoahSemeruHeapRegion::setup_sizes(max_heap_byte_size());
  // Semeru
  reset_the_abandoned_super_class_fields();

}

void ShenandoahSemeruCollectorPolicy::initialize_alignments() {
  // This is expected by our algorithm for ShenandoahHeap::heap_region_containing().
  size_t align = ShenandoahSemeruHeapRegion::region_size_bytes();
  if (UseLargePages) {
    align = MAX2(align, os::large_page_size());
  }
  _space_alignment = align;
  
  
  //_heap_alignment = align;
  _semeru_heap_alignment = MAX2(ShenandoahSemeruHeapRegion::SemeruGrainBytes, align);
  log_info(heap)("%s, _semeru_heap_alignment is 0x%llx ", __func__, 
                              (unsigned long long)_semeru_heap_alignment);

}




void ShenandoahSemeruCollectorPolicy::record_explicit_to_concurrent() {
  _explicit_concurrent++;
}

void ShenandoahSemeruCollectorPolicy::record_explicit_to_full() {
  _explicit_full++;
}

void ShenandoahSemeruCollectorPolicy::record_implicit_to_concurrent() {
  _implicit_concurrent++;
}

void ShenandoahSemeruCollectorPolicy::record_implicit_to_full() {
  _implicit_full++;
}

void ShenandoahSemeruCollectorPolicy::record_alloc_failure_to_full() {
  _alloc_failure_full++;
}


// Haoran: TODO
// What is this
void ShenandoahSemeruCollectorPolicy::record_alloc_failure_to_degenerated(ShenandoahHeap::ShenandoahDegenPoint point) {
  assert(point < ShenandoahHeap::_DEGENERATED_LIMIT, "sanity");
  _alloc_failure_degenerated++;
  _degen_points[point]++;
}

void ShenandoahSemeruCollectorPolicy::record_degenerated_upgrade_to_full() {
  _alloc_failure_degenerated_upgrade_to_full++;
}

void ShenandoahSemeruCollectorPolicy::record_success_concurrent() {
  _success_concurrent_gcs++;
}

void ShenandoahSemeruCollectorPolicy::record_success_degenerated() {
  _success_degenerated_gcs++;
}

void ShenandoahSemeruCollectorPolicy::record_success_full() {
  _success_full_gcs++;
}

size_t ShenandoahSemeruCollectorPolicy::cycle_counter() const {
  return _cycle_counter;
}

void ShenandoahSemeruCollectorPolicy::record_cycle_start() {
  _cycle_counter++;
}

void ShenandoahSemeruCollectorPolicy::record_shutdown() {
  _in_shutdown.set();
}

bool ShenandoahSemeruCollectorPolicy::is_at_shutdown() {
  return _in_shutdown.is_set();
}

void ShenandoahSemeruCollectorPolicy::print_gc_stats(outputStream* out) const {
  out->print_cr("Under allocation pressure, concurrent cycles may cancel, and either continue cycle");
  out->print_cr("under stop-the-world pause or result in stop-the-world Full GC. Increase heap size,");
  out->print_cr("tune GC heuristics, set more aggressive pacing delay, or lower allocation rate");
  out->print_cr("to avoid Degenerated and Full GC cycles.");
  out->cr();

  out->print_cr(SIZE_FORMAT_W(5) " successful concurrent GCs",         _success_concurrent_gcs);
  out->print_cr("  " SIZE_FORMAT_W(5) " invoked explicitly",           _explicit_concurrent);
  out->print_cr("  " SIZE_FORMAT_W(5) " invoked implicitly",           _implicit_concurrent);
  out->cr();

  out->print_cr(SIZE_FORMAT_W(5) " Degenerated GCs",                   _success_degenerated_gcs);
  out->print_cr("  " SIZE_FORMAT_W(5) " caused by allocation failure", _alloc_failure_degenerated);
  for (int c = 0; c < ShenandoahHeap::_DEGENERATED_LIMIT; c++) {
    if (_degen_points[c] > 0) {
      const char* desc = ShenandoahHeap::degen_point_to_string((ShenandoahHeap::ShenandoahDegenPoint)c);
      out->print_cr("    " SIZE_FORMAT_W(5) " happened at %s",         _degen_points[c], desc);
    }
  }
  out->print_cr("  " SIZE_FORMAT_W(5) " upgraded to Full GC",          _alloc_failure_degenerated_upgrade_to_full);
  out->cr();

  out->print_cr(SIZE_FORMAT_W(5) " Full GCs",                          _success_full_gcs + _alloc_failure_degenerated_upgrade_to_full);
  out->print_cr("  " SIZE_FORMAT_W(5) " invoked explicitly",           _explicit_full);
  out->print_cr("  " SIZE_FORMAT_W(5) " invoked implicitly",           _implicit_full);
  out->print_cr("  " SIZE_FORMAT_W(5) " caused by allocation failure", _alloc_failure_full);
  out->print_cr("  " SIZE_FORMAT_W(5) " upgraded from Degenerated GC", _alloc_failure_degenerated_upgrade_to_full);
}




void ShenandoahSemeruCollectorPolicy::initialize_flags() {
  assert(_space_alignment != 0, "Space alignment not set up properly");
  assert(_semeru_heap_alignment != 0, " Semeru Heap alignment not set up properly");
  assert(_semeru_heap_alignment >= _space_alignment,
         "_semeru_heap_alignment: " SIZE_FORMAT " less than space_alignment: " SIZE_FORMAT,
         _semeru_heap_alignment, _space_alignment);
  assert(_semeru_heap_alignment % _space_alignment == 0,
         "_semeru_heap_alignment: " SIZE_FORMAT " not aligned by space_alignment: " SIZE_FORMAT,
         _semeru_heap_alignment, _space_alignment);

  if (FLAG_IS_CMDLINE(SemeruMemPoolMaxSize)) {
    if (FLAG_IS_CMDLINE(SemeruMemPoolInitialSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
      vm_exit_during_initialization("Semeru Initial heap size set to a larger value than the Semeru maximum heap size");
    }
    if (_min_heap_byte_size != 0 && SemeruMemPoolMaxSize < _min_heap_byte_size) {
      vm_exit_during_initialization("Incompatible Semeru minimum and Semeru maximum heap sizes specified");
    }
  }

  // Check heap parameter properties
  if (SemeruMemPoolMaxSize < 2 * M) {
    vm_exit_during_initialization("Too small Semeru maximum heap");
  }
  if (SemeruMemPoolInitialSize < M) {
    vm_exit_during_initialization("Too small Semeru initial heap");
  }
  if (_min_heap_byte_size < M) {
    vm_exit_during_initialization("Too small Semeru minimum heap");
  }

  // User inputs from -Xmx and -Xms must be aligned
  _min_heap_byte_size = align_up(_min_heap_byte_size, _semeru_heap_alignment);
  size_t aligned_initial_heap_size = align_up(SemeruMemPoolInitialSize, _semeru_heap_alignment);
  size_t aligned_max_heap_size = align_up(SemeruMemPoolMaxSize, _semeru_heap_alignment);

  // Write back to flags if the values changed
  if (aligned_initial_heap_size != SemeruMemPoolInitialSize) {
    // Modified by Haoran2
    FLAG_SET_ERGO(SemeruMemPoolInitialSize, aligned_initial_heap_size);
  }

  if (aligned_max_heap_size != SemeruMemPoolMaxSize) {
    // Modified by Haoran2
    FLAG_SET_ERGO(SemeruMemPoolMaxSize, aligned_max_heap_size);
  }

  if (FLAG_IS_CMDLINE(SemeruMemPoolInitialSize) && _min_heap_byte_size != 0 &&
      SemeruMemPoolInitialSize < _min_heap_byte_size) {
    vm_exit_during_initialization("Incompatible minimum and initial heap sizes specified");
  }
  if (!FLAG_IS_DEFAULT(SemeruMemPoolInitialSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
    // Modified by Haoran2
    FLAG_SET_ERGO(SemeruMemPoolMaxSize, SemeruMemPoolInitialSize);
  } else if (!FLAG_IS_DEFAULT(SemeruMemPoolMaxSize) && SemeruMemPoolInitialSize > SemeruMemPoolMaxSize) {
    // Modified by Haoran2
    FLAG_SET_ERGO(SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);
    if (SemeruMemPoolInitialSize < _min_heap_byte_size) {
      _min_heap_byte_size = SemeruMemPoolInitialSize;
    }
  }

  _semeru_initial_heap_byte_size = SemeruMemPoolInitialSize;
  _semeru_max_heap_byte_size = SemeruMemPoolMaxSize;

  // Semeru don't have such a glabal field, MinHeapDeltaBytes
  //FLAG_SET_ERGO(size_t, MinHeapDeltaBytes, align_up(MinHeapDeltaBytes, _space_alignment));
}




// Handle the abandoned fields from super class, CollectorPolicy.
void ShenandoahSemeruCollectorPolicy::reset_the_abandoned_super_class_fields(){
  _initial_heap_byte_size = 0;
  _max_heap_byte_size     = 0;
  //size_t _min_heap_byte_size; // Still use this one.

  // size_t _space_alignment;   // Still use this one.
  _heap_alignment         = 0;       
}


void ShenandoahSemeruCollectorPolicy::initialize_size_info() {
  log_debug(gc, heap)("Semeru Minimum heap " SIZE_FORMAT "Semeru Initial heap " SIZE_FORMAT " Semeru Maximum heap " SIZE_FORMAT,
                      _min_heap_byte_size, SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);

  //DEBUG_ONLY(CollectorPolicy::assert_size_info();)
}


#ifdef ASSERT

void ShenandoahSemeruCollectorPolicy::assert_flags() {

  // abandoned super class, CollectorPolicy, flags.
  assert(_initial_heap_byte_size == 0, "_initial_heap_byte_size(0x%llx) muse be abandoned in Semeru.", 
                                                               (unsigned long long)_initial_heap_byte_size);
  assert(_max_heap_byte_size == 0, "_max_heap_byte_size(0x%llx)  muse be abandoned in Semeru.",
                                                               (unsigned long long)_max_heap_byte_size);
  assert(_heap_alignment == 0, "_heap_alignment(0x%llx) muse be abandoned in Semeru.",
                                                               (unsigned long long)_heap_alignment );

  // Semeru flags 
  assert(SemeruMemPoolInitialSize <= SemeruMemPoolMaxSize, "Ergonomics decided on incompatible SemeruMemPoolInitialSize and maximum heap sizes");
  assert(SemeruMemPoolInitialSize % _semeru_heap_alignment == 0, "SemeruMemPoolInitialSize alignment");
  assert(SemeruMemPoolMaxSize % _semeru_heap_alignment == 0, "SemeruMemPoolMaxSize alignment");
}

#endif
