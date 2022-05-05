/*
 * Copyright (c) 2018, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHSEMERUALLOCREQUEST_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHSEMERUALLOCREQUEST_HPP

#include "memory/allocation.hpp"

class ShenandoahSemeruAllocRequest : StackObj {
public:
  enum Type {
    _alloc_shared,      // Allocate common, outside of TLAB
    _alloc_shared_gc,   // Allocate common, outside of GCLAB
    _alloc_tlab,        // Allocate TLAB
    _alloc_gclab,       // Allocate GCLAB
    _ALLOC_LIMIT
  };

  static const char* alloc_type_to_string(Type type) {
    switch (type) {
      case _alloc_shared:
        ShouldNotReachHere();
        return "Shared";
      case _alloc_shared_gc:
        return "Shared GC";
      case _alloc_tlab:
        ShouldNotReachHere();
        return "TLAB";
      case _alloc_gclab:
        return "GCLAB";
      default:
        ShouldNotReachHere();
        return "";
    }
  }

private:
  size_t _min_size;
  size_t _requested_size;
  size_t _actual_size;
  Type _alloc_type;

  // Modified by Haoran for evacuation
  size_t _region_index;



#ifdef ASSERT
  bool _actual_size_set;
#endif

  // Modified by Haoran for evacuation
  // ShenandoahSemeruAllocRequest(size_t _min_size, size_t _requested_size, Type _alloc_type) :
  ShenandoahSemeruAllocRequest(size_t _min_size, size_t _requested_size, Type _alloc_type, size_t _region_index = 0) :
          _min_size(_min_size), _requested_size(_requested_size),
          _actual_size(0), _alloc_type(_alloc_type)
          , _region_index(_region_index) /* Modified by Haoran for evacuation*/
#ifdef ASSERT
          , _actual_size_set(false)
#endif
  {}

public:
  static inline ShenandoahSemeruAllocRequest for_tlab(size_t min_size, size_t requested_size) {
    return ShenandoahSemeruAllocRequest(min_size, requested_size, _alloc_tlab);
  }

  // Modified by Haoran for evacuation
  static inline ShenandoahSemeruAllocRequest for_gclab(size_t min_size, size_t requested_size) {
    return ShenandoahSemeruAllocRequest(min_size, requested_size, _alloc_gclab);
  }

  static inline ShenandoahSemeruAllocRequest for_shared_gc(size_t requested_size, size_t region_index) {
    return ShenandoahSemeruAllocRequest(0, requested_size, _alloc_shared_gc, region_index);
  }

  static inline ShenandoahSemeruAllocRequest for_shared(size_t requested_size) {
    return ShenandoahSemeruAllocRequest(0, requested_size, _alloc_shared);
  }

  // Modified by Haoran for evacuation
  inline size_t region_index() {
    return _region_index;
  }




  inline size_t size() {
    return _requested_size;
  }

  inline Type type() {
    return _alloc_type;
  }

  inline size_t min_size() {
    assert (is_lab_alloc(), "Only access for LAB allocs");
    return _min_size;
  }

  inline size_t actual_size() {
    assert (_actual_size_set, "Should be set");
    return _actual_size;
  }

  inline void set_actual_size(size_t v) {
#ifdef ASSERT
    assert (!_actual_size_set, "Should not be set");
    _actual_size_set = true;
#endif
    _actual_size = v;
  }

  inline bool is_mutator_alloc() {
    switch (_alloc_type) {
      case _alloc_tlab:
      case _alloc_shared:
        return true;
      case _alloc_gclab:
      case _alloc_shared_gc:
        return false;
      default:
        ShouldNotReachHere();
        return false;
    }
  }

  inline bool is_gc_alloc() {
    switch (_alloc_type) {
      case _alloc_tlab:
      case _alloc_shared:
        return false;
      case _alloc_gclab:
      case _alloc_shared_gc:
        return true;
      default:
        ShouldNotReachHere();
        return false;
    }
  }

  inline bool is_lab_alloc() {
    switch (_alloc_type) {
      case _alloc_tlab:
      case _alloc_gclab:
        return true;
      case _alloc_shared:
      case _alloc_shared_gc:
        return false;
      default:
        ShouldNotReachHere();
        return false;
    }
  }
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHSEMERUALLOCREQUEST_HPP
