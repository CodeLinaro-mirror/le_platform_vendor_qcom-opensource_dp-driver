/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#ifndef __CSM_DP_IOCTL_H__
#define __CSM_DP_IOCTL_H__

#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/uio.h>
#else
#include <sys/uio.h>
#endif

#define CSM_DP_MAX_IOV_SIZE	128
#define CSM_DP_MAX_SG_IOV_SIZE	128

#define CSM_DP_IOCTL_BASE			'f'

#define CSM_DP_IOCTL_MEMPOOL_ALLOC	\
		_IOWR(CSM_DP_IOCTL_BASE, 1, struct csm_dp_ioctl_mempool_alloc)

#define CSM_DP_IOCTL_MEMPOOL_GET_CONFIG	\
		_IOWR(CSM_DP_IOCTL_BASE, 2, struct csm_dp_ioctl_getcfg)

#define CSM_DP_IOCTL_RX_GET_CONFIG	\
		_IOWR(CSM_DP_IOCTL_BASE, 3, struct csm_dp_ioctl_getcfg)

#define CSM_DP_IOCTL_TX			\
		_IOWR(CSM_DP_IOCTL_BASE, 4, struct csm_dp_ioctl_tx)

#define CSM_DP_IOCTL_SG_TX		\
		_IOWR(CSM_DP_IOCTL_BASE, 5, struct csm_dp_ioctl_tx)

/* obsolete */
#define CSM_DP_IOCTL_TX_MODE_CONFIG	\
		_IOWR(CSM_DP_IOCTL_BASE, 6, unsigned int)

#define CSM_DP_IOCTL_RX_POLL	\
		_IOWR(CSM_DP_IOCTL_BASE, 7, struct iovec)

#define CSM_DP_IOCTL_GET_STATS	\
		_IOWR(CSM_DP_IOCTL_BASE, 8, struct csm_dp_ioctl_getstats)

#define CSM_DP_IOCTL_TX_FLAG_MIRROR 0x1

enum csm_dp_mem_type {
	CSM_DP_MEM_TYPE_DL_CONTROL,
	CSM_DP_MEM_TYPE_DL_DATA,
	CSM_DP_MEM_TYPE_UL_CONTROL,
	CSM_DP_MEM_TYPE_UL_DATA,
	CSM_DP_MEM_TYPE_LAST,
};

enum csm_dp_mmap_type {
	CSM_DP_MMAP_TYPE_MEM,
	CSM_DP_MMAP_TYPE_RING,
	CSM_DP_MMAP_TYPE_LAST,
};

enum csm_dp_rx_type {
	CSM_DP_RX_TYPE_FAPI,
	CSM_DP_RX_TYPE_LAST,
};

#define CSM_DP_BUFFER_FENCE_SIG 0xDEADFACE
#define CSM_DP_BUFFER_SIG       0xDAC0FFEE
#define CSM_DP_BUFFER_FENCING   1

/*
 * A buffer control is an area with size of L1_CACHE_BYTES (64 bytes for arm64).
 * It is placed at the beginning of a buffer.
 * csm_dp_buf_cntrl is placed at the control area. The last 4 bytes of the area is a fence defined
 * as CSM_DP_BUFFER_FENCE_SIG.
 * The size of csm_dp_buf_cntrl should be less than L1_CACHE_BYTES.
 * User data is placed after the control area of L1_CACHE_BYTES size.
 */
#define CSM_DP_L1_CACHE_BYTES 64  /*
				   * CSM_DP_L1_CACHE_BYTES is the same as
				   * L1_CACHE_BYTES.
				   * csm_dp_ioctl.h is included in
				   * the applications,
				   * The symbol L1_CACHE_BYTES is defined in the
				   * kernel, not be used here. Therefore,
				   * it is redefined.
				   */
/*
 * xmit_status definition
 * If xmit errors, defined as -(error code)
 */
#define CSM_DP_XMIT_IN_PROGRESS (1)
#define CSM_DP_XMIT_OK		0

/*
 * maximum mtu size for CSM DP application, including csm_dp header
 * Note, need to make sure both sides in sync between Host and Q6
 */
#define CSM_DP_MAX_DL_MSG_LEN   ((2 * 1024 * 1024) - CSM_DP_L1_CACHE_BYTES)
#define CSM_DP_MAX_UL_MSG_LEN   CSM_DP_MAX_DL_MSG_LEN

#define CSM_DP_DEFAULT_UL_BUF_SIZE	(512 * 1024)
#define CSM_DP_DEFAULT_UL_BUF_CNT	2500

#define CSM_DP_INVALID_BUF_INDEX ((uint32_t)-1)

struct csm_dp_buf_cntrl {
	uint32_t signature;
	uint32_t state;
	int32_t xmit_status;
	uint16_t mem_type;	/* enum csm_dp_mem_type */
	uint32_t buf_index;
	struct csm_dp_buf_cntrl *next;	/* used by kernel only */
	uint32_t next_buf_index;	/* used in Rx, kernel writes, user reads */
	uint32_t len;			/* used in Rx, kernel writes, user reads */
	struct csm_dp_buf_cntrl *next_packet;	/* used in Rx only */
	uint16_t buf_count;	/* used in Rx only */
	unsigned char spare[CSM_DP_L1_CACHE_BYTES
		- sizeof(uint32_t) /* signature */
		- sizeof(uint32_t) /* state */
		- sizeof(int32_t) /* xmit_status */
		- sizeof(uint16_t) /* mem_type */
		- sizeof(uint32_t) /* buf_index */
		- sizeof(struct csm_dp_buf_cntrl *) /* next */
		- sizeof(uint32_t) /* next_buf_index */
		- sizeof(uint32_t) /* len */
		- sizeof(struct csm_dp_buf_cntrl *) /* next_packet */
		- sizeof(uint16_t) /* buf_count */
		- sizeof(uint32_t)];/* fence */
	uint32_t fence; /* must be last */
} __attribute__((packed));

enum csm_dp_buf_state {
	CSM_DP_BUF_STATE_KERNEL_FREE,
	CSM_DP_BUF_STATE_KERNEL_ALLOC_RECV_DMA,
	CSM_DP_BUF_STATE_KERNEL_RECVCMP_MSGQ_TO_APP,
	CSM_DP_BUF_STATE_KERNEL_XMIT_DMA,
	CSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP,
	CSM_DP_BUF_STATE_USER_FREE,
	CSM_DP_BUF_STATE_USER_ALLOC,
	CSM_DP_BUF_STATE_USER_RECV,
	CSM_DP_BUF_STATE_LAST,
};

typedef unsigned long csm_dp_ring_element_data_t;
typedef unsigned int csm_dp_ring_index_t;

struct csm_dp_ring_element {
	uint64_t element_ctrl;	/* 1 entry not valid, 0 valid */
				/* Other bits for control flags: tbd */

	csm_dp_ring_element_data_t element_data;
				/*
				 * If the ring is used for
				 * csm dp buffer management,
				 * ring data is pointing to
				 * user data
				 */
};

typedef struct csm_dp_ring_element csm_dp_ring_element_t;

struct csm_dp_mmap_cfg {
	__u64 length;	/* length parameter for mmap */
	__u32 cookie;	/* last parameter for mmap */
};

struct csm_dp_ring_cfg {
	struct csm_dp_mmap_cfg mmap;	/* mmap parameters */
	__u32 size;			/* ring size */
	__u32 prod_head_off;		/* page offset of prod_head */
	__u32 prod_tail_off;		/* page offset of prod_tail */
	__u32 cons_head_off;		/* page offset of cons_head */
	__u32 cons_tail_off;		/* page offset of cons_tail */
	__u32 ringbuf_off;		/* page offset of ring buffer */
};

struct csm_dp_mem_cfg {
	struct csm_dp_mmap_cfg mmap;	/* mmap parameters */
	__u32 buf_sz;			/* size of buffer for user data */
	__u32 buf_cnt;			/* number of buffer */
	__u32 buf_overhead_sz;		/*
					 * size of buffer overhead,
					 * on top of buf_sz.
					 */
	__u32 cluster_size;		/* cluster size in bytes.
					 * number of buffers in a cluster:
					 *  cluster_size /(buf_overhead_sz +
					 *                 buf_sz)
					 * A buffer starts at beginning of
					 * a cluster. Spared space with
					 * size less than (buf_overhead_sz
					 *          +  buf_sz) at end of
					 * a cluster is not used.
					 */
	__u32 num_cluster;		/* number of cluster */
	__u32 buf_per_cluster;		/* number of buffers per cluster  */
};

struct csm_dp_mempool_cfg {
	enum csm_dp_mem_type type;
	struct csm_dp_mem_cfg mem;
	struct csm_dp_ring_cfg ring;
};

enum csm_dp_channel {
	CSM_DP_CH_CONTROL,
	CSM_DP_CH_DATA,
};

struct csm_dp_ioctl_mempool_alloc {
	__u32 type;		/* type defined in enum csm_dp_mem_type */
	__u32 buf_sz;		/* size of buffer */
	__u32 buf_num;		/* number of buffer */
	struct csm_dp_mempool_cfg *cfg;	/* for kernel to return config info */
};

struct csm_dp_ioctl_getcfg {
	__u32 type;
	void *cfg;
};

struct csm_dp_ioctl_tx {
	enum csm_dp_channel ch;
	struct iovec iov;
	__u32 flags;	/* CSM_DP_IOCTL_TX_FLAG_xxx */
};

struct csm_dp_ioctl_getstats {
	enum csm_dp_channel ch;	/* IN param, set by caller */
	__u64 tx_cnt;
	__u64 tx_acked;
	__u64 rx_cnt;
	__u64 reserved[10];	/* for future use */
};

static inline int csm_dp_mem_type_is_valid(enum csm_dp_mem_type type)
{
	return (type >= 0 && type < CSM_DP_MEM_TYPE_LAST);
}

static inline const char *csm_dp_mem_type_to_str(enum csm_dp_mem_type type)
{
	switch (type) {
	case CSM_DP_MEM_TYPE_DL_CONTROL: return "DL_CTRL";
	case CSM_DP_MEM_TYPE_DL_DATA: return "DL_DATA";
	case CSM_DP_MEM_TYPE_UL_CONTROL: return "UL_CTRL";
	case CSM_DP_MEM_TYPE_UL_DATA: return "UL_DATA";
	default: return "unknown";
	}
}

static inline int csm_dp_mmap_type_is_valid(enum csm_dp_mmap_type type)
{
	return (type >= 0 && type < CSM_DP_MMAP_TYPE_LAST);
}

static inline const char *csm_dp_mmap_type_to_str(enum csm_dp_mmap_type type)
{
	switch (type) {
	case CSM_DP_MMAP_TYPE_MEM: return "Memory";
	case CSM_DP_MMAP_TYPE_RING: return "Ring";
	default: return "unknown";
	}
}

static inline int csm_dp_rx_type_is_valid(enum csm_dp_rx_type type)
{
	return (type >= 0 && type < CSM_DP_RX_TYPE_LAST);
}

static inline const char *csm_dp_rx_type_to_str(enum csm_dp_rx_type type)
{
	switch (type) {
	case CSM_DP_RX_TYPE_FAPI: return "FAPI";
	default: return "unknown";
	}
}

static inline const char *csm_dp_buf_state_to_str(enum csm_dp_buf_state state)
{
	switch (state) {
	case CSM_DP_BUF_STATE_KERNEL_FREE:
		return "KERNEL FREE";
	case CSM_DP_BUF_STATE_KERNEL_ALLOC_RECV_DMA:
		return "KERNEL ALLOC RECV DMA";
	case CSM_DP_BUF_STATE_KERNEL_RECVCMP_MSGQ_TO_APP:
		return "KERNEL RECV CMP MSGQ TO APP";
	case CSM_DP_BUF_STATE_KERNEL_XMIT_DMA:
		return "KERNEL XMIT DMA";
	case CSM_DP_BUF_STATE_KERNEL_XMIT_DMA_COMP:
		return "KERNEL XMIT DMA COMP";
	case CSM_DP_BUF_STATE_USER_FREE:
		return "USER FREE";
	case CSM_DP_BUF_STATE_USER_ALLOC:
		return "USER ALLOC";
	case CSM_DP_BUF_STATE_USER_RECV:
		return "USER RECV";
	case CSM_DP_BUF_STATE_LAST:
	default:
		return "unknown";
	};
}

static inline bool csm_dp_mem_type_is_ul(enum csm_dp_mem_type type)
{
	return type == CSM_DP_MEM_TYPE_UL_CONTROL || type == CSM_DP_MEM_TYPE_UL_DATA;
}

static inline bool csm_dp_mem_type_is_dl(enum csm_dp_mem_type type)
{
	return type == CSM_DP_MEM_TYPE_DL_CONTROL || type == CSM_DP_MEM_TYPE_DL_DATA;
}

#endif /* __CSM_DP_IOCTL_H__ */
