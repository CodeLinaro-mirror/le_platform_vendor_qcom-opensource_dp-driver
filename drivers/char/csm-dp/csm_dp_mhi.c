// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
 * ​Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.​
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>

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
	struct mhi_device *mhi_dev;
	struct csm_dp_dev *pdev;
	struct csm_dp_mempool *mempool;
	int nr;
	void *buf;
	int ret = 0, i, to_xfer;
	int nr_avail_bufs, nr_total_bufs, threshold;
	bool is_control;
	unsigned int cluster, c_offset;
	struct csm_dp_buf_cntrl *first_buf_cntrl = NULL, *buf_cntrl = NULL, *prev_buf_cntrl = NULL;


	if (!mhi || !mhi->mhi_dev) {
		CSM_DP_ERROR("%s: Invalid mhi or mhi_dev\n", __func__);
		return -EINVAL;
	}

	mhi_dev = mhi->mhi_dev;
	pdev = dev_get_drvdata(&mhi_dev->dev);
	nr = mhi_get_free_desc_count(mhi_dev, DMA_FROM_DEVICE);
	is_control = (mhi_dev->id->driver_data == CSM_DP_CH_CONTROL);

	mempool = is_control ? pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL] :
			       pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA];

	if (!mempool) {
		CSM_DP_LIMIT_ERROR("%s: UL %s mempool not available\n",
			__func__, is_control ? "CONTROL" : "DATA");
		return -ENODEV;
	}

	/* Get total buffer count */
	nr_total_bufs = mempool->mem.buf_cnt;

	/* Calculate adaptive threshold */
	threshold = min(nr_total_bufs / CSM_DP_RX_BUF_ADAPTIVE_THRSHOLD_FACTOR,
		mhi_get_total_descriptors(mhi_dev, DMA_FROM_DEVICE) / CSM_DP_RX_BUF_ADAPTIVE_THRSHOLD_FACTOR);

	/* Use adaptive threshold */
	if ((threshold < 1) || (nr < threshold))
		return ret;

	/* Get available free buffers */
	nr_avail_bufs = csm_dp_mempool_get_free_count(mempool);

	/* Reject if available buffers < threshold */
	if (nr_avail_bufs < threshold)
		return ret;

	/* Limit replenishment by available buffers */
	nr = min(nr, nr_avail_bufs);

	for (; nr > 0;) {
		to_xfer = min(CSM_DP_MAX_IOV_SIZE, nr);
		for (i = 0; i < to_xfer; i++) {
			buf = csm_dp_mempool_get_buf(mempool, &cluster, &c_offset);
			if (buf == NULL) {
				mhi->stats.rx_out_of_buf++;
				CSM_DP_LIMIT_ERROR(
					"%s: out of rx buffer (nr %d to_xfer %d)! processed buffer: %d\n",
					__func__, nr, to_xfer, i);
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
	CSM_DP_LIMIT_ERROR("%s: failed to load rx buf for bus:%d VF:%d %s\n",
			__func__, pdev->bus_num, pdev->vf_num, csm_dp_mem_type_to_str(mempool->type));
	get_mempool_buf_status(mempool);
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


	if ((result->transaction_status == -ENOTCONN) || (mhi->mhi_dev_suspended)) {
		CSM_DP_DEBUG("%s: (TX Dropped) ch %s bus %d VF %d addr=%p bytes=%lu status=%d mhi->mhi_dev_suspended %d\n",
			  __func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
			  result->bytes_xferd, result->transaction_status, mhi->mhi_dev_suspended == true?1:0);

	} else {
		CSM_DP_DEBUG("%s: (TX complete) ch %s bus %d VF %d addr=%p bytes=%lu status=%d\n",
			__func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
			result->bytes_xferd, result->transaction_status);

		csm_dp_hex_dump(result->buf_addr, result->bytes_xferd);
		mhi->stats.tx_acked++;
	}

	buf_cntrl = addr - sizeof(struct csm_dp_buf_cntrl);
	while (buf_cntrl) {
		mempool = csm_dp_get_mempool(pdev, buf_cntrl, NULL);
		if (unlikely(mempool == NULL)) {
			CSM_DP_LIMIT_ERROR("%s: cannot find mempool for ch %s bus %d VF %d, addr=0x%p\n",
					   __func__, ch_name(mhi_dev->id->driver_data),
					   pdev->bus_num, pdev->vf_num, addr);
			return;
		}

		if (mempool->signature != CSM_DP_MEMPOOL_SIG) {
			CSM_DP_LIMIT_ERROR("%s: mempool 0x%p signature 0x%x error, expect 0x%x"
					   " for ch %s bus %d VF %d\n",
					   __func__, mempool, mempool->signature,
					   CSM_DP_MEMPOOL_SIG, ch_name(mhi_dev->id->driver_data),
					   pdev->bus_num, pdev->vf_num);
			return;
		}

		if (atomic_read(&mempool->out_xmit) == 0) {
			CSM_DP_LIMIT_ERROR("%s: mempool 0x%p out xmit cnt should not be zero"
					   " for ch %s bus %d VF %d\n",
					   __func__, mempool, ch_name(mhi_dev->id->driver_data),
					   pdev->bus_num, pdev->vf_num);
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
			CSM_DP_LIMIT_ERROR("%s: no mempool (ch %s bus %d VF %d status %d)\n",
				__func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num,
				result->transaction_status);
		return;
	}

	if ((result->transaction_status == -ENOTCONN) || (mhi->mhi_dev_suspended)) {
		CSM_DP_DEBUG("%s: (RX) ch %s bus %d VF %d addr=%p bytes=%lu status=%d mhi->mhi_dev_suspended %d\n",
			  __func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
			  result->bytes_xferd, result->transaction_status, mhi->mhi_dev_suspended == true?1:0);
	} else {
		CSM_DP_DEBUG("%s: (RX) ch %s bus %d VF %d addr=%p bytes=%lu status=%d\n",
			__func__, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num, result->buf_addr,
			result->bytes_xferd, result->transaction_status);

		csm_dp_hex_dump(result->buf_addr, result->bytes_xferd);
	}

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
	for (; ((mhi->rx_head_buf_cntrl != mhi->rx_tail_buf_cntrl) || (mhi->rx_head_buf_cntrl == packet_end));
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
		if(mhi->rx_head_buf_cntrl != mhi->rx_tail_buf_cntrl)
			mhi->rx_head_buf_cntrl = packet_end->next;
		packet_start->buf_count = buf_count;
		packet_end->next = NULL;
		packet_end->next_buf_index = CSM_DP_INVALID_BUF_INDEX;
		packet_end->len = result->bytes_xferd;

		if (result->transaction_status == -ENOTCONN) {
			for (; packet_start; packet_start = packet_start->next)
				csm_dp_mempool_put_buf(mempool, packet_start + 1);
			return;
		}

		mhi->stats.rx_cnt++;
		csm_dp_rx(pdev, packet_start, result->bytes_xferd);

		return;
	}

	CSM_DP_LIMIT_ERROR("couldn't find end of packet for bus:%d VF:%d %s,"
			   "buf_addr 0x%p bytes:%lu "
			   "rx_head_buf_cntrl 0x%p rx_tail_buf_cntrl 0x%p buf_count %d\n",
			   pdev->bus_num, pdev->vf_num, csm_dp_mem_type_to_str(mempool->type),
			   result->buf_addr, result->bytes_xferd, mhi->rx_head_buf_cntrl,
			   mhi->rx_tail_buf_cntrl, buf_count);
	mhi->rx_head_buf_cntrl = packet_start;
}

/* worker function to reset (unprepare and prepare) MHI channel when channel goes into error state */
static void csm_dp_mhi_alloc_work(struct work_struct *work)
{
	struct csm_dp_mhi *mhi;
	const int sleep_us =  500;
	int retry = 10;
	unsigned int bus_num, vf_num;
	int ret;

	mhi = container_of(work, struct csm_dp_mhi, alloc_work);

	if (!mhi || !mhi->mhi_dev)
		return;

	bus_num = mhi_get_device_bus_number(mhi->mhi_dev->mhi_cntrl);
	vf_num = mhi_get_device_instance_id(mhi->mhi_dev->mhi_cntrl);
	CSM_DP_INFO("%s: bus %d VF %d ch %s\n", __func__, bus_num, vf_num, ch_name(mhi->mhi_dev->id->driver_data));

	if (!mhi->mhi_dev_suspended) {
		CSM_DP_ERROR("%s: mhi is not suspended\n", __func__);
		return;
	}

	mhi->stats.ch_err_cnt++;
	do {
		if (atomic_read(&mhi->mhi_dev_refcnt) == 0) {
			break;
		} else {
			usleep_range(sleep_us, 2*sleep_us);
			retry--;
		}
	} while (retry);

	mhi_unprepare_from_transfer(mhi->mhi_dev);
	CSM_DP_INFO("%s: bus %d VF %d ch %s mhi_unprepare_from_transfer completed\n", __func__, bus_num, vf_num, ch_name(mhi->mhi_dev->id->driver_data));

	/* mhi_prepare_for_transfer is a blocking call that will return only after the mhi channel connection is restored */
	ret = mhi_prepare_for_transfer(mhi->mhi_dev, 0);
	if (ret) {
		CSM_DP_ERROR("%s: mhi_prepare_for_transfer failed\n", __func__);
		return;
	}
	CSM_DP_INFO("%s: bus %d VF %d ch %s mhi_prepare_for_transfer completed\n", __func__, bus_num, vf_num, ch_name(mhi->mhi_dev->id->driver_data));
	mhi->rx_head_buf_cntrl = NULL;
	mhi->rx_tail_buf_cntrl = NULL;

	ret = csm_dp_mhi_rx_replenish(mhi);
	if (ret) {
		CSM_DP_ERROR("%s: csm_dp_mhi_rx_replenish failed\n", __func__);
		return;
	}

	mhi->mhi_dev_suspended = false;
	CSM_DP_INFO("%s: bus %d VF %d ch %s mhi channel reset completed\n", __func__, bus_num, vf_num, ch_name(mhi->mhi_dev->id->driver_data));
}

static void __mhi_status_cb(struct mhi_device *mhi_dev, enum mhi_callback mhi_cb)
{
	struct csm_dp_dev *pdev;
	struct csm_dp_mhi *mhi;

	switch (mhi_cb) {
	case MHI_CB_PENDING_DATA:
		pdev = dev_get_drvdata(&mhi_dev->dev);
		if (napi_schedule_prep(&pdev->napi)) {
			__napi_schedule(&pdev->napi);
			pdev->stats.rx_int++;
		}
		break;
	case MHI_CB_CHANNEL_ERROR:
		mhi = get_dp_mhi(mhi_dev);
		mhi->mhi_dev_suspended = true;
		queue_work(mhi->mhi_dev_workqueue, &mhi->alloc_work);
		break;
	default:
		break;
	}
}

int csm_dp_mhi_rx_replenish(struct csm_dp_mhi *mhi)
{
	int ret;

	spin_lock_bh(&mhi->rx_lock);

	if(mhi->mhi_dev_destroyed) {
		ret = -ENODEV;
		CSM_DP_LIMIT_ERROR("%s: Replenish error:%d Device destroyed\n",
			__func__, ret);
	}
	else
		ret = __mhi_rx_replenish(mhi);

	spin_unlock_bh(&mhi->rx_lock);
	return ret;
}

void csm_dp_mhi_tx_poll(struct csm_dp_mhi* mhi)
{
    int n;

    do {
        n = mhi_poll(mhi->mhi_dev, CSM_DP_NAPI_WEIGHT, DMA_TO_DEVICE);
        if (n < 0)
            CSM_DP_LIMIT_ERROR("%s: Error Tx polling n:%d\n",
		__func__, n);
    } while (n == CSM_DP_NAPI_WEIGHT);
}

void csm_dp_mhi_rx_poll(struct csm_dp_mhi* mhi)
{
	int n;

	do {
		n = mhi_poll(mhi->mhi_dev, CSM_DP_NAPI_WEIGHT, DMA_FROM_DEVICE);
		if (n < 0)
			pr_err_ratelimited("Error Rx polling n:%d\n", n);
		CSM_DP_INFO("%s: number of Rx poll %d\n", __func__, n);
	} while (n == CSM_DP_NAPI_WEIGHT);
}

static void csm_dp_mhi_packet_stats_reset(struct csm_dp_mhi *mhi,
					  struct csm_dp_dev *pdev,
					  enum csm_dp_channel ch)
{
	struct csm_dp_core_stats *stats = &pdev->stats;
	bool is_control = (ch == CSM_DP_CH_CONTROL);

	if (mhi) {
		mhi->stats.tx_cnt = 0;
		mhi->stats.tx_acked = 0;
		mhi->stats.tx_err = 0;
		mhi->stats.rx_cnt = 0;
		mhi->stats.rx_out_of_buf = 0;
		mhi->stats.rx_replenish = 0;
		mhi->stats.rx_replenish_err = 0;
		mhi->stats.ch_err_cnt = 0;
		if (stats && is_control)
			stats->rx_drop = 0;
	}
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

	bus_num = mhi_get_device_bus_number(mhi_dev->mhi_cntrl);
	vf_num = mhi_get_device_instance_id(mhi_dev->mhi_cntrl);
	CSM_DP_INFO("%s: probing bus:%d VF:%d mhi chan %s dp chan %s\n",
		     __func__, bus_num, vf_num, id->chan, ch_name(id->driver_data));

	if (__pdrv == NULL)
		return -ENODEV;

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
		CSM_DP_ERROR("%s: unexpected driver_data %ld for bus:%d VF:%d\n", __func__,
			     id->driver_data, bus_num, vf_num);
		ret = -EINVAL;
		goto err;
	}

	free_rx_ring_buffers(mempool, true);
	csm_dp_mhi_packet_stats_reset(mhi, pdev, id->driver_data);

	dev_set_drvdata(&mhi_dev->dev, pdev);

	ret = mhi_prepare_for_transfer(mhi_dev, 0);
	if (ret) {
		CSM_DP_ERROR("%s: mhi_prepare_for_transfer failed for bus:%d VF:%d mhi chan %s dp chan %s\n",
			     __func__, bus_num, vf_num, id->chan, ch_name(id->driver_data));
		goto err;
	}

	/* Creating workqueue */
	mhi->mhi_dev_workqueue = alloc_workqueue("csm_dp_mhi_workqueue", WQ_UNBOUND|WQ_MEM_RECLAIM, 0);
	if (!mhi->mhi_dev_workqueue) {
		CSM_DP_ERROR("%s: Failed to allocate workqueue for bus:%d VF:%d mhi chan %s dp chan %s\n",
			     __func__, bus_num, vf_num, id->chan, ch_name(id->driver_data));
		goto err;
	}

	INIT_WORK(&mhi->alloc_work, csm_dp_mhi_alloc_work);

	mhi->mhi_dev = mhi_dev;
	mhi->mhi_dev_suspended = false;
	atomic_set(&mhi->mhi_dev_refcnt, 0);
	spin_lock_init(&mhi->rx_lock);
	mutex_init(&mhi->tx_mutex);
	mhi->rx_head_buf_cntrl = NULL;
	mhi->rx_tail_buf_cntrl = NULL;

	mhi->mhi_dev_destroyed = false;
	CSM_DP_DEBUG("%s: csm_dp_mhi_rx_replenish\n", __func__);
	if (mempool) {
		ret = csm_dp_mempool_dma_map(mhi_dev->mhi_cntrl->cntrl_dev, mempool);
		if (ret) {
			CSM_DP_ERROR("%s: dma_map failed for bus:%d VF:%d mhi chan %s dp chan %s, "
				     "mempool type %d ret %d\n",
				     __func__, bus_num, vf_num, id->chan,
				     ch_name(id->driver_data), mempool->type, ret);
			goto err;
		}

		ret = csm_dp_mhi_rx_replenish(mhi);
		if (ret) {
			CSM_DP_ERROR("%s: csm_dp_mhi_rx_replenish failed "
				     "for bus:%d VF:%d mhi chan %s dp chan %s\n",
				     __func__, bus_num, vf_num, id->chan, ch_name(id->driver_data));
			goto err;
		}
	}

	CSM_DP_INFO("%s: successful for bus:%d VF:%d mhi chan %s dp chan %s\n",
                    __func__, bus_num, vf_num, id->chan, ch_name(id->driver_data));
	return 0;

err:
	mhi->mhi_dev_destroyed = true;
	csm_dp_cdev_del(pdev);
	return ret;
}

static void csm_dp_mhi_remove(struct mhi_device *mhi_dev)
{
	struct csm_dp_dev *pdev = dev_get_drvdata(&mhi_dev->dev);
	struct csm_dp_mhi *mhi;

	CSM_DP_INFO("%s: mhi chan %s dp chan %s bus %d VF %d\n",
		__func__, mhi_dev->id->chan, ch_name(mhi_dev->id->driver_data), pdev->bus_num, pdev->vf_num);

	switch (mhi_dev->id->driver_data) {
	case CSM_DP_CH_CONTROL:
		mhi = &pdev->mhi_control_dev;
		break;
	case CSM_DP_CH_DATA:
		mhi = &pdev->mhi_data_dev;
		break;
	default:
		CSM_DP_ERROR("%s: unexpected driver_data %ld\n", __func__, mhi_dev->id->driver_data);
		return;
	}

	flush_work(&mhi->alloc_work);
	destroy_workqueue(mhi->mhi_dev_workqueue);
	mhi_unprepare_from_transfer(mhi_dev);

	spin_lock_bh(&mhi->rx_lock);
	mhi->mhi_dev_destroyed = true;
	spin_unlock_bh(&mhi->rx_lock);

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
