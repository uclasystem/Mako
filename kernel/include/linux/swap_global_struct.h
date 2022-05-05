/**
 * Include some global structures used for Semeru swapping
 * 
 * Warning : 
 *  1) Only include this file in .ccp file for debuging.
 *  2) The inline function can be invoked in multiple .ccp file.
 * 		 And these .ccp files may be merged into one .o 
 *     So, we have to declare the inline function as static.
 * 		 static functions can only be used in the .cpp included it by making a copy.
 * 		 In this case, every .ccp will have its self copy of inline function without conflict.
 */ 

#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/mm_inline.h>
#include <linux/list.h>
#include <asm-generic/bug.h>


//
//  ###################### Module control option  ######################
//

// Some functions are under developtment, disable them when releasing the code.
// #define UNDER_DEVELOPEMENT 1

// #1 enable the swp_entry_t to virtual address remap or not
// The memory range not in the RANGE will be not swapped out by adding them into unevictable list.
#define ENABLE_SWP_ENTRY_VIRT_REMAPPING 1

// #2 This sync is uselesss. Because all the unmapped dirty page will be writteen to swap partition immediately.
//#define SYNC_PAGE_OUT

// #3 Using frontswap path. The frontswap path bypassed the block layer.
//    frontswap path only supports one memory server now. We will add the supports of multiple memory servers later.
#define SEMERU_FRONTSWAP_PATH 1

#define SMALL
// #define LARGE


//
// ##################### Parameters configuration  ###################### 
//

// Structures of the Regions
// | -- Meta Region -- | -- Data Regsons --|
//  The meta Regions starts from SEMERU_START_ADDR. Its size is defined by RDMA_STRUCTURE_SPACE_SIZE.
#define REGION_SIZE_GB 4UL // RDMA manage granularity, not the Heap Region.
// #define RDMA_META_REGION_NUM 1UL
#define RDMA_META_REGION_NUM 2UL

// JVM configurations
#define HEAP_REGION_BYTE_LOG	20UL // Update the value to log(heap_region_size) 
#define HEAP_REGION_NUM		64*ONE_KB // The number of heap region


#ifdef LARGE
#define RDMA_DATA_REGION_NUM 16UL
#endif
#ifdef SMALL
#define RDMA_DATA_REGION_NUM 8UL
#endif


#define SEMERU_START_ADDR 0x400000000000UL // 0x400000000000UL
#define NUM_OF_MEMORY_SERVER 1UL

// ###
// below is derived macros

//
// Meta space
#define RDMA_META_SPACE_START_ADDR (SEMERU_START_ADDR)
#define RDMA_STRUCTURE_SPACE_SIZE (RDMA_META_REGION_NUM * REGION_SIZE_GB * ONE_GB)

//
// Data space
#define RDMA_DATA_SPACE_START_ADDR (RDMA_META_SPACE_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE)
#define DATA_REGION_PER_MEM_SERVER (RDMA_DATA_REGION_NUM / NUM_OF_MEMORY_SERVER)

// Memory server #1, Data Region[1] to Region[5]
// Only being used for correctness checks,
// Plase calculated this derived information.
#define MEMORY_SERVER_0_REGION_START_ID (RDMA_META_REGION_NUM)
#define MEMORY_SERVER_0_START_ADDR (RDMA_DATA_SPACE_START_ADDR)

// Memory server #2, Data Region[5] to Region[9]
#define MEMORY_SERVER_1_REGION_START_ID (MEMORY_SERVER_0_REGION_START_ID + DATA_REGION_PER_MEM_SERVER)
//#define MEMORY_SERVER_1_REGION_START_ID 9 //debug, single server
#define MEMORY_SERVER_1_START_ADDR (MEMORY_SERVER_0_START_ADDR + DATA_REGION_PER_MEM_SERVER * REGION_SIZE_GB * ONE_GB)


//
// ###################### Debug options ######################
//



//
// Enable debug information printing 
//

//#define DEBUG_SERVER_HOME  // Do kernel bug @ local server


//#define DEBUG_SWAP_PATH 1
//#define DEBUG_SWAP_PATH_DETAIL 1

//#define DEBUG_FLUSH_LIST 1
//#define DEBUG_FLUSH_LIST_DETAIL 1

//#define DEBUG_BIO_MERGE 1
//#define DEBUG_BIO_MERGE_DETAIL 1

// #define DEBUG_REQUEST_TAG 1

//#define DEBUG_LATENCY_CLIENT 1
//#define DEBUG_MODE_BRIEF 1 
//#define DEBUG_MODE_DETAIL 1
//#define DEBUG_BD_ONLY 1			// Build and install BD & RDMA modules, but not connect them.
//#define DEBUG_RDMA_ONLY 1			// Only build and install RDMA modules.
//#define DEBUG_FRONTSWAP_ONLY 1    // Use the local DRAM, not connect to RDMA

//#define ASSERT 1		// general debug

//
// Basic Macro
//

#ifndef ONE_KB
#define ONE_KB ((size_t)1024) // 1024 x 2014 bytes
#endif

#ifndef ONE_MB
#define ONE_MB ((size_t)1048576) // 1024 x 2014 bytes
#endif

#ifndef ONE_GB
#define ONE_GB ((size_t)1073741824) // 1024 x 1024 x 1024 bytes
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE ((size_t)4096) // bytes, use the define of kernel.
#endif

//
// RDMA Related
//


// 1) Limit the outstanding rdma wr.
//    1-sided RDMA only need to issue a ib_rdma_wr to read/write the remote data.
//    2-sided RDMA need to post a recv wr to receive the data sent by remote side.
// 2) Both the send/recv queue will cost swiotlb buffer, so we can't make them too large.
//    For Semeru, most of our data are transfered by 1-sided RDMA.
//    So we don't need a large queue for the recv queue.
// 3) [X] This is the queue depth for the only qp. Nothing to do with the core number.
//
#define RDMA_SEND_QUEUE_DEPTH		4096		// for the qp. Find the max number without warning.
#define RDMA_RECV_QUEUE_DEPTH		32

extern uint64_t RMEM_SIZE_IN_PHY_SECT;			// [?] Where is it defined ? 

#define REGION_BIT					ilog2(REGION_SIZE_GB) + ilog2(ONE_GB)
#define REGION_MASK					(size_t)(((size_t)1 << REGION_BIT) -1)


// Each request can have multiple bio, but each bio can only have 1  pages ??
#define MAX_REQUEST_SGL								 32 		// number of segments, get from ibv_query_device. Use 30, or it's not safe..
//#define MAX_SEGMENT_IN_REQUEST			 32 // use the MAX_REQUEST_SGL
//#define ONE_SIEDED_RDMA_BUF_SIZE			(u64)MAX_REQUEST_SGL * PAGE_SIZE



// Synchronization mask
#define	DIRTY_TAG_MASK 	 (uint32_t)( ((1<<16) - 1) << 16)		// high 16 bits of the uint32_t
#define VERSION_MASK	 	 (uint32_t)( (1<<16) - 1 )						// low 16 bits of the uint32_t

// The high 16 bits can only be 1 or 0.
#define DIRTY_TAG_SEND_START	 (uint32_t)(1<<16)				// 1,0000,0000,0000,0000, OR this value to set the high 16 bits as 1.
#define DIRTY_TAG_SEND_END		 (uint32_t)( (1<<16) - 1)	// 0,1111,1111,1111,1111, AND this vlaue to set high 16 bits as 0.






//
//################################## Address information ##################################


#define RDMA_ALIGNMENT_BYTES    64  // cache line.

// RDMA structure space
// [  Small meta data  ]  [ aliv_bitmap per region ]   [ dest_bitmap per region ] [ reserved for now]
//#define RDMA_STRUCTURE_SPACE_SIZE  ((size_t) ONE_GB *4) // moved to semeru_cpu.h



//
// ##################################  RDMA Meta Region ################################## 
//
// | -- No Swap Part -- | -- Swap Part --|
// 
// There are 2 parts in the RDMA Meta Region, No-Swap-Part and Swap-Part.
// The data of No-Swap-Part can only be sent via the Control Path.
// Cause the object instances stored in the No-Swap-Part :
// 1) May contain virtual functions. 
//    The virtual address of virtual function are usually different in the CPU server and memory servers.
// 2) The data on memory serves maybe newer than CPU server.
//    CPU server needs to read the data in a safe time windwon.

//
// Offset for each Part
//

// ## No-Swap-Part ##
//

// #define ALIVE_BITMAP_OFFSET      (size_t)0
// // #define ALIVE_BITMAP_SIZE        (size_t)(512 * (size_t)ONE_MB)
// #define ALIVE_BITMAP_SIZE        (size_t)(1024 * (size_t)ONE_MB)

// // 1. Klass instance space.
// //    Used for store klass instance, class loader related information.
// //    range [1GB, 1GB+256MB). The usage is based on application.
// //    [?] Pre commit tall the space ?
// // #define KLASS_INSTANCE_OFFSET               (size_t)0x0    // +0MB, 0x400,000,000,000
// #define KLASS_INSTANCE_OFFSET               (size_t)(ALIVE_BITMAP_OFFSET + ALIVE_BITMAP_SIZE) //       0x400,020,000,000
// #define KLASS_INSTANCE_OFFSET_SIZE_LIMIT    (size_t)(256*ONE_MB)                                 



// #define MEM_REF_Q_OFFSET                      (size_t)(KLASS_INSTANCE_OFFSET + KLASS_INSTANCE_OFFSET_SIZE_LIMIT)// (SATB_BUFFER_OFFSET + SATB_BUFFER_SIZE_LIMIT + 512 * (size_t)ONE_MB) //       0x400,030,000,000
// #define MEM_REF_Q_LEN                         (size_t)(8387584) // (size_t)(67107840)       //(256M - 4K) * 1024 / 4
// #define MEM_REF_Q_SIZE_LIMIT                  (size_t)(64 * (size_t)ONE_MB) // (size_t)(512 * (size_t)ONE_MB)
// #define MEM_REF_Q_PER_SIZE                    (size_t)(32 * (size_t)ONE_MB) // (size_t)(256 * (size_t)ONE_MB)

// // Semeru Specific meta data start
// // 3. Small meta data 
// //

// // 3.1 Meta of HeapRegion.
// // These information need to be synchronized between CPU server and memory server.
// // Reserve 4K per region is enough.

// // 3.1 SemeruHeapRegion Manager
// //     The structure of SemeruHeapRegion. 4K for each Region is enough.
// //     [x] precommit all the space.
// #define HEAP_REGION_MANAGER_OFFSET           (size_t)(MEM_REF_Q_OFFSET+MEM_REF_Q_SIZE_LIMIT) // (KLASS_INSTANCE_OFFSET + KLASS_INSTANCE_OFFSET_SIZE_LIMIT)  // +832MB,  0x400,034,000,000
// #define HEAP_REGION_MANAGER_SIZE_LIMIT       (size_t)(8*ONE_MB) // each SemeruHeapRegion should less than 4K, this is enough for 1024 HeapRegion.


// // 3.1.1 CPU Server To Memory server, Initialization
// // [x] precommit
// #define CPU_TO_MEMORY_INIT_OFFSET     (size_t)(HEAP_REGION_MANAGER_OFFSET + HEAP_REGION_MANAGER_SIZE_LIMIT) // +836MB, 0x400,034,400,000
// #define CPU_TO_MEMORY_INIT_SIZE_LIMIT (size_t) 8*ONE_MB    //

// // 3.1.2 CPU Server To Memory server, GC
// // [x] precommit
// #define CPU_TO_MEMORY_GC_OFFSET       (size_t)(CPU_TO_MEMORY_INIT_OFFSET + CPU_TO_MEMORY_INIT_SIZE_LIMIT) // +840MB, 0x400,034,800,000
// #define CPU_TO_MEMORY_GC_SIZE_LIMIT   (size_t) 8*ONE_MB    //


// // 3.1.3 Memory server To CPU server 
// // [x] precommit
// #define MEMORY_TO_CPU_GC_OFFSET       (size_t)(CPU_TO_MEMORY_GC_OFFSET + CPU_TO_MEMORY_GC_SIZE_LIMIT) // +844MB, 0x400,034,C00,000
// #define MEMORY_TO_CPU_GC_SIZE_LIMIT   (size_t) 8*ONE_MB    //


// // 3.1.4 Synchonize between CPU server and memory server
// // [x] precommit
// #define SYNC_MEMORY_AND_CPU_OFFSET       (size_t)(MEMORY_TO_CPU_GC_OFFSET + MEMORY_TO_CPU_GC_SIZE_LIMIT) // +848MB, 0x400,035,000,000
// #define SYNC_MEMORY_AND_CPU_SIZE_LIMIT   (size_t) 8*ONE_MB    //


// // 5. JVM global flags.
// //

// // 5.1 Memory server CSet
// // [x] precommit
// #define MEMORY_SERVER_CSET_OFFSET     (size_t)(SYNC_MEMORY_AND_CPU_OFFSET + SYNC_MEMORY_AND_CPU_SIZE_LIMIT)    // +852MB, 0x400,035,400,000
// #define MEMORY_SERVER_CSET_SIZE       (size_t)PAGE_SIZE   // 4KB 

// // 5.2 cpu server state, STW or Mutator 
// // Used as CPU <--> Memory server state exchange
// // [x] precommit
// #define FLAGS_OF_CPU_SERVER_STATE_OFFSET    (size_t)(MEMORY_SERVER_CSET_OFFSET + MEMORY_SERVER_CSET_SIZE)  // +852MB +4KB,  0x400,035,401,000
// #define FLAGS_OF_CPU_SERVER_STATE_SIZE      (size_t)PAGE_SIZE      // 4KB 


// // 5.3 memory server flags 
// // [x] precommit
// #define FLAGS_OF_MEM_SERVER_STATE_OFFSET    (size_t)(FLAGS_OF_CPU_SERVER_STATE_OFFSET + FLAGS_OF_CPU_SERVER_STATE_SIZE)  // +852MB +8KB, 0x400,035,402,000
// #define FLAGS_OF_MEM_SERVER_STATE_SIZE      (size_t)PAGE_SIZE      // 4KB 

// // 5.4 one-sided RDMA write check flags
// // 4 bytes per HeapRegion |-- 16 bits for dirty --|-- 16 bits for version --|
// // Assume the number of Region is 1024, 
// // Reserve 4KB for the write check flags.
// // [x] precommit
// #define FLAGS_OF_CPU_WRITE_CHECK_OFFSET       (size_t)(FLAGS_OF_MEM_SERVER_STATE_OFFSET + FLAGS_OF_MEM_SERVER_STATE_SIZE)  // +852MB +12KB, 0x400,035,403,000
// #define FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT   (size_t)PAGE_SIZE     // 4KB 

// #define SATB_METADATA_MEM_SERVER_OFFSET           (size_t)(FLAGS_OF_CPU_WRITE_CHECK_OFFSET+FLAGS_OF_CPU_WRITE_CHECK_SIZE_LIMIT)  // +852MB +16KB, 0x400,035,404,000
// #define SATB_METADATA_MEM_SERVER_SIZE             (size_t)PAGE_SIZE     // 4KB 
// #define SATB_METADATA_CPU_SERVER_OFFSET           (size_t)(SATB_METADATA_MEM_SERVER_OFFSET+ SATB_METADATA_MEM_SERVER_SIZE)  // +852MB +20KB, 0x400,035,405,000
// #define SATB_METADATA_CPU_SERVER_SIZE             (size_t)PAGE_SIZE     // 4KB 


// #define INDIRECTION_METADATA_OFFSET           (size_t)(SATB_METADATA_CPU_SERVER_OFFSET+ SATB_METADATA_CPU_SERVER_SIZE)  // +852MB +24KB, 0x400,035,406,000
// #define INDIRECTION_METADATA_SIZE             (size_t)PAGE_SIZE     // 4KB 


// #define RDMA_FLAG_OFFSET                          (size_t)(INDIRECTION_METADATA_OFFSET + INDIRECTION_METADATA_SIZE) // +852MB +28KB, 0x400,035,407,000
// #define RDMA_FLAG_SIZE                            (size_t)(249 * PAGE_SIZE)   // 249 * 4 = 996KB
// #define RDMA_FLAG_PER_SIZE                        (size_t)PAGE_SIZE     // 4KB 

// // The space upper should be all committed contiguously.
// // and then can register them as RDMA buffer.



// // 5.5 cset_map 1MB for byte map:
// #define CSET_SYNC_MAP_OFFSET                   (size_t)(RDMA_FLAG_OFFSET + RDMA_FLAG_SIZE)    // +853MB, 0x400,035,500,000
// #define CSET_SYNC_MAP_SIZE                     (size_t)(1 * (size_t)ONE_MB)


// #define COMPACTED_MAP_OFFSET                   (size_t)(CSET_SYNC_MAP_OFFSET + CSET_SYNC_MAP_SIZE)    // +854MB, 0x400,035,600,000
// #define COMPACTED_MAP_SIZE                     (size_t)(1 * (size_t)ONE_MB)


// #define SYNC_PAGE_OFFSET                      (size_t)(COMPACTED_MAP_OFFSET + COMPACTED_MAP_SIZE)     // +855MB, 0x400,035,700,000
// #define SYNC_PAGE_SIZE_LIMIT                  (size_t)(8 * (size_t)ONE_MB)


// // 6. Cross-Region reference update queue For use of root object
// // Record the <old_addr, new_addr > for the target object queue.
// // [?] Not sure how much space is needed for the cross-region-reference queue, give all the rest space to it. Need to shrink it latter.
// //
// #define CROSS_REGION_REF_UPDATE_Q_OFFSET      (size_t)(SYNC_PAGE_OFFSET + SYNC_PAGE_SIZE_LIMIT)    // +863MB, 0x400,035,f00,000
// #define CROSS_REGION_REF_UPDATE_Q_SIZE_LIMIT  (size_t)(1 * (size_t)ONE_MB)
// #define CROSS_REGION_REF_UPDATE_PADDING       (size_t)(161 * (size_t)ONE_MB)
// #define CROSS_REGION_REF_UPDATE_Q_LEN         (size_t)(100000) // 1M * 1024/8



// #define SATB_BUFFER_OFFSET                    (size_t)(CROSS_REGION_REF_UPDATE_Q_OFFSET + CROSS_REGION_REF_UPDATE_PADDING) //1024M, 0x400,040,000,000
// #define SATB_BUFFER_SIZE_LIMIT                (size_t)(/*256*/1024 * (size_t)ONE_MB)


// // +512MB for bitmap

// #define INDIRECTION_OFFSET                    (size_t)(SATB_BUFFER_OFFSET + SATB_BUFFER_SIZE_LIMIT) //2048M, 0x400,080,000,000
// #define INDIRECTION_TABLE_SIZE_LIMIT                (size_t)(0 * (size_t)ONE_MB)



// // ----------------------------------Indirection Table Related MetaData----------------------------------

// // #define MAX_NUM_REGIONS 2048

// // #define HEAP_SIZE (size_t)(32 * ONE_GB)
// // #define HEAP_REGION_SIZE (size_t)(64 * ONE_MB) // 64MB, 0x4000000
// // #define LOG_HEAP_REGION_SIZE (int)(log2_long(HEAP_REGION_SIZE)) // 0x4000000 = 1 << 26

// #define INDIRECTION_TABLE_START_ADDR (size_t)(SEMERU_START_ADDR + INDIRECTION_OFFSET)
// // #define INDIRECTION_TABLE_INIT_SIZE (size_t)(16 * ONE_MB) // 16MB, 0x1000000
// // #define INDIRECTION_TABLE_INCREMENT (size_t)(16 * ONE_MB) // 16MB, 0x1000000
// // #define INDIRECTION_TABLE_SIZE (size_t)(16 * ONE_MB) // 64MB, 0x4000000
// // //***** end of which is subject to change with different size options *****

// // #define LOG_INDIRECTION_TABLE_SIZE (int)(log2_long(INDIRECTION_TABLE_SIZE)) // 0x4000000 = 1 << 26
// // // only half of the heap is used at most, only need half the number of tables
// // #define INDIRECTION_TABLE_NUM (int)(HEAP_SIZE / HEAP_REGION_SIZE / 2)
// // #define INDIRECTION_TABLE_SIZE_LIMIT  (size_t)(INDIRECTION_TABLE_NUM * INDIRECTION_TABLE_SIZE) // 4GB, 0x100000000


// #define INDIRECTION_TABLE_END_ADDR (size_t)(INDIRECTION_TABLE_START_ADDR + INDIRECTION_TABLE_SIZE_LIMIT)

// // ------------------------------------------------------------------------------------------------------



// // #define MEM_REF_Q_OFFSET                      (size_t)(SATB_BUFFER_OFFSET + SATB_BUFFER_SIZE_LIMIT + 512 * (size_t)ONE_MB)
// // #define MEM_REF_Q_LEN                         (size_t)(67107840)       //(256M - 4K) * 1024 / 4
// // #define MEM_REF_Q_SIZE_LIMIT                  (size_t)(512 * (size_t)ONE_MB)
// // #define MEM_REF_Q_PER_SIZE                    (size_t)(256 * (size_t)ONE_MB)



// // Padding:

// #define RDMA_RESERVE_PADDING_OFFSET (size_t)(INDIRECTION_OFFSET+INDIRECTION_TABLE_SIZE_LIMIT)
// #define RDMA_RESERVE_PADDING_SIZE  (size_t)(RDMA_STRUCTURE_SPACE_SIZE - RDMA_RESERVE_PADDING_OFFSET)


// // Semeru specific meta data end

// // Semeru G1 specific meta data complement:
// // #define ALIVE_BITMAP_OFFSET      (size_t)0x3f3f3f3f
// // #define ALIVE_BITMAP_SIZE        (size_t)0x3f3f3f3f

// #define CROSS_REGION_REF_TARGET_Q_OFFSET        (size_t)0x3f3f3f3f
// #define CROSS_REGION_REF_TARGET_Q_LEN           (size_t)0x3f3f3f3f
// #define CROSS_REGION_REF_TARGET_Q_SIZE_LIMIT    (size_t)0x3f3f3f3f
// #define BOT_GLOBAL_STRUCT_OFFSET            (size_t)0x3f3f3f3f
// #define BOT_GLOBAL_STRUCT_SIZE_LIMIT        (size_t)0x3f3f3f3f

// #define BLOCK_OFFSET_TABLE_OFFSET             (size_t)0x3f3f3f3f
// #define BLOCK_OFFSET_TABLE_OFFSET_SIZE_LIMIT  (size_t)0x3f3f3f3f







// Pauseless specific meta data start








// struct AddrPair{
//   char* st;
//   char* ed;
// };


// //
// // x. End of RDMA structure commit size
// //
// #define END_OF_RDMA_COMMIT_ADDR   (size_t)(SEMERU_START_ADDR + RDMA_STRUCTURE_SPACE_SIZE)


// properties for the whole Semeru heap.
// [ RDMA meta data sapce] [RDMA data space]

#define MAX_FREE_MEM_GB   ((size_t) REGION_SIZE_GB * RDMA_DATA_REGION_NUM + RDMA_STRUCTURE_SPACE_SIZE/ONE_GB)    //for local memory management
#define MAX_REGION_NUM    ((size_t) MAX_FREE_MEM_GB/REGION_SIZE_GB)     //for msg passing, ?
#define MAX_SWAP_MEM_GB   (u64)(REGION_SIZE_GB * RDMA_DATA_REGION_NUM)		// Space managed by SWAP








/**
 * Bit operations 
 * 
 */
#define GB_SHIFT 				30 
#define CHUNK_SHIFT			(u64)(GB_SHIFT + ilog2(REGION_SIZE_GB))	 // Used to calculate the chunk index in Client (File chunk). Initialize it before using.
#define	CHUNK_MASK			(u64)( ((u64)1 << CHUNK_SHIFT)  -1)		// get the address within a chunk

#define RMEM_LOGICAL_SECT_SHIFT		(u64)(ilog2(RMEM_LOGICAL_SECT_SIZE))  // the power to 2, shift bits.

//
// File address to Remote virtual memory address translation
//









/**
 * ################### utility functions ####################
 */

//
// Calculate the number's power of 2.
// [?] can we use the MACRO ilog2() ?
// uint64_t power_of_2(uint64_t  num){
    
//     uint64_t  power = 0;
//     while( (num = (num >> 1)) !=0 ){
//         power++;
//     }

//     return power;
// }










// from kernel 
/*  host to network long long
 *  endian dependent
 *  http://www.bruceblinn.com/linuxinfo/ByteOrder.html
 */
#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
		    (unsigned int)ntohl(((int)(x >> 32))))
#define htonll(x) ntohll(x)

#define htonll2(x) cpu_to_be64((x))
#define ntohll2(x) cpu_to_be64((x))











//
// ###################### Debug functions ######################
//
bool within_range(u64 val);





#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_H


