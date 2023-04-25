/* Copyright (c) 2019-2022, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/slab.h>
#include <linux/list.h>

#include "csm_dp.h"
#include "csm_dp_mem.h"

#define CSM_DP_MEMPOOL_RELEASE_DELAY	(HZ * 2)

static inline struct csm_dp_mempool *csm_dp_mem_to_mempool(
	struct csm_dp_mem *mem)
{
	struct csm_dp_mempool *mempool = container_of(mem,
						   struct csm_dp_mempool, mem);
	return mempool;
}

static inline void csm_dp_mem_loc_set(
	struct csm_dp_mem_loc *loc,
	size_t size,
	unsigned int mmap_cookie)
{
	loc->base = loc->cluster_kernel_addr[0];
	loc->size = size;
	loc->cookie = mmap_cookie;
	loc->dma_mapped = false;
}

static inline int __alloc_ring(
	size_t size,
	unsigned int mmap_cookie,
	struct csm_dp_mem_loc *loc)
{
	unsigned int order;
	struct page *page;

	order = get_order(size);
	if (order > get_order(CSM_DP_MEMPOOL_CLUSTER_SIZE)) {
		CSM_DP_ERROR("%s: failed to allocate memory. Too Big %ld\n",
				__func__, size);
		return -ENOMEM;
	}
	page = alloc_pages(GFP_KERNEL, order);
	loc->last_cl_order = order;
	loc->num_cluster = 1;
	if (page) {
		loc->page[0] = page;
		loc->cluster_kernel_addr[0] = page_address(page);
		csm_dp_mem_loc_set(loc, size, mmap_cookie);
		return 0;
	}
	return -ENOMEM;
}

static inline void __free_ring(struct csm_dp_mem_loc *loc)
{
	if (loc && loc->page[0]) {
		__free_pages(loc->page[0], loc->last_cl_order);
		memset(loc, 0, sizeof(*loc));
	}
}

static inline int __buf_mem_alloc(size_t size,
			      unsigned int mmap_cookie,
			      struct csm_dp_mem_loc *loc)
{
	unsigned int order;
	struct page *page;
	int i;
	unsigned long rem = size;
	unsigned long len;

	for (i = 0; i < loc->num_cluster; i++) {
		if (i == loc->num_cluster - 1)
			len = rem;
		else
			len = CSM_DP_MEMPOOL_CLUSTER_SIZE;
		order = get_order(len);
		if (i == loc->num_cluster - 1)
			loc->last_cl_order = order;
		page = alloc_pages(GFP_KERNEL, order);
		if (!page)
			goto error;
		loc->page[i] = page;
		loc->cluster_kernel_addr[i] = page_address(page);
		rem -= len;
	}
	csm_dp_mem_loc_set(loc, size, mmap_cookie);
	return 0;
error:
	for (i = 0; i < loc->num_cluster; i++) {
		if (loc->page[i]) {
			if (i == loc->num_cluster - 1)
				order = loc->last_cl_order;
			else
				order = get_order(CSM_DP_MEMPOOL_CLUSTER_SIZE);
			__free_pages(loc->page[i], order);
			loc->page[i] = NULL;
		}
	}
	loc->num_cluster = 0;
	return -ENOMEM;
}

static inline void __buf_mem_free(struct csm_dp_mem_loc *loc)
{
	int i;
	unsigned int order = get_order(CSM_DP_MEMPOOL_CLUSTER_SIZE);

	if (loc) {
		for (i = 0; i < loc->num_cluster; i++) {
			if (loc->page[i]) {
				if (i == loc->num_cluster - 1)
					order = loc->last_cl_order;
				__free_pages(loc->page[i], order);
			}
		}
		memset(loc, 0, sizeof(*loc));
	}
}

/*
 * get the MHI controller dev - needed for dma operations. Control and data
 * channels refer to same MHI controller dev.
 */
static struct device *get_mhi_cntrl_dev(struct csm_dp_dev *pdev)
{
	atomic_inc(&pdev->mhi_control_dev.mhi_dev_refcnt);
	atomic_inc(&pdev->mhi_data_dev.mhi_dev_refcnt);

	if (csm_dp_mhi_is_ready(&pdev->mhi_control_dev))
		return pdev->mhi_control_dev.mhi_dev->mhi_cntrl->cntrl_dev;
	else if (csm_dp_mhi_is_ready(&pdev->mhi_data_dev))
		return pdev->mhi_data_dev.mhi_dev->mhi_cntrl->cntrl_dev;

	atomic_dec(&pdev->mhi_control_dev.mhi_dev_refcnt);
	atomic_dec(&pdev->mhi_data_dev.mhi_dev_refcnt);

	return NULL;
}

static void put_mhi_cntrl_dev(struct csm_dp_dev *pdev)
{
	atomic_dec(&pdev->mhi_control_dev.mhi_dev_refcnt);
	atomic_dec(&pdev->mhi_data_dev.mhi_dev_refcnt);
}

int csm_dp_ring_init(
	struct csm_dp_ring *ring,
	unsigned int ringsz,
	unsigned int mmap_cookie)
{
	unsigned int allocsz = ringsz * sizeof(*ring->element);
	char *aligned_ptr;
	csm_dp_ring_element_t *elem_p;

	/* cons and prod index space, aligned to cache line */
	allocsz += 4 * cache_line_size();
	allocsz = ALIGN(allocsz, cache_line_size());

	if (__alloc_ring(allocsz, mmap_cookie, &ring->loc)) {
		CSM_DP_ERROR("%s: failed to allocate ring memory\n", __func__);
		return -ENOMEM;
	}

	aligned_ptr = (char *)ALIGN((unsigned long)ring->loc.base,
				    cache_line_size());
	ring->prod_head = (csm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->prod_tail = (csm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->cons_head = (csm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->cons_tail = (csm_dp_ring_index_t *)aligned_ptr;
	aligned_ptr += cache_line_size();
	ring->element = elem_p = (csm_dp_ring_element_t *)aligned_ptr;
	ring->size = ringsz;
	*ring->prod_head = *ring->prod_tail = *ring->cons_head =
						*ring->cons_tail = 0;
	return 0;
}

void csm_dp_ring_cleanup(struct csm_dp_ring *ring)
{
	if (ring) {
		__free_ring(&ring->loc);
		memset(ring, 0, sizeof(*ring));
	}
}

int csm_dp_ring_get_cfg(struct csm_dp_ring *ring, struct csm_dp_ring_cfg *cfg)
{
	if (unlikely(ring == NULL || cfg == NULL))
		return -EINVAL;
	cfg->mmap.length = ring->loc.size;
	cfg->mmap.cookie = ring->loc.cookie;

	cfg->size = ring->size;
	cfg->prod_head_off = vaddr_offset((void *)ring->prod_head,
							ring->loc.base);
	cfg->prod_tail_off = vaddr_offset((void *)ring->prod_tail,
							ring->loc.base);
	cfg->cons_head_off = vaddr_offset((void *)ring->cons_head,
							ring->loc.base);
	cfg->cons_tail_off = vaddr_offset((void *)ring->cons_tail,
							ring->loc.base);
	cfg->ringbuf_off = vaddr_offset((void *)ring->element,
							ring->loc.base);
	return 0;
}

/* Read from ring */
int csm_dp_ring_read(
	struct csm_dp_ring *ring,
	csm_dp_ring_element_data_t *element_ptr)
{
	register csm_dp_ring_index_t cons_head, cons_next;
	register csm_dp_ring_index_t prod_tail, mask;
	csm_dp_ring_element_data_t data;

	if (unlikely(ring == NULL))
		return -EINVAL;

	mask = ring->size - 1;

again:
	/* test to see if the ring is empty.
	 * If not, advance cons_head and read the data
	 */
	cons_head = *ring->cons_head;
	prod_tail = *ring->prod_tail;
	rmb();	/* Get current cons_head and prod_tail */
	if ((cons_head & mask) == (prod_tail & mask)) {
		rmb();
		if (cons_head == *ring->cons_head && prod_tail == *ring->prod_tail) {
			atomic_inc(&ring->opstats.read_empty);
			return -EAGAIN;
		}
		goto again;
	}
	cons_next = cons_head + 1;
	if (atomic_cmpxchg((atomic_t *)ring->cons_head,
			   cons_head,
			   cons_next) != cons_head)
		goto again;

	/* Read the ring */
	data = ring->element[(cons_head & mask)].element_data;
	rmb();	/* Get current element */

	if (element_ptr)
		*element_ptr = data;

	atomic_inc(&ring->opstats.read_ok);

	/* Potential two consumer is updating */
	while(atomic_cmpxchg((atomic_t *)ring->cons_tail,
			   cons_head,
			   cons_next) != cons_head);

	return 0;
}

/* Write to ring */
int csm_dp_ring_write(struct csm_dp_ring *ring, csm_dp_ring_element_data_t data)
{
	register csm_dp_ring_index_t prod_head, prod_next;
	register csm_dp_ring_index_t cons_tail, mask;

	if (unlikely(ring == NULL))
		return -EINVAL;

	mask = ring->size - 1;

again:
	/* test to see if the ring is full.
	 * If not, advance prod_head and write the data
	 */
	prod_head = *ring->prod_head;
	cons_tail = *ring->cons_tail;
	rmb();	/* Get current prod_head and cons_tail */
	prod_next = prod_head + 1;
	if ((prod_next & mask) == (cons_tail & mask)) {
		rmb();
		if (prod_head == *ring->prod_head && cons_tail == *ring->cons_tail) {
			atomic_inc(&ring->opstats.write_full);
			return -EAGAIN;
		}
		goto again;
	}
	if (atomic_cmpxchg((atomic_t *)ring->prod_head,
			   prod_head,
			   prod_next) != prod_head)
		goto again;

	ring->element[(prod_head & mask)].element_data = data;
	wmb();	/* Ensure element is written */

	atomic_inc(&ring->opstats.write_ok);

	/* Potential two producer is updating */
	while(atomic_cmpxchg((atomic_t *)ring->prod_tail,
			   prod_head,
			   prod_next) != prod_head);

	return 0;
}

bool csm_dp_ring_is_empty(struct csm_dp_ring *ring)
{
	csm_dp_ring_index_t prod_tail, cons_tail;

	prod_tail = *ring->prod_tail;
	cons_tail = *ring->cons_tail;
	if (prod_tail == cons_tail)
		return true;
	return false;
}

static int csm_dp_mem_init(
	struct csm_dp_mem *mem,
	unsigned int bufcnt,
	unsigned int bufsz,
	unsigned int cookie)
{
	unsigned int num_buf_cl; /* number of buffers per cluster */
	unsigned int num_cl; /* number of clusters */
	unsigned long size;
	unsigned long rem_size = 0;
	unsigned int rem_buf;

	mem->buf_cnt = bufcnt;
	mem->buf_sz = ALIGN(bufsz, cache_line_size());
	mem->buf_overhead_sz = CSM_DP_L1_CACHE_BYTES;

	if (csm_dp_buf_true_size(mem) > CSM_DP_MEMPOOL_CLUSTER_SIZE) {
		CSM_DP_ERROR("%s: buf_true_size too big %d (CLUSTER_SIZE %d)\n",
			__func__, csm_dp_buf_true_size(mem), CSM_DP_MEMPOOL_CLUSTER_SIZE);
		return -ENOMEM;
	}

	num_buf_cl = CSM_DP_MEMPOOL_CLUSTER_SIZE / csm_dp_buf_true_size(mem);
	num_cl = bufcnt / num_buf_cl;
	size = (long)num_cl * CSM_DP_MEMPOOL_CLUSTER_SIZE;
	rem_buf = bufcnt % num_buf_cl;
	if (rem_buf) {
		num_cl++;
		rem_size = csm_dp_buf_true_size(mem) * rem_buf;
	}
	if (num_cl > MAX_CSM_DP_MEMPOOL_CLUSTERS) {
		CSM_DP_ERROR("%s: mempool size too big. num_cl %d\n", __func__, num_cl);
		return -ENOMEM;
	}
	if (ULONG_MAX / CSM_DP_MEMPOOL_CLUSTER_SIZE < num_cl) {
		CSM_DP_ERROR("%s: mempool size too big. num_cl %d CLUSTER_SIZE %d\n",
			__func__, num_cl, CSM_DP_MEMPOOL_CLUSTER_SIZE);
		return -ENOMEM;
	}
	mem->loc.num_cluster = num_cl;
	mem->loc.buf_per_cluster = num_buf_cl;
	size += rem_size;
	if (__buf_mem_alloc(size, cookie, &mem->loc)) {
		CSM_DP_ERROR("%s: failed to allocate DMA memory\n", __func__);
		return -ENOMEM;
	}
	return 0;
}

static void csm_dp_mem_cleanup(struct csm_dp_mem *mem)
{
	struct csm_dp_mempool *mempool = csm_dp_mem_to_mempool(mem);
	struct csm_dp_dev *pdev = mempool->dp_dev;
	int i;
	unsigned int size;
	struct device *mhi_cntrl_dev = get_mhi_cntrl_dev(pdev);

	if (!mhi_cntrl_dev)
		return;

	spin_lock(&mempool->lock);
	if (mem->loc.dma_mapped) {
		size = mem->loc.size;
		for (i = 0; i < mem->loc.num_cluster; i++) {
			if (i ==  mem->loc.num_cluster - 1) {
				dma_unmap_single(
					mhi_cntrl_dev,
					mem->loc.cluster_dma_addr[i],
					size,
					mem->loc.direction);
			} else {
				dma_unmap_single(
					mhi_cntrl_dev,
					mem->loc.cluster_dma_addr[i],
					CSM_DP_MEMPOOL_CLUSTER_SIZE,
					mem->loc.direction);
				size -= CSM_DP_MEMPOOL_CLUSTER_SIZE;
			}
		}
	}
	mem->loc.dma_mapped = false;
	spin_unlock(&mempool->lock);

	__buf_mem_free(&mem->loc);
	memset(mem, 0, sizeof(*mem));

	put_mhi_cntrl_dev(pdev);
}

static int csm_dp_mem_get_cfg(
	struct csm_dp_mem *mem,
	struct csm_dp_mem_cfg *cfg)
{
	cfg->mmap.length = mem->loc.size;
	cfg->mmap.cookie = mem->loc.cookie;

	cfg->buf_sz = mem->buf_sz;
	cfg->buf_cnt = mem->buf_cnt;
	cfg->buf_overhead_sz = CSM_DP_L1_CACHE_BYTES;
	cfg->cluster_size = CSM_DP_MEMPOOL_CLUSTER_SIZE;
	cfg->num_cluster = mem->loc.num_cluster;
	cfg->buf_per_cluster = mem->loc.buf_per_cluster;

	CSM_DP_DEBUG("%s: buf_sz %d buf_cnt %d buf_overhead_sz %d cluster_size %d num_cluster %d buf_per_cluster %d\n",
		__func__, cfg->buf_sz, cfg->buf_cnt, cfg->buf_overhead_sz,
		cfg->cluster_size, cfg->num_cluster, cfg->buf_per_cluster);

	return 0;
}

static void csm_dp_mempool_init(struct csm_dp_mempool *mempool)
{
	struct csm_dp_mem *mem = &mempool->mem;
	struct csm_dp_ring *ring = &mempool->ring;
	csm_dp_ring_element_data_t element_data;
	int i, j;
	struct csm_dp_buf_cntrl *p;
	unsigned int cl_buf_cnt;
	unsigned int buf_index = 0;
	char *cl_start;

	if (!csm_dp_mem_type_is_valid(mempool->type))
		return;

	for (j = 0; j < mem->loc.num_cluster; j++) {
		element_data = (long)j * CSM_DP_MEMPOOL_CLUSTER_SIZE;
		cl_start = mem->loc.cluster_kernel_addr[j];
		if (j == mem->loc.num_cluster - 1)
			cl_buf_cnt = mem->buf_cnt -
				(mem->loc.buf_per_cluster * j);
		else
			cl_buf_cnt = mem->loc.buf_per_cluster;
		for (i = 0; i < cl_buf_cnt; i++) {
			p = (struct csm_dp_buf_cntrl *) (cl_start +
				(i * csm_dp_buf_true_size(mem)));
			p->signature = CSM_DP_BUFFER_SIG;
			p->fence = CSM_DP_BUFFER_FENCE_SIG;
			p->state = CSM_DP_BUF_STATE_KERNEL_FREE;
			p->mem_type = mempool->type;
			p->buf_index = buf_index;
			p->next_packet = NULL;
			if (!csm_dp_mem_type_is_ul(mempool->type))
				p->xmit_status = CSM_DP_XMIT_OK;
			/* pointing to start of user data */
			ring->element[buf_index].element_data =
				element_data + mem->buf_overhead_sz;
			element_data += csm_dp_buf_true_size(mem);
			buf_index++;
		}
	}
	*ring->cons_head = *ring->cons_tail = 0;
	*ring->prod_head = *ring->prod_tail = mem->buf_cnt - 1;
	wmb();	/* Ensure all the data are written */
}

static struct csm_dp_mempool *__csm_dp_mempool_alloc(
	struct csm_dp_dev *pdev,
	enum csm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt,
	unsigned int ring_sz,
	bool may_map)
{
	struct csm_dp_mempool *mempool;
	unsigned int cookie;

	mempool = kzalloc(sizeof(*mempool), GFP_KERNEL);
	if (IS_ERR_OR_NULL(mempool)) {
		CSM_DP_ERROR("%s: failed to allocate mempool\n", __func__);
		return NULL;
	}

	mempool->dp_dev = pdev;
	mempool->type = type;
	mempool->signature = CSM_DP_MEMPOOL_SIG;

	cookie = MMAP_COOKIE(type, CSM_DP_MMAP_TYPE_MEM);
	if (csm_dp_mem_init(&mempool->mem, buf_cnt, buf_sz, cookie)) {
		CSM_DP_ERROR("%s: failed to initialize memory\n", __func__);
		goto cleanup;
	}

	if (may_map) {
		struct device *dev = get_mhi_cntrl_dev(pdev);

		if (dev) {
			int ret = csm_dp_mempool_dma_map(dev, mempool);
			put_mhi_cntrl_dev(pdev);
			if (ret)
				goto cleanup_mem;
		}
	}
	cookie = MMAP_COOKIE(type, CSM_DP_MMAP_TYPE_RING);
	if (csm_dp_ring_init(&mempool->ring, ring_sz, cookie)) {
		CSM_DP_ERROR("%s: failed to initialize ring\n", __func__);
		goto cleanup_mem;
	}

	csm_dp_mempool_init(mempool);

	CSM_DP_DEBUG("%s: mempool is created, type=%u bufsz=%u bufcnt=%u\n",
		  __func__, type, buf_sz, buf_cnt);

	return mempool;

cleanup_mem:
	csm_dp_mem_cleanup(&mempool->mem);
cleanup:
	kfree(mempool);
	return NULL;
}

static void csm_dp_mempool_release(struct csm_dp_mempool *mempool)
{
	if (mempool) {
		enum csm_dp_mem_type type = mempool->type;

		mempool->signature = CSM_DP_MEMPOOL_SIG_BAD;
		wmb();
		csm_dp_mem_cleanup(&mempool->mem);
		csm_dp_ring_cleanup(&mempool->ring);
		kfree(mempool);
		CSM_DP_DEBUG("%s: mempool is freed, type=%u\n", __func__, type);
	}
}

#define CSM_DP_MEMPOOL_RELEASE_SLEEP 200 /* 200 ms */
void csm_dp_mempool_release_no_delay(struct csm_dp_mempool *mempool)
{
	unsigned int out_xmit, out_xmit1;

	if (!mempool)
		return;
	/* wait for all tx buffers done */

	out_xmit1 = atomic_read(&mempool->out_xmit);
	if (out_xmit1) {
		msleep(CSM_DP_MEMPOOL_RELEASE_SLEEP);
		out_xmit = atomic_read(&mempool->out_xmit);
		if (out_xmit)
			CSM_DP_ERROR(
				"mempool %p out_xmit changed from %d to %d after %d ms\n",
				mempool, out_xmit1, out_xmit, CSM_DP_MEMPOOL_RELEASE_SLEEP);
	}

	csm_dp_mempool_release(mempool);
}

int csm_dp_mempool_dma_map(
	struct device *dev,	/* device for iommu ops */
	struct csm_dp_mempool *mpool)
{
	enum dma_data_direction direction;
	int i, k;
	unsigned int size;
	struct csm_dp_mem_loc *loc;

	loc = &mpool->mem.loc;
	if (loc->dma_mapped)
		return 0;
	if (csm_dp_mem_type_is_ul(mpool->type))
		direction = DMA_BIDIRECTIONAL; /* rx, tx for rx loopback */
	else
		direction = DMA_TO_DEVICE;
	size = loc->size;
	for (i = 0; i < loc->num_cluster; i++) {
		if (i == loc->num_cluster - 1)
			loc->cluster_dma_addr[i] =
				dma_map_single(dev,
					loc->cluster_kernel_addr[i],
					size,
					direction);
		else {
			loc->cluster_dma_addr[i] =
				dma_map_single(dev,
					loc->cluster_kernel_addr[i],
					CSM_DP_MEMPOOL_CLUSTER_SIZE,
					direction);
			size -= CSM_DP_MEMPOOL_CLUSTER_SIZE;
		}
		if (dma_mapping_error(dev, loc->cluster_dma_addr[i]))
			goto error;
	}
	mpool->mem.loc.dma_mapped = true;
	mpool->mem.loc.direction = direction;
	return 0;
error:
	for (k = 0; k < i - 1; k++) {
		dma_unmap_single(dev, loc->cluster_dma_addr[k],
			CSM_DP_MEMPOOL_CLUSTER_SIZE,
			loc->direction);
	}
	return -ENOMEM;
}

struct csm_dp_mempool *csm_dp_mempool_alloc(
	struct csm_dp_dev *pdev,
	enum csm_dp_mem_type type,
	unsigned int buf_sz,
	unsigned int buf_cnt,
	bool may_dma_map)
{
	struct csm_dp_mempool *mempool;
	unsigned int ring_sz;

	if (unlikely(!buf_sz || !buf_cnt || !csm_dp_mem_type_is_valid(type)))
		return NULL;
	if (unlikely(((ULONG_MAX) / (buf_sz + CSM_DP_L1_CACHE_BYTES) < buf_cnt)))
		return NULL;
	if (buf_sz > CSM_DP_MAX_DL_MSG_LEN) {
		CSM_DP_ERROR("%s: mempool alloc buffer size %d exceeds limit %d\n",
			__func__, buf_sz, CSM_DP_MAX_DL_MSG_LEN);
		return NULL;
	}
	/* TODO: return in case of race with mhi_xxx_dev getting destroyed */

	ring_sz = calc_ring_size(buf_cnt);
	if (unlikely(!ring_sz))
		return NULL;

	mutex_lock(&pdev->mempool_lock);
	mempool = pdev->mempool[type];
	if (mempool) {
		if (!csm_dp_mem_type_is_ul(type)) {
			CSM_DP_ERROR(
				"%s: can't use existing mempool, type=%u\n",
				__func__, type);
			mempool = NULL;
			goto done;
		}
		goto mempool_hold;
	}

	mempool = __csm_dp_mempool_alloc(pdev, type, buf_sz,
					buf_cnt, ring_sz, may_dma_map);
	if (mempool == NULL)
		goto done;
	atomic_set(&mempool->ref, 1);
	atomic_set(&mempool->out_xmit, 0);
	spin_lock_init(&mempool->lock);
	pdev->mempool[type] = mempool;
	goto done;
mempool_hold:
	if (!csm_dp_mempool_hold(mempool))
		mempool = NULL;
done:
	mutex_unlock(&pdev->mempool_lock);
	return mempool;
}

void csm_dp_mempool_free(struct csm_dp_mempool *mempool)
{
	struct csm_dp_dev *pdev = mempool->dp_dev;

	if (!mempool)
		return;
	csm_dp_mempool_release_no_delay(mempool);
	pdev->mempool[mempool->type] = NULL;
	wmb();
	return;
}

int csm_dp_mempool_get_cfg(
	struct csm_dp_mempool *mempool,
	struct csm_dp_mempool_cfg *cfg)
{
	if (unlikely(mempool == NULL || cfg == NULL))
		return -EINVAL;

	cfg->type = mempool->type;
	csm_dp_mem_get_cfg(&mempool->mem, &cfg->mem);
	csm_dp_ring_get_cfg(&mempool->ring, &cfg->ring);
	return 0;
}

/* For CSM_DP_MEM_TYPE_UL_* pool only */
int csm_dp_mempool_put_buf(struct csm_dp_mempool *mempool, void *vaddr)
{
	struct csm_dp_mem *mem;
	unsigned long offset;
	int ret;
	struct csm_dp_buf_cntrl *p;
	unsigned int buf_index;
	unsigned int cluster;

	if (unlikely(mempool == NULL || vaddr == NULL))
		return -EINVAL;

	mem = &mempool->mem;
	p = (vaddr - mem->buf_overhead_sz);
	buf_index = p->buf_index;
	if (buf_index >= mem->buf_cnt) {
		CSM_DP_ERROR("%s: buf_index %d exceed %d\n", __func__,
				buf_index, mem->buf_cnt);
		return -EINVAL;
	}
	cluster = buf_index / mem->loc.buf_per_cluster;
	offset = ((long)cluster * CSM_DP_MEMPOOL_CLUSTER_SIZE) +
			(buf_index % mem->loc.buf_per_cluster) *
					csm_dp_buf_true_size(mem);
#ifdef CSM_DP_BUFFER_FENCING
	if (p->signature != CSM_DP_BUFFER_SIG) {
		mempool->stats.invalid_buf_put++;
		CSM_DP_ERROR("%s: mempool %llx type %d buffer at "
			"offset %ld corrupted, sig %x, exp %x\n",
			__func__, (u64) mempool, mempool->type,
			offset, p->signature, CSM_DP_BUFFER_SIG);
		return -EINVAL;
	}
	if (p->fence != CSM_DP_BUFFER_FENCE_SIG) {
		mempool->stats.invalid_buf_put++;
		CSM_DP_ERROR("%s: mempool %llx type %d buffer at "
			"offset %ld corrupted, fence %x, exp %x\n",
			__func__, (u64) mempool, mempool->type,
			offset, p->fence, CSM_DP_BUFFER_FENCE_SIG);
		CSM_DP_ERROR("%s: vaddr %llx  p %llx\n",
			__func__, (u64) vaddr, (u64) p);
		return -EINVAL;
	}
	p->state = CSM_DP_BUF_STATE_KERNEL_FREE;
#endif
	offset += sizeof(struct csm_dp_buf_cntrl);

	ret = csm_dp_ring_write(&mempool->ring, (csm_dp_ring_element_data_t)offset);
	if (ret)
		mempool->stats.buf_put_err++;
	else
		mempool->stats.buf_put++;

	return ret;
}

/* For CSM_DP_MEM_TYPE_UL_* pool only */
void *csm_dp_mempool_get_buf(struct csm_dp_mempool *mempool,
				unsigned int *cluster,  unsigned int *c_offset)
{
	struct csm_dp_mem *mem;
	csm_dp_ring_element_data_t val;
	void *ptr;
#ifdef CSM_DP_BUFFER_FENCING
	struct csm_dp_buf_cntrl *p;
#endif

	if (unlikely(mempool == NULL))
		return NULL;

	if (csm_dp_ring_read(&mempool->ring, &val)) {
		mempool->stats.buf_get_err++;
		return NULL;
	}

	mem = &mempool->mem;
	*cluster = val >> CSM_DP_MEMPOOL_CLUSTER_SHIFT;
	*c_offset = val & CSM_DP_MEMPOOL_CLUSTER_MASK;
	ptr = (char *)mempool->mem.loc.cluster_kernel_addr[*cluster] +
								*c_offset;
	if ((*c_offset - mem->buf_overhead_sz) % csm_dp_buf_true_size(mem)) {
		mempool->stats.invalid_buf_get++;
		CSM_DP_ERROR("%s: get unaligned buffer from ring, buf true size %d offset %d\n",
			__func__, csm_dp_buf_true_size(mem), *c_offset);
		return NULL;
	}
#ifdef CSM_DP_BUFFER_FENCING
	p = ptr - mem->buf_overhead_sz;
	if (p->signature !=  CSM_DP_BUFFER_SIG) {
		mempool->stats.invalid_buf_get++;
		CSM_DP_ERROR("%s: mempool type %d buffer "
			"at %d corrupted, %x, exp %x\n",
			__func__, *c_offset, mempool->type,
			p->signature, CSM_DP_BUFFER_SIG);
		return NULL;
	}
	if (p->fence !=  CSM_DP_BUFFER_FENCE_SIG) {
		mempool->stats.invalid_buf_get++;
		CSM_DP_ERROR("%s: mempool type %d "
			"buffer at %d corrupted, fence %x, exp %x\n",
			__func__, *c_offset, mempool->type,
			p->fence, CSM_DP_BUFFER_FENCE_SIG);
		return NULL;
	}
#endif
	mempool->stats.buf_get++;
	return ptr;
}

struct csm_dp_mempool *csm_dp_get_mempool(
	struct csm_dp_dev *pdev,
	struct csm_dp_buf_cntrl *buf_cntrl,
	unsigned int *cluster)
{
	struct csm_dp_mempool *mempool;

	if (!csm_dp_mem_type_is_valid(buf_cntrl->mem_type))
		return NULL;

	mempool = pdev->mempool[buf_cntrl->mem_type];
	if (!mempool)
		return NULL;

	if (buf_cntrl->buf_index >= U16_MAX * mempool->mem.loc.buf_per_cluster)
		return NULL;

	if (cluster)
		*cluster = csm_dp_mem_get_cluster(&mempool->mem, buf_cntrl->buf_index);

	return mempool;
}

uint16_t csm_dp_mem_get_cluster(struct csm_dp_mem *mem, unsigned int buf_index)
{
	CSM_DP_ASSERT(buf_index >= U16_MAX * mem->loc.buf_per_cluster, "invalid buf_index");
	return buf_index / mem->loc.buf_per_cluster;
}
