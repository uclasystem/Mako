/**
 *  Only contain the Memory Related structure and functions
 * 
 * Warning : 
 *  1) Only include this file in .ccp file for debuging.
 *  2) The inline function can be invoked in multiple .ccp file.
 * 		 And these .ccp files may be merged into one .o 
 *     So, we have to declare the inline function as static.
 * 		 static functions can only be used in the .cpp included it by making a copy.
 * 		 In this case, every .ccp will have its self copy of inline function without conflict.
 */ 

#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H

#include <linux/swap_global_struct.h>

//
// ###################### MACRO #########################
//


#define SWAP_OUT_MONITOR_VADDR_START		(size_t)(SEMERU_START_ADDR+ RDMA_STRUCTURE_SPACE_SIZE)  // Start of Data Regions, 0x400,100,000,000
//#define SWAP_OUT_MONITOR_VADDR_START		(size_t)SEMERU_START_ADDR // Start of Meta Region, 0x400,000,000,000
#define SWAP_OUT_MONITOR_UNIT_LEN_LOG		HEAP_REGION_BYTE_LOG // e.g., 1<<20, recording granulairy is 1M per entry. The query can span multiple entries.
#define SWAP_OUT_MONITOR_OFFSET_MASK		(u64)(~((1<<SWAP_OUT_MONITOR_UNIT_LEN_LOG) -1))		//0xffff,ffff,fff0,0000
#define SWAP_OUT_MONITOR_ARRAY_LEN		(u64)HEAP_REGION_NUM //2M item, Coverred heap size: SWAP_OUT_MONITOR_ARRAY_LEN * (1<<SWAP_OUT_MONITOR_UNIT_LENG_LOG)






//
// ###################### Functions ######################
//

//
// Control path support
extern int control_path_control_enabled;
extern atomic_t cp_path_prepare_to_flush;
extern atomic_t enter_swap_zone_counter;


static inline void disable_control_path(void){
	control_path_control_enabled = 0;
}

static inline void reset_swap_zone_counter(void){
	atomic_set(&enter_swap_zone_counter, 0);
}

static inline void enter_swap_zone(void){
	atomic_inc(&enter_swap_zone_counter);
}

static inline void enter_swap_zone_with_debug_info(size_t addr, const char* message){
	atomic_inc(&enter_swap_zone_counter);

#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
	pr_warn("%s, fault on 0x%lx increased enter_swap_zone_counter to %d ===>\n",
		message, addr, atomic_read(&enter_swap_zone_counter));
#endif
}

static inline void leave_swap_zone(void){
	atomic_dec(&enter_swap_zone_counter);
}

static inline void leave_swap_zone_with_debug_info(size_t addr, const char* message){
	int ret = atomic_dec_return(&enter_swap_zone_counter);

	if(ret < 0) {
		pr_warn("%s, fault on 0x%lx, decreased enter_swap_zone_counter to %d <=== \n",
				message, addr, ret);
	}

#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)	
	pr_warn("%s, fault on 0x%lx, decreased enter_swap_zone_counter to %d <=== \n",
		message, addr, atomic_read(&enter_swap_zone_counter));
#endif
}



static inline void prepare_control_path_flush(void){

	atomic_t debug_counter;

	// enter flush zone,
	// 1) prevent new swap-out jumping in
	// 2) wait the exits of all the entered swap-outs
	atomic_set(&cp_path_prepare_to_flush, 1);
	atomic_set(&debug_counter, 0);

	// wait all the threads exit the swap zone
	do{
		if(likely(atomic_read(&enter_swap_zone_counter))){
			// some threads are still in swap zone
			// block and wait
//#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
			pr_warn("%s, wait for the existing of %d swap operations.\n", 
				__func__, atomic_read(&enter_swap_zone_counter));
//#endif
			//debug
			atomic_inc(&debug_counter);
			if(atomic_read(&debug_counter) > 999){
				pr_warn("%s, wait too long, give up cp flush.\n", __func__);
				atomic_set(&cp_path_prepare_to_flush, 0);
				break;
			}

			cond_resched();
		}else{

			pr_warn("%s, All threads exited swap zone.\n", 
				__func__);

			break; // safe to return.
		}
	}while(1);
}

static inline void control_path_flush_done(void){
	
	// enter_swap_zone_counter must be 0 here.
	//BUG_ON(atomic_read(&enter_swap_zone_counter));
	if((unlikely(atomic_read(&enter_swap_zone_counter)))){
		pr_err("%s, %d swap ops not flushed before sent signal !!\n",
		__func__, atomic_read(&enter_swap_zone_counter));
	}

	atomic_set(&cp_path_prepare_to_flush, 0);

	pr_warn("%s, <<==.\n", __func__);
}

static inline void reset_control_path_flush_flags(void){
	atomic_set(&cp_path_prepare_to_flush, 0);
}

// spin lock check
// static inline void test_and_enter_swap_zone(void){
// 	unsigned long flags; // store the context before disabling interrup
// 	spin_lock_irqsave(&control_path_flush_lock, flags);
// 	enter_swap_zone(); // no need to use atomic here
// 	spin_unlock_irqrestore(&control_path_flush_lock, flags);
// }


static inline void test_and_enter_swap_zone(void){
	do{
		if(unlikely(atomic_read(&cp_path_prepare_to_flush))){
			// control path is preparing to flush the on-the-fly data
			// reshedule and try again
			cond_resched();
		}else{
			// safe to enter the swap zone
			enter_swap_zone();
			break;
		}

	}while(1);
}


static inline void test_and_enter_swap_zone_with_debug_info(size_t addr, const char* message){
	do{
		if(unlikely(atomic_read(&cp_path_prepare_to_flush))){
			// control path is preparing to flush the on-the-fly data
			// reshedule and try again
			cond_resched();
		}else{
			// safe to enter the swap zone
			enter_swap_zone_with_debug_info(addr, message);
			break;
		}

	}while(1);
}



// not thread-safe, used in swap_on with lock aquired
// Only intilize these fields once for all of  the swap partitions.
static inline int enable_control_path(void){
	if(!control_path_control_enabled){
		control_path_control_enabled = 1;

		reset_swap_zone_counter();
		reset_control_path_flush_flags();

		return 1;
	}else{
		return 0; // no need to re-initialize the counters.
	}
}




//
// profilings 
extern atomic_t on_demand_swapin_number;
extern atomic_t prefetch_swapin_number;
extern atomic_t hit_on_swap_cache_number;


extern atomic_t jvm_region_swap_out_counter[]; // 4 bytes for each counter is good enough.



// Invoked in syscall sys_swap_stat_reset_and_check
static inline void reset_swap_info(void){
	atomic_set(&on_demand_swapin_number,0);
	atomic_set(&prefetch_swapin_number, 0);
	atomic_set(&hit_on_swap_cache_number,0);
}

// Multiple thread safe.
static inline void on_demand_swapin_inc(void){
	atomic_inc(&on_demand_swapin_number);
}

static inline void prefetch_swapin_inc(void){
	atomic_inc(&prefetch_swapin_number);
}


static inline void hit_on_swap_cache_inc(void){
	atomic_inc(&hit_on_swap_cache_number);
}

static inline int get_on_demand_swapin_number(void){
	return (int)atomic_read(&on_demand_swapin_number);
}

static inline int get_prefetch_swapin_number(void){
	return (int)atomic_read(&prefetch_swapin_number);
}

static inline int get_hit_on_swap_cache_number(void){
	return (int)atomic_read(&hit_on_swap_cache_number);
}



//
// Please invoke the syscall sys_swap_stat_reset_and_check to initialize the parameters.
//



//
// The swap out procedure is done by kernel.
// 1) It should be 1-thread.
// 2) If swap out/in one page frequently, will this cause error ?
//	  Each page can only be swapped out once.
//	  If it's swapped in, we already decrease it from the count.
static inline void swap_out_one_page_record(u64 vaddr){
	u64 entry_ind = (vaddr - SWAP_OUT_MONITOR_VADDR_START) >> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	atomic_inc(&jvm_region_swap_out_counter[entry_ind]);

	#ifdef DEBUG_MODE_DETAIL
		printk("%s, swap out page, entry[0x%llx] vaddr 0x%llx \n", __func__, entry_ind, vaddr);
	#endif
}

// Cause we can't monitor the pages via prefeched path accurately. 
// We are recording pte <-> Page map, not the actual swapin.
//
static inline void swap_in_one_page_record(u64 vaddr){
	u64 entry_ind = (vaddr - SWAP_OUT_MONITOR_VADDR_START) >> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	//jvm_region_swap_out_counter[entry_ind]--;
	atomic_dec(&jvm_region_swap_out_counter[entry_ind]);

	#ifdef DEBUG_MODE_DETAIL
		printk("%s, swap in page, entry[0x%llx], vaddr 0x%llx \n", __func__, entry_ind, vaddr);
	#endif
}

/**
 * Count and return the swap out pages number per Region/Unit
 * 1) We assume all the pages are touched and allocated before.
 * 2) end_vaddr - start_vaddr should be Region alignment and 1 Region at least.
 * 		For the corner case, end_vaddr must > start_vaddr, 
 * 		the size can't be 0.
 */
static inline u64 swap_out_pages_for_range(u64 start_vaddr, u64 end_vaddr){
	u64 entry_start = (start_vaddr - SWAP_OUT_MONITOR_VADDR_START)>> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	u64 entry_end	= (end_vaddr -1 - SWAP_OUT_MONITOR_VADDR_START)>> SWAP_OUT_MONITOR_UNIT_LEN_LOG;
	long swap_out_total = 0;
	u32 i;

	#if defined(DEBUG_MODE_BRIEF) || defined(DEBUG_MODE_DETAIL)
	printk("%s, Get the swapped out pages for addr[0x%llx, 0x%llx), entry[0x%llx, 0x%llx] \n", __func__, 
																															(u64)start_vaddr, (u64)end_vaddr,
																															entry_start, entry_end);
	#endif

	for(i=entry_start; i<=entry_end; i++ ){
		swap_out_total += (u64)atomic_read(&jvm_region_swap_out_counter[i]);
	}

	// saint check
	if(swap_out_total < 0){
		pr_err("%s, counter is wrong. swap_out_total is negative %ld. reset it to 0. RESET it to 0.", __func__, swap_out_total);
		swap_out_total = 0;
	}


	return swap_out_total;
}



//
// ###################### Debug Functions ######################
//

int is_page_in_swap_cache(pte_t	pte); 
struct page* page_in_swap_cache(pte_t	pte); 


static inline void print_skipped_page(pte_t pte, unsigned long addr, const char * message){
	// if((addr>=0x400100000000ULL && addr < 0x400108000000) || (addr>=0x400500000000ULL && addr < 0x400508000000))
		pr_warn("%s, skip virt addr 0x%lx, pte val 0x%lx",
			message, addr, pte.pte);
}


// yifan debug
enum page_state {
	PG_INIT = 0,
	PG_MAP,
	PG_RDIN,
	PG_UNMAP,
	PG_WROUT,
};

extern int *PAGE_STATUS;
extern pte_t *PTE_STATUS;

void init_page_status(void);
void set_page_status(unsigned long addr, int state, pte_t pte);
void get_page_status(unsigned long addr);
bool check_range_geq(unsigned long stt, unsigned long end,
		     int state);
bool check_range_eq(unsigned long stt, unsigned long end,
		    int state);
bool check_range_neq(unsigned long stt, unsigned long end, int state);


#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H


