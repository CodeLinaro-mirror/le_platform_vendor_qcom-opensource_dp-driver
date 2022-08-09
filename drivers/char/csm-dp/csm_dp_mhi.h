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
#ifndef __CSM_DP_MHI_H__
#define __CSM_DP_MHI_H__

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/local_mhi.h>
#include <linux/kthread.h>

#define CSM_DP_MHI_NAME	"csm-l1rf-mhi"

struct csm_dp_drv;

struct csm_dp_mhi_stats {
	unsigned long tx_cnt;
	unsigned long tx_acked;
	unsigned long tx_err;
	unsigned long rx_cnt;
	unsigned long rx_err;
	unsigned long rx_out_of_buf;

	unsigned long rx_replenish;
	unsigned long rx_replenish_err;
	unsigned long rx_outofbuf_drop;
	unsigned long rx_resync;
};

/* represents MHI channel pair - Tx and Rx */
struct csm_dp_mhi {
	struct mhi_device *mhi_dev;
	struct csm_dp_mhi_stats stats;
	spinlock_t rx_lock;
	spinlock_t tx_lock;
	struct completion poll_comp;
	struct hrtimer poll_timer;
	struct task_struct *tx_poll_thread;
	bool mhi_destroyed;	/* TODO: remove? */
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

	if (mhi->mhi_destroyed)
		return -ENODEV;

	ret = mhi_queue_n_dma(mhi->mhi_dev, DMA_TO_DEVICE, mhi->dl_buf_array,
			      mhi->dl_flag_array, num);
	if (!ret) {
		mhi->stats.tx_cnt += num;
	} else {
		pr_err_ratelimited("mhi_queue_n_dma failed, num %d ret %d\n", num, ret);
		mhi->stats.tx_err += num;
	}
	return ret;
}

static inline bool csm_dp_mhi_is_ready(struct csm_dp_mhi *mhi)
{
	return ((mhi->mhi_dev) ? true : false);
}

#endif /* __CSM_DP_MHI_H__ */
