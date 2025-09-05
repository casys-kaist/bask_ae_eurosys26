#include "linux/scatterlist.h"
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include "linux/xarray.h"

#define LOOKUP_KSM_RDMA_(func) \
    do { \
        rdma_##func = (void *) kallsyms_lookup_name("ksm_rdma_" #func); \
        if (!rdma_##func) { \
            DEBUG_LOG("Failed to find ksm_rdma_" #func "\n"); \
            ret = false; \
        } \
    } while (0)

#define LOOKUP_MLX_(func) \
    do { \
        do_mlx_##func = (void *) kallsyms_lookup_name("mlx_" #func); \
        if (!do_mlx_##func) { \
            DEBUG_LOG("Failed to find mlx_" #func "\n"); \
            ret = false; \
        } \
    } while (0)

#define MAX_MM_DESCS 32

#define MAX_PAGES_DESCS 512
#define MAX_PAGES_IN_SGL 65536 // => Covers 65536 * 8 = 512KB pages = 2GB

#define MAX_CAPACITY_PER_TABLE ((KMALLOC_MAX_SIZE / sizeof(struct shadow_pte)) / 2)
#define MAX_VA_ARRAYS (MAX_PAGES_IN_SGL * PAGE_SIZE / KMALLOC_MAX_SIZE)
#define MAX_RESULT_TABLE_ENTRIES ((KMALLOC_MAX_SIZE / sizeof(struct ksm_event_log)))

enum ksm_wr_tag {
    WR_SEND_METADATA = 1,
    WR_RECV_METADATA,
    WR_SEND_RESULT,
    WR_RECV_RESULT,
    WR_REG_MR,
    WR_READ_MAP,
    WR_READ_PAGE,
	WR_READ_RESULT,
	WR_SEND_SINGLE_OP,
	WR_RECV_SINGLE_OP,
	WR_SEND_SINGLE_RESULT,
	WR_RECV_SINGLE_RESULT,
	WR_INVALIDATE_MR,
};

enum ksm_rdma_state {
	KSM_IDLE = 1,
	KSM_CONNECT_REQUEST,
	KSM_ADDR_RESOLVED,
	KSM_ROUTE_RESOLVED,
	KSM_CONNECTED,
    KSM_MEM_REG_WAIT,
	KSM_MEM_REG_COMPLETE,
	KSM_RDMA_READ_WAIT,
	KSM_RDMA_READ_COMPLETE,
	KSM_RDMA_WRITE_COMPLETE,
	KSM_RDMA_SEND_COMPLETE,
	KSM_RDMA_RECV_COMPLETE,
	KSM_MR_INVALIDATE_WAIT,
	KSM_MR_INVALIDATE_COMPLETE,
	KSM_ERROR
};

enum event_tag {
	DPU_STABLE_MERGE = 1,
	DPU_UNSTABLE_MERGE,
	DPU_STALE_STABLE_NODE,
	DPU_ITEM_STATE_CHANGE,
	HOST_STALE_STABLE_NODE,
	HOST_NO_STABLE_NODE,
	HOST_MERGE_ONE_FAILED,
	HOST_MERGE_TWO_FAILED,
};

struct shadow_pte {
    unsigned long va;
	unsigned long pfn;
};

struct desc_entry {
	uint32_t pages_rkey;
	uint64_t pages_base_addr;
};

struct shadow_pt_descriptor {
    int mm_id;
    uint32_t map_rkey;
    uint64_t pt_base_addr;
	struct desc_entry desc_entries[MAX_PAGES_DESCS];
    uint64_t entry_cnt;
};

struct shadow_pt {
    int mm_id;
    uint64_t entry_cnt;
    struct shadow_pte* va2dma_map; 
};

// WARNING: Make it 32 byte size
struct ksm_event_log {
	enum event_tag type;
	union {
		// Unstable merge related
		struct {
			uint64_t from_va;
			uint64_t to_va;
			int from_mm_id;
			int to_mm_id;
		} unstable_merge;
		// Stable merge related
		struct {
			uint64_t from_va;
			unsigned long kpfn;
			int from_mm_id;
			int shared_cnt;
		} stable_merge;
		// Stale stable node
		struct {
			uint64_t last_va;
			unsigned long kpfn;
			int last_mm_id;
		} stale_node;
	};
};

struct error_table {
	struct ksm_event_log **entry_tables;
	int tables_cnt;
	int total_cnt;
	int capacity;
	int registered;
	struct {
		struct ib_mr* mr[MAX_PAGES_DESCS];
		struct scatterlist* sgt[MAX_PAGES_DESCS];
		int sgt_cnt;
	} rdma;
};

struct error_table_desc_entry {
	uint64_t rkey;
	uint64_t base_addr;
};

struct error_table_descriptor {
	int total_cnt;
	int desc_cnt;
	struct error_table_desc_entry entries[MAX_PAGES_DESCS];
};

struct metadata_descriptor {
    uint64_t pt_cnt;
    struct shadow_pt_descriptor pt_descs[MAX_MM_DESCS];
	struct error_table_descriptor et_descs;
};

enum operation_cmd {
    PAGE_COMPARE,
    PAGE_HASH,
};

struct operation_descriptor {
	enum operation_cmd cmd;
	int id;
	uint64_t rkey;
	uint64_t iova;
	uint64_t page_num;
};

struct operation_result {
	enum operation_cmd cmd;
	int id;
	union {
		uint64_t xxhash;
		int value;
	};
};

struct result_desc {
	int total_scanned_cnt;
	int merged_cnt;
	uint64_t rkey;
	uint64_t result_table_addr;
	uint64_t pad;
};

struct result_table {
	dma_addr_t *unmap_addrs;
	struct ksm_event_log **entry_tables;
	int tables_cnt;
	int total_cnt;
};

int insert_error_log(struct error_table* error_table, enum event_tag tag, struct ksm_event_log* log);
struct error_table* create_error_table(void);
void free_error_table(struct error_table* error_table);
void clear_error_table(void);

void rdma_register_error_table(void);
void rdma_unregister_error_table(void);

struct ksm_cb {
	enum ksm_rdma_state state;
	wait_queue_head_t sem;

	char *addr_str;			/* dst addr string */
	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */

	struct rdma_cm_id *cm_id;

	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct list_head shadow_pt_list;

	struct ib_send_wr md_send_wr;
	struct ib_mr  *md_desc_mr;
	struct ib_sge md_desc_sgl;
	u64           md_desc_dma_addr;
	struct metadata_descriptor md_desc_tx __aligned(16);

	struct ib_recv_wr result_recv_wr;
	struct ib_mr   *result_mr;
	struct ib_sge  result_sgl;
	u64            result_dma_addr;
	struct result_desc result_desc __aligned(16);

	struct ib_send_wr single_op_send_wr;
	struct ib_mr *single_op_desc_mr;
	struct ib_sge single_op_desc_sgl;
	u64          single_op_desc_dma_addr;
	struct operation_descriptor single_op_desc_tx __aligned(16);

	struct ib_recv_wr single_op_recv_wr;
	struct ib_mr *single_op_result_mr;
	struct ib_sge single_op_result_sgl;
	u64          single_op_result_dma_addr;
	struct operation_result single_op_result_rx __aligned(16);

	int tag;
};

extern void (*rdma_create_connection)(struct ksm_cb *cb);
extern int (*rdma_meta_send)(struct ksm_cb *cb);
extern struct result_table* (*rdma_result_recv)(struct ksm_cb *cb, unsigned long* ksm_pages_scanned);
extern int (*rdma_reg_mr)(struct ksm_cb* cb, struct ib_mr* mr, int access);
extern void (*rdma_print_timer)(void);
extern int (*rdma_styx_memcmp)(struct ksm_cb* cb, void *page1, void *page2);
extern unsigned long long (*rdma_styx_hash)(struct ksm_cb* cb, void *page);

extern struct ib_mr *(*do_mlx_ib_alloc_mr)(struct ib_pd *pd, enum ib_mr_type mr_type,
			  u32 max_num_sg);
extern int (*do_mlx_ib_dereg_mr)(struct ib_mr *mr);
extern int (*do_mlx_ib_map_mr_sg)(struct ib_mr *mr, struct scatterlist *sg, int sg_nents,
		 unsigned int *sg_offset, unsigned int page_size);

extern u64 (*do_mlx_ib_dma_map_page)(struct ib_device *dev,
				  struct page *page,
				  unsigned long offset,
				  size_t size,
					 enum dma_data_direction direction);
extern void (*do_mlx_ib_dma_unmap_page)(struct ib_device *dev,
				     u64 addr, size_t size,
				     enum dma_data_direction direction);

extern int (*do_mlx_ib_dma_map_sg)(struct ib_device *dev,
				struct scatterlist *sg, int nents,
				enum dma_data_direction direction);
extern void (*do_mlx_ib_dma_unmap_sg)(struct ib_device *dev,
				  struct scatterlist *sg, int nents,
				  enum dma_data_direction direction);

extern u64 (*do_mlx_ib_dma_map_single)(struct ib_device *dev, void* cpu_addr, size_t size,
				  enum dma_data_direction direction);
extern void (*do_mlx_ib_dma_unmap_single)(struct ib_device *dev,  u64 addr,
				    size_t size, enum dma_data_direction direction);

extern void (*do_mlx_ib_dma_sync_single_for_cpu)(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);
extern void (*do_mlx_ib_dma_sync_single_for_device)(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);

bool try_update_api_function(void);
void init_ksm_rdma(void);
struct ksm_cb* get_ksm_cb(void);

void rdma_register_shadow_mms(void);
void rdma_unregister_shadow_mms(bool disconnected, int curr_iteration);
struct list_head* send_meta_desc(void);
struct result_table* recv_offload_result(unsigned long* ksm_pages_scanned);
void free_result_table(struct result_table* result);

bool okay_to_run(void);

/* Shadow MM related structures */

struct address_to_page_map {
    struct shadow_pte **va_arrays;
	struct xarray page_xa;
    size_t cnt;
    size_t capacity;
	size_t va_array_cnt;
};

struct shadow_mm {
    struct list_head list;

	int mm_id;
    struct address_to_page_map pt_map;
	struct ksm_cb* connected_cb;
    
	struct scatterlist* map_sgt;
	int map_sg_cnt;
	struct ib_mr *map_mr;
    dma_addr_t map_dma_addr;
	struct shadow_pte *va_array_tx;
    
    struct ib_mr *pages_mr[MAX_PAGES_DESCS];
    struct scatterlist* pages_sgt[MAX_PAGES_DESCS];
	int pages_sgt_cnt;
};

struct mm_walk_args {
	struct shadow_mm* shadow_mm;
	struct ksm_mm_slot* mm_slot;
};

struct shadow_mm* create_empty(struct ksm_cb* cb);
int insert_entry_to_shadow_mm(struct shadow_mm* shadow_mm, unsigned long va, unsigned long kpfn, void* rmap_item);
void free_shadow_mm(struct shadow_mm* shadow_mm, bool disconnected, int curr_iteration);
struct shadow_mm* get_shadow_mm(struct list_head* shadow_pt_list, int mm_id);
struct ksm_rmap_item* shadow_mm_lookup(struct shadow_mm* shadow_mm, unsigned long va);
unsigned long get_va_at(struct shadow_mm* shadow_mm, int idx);
