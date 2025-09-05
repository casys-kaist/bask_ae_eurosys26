#include <linux/scatterlist.h>
#include <linux/types.h>

#include <rdma/ib_verbs.h>

// #include "/usr/src/ofa_kernel/default/include/rdma/rdma_cm.h"
// #include "/usr/src/ofa_kernel/default/include/rdma/ib_verbs.h"

u64 mlx_ib_dma_map_page(struct ib_device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction direction) {
	return ib_dma_map_page(dev, page, offset, size, direction);
}

void mlx_ib_dma_unmap_page(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) {
	ib_dma_unmap_page(dev, addr, size, direction);
}

u64 mlx_ib_dma_map_single(struct ib_device *dev, void* cpu_addr, size_t size, enum dma_data_direction direction) {
	return ib_dma_map_single(dev, cpu_addr, size, direction);
}

void mlx_ib_dma_unmap_single(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) {
	ib_dma_unmap_single(dev, addr, size, direction);
}

struct ib_mr *mlx_ib_alloc_mr(struct ib_pd *pd, enum ib_mr_type mr_type, u32 max_num_sg) {
	return ib_alloc_mr(pd, mr_type, max_num_sg);
}

static int cnt = 0;

int mlx_ib_map_mr_sg(struct ib_mr *mr, struct scatterlist *sg, int sg_nents, unsigned int *sg_offset, unsigned int page_size) {
	int result;

	cnt += 1;
	result = ib_map_mr_sg(mr, sg, sg_nents, sg_offset, page_size);
	if (result <= 0 ) {
		pr_err("ib_map_mr_sg failed %d, cnt: %d, sg_nents: %d\n", result, cnt, sg_nents);
	}

	return result;
}

int mlx_ib_dereg_mr(struct ib_mr *mr) {
	return ib_dereg_mr(mr);
}

int mlx_ib_dma_map_sg(struct ib_device *dev,
				struct scatterlist *sg, int nents,
				enum dma_data_direction direction) {
	return ib_dma_map_sg(dev, sg, nents, direction);
}

void mlx_ib_dma_unmap_sg(struct ib_device *dev,
				  struct scatterlist *sg, int nents,
				  enum dma_data_direction direction) {
	ib_dma_unmap_sg(dev, sg, nents, direction);
}

void mlx_ib_dma_sync_single_for_cpu(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) {
	ib_dma_sync_single_for_cpu(dev, addr, size, direction);
}

void mlx_ib_dma_sync_single_for_device(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) {
	ib_dma_sync_single_for_device(dev, addr, size, direction);
}

EXPORT_SYMBOL(mlx_ib_dma_map_page);
EXPORT_SYMBOL(mlx_ib_dma_unmap_page);

EXPORT_SYMBOL(mlx_ib_dma_map_single);
EXPORT_SYMBOL(mlx_ib_dma_unmap_single);

EXPORT_SYMBOL(mlx_ib_alloc_mr);
EXPORT_SYMBOL(mlx_ib_dereg_mr);
EXPORT_SYMBOL(mlx_ib_map_mr_sg);

EXPORT_SYMBOL(mlx_ib_dma_map_sg);
EXPORT_SYMBOL(mlx_ib_dma_unmap_sg);

EXPORT_SYMBOL(mlx_ib_dma_sync_single_for_cpu);
EXPORT_SYMBOL(mlx_ib_dma_sync_single_for_device);