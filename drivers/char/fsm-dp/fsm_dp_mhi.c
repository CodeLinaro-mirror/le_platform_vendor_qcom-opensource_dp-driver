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
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mod_devicetable.h>

#include "fsm_dp.h"
#include "fsm_dp_mhi.h"

static struct fsm_dp_drv *__pdrv;


/*
 * Dump a packet.
 */
#define FSM_DP_HEX_DUMP_BUF_SIZE (32 * 3 + 2 + 32 + 1)
static void fsm_dp_print_hex_dump(const char *level,
			const char *prefix_str, int prefix_type,
			int rowsize, int groupsize,
			const void *buf, size_t len, bool ascii)
{
	const u8 *ptr = buf;
	int i, linelen, remaining = len;
	unsigned char linebuf[FSM_DP_HEX_DUMP_BUF_SIZE];

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;

	for (i = 0; i < len; i += rowsize) {
		linelen = min(remaining, rowsize);
		remaining -= rowsize;

		hex_dump_to_buffer(ptr + i, linelen, rowsize, groupsize,
				   linebuf, sizeof(linebuf), ascii);

		switch (prefix_type) {
		case DUMP_PREFIX_ADDRESS:
			printk("%s%s%p: %s\n",
			       level, prefix_str, ptr + i, linebuf);
			break;
		case DUMP_PREFIX_OFFSET:
			printk("%s%s%.8x: %s\n", level, prefix_str, i, linebuf);
			break;
		default:
			printk("%s%s%s\n", level, prefix_str, linebuf);
			break;
		}
	}
}

static int do_dump;
void fsm_dp_hex_dump(unsigned char *buf, unsigned int len)
{
	if (do_dump)
		fsm_dp_print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, buf, len, false);
}
EXPORT_SYMBOL(fsm_dp_hex_dump);

static int __mhi_rx_replenish(
	struct fsm_dp_drv *drv,
	struct fsm_dp_mempool *mempool)
{
	struct fsm_dp_mhi *mhi = &drv->mhi;
	struct mhi_device *mhi_dev = mhi->mhi_dev;
	int nr = mhi_get_free_desc_count(mhi_dev, DMA_FROM_DEVICE);
	void *buf;
	int ret, i, to_xfer;
	bool outofbuf;
	unsigned int cluster, c_offset;
	struct fsm_dp_buf_cntrl *first_buf_cntrl = NULL, *buf_cntrl = NULL, *prev_buf_cntrl = NULL;

	ret = 0;
	if (nr < mhi_get_total_descriptors(mhi_dev, DMA_FROM_DEVICE) / 8)
		return ret;
	for (; nr > 0;) {
		to_xfer = min(FSM_DP_MAX_IOV_SIZE, nr);
		outofbuf = false;
		for (i = 0; i < to_xfer; i++) {
			buf = fsm_dp_mempool_get_buf(mempool, &cluster,
								&c_offset);
			FSM_DP_ASSERT(!buf, "can not alloc buffer, cannot use dummy_buf");
			if (buf == NULL) {
				mhi->stats.rx_out_of_buf++;
				FSM_DP_DEBUG("%s: out of rx buffer!\n", __func__);
				outofbuf = true;
				buf = mempool->dummy_buf;
			}
			FSM_DP_ASSERT(!buf, "can not alloc buffer");
			if (buf !=  mempool->dummy_buf)
				fsm_dp_set_buf_state(buf,
					FSM_DP_BUF_STATE_KERNEL_ALLOC_RECV_DMA);
			/* link all buffers */
			buf_cntrl = buf - sizeof(struct fsm_dp_buf_cntrl);
			if (!first_buf_cntrl)
				first_buf_cntrl = buf_cntrl;
			else
				prev_buf_cntrl->next = buf_cntrl;
			prev_buf_cntrl = buf_cntrl;

			mhi->ul_buf_array[i].buf = buf;
			mhi->ul_buf_array[i].len = mempool->mem.buf_sz;
			mhi->ul_flag_array[i] = MHI_EOT;
			if (mempool->mem.loc.dma_mapped &&
					buf != mempool->dummy_buf) {

				mhi->ul_buf_array[i].dma_addr =
					mempool->mem.loc.cluster_dma_addr
							[cluster] + c_offset;
				mhi->ul_buf_array[i].streaming_dma = true;
			} else {
				mhi->ul_buf_array[i].dma_addr = 0;
				mhi->ul_buf_array[i].streaming_dma = false;
			}
		}
		ret = mhi_queue_n_dma(mhi_dev,
				      DMA_FROM_DEVICE,
				      mhi->ul_buf_array,
				      mhi->ul_flag_array,
				      to_xfer);
		if (ret) {
			for (i = 0; i < to_xfer; i++) {
				if (mhi->ul_buf_array[i].buf !=
					mempool->dummy_buf) {
					fsm_dp_set_buf_state(
						mhi->ul_buf_array[i].buf,
						FSM_DP_BUF_STATE_KERNEL_FREE);
					fsm_dp_mempool_put_buf(mempool,
						mhi->ul_buf_array[i].buf);
				}
			}
			mhi->stats.rx_replenish_err++;
			FSM_DP_ERROR("%s: failed to load rx buf!\n",
				  __func__);
			return ret;
		}
		mhi->stats.rx_replenish++;
		if (outofbuf) {
			ret = -ENOMEM;
			break;
		}
		nr -= to_xfer;
	}

	if (!drv->rx_tail_buf_cntrl) {
		/* first repelenish (after probe) */
		buf_cntrl->next = first_buf_cntrl;
		drv->rx_head_buf_cntrl = first_buf_cntrl;
	} else {
		drv->rx_tail_buf_cntrl->next = first_buf_cntrl;
		buf_cntrl->next = drv->rx_head_buf_cntrl;
	}
	drv->rx_tail_buf_cntrl = buf_cntrl;

	return ret;
}

static void __mhi_ul_skb_xfer_cmplt(struct sk_buff *skb)
{
	struct fsm_dp_msghdr *msghdr;
	struct fsm_dp_kernel_register_db_entry *preg;

	msghdr = (struct fsm_dp_msghdr *)skb->data;
	preg = fsm_dp_find_reg_db_type(msghdr->type);
	if (!preg || !preg->tx_cmplt_cb) {
		kfree_skb(skb);
		return;
	}
	skb_pull(skb, sizeof(*msghdr));
	preg->tx_cmplt_cb(skb);
}

/* TX complete */
static void __mhi_ul_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct fsm_dp_drv *drv = dev_get_drvdata(&mhi_dev->dev);
	struct fsm_dp_mhi *mhi = &drv->mhi;
	void *addr = result->buf_addr;
	struct fsm_dp_mempool *mempool;
	unsigned int cl;
	struct fsm_dp_buf_cntrl *buf_cntrl;

	FSM_DP_DEBUG("%s: ul_xfer_result addr=%p dir=%u bytes=%lu status=%d\n",
		     __func__, result->buf_addr, result->dir,
		     result->bytes_xferd, result->transaction_status);

	fsm_dp_hex_dump(result->buf_addr, result->bytes_xferd);
	mhi->stats.tx_acked++;

	buf_cntrl = addr - sizeof(struct fsm_dp_buf_cntrl);
	while (buf_cntrl) {
		/* Try DL mempool first */
		mempool = fsm_dp_find_mempool(drv, addr, true, &cl);

		/* Try UL mempool for loopback packet */
		if (mempool == NULL)
			mempool = fsm_dp_find_mempool(drv, addr, false, &cl);

		if (unlikely(mempool == NULL)) {
			FSM_DP_ERROR("%s: cannot find mempool, addr=%p\n",
				  __func__, addr);
			return;
		}

		if (mempool->signature != FSM_DP_MEMPOOL_SIG) {
			FSM_DP_ERROR("%s: mempool %p signature 0x%x error, expect 0x%x\n",
				  __func__, mempool, mempool->signature, FSM_DP_MEMPOOL_SIG);
			return;
		}

		if (atomic_read(&mempool->out_xmit) == 0) {
			FSM_DP_ERROR("%s: mempool %p out xmit cnt should not be zero\n",
				  __func__, mempool);
			return;
		}

		atomic_dec(&mempool->out_xmit);

		switch (mempool->type) {
		case FSM_DP_MEM_TYPE_UL:
			fsm_dp_mempool_put_buf(mempool, addr); /* rx loop back */
			break;
		default:
#ifdef FSM_DP_BUFFER_FENCING
			if (buf_cntrl->state == FSM_DP_BUF_STATE_KERNEL_XMIT_DMA)
				buf_cntrl->state =
					FSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP;
			buf_cntrl->xmit_status = FSM_DP_XMIT_OK;
			wmb(); /* make it visible to other CPU */
#endif
			break;
		}

		buf_cntrl = buf_cntrl->next;
		addr = buf_cntrl + 1;
	}
}

/* RX */
static void __mhi_dl_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct fsm_dp_drv *drv = dev_get_drvdata(&mhi_dev->dev);
	struct fsm_dp_mhi *mhi = &drv->mhi;
	struct fsm_dp_mempool *mempool = drv->mempool[FSM_DP_MEM_TYPE_UL];
	struct fsm_dp_buf_cntrl *packet_start, *packet_end, *prev_buf_cntrl = NULL;

	FSM_DP_DEBUG("%s: dl_xfer_result addr=%p dir=%u bytes=%lu status=%d\n",
		  __func__, result->buf_addr, result->dir,
		  result->bytes_xferd, result->transaction_status);

	fsm_dp_hex_dump(result->buf_addr, result->bytes_xferd);

	if (result->buf_addr == mempool->dummy_buf) {
		mhi->stats.rx_outofbuf_drop++;

		fsm_dp_mem_ul_ring_sync(drv);

		return;
	}
	drv->fsm_dp_outbuf_drop_sync = 0;

	packet_start = drv->rx_head_buf_cntrl;
	packet_end = result->buf_addr - sizeof(struct fsm_dp_buf_cntrl);
	for (; drv->rx_head_buf_cntrl != drv->rx_tail_buf_cntrl;
	     drv->rx_head_buf_cntrl = drv->rx_head_buf_cntrl->next) {
		if (prev_buf_cntrl)
			prev_buf_cntrl->next_buf_index = drv->rx_head_buf_cntrl->buf_index;
		prev_buf_cntrl = drv->rx_head_buf_cntrl;
		if (drv->rx_head_buf_cntrl != packet_end) {
			drv->rx_head_buf_cntrl->len = mempool->mem.buf_sz;
			continue;
		}

		/* reached end of packet */
		drv->rx_head_buf_cntrl = packet_end->next;
		packet_end->next = NULL;
		packet_end->next_buf_index = FSM_DP_INVALID_BUF_INDEX;
		packet_end->len = (result->bytes_xferd % mempool->mem.buf_sz);
		if (result->transaction_status == -ENOTCONN) {
			mhi->stats.rx_err++;
			for (; packet_start; packet_start = packet_start->next)
				fsm_dp_mempool_put_buf(mempool, packet_start + 1);
			return;
		}
		{
			/* remove msghdr from 1st buffer (next buffers don't have msghdr) */
			void *buf = packet_start + 1;

			FSM_DP_ASSERT(result->bytes_xferd <= sizeof(struct fsm_dp_msghdr),
					"invalid bytes_xferd");
			memmove(buf,
				buf + sizeof(struct fsm_dp_msghdr),
				min_t(unsigned int, result->bytes_xferd, mempool->mem.buf_sz)
					- sizeof(struct fsm_dp_msghdr));
			packet_start->len -= sizeof(struct fsm_dp_msghdr);
		}

		mhi->stats.rx_cnt++;
		fsm_dp_rx(drv, packet_start, result->bytes_xferd);

		return;
	}

	FSM_DP_ERROR("couldn't find end of packet, buf_addr 0x%p", result->buf_addr);
}

static void __mhi_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb)
{

	struct fsm_dp_drv *pdrv = dev_get_drvdata(&mhi_dev->dev);

	switch (mhi_cb) {
	/* TODO: find a replacement for MHI_CB_DEVICE_DESTROYED */
	case MHI_CB_PENDING_DATA:
		if (napi_schedule_prep(&pdrv->napi)) {
			__napi_schedule(&pdrv->napi);
			pdrv->stats.rx_int++;
		}
		break;
	default:
		break;
	}
}

int fsm_dp_mhi_rx_replenish(struct fsm_dp_drv *drv)
{
	struct fsm_dp_mhi *mhi = &drv->mhi;
	struct fsm_dp_mempool *mempool = drv->mempool[FSM_DP_MEM_TYPE_UL];
	int ret;

	spin_lock_bh(&mhi->rx_lock);
	ret = __mhi_rx_replenish(drv, mempool);
	spin_unlock_bh(&mhi->rx_lock);
	return ret;
}


static int fsm_dp_mhi_probe(
	struct mhi_device *mhi_dev,
	const struct mhi_device_id *id)
{
	struct fsm_dp_drv *pdrv = __pdrv;
	int ret;

	FSM_DP_DEBUG("%s: probing mhi\n", __func__);

	if (__pdrv == NULL)
		return -ENODEV;

	dev_set_drvdata(&mhi_dev->dev, pdrv);

	ret = mhi_prepare_for_transfer(mhi_dev, 0);
	if (ret) {
		FSM_DP_ERROR("%s: mhi_prepare_for_transfer failed\n", __func__);
		return ret;
	}

	pdrv->mhi.mhi_dev = mhi_dev;
	pdrv->mhi.mhi_destroyed = false;
	spin_lock_init(&pdrv->mhi.rx_lock);
	spin_lock_init(&pdrv->mhi.tx_lock);

	FSM_DP_INFO("%s: fsm_dp_mhi_rx_replenish\n", __func__);
	if (pdrv->mempool[FSM_DP_MEM_TYPE_UL]) {
		FSM_DP_INFO("%s: fsm_dp_mempool_dma_map FSM_DP_MEM_TYPE_UL "
			"pool , ret %d\n", __func__,
			fsm_dp_mempool_dma_map(pdrv,
				pdrv->mempool[FSM_DP_MEM_TYPE_UL],
				FSM_DP_MEM_TYPE_UL));
		ret = fsm_dp_mhi_rx_replenish(pdrv);
		if (ret) {
			FSM_DP_ERROR("%s: fsm_dp_mhi_rx_replenish failed\n",
								__func__);
			return ret;
		}
	}
	FSM_DP_DEBUG("%s: mhi_probed\n", __func__);
	return 0;
}

static void fsm_dp_mhi_remove(struct mhi_device *mhi_dev)
{
	mhi_unprepare_from_transfer(mhi_dev);
}

static struct mhi_device_id fsm_dp_mhi_match_table[] = {
	{ .chan = "IP_HW0" },
	{},
};

static struct mhi_driver __fsm_dp_mhi_drv = {
	.id_table = fsm_dp_mhi_match_table,
	.remove = fsm_dp_mhi_remove,
	.probe = fsm_dp_mhi_probe,
	.ul_xfer_cb = __mhi_ul_xfer_cb,
	.dl_xfer_cb = __mhi_dl_xfer_cb,
	.status_cb = __mhi_status_cb,
	.driver = {
		.name = FSM_DP_MHI_NAME,
		.owner = THIS_MODULE,
	},
};


int fsm_dp_mhi_init(struct fsm_dp_drv *pdrv)
{
	int ret = -EBUSY;

	if (__pdrv == NULL) {
		__pdrv = pdrv;
		ret = mhi_driver_register(&__fsm_dp_mhi_drv);
		if (ret) {
			__pdrv = NULL;
			pr_err("FSM-DP: mhi registration failed!\n");
			return ret;
		}

		pr_info("FSM-DP: Register MHI driver!\n");
	}
	return ret;
}

void fsm_dp_mhi_cleanup(struct fsm_dp_drv *pdrv)
{
	if (__pdrv) {
		mhi_driver_unregister(&__fsm_dp_mhi_drv);
		__pdrv = NULL;
		pr_info("FSM-DP: Unregister MHI driver\n");
	}
}
