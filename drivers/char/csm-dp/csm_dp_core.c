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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include "csm_dp.h"

#define DEFAULT_RX_QUEUE_SIZE 1024

static struct csm_dp_drv *csm_dp_pdrv;

static uint rx_queue_size = DEFAULT_RX_QUEUE_SIZE;
module_param(rx_queue_size, uint, 0444);
MODULE_PARM_DESC(rx_queue_size, " Rx queue size, default 1024");

static struct csm_dp_mhi *get_dp_mhi(struct csm_dp_dev *pdev, enum csm_dp_channel ch)
{
	switch (ch) {
	case CSM_DP_CH_CONTROL:
		return &pdev->mhi_control_dev;
	case CSM_DP_CH_DATA:
		return &pdev->mhi_data_dev;
	default:
		CSM_DP_ASSERT(0, "invalid ch");
		return NULL;
	}
}

static int csm_dp_rxqueue_init(
	struct csm_dp_rxqueue *rxq,
	enum csm_dp_rx_type rx_type,
	unsigned int size)
{
	unsigned int ring_size;
	int ret;

	if (!csm_dp_rx_type_is_valid(rx_type))
		return -EINVAL;

	if (rxq->inited) {
		CSM_DP_ERROR("%s: rx queue already initialized!\n", __func__);
		return -EINVAL;
	}

	ring_size = calc_ring_size(size);
	if (!ring_size)
		return -EINVAL;

	rxq->ring = kzalloc(sizeof(struct csm_dp_ring ), GFP_KERNEL);
	if (!rxq->ring)
		return -ENOMEM;

	ret = csm_dp_ring_init(rxq->ring, ring_size, MMAP_RX_COOKIE(rx_type));
	if (ret) {
		CSM_DP_DEBUG("%s: failed to initialize rx ring!\n", __func__);
		kfree(rxq->ring);
		rxq->ring = NULL;

		return ret;
	}

	init_waitqueue_head(&rxq->wq);
	rxq->type = rx_type,
	rxq->inited = true;
	atomic_set(&rxq->refcnt, 0);

	return 0;
}

static void csm_dp_rxqueue_cleanup(struct csm_dp_rxqueue *rxq)
{
	if (rxq->inited) {
		rxq->inited = false;
		wake_up(&rxq->wq);
		csm_dp_ring_cleanup(rxq->ring);
		kfree(rxq->ring);
		rxq->ring = NULL;
	}
}

void csm_dp_rx(struct csm_dp_dev *pdev, struct csm_dp_buf_cntrl *buf_cntrl, unsigned int length)
{
	struct csm_dp_mempool *mempool;
	struct csm_dp_rxqueue *rxq;
	unsigned int offset;
	unsigned int cl;
	void *addr = buf_cntrl + 1;

	if (unlikely(pdev == NULL || addr == NULL || !length)) {
		CSM_DP_ERROR("%s: invalid argument\n", __func__);
		return;
	}

	mempool = csm_dp_get_mempool(pdev, buf_cntrl, &cl);
	if (mempool == NULL) {
		CSM_DP_ERROR("%s: not UL address, addr=%p\n",
			  __func__, addr);
		return;
	}

	if (mempool->type == CSM_DP_MEM_TYPE_UL_DATA) {
		struct csm_dp_buf_cntrl **p = &pdev->pending_packets;

		while (*p)
			p = &((*p)->next_packet);

		*p = buf_cntrl;

		return;
	}

	/* only one Rx queue */
	rxq = &pdev->rxq[CSM_DP_RX_TYPE_FAPI];

	if (!atomic_read(&rxq->refcnt)) {
		CSM_DP_DEBUG("%s: rxq not active, drop message\n", __func__);
		goto free_rxbuf;
	}

#ifdef CSM_DP_BUFFER_FENCING
	csm_dp_set_buf_state(addr,
			CSM_DP_BUF_STATE_KERNEL_RECVCMP_MSGQ_TO_APP);
#endif
	offset = csm_dp_get_mem_offset(addr, &mempool->mem.loc, cl);
	if (csm_dp_ring_write(rxq->ring, offset)) {
		CSM_DP_ERROR("%s: failed to enqueue rx packet\n", __func__);
		goto free_rxbuf;
	}
	wake_up(&rxq->wq);
	pdev->stats.rx_cnt++;
	return;
free_rxbuf:
	pdev->stats.rx_drop++;
	csm_dp_mempool_put_buf(mempool, addr);
}

int csm_dp_rx_init(struct csm_dp_dev *pdev)
{
	unsigned int type;
	int ret;
	unsigned int csm_dp_ul_buf_size = CSM_DP_DEFAULT_UL_BUF_SIZE;
	unsigned int csm_dp_ul_buf_cnt = CSM_DP_DEFAULT_UL_BUF_CNT;

	// TODO: add module params for ul_buf_size/cnt
	if (csm_dp_ul_buf_size > CSM_DP_MAX_UL_MSG_LEN) {
		CSM_DP_ERROR("%s: UL buffer size %d exceeds limit %d\n",
			__func__,
			csm_dp_ul_buf_size,
			CSM_DP_MAX_UL_MSG_LEN);
		return -ENOMEM;
	}

	pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL] = csm_dp_mempool_alloc(
		pdev,
		CSM_DP_MEM_TYPE_UL_CONTROL,
		csm_dp_ul_buf_size,
		csm_dp_ul_buf_cnt,
		false); /* no dma map yet since io dev is not ready */
	if (pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL] == NULL) {
		CSM_DP_ERROR("%s: failed to allocate UL_CONTROL memory pool!\n",
				  __func__);
		return -ENOMEM;
	}

	/* TODO: use different buf_size & cnt for UL_DATA */
	pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA] = csm_dp_mempool_alloc(
		pdev,
		CSM_DP_MEM_TYPE_UL_DATA,
		csm_dp_ul_buf_size,
		csm_dp_ul_buf_cnt,
		false); /* no dma map yet since io dev is not ready */
	if (pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA] == NULL) {
		CSM_DP_ERROR("%s: failed to allocate UL_DATA memory pool!\n",
				  __func__);
		return -ENOMEM;
	}

	for (type = 0; type < CSM_DP_RX_TYPE_LAST; type++) {
		ret = csm_dp_rxqueue_init(&pdev->rxq[type], type, rx_queue_size);
		if (ret) {
			CSM_DP_ERROR("%s: failed to init rxqueue!\n", __func__);
			return ret;
		}
	}

	return 0;
}

void csm_dp_rx_cleanup(struct csm_dp_dev *pdev)
{
	unsigned int type;

	if (pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL])
		csm_dp_mempool_free(pdev->mempool[CSM_DP_MEM_TYPE_UL_CONTROL]);
	if (pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA])
		csm_dp_mempool_free(pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA]);

	for (type = 0; type < CSM_DP_RX_TYPE_LAST; type++)
		csm_dp_rxqueue_cleanup(&pdev->rxq[type]);
}

int csm_dp_tx(
	struct csm_dp_dev *pdev,
	enum csm_dp_channel ch,
	struct iovec *iov,
	unsigned int iov_nr,
	unsigned int flag,
	dma_addr_t dma_addr_array[])
{
	int ret = 0, n;
	unsigned int num, to_send;
	int j;
	struct csm_dp_mhi *mhi;

	if (unlikely(!pdev || !iov || !iov_nr))
		return -EINVAL;

	mhi = get_dp_mhi(pdev, ch);

	if (flag & CSM_DP_TX_FLAG_SG) {
		if (iov_nr > CSM_DP_MAX_SG_IOV_SIZE) {
			CSM_DP_ERROR("%s: sg iov size too big!\n", __func__);
			return -EINVAL;
		}
	}

	atomic_inc(&mhi->mhi_dev_refcnt);
	if (!csm_dp_mhi_is_ready(mhi)) {
		atomic_dec(&mhi->mhi_dev_refcnt);
		CSM_DP_ERROR("%s: mhi is not ready!\n", __func__);
		pdev->stats.tx_err++;
		return -EIO;
	}

	mutex_lock(&mhi->tx_mutex);
	to_send = 0;
	for (n = 0, to_send = iov_nr; to_send > 0; ) {
		if (to_send > CSM_DP_MAX_IOV_SIZE)
			num = CSM_DP_MAX_IOV_SIZE;
		else
			num = to_send;
		for (j = 0; j < num; j++) {
			if ((flag & CSM_DP_TX_FLAG_SG) && n != (iov_nr - 1))
				mhi->dl_flag_array[j] = MHI_CHAIN;
			else
				mhi->dl_flag_array[j] =  MHI_EOT;
			mhi->dl_buf_array[j].len = iov[n].iov_len;

			if (flag & CSM_DP_TX_FLAG_SG) {
				mhi->dl_flag_array[j] |= MHI_SG;
				mhi->dl_buf_array[j].buf = iov[0].iov_base;
			} else {
				mhi->dl_buf_array[j].buf = iov[n].iov_base;
			}

			if (ch == CSM_DP_CH_DATA)
				mhi->dl_flag_array[j] |= MHI_BEI;

			if (flag & CSM_DP_TX_FLAG_MIRROR)
				mhi->dl_flag_array[j] |= MHI_MIRROR;

			if (dma_addr_array[n]) {
				mhi->dl_buf_array[j].dma_addr =
					dma_addr_array[n];
				mhi->dl_buf_array[j].streaming_dma = true;
			} else {
				mhi->dl_buf_array[j].dma_addr = 0;
				mhi->dl_buf_array[j].streaming_dma = false;
			}
			n++;
		}
		ret = csm_dp_mhi_n_tx(mhi, num);
		if (ret) {
			pdev->stats.tx_err++;
			break;
		}
		to_send -= num;
	}

	if (!(flag & CSM_DP_TX_FLAG_SG))
		pdev->stats.tx_cnt += (iov_nr - to_send);
	else if (!to_send)
		pdev->stats.tx_cnt++;
	mutex_unlock(&mhi->tx_mutex);
	atomic_dec(&mhi->mhi_dev_refcnt);
	return ret;
}

int csm_dp_rx_poll(struct csm_dp_dev *pdev, struct iovec *iov, size_t iov_nr)
{
	int ret;
	struct csm_dp_buf_cntrl *cur_packet;
	size_t n = 0, remain = iov_nr;

	atomic_inc(&pdev->mhi_data_dev.mhi_dev_refcnt);
	if (!csm_dp_mhi_is_ready(&pdev->mhi_data_dev)) {
		atomic_dec(&pdev->mhi_data_dev.mhi_dev_refcnt);
		return 0;
	}

	/*
	 * poll to get packets from MHI. This will cause dl_xfer (RX callback) to get called which
	 * will then link the Rx packets into pdrv->pending_packets
	 */
	ret = mhi_poll(pdev->mhi_data_dev.mhi_dev, CSM_DP_NAPI_WEIGHT, DMA_FROM_DEVICE);
	if (ret < 0)
		pr_err_ratelimited("%s: Error rx polling %d\n", __func__, ret);

	ret = csm_dp_mhi_rx_replenish(&pdev->mhi_data_dev);
	if (ret < 0)
		pr_err_ratelimited("%s: Error rx replenish %d\n", __func__, ret);

	atomic_dec(&pdev->mhi_data_dev.mhi_dev_refcnt);

	/* fill iov with the received packets */
	cur_packet = pdev->pending_packets;
	while (cur_packet) {
		struct csm_dp_buf_cntrl *cur_buf, *tmp;

		if (cur_packet->buf_count > remain) {
			if (cur_packet == pdev->pending_packets)
				return -EINVAL;	/* provided iov is too short even for 1st packet */
			/* no more room in iov, we're done */
			break;
		}

		for (cur_buf = cur_packet; cur_buf; cur_buf = cur_buf->next) {
			unsigned int cl;
			struct csm_dp_mempool *mempool = pdev->mempool[CSM_DP_MEM_TYPE_UL_DATA];

			CSM_DP_ASSERT(mempool == NULL, "not UL address\n");

			cl = csm_dp_mem_get_cluster(&mempool->mem, cur_buf->buf_index);
			iov[n].iov_base = (void *)csm_dp_get_mem_offset(cur_buf + 1,
									&mempool->mem.loc, cl);
			iov[n].iov_len = cur_buf->len;
			n++;
			remain--;
		}

		tmp = cur_packet;
		cur_packet = cur_packet->next_packet;
		tmp->next_packet = NULL;
	}

	pdev->pending_packets = cur_packet;

	return n;
}

int csm_dp_get_stats(struct csm_dp_dev *pdev, struct csm_dp_ioctl_getstats *stats)
{
	struct csm_dp_mhi *mhi = NULL;

	switch (stats->ch) {
	case CSM_DP_CH_CONTROL:
		mhi = &pdev->mhi_control_dev;
		break;
	case CSM_DP_CH_DATA:
		mhi = &pdev->mhi_data_dev;
		break;
	}

	if (!mhi)
		return -EINVAL;

	stats->tx_cnt = mhi->stats.tx_cnt;
	stats->tx_acked = mhi->stats.tx_acked;
	stats->rx_cnt = mhi->stats.rx_cnt;

	return 0;
}

/* napi function to replenish control channel */
static int csm_dp_poll(struct napi_struct *napi, int budget)
{
	int rx_work = 0;
	struct csm_dp_dev *pdev;
	int ret;

	pdev = container_of(napi, struct csm_dp_dev, napi);
	atomic_inc(&pdev->mhi_control_dev.mhi_dev_refcnt);
	if (!csm_dp_mhi_is_ready(&pdev->mhi_control_dev)) {
		atomic_dec(&pdev->mhi_control_dev.mhi_dev_refcnt);
		return 0;
	}

	rx_work = mhi_poll(pdev->mhi_control_dev.mhi_dev, budget, DMA_FROM_DEVICE);
	if (rx_work < 0) {
		pr_err("%s: Error Rx polling ret:%d\n", __func__, rx_work);
		rx_work = 0;
		napi_complete(napi);
		goto exit_poll;
	}

	ret = csm_dp_mhi_rx_replenish(&pdev->mhi_control_dev);
	if (ret == -ENOMEM)
		schedule_work(&pdev->alloc_work);  /* later */
	if (rx_work < budget)
		napi_complete(napi);
	else
		pdev->stats.rx_budget_overflow++;
exit_poll:
	atomic_dec(&pdev->mhi_control_dev.mhi_dev_refcnt);
	return rx_work;
}

/* worker function to replenish control channel, in case replenish failed in napi function */
static void csm_dp_alloc_work(struct work_struct *work)
{
	struct csm_dp_dev *pdev;
	const int sleep_ms =  1000;
	int retry = 60;
	int ret;

	pdev = container_of(work, struct csm_dp_dev, alloc_work);

	do {
		ret = csm_dp_mhi_rx_replenish(&pdev->mhi_control_dev);
		/* sleep and try again */
		if (ret == -ENOMEM) {
			msleep(sleep_ms);
			retry--;
		}
	} while (ret == -ENOMEM && retry);
}

static int csm_dp_core_init(struct csm_dp_drv *pdrv)
{
	int i;

	for (i = 0; i < CSM_DP_MAX_NUM_DEVS; i++) {
		struct csm_dp_dev *pdev = &pdrv->dp_devs[i];

		pdev->pdrv = pdrv;
		mutex_init(&pdev->mempool_lock);
		mutex_init(&pdev->cdev_lock);
		INIT_LIST_HEAD(&pdev->cdev_head);
		init_dummy_netdev(&pdev->dummy_dev);
		netif_napi_add(&pdev->dummy_dev, &pdev->napi, csm_dp_poll,
							CSM_DP_NAPI_WEIGHT);
		napi_enable(&pdev->napi);
		INIT_WORK(&pdev->alloc_work, csm_dp_alloc_work);
	}

	return 0;
}

static void csm_dp_core_cleanup(struct csm_dp_drv *pdrv)
{
	int i;

	for (i = 0; i < CSM_DP_MAX_NUM_DEVS; i++) {
		struct csm_dp_dev *pdev = &pdrv->dp_devs[i];

		flush_work(&pdev->alloc_work);
		napi_disable(&pdev->napi);
		netif_napi_del(&pdev->napi);

		csm_dp_rx_cleanup(pdev);
	}

	kfree(pdrv);
}

static int csm_dp_probe(void)
{
	struct csm_dp_drv *pdrv;
	int ret;

	pr_info("CSM-DP: probing CSM\n");

	pdrv = kzalloc(sizeof(*pdrv), GFP_KERNEL);
	if (IS_ERR_OR_NULL(pdrv))
		return -ENOMEM;
	csm_dp_pdrv = pdrv;

	ret = csm_dp_core_init(pdrv);
	if (ret)
		goto cleanup;

	ret = csm_dp_cdev_init(pdrv);
	if (ret)
		goto cleanup;

	ret = csm_dp_debugfs_init(pdrv);
	if (ret)
		goto cleanup_cdev;

	ret = csm_dp_mhi_init(pdrv);
	if (ret)
		goto cleanup_debugfs;

	pr_info("CSM-DP: module initialized now\n");
	return 0;

cleanup_debugfs:
	csm_dp_debugfs_cleanup(pdrv);
cleanup_cdev:
	csm_dp_cdev_cleanup(pdrv);
cleanup:
	csm_dp_core_cleanup(pdrv);
	csm_dp_pdrv = NULL;
	pr_err("CSM-DP: module init failed!\n");
	return ret;
}

static int csm_dp_remove(void)
{
	struct csm_dp_drv *pdrv = csm_dp_pdrv;

	if (pdrv) {
		csm_dp_mhi_cleanup(pdrv);
		csm_dp_cdev_cleanup(pdrv);
		csm_dp_debugfs_cleanup(pdrv);
		csm_dp_core_cleanup(pdrv);
	}
	csm_dp_pdrv = NULL;

	return 0;
}

static int __init csm_dp_module_init(void)
{
	pr_info("csm_dp_module_init\n");

	return csm_dp_probe();
}
module_init(csm_dp_module_init);

static void __exit csm_dp_module_exit(void)
{
	pr_info("csm_dp_module_exit\n");

	csm_dp_remove();
}
module_exit(csm_dp_module_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CSM DP driver");
MODULE_VERSION(DP_MODULE_VERSION);
