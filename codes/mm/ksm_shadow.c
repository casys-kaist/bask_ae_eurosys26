#include "ksm.h"
#include "linux/scatterlist.h"
#include "linux/xarray.h"

#define GROWTH_FACTOR 2

struct shadow_mm* create_empty(struct ksm_cb* cb) {
    int capacity;
    struct shadow_mm* shadow_page_table;
    if (!cb) {
        pr_err("ksm_cb not initialized\n");
        return NULL;
    }

    capacity = PAGE_SIZE / sizeof(struct shadow_pte);
    
    shadow_page_table = (struct shadow_mm*) kmalloc(sizeof(struct shadow_mm), GFP_KERNEL);
    if (!shadow_page_table) {
        return NULL;
    }

    shadow_page_table->pt_map.va_arrays = kmalloc(sizeof(struct shadow_pte*), GFP_KERNEL);
    if (!shadow_page_table->pt_map.va_arrays) {
        kfree(shadow_page_table);
        return NULL;
    }
    
    shadow_page_table->pt_map.va_arrays[0] = kmalloc(capacity * sizeof(struct shadow_pte), GFP_KERNEL);
    if (!shadow_page_table->pt_map.va_arrays[0]) {
        kfree(shadow_page_table);
        return NULL;
    }

    xa_init(&shadow_page_table->pt_map.page_xa);

    shadow_page_table->pt_map.va_array_cnt = 1;
    shadow_page_table->pt_map.cnt = 0;
    shadow_page_table->pt_map.capacity = capacity;
    shadow_page_table->connected_cb = cb;

    return shadow_page_table;
}

void grow_shadow_page_table(struct shadow_mm* shadow_page_table) {
    struct shadow_pte* new_va_array;

    size_t curr_capacity = shadow_page_table->pt_map.capacity - MAX_CAPACITY_PER_TABLE * (shadow_page_table->pt_map.va_array_cnt - 1); 
    size_t new_capacity = curr_capacity * GROWTH_FACTOR;

    if (new_capacity > MAX_CAPACITY_PER_TABLE) {
        new_va_array = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!new_va_array) {
            return;
        }
        shadow_page_table->pt_map.va_arrays = krealloc(shadow_page_table->pt_map.va_arrays, (shadow_page_table->pt_map.va_array_cnt + 1) * sizeof(struct shadow_pte*), GFP_KERNEL);

        shadow_page_table->pt_map.va_arrays[shadow_page_table->pt_map.va_array_cnt++] = new_va_array;
        shadow_page_table->pt_map.capacity += PAGE_SIZE / sizeof(struct shadow_pte);
        return;
    } else {
        new_va_array = krealloc(shadow_page_table->pt_map.va_arrays[shadow_page_table->pt_map.va_array_cnt - 1], new_capacity * sizeof(struct shadow_pte), GFP_KERNEL);
        if (!new_va_array) {
            pr_err("Faild to realloc va_array: size %ld\n", new_capacity * sizeof(struct shadow_pte));
            return;
        }
        shadow_page_table->pt_map.va_arrays[shadow_page_table->pt_map.va_array_cnt - 1] = new_va_array;
        shadow_page_table->pt_map.capacity += (new_capacity - curr_capacity);
    }
}

int insert_entry_to_shadow_mm(struct shadow_mm* shadow_mm, unsigned long va, unsigned long kpfn, void* rmap_item) {
    size_t cnt = shadow_mm->pt_map.cnt;
    size_t array_idx, idx;
    int err;
    
    if (cnt >= shadow_mm->pt_map.capacity) {
        grow_shadow_page_table(shadow_mm);
    }

    if (cnt >= shadow_mm->pt_map.capacity) {
        pr_err("Failed to grow shadow page table\n");
        return -1;
    }

    array_idx = cnt / MAX_CAPACITY_PER_TABLE;
    idx = cnt % MAX_CAPACITY_PER_TABLE;

    shadow_mm->pt_map.va_arrays[array_idx][idx].va = va;
    shadow_mm->pt_map.va_arrays[array_idx][idx].pfn = kpfn;

    err = xa_insert(&shadow_mm->pt_map.page_xa, va >> PAGE_SHIFT, rmap_item, GFP_KERNEL);
    if (err) {
        pr_err("Failed to insert into xa\n");
        return -1;
    }

    shadow_mm->pt_map.cnt++;
    return 0;
}

void free_shadow_mm(struct shadow_mm* shadow_mm, bool disconnected, int curr_iteration) {
    int i = 0, j = 0;
    int iters;
    int this_size = 0, freed = 0, this_sgl_size = 0, remaining_size = 0;
    struct scatterlist *sgt, *next_sgt, *sg;

    struct ksm_rmap_item* entry;
    unsigned long index;

    if (disconnected)
        pr_info("[BASK] Disconnect detected. We need to clean up shadow mm cleanly");

    xa_for_each(&shadow_mm->pt_map.page_xa, index, entry) {
        if (!entry->page) {
            pr_err("Page is NULL at va %lx\n", entry->address);
        } else {
            if (!PageKsm(entry->page)) {
                put_page(entry->page);
            }
            if (disconnected) {
                entry->page = 0;
                entry->age = curr_iteration - 1;
            }
        }
    }

    xa_destroy(&shadow_mm->pt_map.page_xa);

    for (j = 0; j < shadow_mm->pages_sgt_cnt; j++) {
        sgt = shadow_mm->pages_sgt[j];
        freed = 0;

        this_sgl_size = j == (shadow_mm->pages_sgt_cnt - 1) ? shadow_mm->pt_map.cnt - j * MAX_PAGES_IN_SGL : MAX_PAGES_IN_SGL;
        
        if (this_sgl_size <= SG_CHUNK_SIZE) {
            iters = 1;
        } else {
            remaining_size = this_sgl_size - SG_CHUNK_SIZE;
            iters = (remaining_size + (SG_CHUNK_SIZE - 1) - 1) / (SG_CHUNK_SIZE - 1) + 1;
        }

        for (i = 0; i < iters ; i++) {
            this_size = i == (iters - 1) ? this_sgl_size - freed : SG_CHUNK_SIZE;
            if (this_size == 0) {
                break;
            }

            sg = &sgt[this_size - 1];
            if (sg_is_chain(sg)) {
                next_sgt = sg_chain_ptr(sg);
                kfree(sgt);
                sgt = next_sgt;
            } else if (sg_is_last(sg)) {
                kfree(sgt);
                break;
            } else {
                pr_err("Invalid sg: %d %d %d %d\n", j, this_sgl_size, i, this_size);
            }
            freed += (this_size - 1);
        }
    }

    for (i = 0; i < shadow_mm->pt_map.va_array_cnt; i++) {
        kfree(shadow_mm->pt_map.va_arrays[i]);
    }
    kfree(shadow_mm->pt_map.va_arrays);
    kfree(shadow_mm);
}

unsigned long get_va_at(struct shadow_mm* shadow_mm, int idx) {
    int array_idx = idx / MAX_CAPACITY_PER_TABLE;
    int idx_in_array = idx - array_idx * MAX_CAPACITY_PER_TABLE;

    BUG_ON(idx >= shadow_mm->pt_map.cnt);

    return shadow_mm->pt_map.va_arrays[array_idx][idx_in_array].va;
}

struct shadow_mm* get_shadow_mm(struct list_head* shadow_pt_list, int mm_id) {
    struct shadow_mm* entry;
    list_for_each_entry(entry, shadow_pt_list, list) {
        if (entry->mm_id == mm_id) {
            return entry;
        }
    }
    return NULL;
}

struct ksm_rmap_item* shadow_mm_lookup(struct shadow_mm* shadow_mm, unsigned long va) {
    return xa_load(&shadow_mm->pt_map.page_xa, va >> PAGE_SHIFT);
}

/*===================================================================================*/

struct error_table* create_error_table(void) {
    struct error_table* error_table = kmalloc(sizeof(struct error_table), GFP_KERNEL);
    if (!error_table) {
        return NULL;
    }

    error_table->entry_tables = kmalloc(sizeof(struct ksm_event_log*), GFP_KERNEL);
    if (!error_table->entry_tables) {
        kfree(error_table);
        return NULL;
    }
    error_table->entry_tables[0] = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!error_table->entry_tables[0]) {
        kfree(error_table->entry_tables);
        kfree(error_table);
        return NULL;
    }

    error_table->tables_cnt = 1;
    error_table->total_cnt = 0;
    error_table->capacity = PAGE_SIZE / sizeof(struct ksm_event_log);

    return error_table;
}

void clear_error_table(void) {
    struct error_table* tbl = ksm_error_table;
    if (tbl->registered == tbl->total_cnt) {
        tbl->registered = 0;
        tbl->total_cnt = 0;
    } else {
        pr_err("Error table not fully registered: %d vs :%d\n", tbl->registered, tbl->total_cnt);
        debug_stop();
    }
}

void free_error_table(struct error_table* error_table) {
    int i;
    for (i = 0; i < error_table->tables_cnt; i++) {
        kfree(error_table->entry_tables[i]);
    }
    kfree(error_table->entry_tables);
    kfree(error_table);
}

void grow_error_table(struct error_table* error_table) {
    struct ksm_event_log* new_entry_table;

    size_t curr_capacity = error_table->capacity - MAX_RESULT_TABLE_ENTRIES * (error_table->tables_cnt - 1);
    size_t new_capacity = curr_capacity * GROWTH_FACTOR;

    if (new_capacity > MAX_RESULT_TABLE_ENTRIES) {
        new_entry_table = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!new_entry_table) {
            pr_err("Failed to allocate new entry table\n");
            return;
        }
        
        error_table->entry_tables = krealloc(error_table->entry_tables, (error_table->tables_cnt + 1) * sizeof(struct ksm_event_log*), GFP_KERNEL);
        if (!error_table->entry_tables) {
            pr_err("Failed to realloc entry_tables\n");
            return;
        }

        error_table->entry_tables[error_table->tables_cnt++] = new_entry_table;
        error_table->capacity += PAGE_SIZE / sizeof(struct ksm_event_log);
        return;
    } else {
        new_entry_table = krealloc(error_table->entry_tables[error_table->tables_cnt - 1], new_capacity * sizeof(struct ksm_event_log), GFP_KERNEL);
        if (!new_entry_table) {
            pr_err("Faild to realloc entry_tables: size %ld\n", new_capacity * sizeof(struct ksm_event_log));
            return;
        }
        error_table->entry_tables[error_table->tables_cnt - 1] = new_entry_table;
        error_table->capacity += (new_capacity - curr_capacity);
    }
}

int insert_error_log(struct error_table* error_table, enum event_tag tag, struct ksm_event_log* log) {
    size_t cnt = error_table->total_cnt;
    size_t array_idx, idx;

    if (!error_table) {
        return -1;
    }

    if (cnt >= error_table->capacity) {
        grow_error_table(error_table);
    }

    if (cnt >= error_table->capacity) {
        pr_err("Failed to grow error table: cnt %lu vs capacity %d\n", cnt, error_table->capacity);
        return -1;
    }

    array_idx = cnt / MAX_RESULT_TABLE_ENTRIES;
    idx = cnt % MAX_RESULT_TABLE_ENTRIES;

    error_table->entry_tables[array_idx][idx] = *log;
    error_table->entry_tables[array_idx][idx].type = tag;
    error_table->total_cnt++;

    DEBUG_LOG("Inserted error log at %d for %d \n", error_table->total_cnt, tag);

    return 0;
}