#ifndef MY_KSM_H
#define MY_KSM_H
#include <linux/types.h>
#include <linux/rbtree.h>
#include "ksm_rdma.h"
#include "time_util.h"

#include "mm_slot.h"

typedef u8 rmap_age_t;

/**
 * struct ksm_mm_slot - ksm information per mm that is being scanned
 * @slot: hash lookup from mm to mm_slot
 * @rmap_list: head for this mm_slot's singly-linked list of rmap_items
 */
 struct ksm_mm_slot {
	struct mm_slot slot;
	struct ksm_rmap_item *rmap_list;
};

/**
 * struct ksm_scan - cursor for scanning
 * @mm_slot: the current mm_slot we are scanning
 * @address: the next address inside that to be scanned
 * @rmap_list: link to the next rmap to be scanned in the rmap_list
 * @seqnr: count of completed full scans (needed when removing unstable node)
 *
 * There is only the one ksm_scan instance of this cursor structure.
 */
 struct ksm_scan {
	struct ksm_mm_slot *mm_slot;
	unsigned long address;
	struct ksm_rmap_item **rmap_list;
	unsigned long seqnr;
};

/**
 * struct ksm_stable_node - node of the stable rbtree
 * @node: rb node of this ksm page in the stable tree
 * @head: (overlaying parent) &migrate_nodes indicates temporarily on that list
 * @hlist_dup: linked into the stable_node->hlist with a stable_node chain
 * @list: linked into migrate_nodes, pending placement in the proper node tree
 * @hlist: hlist head of rmap_items using this ksm page
 * @kpfn: page frame number of this ksm page (perhaps temporarily on wrong nid)
 * @chain_prune_time: time of the last full garbage collection
 * @rmap_hlist_len: number of rmap_item entries in hlist or STABLE_NODE_CHAIN
 * @nid: NUMA node id of stable tree in which linked (may not match kpfn)
 */
 struct ksm_stable_node {
	union {
		struct rb_node node;	/* when node of stable tree */
		struct {		/* when listed for migration */
			struct list_head *head;
			struct {
				struct hlist_node hlist_dup;
				struct list_head list;
			};
		};
	};
	struct hlist_head hlist;
	union {
		unsigned long kpfn;
		unsigned long chain_prune_time;
	};
	/*
	 * STABLE_NODE_CHAIN can be any negative number in
	 * rmap_hlist_len negative range, but better not -1 to be able
	 * to reliably detect underflows.
	 */
#define STABLE_NODE_CHAIN -1024
	int rmap_hlist_len;
#ifdef CONFIG_NUMA
	int nid;
#endif
};

/**
 * struct ksm_rmap_item - reverse mapping item for virtual addresses
 * @rmap_list: next rmap_item in mm_slot's singly-linked rmap_list
 * @anon_vma: pointer to anon_vma for this mm,address, when in stable tree
 * @nid: NUMA node id of unstable tree in which linked (may not match page)
 * @mm: the memory structure this rmap_item is pointing into
 * @address: the virtual address this rmap_item tracks (+ flags in low bits)
 * @oldchecksum: previous checksum of the page at that virtual address
 * @node: rb node of this rmap_item in the unstable tree
 * @head: pointer to stable_node heading this list in the stable tree
 * @hlist: link into hlist of rmap_items hanging off that stable_node
 * @age: number of scan iterations since creation
 * @remaining_skips: how many scans to skip
 */
 struct ksm_rmap_item {
	struct ksm_rmap_item *rmap_list;
	union {
		struct anon_vma *anon_vma;	/* when stable */
#ifdef CONFIG_NUMA
		int nid;		/* when node of unstable tree */
#endif
	};
	struct mm_struct *mm;
	unsigned long address;		/* + low bits used for flags below */
	union {
		struct page* page;
		struct {
			unsigned int oldchecksum;	/* when unstable */
			rmap_age_t age;
			rmap_age_t remaining_skips;
		};
	};
	union {
		struct rb_node node;	/* when node of unstable tree */
		struct {		/* when listed from stable tree */
			struct ksm_stable_node *head;
			struct hlist_node hlist;
		};
	};
};

extern void debug_stop(void);

enum offload_mode {
	NO_OFFLOAD = 0,
    SINGLE_OPERATION_OFFLOAD = 1,
	KSM_OFFLOAD = 2,
};

enum merge_fail_reason {
	No_mergeable_vma_found = 0,
	Failed_to_lock_page = 1,
	Pages_are_not_identical = 2,
	Page_address_in_vma_failed = 3,
	page_vma_mapped_walk_failed = 4,
	Pvmw_pte_is_null = 5,
	Page_mapcount_unequal = 6,
	Page_is_shared = 7,
	Not_an_anonymous_page = 8,
	Failed_to_split_page = 9,
	But_same_hash = 10,
};

enum remote_status {
	UNINITIALIZED = 0,
	INITIALIZED = 1,
	DISCONNECTED = 2,
};

extern struct ksm_cb* ksm_cb;
extern struct error_table* ksm_error_table;
extern bool is_offload_decided;
extern enum offload_mode *current_mode;
extern enum remote_status offload_server_status;
extern char* fail_reason_str[11];
extern long fail_reason_cnts[11];

bool is_rdma_initialized(void);
bool is_ksm_offload(void);
bool is_styx_offload(void);

#define OFFLOAD_MODE (*current_mode)
#define DEBUG_PRINT_FLAG 0

#define DEBUG_LOG(fmt, args...) do { if (DEBUG_PRINT_FLAG) pr_info(fmt, ##args); } while (0)
#define DEBUG_ERR(fmt, args...) do { pr_err(fmt, ##args); debug_stop(); } while (0)

#endif
