#include "logging/log.hpp"
#include "gc/shenandoah/makoOffsetTable.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
// mutex
#include "runtime/mutexLocker.hpp"

#include "runtime/os.hpp"
#include "services/memTracker.hpp"
#include "utilities/bitMap.inline.hpp"
// assert
#include "utilities/debug.hpp"

// not sure whether include grammar is right
#include "os_linux.hpp"

// TODO: ensure that it is fine to let offset table lock-free

// OffsetTable::OffsetTable(size_t region_id, size_t region_byte_size) {
//   _region_id = region_id;
//   assert(region_byte_size <= ONE_MB, "Region size too big for 2-byte offset.");
//   _region_byte_size = region_byte_size;
//   _table_byte_size = region_byte_size / heap_alignment_byte_size * offset_byte_size;
//   _base = OFFSET_TABLE_START_ADDR + region_id * _table_byte_size;
//   _table = MemRegion((HeapWord*)_base, _table_byte_size / HeapWordSize);
//   size_t alignment = 0x1000;
//   // os::commit_memory_or_exit((char*)_base, _table_byte_size, alignment, false, "failed to commit offset table");
// // #ifdef ASSERT
//   // memset((void*)_base, 0, _table_byte_size);
// // #endif
//   set_from_region_start(NULL);
//   set_to_region_start(NULL);
//   set_root_offset_in_dwords((size_t)0);
// }

// size_t OffsetTable::addr_to_offset(HeapWord* addr, size_t region_start) {
//   shenandoah_assert_in_heap(NULL, (oop)addr);
//   size_t offset_in_bytes = (size_t)addr - region_start;
//   assert(offset_in_bytes < _region_byte_size, "Address out of range, or region start set wrong");
//   assert(offset_in_bytes % heap_alignment_byte_size == 0, "Address not 2-word aligned");
//   return offset_in_bytes >> log_heap_alignment_byte_size;
// }

// HeapWord* OffsetTable::offset_to_addr(size_t offset, size_t region_start) {
//   size_t offset_in_bytes = offset << log_heap_alignment_byte_size;
//   assert(offset_in_bytes < _region_byte_size, "Address out of range, or region start set wrong");
//   assert(offset_in_bytes % heap_alignment_byte_size == 0, "Address not 2-word aligned");
//   HeapWord* addr = (HeapWord*)(region_start + offset_in_bytes);
//   shenandoah_assert_in_heap(NULL, (oop)addr);
//   return addr;
// }

// void OffsetTable::set(HeapWord* from, HeapWord* to) {
//   size_t to_offset = addr_to_offset(to, _to_region_start);
//   set_using_offset(from, to_offset);
// }

// void OffsetTable::set_using_byte_offset(HeapWord* from, size_t to_byte_offset) {
//   assert((to_byte_offset>> log_heap_alignment_byte_size) <= UINT16_MAX, "offset out of range");
//   size_t from_offset = addr_to_offset(from, _from_region_start);
//   *((uint16_t*)(_base) + from_offset) = (to_byte_offset >> log_heap_alignment_byte_size);
// }

// void OffsetTable::set_using_offset(HeapWord* from, size_t to_offset) {
//   assert(to_offset <= UINT16_MAX, "offset out of range");
//   size_t from_offset = addr_to_offset(from, _from_region_start);
//   *((uint16_t*)(_base) + from_offset) = to_offset;
// }

// void OffsetTable::subtract_root_offset(HeapWord* from_addr) {
//   size_t from_offset = addr_to_offset(from_addr, _from_region_start);
//   assert(*((uint16_t*)(_base) + from_offset) < (uint16_t)(_root_offset_in_dwords), "offset of root object start should be smaller than root offset");
//   *((uint16_t*)(_base) + from_offset) -= (uint16_t)(_root_offset_in_dwords);
// }

// HeapWord* OffsetTable::get(HeapWord* from) {
//   size_t from_offset = addr_to_offset(from, _from_region_start);
//   size_t to_offset = *((uint16_t*)(_base) + from_offset);
//   return offset_to_addr(to_offset, _to_region_start);
// }

// HeapWord* OffsetTable::get_with_root_offset(HeapWord* from) {
//   size_t from_offset = addr_to_offset(from, _from_region_start);
//   size_t to_offset = (size_t)(*((uint16_t*)(_base) + from_offset) + (uint16_t)(_root_offset_in_dwords));
//   return offset_to_addr(to_offset, _to_region_start);
// }

// size_t OffsetTable::root_offset_in_dwords() {
//   return _root_offset_in_dwords;
// }

// void OffsetTable::set_root_offset_in_dwords(size_t to_offset) {
//   assert(to_offset <= UINT16_MAX, "offset out of range");
//   _root_offset_in_dwords = to_offset;
// }

// void OffsetTable::set_root_offset_in_dwords(HeapWord* to) {
//   size_t to_offset = addr_to_offset(to, _to_region_start);
//   set_root_offset_in_dwords(to_offset);
// }

// void OffsetTable::set_from_region_start(HeapWord* start) {
//   _from_region_start = (size_t)start;
// }

// void OffsetTable::set_to_region_start(HeapWord* start) {
//   _to_region_start = (size_t)start;
// }

// void OffsetTable::clear() {
//   memset((void*)_base, 0, _table_byte_size);
// }

// char* OffsetTable::base() {
//   return (char*)_base;
// }

// size_t OffsetTable::table_byte_size() {
//   return _table_byte_size;
// }
