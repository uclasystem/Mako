#ifndef MAKO_OFFSETTABLE_HPP
#define MAKO_OFFSETTABLE_HPP

#include "memory/virtualspace.hpp"
#include "memory/memRegion.hpp"
#include "utilities/globalDefinitions.hpp"

// class OffsetTable : public CHeapObj<mtInternal> {
//   size_t _region_id;
//   MemRegion _table;
//   size_t _base;
//   size_t _from_region_start;
//   size_t _to_region_start;
//   // extra offset caused by root objects, 1 = 16 bytes
//   size_t _root_offset_in_dwords;
//   size_t _table_byte_size;

//   size_t _region_byte_size;
//   static const size_t heap_alignment_byte_size = 16;
//   static const size_t log_heap_alignment_byte_size = 4;
//   static const size_t offset_byte_size = 2;

//   size_t addr_to_offset(HeapWord* addr, size_t region_start);
//   HeapWord* offset_to_addr(size_t offset, size_t region_start);

// public:
//   OffsetTable(size_t region_id, size_t region_size);
//   // do not consider _root_offset_in_dwords by default
//   void set(HeapWord* from, HeapWord* to);
//   // set to-space offset directly with the offset, instead of to-space address
//   // do not consider _root_offset_in_dwords by default, add to to_offset before calling if needed
//   void set_using_byte_offset(HeapWord* from, size_t to_offset);
//   void set_using_offset(HeapWord* from, size_t to_offset);
//   // given a from-space address of a root object, subtract root offset from its offset entry
//   // Then, during reference update, all offset can be safely calculated by adding the root offset
//   void subtract_root_offset(HeapWord* from_addr);
//   HeapWord* get(HeapWord* from);
//   // get to-space address with _root_offset_in_dwords
//   HeapWord* get_with_root_offset(HeapWord* from);
//   size_t root_offset_in_dwords();
//   void set_root_offset_in_dwords(size_t to_offset);
//   void set_root_offset_in_dwords(HeapWord* to);
//   void set_from_region_start(HeapWord* start);
//   void set_to_region_start(HeapWord* start);
//   // memset offset table to all 0
//   void clear();
//   char* base();
//   size_t table_byte_size();

//   size_t region_id() {return _region_id;}
//   void set_region_id(size_t region_id) {_region_id = region_id;}

// };

#endif