/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#ifndef AMD_IP_BLOCKS_H
#define AMD_IP_BLOCKS_H

#include <amdgpu_drm.h>
#include <amdgpu.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

#include "amd_registers.h"
#include "amd_family.h"
#include "amd_PM4.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define MAX_CARDS_SUPPORTED 4

/* reset mask */
#define AMDGPU_RESET_TYPE_FULL (1 << 0) /* full adapter reset, mode1/mode2/BACO/etc. */
#define AMDGPU_RESET_TYPE_SOFT_RESET (1 << 1) /* IP level soft reset */
#define AMDGPU_RESET_TYPE_PER_QUEUE (1 << 2) /* per queue */
#define AMDGPU_RESET_TYPE_PER_PIPE (1 << 3) /* per pipe */

/* User queue */
#define   S_3F3_INHERIT_VMID_MQD_GFX(x)        (((unsigned int)(x)&0x1) << 22)/* userqueue only */
#define   S_3F3_VALID_COMPUTE(x)		(((unsigned int)(x)&0x1) << 23)/* userqueue only */
#define   S_3F3_INHERIT_VMID_MQD_COMPUTE(x)	(((unsigned int)(x)&0x1) << 30)/* userqueue only */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/*
* USERMODE_QUEUE_SIZE
* USERMODE_QUEUE_SIZE_DW
* USERMODE_QUEUE_SIZE_DW_MASK
* --------------------------------------------------------------
* USERMODE_QUEUE_SIZE        : Total size of the usermode command queue (1 MB).
* USERMODE_QUEUE_SIZE_DW     : Same size expressed in DWORDs (size / 4).
* USERMODE_QUEUE_SIZE_DW_MASK: Bitmask used for fast ring wrap-around.
*
* The ring size is a power of two, so the mask (DW_size - 1) enables:
*
*      index % queue_size  →  index & USERMODE_QUEUE_SIZE_DW_MASK
*
* This provides fast modulo arithmetic and prevents buffer overruns.
*/
#define USERMODE_QUEUE_SIZE		(PAGE_SIZE * 256)   //In bytes with total size as 1 Mbyte
#define ALIGNMENT			4096
#define DOORBELL_INDEX			4
/* SDMA per-page sub-aperture starts at dword 0x200 within any 4K page.
 * In qword units (USERQ doorbell_offset is qword-indexed): 0x100. */
#define SDMA_DOORBELL_INDEX		0x100
/* GFX UMQ per-page sub-aperture starts at dword 0x11A within any 4K page
 * (gfx_userqueue_start in amdgpu_doorbell.h). In qword units: 0x08D. */
#define GFX_DOORBELL_INDEX		0x08D
#define USERMODE_QUEUE_SIZE_DW		(USERMODE_QUEUE_SIZE >> 2)
#define USERMODE_QUEUE_SIZE_DW_MASK	(USERMODE_QUEUE_SIZE_DW - 1)

/*
* amdgpu_pkt_begin()
* ---------------------------------------------------------------
* Begins building a packet in the usermode queue.
* - __num_dw_written tracks DWORDs written in the current packet.
* - __ring_start is the starting write pointer for this packet,
*   wrapped by the ring mask to ensure it is within bounds.
*/
#define amdgpu_pkt_begin() uint32_t __num_dw_written = 0; \
	uint32_t __ring_start = *ring_context->wptr_cpu & USERMODE_QUEUE_SIZE_DW_MASK;

/*
* amdgpu_sdma_pkt_begin()
* ---------------------------------------------------------------
* Same as amdgpu_pkt_begin, but SDMA rings use a byte-based wptr.
* - Convert byte-based wptr → DWORD index (>> 2)
* - Wrap with mask to stay within the ring
* - __ring_start holds DWORD index of the write pointer
*/
#define amdgpu_sdma_pkt_begin() \
	uint32_t __num_dw_written = 0, __ring_start = 0; \
	if (ring_context->wptr_cpu) \
		*ring_context->wptr_cpu = (*ring_context->wptr_cpu & USERMODE_QUEUE_SIZE_DW_MASK) >> 2; \
	__ring_start = *ring_context->wptr_cpu;

/*
* amdgpu_pkt_add_dw(value)
* ---------------------------------------------------------------
* Writes a single DWORD into the queue at:
*
*    queue_cpu[(ring_start + num_dw_written) & mask]
*
* Masking handles ring wrap-around. Increments __num_dw_written.
*/
#define amdgpu_pkt_add_dw(value) do { \
	*(ring_context->queue_cpu + \
	((__ring_start + __num_dw_written) & USERMODE_QUEUE_SIZE_DW_MASK)) \
	= value; \
	__num_dw_written++;\
} while (0)

/*
* amdgpu_pkt_end()
* ---------------------------------------------------------------
* Finalizes the packet:
* - Advances the write pointer by the number of DWORDs written.
* - Wrap-around handled by mask.
*
* The wptr remains in DWORD units (unlike SDMA).
*/
#define amdgpu_pkt_end() do { \
	*ring_context->wptr_cpu = (*ring_context->wptr_cpu + __num_dw_written) & USERMODE_QUEUE_SIZE_DW_MASK; \
} while (0)

/*
* amdgpu_sdma_pkt_end()
* ---------------------------------------------------------------
* Finalizes SDMA packet:
* - Advance DWORD wptr
* - Wrap with mask
* - Convert back to a byte offset (<< 2) because SDMA uses byte-based wptrs.
*/
#define amdgpu_sdma_pkt_end() do { \
	*ring_context->wptr_cpu = (((*ring_context->wptr_cpu + __num_dw_written) & USERMODE_QUEUE_SIZE_DW_MASK) << 2); \
} while (0)

enum amd_ip_block_type {
	AMD_IP_GFX = 0,
	AMD_IP_COMPUTE,
	AMD_IP_DMA,
	AMD_IP_UVD,
	AMD_IP_VCE,
	AMD_IP_UVD_ENC,
	AMD_IP_VCN_DEC,
	AMD_IP_VCN_ENC,
	AMD_IP_VCN_UNIFIED = AMD_IP_VCN_ENC,
	AMD_IP_VCN_JPEG,
	AMD_IP_VPE,
	AMD_IP_MAX,
};

enum  cmd_error_type {
	CMD_STREAM_EXEC_SUCCESS,
	CMD_STREAM_EXEC_INVALID_OPCODE,
	CMD_STREAM_EXEC_INVALID_PACKET_LENGTH,
	CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE,
	CMD_STREAM_EXEC_INVALID_PACKET_EOP_QUEUE,
	CMD_STREAM_TRANS_BAD_REG_ADDRESS,
	CMD_STREAM_TRANS_BAD_MEM_ADDRESS,
	CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC,

	BACKEND_SE_GC_SHADER_EXEC_SUCCESS,
	BACKEND_SE_GC_SHADER_INVALID_SHADER,
	BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR,    /* COMPUTE_PGM */
	BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING, /* COMPUTE_PGM_RSRC */
	BACKEND_SE_GC_SHADER_INVALID_USER_DATA, /* COMPUTE_USER_DATA */

	DMA_CORRUPTED_HEADER_HANG,
	DMA_SLOW_LINEARCOPY_HANG,
	DMA_ATOMIC_LOOP_HANG
};

#define _MAX_NUM_ASIC_ID_EXCLUDE_FILTER 3

struct asic_id_filter {
	int family_id;
	int chip_id_begin;
	int chip_id_end;
};

/* expected reset result for differant ip's */
struct reset_err_result {
	int compute_reset_result;
	int gfx_reset_result;
	int sdma_reset_result;
};

struct dynamic_test{
	enum cmd_error_type test;
	const char *name;
	const char *describe;
	struct asic_id_filter exclude_filter[_MAX_NUM_ASIC_ID_EXCLUDE_FILTER];
	struct reset_err_result result;
	bool support_sdma;
};

struct amdgpu_userq_bo {
	amdgpu_bo_handle handle;
	amdgpu_va_handle va_handle;
	uint64_t mc_addr;
	uint64_t size;
	void *ptr;
};

/* Submission modes for user queues */
enum uq_submission_mode {
	UQ_SUBMIT_NORMAL,        /* Full synchronization */
	UQ_SUBMIT_NO_SYNC,       /* Skip sync for error injection */
};

#define for_each_test(t, T) for (typeof(*T) *t = T; t->name; t++)

/* set during execution */
struct amdgpu_cs_err_codes {
	int err_code_cs_submit;
	int err_code_wait_for_fence;
};

/* aux struct to hold parameters for userqueue fence */
struct amdgpu_userq_params {
	uint64_t num_fences;
	struct drm_amdgpu_userq_fence_info *fence_info;
	uint64_t job_start_write_data_va_addr;
	uint64_t job_start_write_data_val;
};

/**
 * struct amdgpu_dma_limits - hardware transfer limits per IP block
 *
 * All sizes are in BYTES.  Packet builders convert internally
 * (e.g. /4 for DWORD counts in SDMA inline data or GFX PACKET3).
 *
 * Queried once with amdgpu_dma_limits_query(), then passed to helpers.
 */
struct amdgpu_dma_limits {
	uint64_t sdma_max_bytes;        /* max SDMA write/fill/copy per packet */
	uint64_t gfx_max_bytes;         /* max GFX CP_DMA fill/copy per packet */
	uint64_t compute_max_bytes;     /* max Compute CP_DMA per packet */

	/* safe small sizes for quick smoke tests (bytes) */
	uint64_t sdma_default_bytes;
	uint64_t gfx_default_bytes;
	uint64_t compute_default_bytes;
};

/**
 * amdgpu_dma_max_bytes - return HW max transfer size for an IP type
 */
static inline uint64_t
amdgpu_dma_max_bytes(const struct amdgpu_dma_limits *lim,
		     unsigned int ip_type)
{
	switch (ip_type) {
	case AMDGPU_HW_IP_DMA:     return lim->sdma_max_bytes;
	case AMDGPU_HW_IP_GFX:     return lim->gfx_max_bytes;
	case AMDGPU_HW_IP_COMPUTE: return lim->compute_max_bytes;
	default:                    return lim->sdma_default_bytes;
	}
}

/**
 * amdgpu_dma_default_bytes - return safe small test size for an IP type
 */
static inline uint64_t
amdgpu_dma_default_bytes(const struct amdgpu_dma_limits *lim,
			 unsigned int ip_type)
{
	switch (ip_type) {
	case AMDGPU_HW_IP_DMA:     return lim->sdma_default_bytes;
	case AMDGPU_HW_IP_GFX:     return lim->gfx_default_bytes;
	case AMDGPU_HW_IP_COMPUTE: return lim->compute_default_bytes;
	default:                    return lim->sdma_default_bytes;
	}
}

/*
 * libdrm exposes this legacy name although the kernel UAPI now calls the
 * corresponding container drm_amdgpu_info_uq_metadata.
 */
struct drm_amdgpu_info_uq_fw_areas {
	union {
		struct drm_amdgpu_info_uq_metadata_gfx gfx;
		struct drm_amdgpu_info_uq_metadata_compute compute;
		struct drm_amdgpu_info_uq_metadata_sdma sdma;
	};
};

/*
 * struct amdgpu_ring_context - hand-off structure shared by the packet builder
 * and submitter roles.
 *
 * Builder role: the ip_block->funcs PM4 callbacks declared in struct
 *   amdgpu_ip_funcs (write_linear, copy_linear, const_fill, ...). A builder
 *   reads only the IN fields, populates pm4[], and reports the produced length
 *   in pm4_dw. It does not access the device, allocate buffer objects, or
 *   submit.
 *
 * Submitter role: the caller that drives a builder. It populates the IN fields
 *   required by the target packet type prior to invocation, then consumes the
 *   OUT fields (pm4[]/pm4_dw) to perform the command submission.
 *
 * This structure is the sole interface between the two roles; each field is
 * annotated with a direction:
 *   IN       set by the submitter before invocation; read by the builder.
 *   OUT      set by the builder; read by the submitter.
 *   INTERNAL owned by the submitter; the builder populates its contents only.
 */
struct amdgpu_ring_context {

	int ring_id; /* ring_id from amdgpu_query_hw_ip_info */
	int res_cnt; /* IN: number of valid entries in resources[] */

	/*
	 * IN: per-packet transfer size in bytes. The builder interprets it
	 * per packet type:
	 *   write_linear/atomic    - inlined as write_length/4 dwords into pm4[]
	 *                            (grows pm4_dw; pm4_size must accommodate it);
	 *   copy_linear/const_fill - HW DMA transfer data count;
	 *   compare/compare_pattern - verification span (num_compare = write_length/div).
	 */
	uint64_t write_length;
	uint64_t write_length2; /* IN: transfer size in bytes, second packet */
	uint32_t *pm4;		/* INTERNAL: packet buffer (submitter-owned, builder fills) */
	uint32_t pm4_size;	/* IN: capacity of pm4[] in dwords (required, non-zero) */
	bool secure;		/* IN: secure or not */
	uint32_t priority;	/* user queue priority */

	/*
	 * Operand GPU virtual addresses, assigned per packet type by the
	 * submitter prior to builder invocation:
	 *   WRITE/ATOMIC/FILL: bo_mc = destination VA
	 *   COPY:              bo_mc = source VA, bo_mc2 = destination VA
	 */
	uint64_t bo_mc;		/* IN: GPU VA, primary operand (destination, or source for copy) */
	uint64_t bo_mc2;	/* IN: GPU VA, secondary operand (destination for copy) */
	uint64_t bo_mc3;	/* GPU address of second buffer */
	uint64_t bo_mc4;	/* GPU address of second p4 packet */

	uint32_t pm4_dw;	/* OUT: actual packet size in dwords */
	uint32_t pm4_dw2;	/* OUT: actual size of second pm4 in dwords */

	volatile uint32_t *bo_cpu;	/* cpu adddress of mapped GPU buf */
	volatile uint32_t *bo2_cpu;	/* cpu adddress of mapped pm4 */
	volatile uint32_t *bo3_cpu;	/* cpu adddress of mapped GPU second buf */
	volatile uint32_t *bo4_cpu;	/* cpu adddress of mapped second pm4 */

	uint32_t bo_cpu_origin;

	amdgpu_bo_handle bo;
	amdgpu_bo_handle bo2;
	amdgpu_bo_handle bo3;
	amdgpu_bo_handle bo4;

	amdgpu_bo_handle boa_vram[2];
	amdgpu_bo_handle boa_gtt[2];

	amdgpu_context_handle context_handle;
	struct drm_amdgpu_info_hw_ip hw_ip_info;  /* result of amdgpu_query_hw_ip_info */

	amdgpu_bo_handle resources[4]; /* IN: bo_list residency set, maintained by the submitter */
	amdgpu_va_handle va_handle;    /* amdgpu_bo_alloc_and_map */
	amdgpu_va_handle va_handle2;   /* amdgpu_bo_alloc_and_map */
	amdgpu_va_handle va_handle3;   /* amdgpu_bo_alloc_and_map */
	amdgpu_va_handle va_handle4;   /* amdgpu_bo_alloc_and_map */

	struct amdgpu_cs_ib_info ib_info;     /* amdgpu_bo_list_create */
	struct amdgpu_cs_request ibs_request; /* amdgpu_cs_query_fence_status */
	struct amdgpu_cs_err_codes err_codes;

	/* User queue resources */
	struct  amdgpu_userq_bo queue;
	struct  amdgpu_userq_bo shadow;
	struct  amdgpu_userq_bo doorbell;
	struct  amdgpu_userq_bo rptr;
	struct  amdgpu_userq_bo wptr;
	struct  amdgpu_userq_bo csa;
	struct  amdgpu_userq_bo eop;

	uint32_t *queue_cpu;
	volatile uint64_t *wptr_cpu;
	volatile uint64_t *rptr_cpu;
	volatile uint64_t *doorbell_cpu;

	uint32_t db_handle;
	uint32_t queue_id;

	/*
	 * Byte offset of the wptr within its BO. 0 (default) keeps the wptr at
	 * BO offset 0. A non-zero value places the wptr at that offset inside a
	 * one-page wptr BO, exercising the kernel's handling of a wptr that is
	 * not at the start of the BO.
	 */
	uint64_t wptr_offset;

	uint32_t timeline_syncobj_handle;
	uint64_t point;
	bool user_queue;
	uint64_t time_out;
	enum uq_submission_mode submit_mode;
	uint32_t max_num_fences_fwm;
	struct amdgpu_userq_params *userq_params;

	struct drm_amdgpu_info_uq_fw_areas info;
};

struct amdgpu_cmd_base;

struct amdgpu_ip_funcs {
	uint32_t	family_id;
	uint32_t	chip_external_rev;
	uint32_t	chip_rev;
	uint32_t	align_mask;
	uint32_t	nop;
	uint32_t	deadbeaf;
	uint32_t	pattern;
	/* functions */
	int (*write_linear)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*bad_write_linear)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context,
				uint32_t *pm4_dw, unsigned int cmd_error);
	int (*write_linear_atomic)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*const_fill)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*copy_linear)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*compare)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, int div);
	int (*compare_pattern)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, int div);
	int (*get_reg_offset)(enum general_reg reg);
	int (*wait_reg_mem)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);

	/* userq functions */
	void (*userq_create)(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt, unsigned int type);
	int (*userq_submit)(amdgpu_device_handle device, struct amdgpu_ring_context *ring_context, unsigned int ip_type, uint64_t mc_address);
	void (*userq_destroy)(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt, unsigned int type);

	/* program minimal compute pipeline for a raw PM4 launch */
	void (*gfx_program_compute)(
		const struct amdgpu_ip_funcs *funcs,
		struct amdgpu_cmd_base *base,
		uint64_t code_addr,
		uint64_t user_data0_addr,
		uint32_t rsrc1_dw,
		uint32_t rsrc2_or_tmp,
		uint32_t thr_x, uint32_t thr_y, uint32_t thr_z
	);

	/* launch a direct dispatch (grid + family flags) */
	void (*gfx_dispatch_direct)(
		const struct amdgpu_ip_funcs *funcs,
		struct amdgpu_cmd_base *base,
		uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
		uint32_t flags
	);

	/* WRITE_DATA with WR_CONFIRM (or family equivalent) */
	void (*gfx_write_confirm)(
		const struct amdgpu_ip_funcs *funcs,
		struct amdgpu_cmd_base *base,
		uint64_t dst_addr, uint32_t value
	);

	/* Emit 'count' PACKET3 NOPs on the GFX ring. */
	void (*gfx_emit_nops)(
		struct amdgpu_cmd_base *base,
		uint32_t count
	);

	/*
	 * Emit a PACKET3 WRITE_DATA targeting a memory address.
	 *
	 * @engine_sel: engine selection per packet encoding
	 *              (e.g., ME=0, PFP=1, CE=2, MEC=3 when applicable)
	 * @dst_addr:   destination GPU address (64-bit)
	 * @value:      32-bit value to write
	 * @wr_confirm: whether to set WR_CONFIRM in the packet
	 */
	void (*gfx_write_data_mem)(
		const struct amdgpu_ip_funcs *funcs,
		struct amdgpu_cmd_base *base,
		uint32_t engine_sel,
		uint64_t dst_addr,
		uint32_t value,
		bool wr_confirm
	);

	/*
	 * Emit PACKET3_WAIT_REG_MEM for deadlock/hang tests. Uses FUNCTION(4)
	 * for != comparison and polls ring_context->bo_mc (initialised to 0)
	 * for a value that never arrives, hanging the queue for reset testing.
	 */
	int (*wait_reg_mem_hang)(
		const struct amdgpu_ip_funcs *func,
		const struct amdgpu_ring_context *context,
		uint32_t *pm4_dw
	);

	/*
	 * Emit an invalid opcode followed by a WAIT_REG_MEM hang for priv-fault
	 * tests: the bad opcode raises CP_BAD_OPCODE_ERROR, then the queue hangs
	 * cleanly so the driver recovers it with a per-queue reset.
	 */
	int (*priv_fault_hang)(
		const struct amdgpu_ip_funcs *func,
		const struct amdgpu_ring_context *context,
		uint32_t *pm4_dw
	);

	/*
	 * Emit a privileged INDIRECT_BUFFER (PRIV bit) from an unprivileged user
	 * queue followed by a WAIT_REG_MEM hang: this raises a priv-instruction
	 * fault and the queue then hangs cleanly, so the driver recovers it with a
	 * per-queue reset.
	 */
	int (*priv_inst_hang)(
		const struct amdgpu_ip_funcs *func,
		const struct amdgpu_ring_context *context,
		uint32_t *pm4_dw
	);

	/*
	 * Emit a bad opcode with a non-zero count field and no body: the pipe HW
	 * cannot find the packet end and stalls mid-packet, so a per-queue (vmid)
	 * reset cannot recover it -- only a gfx pipe reset can.
	 */
	int (*priv_fault_badcount_hang)(
		const struct amdgpu_ip_funcs *func,
		const struct amdgpu_ring_context *context,
		uint32_t *pm4_dw
	);

};

extern const struct amdgpu_ip_block_version gfx_v6_0_ip_block;

struct amdgpu_ip_block_version {
	const enum amd_ip_block_type type;
	const int major;
	const int minor;
	const int rev;
	struct amdgpu_ip_funcs *funcs;
};

/* global holder for the array of in use ip blocks */

struct amdgpu_ip_blocks_device {
	struct amdgpu_ip_block_version *ip_blocks[AMD_IP_MAX];
	int			num_ip_blocks;
};

struct chip_info {
	const char *name;
	enum radeon_family family;
	enum chip_class chip_class;
	amdgpu_device_handle dev;
};

struct pci_addr {
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int function;
};

extern  struct amdgpu_ip_blocks_device amdgpu_ips;
extern const struct chip_info  *g_pChip;
int
setup_amdgpu_ip_blocks(uint32_t major, uint32_t minor, struct amdgpu_gpu_info *amdinfo,
		       amdgpu_device_handle device);

const struct amdgpu_ip_block_version *
get_ip_block(amdgpu_device_handle device, enum amd_ip_block_type type);

struct amdgpu_cmd_base {
	uint32_t cdw;  /* Number of used dwords. */
	uint32_t max_dw; /* Maximum number of dwords. */
	uint32_t *buf; /* The base pointer of the chunk. */
	bool is_assigned_buf;

	/* functions */
	int (*allocate_buf)(struct amdgpu_cmd_base  *base, uint32_t size);
	int (*attach_buf)(struct amdgpu_cmd_base  *base, void *ptr, uint32_t size_bytes);
	void (*emit)(struct amdgpu_cmd_base  *base, uint32_t value);
	void (*emit_aligned)(struct amdgpu_cmd_base  *base, uint32_t mask, uint32_t value);
	void (*emit_repeat)(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t number_of_times);
	void (*emit_at_offset)(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t offset_dwords);
	void (*emit_buf)(struct amdgpu_cmd_base  *base, const void *ptr, uint32_t offset_bytes, uint32_t size_bytes);
	void (*ib_fill_nop)(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords);
	void (*ib_fill_chain)(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
				uint64_t next_va, uint32_t next_size_dw);
	void (*ib_fill_writedata)(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
				uint64_t dst_va, uint32_t dst_size);
	void (*ib_fill_writedata_chain)(struct amdgpu_cmd_base *base,
				uint32_t *ib, uint32_t ib_dwords,
					uint64_t dst_va, uint32_t dst_size,
					uint64_t next_va, uint32_t next_size_dw);
	void (*ib_fill_wait)(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
			     uint64_t signal_va, uint64_t next_ib_va,
			     uint32_t next_ib_size_dw);

};

struct amdgpu_cmd_base *get_cmd_base(void);

void free_cmd_base(struct amdgpu_cmd_base *base);

int
amdgpu_open_devices(bool open_render_node, int max_cards_supported, int drm_amdgpu_fds[]);

void
asic_rings_readness(amdgpu_device_handle device_handle, uint32_t mask, bool arr[AMD_IP_MAX]);

void asic_userq_readiness(amdgpu_device_handle device_handle, bool arr[AMD_IP_MAX]);

bool
is_reset_enable(enum amd_ip_block_type ip_type, uint32_t reset_type, const struct pci_addr *pci);

int
get_pci_addr_from_fd(int fd, struct pci_addr *pci);

bool
is_support_page_queue(enum amd_ip_block_type ip_type, const struct pci_addr *pci);

int
find_dri_id_by_pci(const struct pci_addr *pci);

long
amdgpu_get_ip_schedule_mask(const struct pci_addr *pci, enum amd_ip_block_type ip_type, char *sysfs_path);

bool is_spx_mode(const struct pci_addr *pci);

int
get_dri_index_from_device(amdgpu_device_handle device, int fd);
/**
 * is_apu - Check if the GPU is an APU (accelerated processing unit)
 * @info: Pointer to amdgpu_gpu_info structure containing device info
 *
 * Returns true if the device is an APU, false otherwise.
 */
bool is_apu(const struct amdgpu_gpu_info *info);

const char *cmd_get_ip_name(enum amd_ip_block_type ip_type);

int
amdgpu_bo_alloc_and_map_uq(amdgpu_device_handle device_handle, unsigned int size,
			       unsigned int alignment, unsigned int heap, uint64_t alloc_flags,
			       uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			       uint64_t *mc_address, amdgpu_va_handle *va_handle,
			       uint32_t timeline_syncobj_handle, uint64_t point);

int
amdgpu_timeline_syncobj_wait(amdgpu_device_handle device_handle,
				 uint32_t timeline_syncobj_handle, uint64_t point);

void
amd_ip_blocks_ex_init(struct amdgpu_ip_funcs *funcs);

#endif
