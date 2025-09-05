#ifndef dma_addr_t
#define dma_addr_t unsigned long long
#endif

#ifndef uint64_t
#define uint64_t unsigned long long
#endif

#ifndef uint32_t
#define uint32_t unsigned int
#endif

#ifndef int64_t
#define int64_t long long
#endif

#define MAX_SEND_WR 128
#define MAX_RECV_WR 128
#define MAX_SGE     16

#define SERVER_IP "10.0.25.100"
#define SERVER_PORT 10103

#define MAX_MM_DESCS 32

#define MAX_PAGES_DESCS 512
#define MAX_PAGES_IN_SGL 65536 // => Covers 65536 * 8 = 512KB pages = 2GB

struct shadow_pte {
    unsigned long va;
	unsigned long kpfn;
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

const char *ksm_wr_tag_str(enum ksm_wr_tag tag) {
	switch (tag) {
	case WR_SEND_METADATA:
		return "WR_SEND_METADATA";
	case WR_RECV_METADATA:
		return "WR_RECV_METADATA";
	case WR_SEND_RESULT:
		return "WR_SEND_RESULT";
	case WR_RECV_RESULT:
		return "WR_RECV_RESULT";
	case WR_REG_MR:
		return "WR_REG_MR";
	case WR_READ_MAP:
		return "WR_READ_MAP";
	case WR_READ_PAGE:
		return "WR_READ_PAGE";
    case WR_READ_RESULT:
        return "WR_READ_RESULT";
	case WR_SEND_SINGLE_OP:
        return "WR_SEND_SINGLE_OP";
	case WR_RECV_SINGLE_OP:
        return "WR_RECV_SINGLE_OP";
	case WR_SEND_SINGLE_RESULT:
        return "WR_SEND_SINGLE_RESULT";
	case WR_RECV_SINGLE_RESULT:
        return "WR_RECV_SINGLE_RESULT";
	default:
		return "WR_UNKNOWN";
	}
}

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

#define MAX_RESULT_TABLE_ENTRIES (4096 * 1024 / sizeof(struct ksm_event_log))

struct result_desc {
	int total_scanned_cnt;
	int log_cnt;
	uint64_t rkey;
	uint64_t result_table_addr;
	uint64_t pad;
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

enum offload_mode {
	NO_OFFLOAD = 0,
    SINGLE_OPERATION_OFFLOAD = 1,
	KSM_OFFLOAD = 2,
};

#ifndef KSM_OFFLOAD_MODE
#define KSM_OFFLOAD_MODE KSM_OFFLOAD
#endif

enum offload_mode ksm_offload_mode = KSM_OFFLOAD_MODE;

// enum offload_mode ksm_offload_mode = NO_OFFLOAD;
// enum offload_mode ksm_offload_mode = SINGLE_OPERATION_OFFLOAD;
// enum offload_mode ksm_offload_mode = KSM_OFFLOAD;
