#include "linux/slab.h"
#include "rdma/ib_verbs.h"
#include "rdma/rdma_cm.h"

// #include "/usr/src/ofa_kernel/default/include/rdma/rdma_cm.h"
// #include "/usr/src/ofa_kernel/default/include/rdma/ib_verbs.h"

#include "rdma_common.h"

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

struct result_table {
	dma_addr_t *unmap_addrs;
	struct ksm_event_log **entry_tables;
	int tables_cnt;
	int total_cnt;
};

struct error_table {
	struct ksm_event_log **entry_tables;
	int tables_cnt;
	int total_cnt;
	int capacity;
};

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


#define MAX_HUGE_ALLOC 128
struct huge_alloc_reserve {
	int used;
	void* ptrs[MAX_HUGE_ALLOC];
};

static struct huge_alloc_reserve huge_allocator;

int ksm_rdma_huge_alloc_init(void)
{
	int i;
	memset(&huge_allocator, 0, sizeof(huge_allocator));

	for (i = 0; i < MAX_HUGE_ALLOC; i++) {
		huge_allocator.ptrs[i] = kmalloc(KMALLOC_MAX_SIZE, GFP_KERNEL);

		if (!huge_allocator.ptrs[i]) {
			pr_err("Failed to allocate huge memory\n");
			return -1;
		}
	}

	return 0;
}

void* ksm_rdma_huge_alloc(void)
{
	int i;
	void* ptr;
	for (i = 0; i < MAX_HUGE_ALLOC; i++) {
		if (huge_allocator.ptrs[i]) {
			huge_allocator.used++;
			ptr = huge_allocator.ptrs[i];
			huge_allocator.ptrs[i] = NULL;
			return ptr;
		}
	}
	pr_err("No more huge memory\n");
	return NULL;
}

void ksm_rdma_huge_dealloc(void* ptr) {
	int i;
	for (i = 0; i < MAX_HUGE_ALLOC; i++) {
		if (huge_allocator.ptrs[i] == NULL) {
			huge_allocator.used--;
			huge_allocator.ptrs[i] = ptr;
			return;
		}
	}
	pr_err("Error during freeing huge memory\n");
}

static int debug = 0;
#define DEBUG_LOG(fmt, args...) do { if (debug) pr_info(fmt, ##args); } while (0)

static u64 iteration = 1;

void ksm_rdma_create_connection(struct ksm_cb* cb);
int ksm_rdma_meta_send(struct ksm_cb* cb);
struct result_table* ksm_rdma_result_recv(struct ksm_cb* cb, unsigned long* ksm_pages_scanned);
int ksm_rdma_reg_mr(struct ksm_cb* cb, struct ib_mr* mr, int access);
int ksm_rdma_invalidate_mr(struct ksm_cb* cb, struct ib_mr* mr);

int ksm_rdma_styx_memcmp(struct ksm_cb* cb, struct page *page1, struct page *page2);
unsigned long long ksm_rdma_styx_hash(struct ksm_cb* cb, struct page *page);

/* RDMA stub function declaration */
u64 mlx_ib_dma_map_page(struct ib_device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction direction);
void mlx_ib_dma_unmap_page(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);

u64 mlx_ib_dma_map_single(struct ib_device *dev, void * cpu_addr, size_t size, enum dma_data_direction direction);
void mlx_ib_dma_unmap_single(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);

struct ib_mr *mlx_ib_alloc_mr(struct ib_pd *pd, enum ib_mr_type mr_type, u32 max_num_sg);
int mlx_ib_dereg_mr(struct ib_mr *mr);

int mlx_ib_map_mr_sg(struct ib_mr *mr, struct scatterlist *sg, int sg_nents, unsigned int *sg_offset, unsigned int page_size);

int mlx_ib_dma_map_sg(struct ib_device *dev, struct scatterlist *sg, int nents, enum dma_data_direction direction);
void mlx_ib_dma_unmap_sg(struct ib_device *dev, struct scatterlist *sg, int nents, enum dma_data_direction direction);

void mlx_ib_dma_sync_single_for_cpu(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);
void mlx_ib_dma_sync_single_for_device(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction);
