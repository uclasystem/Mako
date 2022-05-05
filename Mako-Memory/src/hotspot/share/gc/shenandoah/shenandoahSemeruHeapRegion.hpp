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

#ifndef SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAPREGION_HPP

#include "gc/shared/space.hpp"
#include "gc/shenandoah/makoOffsetTable.hpp"
// Modified by Haoran
#include "gc/shenandoah/shenandoahSemeruAllocRequest.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahSemeruHeap.hpp"
#include "gc/shenandoah/shenandoahPacer.hpp"
#include "memory/universe.hpp"
#include "utilities/sizes.hpp"


// Semeru
#include "gc/shared/rdmaStructure.hpp"    // .hpp not to inlcude inline.hpp, will crash the include hierarchy


class VMStructs;



class ShenandoahCPUToMemoryAtInit : public CHeapRDMAObj< ShenandoahCPUToMemoryAtInit, CPU_TO_MEM_AT_INIT_ALLOCTYPE>{

public:
  // 1.1) These fields are integrated into the super-instance.
  //
  // The index of this region in the heap region sequence.
  size_t _region_number;


  // Declare the  functions in ShenandoahSemeruHeapRegion directly.
  ShenandoahCPUToMemoryAtInit(size_t region_number): _region_number(region_number) {  }



};


class ShenandoahSyncBetweenMemoryAndCPU : public CHeapRDMAObj< ShenandoahSyncBetweenMemoryAndCPU, SYNC_BETWEEN_MEM_AND_CPU_ALLOCTYPE>{

public:

  // override the value in G1SemeruContiguousSpace
  // Used by CPU server for object allocation for both Mutators and GC.
  HeapWord* volatile _top; 
  size_t volatile _state;
  HeapWord* volatile _evac_start;
  // G1SemeruBlockOffsetTablePart _bot_part;  // [?] 1 byte for each card. To record the start object offset for each card.

  ShenandoahSyncBetweenMemoryAndCPU(size_t region_number) :
  _top(NULL),       // set in intialization
  _state(2),
  _evac_start(NULL)
  { }

};

class ShenandoahCPUToMemoryAtGC : public CHeapRDMAObj< ShenandoahCPUToMemoryAtGC, CPU_TO_MEM_AT_GC_ALLOCTYPE>{

public:
  size_t _nothing;
  ShenandoahCPUToMemoryAtGC() :
  _nothing(0)       // set in intialization
  { }

};

class ShenandoahMemoryToCPUAtGC : public CHeapRDMAObj< ShenandoahMemoryToCPUAtGC, MEM_TO_CPU_AT_GC_ALLOCTYPE>{

public:
  volatile size_t _live_data;
  ShenandoahMemoryToCPUAtGC(size_t region_number) :
  _live_data(0)       // set in intialization
  { }

};

// class ShenandoahMemoryToCPUAtGC : public CHeapRDMAObj< ShenandoahMemoryToCPUAtGC, MEM_TO_CPU_AT_GC_ALLOCTYPE>{

// public:

//   // override the value in G1SemeruContiguousSpace
//   // Used by CPU server for object allocation for both Mutators and GC.
//   // HeapWord* volatile _top; 
//   volatile size_t _live_data;
//   // G1SemeruBlockOffsetTablePart _bot_part;  // [?] 1 byte for each card. To record the start object offset for each card.

//   ShenandoahMemoryToCPUAtGC(size_t region_number) :
//   _live_data(0)       // set in intialization
//   { }

// };




// A space in which the free area is contiguous.  It therefore supports
// faster allocation, and compaction.
class SemeruContiguousSpace: public CompactibleSpace {
  friend class VMStructs;
  // Allow scan_and_forward function to call (private) overrides for auxiliary functions on this class
  // template <typename SpaceType>
  // friend void CompactibleSpace::scan_and_forward(SpaceType* space, CompactPoint* cp);

 private:
  // Auxiliary functions for scan_and_forward support.
  // See comments for CompactibleSpace for more information.


  // move to 
  // inline HeapWord* scan_limit() const {
  //   return top();
  // }

  // inline bool scanned_block_is_obj(const HeapWord* addr) const {
  //   return true; // Always true, since scan_limit is top
  // }

  // inline size_t scanned_block_size(const HeapWord* addr) const;

 protected:
  // HeapWord* _top;
  // HeapWord* _concurrent_iteration_safe_limit;
  // A helper for mangling the unused area of the space in debug builds.
  // GenSpaceMangler* _mangler;

  // GenSpaceMangler* mangler() { return _mangler; }

  // Allocation helpers (return NULL if full).
  // inline HeapWord* allocate_impl(size_t word_size);
  // inline HeapWord* par_allocate_impl(size_t word_size);

 public:
  SemeruContiguousSpace();
  ~SemeruContiguousSpace();

  virtual void initialize(MemRegion mr, bool clear_space, bool mangle_space);
  virtual void clear(bool mangle_space);

  // Accessors
  // HeapWord* top() const            { return _top;    }
  // void set_top(HeapWord* value)    { _top = value; }

  // void set_saved_mark()            { _saved_mark_word = top();    }
  // void reset_saved_mark()          { _saved_mark_word = bottom(); }

  // bool saved_mark_at_top() const { return saved_mark_word() == top(); }

  // In debug mode mangle (write it with a particular bit
  // pattern) the unused part of a space.

  // // Used to save the an address in a space for later use during mangling.
  // void set_top_for_allocations(HeapWord* v) PRODUCT_RETURN;
  // // Used to save the space's current top for later use during mangling.
  // void set_top_for_allocations() PRODUCT_RETURN;

  // Mangle regions in the space from the current top up to the
  // previously mangled part of the space.
  void mangle_unused_area();
  // Mangle [top, end)
  void mangle_unused_area_complete();

  // // Do some sparse checking on the area that should have been mangled.
  // void check_mangled_unused_area(HeapWord* limit) PRODUCT_RETURN;
  // // Check the complete area that should have been mangled.
  // // This code may be NULL depending on the macro DEBUG_MANGLING.
  // void check_mangled_unused_area_complete() PRODUCT_RETURN;

  // Size computations: sizes in bytes.
  //size_t capacity() const        { return byte_size(bottom(), end()); }
  //size_t used() const            { return byte_size(bottom(), top()); }
  //size_t free() const            { return byte_size(top(),    end()); }

  virtual bool is_free_block(const HeapWord* p) const {ShouldNotReachHere(); return 0;}

  // In a contiguous space we have a more obvious bound on what parts
  // contain objects.
  //MemRegion used_region() const { return MemRegion(bottom(), top()); }

  // Allocation (return NULL if full)
  // virtual HeapWord* allocate(size_t word_size);
  virtual HeapWord* par_allocate(size_t word_size) {ShouldNotReachHere(); return NULL;}
  // HeapWord* allocate_aligned(size_t word_size);

  // Iteration
  // void oop_iterate(OopIterateClosure* cl);
  void object_iterate(ObjectClosure* blk) {ShouldNotReachHere();}
  // For contiguous spaces this method will iterate safely over objects
  // in the space (i.e., between bottom and top) when at a safepoint.
  void safe_object_iterate(ObjectClosure* blk) {ShouldNotReachHere();}

  // Iterate over as many initialized objects in the space as possible,
  // calling "cl.do_object_careful" on each. Return NULL if all objects
  // in the space (at the start of the iteration) were iterated over.
  // Return an address indicating the extent of the iteration in the
  // event that the iteration had to return because of finding an
  // uninitialized object in the space, or if the closure "cl"
  // signaled early termination.
  // HeapWord* object_iterate_careful(ObjectClosureCareful* cl);
  // HeapWord* concurrent_iteration_safe_limit() {
  //   assert(_concurrent_iteration_safe_limit <= top(),
  //          "_concurrent_iteration_safe_limit update missed");
  //   return _concurrent_iteration_safe_limit;
  // }
  // changes the safe limit, all objects from bottom() to the new
  // limit should be properly initialized
  // void set_concurrent_iteration_safe_limit(HeapWord* new_limit) {
  //   assert(new_limit <= top(), "uninitialized objects in the safe range");
  //   _concurrent_iteration_safe_limit = new_limit;
  // }

  // In support of parallel oop_iterate.
  // template <typename OopClosureType>
  // void par_oop_iterate(MemRegion mr, OopClosureType* blk);

  // Compaction support
  virtual void reset_after_compaction() {
    ShouldNotReachHere();
    // assert(compaction_top() >= bottom() && compaction_top() <= end(), "should point inside space");
    // set_top(compaction_top());
    // // set new iteration safe limit
    // set_concurrent_iteration_safe_limit(compaction_top());
  }

  // Override.
  // DirtyCardToOopClosure* new_dcto_cl(OopIterateClosure* cl,
  //                                    CardTable::PrecisionStyle precision,
  //                                    HeapWord* boundary,
  //                                    bool parallel);

  // Apply "blk->do_oop" to the addresses of all reference fields in objects
  // starting with the _saved_mark_word, which was noted during a generation's
  // save_marks and is required to denote the head of an object.
  // Fields in objects allocated by applications of the closure
  // *are* included in the iteration.
  // Updates _saved_mark_word to point to just after the last object
  // iterated over.
  // template <typename OopClosureType>
  // void oop_since_save_marks_iterate(OopClosureType* blk);

  // // Same as object_iterate, but starting from "mark", which is required
  // // to denote the start of an object.  Objects allocated by
  // // applications of the closure *are* included in the iteration.
  // virtual void object_iterate_from(HeapWord* mark, ObjectClosure* blk);

  // Very inefficient implementation.
  // virtual HeapWord* block_start_const(const void* p) const;
  size_t block_size(const HeapWord* p) const {ShouldNotReachHere(); return 0;}
  // If a block is in the allocated area, it is an object.
  bool block_is_obj(const HeapWord* p) const {ShouldNotReachHere(); return 0; /* return p < top();*/ }

  // Addresses for inlined allocation
  // HeapWord** top_addr() { return &_top; }
  // HeapWord** end_addr() { return &_end; }

// #if INCLUDE_SERIALGC
//   // Overrides for more efficient compaction support.
//   void prepare_for_compaction(CompactPoint* cp);
// #endif

  // virtual void print_on(outputStream* st) const;

  // Checked dynamic downcasts.
  // virtual ContiguousSpace* toContiguousSpace() {
  //   return this;
  // }

  // Debugging
  virtual void verify() const {ShouldNotReachHere();}

  // Used to increase collection frequency.  "factor" of 0 means entire
  // space.
  // void allocate_temporary_filler(int factor);
};





class ShenandoahSemeruHeapRegion : public SemeruContiguousSpace {
  friend class VMStructs;
private:
  /*
    Region state is described by a state machine. Transitions are guarded by
    heap lock, which allows changing the state of several regions atomically.
    Region states can be logically aggregated in groups.

      "Empty":
      .................................................................
      .                                                               .
      .                                                               .
      .         Uncommitted  <-------  Committed <------------------------\
      .              |                     |                          .   |
      .              \---------v-----------/                          .   |
      .                        |                                      .   |
      .........................|.......................................   |
                               |                                          |
      "Active":                |                                          |
      .........................|.......................................   |
      .                        |                                      .   |
      .      /-----------------^-------------------\                  .   |
      .      |                                     |                  .   |
      .      v                                     v    "Humongous":  .   |
      .   Regular ---\-----\     ..................O................  .   |
      .     |  ^     |     |     .                 |               .  .   |
      .     |  |     |     |     .                 *---------\     .  .   |
      .     v  |     |     |     .                 v         v     .  .   |
      .    Pinned  Cset    |     .  HStart <--> H/Start   H/Cont   .  .   |
      .       ^    / |     |     .  Pinned         v         |     .  .   |
      .       |   /  |     |     .                 *<--------/     .  .   |
      .       |  v   |     |     .                 |               .  .   |
      .  CsetPinned  |     |     ..................O................  .   |
      .              |     |                       |                  .   |
      .              \-----\---v-------------------/                  .   |
      .                        |                                      .   |
      .........................|.......................................   |
                               |                                          |
      "Trash":                 |                                          |
      .........................|.......................................   |
      .                        |                                      .   |
      .                        v                                      .   |
      .                      Trash ---------------------------------------/
      .                                                               .
      .                                                               .
      .................................................................

    Transition from "Empty" to "Active" is first allocation. It can go from {Uncommitted, Committed}
    to {Regular, "Humongous"}. The allocation may happen in Regular regions too, but not in Humongous.

    Transition from "Active" to "Trash" is reclamation. It can go from CSet during the normal cycle,
    and from {Regular, "Humongous"} for immediate reclamation. The existence of Trash state allows
    quick reclamation without actual cleaning up.

    Transition from "Trash" to "Empty" is recycling. It cleans up the regions and corresponding metadata.
    Can be done asynchronously and in bulk.

    Note how internal transitions disallow logic bugs:
      a) No region can go Empty, unless properly reclaimed/recycled;
      b) No region can go Uncommitted, unless reclaimed/recycled first;
      c) Only Regular regions can go to CSet;
      d) Pinned cannot go Trash, thus it could never be reclaimed until unpinned;
      e) Pinned cannot go CSet, thus it never moves;
      f) Humongous cannot be used for regular allocations;
      g) Humongous cannot go CSet, thus it never moves;
      h) Humongous start can go pinned, and thus can be protected from moves (humongous continuations should
         follow associated humongous starts, not pinnable/movable by themselves);
      i) Empty cannot go Trash, avoiding useless work;
      j) ...
   */

  enum RegionState {
    _empty_uncommitted,       // region is empty and has memory uncommitted
    _empty_committed,         // region is empty and has memory committed
    _regular,                 // region is for regular allocations
    _humongous_start,         // region is the humongous start
    _humongous_cont,          // region is the humongous continuation
    _pinned_humongous_start,  // region is both humongous start and pinned
    _cset,                    // region is in collection set
    _pinned,                  // region is pinned
    _pinned_cset,             // region is pinned and in cset (evac failure path)
    _trash                    // region contains only trash
  };

  const char* region_state_to_string(RegionState s) const {
    switch (s) {
      case _empty_uncommitted:       return "Empty Uncommitted";
      case _empty_committed:         return "Empty Committed";
      case _regular:                 return "Regular";
      case _humongous_start:         return "Humongous Start";
      case _humongous_cont:          return "Humongous Continuation";
      case _pinned_humongous_start:  return "Humongous Start, Pinned";
      case _cset:                    return "Collection Set";
      case _pinned:                  return "Pinned";
      case _pinned_cset:             return "Collection Set, Pinned";
      case _trash:                   return "Trash";
      default:
        ShouldNotReachHere();
        return "";
    }
  }

  // This method protects from accidental changes in enum order:
  int region_state_to_ordinal(RegionState s) const {
    switch (s) {
      case _empty_uncommitted:      return 0;
      case _empty_committed:        return 1;
      case _regular:                return 2;
      case _humongous_start:        return 3;
      case _humongous_cont:         return 4;
      case _cset:                   return 5;
      case _pinned:                 return 6;
      case _trash:                  return 7;
      case _pinned_cset:            return 8;
      case _pinned_humongous_start: return 9;
      default:
        ShouldNotReachHere();
        return -1;
    }
  }

  void report_illegal_transition(const char* method);

public:
  // Allowed transitions from the outside code:
  void make_regular_allocation();
  void make_regular_bypass();
  void make_humongous_start();
  void make_humongous_cont();
  void make_humongous_start_bypass();
  void make_humongous_cont_bypass();
  void make_pinned();
  void make_unpinned();
  void make_cset();
  void make_trash();
  void make_trash_immediate();
  void make_empty();
  void make_uncommitted();
  void make_committed_bypass();

  // Individual states:
  bool is_empty_uncommitted()      const { return _state == _empty_uncommitted; }
  bool is_empty_committed()        const { return _state == _empty_committed; }
  bool is_regular()                const { return _state == _regular; }
  bool is_humongous_continuation() const { return _state == _humongous_cont; }

  // Participation in logical groups:
  bool is_empty()                  const { return is_empty_committed() || is_empty_uncommitted(); }
  bool is_active()                 const { return !is_empty() && !is_trash(); }
  bool is_trash()                  const { return _state == _trash; }
  bool is_humongous_start()        const { return _state == _humongous_start || _state == _pinned_humongous_start; }
  bool is_humongous()              const { return is_humongous_start() || is_humongous_continuation(); }
  bool is_committed()              const { return !is_empty_uncommitted(); }
  bool is_cset()                   const { return _state == _cset   || _state == _pinned_cset; }
  bool is_pinned()                 const { return _state == _pinned || _state == _pinned_cset || _state == _pinned_humongous_start; }

  // Macro-properties:
  bool is_alloc_allowed()          const { return is_empty() || is_regular() || _state == _pinned; }
  bool is_move_allowed()           const { return is_regular() || _state == _cset || (ShenandoahHumongousMoves && _state == _humongous_start); }

  RegionState state()              const { return _state; }
  void set_state(size_t s)         { _state = static_cast<RegionState>(s); }
  int  state_ordinal()             const { return region_state_to_ordinal(_state); }

private:
  static size_t RegionCount;
  static size_t RegionSizeBytes;
  static size_t RegionSizeWords;
  static size_t RegionSizeBytesShift;
  static size_t RegionSizeWordsShift;
  static size_t RegionSizeBytesMask;
  static size_t RegionSizeWordsMask;
  static size_t HumongousThresholdBytes;
  static size_t HumongousThresholdWords;
  static size_t MaxTLABSizeBytes;
  static size_t MaxTLABSizeWords;

public:
  static int    SemeruLogOfHRGrainBytes;
  static int    SemeruLogOfHRGrainWords;

  static size_t SemeruGrainBytes;   // The Region size, decided in function SemeruHeapRegion::setup_heap_region_size
  static size_t SemeruGrainWords;    


// above should not be sent to memory servers.


private:
  // Global allocation counter, increased for each allocation under Shenandoah heap lock.
  // Padded to avoid false sharing with the read-only fields above.
  struct PaddedAllocSeqNum {
    DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, sizeof(uint64_t));
    uint64_t value;
    DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, 0);

    PaddedAllocSeqNum() {
      // start with 1, reserve 0 for uninitialized value
      value = 1;
    }
  };

  static PaddedAllocSeqNum _alloc_seq_num;

  // Never updated fields
  ShenandoahSemeruHeap* _heap;
  MemRegion _reserved;
  // move to CPU to Mem init
  // size_t _region_number;

  // Rarely updated fields
  HeapWord* _new_top;
  size_t _critical_pins;
  double _empty_time;

  // Seldom updated fields
  RegionState _state;

  // Frequently updated fields
  size_t _tlab_allocs;
  size_t _gclab_allocs;
  size_t _shared_allocs;

  uint64_t _seqnum_first_alloc_mutator;
  uint64_t _seqnum_first_alloc_gc;
  uint64_t _seqnum_last_alloc_mutator;
  uint64_t _seqnum_last_alloc_gc;

  volatile size_t _live_data;


  ShenandoahCPUToMemoryAtInit* _cpu_to_mem_at_init;
  ShenandoahSyncBetweenMemoryAndCPU* _sync_between_mem_and_cpu;
  // ShenandoahCPUToMemoryAtGC* _cpu_to_mem_at_gc;
  ShenandoahMemoryToCPUAtGC* _mem_to_cpu_at_gc;

public:
  HeapWord* _evac_top;

  // Claim some space at the end to protect next region
  // No need because we have align them with 4KB space
  // DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, 0);

public:
  ShenandoahSemeruHeapRegion(ShenandoahSemeruHeap* heap, HeapWord* start, size_t size_words, size_t index, bool committed);

  static const size_t MIN_NUM_REGIONS = 10;

  static void setup_sizes(size_t max_heap_size);

  double empty_time() {
    return _empty_time;
  }

  inline static size_t required_regions(size_t bytes) {
    return (bytes + ShenandoahSemeruHeapRegion::region_size_bytes() - 1) >> ShenandoahSemeruHeapRegion::region_size_bytes_shift();
  }

  inline static size_t region_count() {
    return ShenandoahSemeruHeapRegion::RegionCount;
  }

  inline static size_t region_size_bytes() {
    return ShenandoahSemeruHeapRegion::RegionSizeBytes;
  }

  inline static size_t region_size_words() {
    return ShenandoahSemeruHeapRegion::RegionSizeWords;
  }

  inline static size_t region_size_bytes_shift() {
    return ShenandoahSemeruHeapRegion::RegionSizeBytesShift;
  }

  inline static size_t region_size_words_shift() {
    return ShenandoahSemeruHeapRegion::RegionSizeWordsShift;
  }

  inline static size_t region_size_bytes_mask() {
    return ShenandoahSemeruHeapRegion::RegionSizeBytesMask;
  }

  inline static size_t region_size_words_mask() {
    return ShenandoahSemeruHeapRegion::RegionSizeWordsMask;
  }



  // Semeru
  inline static void setup_semeru_heap_region_size(size_t initial_semeru_heap_size, size_t max_semeru_heap_size) {
    // Confirm their initial value is 0.
    assert(initial_semeru_heap_size == max_semeru_heap_size, 
              "%s, Semeru's initial heap size(0x%llx) must equal to its max size(0x%llx) .", 
              __func__, (unsigned long long)initial_semeru_heap_size, (unsigned long long)max_semeru_heap_size );
    // region size control, use the Semeru specific policy 
    //
    // size_t region_size = G1HeapRegionSize;
    // if (FLAG_IS_DEFAULT(G1HeapRegionSize)) {
    //   size_t average_heap_size = (initial_sermeru_heap_size + max_semeru_heap_size) / 2;
    //   region_size = MAX2(average_heap_size / HeapRegionBounds::target_number(),
    //                      HeapRegionBounds::min_size());
    // }
    // For Semeru memopy pool, Region size equal to the allocation alignment.
    size_t region_size = SemeruMemPoolAlignment;
    int region_size_log = log2_long((jlong) region_size);
    // Recalculate the region size to make sure it's a power of
    // 2. This means that region_size is the largest power of 2 that's
    // <= what we've calculated so far.
    region_size = ((size_t)1 << region_size_log);

    // // Now make sure that we don't go over or under our limits.
    // if (region_size < HeapRegionBounds::min_size()) {
    //   region_size = HeapRegionBounds::min_size();
    // } else if (region_size > HeapRegionBounds::max_size()) {
    //   region_size = HeapRegionBounds::max_size();
    // }

    // And recalculate the log.
    region_size_log = log2_long((jlong) region_size);

    // Now, set up the globals.
    guarantee(SemeruLogOfHRGrainBytes == 0, "we should only set it once");
    SemeruLogOfHRGrainBytes = region_size_log;
    guarantee(SemeruLogOfHRGrainWords == 0, "we should only set it once");
    SemeruLogOfHRGrainWords = SemeruLogOfHRGrainBytes - LogHeapWordSize;
    guarantee(SemeruGrainBytes == 0, "we should only set it once");
    // The cast to int is safe, given that we've bounded region_size by
    // MIN_REGION_SIZE and MAX_REGION_SIZE.
    SemeruGrainBytes = region_size;
    log_info(semeru,heap)("Semeru Heap region size: " SIZE_FORMAT "M", SemeruGrainBytes / M);
    guarantee(SemeruGrainWords == 0, "we should only set it once");
    SemeruGrainWords = SemeruGrainBytes >> LogHeapWordSize;
    guarantee((size_t) 1 << SemeruLogOfHRGrainWords == SemeruGrainWords, "sanity");

    if (SemeruMemPoolAlignment != SemeruGrainBytes) {
      tty->print("%s, Warning! SemeruGrainBytes != SemeruMemPoolAlignment. Reset the SemeruMemPoolAlignment. \n", __func__);
      // Modified by Haoran2
      // FLAG_SET_ERGO(size_t, SemeruMemPoolAlignment, SemeruGrainBytes);
      FLAG_SET_ERGO(SemeruMemPoolAlignment, SemeruGrainBytes);
    }

    // Now, set up the globals.
    guarantee(RegionSizeBytesShift == 0, "we should only set it once");
    RegionSizeBytesShift = (size_t)region_size_log;

    guarantee(RegionSizeWordsShift == 0, "we should only set it once");
    RegionSizeWordsShift = RegionSizeBytesShift - LogHeapWordSize;

    guarantee(RegionSizeBytes == 0, "we should only set it once");
    RegionSizeBytes = region_size;
    RegionSizeWords = RegionSizeBytes >> LogHeapWordSize;
    assert (RegionSizeWords*HeapWordSize == RegionSizeBytes, "sanity");

    guarantee(RegionSizeWordsMask == 0, "we should only set it once");
    RegionSizeWordsMask = RegionSizeWords - 1;

    guarantee(RegionSizeBytesMask == 0, "we should only set it once");
    RegionSizeBytesMask = RegionSizeBytes - 1;
    guarantee(RegionCount == 0, "we should only set it once");
    RegionCount = max_semeru_heap_size / RegionSizeBytes;
    guarantee(RegionCount >= MIN_NUM_REGIONS, "Should have at least minimum regions");

    guarantee(HumongousThresholdWords == 0, "we should only set it once");
    HumongousThresholdWords = RegionSizeWords * ShenandoahHumongousThreshold / 100;
    HumongousThresholdWords = align_down(HumongousThresholdWords, MinObjAlignment);
    assert (HumongousThresholdWords <= RegionSizeWords, "sanity");

    guarantee(HumongousThresholdBytes == 0, "we should only set it once");
    HumongousThresholdBytes = HumongousThresholdWords * HeapWordSize;
    assert (HumongousThresholdBytes <= RegionSizeBytes, "sanity");

    // The rationale for trimming the TLAB sizes has to do with the raciness in
    // TLAB allocation machinery. It may happen that TLAB sizing policy polls Shenandoah
    // about next free size, gets the answer for region #N, goes away for a while, then
    // tries to allocate in region #N, and fail because some other thread have claimed part
    // of the region #N, and then the freeset allocation code has to retire the region #N,
    // before moving the allocation to region #N+1.
    //
    // The worst case realizes when "answer" is "region size", which means it could
    // prematurely retire an entire region. Having smaller TLABs does not fix that
    // completely, but reduces the probability of too wasteful region retirement.
    // With current divisor, we will waste no more than 1/8 of region size in the worst
    // case. This also has a secondary effect on collection set selection: even under
    // the race, the regions would be at least 7/8 used, which allows relying on
    // "used" - "live" for cset selection. Otherwise, we can get the fragmented region
    // below the garbage threshold that would never be considered for collection.
    //
    // The whole thing is mitigated if Elastic TLABs are enabled.
    //
    guarantee(MaxTLABSizeWords == 0, "we should only set it once");
    MaxTLABSizeWords = MIN2(ShenandoahElasticTLAB ? RegionSizeWords : (RegionSizeWords / 8), HumongousThresholdWords);
    MaxTLABSizeWords = align_down(MaxTLABSizeWords, MinObjAlignment);

    guarantee(MaxTLABSizeBytes == 0, "we should only set it once");
    MaxTLABSizeBytes = MaxTLABSizeWords * HeapWordSize;
    assert (MaxTLABSizeBytes > MinTLABSize, "should be larger");

    log_info(gc, init)("Regions: " SIZE_FORMAT " x " SIZE_FORMAT "%s",
                      RegionCount, byte_size_in_proper_unit(RegionSizeBytes), proper_unit_for_byte_size(RegionSizeBytes));
    log_info(gc, init)("Humongous object threshold: " SIZE_FORMAT "%s",
                      byte_size_in_proper_unit(HumongousThresholdBytes), proper_unit_for_byte_size(HumongousThresholdBytes));
    log_info(gc, init)("Max TLAB size: " SIZE_FORMAT "%s",
                      byte_size_in_proper_unit(MaxTLABSizeBytes), proper_unit_for_byte_size(MaxTLABSizeBytes));
  }
  
  inline ShenandoahCPUToMemoryAtInit* cpu_to_mem_at_init() const {return _cpu_to_mem_at_init;}
  inline ShenandoahSyncBetweenMemoryAndCPU* sync_between_mem_and_cpu() const {return _sync_between_mem_and_cpu;}
  inline ShenandoahMemoryToCPUAtGC* mem_to_cpu_at_gc() const {return _mem_to_cpu_at_gc;}


  inline HeapWord* top() const {
    return sync_between_mem_and_cpu()->_top;
  }

  inline HeapWord* scan_limit() const {
    return top();
  }

  HeapWord* volatile* top_addr() { return &(sync_between_mem_and_cpu()->_top);}

  void set_top(HeapWord* value)    { sync_between_mem_and_cpu()->_top = value; }
  void set_saved_mark()            { _saved_mark_word = top();    }
  bool saved_mark_at_top() const { return saved_mark_word() == top(); }

  // Size computations: sizes in bytes.
  size_t capacity() const        { return byte_size(bottom(), end()); }
  size_t used() const            { return byte_size(bottom(), top()); }
  size_t free() const            { return byte_size(top(),    end()); }

  // In a contiguous space we have a more obvious bound on what parts
  // contain objects.
  MemRegion used_region() const { return MemRegion(bottom(), top()); }
  HeapWord** end_addr() { return &_end; }

  //inline size_t region_number() const{ return cpu_to_mem_at_init()->_region_number;}




  // Convert to jint with sanity checking
  inline static jint region_size_bytes_jint() {
    assert (ShenandoahSemeruHeapRegion::RegionSizeBytes <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahSemeruHeapRegion::RegionSizeBytes;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_words_jint() {
    assert (ShenandoahSemeruHeapRegion::RegionSizeWords <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahSemeruHeapRegion::RegionSizeWords;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_bytes_shift_jint() {
    assert (ShenandoahSemeruHeapRegion::RegionSizeBytesShift <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahSemeruHeapRegion::RegionSizeBytesShift;
  }

  // Convert to jint with sanity checking
  inline static jint region_size_words_shift_jint() {
    assert (ShenandoahSemeruHeapRegion::RegionSizeWordsShift <= (size_t)max_jint, "sanity");
    return (jint)ShenandoahSemeruHeapRegion::RegionSizeWordsShift;
  }

  inline static size_t humongous_threshold_bytes() {
    return ShenandoahSemeruHeapRegion::HumongousThresholdBytes;
  }

  inline static size_t humongous_threshold_words() {
    return ShenandoahSemeruHeapRegion::HumongousThresholdWords;
  }

  inline static size_t max_tlab_size_bytes() {
    return ShenandoahSemeruHeapRegion::MaxTLABSizeBytes;
  }

  inline static size_t max_tlab_size_words() {
    return ShenandoahSemeruHeapRegion::MaxTLABSizeWords;
  }

  static uint64_t seqnum_current_alloc() {
    // Last used seq number
    return _alloc_seq_num.value - 1;
  }

  size_t region_number() const;

  // Allocation (return NULL if full)
  inline HeapWord* allocate(size_t word_size, ShenandoahSemeruAllocRequest::Type type);

  HeapWord* allocate(size_t word_size) shenandoah_not_implemented_return(NULL)

  void clear_live_data();
  void set_live_data(size_t s);

  // OffsetTable* offset_table() {
  //   return _offset_table;
  // }

  // void set_offset_table(OffsetTable* t) {
  //   _offset_table = t;
  // }

  // void acquire_offset_table() {
  //   if(_offset_table != NULL) return;
  //   ShenandoahSemeruHeapRegion* corr_region = _heap->get_corr_region(region_number());
  //   OffsetTable* ot = corr_region->offset_table();
  //   assert(ot != NULL, "Invariant!");
  //   // assert(!corr_region->is_active(), "Must be empty or trash!");
  //   ot->set_from_region_start(bottom());
  //   ot->set_to_region_start(corr_region->bottom());
  //   ot->set_region_id(region_number());
  //   set_offset_table(ot);
  //   corr_region->set_offset_table(NULL);
  // }

  // Increase live data for newly allocated region
  inline void increase_live_data_alloc_words(size_t s);

  // Increase live data for region scanned with GC
  // inline void increase_live_data_gc_words(size_t s);
  inline size_t increase_live_data_gc_words(size_t s);

  bool has_live() const;
  size_t get_live_data_bytes() const;
  size_t get_live_data_words() const;

  void print_on(outputStream* st) const;

  size_t garbage() const;

  void recycle();

  void oop_iterate(OopIterateClosure* cl);

  HeapWord* block_start_const(const void* p) const;

  bool in_collection_set() const;

  // Find humongous start region that this region belongs to
  ShenandoahSemeruHeapRegion* humongous_start_region() const;

  CompactibleSpace* next_compaction_space() const shenandoah_not_implemented_return(NULL);
  void prepare_for_compaction(CompactPoint* cp)   shenandoah_not_implemented;
  void adjust_pointers()                          shenandoah_not_implemented;
  void compact()                                  shenandoah_not_implemented;

  void set_new_top(HeapWord* new_top) { _new_top = new_top; }
  HeapWord* new_top() const { return _new_top; }

  inline void adjust_alloc_metadata(ShenandoahSemeruAllocRequest::Type type, size_t);
  void reset_alloc_metadata_to_shared();
  void reset_alloc_metadata();
  size_t get_shared_allocs() const;
  size_t get_tlab_allocs() const;
  size_t get_gclab_allocs() const;

  uint64_t seqnum_first_alloc() const {
    if (_seqnum_first_alloc_mutator == 0) return _seqnum_first_alloc_gc;
    if (_seqnum_first_alloc_gc == 0)      return _seqnum_first_alloc_mutator;
    return MIN2(_seqnum_first_alloc_mutator, _seqnum_first_alloc_gc);
  }

  uint64_t seqnum_last_alloc() const {
    return MAX2(_seqnum_last_alloc_mutator, _seqnum_last_alloc_gc);
  }

  uint64_t seqnum_first_alloc_mutator() const {
    return _seqnum_first_alloc_mutator;
  }

  uint64_t seqnum_last_alloc_mutator()  const {
    return _seqnum_last_alloc_mutator;
  }

  uint64_t seqnum_first_alloc_gc() const {
    return _seqnum_first_alloc_gc;
  }

  uint64_t seqnum_last_alloc_gc()  const {
    return _seqnum_last_alloc_gc;
  }

private:
  void do_commit();
  void do_uncommit();

  void oop_iterate_objects(OopIterateClosure* cl);
  void oop_iterate_humongous(OopIterateClosure* cl);

  inline size_t internal_increase_live_data(size_t s);
};

#endif // SHARE_VM_GC_SHENANDOAH_SEMERU_SHENANDOAHHEAPREGION_HPP
