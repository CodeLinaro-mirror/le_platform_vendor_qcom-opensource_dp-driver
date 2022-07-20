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
#ifndef __FSM_DP_MHI_H__
#define __FSM_DP_MHI_H__

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/local_mhi.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>

#define FSM_DP_MHI_NAME	"fsm-l1rf-mhi"

struct fsm_dp_drv;

struct fsm_dp_mhi_stats {
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
struct fsm_dp_mhi {
	struct mhi_device *mhi_dev;
	struct fsm_dp_mhi_stats stats;
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
	enum mhi_flags ul_flag_array[FSM_DP_MAX_IOV_SIZE];
	enum mhi_flags dl_flag_array[FSM_DP_MAX_IOV_SIZE];
	struct mhi_buf dl_buf_array[FSM_DP_MAX_IOV_SIZE];
	struct mhi_buf ul_buf_array[FSM_DP_MAX_IOV_SIZE];

	struct fsm_dp_buf_cntrl *rx_head_buf_cntrl, *rx_tail_buf_cntrl;
};

int fsm_dp_mhi_init(struct fsm_dp_drv *pdrv);
void fsm_dp_mhi_cleanup(struct fsm_dp_drv *pdrv);

int fsm_dp_mhi_rx_replenish(struct fsm_dp_mhi *mhi);

static inline int fsm_dp_mhi_skb_ul_xfer(
	struct fsm_dp_mhi *mhi, struct sk_buff *skb)
{
	return 0;
}

static inline int fsm_dp_mhi_n_tx(struct fsm_dp_mhi *mhi,
				unsigned int num)
{
	int ret;

	if (mhi->mhi_destroyed)
		return -ENODEV;

	ret = mhi_queue_n_dma(mhi->mhi_dev, DMA_TO_DEVICE, mhi->dl_buf_array,
			      mhi->dl_flag_array, num);
	if (!ret)
		mhi->stats.tx_cnt += num;
	else
		mhi->stats.tx_err += num;
	return ret;
}

static inline bool fsm_dp_mhi_is_ready(struct fsm_dp_mhi *mhi)
{
	return ((mhi->mhi_dev) ? true : false);
}

#endif /* __FSM_DP_MHI_H__ */
