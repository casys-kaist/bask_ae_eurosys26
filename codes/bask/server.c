#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include <xxhash.h>
#include "server.h"
#include "glib.h"

struct timer {
    unsigned long count;
    unsigned long time_sum;
    struct timespec curr_time;
};

struct timer read_4k_timer = {0, 0, {0, 0}};
struct timer read_8k_timer = {0, 0, {0, 0}};
struct timer memcmp_timer = {0, 0, {0, 0}};
struct timer hash_timer = {0, 0, {0, 0}};
struct timer total_timer = {0, 0, {0, 0}};

struct timer rdma_read_timer = {0, 0, {0, 0}};
struct timer big_hash_timer = {0, 0, {0, 0}};
struct timer revert_timer = {0, 0, {0, 0}};
struct timer ksm_operation_timer = {0, 0, {0, 0}};
struct timer total_snic_timer = {0, 0, {0, 0}};
struct timer rdma_read_wait_timer = {0, 0, {0, 0}};

#define MEASURE_TIME 1

#ifdef MEASURE_TIME
#define START_TIMER(timer) \
    do { \
        clock_gettime(CLOCK_MONOTONIC, &(timer).curr_time); \
    } while (0)

#define END_TIMER(timer) \
    do { \
        struct timespec end_time; \
        clock_gettime(CLOCK_MONOTONIC, &end_time); \
        unsigned long duration = (end_time.tv_sec * 1000000000UL + end_time.tv_nsec) \
                                - ((timer).curr_time.tv_sec * 1000000000UL + (timer).curr_time.tv_nsec); \
        (timer).time_sum += duration; \
        (timer).count += 1; \
        (timer).curr_time.tv_sec = 0; \
        (timer).curr_time.tv_nsec = 0; \
    } while (0)

#define IS_TIMER_START(timer) \
    ((timer).curr_time.tv_sec != 0 || (timer).curr_time.tv_nsec != 0)

#define ABORT_TIMER(timer) \
    do { \
        (timer).curr_time.tv_sec = 0; \
        (timer).curr_time.tv_nsec = 0; \
    } while (0)

#define PRINT_AND_RESET_TIMER(timer, msg) \
    do { \
        if ((timer).count > 0) { \
            printf("[BASK Breakdown], %s, %.2f, us avg, total, %lu, count\n", msg, ((double) (timer).time_sum / (double) (timer).count) / 1000.00, (timer).count); \
        } \
        (timer).count = 0; \
        (timer).time_sum = 0; \
        (timer).curr_time.tv_sec = 0; \
        (timer).curr_time.tv_nsec = 0; \
    } while (0)
#else
#define START_TIMER(timer)
#define END_TIMER(timer)
#define PRINT_AND_RESET_TIMER(timer, msg)
#endif

void print_bask_timer(void) {
    PRINT_AND_RESET_TIMER(rdma_read_timer, "RDMA Read");
    PRINT_AND_RESET_TIMER(big_hash_timer, "Big Hash");
    PRINT_AND_RESET_TIMER(revert_timer, "Revert");
    PRINT_AND_RESET_TIMER(ksm_operation_timer, "KSM Operation");
    PRINT_AND_RESET_TIMER(total_snic_timer, "Total server time");
    PRINT_AND_RESET_TIMER(rdma_read_wait_timer, "RDMA read wait timer");
}

// Forward declarations
static void cleanup_rdma_cb(struct rdma_cb *cb);

rmap_item* lookup_rmap_item(struct ksm_metadata* metadata, int mm_id, struct shadow_pte* pte);
int cmp_and_merge_one(struct ksm_metadata* metadata, struct ksm_log_table* merge_table,
    void* page, rmap_item* curr_item, unsigned int rkey, dma_addr_t addr);
int do_handle_error(struct rdma_cb* cb, struct error_table_descriptor* et_desc);

static int (*ksm_ops)(struct ksm_metadata* metadata, struct ksm_log_table* log_table,
    void* page, rmap_item* curr_item, unsigned int rkey, dma_addr_t addr)  = cmp_and_merge_one;
static unsigned long zero_hash = 0;

void debug_stop(void) {
    while (1) {
        sleep(1);
    }
}

static void die(const char *reason)
{
    perror(reason);
    exit(EXIT_FAILURE);
}

static void cleanup_rdma_cb(struct rdma_cb *cb)
{
    DEBUG_LOG("Cleaning up resources...");
    // Disconnect/destroy connection ID
    if (cb->conn_id) {
        if (cb->qp) {
            rdma_destroy_qp(cb->conn_id);
            cb->qp = NULL;
        }
        rdma_disconnect(cb->conn_id);
        rdma_destroy_id(cb->conn_id);
        cb->conn_id = NULL;
    }
    // Listening ID
    if (cb->listen_id) {
        rdma_destroy_id(cb->listen_id);
        cb->listen_id = NULL;
    }
    // Memory region
    if (cb->md_desc_mr) {
        ibv_dereg_mr(cb->md_desc_mr);
        cb->md_desc_mr = NULL;
    }
    if (cb->ksm_result_mr) {
        ibv_dereg_mr(cb->ksm_result_mr);
        cb->ksm_result_mr = NULL;
    }
    // Metadata
    if (cb->metadata.stable_hash_table) {
        g_hash_table_foreach(cb->metadata.stable_hash_table,  (GHFunc) free_stable_node, NULL);
        g_hash_table_destroy(cb->metadata.stable_hash_table);
        cb->metadata.stable_hash_table = NULL;
    }
    
    if (cb->metadata.unstable_hash_table) {
        g_hash_table_destroy(cb->metadata.unstable_hash_table);
        cb->metadata.unstable_hash_table = NULL;
    }

    if (cb->metadata.rmap_tree) {
        g_tree_foreach(cb->metadata.rmap_tree, free_rmap_item, NULL);
        g_tree_destroy(cb->metadata.rmap_tree);
        cb->metadata.rmap_tree = NULL;
    }

    // Merge table
    if (cb->log_table.entries) {
        free(cb->log_table.entries);
        cb->log_table.entries = NULL;
    }

    if (cb->comp_chan) {
        ibv_destroy_comp_channel(cb->comp_chan);
        cb->comp_chan = NULL;
    }
    // CQ
    if (cb->cq) {
        ibv_destroy_cq(cb->cq);
        cb->cq = NULL;
    }
    // PD
    if (cb->pd) {
        ibv_dealloc_pd(cb->pd);
        cb->pd = NULL;
    }
    // Event channel
    if (cb->ec) {
        rdma_destroy_event_channel(cb->ec);
        cb->ec = NULL;
    }
}

// Utility to wait for one CQ event and poll for a completion
static int wait_cq_event_and_poll(struct rdma_cb *cb, const char *tag)
{
    struct ibv_wc wc;
    struct ibv_cq *ev_cq = cb->cq;
    memset(&wc, 0, sizeof(wc));

    int n;

    // Poll CQ for fixed times
    // for (int i = 0; i < 100; i++) {
    //     n = ibv_poll_cq(ev_cq, 1, &wc);
    //     if (n > 0) {
    //         goto success;
    //     }
    // }

    while (1) {
        n = ibv_poll_cq(ev_cq, 1, &wc); 
        if (n > 0) {
            goto success;
        }
    }

    // Go to interrupt mode
    if (ibv_req_notify_cq(ev_cq, 0)) {
        fprintf(stderr, "%s: ibv_req_notify_cq failed\n", tag);
        return -1;
    }

    void *ev_ctx;
    if (ibv_get_cq_event(cb->comp_chan, &ev_cq, &ev_ctx)) {
        fprintf(stderr, "%s: ibv_get_cq_event failed\n", tag);
        return -1;
    }
    ibv_ack_cq_events(ev_cq, 1);

    do {
        n = ibv_poll_cq(ev_cq, 1, &wc);
    } while (n == 0);

success:
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s: completion with status=%d(%s)\n", tag, wc.status, ibv_wc_status_str(wc.status));
        fprintf(stderr, "  wr_id=%s(%lu)\n  opcode=%d\n  bytes=%d\n", 
            ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.opcode, wc.byte_len);
        debug_stop();
        return -1;
    }

    // DEBUG_LOG("%s: operation completed successfully\n", tag);
    // DEBUG_LOG("  wr_id=%s(%lu)\n  opcode=%d\n  bytes=%d\n", 
    //     ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.opcode, wc.byte_len);
    return 0;
}

int rdma_read_memory(struct rdma_cb* cb, struct ibv_mr* mr, uint32_t rkey, dma_addr_t addr, uint32_t length, void* buf) {
    struct ibv_send_wr read_wr, *bad_wr = NULL;
    struct ibv_sge sge;
    int ret = 0;

START_TIMER(rdma_read_timer);
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t) buf;
    sge.length = length;
    sge.lkey = mr->lkey;

    memset(&read_wr, 0, sizeof(read_wr));
    read_wr.wr_id = WR_READ_PAGE;
    read_wr.opcode = IBV_WR_RDMA_READ;
    read_wr.sg_list = &sge;
    read_wr.num_sge = 1;
    read_wr.send_flags = IBV_SEND_SIGNALED;
    read_wr.wr.rdma.remote_addr = addr;
    read_wr.wr.rdma.rkey = rkey;

    DEBUG_LOG("[Server] Reading memory from %llx, size %d\n", addr, length);

    if (ibv_post_send(cb->qp, &read_wr, &bad_wr)) {
        fprintf(stderr, "[Server] ibv_post_send failed.\n");
        return -1;
    }

    ret = wait_cq_event_and_poll(cb, "[SERVER MEMORY READ]");
END_TIMER(rdma_read_timer);
    return ret;
}

int rdma_read_page(struct rdma_cb* cb, struct ibv_mr* mr, uint32_t rkey, dma_addr_t addr, void* buf) {
    int ret = rdma_read_memory(cb, mr, rkey, addr, PAGE_SIZE, buf);
    return ret;
}

static pthread_mutex_t page_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t page_worker_cond = PTHREAD_COND_INITIALIZER;

static struct worker_job worker_todo = {
    .metadata = NULL,
    .log_table = NULL,
    .mm_id = -1,
    .va2dma_map = NULL,
    .pages_buf = NULL,
    .num_pages = 0,
    .idx_adjust = 0,
    .rkey = 0,
    .pages_addr = 0,
    .status = NO_WORKER
};

void* ksm_page_worker(void * arg) {
    uint64_t i, idx;
    rmap_item* curr_item;
    int err;
    void* page;

    struct worker_job* work = (struct worker_job*)(&worker_todo);

    while(1) {
        pthread_mutex_lock(&page_worker_mutex);

        // while (work->status != DATA_READY) {
        //     pthread_cond_wait(&worker_cond, &worker_mutex);
        // }

        if (work->status == DATA_READY) {
            for (i = 0; i < work->num_pages; i++) {
                page = (char*)work->pages_buf + i * PAGE_SIZE;

                if (PRE_HASH_ON) {
                    if (i % PRE_HASH_NUM == 0) {
                    int diff = work->num_pages - i;
                    int max_idx = diff > PRE_HASH_NUM ? PRE_HASH_NUM : diff;
                    start_pre_hash_pair_table(page, max_idx);
                    }
                }

                idx = work->idx_adjust + i;
                
                DEBUG_LOG("[KSM Worker] working on va: %lx (%llu-th)\n", work->va2dma_map[idx].va, idx);
                
START_TIMER(ksm_operation_timer);
                rmap_item* curr_item = lookup_rmap_item(work->metadata, work->mm_id, &work->va2dma_map[idx]);
                if (!curr_item) {
                    ERR_LOG_AND_STOP("[KSM] Failed to lookup rmap item.\n");
                }

                int err = ksm_ops(work->metadata, work->log_table, page, curr_item, work->rkey, work->pages_addr + i * PAGE_SIZE);
                if (err) {
                    ERR_LOG_AND_STOP("[KSM] cmp_and_merge_one failed.\n");
                }
END_TIMER(ksm_operation_timer);
            }

            work->status = WORK_DONE;
START_TIMER(rdma_read_wait_timer);
        }
        pthread_mutex_unlock(&page_worker_mutex);
    }
}

static int do_ksm_v3(struct rdma_cb* cb, struct metadata_descriptor* meta_desc) {
    int scanned_cnt = 0, i, j, err;
    struct shadow_pt_descriptor* pt_desc;
    struct shadow_pt* pt;
    struct ibv_send_wr read_wr, *bad_wr = NULL;
    struct ibv_sge sge;
    struct ibv_mr *map_mr;

    struct shadow_pte* va2dma_map;
    
    struct ibv_mr *page_mr;
    void *page_buf, *page;
    dma_addr_t page_addr;

    rmap_item* curr_item;

    void *prev_page_buf = NULL;
    struct ibv_mr *prev_page_mr;
   
    for (i = 0; i < meta_desc->pt_cnt; i++) {
        pt_desc = &meta_desc->pt_descs[i];

        printf("[Server] KSM working on %d-th mm: %d\n", i, pt_desc->mm_id);
        
        pt = (struct shadow_pt*) malloc(sizeof(struct shadow_pt));
        if (!pt) {
            fprintf(stderr, "[Server] malloc for pt failed.\n");
            return -1;
        }

        pt->mm_id = pt_desc->mm_id;
        pt->entry_cnt = pt_desc->entry_cnt;

        va2dma_map = (struct shadow_pte*) calloc(pt->entry_cnt, sizeof(struct shadow_pte));
        if (!va2dma_map) {
            fprintf(stderr, "[Server] calloc for va2dma_map failed.\n");
            return -1;
        }

        pt->va2dma_map = va2dma_map;

        map_mr = ibv_reg_mr(cb->pd, va2dma_map, sizeof(struct shadow_pte) * pt->entry_cnt,
                            IBV_ACCESS_LOCAL_WRITE);

        if (!map_mr) {
            fprintf(stderr, "[Server] ibv_reg_mr for pt->va2dma_map failed.\n");
            return -1;
        }

        // Read the page table
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uintptr_t) va2dma_map;
        sge.length = sizeof(struct shadow_pte) * pt->entry_cnt;
        sge.lkey = map_mr->lkey;

        memset(&read_wr, 0, sizeof(read_wr));
        read_wr.wr_id = WR_READ_MAP;
        read_wr.opcode = IBV_WR_RDMA_READ;
        read_wr.sg_list = &sge;
        read_wr.num_sge = 1;
        read_wr.send_flags = IBV_SEND_SIGNALED;
        read_wr.wr.rdma.remote_addr = pt_desc->pt_base_addr;
        read_wr.wr.rdma.rkey = pt_desc->map_rkey;

        if (ibv_post_send(cb->qp, &read_wr, &bad_wr)) {
            fprintf(stderr, "[Server] ibv_post_send failed.\n");
            return -1;
        }

        if (wait_cq_event_and_poll(cb, "[SERVER PT READ]")) {
            fprintf(stderr, "[Server] wait_cq_event_and_poll failed.\n");
            fprintf(stderr, "[Server] Failed to read pt %llx\n", pt_desc->pt_base_addr);
            return -1;
        }

        if (pt->va2dma_map[0].va == 0) {
            fprintf(stderr, "[Server] Invalid page table read.\n");
            return -1;
        }

        int sgl_nums = DIV_ROUND_UP(pt->entry_cnt, MAX_PAGES_IN_SGL);

        for (int sgl_idx = 0; sgl_idx < sgl_nums; sgl_idx++) {
            unsigned long long this_sgl_size = sgl_idx == (sgl_nums - 1) ? pt->entry_cnt - sgl_idx * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;
            page_buf = malloc(PAGE_SIZE * this_sgl_size);
            if (!page_buf) {
                fprintf(stderr, "[Server] calloc for page_buf failed.\n");
                return -1;
            }
            memset(page_buf, 0, PAGE_SIZE * this_sgl_size);
            DEBUG_LOG("[Server] Reading pages batched size %llu\n", PAGE_SIZE * this_sgl_size);
        
            page_mr = ibv_reg_mr(cb->pd, page_buf, PAGE_SIZE * this_sgl_size, IBV_ACCESS_LOCAL_WRITE);
            if (!page_mr) {
                fprintf(stderr, "[Server] ibv_reg_mr for page_buf failed.\n");
                return -1;
            }

            page_addr = pt_desc->desc_entries[sgl_idx].pages_base_addr;
            if (rdma_read_memory(cb, page_mr, pt_desc->desc_entries[sgl_idx].pages_rkey, page_addr, PAGE_SIZE * this_sgl_size, page_buf)) {
                fprintf(stderr, "[Server][%d] rdma failed for dma addr %llx, size %llu\n", iteration, page_addr, PAGE_SIZE * this_sgl_size);
                return -1;
            }

            while (1) {
                pthread_mutex_lock(&page_worker_mutex);
                if ((worker_todo.status == WORK_DONE) || (worker_todo.status == WORKER_READY)) {
                    scanned_cnt += worker_todo.num_pages;

                    worker_todo.metadata = &cb->metadata;
                    worker_todo.log_table = &cb->log_table;
                    worker_todo.mm_id = pt->mm_id;
                    worker_todo.va2dma_map = pt->va2dma_map;
                    worker_todo.pages_buf = page_buf;
                    worker_todo.num_pages = this_sgl_size;
                    worker_todo.idx_adjust = sgl_idx * MAX_PAGES_IN_SGL;
                    worker_todo.rkey = pt_desc->desc_entries[sgl_idx].pages_rkey;
                    worker_todo.pages_addr = pt_desc->desc_entries[sgl_idx].pages_base_addr;
                    worker_todo.status = DATA_READY;
                    
                    if (IS_TIMER_START(rdma_read_wait_timer)) {
                        END_TIMER(rdma_read_wait_timer);
                    }

                    // pthread_cond_signal(&worker_cond);
                    pthread_mutex_unlock(&page_worker_mutex);
                    break;
                } else {
                    pthread_mutex_unlock(&page_worker_mutex);
                }
            }

            if (prev_page_buf) {
                ibv_dereg_mr(prev_page_mr);
                free(prev_page_buf);
            }

            prev_page_buf = page_buf;
            prev_page_mr = page_mr;
        }

        while (1) {
            pthread_mutex_lock(&page_worker_mutex);
            if (worker_todo.status == WORK_DONE) {
                ABORT_TIMER(rdma_read_wait_timer);
                break;
            } else {
                pthread_mutex_unlock(&page_worker_mutex);
            }
        }
        scanned_cnt += worker_todo.num_pages;

        worker_todo.metadata = NULL;
        worker_todo.log_table = NULL;
        worker_todo.mm_id = -1;
        worker_todo.va2dma_map = NULL;
        worker_todo.pages_buf = NULL;
        worker_todo.num_pages = 0;
        worker_todo.idx_adjust = 0;
        worker_todo.status = WORKER_READY;

        pthread_mutex_unlock(&page_worker_mutex);

        if (prev_page_buf) {
            ibv_dereg_mr(prev_page_mr);
            free(prev_page_buf);
            prev_page_buf = NULL;
        }

        ibv_dereg_mr(map_mr);
        free(pt->va2dma_map);
        free(pt);

        printf("[KSM] Current Metadata status: %d items, %d stable nodes, %d unstable nodes\n",
            g_tree_nnodes(cb->metadata.rmap_tree), g_hash_table_size(cb->metadata.stable_hash_table), g_hash_table_size(cb->metadata.unstable_hash_table));
    }
    printf("[KSM] Hash collision occured: %lu, at most node %lu\n", hash_collision_cnt, hash_collision_cnt_max);
    hash_collision_cnt = 0;
    hash_collision_cnt_max = 0;

    prune_metadata(&cb->metadata, &cb->log_table);

    total_accessed_cnt = 0;
    return scanned_cnt;
}

rmap_item* lookup_rmap_item(struct ksm_metadata* metadata, int mm_id, struct shadow_pte* pte) {
    rmap_item lookup_item, *item;
    lookup_item.mm_id = mm_id;
    lookup_item.va = pte->va;

    item = g_tree_lookup(metadata->rmap_tree, &lookup_item);
    if (!item) {
        DEBUG_LOG("[KSM] New rmap item: mm_id=%d, va=%lx\n", mm_id, pte->va);
        item = (rmap_item*) malloc(sizeof(rmap_item));
        if (!item) {
            fprintf(stderr, "[Server] malloc for item failed.\n");
            return NULL;
        }

        item->state = Volatile;
        item->mm_id = mm_id;
        item->va = pte->va;
        item->old_hash = null_hash;
        item->age = 0;
        
        if (metadata->rmap_tree == NULL) {
            ERR_LOG_AND_STOP("[KSM] rmap tree is null while lookup\n");
        }
        g_tree_insert(metadata->rmap_tree, item, item);
    }

    total_accessed_cnt += 1;
    item->last_access = iteration;
    item->pfn = pte->kpfn;
    
    return item;
}

int cmp_and_merge_one(struct ksm_metadata* metadata, struct ksm_log_table* log_table,
    void* page, rmap_item* curr_item, unsigned int rkey, dma_addr_t addr) {
    // XXH64_hash_t curr_checksum;
    // XXH64_hash_t node_checksum;
    struct stable_node* stable_node, *chain_node;
    rmap_item* unstable_node;
    struct ksm_event_log result_entry;
    hash_pair curr_hash;
    hash_pair node_hash;

again:
    switch (curr_item->state) {
        case None:
        case Unstable:
            ERR_LOG_AND_STOP( "[KSM] Invalid state for item : %d\n", curr_item->state);
            break;
        
        case Stable:
            DEBUG_LOG("[KSM] Already merged stable item.\n");
            
            struct stable_node* curr_node = curr_item->stable_node;
            if (curr_node->pfn != curr_item->pfn) {
                DEBUG_LOG("[KSM] PFN mismatch implies mapping change: %lu vs %lu\n", curr_node->pfn, curr_item->pfn);

                remove_item_from_node(curr_node, curr_item);
                reset_item_state(curr_item);
                
                if (curr_node->shared_cnt == 0) {
                    remove_stale_node_and_log(metadata, curr_node, curr_item, log_table);
                } else {
                    log_item_state_change(log_table, curr_item, curr_node);
                }

                curr_item->volatility_score += 1;
                broken_merges += 1;

                goto again;
            } else {
                /* Additional error check logic required to avoid stale stable node */
                // PFN이 안 바뀌었는데, checksum이 달라진 경우 (리눅스 Page fault는 항상 새 page로 변경함)
                // => unstable merge가 성공한 시점에서, page contents는 지금 checksum이 올바름
                START_TIMER(big_hash_timer);
                curr_hash = calculate_hash_pair(page);
                END_TIMER(big_hash_timer);

                if (!compare_hash_pair_equal(&curr_hash, &curr_item->old_hash)) {
                    if (!compare_hash_pair_equal(&curr_item->old_hash, &curr_node->page_hash)) {
                        ERR_LOG_AND_STOP( "[KSM] Checksum mismatch in Stable item: %lx%lx%lx%lx vs %lx%lx%lx%lx and node %lx%lx%lx%lx\n",
                            PRINT_HASH_PAIR(curr_item->old_hash),
                            PRINT_HASH_PAIR(curr_hash),
                            PRINT_HASH_PAIR(curr_node->page_hash));
                    }

                    if (curr_node->chain.type == HEAD) {
                        g_hash_table_remove(metadata->stable_hash_table, curr_node);

                        curr_node->page_hash = curr_hash;
                        g_tree_foreach(curr_node->sharing_item_tree, update_item_checksum, &curr_hash);

                        chain_node = curr_node->chain.next;
                        while (chain_node) {
                            chain_node->page_hash = curr_hash;
                            g_tree_foreach(chain_node->sharing_item_tree, update_item_checksum, &curr_hash);

                            chain_node = chain_node->chain.next;
                        }

                        g_hash_table_insert(metadata->stable_hash_table, curr_node, curr_node);
                        
                        DEBUG_LOG("Head Node checksum updated: %lx%lx%lx%lx\n", PRINT_HASH_PAIR(curr_node->page_hash));
                    }else{
                        if (!curr_node->chain.prev) {
                            ERR_LOG_AND_STOP("[KSM] Invalid stable node type: %d\n", curr_node->chain.type);
                        }

                        while (curr_node->chain.prev) {
                            curr_node = curr_node->chain.prev;
                        }

                        if (curr_node->chain.type != HEAD) {
                            ERR_LOG_AND_STOP("[KSM] Invalid stable node type: %d\n", curr_node->chain.type);
                        }

                        g_hash_table_remove(metadata->stable_hash_table, curr_node);
                        
                        curr_node->page_hash = curr_hash;
                        g_tree_foreach(curr_node->sharing_item_tree, update_item_checksum, &curr_hash);

                        chain_node = curr_node->chain.next;
                        while (chain_node) {
                            chain_node->page_hash = curr_hash;
                            g_tree_foreach(chain_node->sharing_item_tree, update_item_checksum, &curr_hash);

                            chain_node = chain_node->chain.next;
                        }

                        g_hash_table_insert(metadata->stable_hash_table, curr_node, curr_node);
                        
                        DEBUG_LOG("Chain Node checksum updated: %lx%lx%lx%lx\n", PRINT_HASH_PAIR(curr_node->page_hash));
                    }
                }
            }

            if (curr_item->volatility_score > 0) {
                curr_item->volatility_score -= 1;
            }
            
            break;

        case Volatile:
            DEBUG_LOG("[KSM] Volatile item.\n");
            volatile_items_cnt += 1;
            curr_item->age += 1;

            if (should_skip_item(curr_item)) {
                DEBUG_LOG("[KSM] Skipping volatile item: %llx(%d) skip count: %d\n", curr_item->va, curr_item->mm_id, curr_item->skip_cnt);
                skipped_cnt += 1;
                return 0;
            } else {
                DEBUG_LOG("[KSM] Not Skipped volatile item: %llx(%d) skip count: %d\n", curr_item->va, curr_item->mm_id, curr_item->skip_cnt);
            }

            START_TIMER(big_hash_timer);
            curr_hash = calculate_hash_pair(page);
            END_TIMER(big_hash_timer);

            if (compare_hash_pair_equal(&curr_item->old_hash, &curr_hash)) {
                if (curr_item->volatility_score > 0) {
                    curr_item->volatility_score -= 1;
                }

                // Try to find a match in stable nodes
                stable_node = cmp_with_stable(metadata, page, curr_hash);

                if (stable_node) {
                    if (stable_node->shared_cnt >= MAX_PAGE_SHARING) {
                        ERR_LOG_AND_STOP( "[KSM] Invalid shared count for stable node: %d\n", stable_node->shared_cnt);
                    }

                    if (curr_item->volatility_score > 0) {
                        highly_volatile_but_stable_merged_cnt += 1;
                    }

                    // Merge with stable node                
                    insert_item_to_node(stable_node, curr_item);
                    log_stable_merge(log_table, curr_item, stable_node);

                    DEBUG_LOG("[KSM] %llx(%d) Merged with stable node %lu Shared count: %d\n", curr_item->va, curr_item->mm_id, stable_node->pfn, stable_node->shared_cnt);
                }else{
                    curr_item->old_hash = curr_hash;
                    // Checksum matches with old checksum
                    // Try to find a match in unstable nodes
                    unstable_node = cmp_with_unstable(metadata,  curr_item);
                    if (unstable_node) {
                        // Merge with unstable node. Promote to stable
                        stable_node = (struct stable_node*) malloc(sizeof(struct stable_node));
                        if (!stable_node) {
                            fprintf(stderr, "[Server] malloc for stable_node failed.\n");
                            return -1;
                        }
                        memset(stable_node, 0, sizeof(struct stable_node));

                        stable_node->shared_cnt = 0;
                        stable_node->page_hash = curr_hash;
                        stable_node->pfn = curr_item->pfn;
                        stable_node->sharing_item_tree = g_tree_new(rmap_item_compare);
                        
                        insert_stable_node(metadata, stable_node);

                        insert_item_to_node(stable_node, unstable_node);
                        insert_item_to_node(stable_node, curr_item);

                        log_unstable_merge(log_table, curr_item, unstable_node);

                        if (curr_item->volatility_score > 0 || unstable_node->volatility_score > 0) {
                            highly_volatile_but_unstable_merged_cnt += 1;
                        }

                        DEBUG_LOG("[KSM] %llx(%d) and %llx(%d) Merged into stable node %lu Shared count: %d\n", curr_item->va, curr_item->mm_id, unstable_node->va, unstable_node->mm_id, stable_node->pfn, stable_node->shared_cnt);

                    }else{
                        curr_item->old_hash = curr_hash;                        
                        curr_item->state = Unstable;
                        insert_unstable_node(metadata, curr_item);                                
                    }
                }
            } else {
                if (!compare_hash_pair_equal(&curr_item->old_hash, &null_hash)) {
                    curr_item->volatility_score += 1;
                }

                curr_item->old_hash = curr_hash;
            }

            break;
    }

    return 0;
}

int cmp_and_merge_one_old(struct ksm_metadata* metadata, struct ksm_log_table* log_table,
    void* page, rmap_item* curr_item, unsigned int rkey, dma_addr_t addr) {
    struct stable_node* stable_node, *chain_node;
    rmap_item* unstable_node;
    struct ksm_event_log result_entry;
    hash_pair curr_hash;
    hash_pair node_hash;

again:
    switch (curr_item->state) {
        case None:
        case Unstable:
            ERR_LOG_AND_STOP( "[KSM] Invalid state for item : %d\n", curr_item->state);
            break;
        
        case Stable:
            DEBUG_LOG("[KSM] Already merged stable item.\n");
            
            struct stable_node* curr_node = curr_item->stable_node;
            if (curr_node->pfn != curr_item->pfn) {
                DEBUG_LOG("[KSM] PFN mismatch implies mapping change: %lu vs %lu\n", curr_node->pfn, curr_item->pfn);

                remove_item_from_node(curr_node, curr_item);
                reset_item_state(curr_item);
                
                if (curr_node->shared_cnt == 0) {
                    remove_stale_node_and_log(metadata, curr_node, curr_item, log_table);
                } else {
                    log_item_state_change(log_table, curr_item, curr_node);
                }

                goto again;
            } else {
                /* Additional error check logic required to avoid stale stable node */
                // PFN이 안 바뀌었는데, checksum이 달라진 경우 (리눅스 Page fault는 항상 새 page로 변경함)
                // => unstable merge가 성공한 시점에서, page contents는 지금 checksum이 올바름
                START_TIMER(big_hash_timer);
                curr_hash = calculate_hash_pair(page);
                END_TIMER(big_hash_timer);

                if (!compare_hash_pair_equal(&curr_hash, &curr_item->old_hash)) {
                    if (!compare_hash_pair_equal(&curr_item->old_hash, &curr_node->page_hash)) {
                        ERR_LOG_AND_STOP( "[KSM] Checksum mismatch in Stable item: %lx%lx%lx%lx vs %lx%lx%lx%lx and node %lx%lx%lx%lx\n",
                            PRINT_HASH_PAIR(curr_item->old_hash),
                            PRINT_HASH_PAIR(curr_hash),
                            PRINT_HASH_PAIR(curr_node->page_hash));
                    }

                    if (curr_node->chain.type == HEAD) {
                        g_hash_table_remove(metadata->stable_hash_table, curr_node);

                        curr_node->page_hash = curr_hash;
                        g_tree_foreach(curr_node->sharing_item_tree, update_item_checksum, &curr_hash);

                        chain_node = curr_node->chain.next;
                        while (chain_node) {
                            chain_node->page_hash = curr_hash;
                            g_tree_foreach(chain_node->sharing_item_tree, update_item_checksum, &curr_hash);

                            chain_node = chain_node->chain.next;
                        }

                        g_hash_table_insert(metadata->stable_hash_table, curr_node, curr_node);
                        
                        DEBUG_LOG("Head Node checksum updated: %lx%lx%lx%lx\n", PRINT_HASH_PAIR(curr_node->page_hash));
                    }else{
                        if (!curr_node->chain.prev) {
                            ERR_LOG_AND_STOP("[KSM] Invalid stable node type: %d\n", curr_node->chain.type);
                        }

                        while (curr_node->chain.prev) {
                            curr_node = curr_node->chain.prev;
                        }

                        if (curr_node->chain.type != HEAD) {
                            ERR_LOG_AND_STOP("[KSM] Invalid stable node type: %d\n", curr_node->chain.type);
                        }

                        g_hash_table_remove(metadata->stable_hash_table, curr_node);
                        
                        curr_node->page_hash = curr_hash;
                        g_tree_foreach(curr_node->sharing_item_tree, update_item_checksum, &curr_hash);

                        chain_node = curr_node->chain.next;
                        while (chain_node) {
                            chain_node->page_hash = curr_hash;
                            g_tree_foreach(chain_node->sharing_item_tree, update_item_checksum, &curr_hash);

                            chain_node = chain_node->chain.next;
                        }

                        g_hash_table_insert(metadata->stable_hash_table, curr_node, curr_node);
                        
                        DEBUG_LOG("Chain Node checksum updated: %lx%lx%lx%lx\n", PRINT_HASH_PAIR(curr_node->page_hash));
                    }
                }
            }

            break;
            
        case Volatile:
            DEBUG_LOG("[KSM] Volatile item.\n");
            START_TIMER(big_hash_timer);
            curr_hash = calculate_hash_pair(page);
            END_TIMER(big_hash_timer);
            // Try to find a match in stable nodes
            stable_node = cmp_with_stable(metadata, page, curr_hash);

            if (stable_node) {
                if (stable_node->shared_cnt >= MAX_PAGE_SHARING) {
                    ERR_LOG_AND_STOP( "[KSM] Invalid shared count for stable node: %d\n", stable_node->shared_cnt);
                }

                // Merge with stable node                
                insert_item_to_node(stable_node, curr_item);
                log_stable_merge(log_table, curr_item, stable_node);

                DEBUG_LOG("[KSM] %llx(%d) Merged with stable node %lu Shared count: %d\n", curr_item->va, curr_item->mm_id, stable_node->pfn, stable_node->shared_cnt);
            }else{
                // curr_checksum = XXH64(page, PAGE_SIZE, 0);

                if (compare_hash_pair_equal(&curr_item->old_hash, &curr_hash)) {
                    curr_item->old_hash = curr_hash;
                    // Checksum matches with old checksum
                    // Try to find a match in unstable nodes
                    unstable_node = cmp_with_unstable(metadata,  curr_item);
                    if (unstable_node) {
                        if (!compare_hash_pair_equal(&unstable_node->old_hash, &curr_hash)) {
                            ERR_LOG_AND_STOP("[KSM] Checksum mismatch: %lx%lx%lx%lx vs %lx%lx%lx%lx", 
                                PRINT_HASH_PAIR(unstable_node->old_hash), PRINT_HASH_PAIR(curr_hash));
                        }

                        // Merge with unstable node. Promote to stable
                        stable_node = (struct stable_node*) malloc(sizeof(struct stable_node));
                        if (!stable_node) {
                            fprintf(stderr, "[Server] malloc for stable_node failed.\n");
                            return -1;
                        }
                        memset(stable_node, 0, sizeof(struct stable_node));

                        stable_node->shared_cnt = 0;
                        stable_node->page_hash = curr_hash;
                        stable_node->pfn = curr_item->pfn;
                        stable_node->sharing_item_tree = g_tree_new(rmap_item_compare);
                        
                        insert_stable_node(metadata, stable_node);

                        insert_item_to_node(stable_node, unstable_node);
                        insert_item_to_node(stable_node, curr_item);

                        log_unstable_merge(log_table, curr_item, unstable_node);

                        DEBUG_LOG("[KSM] %llx(%d) and %llx(%d) Merged into stable node %lu Shared count: %d\n", curr_item->va, curr_item->mm_id, unstable_node->va, unstable_node->mm_id, stable_node->pfn, stable_node->shared_cnt);

                        // free(unstable_node);
                    }else{
                        curr_item->old_hash = curr_hash;
                        // Insert as new unstable node
                        // unstable_node = (struct unstable_node*) malloc(sizeof(struct unstable_node));
                        // if (!unstable_node) {
                        //     fprintf(stderr, "[Server] malloc for unstable_node failed.\n");
                        //     return -1;
                        // }
                        // unstable_node->item = curr_item;
                        
                        curr_item->state = Unstable;
                        // curr_item->unstable_node = unstable_node;
                        insert_unstable_node(metadata, curr_item);                                
                    }

                } else {
                    // Not a merge candidate
                    curr_item->old_hash = curr_hash;
                }
            }

            break;
    }

    return 0;
}

int do_handle_error(struct rdma_cb* cb, struct error_table_descriptor* et_desc) {
    int i, j, total_log_cnt = 0, this_log_cnt = 0, undo_cnt;
    unsigned long this_sgl_size, total_sgl_entries;
    void* buf;
    struct ibv_mr* buf_mr;

    rmap_item lookup_item, *item, *from_item, *to_item;
    struct stable_node* curr_node;
    struct ksm_event_log result_entry;

    total_log_cnt = et_desc->total_cnt;
    total_sgl_entries = DIV_ROUND_UP(et_desc->total_cnt * sizeof(struct ksm_event_log), PAGE_SIZE);
    for (i = 0; i < et_desc->desc_cnt; i++) {
        this_sgl_size = i == (et_desc->desc_cnt - 1) ? total_sgl_entries - i * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;

        buf = malloc(PAGE_SIZE * this_sgl_size);
        if (!buf) {
            fprintf(stderr, "[Server] malloc for buf failed.\n");
            return -1;
        }
        memset(buf, 0, PAGE_SIZE * this_sgl_size);

        buf_mr = ibv_reg_mr(cb->pd, buf, PAGE_SIZE * this_sgl_size, IBV_ACCESS_LOCAL_WRITE);
        if (!buf_mr) {
            fprintf(stderr, "[Server] ibv_reg_mr for buf failed.\n");
            return -1;
        }

        if (rdma_read_memory(cb, buf_mr, et_desc->entries[i].rkey, et_desc->entries[i].base_addr,
                             PAGE_SIZE * this_sgl_size, buf)) {
            fprintf(stderr, "[Server] rdma_read_memory failed.\n");
            return -1;
        }

        this_log_cnt = MIN(total_log_cnt, this_sgl_size * PAGE_SIZE / sizeof(struct ksm_event_log));
        for (j = 0; j < this_log_cnt; j++) {
            struct ksm_event_log* entry = (struct ksm_event_log*) (buf + j * sizeof(struct ksm_event_log));
            switch (entry->type) {
                case HOST_STALE_STABLE_NODE:
                    ERR_LOG_AND_STOP( "[KSM][%d-th] HOST_STALE_STABLE_NODE: %lu\n", j, entry->stale_node.kpfn);
                    break;
                case HOST_NO_STABLE_NODE:
                    ERR_LOG_AND_STOP( "[KSM][%d-th] HOST_NO_STABLE_NODE - currently unreachable\n", i);
                    break;
                case HOST_MERGE_ONE_FAILED:
                DEBUG_LOG("[KSM][%d-th] HOST_MERGE_ONE_FAILED: %llx(%d) -> %lu\n", j,
                    entry->stable_merge.from_va, entry->stable_merge.from_mm_id, entry->stable_merge.kpfn);
                    
                    lookup_item.mm_id = entry->stable_merge.from_mm_id;
                    lookup_item.va = entry->stable_merge.from_va;

                    item = g_tree_lookup(cb->metadata.rmap_tree, &lookup_item);
                    if (!item) {
                        ERR_LOG_AND_STOP( "[KSM] lookup_rmap_item failed: %d->%llx\n", lookup_item.mm_id, lookup_item.va);
                    }

                    curr_node = item->stable_node;
                    if (!curr_node) {
                        ERR_LOG_AND_STOP( "[KSM] Invalid stable node for item in merge one: %llx(%d)\n", entry->stable_merge.from_va, entry->stable_merge.from_mm_id);
                    }

                    if (curr_node->pfn != entry->stable_merge.kpfn) {
                        ERR_LOG_AND_STOP( "[KSM] Unexpected pfn while undoing stable merge: %lu vs %lu\n", curr_node->pfn, entry->stable_merge.kpfn);
                    }

                    if (curr_node->shared_cnt < 1) {
                        ERR_LOG_AND_STOP("[KSM] Invalid shared count for stable node: %lu - %d\n", curr_node->pfn, curr_node->shared_cnt);
                    }

                    remove_item_from_node(curr_node, item);
                    reset_item_state(item);
                    item->volatility_score += 1;

                    if (curr_node->shared_cnt == 0) {
                        remove_stale_node_and_log(&cb->metadata, curr_node, item, &cb->log_table);
                    }

                    break;
                case HOST_MERGE_TWO_FAILED:
                    DEBUG_LOG("[KSM][%d-th] HOST_MERGE_TWO_FAILED: %llx(%d) -> %llx(%d)\n", j,
                    entry->unstable_merge.from_va, entry->unstable_merge.from_mm_id, entry->unstable_merge.to_va, entry->unstable_merge.to_mm_id);

                    undo_cnt = 0;
                    lookup_item.mm_id = entry->unstable_merge.from_mm_id;
                    lookup_item.va = entry->unstable_merge.from_va;
                    from_item = g_tree_lookup(cb->metadata.rmap_tree, &lookup_item);
                    if (!from_item) {
                        ERR_LOG_AND_STOP( "[KSM] lookup_rmap_item failed: %d->%llx\n", lookup_item.mm_id, lookup_item.va);
                    }

                    curr_node = from_item->stable_node;
                    if (!curr_node) {
                        ERR_LOG_AND_STOP( "[KSM] Invalid stable node for item in merge two: %llx(%d)\n", entry->unstable_merge.from_va, entry->unstable_merge.from_mm_id);
                    }
                    
                    g_tree_foreach(curr_node->sharing_item_tree, reset_each_item_state, &undo_cnt);
                    
                    DEBUG_LOG("    Undo merge related to stable node %lu - %d\n", curr_node->pfn, undo_cnt);

                    remove_stable_node_no_item(&cb->metadata, curr_node);
                    break;
                default:
                    ERR_LOG_AND_STOP( "[KSM] Invalid event type: %d\n", entry->type);
                    break;
            }
        }
        total_log_cnt -= this_log_cnt;

        ibv_dereg_mr(buf_mr);
        free(buf);
    }
    return 0;
}

/*=================================================================================================================*/

static void start_listening(struct rdma_cb *cb)
{
    DEBUG_LOG("Creating event channel...");
    cb->ec = rdma_create_event_channel();
    if (!cb->ec)
        die("rdma_create_event_channel");

    DEBUG_LOG("Creating listening ID...");
    if (rdma_create_id(cb->ec, &cb->listen_id, NULL, RDMA_PS_TCP))
        die("rdma_create_id");

    // Bind to hard-coded IP/port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) <= 0)
        die("inet_pton - invalid IP address");

    DEBUG_LOG("Binding address...");
    if (rdma_bind_addr(cb->listen_id, (struct sockaddr*)&addr))
        die("rdma_bind_addr");

    DEBUG_LOG("Listening...");
    if (rdma_listen(cb->listen_id, 1))
        die("rdma_listen");

    printf("[Server] Listening on %s:%d.\n", SERVER_IP, SERVER_PORT);
}

static void on_connect_request(struct rdma_cb *cb, struct rdma_cm_id *child_id)
{
    printf("[Server] Got CONNECT_REQUEST.\n");

    cb->verbs = child_id->verbs;
    cb->pd = ibv_alloc_pd(cb->verbs);
    if (!cb->pd) {
        fprintf(stderr, "[Server] ibv_alloc_pd failed.\n");
        goto err;
    }

    cb->comp_chan = ibv_create_comp_channel(child_id->verbs);
    if (!cb->comp_chan) {
        perror("ibv_create_comp_channel");
        goto err;
    }

    cb->cq = ibv_create_cq(cb->verbs, MAX_SEND_WR + MAX_RECV_WR,
                           NULL, cb->comp_chan, 0);
    if (!cb->cq) {
        fprintf(stderr, "[Server] ibv_create_cq failed.\n");
        goto err;
    }

    // if (ibv_req_notify_cq(cb->cq, 0)) {
    //     fprintf(stderr, "[Server] ibv_req_notify_cq failed.\n");
    //     goto err;
    // }

    // Create QP
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cb->cq,
        .recv_cq = cb->cq,
        .cap     = {
            .max_send_wr  = MAX_SEND_WR,
            .max_recv_wr  = MAX_RECV_WR,
            .max_send_sge = MAX_SGE,
            .max_recv_sge = MAX_SGE,
        },
        .qp_type = IBV_QPT_RC
    };

    DEBUG_LOG("Creating QP...");
    if (rdma_create_qp(child_id, cb->pd, &qp_init_attr)) {
        fprintf(stderr, "[Server] rdma_create_qp failed.\n");
        goto err;
    }

    cb->conn_id = child_id;
    cb->qp      = child_id->qp;

    cb->md_desc_mr = ibv_reg_mr(cb->pd, &cb->md_desc_rx, sizeof(cb->md_desc_rx),
                                 IBV_ACCESS_LOCAL_WRITE);
    if (!cb->md_desc_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr for metadata failed.\n");
        goto err;
    }

    cb->ksm_result_mr = ibv_reg_mr(cb->pd, &cb->result_desc_tx, sizeof(cb->result_desc_tx),
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!cb->ksm_result_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr for ksm_result failed.\n");
        goto err;
    }

    cb->single_op_desc_mr = ibv_reg_mr(cb->pd, &cb->single_op_desc_rx, sizeof(cb->single_op_desc_rx),
                                    IBV_ACCESS_LOCAL_WRITE);
    if (!cb->single_op_desc_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr for op_desc_mr failed.\n");
        goto err;
    }

    cb->single_op_result_mr = ibv_reg_mr(cb->pd, &cb->single_op_result_tx, sizeof(cb->single_op_result_tx),
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!cb->ksm_result_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr for op_result failed.\n");
        goto err;
    }

    // Post one receive
    struct ibv_sge sge;
    struct ibv_recv_wr recv_wr, *bad_wr = NULL;
    switch (ksm_offload_mode) {
        case SINGLE_OPERATION_OFFLOAD:
            sge.addr   = (uintptr_t)&cb->single_op_desc_rx;
            sge.length = sizeof(cb->single_op_desc_rx);
            sge.lkey   = cb->single_op_desc_mr->lkey;
        
            memset(&recv_wr, 0, sizeof(recv_wr));
            recv_wr.wr_id   = WR_RECV_SINGLE_OP;
            recv_wr.sg_list = &sge;
            recv_wr.num_sge = 1;
        
            if (ibv_post_recv(cb->qp, &recv_wr, &bad_wr)) {
                fprintf(stderr, "[Server] ibv_post_recv failed.\n");
                goto err;
            }

            break;
        case KSM_OFFLOAD:
            sge.addr   = (uintptr_t)&cb->md_desc_rx;
            sge.length = sizeof(cb->md_desc_rx);
            sge.lkey   = cb->md_desc_mr->lkey;
        
            memset(&recv_wr, 0, sizeof(recv_wr));
            recv_wr.wr_id   = WR_RECV_METADATA; // just a tag
            recv_wr.sg_list = &sge;
            recv_wr.num_sge = 1;
        
            if (ibv_post_recv(cb->qp, &recv_wr, &bad_wr)) {
                fprintf(stderr, "[Server] ibv_post_recv failed.\n");
                goto err;
            }

            break;
        default:
            fprintf(stderr, "[Server] Invalid server operation mode.\n");
            break;
    }

    // Accept
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.responder_resources = 1;
    conn_param.initiator_depth     = 1;
    conn_param.rnr_retry_count     = 7;

    DEBUG_LOG("Accepting connection...");
    if (rdma_accept(child_id, &conn_param)) {
        fprintf(stderr, "[Server] rdma_accept failed.\n");
        goto err;
    }

    printf("[Server] Connection accepted.\n");
    return;

err:
    cleanup_rdma_cb(cb);
}

static void on_established(struct rdma_cb *cb)
{
    int err;
    struct ibv_sge sge_tx, sge_rx;
    struct ibv_recv_wr recv_wr, *bad_wr_recv = NULL;
    struct ibv_send_wr send_wr, *bad_wr_send = NULL;

    struct ibv_mr *result_mr = NULL;

    printf("[Server] Connection ESTABLISHED.\n");

    cb->metadata.rdma_buf.temp_buf = malloc(PAGE_SIZE);
    if (!cb->metadata.rdma_buf.temp_buf) {
        fprintf(stderr, "[Server] malloc for temp buf failed.\n");
        cleanup_rdma_cb(cb);
        return;
    }

    cb->metadata.rdma_buf.temp_buf_mr = ibv_reg_mr(cb->pd, cb->metadata.rdma_buf.temp_buf, PAGE_SIZE,
                                          IBV_ACCESS_LOCAL_WRITE);
    if (!cb->metadata.rdma_buf.temp_buf_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr for temp buf failed.\n");
        cleanup_rdma_cb(cb);
        return;
    }

    cb->metadata.rdma_buf.cb = cb;

    printf("[Server] Intialization Done...\n");

    for (;;) {
        printf("[Server] Waiting for metadata...\n");

        // Receive metadata to operate on
        if (wait_cq_event_and_poll(cb, "[SERVER Metadata RECV]")) {
            fprintf(stderr, "[Server] wait_cq_event_and_poll failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }
        START_TIMER(total_snic_timer);
        printf("[Server] Metadata received: pt_cnt=%llu, et_cnt=%d\n", cb->md_desc_rx.pt_cnt, cb->md_desc_rx.et_descs.total_cnt);

        for (int i = 0; i < MAX_MM_DESCS; i++) {
            printf("[Server] Metadata[%d]:\n", i);
            printf("  mm_id=%d\n", cb->md_desc_rx.pt_descs[i].mm_id);
            printf("  pt_rkey=%x\n", cb->md_desc_rx.pt_descs[i].map_rkey);
            printf("  pt_base_addr=%llx\n", cb->md_desc_rx.pt_descs[i].pt_base_addr);
            printf("  pt_length=%llu\n", cb->md_desc_rx.pt_descs[i].entry_cnt);
        }

        if (result_mr) {
            printf("[Server] Clean up previous result\n");
            ibv_dereg_mr(result_mr);
            memset(&cb->result_desc_tx, 0, sizeof(cb->result_desc_tx));
            clear_log_table(&cb->log_table);
        }

        START_TIMER(revert_timer);
        // Apply error logs from host
        err = do_handle_error(cb, &cb->md_desc_rx.et_descs);
        if (err) {
            fprintf(stderr, "[Server] do_handle_error failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }
        END_TIMER(revert_timer);

        // Do operations
        cb->result_desc_tx.total_scanned_cnt = do_ksm_v3(cb, &cb->md_desc_rx); //rand() % 100;
        if (cb->result_desc_tx.total_scanned_cnt < 0) {
            fprintf(stderr, "[Server] do_ksm failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }
        END_TIMER(total_snic_timer);
        print_bask_timer();
        
        result_mr = ibv_reg_mr(cb->pd, cb->log_table.entries, 
            sizeof(struct ksm_event_log) * cb->log_table.capacity, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!result_mr) {
            fprintf(stderr, "[Server] ibv_reg_mr for result failed. size: %ld, error: %d(%s)\n", sizeof(struct ksm_event_log) * cb->log_table.cnt, errno, strerror(errno));
            cleanup_rdma_cb(cb);
            return;
        }
        cb->result_desc_tx.rkey = result_mr->rkey;
        cb->result_desc_tx.log_cnt = cb->log_table.cnt;
        cb->result_desc_tx.result_table_addr = (uintptr_t)cb->log_table.entries;

        printf("Pre hash effect: hit ,%lu, miss ,%lu\n", hit_count, miss_count);
        printf("[Server][%d] KSM scanned %d pages and merged %d. Also %d rmap_itmes and skipped %ld items\n", iteration, 
            cb->result_desc_tx.total_scanned_cnt, cb->result_desc_tx.log_cnt, g_tree_nnodes(cb->metadata.rmap_tree), skipped_cnt);
        
        printf("[Log] %d, %d, %ld, %ld, %ld, %ld, %ld\n", iteration, cb->result_desc_tx.total_scanned_cnt, skipped_cnt, volatile_items_cnt, highly_volatile_but_stable_merged_cnt, highly_volatile_but_unstable_merged_cnt, broken_merges);
        
        hit_count = 0;
        miss_count = 0;

        skipped_cnt = 0;
        volatile_items_cnt = 0;
        highly_volatile_but_stable_merged_cnt = 0;
        highly_volatile_but_unstable_merged_cnt = 0;
        broken_merges = 0;

        // Send back the result
        sge_tx.addr   = (uintptr_t)&cb->result_desc_tx;
        sge_tx.length = sizeof(cb->result_desc_tx);
        sge_tx.lkey   = cb->ksm_result_mr->lkey;
        
        memset(&send_wr, 0, sizeof(send_wr));
        send_wr.wr_id   = WR_SEND_RESULT;
        send_wr.sg_list = &sge_tx;
        send_wr.opcode = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.num_sge = 1;

        if (ibv_post_send(cb->qp, &send_wr, &bad_wr_send)) {
            fprintf(stderr, "[Server] ibv_post_send failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        if (wait_cq_event_and_poll(cb, "[SERVER Result SEND]")) {
            fprintf(stderr, "[Server] wait_cq_event_and_poll failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        // Wait for receiving next metadata
        sge_rx.addr   = (uintptr_t)&cb->md_desc_rx;
        sge_rx.length = sizeof(cb->md_desc_rx);
        sge_rx.lkey   = cb->md_desc_mr->lkey;
        memset(&recv_wr, 0, sizeof(recv_wr));
        recv_wr.wr_id   = WR_RECV_METADATA;
        recv_wr.sg_list = &sge_rx;
        recv_wr.num_sge = 1;

        if (ibv_post_recv(cb->qp, &recv_wr, &bad_wr_recv)) {
            fprintf(stderr, "[Server] ibv_post_recv failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        iteration += 1;
    }
}

static void on_established_ops_offload_mode(struct rdma_cb *cb)
{
    int err;
    struct ibv_sge sge_tx, sge_rx;
    struct ibv_recv_wr recv_wr, *bad_wr_recv = NULL;
    struct ibv_send_wr send_wr, *bad_wr_send = NULL;
    uint64_t result;

    printf("[Server] Connection ESTABLISHED.\n");

    void* memcmp_buf = malloc(PAGE_SIZE * 2);
    if (!memcmp_buf) {
        fprintf(stderr, "[Server] malloc for memcmp buf failed.\n");
        return;
    }

    struct ibv_mr *memcmp_mr = ibv_reg_mr(cb->pd, memcmp_buf, PAGE_SIZE * 2, IBV_ACCESS_LOCAL_WRITE);
    if (!memcmp_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr failed for memcmp_mr");
        return;
    }

    void* hash_buf = malloc(PAGE_SIZE);
    if (!hash_buf) {
        fprintf(stderr, "[Server] malloc for hash buf failed.\n");
        return;
    }

    struct ibv_mr *hash_mr = ibv_reg_mr(cb->pd, hash_buf, PAGE_SIZE * 2, IBV_ACCESS_LOCAL_WRITE);
    if (!hash_mr) {
        fprintf(stderr, "[Server] ibv_reg_mr failed for memcmp_mr");
        return;
    }

    printf("[Server] Intialization Done...\n");

    for (;;) {
        DEBUG_LOG("[Server] Waiting for operation request...\n");

        // Receive metadata to operate on
        if (wait_cq_event_and_poll(cb, "[SERVER Single operation RECV]")) {
            fprintf(stderr, "[Server] wait_cq_event_and_poll failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        // if (iteration % 100000 == 0) {
        //     PRINT_AND_RESET_TIMER(read_4k_timer, "[Server] 4K Read time");
        //     PRINT_AND_RESET_TIMER(read_8k_timer, "[Server] 8K Read time");
        //     PRINT_AND_RESET_TIMER(memcmp_timer, "[Server] Memcmp time");
        //     PRINT_AND_RESET_TIMER(hash_timer, "[Server] Hash time");
        //     PRINT_AND_RESET_TIMER(total_timer, "[Server] Total time");
        // }
START_TIMER(total_timer);
        DEBUG_LOG("[Server] Operation request received: CMD: %d, ID: %d\n", 
            cb->single_op_desc_rx.cmd, cb->single_op_desc_rx.id);
        memset(&cb->single_op_result_tx, 0, sizeof(cb->single_op_result_tx));

        uint64_t rkey = cb->single_op_desc_rx.rkey;
        uint64_t iova = cb->single_op_desc_rx.iova;
        uint64_t page_num = cb->single_op_desc_rx.page_num;

        switch (cb->single_op_desc_rx.cmd) {
            case PAGE_COMPARE:
                if (page_num != 2) {
                    ERR_LOG_AND_STOP("[SINGLE] Invalid page cnt\n");
                }
                memset(memcmp_buf, 0, PAGE_SIZE * 2);
                
            //START_TIMER(read_8k_timer);
                if (rdma_read_memory(cb, memcmp_mr, rkey, iova, PAGE_SIZE * 2, memcmp_buf)) {
                    ERR_LOG_AND_STOP("[Server][%d] rdma failed for dma addr %llx\n", iteration, iova);
                }
            //END_TIMER(read_8k_timer);

            //START_TIMER(memcmp_timer);
                cb->single_op_result_tx.value = memcmp(&memcmp_buf[0], &memcmp_buf[PAGE_SIZE], PAGE_SIZE);
            //END_TIMER(memcmp_timer);

                DEBUG_LOG("[SINGLE] Memcmp result: %d\n", cb->single_op_result_tx.value);
                break;
            case PAGE_HASH:
                if (page_num != 1) {
                    ERR_LOG_AND_STOP("[SINGLE] Invalid page cnt\n");
                }
                memset(hash_buf, 0, PAGE_SIZE);

            //START_TIMER(read_4k_timer);
                if (rdma_read_page(cb, hash_mr, rkey, iova, hash_buf)) {
                    ERR_LOG_AND_STOP("[Server][%d] rdma failed for dma addr %llx\n", iteration, iova);
                }
            //END_TIMER(read_4k_timer);

            //START_TIMER(hash_timer);
                cb->single_op_result_tx.xxhash = XXH64(hash_buf, PAGE_SIZE, 0);
            //END_TIMER(hash_timer);

                DEBUG_LOG("[SINGLE] Hash result: %llx\n", cb->single_op_result_tx.xxhash);
                break;
            default:
                ERR_LOG_AND_STOP("[SINGLE] Invalid operation cmd\n");
                break;
        }
END_TIMER(total_timer);
        cb->single_op_result_tx.cmd = cb->single_op_desc_rx.cmd;
        cb->single_op_result_tx.id = cb->single_op_desc_rx.id;

        // Send back the result
        sge_tx.addr   = (uintptr_t)&cb->single_op_result_tx;
        sge_tx.length = sizeof(cb->single_op_result_tx);
        sge_tx.lkey   = cb->single_op_result_mr->lkey;
        
        memset(&send_wr, 0, sizeof(send_wr));
        send_wr.wr_id   = WR_SEND_SINGLE_RESULT;
        send_wr.sg_list = &sge_tx;
        send_wr.opcode = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.num_sge = 1;

        if (ibv_post_send(cb->qp, &send_wr, &bad_wr_send)) {
            fprintf(stderr, "[Server] ibv_post_send failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        if (wait_cq_event_and_poll(cb, "[SERVER Result SEND]")) {
            fprintf(stderr, "[Server] wait_cq_event_and_poll failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        // Wait for receiving next metadata
        sge_rx.addr   = (uintptr_t)&cb->single_op_desc_rx;
        sge_rx.length = sizeof(cb->single_op_desc_rx);
        sge_rx.lkey   = cb->single_op_desc_mr->lkey;
        memset(&recv_wr, 0, sizeof(recv_wr));
        recv_wr.wr_id   = WR_RECV_SINGLE_OP;
        recv_wr.sg_list = &sge_rx;
        recv_wr.num_sge = 1;

        if (ibv_post_recv(cb->qp, &recv_wr, &bad_wr_recv)) {
            fprintf(stderr, "[Server] ibv_post_recv failed.\n");
            cleanup_rdma_cb(cb);
            return;
        }

        iteration += 1;
    }
}


static void on_disconnect(struct rdma_cb *cb)
{
    printf("[Server] DISCONNECTED event.\n");
    cleanup_rdma_cb(cb);
}

static void run_event_loop(struct rdma_cb *cb)
{
    while (1) {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(cb->ec, &event)) {
            fprintf(stderr, "[Server] rdma_get_cm_event failed: %s\n",
                strerror(errno));
            break;
        }
        struct rdma_cm_event event_copy = *event;
        rdma_ack_cm_event(event);

        DEBUG_LOG("Got RDMA event %d (status=%d)", event_copy.event, event_copy.status);

        switch (event_copy.event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            on_connect_request(cb, event_copy.id);
            break;

        case RDMA_CM_EVENT_ESTABLISHED:
            if (ksm_offload_mode == SINGLE_OPERATION_OFFLOAD) {
                on_established_ops_offload_mode(cb);
            } else {
                on_established(cb);
            }
            
            break;

        case RDMA_CM_EVENT_DISCONNECTED:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            on_disconnect(cb);
            // single-connection server -> break
            return;

        default:
            printf("[Server] Got unhandled event %d (status=%d).\n",
                   event_copy.event, event_copy.status);
            break;
        }
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    char zero_buf[PAGE_SIZE] = { 0 };

    // Optionally parse a single argument: debug=0 or debug=1
    // e.g.  ./server debug=1
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], "debug=1", 7) == 0) {
                debug = 1;
            } else if (strncmp(argv[i], "no_skip_opt", 11) == 0) {
                smart_scan_opt = 0;
            } else if (strncmp(argv[i], "no_pre_hash_opt", 15) == 0) {
                pre_hash_opt = 0;
            } else if (strncmp(argv[i], "dataplane", 9) == 0) {
                ksm_offload_mode = SINGLE_OPERATION_OFFLOAD;
            } else if (strncmp(argv[i], "old", 3) == 0) {
                ksm_ops = cmp_and_merge_one_old;
                smart_scan_opt = 0;
                pre_hash_opt = 0;
            } else {
                printf("Unknown argument: %s\n", argv[i]);
            }
        }
        printf("[Server] Final config: debug=%d, no_skip_opt=%d, no_pre_hash_opt=%d, styx=%d\n",
               debug, !smart_scan_opt, !pre_hash_opt, ksm_offload_mode == SINGLE_OPERATION_OFFLOAD);
    }
    printf("[Server] debug=%d\n", debug);

    zero_hash = XXH64(&zero_buf, PAGE_SIZE, 0);
    printf("Zero page hash: %lx\n", zero_hash);

    struct rdma_cb cb;
    memset(&cb, 0, sizeof(cb));

    cb.metadata.rmap_tree = g_tree_new(rmap_item_compare);
    cb.metadata.stable_hash_table = g_hash_table_new(stable_node_hash, stable_node_equal);
    cb.metadata.unstable_hash_table = g_hash_table_new(unstable_node_hash, unstable_node_equal);
        
    cb.log_table.entries = calloc(1024, sizeof(struct ksm_event_log));
    cb.log_table.capacity = 1024;

    pthread_t worker;

    if (pthread_create(&worker, NULL, ksm_page_worker, NULL)) {
        fprintf(stderr, "[Server] pthread_create failed.\n");
        return -1;
    }

    init_pre_hash_pair_table();

    if (pthread_detach(worker)) {
        fprintf(stderr, "[Server] pthread_detach failed.\n");
        return -1;
    }

    worker_todo.status = WORKER_READY;

    //table_cleaner_pool = g_thread_pool_new(cleaner_destroy_unstable_bucket, NULL, THREAD_POOL_MAX, TRUE, NULL);
    //g_thread_pool_set_max_idle_time(60);

    start_listening(&cb);
    run_event_loop(&cb);
    cleanup_rdma_cb(&cb);

    printf("[Server] Exiting.\n");
    return 0;
}

