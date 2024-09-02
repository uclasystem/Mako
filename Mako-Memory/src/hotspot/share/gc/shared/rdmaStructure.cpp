/**
 * for RDMA communication structure.
 *  
 */
// Modified by Haoran2

#include "gc/shared/rdmaStructure.inline.hpp"   // why can't find this header by using shared/rdmaStructure.hpp





//
// Structure - TaskQueueRDMASuper
//




//
// Structure - GenericTaskQueueRDMA
//








template<class E, CHeapAllocType Alloc_type, unsigned int N>
int GenericTaskQueueRDMA<E, Alloc_type, N>::next_random_queue_id() {
  return randomParkAndMiller(&_seed);  // defined in taskqueue.inlinel.hpp
}








//
// OverflowTargetObjQueue
//




RDMAByteFlag::RDMAByteFlag(bool val):
_byte_flag(val)
{

}





// The size of the flexible array _region_cset[] is limited by global macro, utilities/globalDefinitions.hpp :
//	#define MEMORY_SERVER_CSET_OFFSET     (size_t)0x8000000   // +128MB
//	#define MEMORY_SERVER_CSET_SIZE       (size_t)0x1000      // 4KB 
received_memory_server_cset::received_memory_server_cset():
_write_check_bit_shenandoah(1)
{

	reset(); // reset all the fields.

}


/* sent from cpu to mem*/
flags_of_cpu_server_state::flags_of_cpu_server_state():
_is_cpu_server_in_stw(false),
_cpu_server_data_sent(false),
_should_start_tracing(false),
_tracing_all_finished(false),
_should_start_evacuation(false),
_evacuation_all_finished(false),
_should_start_update(false),
_update_all_finished(false)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of field _is_cpu_server_in_stw : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_is_cpu_server_in_stw)	);
	#endif
}

/* sent from mem to cpu*/
flags_of_mem_server_state::flags_of_mem_server_state():
_tracing_finished(false), //useless
_mem_server_wait_on_data_exchange(false), //useless
_is_mem_server_in_compact(false),//useless
evacuation_finished(false), // Modified by Haoran for remote compaction
update_finished(false), // Modified by Haoran for remote compaction
_compacted_region_length(0) //useless
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of first field _mem_server_wait_on_data_exchange : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_mem_server_wait_on_data_exchange)	);
	#endif
}

SATBMetadataMemServer::SATBMetadataMemServer():
_mem_server_completed_buffers_head(0),
_mem_server_completed_buffers_tail(0)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of first field _mem_server_completed_buffers_head : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_mem_server_completed_buffers_head)	);
	#endif
}


SATBMetadataCPUServer::SATBMetadataCPUServer():
_cpu_server_completed_buffers_head(0),
_cpu_server_completed_buffers_tail(0)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of first field _cpu_server_completed_buffers_head : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_cpu_server_completed_buffers_head)	);
	#endif
}