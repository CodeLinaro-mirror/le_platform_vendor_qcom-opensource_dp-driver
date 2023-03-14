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

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mod_devicetable.h>

#include "csm_dp.h"
#include "csm_dp_mhi.h"

static struct csm_dp_drv *__pdrv;

#define TX_POLL_INTERVAL_NS 500000 /* 0.5ms */
static uint tx_poll_interval_ns = TX_POLL_INTERVAL_NS;
module_param(tx_poll_interval_ns, uint, 0644);
MODULE_PARM_DESC(tx_poll_interval_ns, " Tx poll interval in nanosec, default 500000");

/*
 * Dump a packet.
 */
#define CSM_DP_HEX_DUMP_BUF_SIZE (32 * 3 + 2 + 32 + 1)
static void csm_dp_print_hex_dump(const char *level,
			const char *prefix_str, int prefix_type,
			int rowsize, int groupsize,
			const void *buf, size_t len, bool ascii)
{
	const u8 *ptr = buf;
	int i, linelen, remaining = len;
	unsigned char linebuf[CSM_DP_HEX_DUMP_BUF_SIZE];

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
void csm_dp_hex_dump(unsigned char *buf, unsigned int len)
{
	if (do_dump)
		csm_dp_print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, buf, len, false);
}
EXPORT_SYMBOL(csm_dp_hex_dump);

static int __mhi_rx_replenish(
	struct csm_dp_mhi *mhi)
{
	struct mhi_device *mhi_dev = mhi->mhi_dev;
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct csm_dp_mempool *mempool;
	int nr = mhi_get_free_desc_count(mhi_dev, DMA_FROM_DEVICE);
	void *buf;
	int ret, i, to_xfer;
	bool is_control = (mhi_dev->id->driver_data == CSM_DP_CH_CONTROL);
	unsigned int cluster, c_offset;
	struct csm_dp_buf_cntrl *first_buf_cntrl = NULL, *buf_cntrl = NULL, *prev_buf_cntrl = NULL;

	mempool = is_control ? pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL] :
			       pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA];

	ret = 0;
	if (nr < mhi_get_total_descriptors(mhi_dev, DMA_FROM_DEVICE) / 8)
		return ret;
	for (; nr > 0;) {
		to_xfer = min(CSM_DP_MAX_IOV_SIZE, nr);
		for (i = 0; i < to_xfer; i++) {
			buf = csm_dp_mempool_get_buf(mempool, &cluster,
								&c_offset);
			if (buf == NULL) {
				mhi->stats.rx_out_of_buf++;
				CSM_DP_DEBUG("%s: out of rx buffer (nr %d to_xfer %d)!\n", __func__, nr, to_xfer);
				to_xfer = i;
				ret = -ENOMEM;
				goto err;
			}
			csm_dp_set_buf_state(buf, CSM_DP_BUF_STATE_KERNEL_ALLOC_RECV_DMA);
			/* link all buffers */
			buf_cntrl = buf - sizeof(struct csm_dp_buf_cntrl);
			if (!first_buf_cntrl)
				first_buf_cntrl = buf_cntrl;
			else
				prev_buf_cntrl->next = buf_cntrl;
			prev_buf_cntrl = buf_cntrl;

			mhi->ul_buf_array[i].buf = buf;
			mhi->ul_buf_array[i].len = mempool->mem.buf_sz;
			mhi->ul_flag_array[i] = MHI_EOT | MHI_SG;
			if (!is_control)
				mhi->ul_flag_array[i] |= MHI_BEI;
			if (mempool->mem.loc.dma_mapped) {

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
		if (ret)
			goto err;

		/* update rx head/tail */
		if (!mhi->rx_tail_buf_cntrl) {
			/* first repelenish (after probe) */
			buf_cntrl->next = first_buf_cntrl;
			mhi->rx_head_buf_cntrl = first_buf_cntrl;
		} else {
			mhi->rx_tail_buf_cntrl->next = first_buf_cntrl;
			buf_cntrl->next = mhi->rx_head_buf_cntrl;
		}
		mhi->rx_tail_buf_cntrl = buf_cntrl;
		first_buf_cntrl = NULL;

		mhi->stats.rx_replenish++;
		nr -= to_xfer;
	}

	return ret;

err:
	for (i = 0; i < to_xfer; i++) {
		csm_dp_set_buf_state(
			mhi->ul_buf_array[i].buf,
			CSM_DP_BUF_STATE_KERNEL_FREE);
		csm_dp_mempool_put_buf(mempool,
			mhi->ul_buf_array[i].buf);
	}
	mhi->stats.rx_replenish_err++;
	CSM_DP_ERROR("%s: failed to load rx buf!\n",
			__func__);
	return ret;
}

static struct csm_dp_mhi *get_dp_mhi(struct mhi_device *mhi_dev)
{
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);

	switch (mhi_dev->id->driver_data) {
	case CSM_DP_CH_CONTROL:
		return &pdev->mhi_control_dev;
	case CSM_DP_CH_DATA:
		return &pdev->mhi_data_dev;
	default:
		CSM_DP_ASSERT(0, "invalid mhi_dev->id->driver_data");
		return NULL;
	}
}

/* TX complete */
static void __mhi_ul_xfer_cb(
	struct mhi_device *mhi_dev,
	struct mhi_result *result)
{
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct csm_dp_mhi *mhi = get_dp_mhi(mhi_dev);
	void *addr = result->buf_addr;
	struct csm_dp_mempool *mempool;
	struct csm_dp_buf_cntrl *buf_cntrl;

	CSM_DP_DEBUG("%s: (TX complete) ch %s bus %d VF %d addr=%p bytes=%lu status=%d\n",
		     __func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
		     result->bytes_xferd, result->transaction_status);

	csm_dp_hex_dump(result->buf_addr, result->bytes_xferd);
	mhi->stats.tx_acked++;

	buf_cntrl = addr - sizeof(struct csm_dp_buf_cntrl);
	while (buf_cntrl) {
		mempool = csm_dp_get_mempool(pdev, buf_cntrl, NULL);
		if (unlikely(mempool == NULL)) {
			CSM_DP_ERROR("%s: cannot find mempool, addr=%p\n",
				  __func__, addr);
			return;
		}

		if (mempool->signature != CSM_DP_MEMPOOL_SIG) {
			CSM_DP_ERROR("%s: mempool %p signature 0x%x error, expect 0x%x\n",
				  __func__, mempool, mempool->signature, CSM_DP_MEMPOOL_SIG);
			return;
		}

		if (atomic_read(&mempool->out_xmit) == 0) {
			CSM_DP_ERROR("%s: mempool %p out xmit cnt should not be zero\n",
				  __func__, mempool);
			return;
		}

		atomic_dec(&mempool->out_xmit);

		switch (mempool->type) {
		case CSM_DP_MEM_TYPE_UL_CONTROL:
		case CSM_DP_MEM_TYPE_UL_DATA:
			CSM_DP_ERROR("unexpected mempool %d\n", mempool->type);
			break;
		default:
#ifdef CSM_DP_BUFFER_FENCING
			if (buf_cntrl->state == CSM_DP_BUF_STATE_KERNEL_XMIT_DMA)
				buf_cntrl->state =
					CSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP;
			buf_cntrl->xmit_status = CSM_DP_XMIT_OK;
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
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct csm_dp_mhi *mhi = get_dp_mhi(mhi_dev);
	struct csm_dp_mempool *mempool;
	struct csm_dp_buf_cntrl *packet_start, *packet_end, *prev_buf_cntrl = NULL;
	bool is_control = (mhi_dev->id->driver_data == CSM_DP_CH_CONTROL);
	unsigned int buf_count = 0;

	mempool = is_control ? pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL] :
			       pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA];
	if (!mempool) {
		/* getting here with transaction_status == -ENOTCONN is an expected situation while
		 * mhi devices gets removed: mempool got released part of 1st mhi device removal
		 * (e.g. CONTROL) and we now get RX complete for 2nd mhi device (e.g. DATA).
		 */
		if (result->transaction_status != -ENOTCONN)
			pr_err_ratelimited("%s: no mempool (ch %s bus %d VF %d status %d)\n",
				__func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num,
				result->transaction_status);
		return;
	}

	CSM_DP_DEBUG("%s: (RX) ch %s bus %d VF %d addr=%p bytes=%lu status=%d\n",
		  __func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
		  result->bytes_xferd, result->transaction_status);

	csm_dp_hex_dump(result->buf_addr, result->bytes_xferd);

	if (result->transaction_status == -EOVERFLOW) {
		CSM_DP_DEBUG("%s: overflow event ignored\n", __func__);
		return;
	}

	while (!mhi->rx_tail_buf_cntrl) {
		CSM_DP_DEBUG("%s: waiting for probe to complete\n", __func__);
		udelay(100);
	}

	packet_start = mhi->rx_head_buf_cntrl;
	packet_end = result->buf_addr - sizeof(struct csm_dp_buf_cntrl);
	for (; mhi->rx_head_buf_cntrl != mhi->rx_tail_buf_cntrl;
	     mhi->rx_head_buf_cntrl = mhi->rx_head_buf_cntrl->next) {
		buf_count++;
		if (prev_buf_cntrl)
			prev_buf_cntrl->next_buf_index = mhi->rx_head_buf_cntrl->buf_index;
		prev_buf_cntrl = mhi->rx_head_buf_cntrl;
		if (mhi->rx_head_buf_cntrl != packet_end) {
			mhi->rx_head_buf_cntrl->len = 0;	/* 0 indicates this is part of SG */
			continue;
		}

		/* reached end of packet */
		mhi->rx_head_buf_cntrl = packet_end->next;
		packet_start->buf_count = buf_count;
		packet_end->next = NULL;
		packet_end->next_buf_index = CSM_DP_INVALID_BUF_INDEX;
		packet_end->len = result->bytes_xferd;

		if (result->transaction_status == -ENOTCONN) {
			mhi->stats.rx_err++;
			for (; packet_start; packet_start = packet_start->next)
				csm_dp_mempool_put_buf(mempool, packet_start + 1);
			return;
		}

		mhi->stats.rx_cnt++;
		csm_dp_rx(pdev, packet_start, result->bytes_xferd);

		return;
	}

	CSM_DP_ERROR("couldn't find end of packet, buf_addr 0x%p", result->buf_addr);
}

static void __mhi_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb)
{
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);

	switch (mhi_cb) {
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

int csm_dp_mhi_rx_replenish(struct csm_dp_mhi *mhi)
{
	int ret;

	spin_lock_bh(&mhi->rx_lock);
	ret = __mhi_rx_replenish(mhi);
	spin_unlock_bh(&mhi->rx_lock);
	return ret;
}

/* This is Tx polling thread - polling for Tx completions */
static int csm_dp_mhi_tx_poll_thread(void *data)
{
	struct mhi_device *mhi_dev = data;
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	int ret;

	while (!kthread_should_stop()) {
		wait_for_completion(&pdev->mhi_data_dev.poll_comp);
		do {
			ret = mhi_poll(mhi_dev, CSM_DP_NAPI_WEIGHT, DMA_TO_DEVICE);
			if (ret < 0)
				pr_err_ratelimited("Error Tx polling ret:%d\n", ret);
		} while (ret == CSM_DP_NAPI_WEIGHT);
	}

	return 0;
}

enum hrtimer_restart csm_dp_mhi_poll_timer_handler(struct hrtimer *timer)
{
	struct csm_dp_mhi *mhi = container_of(timer, struct csm_dp_mhi, poll_timer);

	complete(&mhi->poll_comp);

	hrtimer_forward_now(&mhi->poll_timer, ktime_set(0, tx_poll_interval_ns));

	return HRTIMER_RESTART;
}

static int csm_dp_mhi_probe(
	struct mhi_device *mhi_dev,
	const struct mhi_device_id *id)
{
	struct csm_dp_dev *pdev;
	int ret;
	struct csm_dp_mhi *mhi;
	struct csm_dp_mempool *mempool;
	unsigned int bus_num, vf_num;

	CSM_DP_INFO("%s: probing mhi chan %s driver_data %ld\n",
		     __func__, id->chan, id->driver_data);

	if (__pdrv == NULL)
		return -ENODEV;

	bus_num = mhi_get_device_bus_number(mhi_dev->mhi_cntrl);
	vf_num = mhi_get_device_instance_id(mhi_dev->mhi_cntrl);
	CSM_DP_INFO("%s: bus %d VF %d\n", __func__, bus_num, vf_num);
	if (vf_num < 0) {
		/* SR-IOV disabled, create single device node */
		pdev = &__pdrv->dp_devs[bus_num * CSM_DP_MAX_NUM_VFS];
	} else if (vf_num == 0) {
		/* SR-IOV enabled, PF device: ignore */
		return 0;
	} else if (bus_num >= CSM_DP_MAX_NUM_BUSES || vf_num > CSM_DP_MAX_NUM_VFS) {
		/* invalid ids */
		CSM_DP_ERROR("%s: invalid ids bus_num %d vf_num %d\n", __func__, bus_num, vf_num);
		return -EINVAL;
	} else {
		/* SR-IOV enabled, VF device. bus_num is 0..11, vf_num is 1..4 */
		pdev = &__pdrv->dp_devs[bus_num * CSM_DP_MAX_NUM_VFS + vf_num - 1];
	}

	if (!pdev->cdev_inited) {
		pdev->bus_num = bus_num;
		pdev->vf_num = vf_num;
		ret = csm_dp_cdev_add(pdev, &mhi_dev->dev);
		if (ret)
			return ret;
	}

	switch (id->driver_data) {
	case CSM_DP_CH_CONTROL:
		mhi = &pdev->mhi_control_dev;
		mempool = pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL];
		break;
	case CSM_DP_CH_DATA:
		mhi = &pdev->mhi_data_dev;
		mempool = pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA];
		break;
	default:
		CSM_DP_ERROR("%s: unexpected driver_data %ld\n", __func__, id->driver_data);
		ret = -EINVAL;
		goto err;
	}

	dev_set_drvdata(&mhi_dev->dev, pdev);

	ret = mhi_prepare_for_transfer(mhi_dev, 0);
	if (ret) {
		CSM_DP_ERROR("%s: mhi_prepare_for_transfer failed\n", __func__);
		goto err;
	}

	mhi->mhi_dev = mhi_dev;
	mhi->mhi_dev_destroyed = false;
	atomic_set(&mhi->mhi_dev_refcnt, 0);
	spin_lock_init(&mhi->rx_lock);
	spin_lock_init(&mhi->tx_lock);
	mhi->rx_head_buf_cntrl = NULL;
	mhi->rx_tail_buf_cntrl = NULL;

	CSM_DP_DEBUG("%s: csm_dp_mhi_rx_replenish\n", __func__);
	if (mempool) {
		ret = csm_dp_mempool_dma_map(mhi_dev->mhi_cntrl->cntrl_dev, mempool);
		if (ret) {
			CSM_DP_ERROR("%s: dma_map failed, mempool type %d ret %d\n", __func__, mempool->type, ret);
			goto err;
		}

		ret = csm_dp_mhi_rx_replenish(mhi);
		if (ret) {
			CSM_DP_ERROR("%s: csm_dp_mhi_rx_replenish failed\n", __func__);
			goto err;
		}
	}

	if (id->driver_data == CSM_DP_CH_DATA) {
		/* Data channel specific initialization - Tx and Rx polling */
		init_completion(&mhi->poll_comp);
		mhi->tx_poll_thread = kthread_run(csm_dp_mhi_tx_poll_thread, mhi_dev,
						  "csm_dp_mhi_tx_poll");
		if (IS_ERR_OR_NULL(mhi->tx_poll_thread))
			CSM_DP_WARN("%s: failed to start tx poll thread\n", __func__);

		hrtimer_init(&mhi->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		mhi->poll_timer.function = csm_dp_mhi_poll_timer_handler;
		hrtimer_start(&mhi->poll_timer, ktime_set(0, tx_poll_interval_ns), HRTIMER_MODE_REL);
	}

	CSM_DP_DEBUG("%s: mhi_probed\n", __func__);
	return 0;

err:
	csm_dp_cdev_del(pdev);
	return ret;
}

static void csm_dp_mhi_remove(struct mhi_device *mhi_dev)
{
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct csm_dp_mhi *mhi;

	CSM_DP_INFO("%s: mhi chan %s driver_data %ld bus %d VF %d\n",
		__func__, mhi_dev->id->chan, mhi_dev->id->driver_data, pdev->bus_num, pdev->vf_num);

	switch (mhi_dev->id->driver_data) {
	case CSM_DP_CH_CONTROL:
		mhi = &pdev->mhi_control_dev;
		break;
	case CSM_DP_CH_DATA:
		mhi = &pdev->mhi_data_dev;
		kthread_stop(mhi->tx_poll_thread);
		hrtimer_cancel(&mhi->poll_timer);
		break;
	default:
		CSM_DP_ERROR("%s: unexpected driver_data %ld\n", __func__, mhi_dev->id->driver_data);
		return;
	}

	mhi_unprepare_from_transfer(mhi_dev);

	mhi->mhi_dev_destroyed = true;
	/* wait for idle mhi_dev */
	while (atomic_read(&mhi->mhi_dev_refcnt) > 0) {
		CSM_DP_DEBUG("%s: mhi_dev_refcnt %d\n", __func__, atomic_read(&mhi->mhi_dev_refcnt));
		msleep(10);
	}
	mhi->mhi_dev = NULL;
}

static struct mhi_device_id csm_dp_mhi_match_table[] = {
	{ .chan = "IP_HW0", .driver_data = CSM_DP_CH_CONTROL },
	{ .chan = "IP_HW1", .driver_data = CSM_DP_CH_DATA },
	{},
};

static struct mhi_driver __csm_dp_mhi_drv = {
	.id_table = csm_dp_mhi_match_table,
	.remove = csm_dp_mhi_remove,
	.probe = csm_dp_mhi_probe,
	.ul_xfer_cb = __mhi_ul_xfer_cb,
	.dl_xfer_cb = __mhi_dl_xfer_cb,
	.status_cb = __mhi_status_cb,
	.driver = {
		.name = CSM_DP_MHI_NAME,
		.owner = THIS_MODULE,
	},
};


int csm_dp_mhi_init(struct csm_dp_drv *pdrv)
{
	int ret = -EBUSY;

	if (__pdrv == NULL) {
		__pdrv = pdrv;
		ret = mhi_driver_register(&__csm_dp_mhi_drv);
		if (ret) {
			__pdrv = NULL;
			pr_err("CSM-DP: mhi registration failed!\n");
			return ret;
		}

		pr_info("CSM-DP: Register MHI driver!\n");
	}
	return ret;
}

void csm_dp_mhi_cleanup(struct csm_dp_drv *pdrv)
{
	if (__pdrv) {
		mhi_driver_unregister(&__csm_dp_mhi_drv);
		__pdrv = NULL;
		pr_info("CSM-DP: Unregister MHI driver\n");
	}
}
