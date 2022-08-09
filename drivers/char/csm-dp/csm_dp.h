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
#ifndef __CSM_DP__
#define __CSM_DP__

#ifndef __KERNEL__
#define __KERNEL__
#endif

#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/netdevice.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include <linux/csm_dp_ioctl.h>

#include "csm_dp_mhi.h"
#include "csm_dp_mem.h"

#define CSM_DP_MODULE_NAME		"csm-dp"
#define CSM_DP_DEV_CLASS_NAME	CSM_DP_MODULE_NAME
#define CSM_DP_CDEV_NAME	CSM_DP_MODULE_NAME
#define CSM_DP_NAPI_WEIGHT 64

#define CSM_DP_DEBUG	pr_debug
#define CSM_DP_INFO	pr_info
#define CSM_DP_ERROR	pr_err
#define CSM_DP_WARN	pr_warn

struct vm_area_struct;

#define MMAP_MEM_TYPE_SHIFT	24
#define MMAP_MEM_TYPE_MASK	0xFF
#define MMAP_TYPE_SHIFT		16
#define MMAP_TYPE_MASK		0xFF

#define MMAP_COOKIE(type, target) \
	((((type) & MMAP_MEM_TYPE_MASK) <<  MMAP_MEM_TYPE_SHIFT) | \
	(((target) & MMAP_TYPE_MASK) << MMAP_TYPE_SHIFT))

#define MMAP_COOKIE_TO_MEM_TYPE(cookie) \
	(((cookie) >> MMAP_MEM_TYPE_SHIFT) & MMAP_MEM_TYPE_MASK)

#define MMAP_COOKIE_TO_TYPE(cookie) \
	(((cookie) >> MMAP_TYPE_SHIFT) & MMAP_TYPE_MASK)

#define MMAP_RX_COOKIE(type) \
	MMAP_COOKIE((type)+CSM_DP_MEM_TYPE_LAST, CSM_DP_MMAP_TYPE_RING)

#define MMAP_RX_COOKIE_TO_TYPE(cookie) \
	(MMAP_COOKIE_TO_MEM_TYPE(cookie) - CSM_DP_MEM_TYPE_LAST)

#define TEST_RING_MMAP_COOKIE	0x80000000

#define TX_MODE_LOOPBACK 1

#define DEFAULT_RX_QUEUE_SIZE		1024

#define CSM_DP_TX_FLAG_SG	0x01
#define CSM_DP_TX_FLAG_LOOPBACK	0x02
#define CSM_DP_TX_FLAG_MIRROR	0x04

#define CSM_DP_ASSERT(cond, msg) do { \
	if (cond) \
		panic(msg); \
} while (0)

#define CSM_DP_MAX_NUM_DEVS 4

/*
 * vma mapping for mempool which includes
 * - buffer memory region
 * - ring buffer shared between kernel and user space
 *   for buffer management
 */
struct csm_dp_mempool_vma {
	struct csm_dp_mempool **pp_mempool;
	struct vm_area_struct *vma[CSM_DP_MMAP_TYPE_LAST];	/* mmap vma */
	atomic_t refcnt[CSM_DP_MMAP_TYPE_LAST];
	bool usr_alloc;	/* allocated by user using ioctl */
};

/* vma mapping for receive queue */
struct csm_dp_rxqueue_vma {
	enum csm_dp_rx_type type;
	struct vm_area_struct *vma;
	atomic_t refcnt;
};

/* RX queue using ring buffer */
struct csm_dp_rxqueue {
	enum csm_dp_rx_type type;
	struct csm_dp_ring ring;
	wait_queue_head_t wq;
	atomic_t refcnt;
	bool inited;
};

/* csm_dp_cdev tx_mode field bitmap */
#define TX_MODE_LOOPBACK 1

/* Per-process character device structure */
struct csm_dp_cdev {
	struct list_head list;
	struct csm_dp_dev *pdev;
	pid_t pid;

	/* vma mapping for memory pool */
	struct csm_dp_mempool_vma mempool_vma[CSM_DP_MEM_TYPE_LAST];

	/* vma mapping for receiving queue */
	struct csm_dp_rxqueue_vma rxqueue_vma[CSM_DP_RX_TYPE_LAST];
	unsigned int tx_mode;
};

struct csm_dp_loopback_stats {
	unsigned long tx_cnt;
	unsigned long tx_enque;
	unsigned long tx_drop;
	unsigned long tx_err;
	unsigned long rx_cnt;
	unsigned long rx_enque;
	unsigned long rx_drop;
	unsigned long rx_err;
	unsigned long run;
	unsigned long sched;
};

struct csm_dp_loopback_job {
	struct list_head list;
	void *data;
	unsigned int length;
	unsigned int dest;
	bool rx_loopback;
};

struct csm_dp_loopback_task {
	struct list_head free_q;
	struct list_head job_q;
	spinlock_t lock;
	struct work_struct work;
	struct workqueue_struct *workq;
	void *alloc_ptr;
	bool inited;
	struct csm_dp_loopback_stats stats;
};

struct csm_dp_test_ring {
	struct csm_dp_ring ring;
	bool enable;
};

struct csm_dp_core_stats {
	unsigned long tx_cnt;
	unsigned long tx_err;

	unsigned long rx_cnt;
	unsigned long rx_badmsg;
	unsigned long rx_drop;
	unsigned long rx_int;
	unsigned long rx_budget_overflow;
};

struct csm_dp_dev {
	struct csm_dp_drv *pdrv;		/* parent */
	struct csm_dp_mhi mhi_control_dev;	/* control path Tx/Rx */
	struct csm_dp_mhi mhi_data_dev;		/* data path Tx/Rx */
	bool cdev_inited;
	struct cdev cdev;
	struct net_device dummy_dev;
	struct napi_struct napi;
	struct mutex cdev_lock;
	struct list_head cdev_head;
	struct mutex mempool_lock;
	atomic_t tx_seqnum;
	struct csm_dp_mempool *mempool[CSM_DP_MEM_TYPE_LAST];
	struct csm_dp_rxqueue rxq[CSM_DP_RX_TYPE_LAST];
	struct csm_dp_loopback_task loopback;
	struct csm_dp_core_stats stats;
	struct work_struct alloc_work;
	unsigned int csm_dp_outbuf_drop_sync;
	csm_dp_ring_index_t csm_dp_prev_ul_prod_tail;

	struct csm_dp_buf_cntrl	*pending_packets;

#ifdef CONFIG_CSM_DP_TEST
	struct csm_dp_test_ring test_ring;
#endif
};

struct csm_dp_drv {
	struct device *dev;
	struct class *dev_class;
	dev_t devno;
	struct csm_dp_dev dp_devs[CSM_DP_MAX_NUM_DEVS];
};

int csm_dp_cdev_init(struct csm_dp_drv *pdrv);
void csm_dp_cdev_cleanup(struct csm_dp_drv *pdrv);
int csm_dp_cdev_add(struct csm_dp_dev *pdev, struct device* mhi_dev);
void csm_dp_cdev_del(struct csm_dp_dev *pdev);

int csm_dp_debugfs_init(struct csm_dp_drv *pdrv);
void csm_dp_debugfs_cleanup(struct csm_dp_drv *pdrv);


int csm_dp_tx(
	struct csm_dp_dev *pdev,
	enum csm_dp_channel ch,
	struct iovec *iov,
	unsigned int iov_nr,
	unsigned int flag,
	dma_addr_t dma_addr[]);
int csm_dp_rx_poll(struct csm_dp_dev *pdev, struct iovec *iov, size_t iov_nr);
void csm_dp_rx(struct csm_dp_dev *pdev, struct csm_dp_buf_cntrl *buf_cntrl, unsigned int length);

void csm_dp_hex_dump(unsigned char *buf, unsigned int len);

void csm_dp_mempool_dev_destroy(struct csm_dp_dev *pdev);

#endif /* __CSM_DP__ */
