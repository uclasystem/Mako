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

#ifndef SHARE_VM_GC_SEMERU_SHENANDOAHCOLLECTORPOLICY_HPP
#define SHARE_VM_GC_SEMERU_SHENANDOAHCOLLECTORPOLICY_HPP

// #include "gc/shared/collectorPolicy.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahTracer.hpp"
#include "memory/allocation.hpp" // Modified by Haoran2
#include "utilities/ostream.hpp"

// Modified by Haoran2
// class ShenandoahSemeruCollectorPolicy: public CollectorPolicy {
class ShenandoahSemeruCollectorPolicy: public CHeapObj<mtGC> {
private:

  // Modified by Haoran2
  size_t _min_heap_byte_size;
  size_t _space_alignment;   // [?] Used for OS



  size_t _success_concurrent_gcs;
  size_t _success_degenerated_gcs;
  size_t _success_full_gcs;
  size_t _alloc_failure_degenerated;
  size_t _alloc_failure_degenerated_upgrade_to_full;
  size_t _alloc_failure_full;
  size_t _explicit_concurrent;
  size_t _explicit_full;
  size_t _implicit_concurrent;
  size_t _implicit_full;
  size_t _degen_points[ShenandoahHeap::_DEGENERATED_LIMIT];

  ShenandoahSharedFlag _in_shutdown;

  ShenandoahTracer* _tracer;

  size_t _cycle_counter;

public:
  ShenandoahSemeruCollectorPolicy();


  // Define these parameters in PauselessCollectorPolicy
  size_t  _semeru_max_heap_byte_size;
  size_t  _semeru_initial_heap_byte_size;
  size_t  _semeru_heap_alignment;      // Override the CollectorPolicy->_heap_alignment

  // Abandoned paramters from super class : CollectorPolicy
  //  Override them and assign a , but never use them. 
  size_t _initial_heap_byte_size;
  size_t _max_heap_byte_size;
  //size_t _min_heap_byte_size; // Still use this one.
  //size_t _space_alignment;   // Still use this one.
  size_t _heap_alignment;       // [?] Used for Java heap ?

  // override the virutal functions
  void initialize_alignments();

  void initialize_flags();    
  void initialize_size_info();


  // non-virtual override the 
  void initialize_all() {
    initialize_alignments();
    initialize_flags();
    initialize_size_info();
  }



  // Override the non-virutal functions of base class.
  // These non-virtual function call will be determined in C++/C compilation time.
  // which means that the version of non-virtual function is determined by the class variable, not the real object instance.
  size_t heap_reserved_size_bytes()         { return _semeru_max_heap_byte_size;   }
  size_t semeru_memory_pool_alignment()     { return _semeru_heap_alignment;  }
  // size_t space_alignment()        { return _space_alignment; }
  size_t heap_alignment()         { return _semeru_heap_alignment;  }

  size_t initial_heap_byte_size() { return _semeru_initial_heap_byte_size; }
  size_t max_heap_byte_size()     { return _semeru_max_heap_byte_size; }
  size_t min_heap_byte_size()     { return _min_heap_byte_size; }               // Use the CollectorPolicy default min heap size.



  // TODO: This is different from gc_end: that one encompasses one VM operation.
  // These two encompass the entire cycle.
  void record_cycle_start();

  void record_success_concurrent();
  void record_success_degenerated();
  void record_success_full();
  void record_alloc_failure_to_degenerated(ShenandoahHeap::ShenandoahDegenPoint point);
  void record_alloc_failure_to_full();
  void record_degenerated_upgrade_to_full();
  void record_explicit_to_concurrent();
  void record_explicit_to_full();
  void record_implicit_to_concurrent();
  void record_implicit_to_full();

  void record_shutdown();
  bool is_at_shutdown();

  ShenandoahTracer* tracer() {return _tracer;}

  size_t cycle_counter() const;

  void print_gc_stats(outputStream* out) const;



  void reset_the_abandoned_super_class_fields();
  #ifdef ASSERT

    void assert_flags();

  #endif
};

#endif // SHARE_VM_GC_PAUSELESS_SHENANDOAHCOLLECTORPOLICY_HPP
