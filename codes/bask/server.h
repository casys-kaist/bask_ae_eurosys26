#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <stdlib.h>
#include <stdio.h>
#include <xxhash.h>
#include <glib.h>

#include "rdma_common.h"

#define PFX "rserver: "
#define GROW_FACTOR 2
#define PAGE_SIZE 4096

#define MAX_PAGE_SHARING 256

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define RMAP_PRUNE_MARGIN 1000

extern void debug_stop(void);

enum item_state {
    None = 0,
    Volatile,
    Unstable,
    Stable
};

typedef struct {
    XXH128_hash_t first_hash;
    XXH128_hash_t second_hash;
} hash_pair;

typedef struct {
    int mm_id;
    short last_access;
    short age;
    uint64_t va;
    unsigned long pfn;
    unsigned long old_pfn;
    // XXH64_hash_t oldchecksum;
    hash_pair old_hash;
    enum item_state state;
    unsigned short volatility_score;
    unsigned short skip_cnt;
    union {
       // struct unstable_node* unstable_node;
        struct stable_node* stable_node;
    };
} rmap_item;

enum node_chain_type {
    HEAD,
    CHAIN,
};

struct stable_node {
    hash_pair page_hash;
    // void* page_buf_head;
    // XXH64_hash_t checksum;
    int shared_cnt;
    unsigned long pfn;
    GTree *sharing_item_tree;
    struct {
        enum node_chain_type type;
        struct stable_node *next;
        struct stable_node *prev;
    } chain;
};

// struct unstable_node {
//     // hash_pair page_hash;
//     // XXH64_hash_t checksum;
//     rmap_item *item;
//     // struct {
//     //    unsigned int rkey;
//     //    dma_addr_t addr;
//     // } rdma_desc;
// };

struct ksm_metadata {
    // GHashTable* rmap_table;
    GTree *rmap_tree;
    GHashTable* stable_hash_table;
    // GTree* stable_tree;
    struct {
        struct rdma_cb* cb;
        void* temp_buf;
        struct ibv_mr* temp_buf_mr;
    } rdma_buf;
    GHashTable* unstable_hash_table;
};

struct ksm_log_table {
    struct ksm_event_log* entries;
    int cnt;
    int capacity;
};

// Simple control block for user-space RDMA
struct rdma_cb {
    struct rdma_event_channel *ec;

    struct rdma_cm_id     *listen_id;   // Listening (server) ID
    struct rdma_cm_id     *conn_id;     // Connected client ID

    struct ibv_context        *verbs;
    struct ibv_pd             *pd;
    struct ibv_cq             *cq;
    struct ibv_qp             *qp;
    struct ibv_comp_channel   *comp_chan;

    struct ibv_mr             *md_desc_mr;
    struct metadata_descriptor md_desc_rx;
    
    struct ibv_mr            *ksm_result_mr;
    struct result_desc        result_desc_tx;

    struct ibv_mr            *single_op_desc_mr;
    struct operation_descriptor single_op_desc_rx;
    struct ibv_mr            *single_op_result_mr;
    struct operation_result  single_op_result_tx;

    struct ksm_metadata       metadata;
    struct ksm_log_table log_table;
};

static int debug = 0;
#define DEBUG_LOG(fmt, ...) \
    do { if (debug) fprintf(stdout, "[DEBUG] " fmt , ##__VA_ARGS__); } while (0)

#define ERR_LOG_AND_STOP(fmt, ...) \
    do { fprintf(stderr, "[ERROR] " fmt , ##__VA_ARGS__); debug_stop(); } while (0)

int rdma_read_memory(struct rdma_cb* cb, struct ibv_mr* mr, uint32_t rkey, dma_addr_t addr, uint32_t length, void* buf);
int rdma_read_page(struct rdma_cb* cb, struct ibv_mr* mr, uint32_t rkey, dma_addr_t addr, void* buf);

static int iteration = 0;
static unsigned long skipped_cnt = 0;
static unsigned long volatile_items_cnt = 0;
static unsigned long highly_volatile_but_stable_merged_cnt = 0;
static unsigned long highly_volatile_but_unstable_merged_cnt = 0;
static unsigned long broken_merges = 0;

static unsigned long hash_collision_cnt = 0;
static unsigned long hash_collision_cnt_max = 0;

static unsigned long total_accessed_cnt = 0;
static int require_rmap_prune = FALSE;

#define THREAD_POOL_MAX 5
static GThreadPool* table_cleaner_pool = NULL;

static gint active_jobs = 0;

static hash_pair null_hash = {
    .first_hash = {0, 0},
    .second_hash = {0, 0}
};

static int pre_hash_opt = 1;
static int smart_scan_opt = 1;

#define PRE_HASH_ON pre_hash_opt
#define SMART_SCAN_ON smart_scan_opt

short skip_volatile(short volatility_score, short age) {
    if (volatility_score > 0) {
        int grace_score = volatility_score + age;

        if (grace_score < 3) {
            return 1;
        } else if (grace_score == 3) {
            return 2;
        } else if (grace_score == 4) {
            return 4;
        } else
            return 8;
    }

    return 0;
}

int should_skip_item(rmap_item* item) {
    if (!SMART_SCAN_ON) return FALSE;

    if (item->state == None || item->state == Stable) {
        return FALSE;
    }
    
    if (item->skip_cnt > 0) {
        item->skip_cnt -= 1;
        return TRUE;
    } else {
        item->skip_cnt = skip_volatile(item->volatility_score, item->age);
    }

    return FALSE;
}

enum working_status {
    NO_WORKER,
    WORKER_READY,
    DATA_READY,
    IN_PROGRESS,
    WORK_DONE,
    STOP,
};

struct worker_job {
    struct ksm_metadata* metadata;
    struct ksm_log_table* log_table;
    int mm_id;
    struct shadow_pte * va2dma_map;
    void* pages_buf;
    uint64_t num_pages;
    uint64_t idx_adjust;
    unsigned long rkey;
    dma_addr_t pages_addr;
    enum working_status status;
};

///////////////////////////////////////////////////////////////////////////////////
//////////////////////////* Hash pair Related Functions *//////////////////////////
///////////////////////////////////////////////////////////////////////////////////

static pthread_spinlock_t pre_hash_worker_lock;
static enum working_status pre_hash_worker_status = NO_WORKER;

static void* pre_hash_pair_table_base_ptr = NULL;
static int pre_hash_pair_max_idx = 0;
static char* pre_hash_pair_chunk = NULL;
static atomic_int pre_hash_pair_curr_idx = 0;

static pthread_t pre_hash_thread_id;
void* pre_hash_worker(void * arg);

unsigned long hit_count = 0;
unsigned long miss_count = 0;

#define PRE_HASH_NUM 16384

int init_pre_hash_pair_table(void) {
    pre_hash_pair_chunk = malloc(PRE_HASH_NUM * sizeof(hash_pair));
    if (!pre_hash_pair_chunk) {
        fprintf(stderr, "[KSM] Failed to allocate memory for pre_hash_pair_chunk\n");
        return -1;
    }
    pthread_spin_init(&pre_hash_worker_lock, PTHREAD_PROCESS_PRIVATE);
    pthread_create(&pre_hash_thread_id, NULL, pre_hash_worker, NULL);
    pre_hash_worker_status = WORKER_READY;

    return 0;
}

void start_pre_hash_pair_table(void* base_ptr, int max_idx) {
    while (1) {
        pthread_spin_lock(&pre_hash_worker_lock);
        if ((pre_hash_worker_status == WORKER_READY) || (pre_hash_worker_status == WORK_DONE)) {
            pre_hash_pair_table_base_ptr = base_ptr;
            atomic_store(&pre_hash_pair_curr_idx, 0);
            pre_hash_pair_max_idx = max_idx;
            pre_hash_worker_status = DATA_READY;
            
            pthread_spin_unlock(&pre_hash_worker_lock);
            break;
        } else if (pre_hash_worker_status == IN_PROGRESS) {
            pre_hash_worker_status = STOP;
            pthread_spin_unlock(&pre_hash_worker_lock);
        } else {
            pthread_spin_unlock(&pre_hash_worker_lock);
        }
    }
}

void* pre_hash_worker(void * arg) {
    int i;

    while (1) {
        pthread_spin_lock(&pre_hash_worker_lock);
        
        if (pre_hash_worker_status == DATA_READY) {
            pre_hash_worker_status = IN_PROGRESS;
            pthread_spin_unlock(&pre_hash_worker_lock);

            memset(pre_hash_pair_chunk, 0, PRE_HASH_NUM * sizeof(hash_pair));
            
            for (i = 0; i < pre_hash_pair_max_idx; i++) {
                pthread_spin_lock(&pre_hash_worker_lock);
                if (pre_hash_worker_status == STOP) {
                    pthread_spin_unlock(&pre_hash_worker_lock);
                    break;
                }
                pthread_spin_unlock(&pre_hash_worker_lock);

                hash_pair* hash = &((hash_pair*)pre_hash_pair_chunk)[i];
                void* page_buf = (char*)pre_hash_pair_table_base_ptr + i * PAGE_SIZE;

                hash->first_hash = XXH3_128bits_withSeed(&page_buf[0], 2048, 0);
                hash->second_hash = XXH3_128bits_withSeed(&page_buf[2048], 2048, 0);

                atomic_fetch_add(&pre_hash_pair_curr_idx, 1);
            }

            pthread_spin_lock(&pre_hash_worker_lock);
            pre_hash_worker_status = WORK_DONE;
            pthread_spin_unlock(&pre_hash_worker_lock);
        } else {
            pthread_spin_unlock(&pre_hash_worker_lock);
        }
    }
}

hash_pair* lookup_pre_hash_pair(const void* page_buf) {
    if (!PRE_HASH_ON) return NULL;

    unsigned long page_idx = ((uintptr_t)page_buf - (uintptr_t)pre_hash_pair_table_base_ptr) / PAGE_SIZE;
    int idx = atomic_load(&pre_hash_pair_curr_idx);

    if (page_idx >= PRE_HASH_NUM) {
        printf("[KSM] Invalid page idx for pre_hash_pair: %ld, curr_idx: %d, hit count: %lu, miss count: %lu\n", page_idx, idx, hit_count, miss_count);
        printf("page buf vs base ptr: %lx - %lx\n", (uintptr_t)page_buf, (uintptr_t)pre_hash_pair_table_base_ptr);
        debug_stop();
    }
    
    if (page_idx < idx) {
        hit_count++;
        return &((hash_pair*)pre_hash_pair_chunk)[page_idx];
    } else {
        miss_count++;
        return NULL;
    }
}

hash_pair calculate_hash_pair(const void* page_buf) {
    hash_pair* pre_hash = lookup_pre_hash_pair(page_buf);

    if (pre_hash) {
        return *pre_hash;
    } else {
        hash_pair hash;
        // TODO: 4KB twice with different seed?
        hash.first_hash = XXH3_128bits_withSeed(&page_buf[0], 2048, 0);
        hash.second_hash = XXH3_128bits_withSeed(&page_buf[2048], 2048, 0);

        return hash;
    }
}

int compare_hash_pair_equal(const hash_pair* hash1, const hash_pair* hash2) {
    if (hash1->first_hash.high64 == hash2->first_hash.high64 &&
        hash1->first_hash.low64 == hash2->first_hash.low64 &&
        hash1->second_hash.high64 == hash2->second_hash.high64 &&
        hash1->second_hash.low64 == hash2->second_hash.low64) {
        return 1;
    } else {
        return 0;
    }
}

#define PRINT_HASH_PAIR(hash) \
    hash.first_hash.high64, hash.first_hash.low64, hash.second_hash.high64, hash.second_hash.low64

///////////////////////////////////////////////////////////////////////////////////
//////////////////////////* Log Table Related Functions *//////////////////////////
///////////////////////////////////////////////////////////////////////////////////

static void insert_ksm_log(struct ksm_log_table* table, struct ksm_event_log* entry) {
    if (table->cnt >= table->capacity) {
        int new_capacity = table->capacity * GROW_FACTOR;
        struct ksm_event_log* new_table = realloc(table->entries, new_capacity * sizeof(struct ksm_event_log));
        if (!new_table) {
            fprintf(stderr, "[KSM] Failed to grow log table: %x\n", new_capacity);
            return;
        }

        table->entries = new_table;
        table->capacity = new_capacity;
    }

    DEBUG_LOG("Insert new to log table: %d\n", table->cnt);

    table->entries[table->cnt] = *entry;

    switch (entry->type) {
        case DPU_STABLE_MERGE:
        case DPU_UNSTABLE_MERGE:
        case DPU_STALE_STABLE_NODE:
        case DPU_ITEM_STATE_CHANGE:
            break;

        default:
            ERR_LOG_AND_STOP("[KSM] Invalid log type: %d\n", entry->type);
            break;
    }

    table->cnt += 1;
}

static void clear_log_table(struct ksm_log_table* table) {
    memset(table->entries, 0, table->capacity * sizeof(struct ksm_event_log));
    table->cnt = 0;
}

static void log_stable_merge(struct ksm_log_table* log_table,
    rmap_item* item, struct stable_node* stable_node) 
{
    struct ksm_event_log result_entry;
    memset(&result_entry, 0, sizeof(result_entry));
    result_entry.type = DPU_STABLE_MERGE;
    result_entry.stable_merge.from_mm_id = item->mm_id;
    result_entry.stable_merge.from_va = item->va;
    result_entry.stable_merge.kpfn = stable_node->pfn;
    result_entry.stable_merge.shared_cnt = stable_node->shared_cnt;
    insert_ksm_log(log_table, &result_entry);
}

static void log_item_state_change(struct ksm_log_table* log_table,
    rmap_item* item, struct stable_node* prelinked_node) 
{
    struct ksm_event_log result_entry;
    memset(&result_entry, 0, sizeof(result_entry));
    result_entry.type = DPU_ITEM_STATE_CHANGE;
    result_entry.stable_merge.from_mm_id = item->mm_id;
    result_entry.stable_merge.from_va = item->va;
    result_entry.stable_merge.kpfn = prelinked_node->pfn;
    result_entry.stable_merge.shared_cnt = prelinked_node->shared_cnt;
    insert_ksm_log(log_table, &result_entry);
}

static void log_unstable_merge(struct ksm_log_table* log_table,
    rmap_item* from_item, rmap_item* to_item)
{
    struct ksm_event_log result_entry;
    memset(&result_entry, 0, sizeof(result_entry));
    result_entry.type = DPU_UNSTABLE_MERGE;
    result_entry.unstable_merge.from_mm_id = from_item->mm_id;
    result_entry.unstable_merge.from_va = from_item->va;
    result_entry.unstable_merge.to_mm_id = to_item->mm_id;
    result_entry.unstable_merge.to_va = to_item->va;
    insert_ksm_log(log_table, &result_entry);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////* Item State Related *//////////////////////////
//////////////////////////////////////////////////////////////////////////

static void insert_item_to_node(struct stable_node* node, rmap_item* item) {
    switch (item->state) {
        case None:
        case Stable:
            ERR_LOG_AND_STOP( "[KSM] Cannot insert to stable node: Invalid item state: %d\n", item->state);
            break;
        default:
            break;
    }

    item->state = Stable;
    item->old_hash = node->page_hash;
    item->old_pfn = item->pfn;
    item->pfn = node->pfn;
    item->stable_node = node;

    node->shared_cnt += 1;
    g_tree_insert(node->sharing_item_tree, item, item);
}

static void remove_item_from_node(struct stable_node* node, rmap_item* item) {
    node->shared_cnt -= 1;
    g_tree_remove(node->sharing_item_tree, item);
}

static void reset_item_state(rmap_item* item) {
    item->state = Volatile;
    item->pfn = item->old_pfn;
    item->old_pfn = 0;
    item->old_hash = null_hash;
    item->stable_node = NULL;
}

////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////* GTree Function Arguments related *//////////////////////////
////////////////////////////////////////////////////////////////////////////////////////

gint rmap_item_compare(gconstpointer a, gconstpointer b) {
    rmap_item *item1 = (rmap_item *)a;
    rmap_item *item2 = (rmap_item *)b;

    if (item1->mm_id < item2->mm_id) {
        return -1;
    } else if (item1->mm_id > item2->mm_id) {
        return 1;
    } else {
        if (item1->va < item2->va) {
            return -1;
        } else if (item1->va > item2->va) {
            return 1;
        } else {
            return 0;
        }
    }
}

gint page_buf_compare(gconstpointer a, gconstpointer b) {
    return memcmp(a, b, PAGE_SIZE);
}

gint free_stable_node(gpointer key, gpointer value, gpointer user_data) {
    struct stable_node *node = (struct stable_node *)value;
    struct stable_node *next;

    while (node) {
        next = node->chain.next;
        g_tree_destroy(node->sharing_item_tree);
        
        free(node);

        node = next;
    }   
    
    return 0;
}

gint free_rmap_item(gpointer key, gpointer value, gpointer user_data) {
    rmap_item *item = (rmap_item *)value;

    free(item);

    return 0;
}

gint reset_each_item_state(gpointer key, gpointer value, gpointer data) {
    rmap_item* item = (rmap_item*)value;
    int* undo_cnt = (int*)data;

    if (item->state != Stable) {
        fprintf(stderr, "[KSM] Invalid item state: %d\n", item->state);
        return -1;
    }

    DEBUG_LOG("[KSM] Undo stable merge for item: %llx(%d) from node %lu\n", item->va, item->mm_id, item->stable_node->pfn);

    reset_item_state(item);
    item->volatility_score += 1;

    *undo_cnt += 1;

    return 0;
}

gint update_item_checksum(gpointer key, gpointer value, gpointer data) {
    rmap_item* item = (rmap_item*)value;
    hash_pair* hash = (hash_pair*)data;

    if (item->state != Stable) {
        ERR_LOG_AND_STOP("[KSM] Invalid item state during checksum update: %d\n", item->state);
        return -1;
    }

    item->old_hash = *hash;

    return 0;
}

// void unstable_node_free (void *data) {
//     struct unstable_node *node = (struct unstable_node *)data;

//     node->item->state = Volatile;
//     node->item->unstable_node = NULL;

//     free(node);

//     return;
// }

// void cleaner_destroy_unstable_bucket(gpointer data, gpointer user_data) {
//     struct unstable_node *node = (struct unstable_node *)data;
//     unstable_node_free(node);

//     g_atomic_int_dec_and_test(&active_jobs);
// }

///////////////////////////////////////////////////////////////////////////
//////////////////////////* Stable Tree Related *//////////////////////////
///////////////////////////////////////////////////////////////////////////
guint stable_node_hash(gconstpointer v) {
    const struct stable_node* node = (struct stable_node*) v;
    return node->page_hash.first_hash.high64 ^ node->page_hash.first_hash.low64 ^
           node->page_hash.second_hash.high64 ^ node->page_hash.second_hash.low64;
}

gboolean stable_node_equal(gconstpointer a, gconstpointer b){
    const struct stable_node* node_a = (struct stable_node*) a;
    const struct stable_node* node_b = (struct stable_node*) b;

    return compare_hash_pair_equal(&node_a->page_hash, &node_b->page_hash);
}

static struct stable_node* cmp_with_stable(struct ksm_metadata *ksm_meta, void* item_buf, hash_pair hash) {
    struct stable_node lookup_node = {
        .page_hash = hash,
    };

    struct stable_node* stable_node = g_hash_table_lookup(ksm_meta->stable_hash_table, &lookup_node);

    if (stable_node) {
        if (stable_node->shared_cnt < MAX_PAGE_SHARING) {
            return stable_node;
        }else{
            struct stable_node* node_dup = stable_node->chain.next;

            while (node_dup) {
                if (node_dup->shared_cnt < MAX_PAGE_SHARING) {
                    return node_dup;
                }
                node_dup = node_dup->chain.next;
            }

           return NULL;
        }
    } else{
        return NULL;
    }
}

static void insert_stable_node(struct ksm_metadata* ksm_meta, struct stable_node* new_node) {
    struct stable_node* existing_node = g_hash_table_lookup(ksm_meta->stable_hash_table, new_node);
    if (existing_node) {
        while (existing_node->chain.next) {
            existing_node = existing_node->chain.next;
        }
        existing_node->chain.next = new_node;
        
        new_node->chain.type = CHAIN;
        new_node->chain.next = NULL;
        new_node->chain.prev = existing_node;
    } else {
        new_node->chain.type = HEAD;
        new_node->chain.next = NULL;
        new_node->chain.prev = NULL;

        g_hash_table_insert(ksm_meta->stable_hash_table, new_node, new_node);
    }
}

static void remove_stable_node_no_item(struct ksm_metadata* metadata, struct stable_node *node) {
    switch (node->chain.type) {
        case HEAD:
            if (node->chain.next) {
                if (node->chain.prev) {
                    ERR_LOG_AND_STOP("[KSM] Invalid chain type for stable node.\n");
                }

                struct stable_node* next_node = node->chain.next;
                next_node->chain.type = HEAD;
                next_node->chain.prev = NULL;
                g_hash_table_remove(metadata->stable_hash_table, node);
                g_hash_table_insert(metadata->stable_hash_table, next_node, next_node);
            } else {
                g_hash_table_remove(metadata->stable_hash_table, node);
            }

            g_tree_destroy(node->sharing_item_tree);
            free(node);

            break;
        case CHAIN:
            if (node->chain.prev) {
                struct stable_node* prev_node = node->chain.prev;
                struct stable_node* next_node = node->chain.next;

                prev_node->chain.next = next_node;

                if (next_node) {
                    next_node->chain.prev = prev_node;
                }

            } else {
                ERR_LOG_AND_STOP("[KSM] Invalid chain type for stable node.\n");
            }

            g_tree_destroy(node->sharing_item_tree);
            free(node);

            break;
        default:
            ERR_LOG_AND_STOP("[KSM] Invalid chain type for stable node.\n");
            break;
    }
}

static void remove_stale_node_and_log(struct ksm_metadata* metadata, 
    struct stable_node* node, 
    rmap_item* last_item, 
    struct ksm_log_table* log_table) 
{
    struct ksm_event_log result_entry;
    memset(&result_entry, 0, sizeof(result_entry));
    result_entry.type = DPU_STALE_STABLE_NODE;
    result_entry.stale_node.last_mm_id = last_item->mm_id;
    result_entry.stale_node.last_va = last_item->va;
    result_entry.stale_node.kpfn = node->pfn;
    insert_ksm_log(log_table, &result_entry);

    remove_stable_node_no_item(metadata, node);
}

/////////////////////////////////////////////////////////////////////////////
//////////////////////////* Unstable Tree Related *//////////////////////////
/////////////////////////////////////////////////////////////////////////////

guint unstable_node_hash(gconstpointer v) {
    const rmap_item* node = (rmap_item*) v;
    return node->old_hash.first_hash.high64  ^ node->old_hash.first_hash.low64 ^
           node->old_hash.second_hash.high64 ^ node->old_hash.second_hash.low64;
}

gboolean unstable_node_equal(gconstpointer a, gconstpointer b){
    const rmap_item* node_a = (rmap_item*) a;
    const rmap_item* node_b = (rmap_item*) b;

    return compare_hash_pair_equal(&node_a->old_hash, &node_b->old_hash);
}

static rmap_item* cmp_with_unstable(struct ksm_metadata *ksm_meta, rmap_item* item) {
    // struct unstable_node lookup_node = {
    //     .item = item,
    // };
    rmap_item* node = g_hash_table_lookup(ksm_meta->unstable_hash_table, item);

    if (node) {
        g_hash_table_remove(ksm_meta->unstable_hash_table, node);
    }

    return node;
}

static void insert_unstable_node(struct ksm_metadata* ksm_meta, rmap_item* new_node) {
    rmap_item* existing_node = g_hash_table_lookup(ksm_meta->unstable_hash_table, new_node);
    if (existing_node) {
        ERR_LOG_AND_STOP("[KSM] Collision occured Unstable node already exists.\n");
    }

    g_hash_table_insert(ksm_meta->unstable_hash_table, new_node, new_node);
}

void update_item_state(gpointer key, gpointer value, gpointer user_data) {
    rmap_item *node = (rmap_item *)value;
    node->state = Volatile;
}

static void clean_up_unstable_tree(struct ksm_metadata* ksm_meta) {
    // GHashTableIter iter;
    // gpointer key, value;
    // g_hash_table_iter_init(&iter, ksm_meta->unstable_hash_table);

    // if (g_atomic_int_get(&active_jobs) > 0) {
    //     ERR_LOG_AND_STOP("[KSM] Unstable tree cleanup already in progress.\n");
    // }

    // while (g_hash_table_iter_next(&iter, &key, &value)) {
    //     g_atomic_int_inc(&active_jobs);
    //     g_thread_pool_push(table_cleaner_pool, value, NULL);
    // }

    // while ((g_atomic_int_get(&active_jobs) > 0) || (g_thread_pool_unprocessed(table_cleaner_pool) > 0)) {
    //     g_usleep(10000); // sleep 10ms
    // }

    g_hash_table_foreach(ksm_meta->unstable_hash_table, update_item_state, NULL);

    g_hash_table_remove_all(ksm_meta->unstable_hash_table);
}

/////////////////////////////////////////////////////////////////////////////
//////////////////////////* Rmap Tree Related *//////////////////////////////
/////////////////////////////////////////////////////////////////////////////
guint rmap_hash(gconstpointer v) {
    rmap_item* item = (rmap_item*)v;
    unsigned long hash_target = item->va | (item->mm_id & 0xFFF);

    if ((item->va & 0xFFF) != 0) {
        ERR_LOG_AND_STOP("[KSM] Invalid item: mm_id=%d, va=%llx\n", item->mm_id, item->va);
    }

    return hash_target;
}

gboolean rmap_equal(gconstpointer a, gconstpointer b) {
    const rmap_item *item_a = a;
    const rmap_item *item_b = b;
    return (item_a->va == item_b->va) && (item_a->mm_id == item_b->mm_id);
}

typedef struct {
    GTree *Tree;
    GList *keys_to_remove;
} RemoveContext;

gint collect_keys_to_remove(gpointer key, gpointer value, gpointer data) {
    RemoveContext *ctx = (RemoveContext *)data;
    rmap_item *item = (rmap_item *)value;
    
    if (item->last_access < iteration - 1) {
        switch (item->state) {
            case None:
            case Unstable:
                ERR_LOG_AND_STOP("[KSM] Invalid state for item: %d\n", item->state);
                break;
            case Volatile:
                break;
            case Stable:
                if (!item->stable_node) {
                    ERR_LOG_AND_STOP("[KSM] Invalid stable node for item: %llx(%d)\n", item->va, item->mm_id);
                }

                break;
        }
        ctx->keys_to_remove = g_list_prepend(ctx->keys_to_remove, key);
    }

    return FALSE; // continue iteration
}

static void prune_rmap_tree(struct ksm_metadata* ksm_meta, struct ksm_log_table* log_table) {
    RemoveContext ctx;
    ctx.Tree = ksm_meta->rmap_tree;
    ctx.keys_to_remove = NULL;
    int cnt = 0;

    g_tree_foreach(ksm_meta->rmap_tree, collect_keys_to_remove, &ctx);

    for (GList *l = ctx.keys_to_remove; l != NULL; l = l->next) {
        rmap_item* item = l->data;

        if (item->state == Stable && item->stable_node) {
            remove_item_from_node(item->stable_node, item);
            if (item->stable_node->shared_cnt == 0) {
                remove_stale_node_and_log(ksm_meta, item->stable_node, item, log_table);
            }
        }

        g_tree_remove(ctx.Tree, item);
        free(item);
        cnt ++;
    }

    g_list_free(ctx.keys_to_remove);

    printf("[KSM] Cleaned up %d items from rmap tree.\n", cnt);
}

static void prune_metadata(struct ksm_metadata* ksm_meta, struct ksm_log_table* log_table) {
    printf("[KSM] Cleaning up unstable tree...\n");
    clean_up_unstable_tree(ksm_meta);

    if (g_tree_nnodes(ksm_meta->rmap_tree) - total_accessed_cnt > RMAP_PRUNE_MARGIN) {
        printf("[KSM] We have %lu unaccessed items. Cleaning up...\n", g_tree_nnodes(ksm_meta->rmap_tree) - total_accessed_cnt);
        prune_rmap_tree(ksm_meta, log_table);
    }
}
