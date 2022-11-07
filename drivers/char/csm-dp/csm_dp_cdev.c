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
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/dma-mapping.h>

#include "csm_dp.h"

static inline bool is_rxqueue_mmap_cookie(unsigned int cookie)
{
	unsigned int type, mmap_type;

	mmap_type = MMAP_COOKIE_TO_TYPE(cookie);
	type = MMAP_COOKIE_TO_MEM_TYPE(cookie);
	if (mmap_type == CSM_DP_MMAP_TYPE_RING && type >= CSM_DP_MEM_TYPE_LAST)
		return true;
	return false;
}

static inline void *usr_to_kern_vaddr(
	struct csm_dp_mempool_vma *mempool_vma,
	void __user *addr,
	unsigned int *cluster,
	unsigned int *c_offset)
{
	struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;
	unsigned long offset = (unsigned long)addr -
		mempool_vma->vma[CSM_DP_MMAP_TYPE_MEM]->vm_start;

	*cluster = offset >> CSM_DP_MEMPOOL_CLUSTER_SHIFT;
	*c_offset = offset & CSM_DP_MEMPOOL_CLUSTER_MASK;

	return ((char *)mempool->mem.loc.cluster_kernel_addr[*cluster] +
								*c_offset);
}

static inline struct csm_dp_rxqueue *rxqueue_vma_to_rxqueue(
	struct csm_dp_rxqueue_vma *rxq_vma)
{
	struct csm_dp_cdev *cdev = container_of(rxq_vma,
						struct csm_dp_cdev,
						rxqueue_vma[rxq_vma->type]);
	return &cdev->pdev->rxq[rxq_vma->type];
}

static void __cdev_init_mempool_vma(struct csm_dp_cdev *cdev)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	int type;

	memset(cdev->mempool_vma, 0, sizeof(cdev->mempool_vma));

	for (type = 0; type < CSM_DP_MEM_TYPE_LAST; type++)
		cdev->mempool_vma[type].pp_mempool = &pdev->mempool[type];
}

static struct csm_dp_mempool_vma *find_mempool_vma(
	struct csm_dp_cdev *cdev,
	void __user *addr,
	unsigned int len)
{
	struct csm_dp_mempool_vma *mempool_vma = NULL;
	struct vm_area_struct *vma;
	unsigned int mem_type;

	for (mem_type = 0; mem_type < CSM_DP_MEM_TYPE_LAST; mem_type++) {
		mempool_vma = &cdev->mempool_vma[mem_type];
		vma = mempool_vma->vma[CSM_DP_MMAP_TYPE_MEM];
		if (vma && vaddr_in_vma_range(addr, len, vma))
			return mempool_vma;
	}
	return NULL;
}

static int __cdev_tx(
	struct csm_dp_cdev *cdev,
	enum csm_dp_channel ch,
	struct iovec __user *uiov,
	unsigned int iov_nr,
	unsigned int ioctl_flags,
	bool sg)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_mempool_vma *mempool_vma;
	struct iovec iov[CSM_DP_MAX_IOV_SIZE];
	dma_addr_t dma_addr[CSM_DP_MAX_IOV_SIZE];
	unsigned int n;
	int ret;
	unsigned int flag = 0;
	struct csm_dp_mempool *mempool;
#ifdef CSM_DP_BUFFER_FENCING
	uint32_t iov_off_array[CSM_DP_MAX_IOV_SIZE];
#endif
	unsigned int c_offset;
	unsigned int cluster;
	struct csm_dp_buf_cntrl *buf_cntrl = NULL, *prev_buf_cntrl = NULL;
	unsigned long b_backtrack;

	CSM_DP_DEBUG("%s: iov_nr=%u\n", __func__, iov_nr);
	if (iov_nr > CSM_DP_MAX_IOV_SIZE)
		return  -E2BIG;

	if (copy_from_user(iov, (void __user *)uiov,
				   sizeof(struct iovec) * iov_nr))
		return -EFAULT;

	for (n = 0; n < iov_nr; n++) {
		mempool_vma = find_mempool_vma(cdev,
					       iov[n].iov_base,
					       iov[n].iov_len);
		if (mempool_vma == NULL) {
			CSM_DP_DEBUG(
				"%s: cannot find mempool addr=%p, len=%lu\n",
				__func__, iov[n].iov_base, iov[n].iov_len);
			return -EINVAL;
		}
		mempool = *mempool_vma->pp_mempool;

		/* User passes in the pointer to message payload */
		iov[n].iov_base = usr_to_kern_vaddr(
					mempool_vma,
					iov[n].iov_base,
					&cluster,
					&c_offset);

#ifdef CSM_DP_BUFFER_FENCING
		{
			unsigned long b_backtrack;
			struct csm_dp_buf_cntrl *p;
			b_backtrack = c_offset %
				csm_dp_buf_true_size(&mempool->mem);
			iov_off_array[n] = b_backtrack;
			p = (struct csm_dp_buf_cntrl *)
				(iov[n].iov_base - b_backtrack);
			if (p->signature != CSM_DP_BUFFER_SIG) {
				CSM_DP_ERROR("%s: mempool type %d buffer at "
					"kernel addr %p corrupted, %x, exp %x\n",
					__func__,
					(*mempool_vma->pp_mempool)->type,
					iov[n].iov_base, p->signature,
						CSM_DP_BUFFER_SIG);
				return -EINVAL;
			}
			if (p->fence != CSM_DP_BUFFER_FENCE_SIG) {
				CSM_DP_ERROR("%s: mempool type %d buffer at "
					"kernel addr %p corrupted, fence %x, "
					"exp %x\n", __func__,
					(*mempool_vma->pp_mempool)->type,
					iov[n].iov_base, p->fence,
					CSM_DP_BUFFER_FENCE_SIG);
				return -EINVAL;
			}
			p->state = CSM_DP_BUF_STATE_KERNEL_XMIT_DMA;
			p->xmit_status = CSM_DP_XMIT_IN_PROGRESS;
		}
#endif

		/* link SG fragements */
		b_backtrack = c_offset % csm_dp_buf_true_size(&mempool->mem);
		buf_cntrl = (struct csm_dp_buf_cntrl *)(iov[n].iov_base - b_backtrack);
		if (sg) {
			if (prev_buf_cntrl)
				prev_buf_cntrl->next = buf_cntrl;
			prev_buf_cntrl = buf_cntrl;
			if (n == iov_nr - 1)
				buf_cntrl->next = NULL;
		}	else {
			buf_cntrl->next = NULL;
		}

		atomic_inc(&mempool->out_xmit);
		if (mempool->mem.loc.dma_mapped) {
			/*
			 * set to indicate iov_base is
			 * dma handle instead of
			 * kernal virtual addr
			 */
			dma_addr[n] =
				mempool->mem.loc.cluster_dma_addr[cluster] +
								c_offset;
		} else
			dma_addr[n] = 0;

		CSM_DP_DEBUG("%s: start tx, kaddr=%p len=%lu\n",
			  __func__, iov[n].iov_base, iov[n].iov_len);
	}

	buf_cntrl->next = NULL;

	if (sg)
		flag |= CSM_DP_TX_FLAG_SG;

	if (ioctl_flags & CSM_DP_IOCTL_TX_FLAG_MIRROR)
		flag |= CSM_DP_TX_FLAG_MIRROR;

	ret = csm_dp_tx(pdev, ch, iov, iov_nr, flag, dma_addr);

	if (ret) {
#ifdef CSM_DP_BUFFER_FENCING
		struct csm_dp_buf_cntrl *p;

		for (n = 0; n < iov_nr; n++) {
			p = (struct csm_dp_buf_cntrl *)
				(iov[n].iov_base - iov_off_array[n]);
			p->state = CSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP;
			atomic_dec(&mempool->out_xmit);
			p->xmit_status = ret;
		}
#endif
	} else
		ret = iov_nr;
	wmb(); /* make other CPU see */
	return ret;
}

static int __cdev_rx_poll(
	struct csm_dp_cdev *cdev,
	struct iovec __user *uiov,
	size_t iov_nr)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct iovec iov[CSM_DP_MAX_IOV_SIZE];
	int ret;

	ret = csm_dp_rx_poll(pdev, iov, iov_nr);

	if (ret)
		CSM_DP_DEBUG("%s: csm_dp_rx_poll ret %d\n", __func__, ret);

	if (ret > 0 && copy_to_user((void __user *)uiov, iov, sizeof(struct iovec) * ret))
		ret = -EFAULT;

	return ret;
}

/* Character device interfaces */
static int __cdev_ioctl_mempool_alloc(
	struct csm_dp_cdev *cdev,
	unsigned long ioarg)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_ioctl_mempool_alloc req;
	struct csm_dp_mempool *mempool;

	if (copy_from_user(&req, (void __user *)ioarg, sizeof(req)))
		return -EFAULT;

	mempool = csm_dp_mempool_alloc(pdev, req.type, req.buf_sz, req.buf_num,
				true); /* may do dma_map */
	if (mempool == NULL) {
		CSM_DP_ERROR("%s: csm_mem_alloc failed!\n", __func__);
		return -ENOMEM;
	}

	cdev->mempool_vma[req.type].usr_alloc = true;

	if (req.cfg) {
		struct csm_dp_mempool_cfg cfg;

		csm_dp_mempool_get_cfg(mempool, &cfg);
		if (copy_to_user((void __user *)req.cfg, &cfg, sizeof(cfg))) {
			CSM_DP_ERROR("%s: copy_to_user failed\n", __func__);
			return -EFAULT;
		}
	}
	return 0;
}

static int __cdev_ioctl_mempool_getcfg(
	struct csm_dp_cdev *cdev,
	unsigned long ioarg)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_ioctl_getcfg req;
	struct csm_dp_mempool_cfg cfg;

	if (copy_from_user(&req, (void __user *)ioarg, sizeof(req)))
		return -EFAULT;

	if (!csm_dp_mem_type_is_valid(req.type))
		return -EINVAL;

	if (csm_dp_mempool_get_cfg(pdev->mempool[req.type], &cfg))
		return -EAGAIN;

	if (copy_to_user((void __user *)req.cfg, &cfg, sizeof(cfg))) {
		CSM_DP_ERROR("%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}
	return 0;
}

static int __cdev_ioctl_tx(struct csm_dp_cdev *cdev, unsigned long ioarg)
{
	struct csm_dp_ioctl_tx arg;
	int ret;

	if (copy_from_user(&arg, (void __user *)ioarg, sizeof(arg)))
		return -EFAULT;

	if (!arg.iov.iov_len || arg.iov.iov_len > CSM_DP_MAX_IOV_SIZE)
		return -EINVAL;

	ret = __cdev_tx(cdev, arg.ch, arg.iov.iov_base, arg.iov.iov_len, arg.flags, false);
	return ret;
}

static int __cdev_ioctl_sg_tx(struct csm_dp_cdev *cdev, unsigned long ioarg)
{
	struct csm_dp_ioctl_tx arg;
	int ret;

	if (copy_from_user(&arg, (void __user *)ioarg, sizeof(arg)))
		return -EFAULT;

	if (!arg.iov.iov_len || arg.iov.iov_len > CSM_DP_MAX_IOV_SIZE)
		return -EINVAL;

	ret = __cdev_tx(cdev, arg.ch, arg.iov.iov_base, arg.iov.iov_len, arg.flags, true);
	return ret;
}

static int __cdev_ioctl_rx_getcfg(struct csm_dp_cdev *cdev, unsigned long ioarg)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_ioctl_getcfg req;
	struct csm_dp_ring_cfg cfg;

	if (copy_from_user(&req, (void __user *)ioarg, sizeof(req)))
		return -EFAULT;

	if (!csm_dp_rx_type_is_valid(req.type) || req.cfg == NULL)
		return -EINVAL;

	csm_dp_ring_get_cfg(pdev->rxq[req.type].ring, &cfg);
	if (copy_to_user((void __user *)req.cfg, &cfg, sizeof(cfg))) {
		CSM_DP_ERROR("%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}
	return 0;
}

static int __cdev_ioctl_rx_poll(struct csm_dp_cdev *cdev, unsigned long ioarg)
{
	struct iovec iov;

	if (copy_from_user(&iov, (void __user *)ioarg, sizeof(iov)))
		return -EFAULT;

	if (!iov.iov_len || iov.iov_len > CSM_DP_MAX_IOV_SIZE)
		return -EINVAL;

	return __cdev_rx_poll(cdev, iov.iov_base, iov.iov_len);
}

static unsigned int csm_dp_cdev_poll(struct file *file, poll_table *wait)
{
	struct csm_dp_cdev *cdev = (struct csm_dp_cdev *)file->private_data;
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_rxqueue *rxq;
	unsigned int mask = 0;
	int type, n;

	CSM_DP_DEBUG("%s: poll_table %p\n", __func__, wait);

	for (type = 0, n = 0; type < CSM_DP_RX_TYPE_LAST; type++) {
		if (cdev->rxqueue_vma[type].vma) {
			rxq = &pdev->rxq[type];
			poll_wait(file, &rxq->wq, wait);
			n++;
		}
	}
	if (unlikely(!n)) {
		CSM_DP_DEBUG("%s: rxqueue not mapped!\n", __func__);
		return POLLERR;
	}

	for (type = 0; type < CSM_DP_RX_TYPE_LAST; type++) {
		if (cdev->rxqueue_vma[type].vma) {
			rxq = &pdev->rxq[type];
			if (!csm_dp_ring_is_empty(rxq->ring)) {
				mask |= POLLIN | POLLRDNORM;
				break;
			}
		}
	}
	return mask;
}

static long csm_dp_cdev_ioctl(
	struct file *file,
	unsigned int iocmd,
	unsigned long ioarg)
{
	struct csm_dp_cdev *cdev = (struct csm_dp_cdev *)file->private_data;
	int ret = -EINVAL;

	CSM_DP_DEBUG("%s: ioctl_cmd=%08x pid=%u cdev=%p\n",
		  __func__, iocmd, cdev->pid, cdev);

	switch (iocmd) {
	case CSM_DP_IOCTL_MEMPOOL_ALLOC:
		ret = __cdev_ioctl_mempool_alloc(cdev, ioarg);
		break;
	case CSM_DP_IOCTL_MEMPOOL_GET_CONFIG:
		ret = __cdev_ioctl_mempool_getcfg(cdev, ioarg);
		break;
	case CSM_DP_IOCTL_RX_GET_CONFIG:
		ret = __cdev_ioctl_rx_getcfg(cdev, ioarg);
		break;
	case CSM_DP_IOCTL_TX:
		ret = __cdev_ioctl_tx(cdev, ioarg);
		break;
	case CSM_DP_IOCTL_SG_TX:
		ret = __cdev_ioctl_sg_tx(cdev, ioarg);
		break;
	case CSM_DP_IOCTL_RX_POLL:
		ret = __cdev_ioctl_rx_poll(cdev, ioarg);
		break;
	default:
		break;
	}
	return ret;
}

static void __mempool_mem_vma_open(struct vm_area_struct *vma)
{
	struct csm_dp_mempool_vma *mempool_vma = vma->vm_private_data;
	atomic_t *refcnt = &mempool_vma->refcnt[CSM_DP_MMAP_TYPE_MEM];

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (atomic_add_return(1, refcnt) == 1) {
		struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;

		mempool_vma->vma[CSM_DP_MMAP_TYPE_MEM] = vma;
		if (!csm_dp_mempool_hold(mempool))
			atomic_dec(refcnt);
	}
}

static void __mempool_mem_vma_close(struct vm_area_struct *vma)
{
	struct csm_dp_mempool_vma *mempool_vma = vma->vm_private_data;
	atomic_t *refcnt = &mempool_vma->refcnt[CSM_DP_MMAP_TYPE_MEM];

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (atomic_dec_and_test(refcnt)) {
		struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;

		mempool_vma->vma[CSM_DP_MMAP_TYPE_MEM] = NULL;
		csm_dp_mempool_put(mempool);
	}
}

static const struct vm_operations_struct __mempool_mem_vma_ops = {
	.open   = __mempool_mem_vma_open,
	.close  = __mempool_mem_vma_close,
};

static int __mempool_mem_mmap(
	struct csm_dp_mempool_vma *mempool_vma,
	struct vm_area_struct *vma)
{
	struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;
	struct csm_dp_mem *mem;
	unsigned long size;
	int ret;
	unsigned long addr = vma->vm_start;
	int i;
	unsigned long remainder;

	if (mempool_vma->vma[CSM_DP_MMAP_TYPE_MEM]) {
		CSM_DP_ERROR("%s: memory already mapped\n", __func__);
		return -EBUSY;
	}
	if (!csm_dp_mempool_hold(mempool)) {
		CSM_DP_ERROR("%s: mempool does not exist, mempool %p\n", __func__, mempool);
		return -EAGAIN;
	}

	mem = &mempool->mem;
	size = vma->vm_end - vma->vm_start;
	remainder = mem->loc.size;
	if (size < remainder) {
		ret = -EINVAL;
		CSM_DP_ERROR(
			"%s: size(0x%lx) too small, expect at least 0x%lx\n",
			__func__, size, remainder);
		goto out;
	}

	/* Reset pgoff */
	vma->vm_pgoff = 0;

	for (i = 0; i < mem->loc.num_cluster; i++) {
		unsigned long len;

		if (i ==  mem->loc.num_cluster - 1)
			len = remainder;
		else
			len = CSM_DP_MEMPOOL_CLUSTER_SIZE;

		ret = remap_pfn_range(vma,
				addr,
				page_to_pfn(mem->loc.page[i]),
				len,
				vma->vm_page_prot);
		if (ret) {
			CSM_DP_ERROR("%s: dma mmap failed\n", __func__);
			goto out;
		}
		addr += len;
		remainder -= len;
	}

	vma->vm_private_data = mempool_vma;
	vma->vm_ops = &__mempool_mem_vma_ops;
	__mempool_mem_vma_open(vma);

out:
	csm_dp_mempool_put(mempool);
	return ret;
}

static void __mempool_ring_vma_open(struct vm_area_struct *vma)
{
	struct csm_dp_mempool_vma *mempool_vma = vma->vm_private_data;
	atomic_t *refcnt = &mempool_vma->refcnt[CSM_DP_MMAP_TYPE_RING];

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (atomic_add_return(1, refcnt) == 1) {
		struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;

		mempool_vma->vma[CSM_DP_MMAP_TYPE_RING] = vma;
		__csm_dp_mempool_hold(mempool);
	}
}

static void __mempool_ring_vma_close(struct vm_area_struct *vma)
{
	struct csm_dp_mempool_vma *mempool_vma = vma->vm_private_data;
	atomic_t *refcnt = &mempool_vma->refcnt[CSM_DP_MMAP_TYPE_RING];

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (atomic_dec_and_test(refcnt)) {
		struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;

		mempool_vma->vma[CSM_DP_MMAP_TYPE_RING] = NULL;
		csm_dp_mempool_put(mempool);
	}
}

static const struct vm_operations_struct __mempool_ring_vma_ops = {
	.open   = __mempool_ring_vma_open,
	.close  = __mempool_ring_vma_close,
};

static int __mempool_ring_mmap(
	struct csm_dp_mempool_vma *mempool_vma,
	struct vm_area_struct *vma)
{
	struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;
	struct csm_dp_ring *ring;
	unsigned long size;
	int ret;

	if (mempool_vma->vma[CSM_DP_MMAP_TYPE_RING]) {
		CSM_DP_ERROR("%s: ring already mapped, mem_type=%u\n",
			  __func__, mempool->type);
		ret = -EBUSY;
	}

	if (!csm_dp_mempool_hold(mempool)) {
		CSM_DP_ERROR("%s: mempool not exist\n", __func__);
		return -EAGAIN;
	}

	ring = &mempool->ring;
	size = vma->vm_end - vma->vm_start;
	if (size < csm_dp_mem_loc_mmap_size(&ring->loc)) {
		CSM_DP_ERROR(
			"%s: size(0x%lx) too small, expect at least 0x%lx\n",
			__func__, size, csm_dp_mem_loc_mmap_size(&ring->loc));
		ret = -EINVAL;
		goto out;
	}

	ret = remap_pfn_range(vma,
			      vma->vm_start,
			      page_to_pfn(ring->loc.page[0]),
			      ring->loc.size,
			      vma->vm_page_prot);
	if (ret) {
		CSM_DP_ERROR("%s: remap_pfn_range failed\n", __func__);
		goto out;
	}

	/* Reset pgoff */
	vma->vm_pgoff = 0;
	vma->vm_private_data = mempool_vma;
	vma->vm_ops = &__mempool_ring_vma_ops;
	__mempool_ring_vma_open(vma);

out:
	csm_dp_mempool_put(mempool);
	return ret;
}

/* mmap mempool into user space */
static int __cdev_mempool_mmap(
	struct csm_dp_cdev *cdev,
	struct vm_area_struct *vma)
{
	struct csm_dp_mempool_vma *mempool_vma;
	unsigned int mem_type, type, cookie;
	int ret = 0;

	/* use vm_pgoff to distinguish different area to map */
	cookie = vma->vm_pgoff << PAGE_SHIFT;
	type = MMAP_COOKIE_TO_TYPE(cookie);
	mem_type = MMAP_COOKIE_TO_MEM_TYPE(cookie);

	if (!csm_dp_mem_type_is_valid(mem_type) ||
	    !csm_dp_mmap_type_is_valid(type)) {
		CSM_DP_ERROR("%s: invalid cookie(0x%x)\n", __func__, cookie);
		return -EINVAL;
	}

	mempool_vma = &cdev->mempool_vma[mem_type];
	switch (type) {
	case CSM_DP_MMAP_TYPE_RING:
		/* map ring for buffer manangement */
		ret = __mempool_ring_mmap(mempool_vma, vma);
		break;
	case CSM_DP_MMAP_TYPE_MEM:
		/* map buffer memory */
		ret = __mempool_mem_mmap(mempool_vma, vma);
		break;
	}

	return ret;
}

static void __rxqueue_vma_open(struct vm_area_struct *vma)
{
	struct csm_dp_rxqueue_vma *rxq_vma = vma->vm_private_data;

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (atomic_add_return(1, &rxq_vma->refcnt) == 1) {
		struct csm_dp_rxqueue *rxq;

		rxq_vma->vma = vma;
		rxq_vma->type = MMAP_RX_COOKIE_TO_TYPE(
			vma->vm_pgoff << PAGE_SHIFT);

		rxq = rxqueue_vma_to_rxqueue(rxq_vma);
		atomic_inc(&rxq->refcnt);
	}
}

static void __rxqueue_vma_close(struct vm_area_struct *vma)
{
	struct csm_dp_rxqueue_vma *rxq_vma = vma->vm_private_data;
	struct csm_dp_rxqueue *rxq = rxqueue_vma_to_rxqueue(rxq_vma);

	CSM_DP_DEBUG("%s: vma %p\n", __func__, vma);

	if (!atomic_dec_and_test(&rxq_vma->refcnt))
		return;
	rxq_vma->vma = NULL;
	atomic_dec(&rxq->refcnt);
}

static const struct vm_operations_struct __rxqueue_vma_ops = {
	.open   = __rxqueue_vma_open,
	.close  = __rxqueue_vma_close,
};

/* mmap RXQ into user space */
static int __cdev_rxqueue_mmap(
	struct csm_dp_cdev *cdev,
	struct vm_area_struct *vma)
{
	struct csm_dp_dev *pdev = cdev->pdev;
	struct csm_dp_rxqueue_vma *rxq_vma = cdev->rxqueue_vma;
	struct csm_dp_ring *ring;
	unsigned int type, cookie;
	unsigned long size;
	int ret = 0;

	cookie = vma->vm_pgoff << PAGE_SHIFT;

	type = MMAP_RX_COOKIE_TO_TYPE(cookie);
	if (!csm_dp_rx_type_is_valid(type)) {
		CSM_DP_ERROR(
			"%s: invalid rx queue type, cookie=0x%x, type=%u\n",
			__func__, cookie, type);
		return -EINVAL;
	}

	if (rxq_vma[type].vma) {
		CSM_DP_ERROR("%s: rxqueue already mapped\n", __func__);
		return -EBUSY;
	}

	ring = pdev->rxq[type].ring;
	size = vma->vm_end - vma->vm_start;
	if (size < csm_dp_mem_loc_mmap_size(&ring->loc)) {
		CSM_DP_ERROR(
			"%s: size(0x%lx) too small, expect at least 0x%lx\n",
			__func__, size, csm_dp_mem_loc_mmap_size(&ring->loc));
		return -EINVAL;
	}
	ret = remap_pfn_range(vma,
			      vma->vm_start,
			      page_to_pfn(ring->loc.page[0]),
			      ring->loc.size,
			      vma->vm_page_prot);
	if (ret) {
		CSM_DP_ERROR("%s: rxqueue mmap failed, error=%d\n",
			     __func__, ret);
		return ret;
	}

	vma->vm_private_data = &rxq_vma[type];
	vma->vm_ops = &__rxqueue_vma_ops;
	__rxqueue_vma_open(vma);

	return 0;
}

static int csm_dp_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct csm_dp_cdev *cdev = (struct csm_dp_cdev *)file->private_data;
	unsigned int cookie;
	int ret = 0;

	CSM_DP_DEBUG("%s: start=%lx end=%lx off=%lx proto=%lx flag=%lx",
		  __func__, vma->vm_start, vma->vm_end, vma->vm_pgoff,
		  (unsigned long)vma->vm_page_prot.pgprot, vma->vm_flags);

	cookie = vma->vm_pgoff << PAGE_SHIFT;

	if (is_rxqueue_mmap_cookie(cookie))
		ret = __cdev_rxqueue_mmap(cdev, vma);
	else
		ret = __cdev_mempool_mmap(cdev, vma);

	return ret;
}

static int csm_dp_cdev_open(struct inode *inode, struct file *file)
{
	struct csm_dp_dev *pdev = container_of(inode->i_cdev,
					    struct csm_dp_dev, cdev);
	struct csm_dp_cdev *cdev;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (IS_ERR_OR_NULL(cdev)) {
		CSM_DP_ERROR("%s: failed to alloc memory\n!", __func__);
		return -ENOMEM;
	}

	cdev->pdev = pdev;
	cdev->pid = current->tgid;

	__cdev_init_mempool_vma(cdev);

	mutex_lock(&pdev->cdev_lock);
	list_add_tail(&cdev->list, &pdev->cdev_head);
	mutex_unlock(&pdev->cdev_lock);

	CSM_DP_DEBUG("%s: cdev=%p pid=%u\n", __func__, cdev, cdev->pid);

	file->private_data = cdev;
	return 0;
}

static int csm_dp_cdev_close(struct inode *inode, struct file *file)
{
	struct csm_dp_cdev *cdev = (struct csm_dp_cdev *)file->private_data;
	struct csm_dp_mempool_vma *mempool_vma = cdev->mempool_vma;
	struct csm_dp_dev *pdev = cdev->pdev;
	int type;

	CSM_DP_DEBUG("%s: device close, pid=%u, cdev=%p\n",
		  __func__, cdev->pid, cdev);

	for (type = 0; type < CSM_DP_MEM_TYPE_LAST; type++, mempool_vma++) {
		if (mempool_vma->usr_alloc)
			csm_dp_mempool_put(*mempool_vma->pp_mempool);
	}
	mutex_lock(&pdev->cdev_lock);
	list_del(&cdev->list);
	mutex_unlock(&pdev->cdev_lock);

	kfree(cdev);
	return 0;
}

static const struct file_operations csm_dp_cdev_fops = {
	.owner = THIS_MODULE,
	.poll = csm_dp_cdev_poll,
	.unlocked_ioctl = csm_dp_cdev_ioctl,
	.mmap = csm_dp_cdev_mmap,
	.open = csm_dp_cdev_open,
	.release = csm_dp_cdev_close
};

// called once from DP probe
int csm_dp_cdev_init(struct csm_dp_drv *pdrv)
{
	int ret;

	pdrv->dev_class = class_create(THIS_MODULE, CSM_DP_DEV_CLASS_NAME);
	if (IS_ERR_OR_NULL(pdrv->dev_class)) {
		CSM_DP_ERROR("%s: class_create failed\n", __func__);
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&pdrv->devno, 0, CSM_DP_MAX_NUM_DEVS, CSM_DP_CDEV_NAME);
	if (ret) {
		CSM_DP_ERROR("%s: alloc_chrdev_region failed\n", __func__);
		class_destroy(pdrv->dev_class);
		pdrv->dev_class = NULL;
		pr_err("CSM-DP: failed to initialize cdev\n");
		return ret;
	}

	pr_info("CSM-DP: cdev initialized. __cdev_tx at 0x%p\n", __cdev_tx);
	return 0;
}

void csm_dp_cdev_cleanup(struct csm_dp_drv *pdrv)
{
	if (!pdrv->dev_class)
		return;

	unregister_chrdev_region(pdrv->devno, CSM_DP_MAX_NUM_DEVS);
	class_destroy(pdrv->dev_class);
	pdrv->dev_class = NULL;

	// TODO: cleanup cdevs
}

// called from MHI probe for each VF
int csm_dp_cdev_add(struct csm_dp_dev *pdev, struct device* mhi_dev)
{
	struct device *dev;
	int ret, new_devno;
	struct csm_dp_drv *pdrv = pdev->pdrv;
	unsigned int index = pdev - pdrv->dp_devs;

	if (pdev->cdev_inited) {
		CSM_DP_ERROR("%s: cdev already initialized\n", __func__);
		return -EINVAL;
	}

	ret = csm_dp_rx_init(pdev);
	if (ret)
		return ret;

	mutex_init(&pdev->cdev_lock);
	INIT_LIST_HEAD(&pdev->cdev_head);

	cdev_init(&pdev->cdev, &csm_dp_cdev_fops);
	new_devno = MKDEV(MAJOR(pdrv->devno), index);
	ret = cdev_add(&pdev->cdev, new_devno, 1);
	if (ret) {
		CSM_DP_ERROR("%s: cdev_add failed!\n", __func__);
		goto err;
	}

	dev = device_create(pdrv->dev_class, mhi_dev, new_devno, pdrv, "csm0-dp%d",
			    index);
	if (IS_ERR_OR_NULL(dev)) {
		CSM_DP_ERROR("%s: device_create failed\n", __func__);
		ret = PTR_ERR(dev);
		cdev_del(&pdev->cdev);
		goto err;
	}

	pdev->cdev_inited = true;

	return 0;

err:
	csm_dp_rx_cleanup(pdev);
	mutex_destroy(&pdev->cdev_lock);
	return ret;
}

void csm_dp_cdev_del(struct csm_dp_dev *pdev)
{
	struct csm_dp_drv *pdrv = pdev->pdrv;

	if (!pdev->cdev_inited)
		return;

	device_destroy(pdrv->dev_class, pdev->cdev.dev);
	cdev_del(&pdev->cdev);
	csm_dp_rx_cleanup(pdev);
	mutex_destroy(&pdev->cdev_lock);
	pdev->cdev_inited = false;
}
