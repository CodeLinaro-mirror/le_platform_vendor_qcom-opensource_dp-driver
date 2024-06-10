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
#include "csm_dp.h"
#ifdef CONFIG_DEBUG_FS

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#define MEM_DUMP_COL_WIDTH 16
#define MAX_MEM_DUMP_SIZE 256

#define DEFINE_DEBUGFS_OPS(name, __read, __write)		\
static int name ##_open(struct inode *inode, struct file *file)	\
{								\
	return single_open(file, __read, inode->i_private);	\
}								\
static const struct file_operations name ##_ops = {		\
	.open	 = name ## _open,				\
	.read = seq_read,					\
	.write = __write,					\
	.llseek = seq_lseek,					\
	.release = single_release,				\
}

static struct dentry *__dent;

static int __csm_dp_rxqueue_vma_dump(
	struct seq_file *s,
	struct csm_dp_rxqueue_vma *rxq_vma)
{
	if (rxq_vma->vma) {
		struct vm_area_struct *vma = rxq_vma->vma;

		seq_printf(s, "    Type:               %s\n",
			   csm_dp_rx_type_to_str(rxq_vma->type));
		seq_printf(s, "    RefCnt:             %d\n",
			   atomic_read(&rxq_vma->refcnt));
		seq_printf(s,
			   "        vm_start:       %lx\n"
			   "        vm_end:         %lx\n"
			   "        vm_pgoff:       %lx\n"
			   "        vm_flags:       %lx\n",
			   vma->vm_start,
			   vma->vm_end,
			   vma->vm_pgoff,
			   vma->vm_flags);
	}
	return 0;
}

static int __csm_dp_mempool_vma_dump(
	struct seq_file *s,
	struct csm_dp_mempool_vma *mempool_vma)
{
	struct csm_dp_mempool *mempool = *mempool_vma->pp_mempool;
	struct vm_area_struct *vma;
	int i;

	if (mempool)
		seq_printf(s, "    Type:               %s\n",
			   csm_dp_mem_type_to_str(mempool->type));

	for (i = 0; i < CSM_DP_MMAP_TYPE_LAST; i++) {
		if (mempool_vma->vma[i]) {
			vma = mempool_vma->vma[i];
			seq_printf(s, "    VMA[%d]:             %s\n",
				   i, csm_dp_mmap_type_to_str(i));
			seq_printf(s,
				   "        vm_start:       %lx\n"
				   "        vm_end:         %lx\n"
				   "        vm_pgoff:       %lx\n"
				   "        vm_flags:       %lx\n",
				   vma->vm_start,
				   vma->vm_end,
				   vma->vm_pgoff,
				   vma->vm_flags);
			seq_printf(s, "        refcnt:         %d\n",
				   atomic_read(&mempool_vma->refcnt[i]));
		}
	}
	return 0;
}

static int __csm_dp_ring_opstats_dump(
	struct seq_file *s,
	struct csm_dp_ring_opstats *stats)
{
	seq_puts(s, "Read:\n");
	seq_printf(s, "    Ok:                %u\n", atomic_read(&stats->read_ok));
	seq_printf(s, "    Empty:             %u\n", atomic_read(&stats->read_empty));
	seq_puts(s, "Write:\n");
	seq_printf(s, "    Ok:                %u\n", atomic_read(&stats->write_ok));
	seq_printf(s, "    Full:              %u\n", atomic_read(&stats->write_full));
	return 0;
}

static int __csm_dp_ring_runtime_dump(
	struct seq_file *s,
	struct csm_dp_ring *ring)
{
	seq_printf(s, "ProdHdr:                %u\n", *ring->prod_head);
	seq_printf(s, "ProdTail:               %u\n", *ring->prod_tail);
	seq_printf(s, "ConsHdr:                %u\n", *ring->cons_head);
	seq_printf(s, "ConsTail:               %u\n", *ring->cons_tail);
	seq_printf(s, "NumOfElementAvail:      %u\n",
		   (*ring->prod_head - *ring->cons_tail) & (ring->size - 1));
	return 0;
}

static int __csm_dp_ring_config_dump(
	struct seq_file *s,
	struct csm_dp_ring *ring)
{
	seq_printf(s, "Ring %llx MemoryAlloc:\n", (u64) ring);
	seq_printf(s, "         AllocAddr:     %llx\n", (u64) ring->loc.base);
	seq_printf(s, "         AllocSize:     0x%08lx\n", ring->loc.size);
	seq_printf(s, "         MmapCookie:    0x%08x\n", ring->loc.cookie);
	seq_printf(s, "Size:                   0x%x\n", ring->size);
	seq_printf(s, "ProdHdr:                %llx\n", (u64) ring->prod_head);
	seq_printf(s, "ProdTail:               %llx\n", (u64) ring->prod_tail);
	seq_printf(s, "ConsHdr:                %llx\n", (u64) ring->cons_head);
	seq_printf(s, "ConsTail:               %llx\n", (u64) ring->cons_tail);
	seq_printf(s, "RingBuf:                %llx\n", (u64) ring->element);
	return 0;
}

static int debugfs_rxq_refcnt_read(struct seq_file *s, void *unused)
{
	struct csm_dp_rxqueue *rxq = (struct csm_dp_rxqueue *)s->private;

	if (rxq->inited)
		seq_printf(s, "%d\n", atomic_read(&rxq->refcnt));

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_rxq_refcnt, debugfs_rxq_refcnt_read, NULL);

static int debugfs_rxq_opstats_read(struct seq_file *s, void *unused)
{
	struct csm_dp_rxqueue *rxq = (struct csm_dp_rxqueue *)s->private;

	if (rxq->inited)
		__csm_dp_ring_opstats_dump(s, &rxq->ring->opstats);

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_rxq_opstats, debugfs_rxq_opstats_read, NULL);

static int debugfs_rxq_config_read(struct seq_file *s, void *unused)
{
	struct csm_dp_rxqueue *rxq = (struct csm_dp_rxqueue *)s->private;

	if (rxq->inited) {
		seq_printf(s, "Type:                   %s\n",
			   csm_dp_rx_type_to_str(rxq->type));
		__csm_dp_ring_config_dump(s, rxq->ring);
	}

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_rxq_config, debugfs_rxq_config_read, NULL);

static int debugfs_rxq_runtime_read(struct seq_file *s, void *unused)
{
	struct csm_dp_rxqueue *rxq = (struct csm_dp_rxqueue *)s->private;

	if (rxq->inited)
		__csm_dp_ring_runtime_dump(s, rxq->ring);

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_rxq_runtime, debugfs_rxq_runtime_read, NULL);

static unsigned int __mem_dump_size[CSM_DP_MEM_TYPE_LAST];
static unsigned int __mem_offset[CSM_DP_MEM_TYPE_LAST];

static int debugfs_mem_data_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;
		unsigned int n = __mem_dump_size[mempool->type];
		unsigned int offset = __mem_offset[mempool->type];
		unsigned int i, j;
		unsigned int cluster, c_offset;
		unsigned char *data = (unsigned char *)mem->loc.base + offset;

		data = csm_dp_mem_offset_addr(mem, offset, &cluster, &c_offset);
		if (data == NULL)
			return 0;
		if (n > (mem->loc.size - offset))
			n = mem->loc.size - offset;

		for (i = 0; i < offset % MEM_DUMP_COL_WIDTH; i++)
			seq_puts(s, "   ");

		for (j = 0; j < n; j++, i++) {
			if (i && !(i % MEM_DUMP_COL_WIDTH))
				seq_puts(s, "\n");
			seq_printf(s, "%02x ", *data);
			data++;
			c_offset++;
			if (c_offset >= CSM_DP_MEMPOOL_CLUSTER_SIZE) {
				c_offset = 0;
				cluster++;
				data = mem->loc.cluster_kernel_addr[cluster];
			}
		}
		seq_puts(s, "\n");
	}
	return 0;
}

static ssize_t debugfs_mem_data_write(
	struct file *fp,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct csm_dp_mempool *mempool = *((struct csm_dp_mempool **)
			(((struct seq_file *)fp->private_data)->private));

	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;
		unsigned int value = 0;
		unsigned int *data;
		unsigned int offset = __mem_offset[mempool->type];
		unsigned int cluster, c_offset;

		if (kstrtouint_from_user(buf, count, 0, &value))
			return -EFAULT;
		data = (unsigned int *)csm_dp_mem_offset_addr(
				mem, offset, &cluster, &c_offset);
		if (data == NULL)
			return count;
		*data = value;
	}
	return count;
}
DEFINE_DEBUGFS_OPS(debugfs_mem_data, debugfs_mem_data_read,
		   debugfs_mem_data_write);

static int debugfs_mem_dump_size_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		seq_printf(s, "%u\n", __mem_dump_size[mempool->type]);
	return 0;
}

static ssize_t debugfs_mem_dump_size_write(
	struct file *fp,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct csm_dp_mempool *mempool = *((struct csm_dp_mempool **)
			(((struct seq_file *)fp->private_data)->private));
	unsigned int value = 0;

	if (!mempool)
		goto done;

	if (kstrtouint_from_user(buf, count, 0, &value))
		return -EFAULT;

	if (value > MAX_MEM_DUMP_SIZE)
		return -EINVAL;

	__mem_dump_size[mempool->type] = value;
done:
	return count;
}
DEFINE_DEBUGFS_OPS(debugfs_mem_dump_size, debugfs_mem_dump_size_read,
		   debugfs_mem_dump_size_write);

static int debugfs_mem_offset_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		seq_printf(s, "0x%08x\n", __mem_offset[mempool->type]);
	return 0;
}

static ssize_t debugfs_mem_offset_write(
	struct file *fp,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct csm_dp_mempool *mempool = *((struct csm_dp_mempool **)
			(((struct seq_file *)fp->private_data)->private));

	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;
		unsigned int value = 0;

		if (kstrtouint_from_user(buf, count, 0, &value))
			return -EFAULT;

		if (value >= mem->loc.size)
			return -EINVAL;
		if (value & 3)
			return -EINVAL;

		__mem_offset[mempool->type] = value;
	}
	return count;
}
DEFINE_DEBUGFS_OPS(debugfs_mem_offset, debugfs_mem_offset_read,
		   debugfs_mem_offset_write);

static int debugfs_mem_config_show(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);
	int i;

	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;

		seq_puts(s, "MemoryAlloc:\n");
		seq_printf(s, "    AllocSize:     0x%08lx\n",
			   mem->loc.size);
		seq_printf(s, "    Total Cluster:  %d\n",
			   mem->loc.num_cluster);
		seq_printf(s, "    Cluster Size:  0x%x\n",
			   CSM_DP_MEMPOOL_CLUSTER_SIZE);
		for (i = 0; i < mem->loc.num_cluster; i++)
			seq_printf(s, "    Cluster %d Addr: %llx\n", i,
					(u64) mem->loc.cluster_kernel_addr[i]);
		seq_printf(s, "    Buffer Per Cluster:  %d\n",
			   mem->loc.buf_per_cluster);
		seq_printf(s, "    Last Cluster Order:  %d\n",
			   mem->loc.last_cl_order);
		seq_printf(s, "    MmapCookie:    %08x\n",
			   mem->loc.cookie);
		seq_printf(s, "BufSize:                0x%x\n", mem->buf_sz);
		seq_printf(s, "BufCount:               0x%x\n", mem->buf_cnt);
		seq_printf(s, "BufTrueSize:            0x%x\n",
			   csm_dp_buf_true_size(mem));

	}

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mem_config, debugfs_mem_config_show, NULL);

static int debugfs_mem_buffer_state_show(struct seq_file *s, void *unused)
{
	int i, j;
	int k_free = 0;
	int k_alloc_dma = 0;
	int k_recv_msgq_app = 0;
	int k_xmit_dma = 0;
	int k_xmit_dma_comp = 0;
	int u_free = 0;
	int u_alloc = 0;
	int u_recv = 0;
	char *cl_start;
	unsigned int cl_buf_cnt;
	struct csm_dp_buf_cntrl *p;

	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;

		if (!csm_dp_mem_type_is_valid(mempool->type))
			return 0;

		for (j = 0; j < mem->loc.num_cluster; j++) {
			cl_start = mem->loc.cluster_kernel_addr[j];
			if (j == mem->loc.num_cluster - 1)
				cl_buf_cnt = mem->buf_cnt -
					(mem->loc.buf_per_cluster * j);
			else
				cl_buf_cnt = mem->loc.buf_per_cluster;
			for (i = 0; i < cl_buf_cnt; i++) {
				p = (struct csm_dp_buf_cntrl *) (cl_start +
					(i * csm_dp_buf_true_size(mem)));

				if(!p)
					break;
				if(p->state == CSM_DP_BUF_STATE_KERNEL_FREE)
					k_free++;
				if(p->state == CSM_DP_BUF_STATE_KERNEL_ALLOC_RECV_DMA)
					k_alloc_dma++;
				if(p->state == CSM_DP_BUF_STATE_KERNEL_RECVCMP_MSGQ_TO_APP)
					k_recv_msgq_app++;
				if(p->state == CSM_DP_BUF_STATE_KERNEL_XMIT_DMA)
					k_xmit_dma++;
				if(p->state == CSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP)
					k_xmit_dma_comp++;
				if(p->state == CSM_DP_BUF_STATE_USER_FREE)
					u_free++;
				if(p->state == CSM_DP_BUF_STATE_USER_ALLOC)
					u_alloc++;
				if(p->state == CSM_DP_BUF_STATE_USER_RECV)
					u_recv++;
			}
		}

		seq_puts(s, "MemoryBufferState:\n");
		seq_printf(s, "MemoryType:	%s\n", csm_dp_mem_type_to_str(mempool->type));
		seq_printf(s, "   KERNEL_FREE:     %d\n",
			   k_free);
		seq_printf(s, "   KERNEL_ALLOC_RECV_DMA:  %d\n",
			   k_alloc_dma);
		seq_printf(s, "   KERNEL_RECVCMP_MSGQ_TO_APP:  %d\n",
			   k_recv_msgq_app);
		seq_printf(s, "   KERNEL_XMIT_DMA:     %d\n",
			   k_xmit_dma);
		seq_printf(s, "   KERNEL_XMIT_DMA_COMP:  %d\n",
			   k_xmit_dma_comp);
		seq_printf(s, "   USER_FREE:  %d\n",
			   u_free);
		seq_printf(s, "   USER_ALLOC:  %d\n",
			   u_alloc);
		seq_printf(s, "   USER_RECV:  %d\n",
			   u_recv);

	}

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mem_buffer_state, debugfs_mem_buffer_state_show, NULL);
static int debugfs_ring_config_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		__csm_dp_ring_config_dump(s, &mempool->ring);
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_ring_config, debugfs_ring_config_read, NULL);

static int debugfs_ring_runtime_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		__csm_dp_ring_runtime_dump(s, &mempool->ring);
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_ring_runtime, debugfs_ring_runtime_read, NULL);

static int debugfs_ring_opstats_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		__csm_dp_ring_opstats_dump(s, &mempool->ring.opstats);
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_ring_opstats, debugfs_ring_opstats_read, NULL);

unsigned long __ring_index[CSM_DP_MEM_TYPE_LAST];

static int debugfs_ring_index_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool)
		seq_printf(s, "%lu\n", __ring_index[mempool->type]);
	return 0;
}

static ssize_t debugfs_ring_index_write(
	struct file *fp,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct csm_dp_mempool *mempool = *((struct csm_dp_mempool **)
			(((struct seq_file *)fp->private_data)->private));
	unsigned int value = 0;

	if (!mempool)
		goto done;

	if (kstrtouint_from_user(buf, count, 0, &value))
		return -EFAULT;

	if (value >= mempool->ring.size)
		return -EINVAL;

	__ring_index[mempool->type] = value;
done:
	return count;
}
DEFINE_DEBUGFS_OPS(debugfs_ring_index, debugfs_ring_index_read,
		   debugfs_ring_index_write);

static int debugfs_ring_data_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool) {
		csm_dp_ring_element_t *elem_p;

		elem_p = (mempool->ring.element + __ring_index[mempool->type]);

		seq_printf(s, "0x%lx\n", elem_p->element_data);
	}
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_ring_data, debugfs_ring_data_read, NULL);

static int debugfs_mempool_status_show(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool) {
		seq_printf(s, "BufPut:                 %lu\n",
			   mempool->stats.buf_put);
		seq_printf(s, "InvalidBufPut:          %lu\n",
			   mempool->stats.invalid_buf_put);
		seq_printf(s, "ErrBufPut:              %lu\n",
			   mempool->stats.buf_put_err);
		seq_printf(s, "BufGet:                 %lu\n",
			   mempool->stats.buf_get);
		seq_printf(s, "InvalidBufGet:          %lu\n",
			   mempool->stats.invalid_buf_get);
		seq_printf(s, "ErrBufGet:              %lu\n",
			   mempool->stats.buf_get_err);
	}
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mempool_status, debugfs_mempool_status_show, NULL);

static int debugfs_mempool_state_show(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);
	unsigned long state_cnt[CSM_DP_BUF_STATE_LAST];
	unsigned long buf_bad = 0;
	unsigned long unknown_state = 0;
	int i;

	memset(state_cnt, 0, sizeof(state_cnt));
	if (mempool) {
		struct csm_dp_mem *mem = &mempool->mem;
		struct csm_dp_buf_cntrl *p;

		for (i = 0; i < mem->buf_cnt; i++) {
			p = (struct csm_dp_buf_cntrl *)
				csm_dp_mem_rec_addr(mem, i);
			if (p == NULL)
				return 0;
#ifdef CSM_DP_BUFFER_FENCING
			if (p->signature != CSM_DP_BUFFER_SIG ||
					p->fence != CSM_DP_BUFFER_FENCE_SIG ||
					p->buf_index != i)
				buf_bad++;
			else if (p->state >= CSM_DP_BUF_STATE_LAST)
#else
			if (p->state >= CSM_DP_BUF_STATE_LAST)
#endif
				unknown_state++;
			else
				state_cnt[p->state]++;

		}

		seq_printf(s, "Total Buf:                  %u\n",
			   mem->buf_cnt);
		seq_printf(s, "Buf Real Size:              %u\n",
			   mem->buf_sz + mem->buf_overhead_sz);
		seq_printf(s, "Buf Corrupted:              %lu\n",
			   buf_bad);
		seq_printf(s, "Buf Unknown State:          %lu\n",
			   unknown_state);

		for (i = 0; i < CSM_DP_BUF_STATE_LAST; i++) {
			if (state_cnt[i]) {
				seq_printf(s, "Buf State %s:        ",
						csm_dp_buf_state_to_str(i));
				seq_printf(s, "                    %lu\n",
						state_cnt[i]);
			}
		}
	}
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mempool_state, debugfs_mempool_state_show, NULL);

static int debugfs_mempool_active_show(struct seq_file *s, void *unused)
{
	struct csm_dp_dev *pdev = (struct csm_dp_dev *)s->private;
	unsigned int type;

	for (type = 0; type < CSM_DP_MEM_TYPE_LAST; type++) {
		if (pdev->mempool[type])
			seq_printf(s, "%s ", csm_dp_mem_type_to_str(type));
	}
	seq_puts(s, "\n");
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mempool_active, debugfs_mempool_active_show, NULL);

static int debugfs_mempool_info_show(struct seq_file *s, void *unused)
{
	struct csm_dp_mempool *mempool =
		*((struct csm_dp_mempool **)s->private);

	if (mempool) {
		seq_printf(s, "Driver:                 %llx\n",
							(u64) mempool->dp_dev);
		seq_printf(s, "MemPool:                %llx\n",
							(u64) mempool);
		seq_printf(s, "Type:                   %s\n",
			   csm_dp_mem_type_to_str(mempool->type));
		seq_printf(s, "Ref:                    %d\n",
			   atomic_read(&mempool->ref));
	}
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mempool_info, debugfs_mempool_info_show, NULL);

static int debugfs_start_recovery_dev_read(struct seq_file *s, void *unused)
{
	struct csm_dp_mhi *mhi = (struct csm_dp_mhi *)s->private;
	if (mhi) {
		seq_printf(s, "mhi_dev_suspended: %u\n", mhi->mhi_dev_suspended==true?1:0);
	}

	return 0;
}
static ssize_t debugfs_start_recovery_dev_write(
	struct file *fp,
	const char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct csm_dp_mhi *mhi = ((struct csm_dp_mhi *)
		(((struct seq_file *)fp->private_data)->private));
	int retry = 10;

	if (mhi->mhi_dev_suspended) {
		printk(KERN_INFO "%s: MHI channel is already suspended\n", __func__);
		return count;
	}

	/* Start the recovery operation */
	mhi->mhi_dev_suspended = true;
	printk(KERN_INFO "%s: Recovering MHI channel..\n", __func__);
	/* Allocating work to queue */
	queue_work(mhi->mhi_dev_workqueue, &mhi->alloc_work);

	while (mhi->mhi_dev_suspended && retry) {
		msleep(50);
		retry--;
	}

	if (!mhi->mhi_dev_suspended)
		printk(KERN_INFO "%s: MHI channel recovery is complete\n", __func__);
	return count;
}

DEFINE_DEBUGFS_OPS(debugfs_start_recovery_control_dev, debugfs_start_recovery_dev_read, debugfs_start_recovery_dev_write);
DEFINE_DEBUGFS_OPS(debugfs_start_recovery_data_dev, debugfs_start_recovery_dev_read,  debugfs_start_recovery_dev_write);

static int debugfs_mhi_show(struct seq_file *s, void *unused)
{
	struct csm_dp_mhi *mhi = (struct csm_dp_mhi *)s->private;

	seq_printf(s, "MHIDevice:              %llx\n", (u64) mhi->mhi_dev);
	seq_puts(s, "Stats:\n");
	seq_printf(s, "    TX:                 %lu\n", mhi->stats.tx_cnt);
	seq_printf(s, "    TX_ACKED:           %lu\n", mhi->stats.tx_acked);
	seq_printf(s, "    TX_ERR:             %lu\n", mhi->stats.tx_err);
	seq_printf(s, "    RX:                 %lu\n", mhi->stats.rx_cnt);
	seq_printf(s, "    RX_ERR:             %lu\n", mhi->stats.rx_err);
	seq_printf(s, "    RX_OUT_OF_BUF:      %lu\n",
		   mhi->stats.rx_out_of_buf);
	seq_printf(s, "    RX_REPLENISH:       %lu\n",
		   mhi->stats.rx_replenish);
	seq_printf(s, "    RX_REPLENISH_ERR:   %lu\n",
		   mhi->stats.rx_replenish_err);
	seq_printf(s, "    CHANNEL_ERR_COUNT:   %lu\n",
		   mhi->stats.ch_err_cnt);
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_mhi, debugfs_mhi_show, NULL);

static int debugfs_cdev_show(struct seq_file *s, void *unused)
{
	struct csm_dp_dev *pdev = (struct csm_dp_dev *)s->private;
	struct csm_dp_cdev *cdev;
	int n = 0;
	int i;

	if (!pdev->cdev_inited)
		return 0;

	mutex_lock(&pdev->cdev_lock);
	list_for_each_entry(cdev, &pdev->cdev_head, list) {
		seq_printf(s, "CDEV(%d)\n", n++);
		seq_printf(s, "Driver:                 %llx\n",
							(u64) cdev->pdev);
		seq_printf(s, "Cdev:                   %llx\n",
							(u64) cdev);
		seq_printf(s, "PID:                    %d\n",
							cdev->pid);

		for (i = 0; i < CSM_DP_MEM_TYPE_LAST; i++) {
			seq_printf(s, "MemPoolVMA[%d]\n", i);
			__csm_dp_mempool_vma_dump(s, &cdev->mempool_vma[i]);
		}
		seq_puts(s, "RxQueue\n");
		for (i = 0; i < CSM_DP_RX_TYPE_LAST; i++)
			__csm_dp_rxqueue_vma_dump(s, &cdev->rxqueue_vma[i]);
	}
	mutex_unlock(&pdev->cdev_lock);

	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_cdev, debugfs_cdev_show, NULL);

static int debugfs_dev_status_show(struct seq_file *s, void *unused)
{
	struct csm_dp_dev *pdev = (struct csm_dp_dev *)s->private;
	struct csm_dp_core_stats *stats = &pdev->stats;
	int i;

	seq_printf(s, "TX:             %lu\n", stats->tx_cnt);
	seq_printf(s, "TX_ERR:         %lu\n", stats->tx_err);
	seq_printf(s, "TX_DROP:        %lu\n", stats->tx_drop);
	seq_printf(s, "RX:             %lu\n", stats->rx_cnt);
	seq_printf(s, "RX_BADMSG:      %lu\n", stats->rx_badmsg);
	seq_printf(s, "RX_DROP:        %lu\n", stats->rx_drop);
	seq_printf(s, "RX_INT:         %lu\n", stats->rx_int);
	seq_printf(s, "RX_BUDGET_OVF:  %lu\n", stats->rx_budget_overflow);
	seq_printf(s, "RX_IGNORE:      %lu\n", stats->rx_poll_ignore);
	for (i = 0; i < CSM_DP_MEM_TYPE_LAST; i++) {
		seq_printf(s, "Mempool[%d]\n", i);
		seq_printf(s, "MEM_POOL_IN_USE:	%lu\n", stats->mem_stats.mempool_mem_in_use[i]);
		seq_printf(s, "MEM_DMA_MAPPED:	%lu\n", stats->mem_stats.mempool_mem_dma_mapped[i]);
		seq_printf(s, "MEM_RING_IN_USE:	%lu\n", stats->mem_stats.mempool_ring_in_use[i]);
	}
	for (i = 0; i < CSM_DP_RX_TYPE_LAST; i++) {
		seq_printf(s, "RXQ[%d]\n", i);
		seq_printf(s, "MEM_RXQ_IN_USE:		%lu\n", stats->mem_stats.rxq_ring_in_use[i]);
	}
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_dev_status, debugfs_dev_status_show, NULL);

static int debugfs_drv_show(struct seq_file *s, void *unused)
{
	struct csm_dp_drv *drv = (struct csm_dp_drv *)s->private;

	seq_printf(s, "Driver:         %llx\n", (u64) drv);
	return 0;
}
DEFINE_DEBUGFS_OPS(debugfs_drv, debugfs_drv_show, NULL);

static int debugfs_create_rxq_dir(struct dentry *parent, struct csm_dp_dev *pdev)
{
	struct dentry *entry = NULL, *dentry = NULL, *root = NULL;
	unsigned int type;

	root = debugfs_create_dir("rxque", parent);
	if (IS_ERR_OR_NULL(root))
		return -ENOMEM;

	for (type = 0; type < CSM_DP_RX_TYPE_LAST; type++) {
		dentry = debugfs_create_dir(csm_dp_rx_type_to_str(type),
					    root);
		if (IS_ERR_OR_NULL(dentry))
			return -ENOMEM;

		entry = debugfs_create_file("config", 0444, dentry,
					    &pdev->rxq[type],
					    &debugfs_rxq_config_ops);
		if (!entry)
			return -ENOMEM;

		entry = debugfs_create_file("runtime", 0444, dentry,
					    &pdev->rxq[type],
					    &debugfs_rxq_runtime_ops);
		if (!entry)
			return -ENOMEM;

		entry = debugfs_create_file("opstats", 0444, dentry,
					    &pdev->rxq[type],
					    &debugfs_rxq_opstats_ops);
		if (!entry)
			return -ENOMEM;

		entry = debugfs_create_file("refcnt", 0444, dentry,
					    &pdev->rxq[type],
					    &debugfs_rxq_refcnt_ops);
		if (!entry)
			return -ENOMEM;
	}
	return 0;
}

static int debugfs_create_ring_dir(
	struct dentry *parent,
	struct csm_dp_mempool **mempool)
{
	struct dentry *entry = NULL, *dentry = NULL;

	dentry = debugfs_create_dir("ring", parent);
	if (IS_ERR_OR_NULL(dentry))
		return -ENOMEM;

	entry = debugfs_create_file("config", 0444, dentry,
				    mempool,
				    &debugfs_ring_config_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("runtime", 0444, dentry,
				    mempool,
				    &debugfs_ring_runtime_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("index", 0644, dentry,
				    mempool,
				    &debugfs_ring_index_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("data", 0644, dentry,
				    mempool,
				    &debugfs_ring_data_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("opstats", 0444, dentry,
				    mempool,
				    &debugfs_ring_opstats_ops);
	if (!entry)
		return -ENOMEM;

	return 0;
}

static int debugfs_create_mem_dir(
	struct dentry *parent,
	struct csm_dp_mempool **mempool)
{
	struct dentry *entry = NULL, *dentry = NULL;

	dentry = debugfs_create_dir("mem", parent);
	if (IS_ERR_OR_NULL(dentry))
		return -ENOMEM;

	entry = debugfs_create_file("config", 0444, dentry,
				    mempool,
				    &debugfs_mem_config_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("offset", 0444, dentry,
				    mempool,
				    &debugfs_mem_offset_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("dump_size", 0644, dentry,
				    mempool,
				    &debugfs_mem_dump_size_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("data", 0644, dentry,
				    mempool,
				    &debugfs_mem_data_ops);
	if (!entry)
		return -ENOMEM;

	entry = debugfs_create_file("buffer_state", 0444, dentry,
				     mempool,
				     &debugfs_mem_buffer_state_ops);
	if (!entry)
		return -ENOMEM;

	return 0;
}

static int debugfs_create_mempool_dir(
	struct dentry *parent,
	struct csm_dp_dev *pdev)
{
	struct dentry *entry = NULL, *dentry = NULL, *root = NULL;
	int ret;
	unsigned int type;

	root = debugfs_create_dir("mempool", parent);
	if (IS_ERR_OR_NULL(root))
		return -ENOMEM;

	entry = debugfs_create_file("active", 0444, root, pdev,
				    &debugfs_mempool_active_ops);
	if (!entry)
		return -ENOMEM;

	for (type = 0; type < CSM_DP_MEM_TYPE_LAST; type++) {
		dentry = debugfs_create_dir(csm_dp_mem_type_to_str(type), root);
		if (IS_ERR_OR_NULL(dentry))
			return -ENOMEM;

		ret = debugfs_create_ring_dir(dentry, &pdev->mempool[type]);
		if (ret)
			return ret;

		ret = debugfs_create_mem_dir(dentry, &pdev->mempool[type]);
		if (ret)
			return ret;

		entry = debugfs_create_file("info", 0444, dentry,
					    &pdev->mempool[type],
					    &debugfs_mempool_info_ops);
		if (!entry)
			return -ENOMEM;

		entry = debugfs_create_file("status", 0444, dentry,
					    &pdev->mempool[type],
					    &debugfs_mempool_status_ops);
		if (!entry)
			return -ENOMEM;

		entry = debugfs_create_file("state", 0444, dentry,
					    &pdev->mempool[type],
					    &debugfs_mempool_state_ops);
		if (!entry)
			return -ENOMEM;
	}
	return 0;
}

int csm_dp_debugfs_init(struct csm_dp_drv *drv)
{
	struct dentry *entry = NULL;
	struct dentry *dp_dev_entry;
	int i;

	if (unlikely(drv == NULL))
		return -EINVAL;

	if (unlikely(__dent))
		return -EBUSY;
	__dent = debugfs_create_dir(CSM_DP_MODULE_NAME, 0);
	if (IS_ERR_OR_NULL(__dent))
		return -ENOMEM;
	entry = debugfs_create_file("driver", 0444, __dent, drv,
				    &debugfs_drv_ops);
	if (!entry)
		goto err;
	for (i = 0; i < CSM_DP_MAX_NUM_DEVS; i++) {
		char buf[10];
		struct csm_dp_dev *pdev = &drv->dp_devs[i];

		snprintf(buf, sizeof(buf), "dev%d", i);
		dp_dev_entry = debugfs_create_dir(buf, __dent);
		if (IS_ERR_OR_NULL(dp_dev_entry))
			goto err;
		entry = debugfs_create_file("cdev", 0444, dp_dev_entry, pdev,
							&debugfs_cdev_ops);
		if (!entry)
			goto err;
		entry = debugfs_create_file("mhi_control_dev", 0444, dp_dev_entry,
					    &pdev->mhi_control_dev, &debugfs_mhi_ops);
		if (!entry)
			goto err;

		entry = debugfs_create_file("mhi_data_dev", 0444, dp_dev_entry,
					    &pdev->mhi_data_dev, &debugfs_mhi_ops);
		if (!entry)
			goto err;

		entry = debugfs_create_file("status", 0444, dp_dev_entry, pdev,
							&debugfs_dev_status_ops);
		if (!entry)
			goto err;

		entry = debugfs_create_file("start_recovery_mhi_control_dev", 0644, dp_dev_entry,
					    &pdev->mhi_control_dev, &debugfs_start_recovery_control_dev_ops);
		if (!entry)
			goto err;

		entry = debugfs_create_file("start_recovery_mhi_data_dev", 0644, dp_dev_entry,
					    &pdev->mhi_data_dev, &debugfs_start_recovery_data_dev_ops);
		if (!entry)
			goto err;

		if (debugfs_create_mempool_dir(dp_dev_entry, pdev))
			goto err;

		if (debugfs_create_rxq_dir(dp_dev_entry, pdev))
			goto err;
	}

	return 0;
err:
	debugfs_remove_recursive(__dent);
	__dent = NULL;
	return -ENOMEM;
}

void csm_dp_debugfs_cleanup(struct csm_dp_drv *drv)
{
	debugfs_remove_recursive(__dent);
	__dent = NULL;
}

#else

int csm_dp_debugfs_init(struct csm_dp_drv *drv)
{
	return 0;
}

void csm_dp_debugfs_cleanup(struct csm_dp_drv *drv)
{
}
#endif
