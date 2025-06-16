/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#ifndef __CSM_DP_MHI_H__
#define __CSM_DP_MHI_H__

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/local_mhi.h>

#define CSM_DP_MHI_NAME	"csm-l1rf-mhi"

struct csm_dp_drv;

struct csm_dp_mhi_stats {
	unsigned long tx_cnt;
	unsigned long tx_acked;
	unsigned long tx_err;
	unsigned long rx_cnt;
	unsigned long rx_out_of_buf;

	unsigned long rx_replenish;
	unsigned long rx_replenish_err;
	unsigned long ch_err_cnt;
};

/* represents MHI channel pair - Tx and Rx */
struct csm_dp_mhi {
	struct mhi_device *mhi_dev;
	bool mhi_dev_destroyed;
	bool mhi_dev_suspended;
	atomic_t mhi_dev_refcnt;
	struct csm_dp_mhi_stats stats;
	spinlock_t rx_lock;
	struct mutex tx_mutex;
	struct workqueue_struct *mhi_dev_workqueue;
	struct work_struct alloc_work;
	/*
	 * the following are for needed storage
	 * for mhi_queue_n_transfer.
	 */
	enum mhi_flags ul_flag_array[CSM_DP_MAX_IOV_SIZE];
	enum mhi_flags dl_flag_array[CSM_DP_MAX_IOV_SIZE];
	struct mhi_buf dl_buf_array[CSM_DP_MAX_IOV_SIZE];
	struct mhi_buf ul_buf_array[CSM_DP_MAX_IOV_SIZE];

	struct csm_dp_buf_cntrl *rx_head_buf_cntrl, *rx_tail_buf_cntrl;
};

int csm_dp_mhi_init(struct csm_dp_drv *pdrv);
void csm_dp_mhi_cleanup(struct csm_dp_drv *pdrv);

int csm_dp_mhi_rx_replenish(struct csm_dp_mhi *mhi);

static inline int csm_dp_mhi_n_tx(struct csm_dp_mhi *mhi,
				unsigned int num)
{
	int ret;

	ret = mhi_queue_n_dma(mhi->mhi_dev, DMA_TO_DEVICE, mhi->dl_buf_array,
			      mhi->dl_flag_array, num);
	if (!ret)
		mhi->stats.tx_cnt += num;
	else
		mhi->stats.tx_err += num;
	return ret;
}

static inline bool csm_dp_mhi_is_ready(struct csm_dp_mhi *mhi)
{
	return mhi->mhi_dev && !mhi->mhi_dev_destroyed && !mhi->mhi_dev_suspended;
}

void csm_dp_mhi_tx_poll(struct csm_dp_mhi *mhi);
void csm_dp_mhi_rx_poll(struct csm_dp_mhi *mhi);

#endif /* __CSM_DP_MHI_H__ */
