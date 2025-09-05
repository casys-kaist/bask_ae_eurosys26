#include "linux/math.h"
#include "linux/printk.h"
#include "linux/types.h"
#include "linux/xarray.h"
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/dma-direction.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/scatterlist.h>
#include <rdma/ib_verbs.h>
#include <linux/pagewalk.h>

#include <linux/xxhash.h>

#include <linux/kallsyms.h>

#include "ksm.h"

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

void (*rdma_create_connection)(struct ksm_cb *cb) = NULL;
int (*rdma_meta_send)(struct ksm_cb *cb) = NULL;
struct result_table* (*rdma_result_recv)(struct ksm_cb *cb, unsigned long* ksm_pages_scanned) = NULL;
int (*rdma_reg_mr)(struct ksm_cb* cb, struct ib_mr* mr, int access) = NULL;
void (*rdma_print_timer)(void) = NULL;
int (*rdma_styx_memcmp)(struct ksm_cb* cb, void *page1, void *page2) = NULL;
unsigned long long (*rdma_styx_hash)(struct ksm_cb* cb, void *page) = NULL;

struct ib_mr *(*do_mlx_ib_alloc_mr)(struct ib_pd *pd, enum ib_mr_type mr_type, u32 max_num_sg) = NULL;
int (*do_mlx_ib_dereg_mr)(struct ib_mr *mr) = NULL;
int (*do_mlx_ib_map_mr_sg)(struct ib_mr *mr, struct scatterlist *sg, int sg_nents, unsigned int *sg_offset, unsigned int page_size) = NULL;
u64 (*do_mlx_ib_dma_map_page)(struct ib_device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction dir) = NULL;
void (*do_mlx_ib_dma_unmap_page)(struct ib_device *dev,
				     u64 addr, size_t size,
				     enum dma_data_direction direction) = NULL;
int (*do_mlx_ib_dma_map_sg)(struct ib_device *dev,
                struct scatterlist *sg, int nents,
                enum dma_data_direction direction) = NULL;
void (*do_mlx_ib_dma_unmap_sg)(struct ib_device *dev,
                  struct scatterlist *sg, int nents,
                  enum dma_data_direction direction) = NULL;

u64 (*do_mlx_ib_dma_map_single)(struct ib_device *dev, void* cpu_addr, size_t size,
                  enum dma_data_direction direction) = NULL;
void (*do_mlx_ib_dma_unmap_single)(struct ib_device *dev,  u64 addr,
                    size_t size, enum dma_data_direction direction) = NULL;

void (*do_mlx_ib_dma_sync_single_for_cpu)(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) = NULL;
void (*do_mlx_ib_dma_sync_single_for_device)(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) = NULL;

int (*ksm_huge_alloc_init)(void) = NULL;
void* (*ksm_huge_alloc)(void) = NULL;
void (*ksm_huge_dealloc)(void* ptr) = NULL;

enum offload_mode *current_mode = NULL;

bool try_update_api_function(void) {
    bool ret = true;

    LOOKUP_KSM_RDMA_(create_connection);
    LOOKUP_KSM_RDMA_(meta_send);
    LOOKUP_KSM_RDMA_(result_recv);
    LOOKUP_KSM_RDMA_(reg_mr);
    LOOKUP_KSM_RDMA_(print_timer);
    LOOKUP_KSM_RDMA_(styx_memcmp);
    LOOKUP_KSM_RDMA_(styx_hash);

    ksm_huge_alloc_init = (void *) kallsyms_lookup_name("ksm_rdma_huge_alloc_init"); \
    if (!ksm_huge_alloc_init) { \
        DEBUG_LOG("Failed to find ksm_rdma_huge_alloc_init\n"); \
        ret = false; \
    }

    ksm_huge_alloc = (void *) kallsyms_lookup_name("ksm_rdma_huge_alloc"); \
    if (!ksm_huge_alloc) { \
        DEBUG_LOG("Failed to find ksm_rdma_huge_alloc\n"); \
        ret = false; \
    }

    ksm_huge_dealloc = (void *) kallsyms_lookup_name("ksm_rdma_huge_dealloc"); \
    if (!ksm_huge_dealloc) { \
        DEBUG_LOG("Failed to find ksm_rdma_huge_dealloc\n"); \
        ret = false; \
    }

    LOOKUP_MLX_(ib_alloc_mr);
    LOOKUP_MLX_(ib_dereg_mr);
    LOOKUP_MLX_(ib_map_mr_sg);
    LOOKUP_MLX_(ib_dma_map_page);
    LOOKUP_MLX_(ib_dma_unmap_page);
    LOOKUP_MLX_(ib_dma_map_sg);
    LOOKUP_MLX_(ib_dma_unmap_sg);
    LOOKUP_MLX_(ib_dma_map_single);
    LOOKUP_MLX_(ib_dma_unmap_single);
    LOOKUP_MLX_(ib_dma_sync_single_for_cpu);
    LOOKUP_MLX_(ib_dma_sync_single_for_device);

    current_mode = (void *) kallsyms_lookup_name("ksm_offload_mode");
    if (!current_mode) {
        DEBUG_LOG("Failed to find ksm_offload_mode\n");
        ret = false;
    }

    return ret;
}

void init_cb(void);

bool is_offload_decided = false;
enum remote_status offload_server_status = UNINITIALIZED;
struct ksm_cb* ksm_cb = NULL;
struct error_table* ksm_error_table = NULL;
long fail_reason_cnts[11] = {0};
char *fail_reason_str[11] = {
    "No_mergeable_vma_found",
    "Failed_to_lock_page",
    "Pages_are_not_identical",
    "Page_address_in_vma_failed",
    "page_vma_mapped_walk_failed",
    "Pvmw_pte_is_null",
    "Page_mapcount_unequal",
    "Page_is_shared",
    "Not_an_anonymous_page",
    "Failed_to_split_page",
    "But_same_hash"
};

bool is_rdma_initialized(void) {
    return offload_server_status == INITIALIZED;
}

bool is_ksm_offload(void) {
    return (offload_server_status != DISCONNECTED) 
            && (OFFLOAD_MODE == KSM_OFFLOAD);
}

bool is_styx_offload(void) {
    return (offload_server_status == INITIALIZED)  
            && (OFFLOAD_MODE == SINGLE_OPERATION_OFFLOAD);
}

void init_ksm_rdma(void) {
    if (!is_rdma_initialized()) {
        if (try_update_api_function()) {
            is_offload_decided = true;
            if (OFFLOAD_MODE == NO_OFFLOAD) {
                pr_info("No offload mode\n");
                return;
            }

            ksm_huge_alloc_init();

            offload_server_status = INITIALIZED;
            init_cb();
        } else {
            pr_err("Failed to initialize KSM RDMA\n");
            return;
        }
    }
}

void init_cb(void) {
    ksm_cb = (struct ksm_cb*) kmalloc(sizeof(struct ksm_cb), GFP_KERNEL);
    if (!ksm_cb) {
        pr_err("Failed to allocate ksm_cb\n");
        return;
    }
    memset(ksm_cb, 0, sizeof(struct ksm_cb));

    ksm_cb->tag = sizeof(struct ksm_cb);

    INIT_LIST_HEAD(&ksm_cb->shadow_pt_list);

    ksm_error_table = create_error_table();
    if (!ksm_error_table) {
        pr_err("Failed to create error table\n");
        return;
    }

    rdma_create_connection(ksm_cb);

    msleep(1 * 1000);
    // send_meta_desc();
    // recv_offload_result();

    pr_info("Initialized ksm_cb\n");
}

struct ksm_cb* get_ksm_cb(void) {
    if (!ksm_cb || !is_rdma_initialized()) {
        pr_err("ksm_cb not initialized\n");
        return NULL;
    }
    return ksm_cb;
}

void rdma_register_shadow_mms(void) {
    struct shadow_mm* entry, *tmp;

    struct scatterlist *pages_sgt, *prev_sgt, *curr_sgt, *sg, *map_sg;
    int i, nents, err, j, map_pages_cnt;
    int pt_idx;

    int total_cnt = 0;

    int registered = 0;
    int iter_cnt = 0;
    int iters = 0;
    int this_size = 0;
    int remaining_size = 0;
    int va_idx = 0;

    int sgl_num = 0, sgl_idx = 0;
    long this_sgl_size = 0;

    unsigned long prev_va = 0, this_va;
    struct ksm_rmap_item* item;

    struct page* map_page;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return;
    }

    DEBUG_LOG("Start registering shadow page tables\n");
       
    list_for_each_entry_safe(entry, tmp, &ksm_cb->shadow_pt_list, list) {
        XA_STATE(xa_iter, &entry->pt_map.page_xa, 0);

        pt_idx = ksm_cb->md_desc_tx.pt_cnt++;

        if (entry->pt_map.cnt > MAX_PAGES_DESCS * MAX_PAGES_IN_SGL) {
            pr_err("Too many pages in shadow page table\n");
        }

        sgl_num = (entry->pt_map.cnt + MAX_PAGES_IN_SGL - 1) / MAX_PAGES_IN_SGL;
        for (sgl_idx = 0; sgl_idx < sgl_num; sgl_idx++) {
            registered = 0;
            prev_sgt = NULL;
            pages_sgt = NULL;
            this_sgl_size = sgl_idx == (sgl_num - 1) ? entry->pt_map.cnt - sgl_idx * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;

            DEBUG_LOG("Try map %ld pages\n", this_sgl_size);

            if (this_sgl_size <= SG_CHUNK_SIZE) {
                iters = 1;
            } else {
                remaining_size = this_sgl_size - SG_CHUNK_SIZE;
                iters = (remaining_size + (SG_CHUNK_SIZE - 1) - 1) / (SG_CHUNK_SIZE - 1) + 1;
            }

            prev_va = 0;

            for (iter_cnt = 0; iter_cnt < iters; iter_cnt++) {
                if (iter_cnt == (iters - 1)) {
                    this_size = this_sgl_size - registered;
                    curr_sgt = kzalloc(sizeof(struct scatterlist) * this_size, GFP_KERNEL);
                }else{
                    this_size = SG_CHUNK_SIZE - 1;
                    curr_sgt = kzalloc(sizeof(struct scatterlist) * SG_CHUNK_SIZE, GFP_KERNEL);
                }

                if (!curr_sgt) {
                    pr_err("Failed to allocate sg_table\n");
                }

                if (iter_cnt == 0) {
                    pages_sgt = curr_sgt;
                }

                for (i = 0; i < this_size; i++) {
                    va_idx = sgl_idx * MAX_PAGES_IN_SGL + registered + i;

                    this_va = get_va_at(entry, va_idx);
                    if (prev_va > this_va) {
                        pr_err("Address not ascending: %lx vs %lx\n", prev_va, this_va);
                    }

                    item = xas_next_entry(&xa_iter, ULONG_MAX);
                    if (!item) {
                        pr_err("Failed to find %d-th Page\n", i + registered);
                        debug_stop();
                    }

                    if ((item->address & PAGE_MASK) != this_va) {
                        pr_err("%d-th Page address mismatch: %lx vs %lx\n", va_idx, item->address, this_va);
                        debug_stop();
                    }

                    sg_set_page(&curr_sgt[i], item->page, PAGE_SIZE, 0);
                    prev_va = this_va;
                }
    
                if (prev_sgt) {
                    sg_chain(prev_sgt, SG_CHUNK_SIZE, curr_sgt);
                }
                
                registered += this_size;
                prev_sgt = curr_sgt;    
            }

            if (curr_sgt && (this_size > 0)) {
                sg_mark_end(&curr_sgt[this_size - 1]);
            }

            entry->pages_mr[sgl_idx] = do_mlx_ib_alloc_mr(ksm_cb->pd, IB_MR_TYPE_MEM_REG, this_sgl_size);
            if (IS_ERR(entry->pages_mr)) {
                pr_err("Failed to allocate mr\n");
            }

            nents = do_mlx_ib_dma_map_sg(entry->pages_mr[sgl_idx]->device, pages_sgt, this_sgl_size, DMA_BIDIRECTIONAL);
            DEBUG_LOG("Mapped %d pages\n", nents);
            if (nents <= 0) {
                pr_err("Failed to map sg_table %d\n", nents);
            }

            for_each_sg(pages_sgt, sg, nents, i) {
                DEBUG_LOG("Page at %d: %llx, %u\n", i, sg_dma_address(sg), sg_dma_len(sg));
            }

            err = do_mlx_ib_map_mr_sg(entry->pages_mr[sgl_idx], pages_sgt, nents, NULL, PAGE_SIZE);
            if (err != nents) {
                pr_err("ib_map_mr_sg failed %d vs %d\n", err, nents);
            }

            err = rdma_reg_mr(ksm_cb, entry->pages_mr[sgl_idx], IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ);
            if (err) {
                pr_err("Failed to register mr: %d\n", err);
            }

            if (entry->pages_mr[sgl_idx]->length != PAGE_SIZE * this_sgl_size) {
                pr_err("Page mr size mismatch: %llu\n", entry->pages_mr[sgl_idx]->length);
            }

            entry->pages_sgt[sgl_idx] = pages_sgt;

            ksm_cb->md_desc_tx.pt_descs[pt_idx].desc_entries[sgl_idx].pages_rkey = entry->pages_mr[sgl_idx]->rkey;
            ksm_cb->md_desc_tx.pt_descs[pt_idx].desc_entries[sgl_idx].pages_base_addr = entry->pages_mr[sgl_idx]->iova;
        }
        entry->pages_sgt_cnt = sgl_num;

        {
            BUG_ON(entry->pt_map.va_array_cnt > MAX_VA_ARRAYS);

            map_pages_cnt = (entry->pt_map.capacity * sizeof(struct shadow_pte)) / PAGE_SIZE;
            entry->map_mr = do_mlx_ib_alloc_mr(ksm_cb->pd, IB_MR_TYPE_MEM_REG, map_pages_cnt);
            if (IS_ERR(entry->map_mr)) {
                pr_err("Failed to allocate mr\n");
            }

            // entry->va_array_tx = dma_alloc_coherent(ksm_cb->pd->device->dma_device, map_size, &dma_handle, GFP_KERNEL);
            // if (!entry->va_array_tx) {
            //     pr_err("Failed to allocate va_array_tx\n");
            // }
            // memcpy(entry->va_array_tx, entry->pt_map.va_array, map_size);

            // sg = entry->pages_sgt;
            // for (i = 0; i < entry->pt_map.cnt; i++) {
            //     if (!sg) {
            //         pr_err("No sg for %d-th Page\n", i);
            //     }

            //     if (sg_dma_len(sg) != 0 && i > 0) {
            //         pr_err("New map started in %d-th Page with size: %u\n", i, sg_dma_len(sg));
            //     }
                
            //     entry->va_array_tx[i].dma_addr = sg_dma_address(sg);
            //     sg = sg_next(sg);
            // }


            map_sg = kzalloc(sizeof(struct scatterlist) * map_pages_cnt , GFP_KERNEL);
            if (!map_sg) {
                pr_err("Failed to allocate map_sg\n");
            }

            registered = 0;
            for (i = 0; i < entry->pt_map.va_array_cnt; i++) {
                this_size = i == (entry->pt_map.va_array_cnt - 1) ? entry->pt_map.capacity - i * MAX_CAPACITY_PER_TABLE : MAX_CAPACITY_PER_TABLE;

                for (j = 0; j < this_size; j += (PAGE_SIZE / sizeof(struct shadow_pte))) {
                    if (virt_addr_valid(&entry->pt_map.va_arrays[i][j]) == 0) {
                        pr_err("Invalid address: %lx\n", (uintptr_t) &entry->pt_map.va_arrays[i][j]);
                    }
                    map_page = virt_to_page(&entry->pt_map.va_arrays[i][j]);
                    
                    sg_set_page(&map_sg[registered], map_page, PAGE_SIZE, 0);
                    registered++;
                }
            }
            sg_mark_end(&map_sg[registered - 1]);
            if (registered >= MAX_PAGES_IN_SGL) {
                pr_err("Too many pages in map_sg: %d\n", registered);
                debug_stop();
            }

            nents = do_mlx_ib_dma_map_sg(entry->map_mr->device, map_sg, registered, DMA_BIDIRECTIONAL);
            if (nents <= 0) {
                pr_err("Failed to map sg_table for map %d\n", nents);
            }

            err = do_mlx_ib_map_mr_sg(entry->map_mr, map_sg, nents, NULL, PAGE_SIZE);
            if (err != nents) {
                pr_err("ib_map_mr_sg failed %d\n", err);
            }

            err = rdma_reg_mr(ksm_cb, entry->map_mr, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ);
            if (err) {
                pr_err("Failed to register mr: %d\n", err);
            }

            if (entry->map_mr->length != sizeof(struct shadow_pte) * entry->pt_map.capacity) {
                pr_err("Map mr size mismatch: %llu vs %lu\n", entry->map_mr->length, sizeof(struct shadow_pte) * entry->pt_map.capacity);
                debug_stop();
            }

            do_mlx_ib_dma_sync_single_for_device(entry->map_mr->device, entry->map_mr->iova,  entry->map_mr->length, DMA_BIDIRECTIONAL);
            
            entry->map_dma_addr = entry->map_mr->iova;
            entry->map_sgt = map_sg;
            entry->map_sg_cnt = registered;

            DEBUG_LOG("Registered shadow page table for mm %d -> key %x, addr %llx\n", entry->mm_id, entry->map_mr->rkey, entry->map_dma_addr);
        }
        
        ksm_cb->md_desc_tx.pt_descs[pt_idx].mm_id = entry->mm_id;
        ksm_cb->md_desc_tx.pt_descs[pt_idx].map_rkey = entry->map_mr->rkey;
        ksm_cb->md_desc_tx.pt_descs[pt_idx].pt_base_addr =  entry->map_dma_addr;
        ksm_cb->md_desc_tx.pt_descs[pt_idx].entry_cnt = entry->pt_map.cnt;

        total_cnt += entry->pt_map.cnt;
    }

    DEBUG_LOG("Registered %lld shadow page tables with total %d pages\n", ksm_cb->md_desc_tx.pt_cnt, total_cnt);
}

void rdma_unregister_shadow_mms(bool disconnected, int curr_iteration) {
    struct shadow_mm* entry, *tmp;
    int err, i;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return;
    }

    list_for_each_entry_safe(entry, tmp, &ksm_cb->shadow_pt_list, list) {
        do_mlx_ib_dma_unmap_sg(ksm_cb->pd->device, entry->map_sgt, entry->map_sg_cnt, DMA_BIDIRECTIONAL);
        // do_mlx_ib_dma_unmap_single(ksm_cb->pd->device, entry->map_dma_addr, sizeof(struct shadow_pte) * entry->pt_map.cnt, DMA_BIDIRECTIONAL);
        // dma_free_coherent(ksm_cb->pd->device->dma_device, sizeof(struct shadow_pte) * entry->pt_map.cnt, entry->va_array_tx, entry->map_dma_addr);

        err = do_mlx_ib_dereg_mr(entry->map_mr);
        if (err) {
            pr_err("Failed to deregister mr: %d\n", err);
        }

        for (i = 0; i < entry->pages_sgt_cnt; i++) {
            do_mlx_ib_dma_unmap_sg(ksm_cb->pd->device, entry->pages_sgt[i], entry->pt_map.cnt, DMA_BIDIRECTIONAL);
            err = do_mlx_ib_dereg_mr(entry->pages_mr[i]);
            if (err) {
                pr_err("Failed to deregister mr: %d\n", err);
            }

        }

        list_del(&entry->list);
        free_shadow_mm(entry, disconnected, curr_iteration);
    }

    memset(&ksm_cb->md_desc_tx, 0, sizeof(struct metadata_descriptor));

    DEBUG_LOG("Unregistered all shadow page tables\n");
}

void rdma_register_error_table(void) {
    struct scatterlist *first_sgt, *prev_sgt, *curr_sgt;
    int i, nents, err, entry_pos, array_idx, entry_idx;
    int registered, iter_cnt, iters, this_size, this_sgl_size, total_entries, remaining_size;
    int sgl_num, sgl_idx;
    struct page* page;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return;
    }

    ksm_error_table->registered = ksm_error_table->total_cnt;
    total_entries = DIV_ROUND_UP(ksm_error_table->registered * sizeof(struct ksm_event_log), PAGE_SIZE);
    
    sgl_num = DIV_ROUND_UP(total_entries, MAX_PAGES_IN_SGL);
    for (sgl_idx = 0; sgl_idx < sgl_num; sgl_idx++) {
        registered = 0;
        prev_sgt = NULL;
        first_sgt = NULL;

        this_sgl_size = sgl_idx == (sgl_num - 1) ? total_entries - sgl_idx * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;
        if (this_sgl_size <= SG_CHUNK_SIZE) {
            iters = 1;
        } else {
            remaining_size = this_sgl_size - SG_CHUNK_SIZE;
            iters = (remaining_size + (SG_CHUNK_SIZE - 1) - 1) / (SG_CHUNK_SIZE - 1) + 1;
        }

        for (iter_cnt = 0; iter_cnt < iters; iter_cnt++) {
            if (iter_cnt == (iters - 1)) {
                this_size = this_sgl_size - registered;
                curr_sgt = kzalloc(sizeof(struct scatterlist) * this_size, GFP_KERNEL);
            }else{
                this_size = SG_CHUNK_SIZE - 1;
                curr_sgt = kzalloc(sizeof(struct scatterlist) * SG_CHUNK_SIZE, GFP_KERNEL);
            }

            if (!curr_sgt) {
                pr_err("Failed to allocate sg_table\n");
            }

            if (iter_cnt == 0) {
                first_sgt = curr_sgt;
            }

            for (i = 0; i < this_size; i++) {
                entry_pos = (PAGE_SIZE / sizeof(struct ksm_event_log)) * (sgl_idx * MAX_PAGES_IN_SGL + registered + i);
                array_idx = entry_pos / MAX_RESULT_TABLE_ENTRIES;
                entry_idx = entry_pos % MAX_RESULT_TABLE_ENTRIES;

                page = virt_to_page(&ksm_error_table->entry_tables[array_idx][entry_idx]);

                sg_set_page(&curr_sgt[i], page, PAGE_SIZE, 0);
            }

            if (prev_sgt) {
                sg_chain(prev_sgt, SG_CHUNK_SIZE, curr_sgt);
            }

            registered += this_size;
            prev_sgt = curr_sgt;
        }

        if (curr_sgt && (this_size > 0)) {
            sg_mark_end(&curr_sgt[this_size - 1]);
            DEBUG_LOG("End of sg_table %d, %d\n", iter_cnt, this_size);
            DEBUG_LOG("  -> %lx\n", curr_sgt[this_size - 1].page_link);
        }

        ksm_error_table->rdma.mr[sgl_idx] = do_mlx_ib_alloc_mr(ksm_cb->pd, IB_MR_TYPE_MEM_REG, this_sgl_size);
        if (IS_ERR(ksm_error_table->rdma.mr[sgl_idx])) {
            pr_err("Failed to allocate mr\n");
        }

        nents = do_mlx_ib_dma_map_sg(ksm_error_table->rdma.mr[sgl_idx]->device, first_sgt, this_sgl_size, DMA_BIDIRECTIONAL);
        if (nents <= 0) {
            pr_err("Failed to map sg_table %d\n", nents);
        }

        err = do_mlx_ib_map_mr_sg(ksm_error_table->rdma.mr[sgl_idx], first_sgt, nents, NULL, PAGE_SIZE);
        if (err != nents) {
            pr_err("ib_map_mr_sg failed %d vs %d\n", err, nents);
        }

        err = rdma_reg_mr(ksm_cb, ksm_error_table->rdma.mr[sgl_idx], IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ);
        if (err) {
            pr_err("Failed to register mr: %d\n", err);
        }

        if (ksm_error_table->rdma.mr[sgl_idx]->length != PAGE_SIZE * this_sgl_size) {
            pr_err("Page mr size mismatch: %llu\n", ksm_error_table->rdma.mr[sgl_idx]->length);
        }

        ksm_error_table->rdma.sgt[sgl_idx] = first_sgt;
        ksm_cb->md_desc_tx.et_descs.entries[sgl_idx].rkey = ksm_error_table->rdma.mr[sgl_idx]->rkey;
        ksm_cb->md_desc_tx.et_descs.entries[sgl_idx].base_addr = ksm_error_table->rdma.mr[sgl_idx]->iova;
    }
    ksm_error_table->rdma.sgt_cnt = sgl_num;
    ksm_cb->md_desc_tx.et_descs.total_cnt = ksm_error_table->registered;
    ksm_cb->md_desc_tx.et_descs.desc_cnt = sgl_num;

    pr_info("Registered error table with %d entries with total %d pages\n", ksm_error_table->registered, total_entries);
    return;
}

void rdma_unregister_error_table(void) {
    int err, i, total_entries;
    int iter_cnt, iters, this_size, this_sgl_size, remaining_size, freed;
    struct scatterlist *sg, *curr_sgt, *next_sgt;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return;
    }

    total_entries = DIV_ROUND_UP(ksm_error_table->registered * sizeof(struct ksm_event_log), PAGE_SIZE);
    for (i = 0; i < ksm_error_table->rdma.sgt_cnt; i++) {
        freed = 0;
        this_sgl_size = i == (ksm_error_table->rdma.sgt_cnt - 1) ? total_entries - i * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;
        curr_sgt = ksm_error_table->rdma.sgt[i];

        do_mlx_ib_dma_unmap_sg(ksm_cb->pd->device, ksm_error_table->rdma.sgt[i], this_sgl_size, DMA_BIDIRECTIONAL);
        err = do_mlx_ib_dereg_mr(ksm_error_table->rdma.mr[i]);
        if (err) {
            pr_err("Failed to deregister mr: %d\n", err);
        }

        if (this_sgl_size <= SG_CHUNK_SIZE) {
            iters = 1;
        } else {
            remaining_size = this_sgl_size - SG_CHUNK_SIZE;
            iters = (remaining_size + (SG_CHUNK_SIZE - 1) - 1) / (SG_CHUNK_SIZE - 1) + 1;
        }

        for (iter_cnt = 0; iter_cnt < iters ; iter_cnt++) {
            this_size = iter_cnt == (iters - 1) ? this_sgl_size - freed : SG_CHUNK_SIZE;
            if (this_size == 0) {
                break;
            }

            sg = &curr_sgt[this_size - 1];
            if (sg_is_chain(sg)) {
                next_sgt = sg_chain_ptr(sg);
                kfree(curr_sgt);
                curr_sgt = next_sgt;
            } else if (sg_is_last(sg)) {
                kfree(curr_sgt);
                break;
            } else {
                pr_err("Invalid scatterlist: %d, %d, %d, %d\n", i, iter_cnt, this_size, ksm_error_table->rdma.sgt_cnt);
                pr_err("  ->%lx\n", sg->page_link);
                debug_stop();
            }

            freed += (this_size - 1);
        }
    }

    pr_info("Unregistered error table\n");
}

struct list_head* send_meta_desc(void) {
    int err;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return NULL;
    } 

    err = rdma_meta_send(ksm_cb);
    if (err) {
        pr_err("Failed to send meta desc\n");
    }

    pr_info("Sent metadata descriptor\n");
    return &ksm_cb->shadow_pt_list;
}

struct result_table* recv_offload_result(unsigned long* ksm_pages_scanned) {
    struct result_table* result;

    if (!ksm_cb) {
        pr_err("ksm_cb not initialized\n");
        return NULL;
    }

    result = rdma_result_recv(ksm_cb, ksm_pages_scanned);
    if (!result) {
        pr_err("Failed to receive result\n");
        return NULL;
    }

    pr_info("Received result table\n");

    return result;
}

void free_result_table(struct result_table* result) {
    int i, this_size, dma_size;
    for (i = 0; i < result->tables_cnt; i++) {
        this_size = (i == result->tables_cnt - 1) ? result->total_cnt - i * MAX_RESULT_TABLE_ENTRIES : MAX_RESULT_TABLE_ENTRIES;
        dma_size = sizeof(struct ksm_event_log) * this_size;
        ib_dma_unmap_single(ksm_cb->pd->device, result->unmap_addrs[i], dma_size, DMA_BIDIRECTIONAL);
        ksm_huge_dealloc(result->entry_tables[i]);
        // kfree(result->entry_tables[i]);
    }

    kfree(result);
}
