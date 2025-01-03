/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#ifndef __CSM_DP_MEM_H__
#define __CSM_DP_MEM_H__

#include <linux/types.h>
#include <linux/csm_dp_ioctl.h>

struct csm_dp_drv;

#define MAX_CSM_DP_MEMPOOL_SIZE ((long)1024 * 1024 * 1024 * 16)
#define CSM_DP_MEMPOOL_CLUSTER_SIZE (1024 * 1024 * 2)  /* must be > CSM_DP_MAX_DL_MSG_LEN */
#define CSM_DP_MEMPOOL_CLUSTER_SHIFT 21
#define CSM_DP_MEMPOOL_CLUSTER_MASK (CSM_DP_MEMPOOL_CLUSTER_SIZE - 1)
#define MAX_CSM_DP_MEMPOOL_CLUSTERS \
	(MAX_CSM_DP_MEMPOOL_SIZE / CSM_DP_MEMPOOL_CLUSTER_SIZE)


struct csm_dp_mem_loc {
	size_t size;		/* size of memory chunk */
	void *base;		/* virtual address of first cluster.
				 *  for ring with one cluster only
				 */
	unsigned int cookie;	/* mmap cookie */
	struct page *page[MAX_CSM_DP_MEMPOOL_CLUSTERS];
	unsigned int last_cl_order;
	unsigned int num_cluster; /* number of cluster, 1 for ring */
	char *cluster_kernel_addr[MAX_CSM_DP_MEMPOOL_CLUSTERS];

	/* for  CSM_DP_MMAP_TYPE_MEM */
	dma_addr_t cluster_dma_addr[MAX_CSM_DP_MEMPOOL_CLUSTERS];
	enum dma_data_direction direction;
	bool dma_mapped;
	unsigned int buf_per_cluster;
	unsigned long true_alloc_size;
};

struct csm_dp_mem {
	struct csm_dp_mem_loc loc;		/* location */
	unsigned int buf_cnt;		/* buffer counter */
	unsigned int buf_sz;		/* buffer size */
	unsigned int buf_headroom_sz;	/* headroom unused for now */
	unsigned int buf_overhead_sz;	/* buffer overhead size */
};

struct csm_dp_ring_opstats {
	atomic_t read_ok;
	atomic_t read_empty;

	atomic_t write_ok;
	atomic_t write_full;
};

struct csm_dp_ring {
	struct csm_dp_mem_loc loc;	/* location */
	unsigned int size;		/* size of ring(power of 2) */
	csm_dp_ring_index_t *cons_head;	/* consumer index header */
	csm_dp_ring_index_t *cons_tail;	/* consumer index tail */
	csm_dp_ring_index_t *prod_head;	/* producer index header */
	csm_dp_ring_index_t *prod_tail;	/* producer index tail */
	csm_dp_ring_element_t *element;	/* ring element */
	struct csm_dp_ring_opstats opstats;
};

struct csm_dp_mempool_stats {
	unsigned long buf_put;
	unsigned long buf_get;
	unsigned long invalid_buf_put;
	unsigned long invalid_buf_get;
	unsigned long buf_put_err;
	unsigned long buf_get_err;
};

#define CSM_DP_MEMPOOL_SIG 0xdeadbeef
#define CSM_DP_MEMPOOL_SIG_BAD 0xbeefdead

struct csm_dp_mempool {
	unsigned int signature;
	struct csm_dp_dev *dp_dev;
	enum csm_dp_mem_type type;
	struct csm_dp_ring ring;
	struct csm_dp_mem mem;
	atomic_t ref;
	atomic_t out_xmit;
	struct csm_dp_mempool_stats stats;
	spinlock_t lock;
	struct device *dev;	/* device for iommu ops */
};

struct csm_dp_mempool *csm_dp_mempool_alloc(
	struct csm_dp_dev *pdev,
	enum csm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt,
	bool may_dma_map);

void csm_dp_mempool_free(struct csm_dp_mempool *mempool);

int csm_dp_mempool_get_cfg(
	struct csm_dp_mempool *mempool,
	struct csm_dp_mempool_cfg *cfg);

int csm_dp_mempool_put_buf(struct csm_dp_mempool *mempool, void *vaddr);
void *csm_dp_mempool_get_buf(struct csm_dp_mempool *mempool,
		unsigned int *cluster, unsigned int *c_offset);

static inline bool csm_dp_mempool_hold(struct csm_dp_mempool *mempool)
{
	bool ret = false;

	if (!mempool)
		return ret;
	smp_mb__before_atomic();
	if (atomic_inc_not_zero(&mempool->ref))
		ret = true;
	smp_mb__after_atomic();
	return ret;
}

static inline void __csm_dp_mempool_hold(struct csm_dp_mempool *mempool)
{
	atomic_inc(&mempool->ref);
}

int csm_dp_ring_init(
	struct csm_dp_ring *ring,
	unsigned int ringsz,
	unsigned int mmap_cookie);

void csm_dp_ring_cleanup(struct csm_dp_ring *ring);

int csm_dp_ring_read(struct csm_dp_ring *ring, csm_dp_ring_element_data_t *element_data);
int csm_dp_ring_write(struct csm_dp_ring *ring, csm_dp_ring_element_data_t element_data);

bool csm_dp_ring_is_empty(struct csm_dp_ring *ring);

int csm_dp_ring_get_cfg(struct csm_dp_ring *ring, struct csm_dp_ring_cfg *cfg);

struct csm_dp_mempool *csm_dp_get_mempool(
	struct csm_dp_dev *pdev,
	struct csm_dp_buf_cntrl *buf_cntrl,
	unsigned int *cluster);
uint16_t csm_dp_mem_get_cluster(struct csm_dp_mem *mem, unsigned int buf_index);

int csm_dp_mempool_dma_map(
	struct device *dev,	/* device for iommu ops */
	struct csm_dp_mempool *mpool);

/* inline */
static __always_inline bool __ulong_in_range(
	unsigned long v,
	unsigned long start,
	unsigned long end)
{
	return (v >= start && v < end);
}

static __always_inline bool ulong_in_range(
	unsigned long v,
	unsigned long start,
	size_t size)
{
	return __ulong_in_range(v, start, start+size-1);
}

static __always_inline bool __vaddr_in_range(void *addr, void *start, void *end)
{
	return __ulong_in_range((unsigned long)addr,
				(unsigned long)start,
				(unsigned long)end);
}

static __always_inline bool vaddr_in_range(void *addr, void *start, size_t size)
{
	return __vaddr_in_range(addr, start, (char *)start + size - 1);
}

static __always_inline bool __vaddr_in_vma_range(
	void __user *vaddr_start,
	void __user *vaddr_end,
	struct vm_area_struct *vma)
{
	unsigned long start = (unsigned long)vaddr_start;
	unsigned long end = (unsigned long)vaddr_end;

	return (start >= vma->vm_start && end < vma->vm_end);
}

static __always_inline bool vaddr_in_vma_range(
	void __user *vaddr,
	size_t len,
	struct vm_area_struct *vma)
{
	return __vaddr_in_vma_range(vaddr,
				    (void __user *)((char *)vaddr + len - 1),
				    vma);
}

/* Find offset, this function is used with a memory ring type */
static __always_inline unsigned long vaddr_offset(void *addr, void *base)
{
	return (unsigned long)addr - (unsigned long)base;
}

/* Find mmap size */
static __always_inline unsigned long csm_dp_mem_loc_mmap_size(
	struct csm_dp_mem_loc *loc)
{
	return loc->size;
}

static inline unsigned int calc_ring_size(unsigned int elements)
{
	unsigned int size = 1, shift = 0;

	for (shift = 0; (shift < (sizeof(unsigned int) * 8 - 1)); shift++) {
		if (size >= elements)
			return size;
		size <<= 1;
	}
	return 0;
}

/* set buffer state, ptr: pointing to beginging of buffer user data */
static inline void csm_dp_set_buf_state(void *ptr, enum csm_dp_buf_state state)
{
	struct csm_dp_buf_cntrl *pf = (ptr - CSM_DP_L1_CACHE_BYTES);

	pf->state = state;
}

/* get true buffer size which includes size for user space and control  */
static inline uint32_t csm_dp_buf_true_size(struct csm_dp_mem *mem)
{
	return (mem->buf_sz + mem->buf_overhead_sz);
}

static inline void *csm_dp_mem_rec_addr(struct csm_dp_mem *mem,
					unsigned int rec)
{
	unsigned int cluster;
	unsigned int offset;

	if (rec >= mem->buf_cnt) {
		pr_err("%s: record %d exceed %d\n",
			__func__, rec,  mem->buf_cnt);
		return NULL;
	}
	cluster = rec / mem->loc.buf_per_cluster;
	offset = (rec % mem->loc.buf_per_cluster) * csm_dp_buf_true_size(mem);
	return (void *) mem->loc.cluster_kernel_addr[cluster] + offset;
}

static inline long csm_dp_mem_rec_offset(struct csm_dp_mem *mem,
					unsigned int rec)
{
	unsigned int cluster;
	unsigned int offset;

	if (rec >= mem->buf_cnt) {
		pr_err("%s: record %d exceed %d\n",
			__func__, rec,  mem->buf_cnt);
		return -EINVAL;
	}
	cluster = rec / mem->loc.buf_per_cluster;
	offset = (rec % mem->loc.buf_per_cluster) * csm_dp_buf_true_size(mem);
	return (long)cluster * CSM_DP_MEMPOOL_CLUSTER_SIZE + offset;
}

static inline void *csm_dp_mem_offset_addr(struct csm_dp_mem *mem,
	unsigned long offset, unsigned int *cluster, unsigned int *c_offset)
{
	if (offset >= mem->loc.size) {
		pr_err("%s: offset 0x%lx exceed 0x%lx\n",
				__func__, offset,  mem->loc.size);
		return NULL;
	}
	*cluster = offset >>  CSM_DP_MEMPOOL_CLUSTER_SHIFT;
	*c_offset = offset & CSM_DP_MEMPOOL_CLUSTER_MASK;
	return (void *) (mem->loc.cluster_kernel_addr[*cluster] + *c_offset);
}

static inline unsigned long csm_dp_get_mem_offset(void *addr,
	struct csm_dp_mem_loc *loc, unsigned int cl)
{

	unsigned long offset;

	offset = (char *) addr - loc->cluster_kernel_addr[cl];
	offset += (long)cl * CSM_DP_MEMPOOL_CLUSTER_SIZE;
	return offset;
}

#endif /* __CSM_DP_MEM_H__ */
