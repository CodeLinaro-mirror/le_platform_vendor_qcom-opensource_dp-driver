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
	struct fsm_dp_mhi *mhi)
{
	struct mhi_device *mhi_dev = mhi->mhi_dev;
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct fsm_dp_mempool *mempool;
	int nr = mhi_get_free_desc_count(mhi_dev, DMA_FROM_DEVICE);
	void *buf;
	int ret, i, to_xfer;
	bool outofbuf, is_control = (mhi_dev->id->driver_data == FSM_DP_CH_CONTROL);
	unsigned int cluster, c_offset;
	struct fsm_dp_buf_cntrl *first_buf_cntrl = NULL, *buf_cntrl = NULL, *prev_buf_cntrl = NULL;

	mempool = is_control ? pdev->mempool[FSM_DP_MEM_TYPE_UL_CONTROL] :
			       pdev->mempool[FSM_DP_MEM_TYPE_UL_DATA];

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
			if (is_control)
				mhi->ul_flag_array[i] = MHI_EOT;
			else
				mhi->ul_flag_array[i] = MHI_EOT | MHI_BEI;
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

	if (!mhi->rx_tail_buf_cntrl) {
		/* first repelenish (after probe) */
		buf_cntrl->next = first_buf_cntrl;
		mhi->rx_head_buf_cntrl = first_buf_cntrl;
	} else {
		mhi->rx_tail_buf_cntrl->next = first_buf_cntrl;
		buf_cntrl->next = mhi->rx_head_buf_cntrl;
	}
	mhi->rx_tail_buf_cntrl = buf_cntrl;

	return ret;
}

/*
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
*/

static struct fsm_dp_mhi *get_dp_mhi(struct mhi_device *mhi_dev)
{
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);

	switch (mhi_dev->id->driver_data) {
	case FSM_DP_CH_CONTROL:
		return &pdev->mhi_control_dev;
	case FSM_DP_CH_DATA:
		return &pdev->mhi_data_dev;
	default:
		FSM_DP_ASSERT(0, "invalid mhi_dev->id->driver_data");
		return NULL;
	}
}

/* TX complete */
static void __mhi_ul_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct fsm_dp_mhi *mhi = get_dp_mhi(mhi_dev);
	void *addr = result->buf_addr;
	struct fsm_dp_mempool *mempool;
	struct fsm_dp_buf_cntrl *buf_cntrl;

	FSM_DP_DEBUG("%s: ul_xfer_result (TX complete) addr=%p dir=%u bytes=%lu status=%d\n",
		     __func__, result->buf_addr, result->dir,
		     result->bytes_xferd, result->transaction_status);

	fsm_dp_hex_dump(result->buf_addr, result->bytes_xferd);
	mhi->stats.tx_acked++;

	buf_cntrl = addr - sizeof(struct fsm_dp_buf_cntrl);
	while (buf_cntrl) {
		mempool = fsm_dp_get_mempool(pdev, buf_cntrl, NULL);
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
		case FSM_DP_MEM_TYPE_UL_CONTROL:
		case FSM_DP_MEM_TYPE_UL_DATA:
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
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct fsm_dp_mhi *mhi = get_dp_mhi(mhi_dev);
	struct fsm_dp_mempool *mempool;
	struct fsm_dp_buf_cntrl *packet_start, *packet_end, *prev_buf_cntrl = NULL;
	bool is_control = (mhi_dev->id->driver_data == FSM_DP_CH_CONTROL);
	unsigned int buf_count = 0;

	mempool = is_control ? pdev->mempool[FSM_DP_MEM_TYPE_UL_CONTROL] :
			       pdev->mempool[FSM_DP_MEM_TYPE_UL_DATA];

	FSM_DP_DEBUG("%s: dl_xfer_result (RX) addr=%p dir=%u bytes=%lu status=%d\n",
		  __func__, result->buf_addr, result->dir,
		  result->bytes_xferd, result->transaction_status);

	fsm_dp_hex_dump(result->buf_addr, result->bytes_xferd);

	if (result->buf_addr == mempool->dummy_buf) {
		mhi->stats.rx_outofbuf_drop++;

		if (fsm_dp_mem_ul_ring_sync(pdev))
			mhi->stats.rx_resync++;

		return;
	}
	pdev->fsm_dp_outbuf_drop_sync = 0;

	packet_start = mhi->rx_head_buf_cntrl;
	packet_end = result->buf_addr - sizeof(struct fsm_dp_buf_cntrl);
	for (; mhi->rx_head_buf_cntrl != mhi->rx_tail_buf_cntrl;
	     mhi->rx_head_buf_cntrl = mhi->rx_head_buf_cntrl->next) {
		buf_count++;
		if (prev_buf_cntrl)
			prev_buf_cntrl->next_buf_index = mhi->rx_head_buf_cntrl->buf_index;
		prev_buf_cntrl = mhi->rx_head_buf_cntrl;
		if (mhi->rx_head_buf_cntrl != packet_end) {
			mhi->rx_head_buf_cntrl->len = mempool->mem.buf_sz;
			continue;
		}

		/* reached end of packet */
		mhi->rx_head_buf_cntrl = packet_end->next;
		packet_start->buf_count = buf_count;
		packet_end->next = NULL;
		packet_end->next_buf_index = FSM_DP_INVALID_BUF_INDEX;
		packet_end->len = (result->bytes_xferd % mempool->mem.buf_sz);

		if (result->transaction_status == -ENOTCONN) {
			mhi->stats.rx_err++;
			for (; packet_start; packet_start = packet_start->next)
				fsm_dp_mempool_put_buf(mempool, packet_start + 1);
			return;
		}

		mhi->stats.rx_cnt++;
		fsm_dp_rx(pdev, packet_start, result->bytes_xferd);

		return;
	}

	FSM_DP_ERROR("couldn't find end of packet, buf_addr 0x%p", result->buf_addr);
}

static void __mhi_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb)
{

	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);

	switch (mhi_cb) {
	/* TODO: find a replacement for MHI_CB_DEVICE_DESTROYED */
	case MHI_CB_PENDING_DATA:
		if (napi_schedule_prep(&pdev->napi)) {
			__napi_schedule(&pdev->napi);
			pdev->stats.rx_int++;
		}
		break;
	default:
		break;
	}
}

int fsm_dp_mhi_rx_replenish(struct fsm_dp_mhi *mhi)
{
	int ret;

	spin_lock_bh(&mhi->rx_lock);
	ret = __mhi_rx_replenish(mhi);
	spin_unlock_bh(&mhi->rx_lock);
	return ret;
}

/* This is Tx polling thread - polling for Tx completions */
static int fsm_dp_mhi_tx_poll_thread(void *data)
{
	struct mhi_device *mhi_dev = data;
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	int ret;

	while (!kthread_should_stop()) {
		wait_for_completion(&pdev->mhi_data_dev.poll_comp);
		ret = mhi_poll(mhi_dev, FSM_DP_NAPI_WEIGHT, DMA_TO_DEVICE);
		if (ret < 0)
			pr_err("Error polling ret:%d\n", ret);
	}

	return 0;
}

enum hrtimer_restart fsm_dp_mhi_poll_timer_handler(struct hrtimer *timer)
{
	struct fsm_dp_mhi *mhi = container_of(timer, struct fsm_dp_mhi, poll_timer);

	complete(&mhi->poll_comp);

	hrtimer_forward_now(&mhi->poll_timer, ktime_set(0, 1000000)); /* 1ms */

	return HRTIMER_RESTART;
}

static int fsm_dp_mhi_probe(
	struct mhi_device *mhi_dev,
	const struct mhi_device_id *id)
{
	struct fsm_dp_dev *pdev;
	int ret;
	struct fsm_dp_mhi *mhi;
	struct fsm_dp_mempool *mempool;

	FSM_DP_DEBUG("%s: probing mhi chan %s driver_data %ld\n",
		     __func__, id->chan, id->driver_data);

	if (__pdrv == NULL)
		return -ENODEV;

	pdev = &__pdrv->dp_devs[0];
	ret = fsm_dp_cdev_add(pdev);
	if (ret)
		return ret;

	switch (id->driver_data) {
	case FSM_DP_CH_CONTROL:
		mhi = &pdev->mhi_control_dev;
		mempool = pdev->mempool[FSM_DP_MEM_TYPE_UL_CONTROL];
		break;
	case FSM_DP_CH_DATA:
		mhi = &pdev->mhi_data_dev;
		mempool = pdev->mempool[FSM_DP_MEM_TYPE_UL_DATA];
		break;
	default:
		FSM_DP_ERROR("%s: unexpected driver_data %ld\n", __func__, id->driver_data);
		ret = -EINVAL;
		goto err;
	}

	dev_set_drvdata(&mhi_dev->dev, pdev);

	ret = mhi_prepare_for_transfer(mhi_dev, 0);
	if (ret) {
		FSM_DP_ERROR("%s: mhi_prepare_for_transfer failed\n", __func__);
		goto err;
	}

	mhi->mhi_dev = mhi_dev;
	mhi->mhi_destroyed = false;
	spin_lock_init(&mhi->rx_lock);
	spin_lock_init(&mhi->tx_lock);

	FSM_DP_INFO("%s: fsm_dp_mhi_rx_replenish\n", __func__);
	if (mempool) {
		ret = fsm_dp_mempool_dma_map(mhi_dev->mhi_cntrl->cntrl_dev, mempool);
		FSM_DP_INFO("%s: fsm_dp_mempool_dma_map pool type %d, ret %d\n",
			    __func__, mempool->type, ret);
		/* TODO: check ret */

		ret = fsm_dp_mhi_rx_replenish(mhi);
		if (ret) {
			FSM_DP_ERROR("%s: fsm_dp_mhi_rx_replenish failed\n",
								__func__);
			goto err;
		}
	}

	if (id->driver_data == FSM_DP_CH_DATA) {
		/* Data channel specific initialization - Tx and Rx polling */
		init_completion(&mhi->poll_comp);
		mhi->tx_poll_thread = kthread_run(fsm_dp_mhi_tx_poll_thread, mhi_dev,
						  "fsm_dp_mhi_tx_poll");
		if (IS_ERR(mhi->tx_poll_thread))
			FSM_DP_WARN("%s: failed to start tx poll thread\n", __func__);

		hrtimer_init(&mhi->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		mhi->poll_timer.function = fsm_dp_mhi_poll_timer_handler;
		 /* start in 1ms */
		hrtimer_start(&mhi->poll_timer, ktime_set(0, 1000000), HRTIMER_MODE_REL);
	}

	FSM_DP_DEBUG("%s: mhi_probed\n", __func__);
	return 0;

err:
	fsm_dp_cdev_del(pdev);
	return ret;
}

static void fsm_dp_mhi_remove(struct mhi_device *mhi_dev)
{
	struct fsm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);

	if (mhi_dev->id->driver_data == FSM_DP_CH_DATA) {
		kthread_stop(pdev->mhi_data_dev.tx_poll_thread);
		hrtimer_cancel(&pdev->mhi_data_dev.poll_timer);
	}

	mhi_unprepare_from_transfer(mhi_dev);

	fsm_dp_cdev_del(pdev);
}

static struct mhi_device_id fsm_dp_mhi_match_table[] = {
	{ .chan = "IP_HW0", .driver_data = FSM_DP_CH_DATA },
	{ .chan = "IP_HW1", .driver_data = FSM_DP_CH_CONTROL },
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
