/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd. */

#ifndef IGT_PANTHOR_H
#define IGT_PANTHOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "panthor_drm.h"

struct panthor_bo {
	int handle;
	uint64_t offset;
	uint64_t size;
	void *map;
};

uint32_t igt_panthor_query(int fd, int32_t type, void *data, size_t size, int err);
void igt_panthor_vm_create_userva_range(int fd, uint32_t *vm_id, int err, uint64_t *uva_range);
void igt_panthor_vm_destroy(int fd, uint32_t vm_id, int err);
void igt_panthor_vm_bind_offset(int fd, uint32_t vm_id, uint32_t bo_handle, uint64_t va,
				uint64_t size, uint64_t ofsfet, uint32_t flags, int err);
void igt_panthor_bo_create(int fd, struct panthor_bo *bo, uint64_t size, uint32_t flags, int err);
uint64_t igt_panthor_bo_mmap_offset(int fd, uint32_t handle, int err);
void igt_panthor_free_bo(int fd, struct panthor_bo *bo);
void igt_panthor_bo_create_mapped(int fd, struct panthor_bo *bo, uint64_t size,
				  uint32_t flags, int err);
void *igt_panthor_mmap_bo(int fd, uint32_t handle, uint64_t size,
			  unsigned int prot, uint64_t offset);
void igt_panthor_group_create(int fd, struct drm_panthor_group_create *group_create, int err);
void igt_panthor_group_destroy(int fd, uint32_t group_handle, int err);
void igt_panthor_group_submit(int fd, struct drm_panthor_group_submit *group_submit, int err);
uint32_t igt_panthor_group_create_simple(int fd, uint32_t vm_id, int err);
void igt_panthor_group_submit_simple(int fd, uint32_t group_handle,
				     uint32_t queue_index, uint64_t stream_addr,
				     uint32_t stream_size, uint32_t syncobj_handle,
				     int err);
int igt_panthor_get_arch(int fd);

static inline void igt_panthor_vm_bind(int fd, uint32_t vm_id, uint32_t bo_handle,
				       uint64_t va, uint64_t size, uint32_t flags, int err)
{
	igt_panthor_vm_bind_offset(fd, vm_id, bo_handle, va, size, 0, flags, err);
}

static inline void igt_panthor_vm_create(int fd, uint32_t *vm_id, int err)
{
	igt_panthor_vm_create_userva_range(fd, vm_id, err, 0);
}

static inline void igt_panthor_vm_bind_sparse(int fd, uint32_t vm_id,
					      uint64_t va, uint64_t size, int err)
{
	uint32_t flags = DRM_PANTHOR_VM_BIND_OP_TYPE_MAP |
			 DRM_PANTHOR_VM_BIND_OP_MAP_SPARSE |
			 DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC;

	igt_panthor_vm_bind_offset(fd, vm_id, 0, va, size, 0, flags, err);
}

enum cs_opcode {
	CS_OPCODE_NOP = 0,
	CS_OPCODE_MOVE48 = 1,
	CS_OPCODE_MOVE32 = 2,
	CS_OPCODE_WAIT = 3,
	CS_OPCODE_RUN_COMPUTE = 4,
	CS_OPCODE_STM = 21,
	CS_OPCODE_SET_SB_ENTRY = 23,
	CS_OPCODE_CALL = 32,
	CS_OPCODE_REQ_RESOURCE = 34,
	CS_OPCODE_FLUSH_CACHE = 36,
	CS_OPCODE_ERROR_BARRIER = 47,
};

enum cs_flush_mode {
	CS_FLUSH_MODE_NONE = 0,
	CS_FLUSH_MODE_CLEAN = 1,
	CS_FLUSH_MODE_INVALIDATE = 2,
	CS_FLUSH_MODE_CLEAN_AND_INVALIDATE = 3,
};

enum cs_task_axis {
	CS_TASK_AXIS_X = 0,
	CS_TASK_AXIS_Y = 1,
	CS_TASK_AXIS_Z = 2,
};

/* There's no plan to support big endian in the UMD, so keep
 * things simple and don't bother supporting it.
 */
void igt_panthor_skip_on_big_endian(void);

struct cs_instr {
	union {
		struct {
			uint64_t data: 56;
			uint64_t opcode: 8;
		} any;
		struct {
			uint64_t unused: 56;
			uint64_t opcode: 8;
		} nop;
		struct {
			uint64_t immediate: 48;
			uint64_t dest: 8;
			uint64_t opcode: 8;
		} move48;
		struct {
			uint64_t immediate: 32;
			uint64_t unused: 16;
			uint64_t dest: 8;
			uint64_t opcode: 8;
		} move32;
		struct {
			uint64_t unused0: 16;
			uint64_t wait_mask: 16;
			uint64_t progress_inc: 1;
			uint64_t unused1: 23;
			uint64_t opcode: 8;
		} wait;
		struct {
			uint64_t task_increment: 14;
			uint64_t task_axis: 2;
			uint64_t unused0: 16;
			uint64_t progress: 1;
			uint64_t unused1: 7;
			uint64_t srt_entry: 2;
			uint64_t spd_entry: 2;
			uint64_t tsd_entry: 2;
			uint64_t fau_entry: 2;
			uint64_t unused2: 8;
			uint64_t opcode: 8;
		} run_compute;
		struct {
			uint64_t offset: 16;
			uint64_t mask: 16;
			uint64_t unused: 8;
			uint64_t address: 8;
			uint64_t src: 8;
			uint64_t opcode: 8;
		} stm;
		struct {
			uint64_t ep_entry: 4;
			uint64_t other_entry: 4;
			uint64_t unused: 48;
			uint64_t opcode: 8;
		} set_sb_entry;
		struct {
			uint64_t unused0: 32;
			uint64_t len: 8;
			uint64_t addr: 8;
			uint64_t unused1: 8;
			uint64_t opcode: 8;
		} call;
		struct {
			uint64_t compute: 1;
			uint64_t fragment: 1;
			uint64_t tiler: 1;
			uint64_t idvs: 1;
			uint64_t unused: 52;
			uint64_t opcode: 8;
		} req_resource;
		struct {
			uint64_t l2_mode: 4;
			uint64_t lsc_mode: 4;
			uint64_t other_mode: 4;
			uint64_t unused0: 4;
			uint64_t wait_mask: 16;
			uint64_t unused1: 8;
			uint64_t flush_id: 8;
			uint64_t signal_slot: 4;
			uint64_t unused2: 4;
			uint64_t opcode: 8;
		} flush;
		struct {
			uint64_t unused: 56;
			uint64_t opcode: 8;
		} error_barrier;
		uint64_t raw;
	};
};

static inline uint64_t
cs_nop(void)
{
	struct cs_instr instr = {
		.nop = {
			.opcode = CS_OPCODE_NOP,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_mov48(uint8_t dst, uint64_t imm)
{
	struct cs_instr instr = {
		.move48 = {
			.opcode = CS_OPCODE_MOVE48,
			.dest = dst,
			.immediate = (uint64_t)imm & 0xffffffffffffull,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_mov32(uint8_t dst, uint32_t imm)
{
	struct cs_instr instr = {
		.move32 = {
			.opcode = CS_OPCODE_MOVE32,
			.dest = dst,
			.immediate = (uint32_t)imm,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_wait(uint16_t wait_mask, bool progress_inc)
{
	struct cs_instr instr = {
		.wait = {
			.opcode = CS_OPCODE_WAIT,
			.wait_mask = wait_mask,
			.progress_inc = progress_inc,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_run_compute(uint16_t task_increment, enum cs_task_axis task_axis, bool progress,
	       uint32_t srt_entry, uint32_t spd_entry, uint32_t tsd_entry,
	       uint32_t fau_entry)
{
	struct cs_instr instr = {
		.run_compute = {
			.task_increment = task_increment,
			.task_axis = task_axis,
			.progress = progress,
			.srt_entry = srt_entry,
			.spd_entry = spd_entry,
			.tsd_entry = tsd_entry,
			.fau_entry = fau_entry,
			.opcode = CS_OPCODE_RUN_COMPUTE,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_stm(uint8_t address, uint8_t src, uint16_t mask, int16_t offset)
{
	struct cs_instr instr = {
		.stm = {
			.opcode = CS_OPCODE_STM,
			.offset = (uint16_t)offset,
			.mask = mask,
			.src = src,
			.address = address,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_stm32(uint8_t address, uint8_t src, int16_t offset)
{
	return cs_stm(address, src, 0x1, offset);
}

static inline uint64_t
cs_stm64(uint8_t address, uint8_t src, int16_t offset)
{
	return cs_stm(address, src, 0x3, offset);
}

static inline uint64_t
cs_set_sb_entry(uint32_t ep_entry, uint32_t other_entry)
{
	struct cs_instr instr = {
		.set_sb_entry = {
			.ep_entry = ep_entry,
			.other_entry = other_entry,
			.opcode = CS_OPCODE_SET_SB_ENTRY,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_call(uint8_t addr, uint8_t len)
{
	struct cs_instr instr = {
		.call = {
			.addr = addr,
			.len = len,
			.opcode = CS_OPCODE_CALL,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_req_resource(bool compute, bool fragment, bool tiler, bool idvs)
{
	struct cs_instr instr = {
		.req_resource = {
			.compute = compute,
			.fragment = fragment,
			.tiler = tiler,
			.idvs = idvs,
			.opcode = CS_OPCODE_REQ_RESOURCE,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_flush(enum cs_flush_mode l2_mode,
	 enum cs_flush_mode lsc_mode,
	 enum cs_flush_mode other_mode,
	 uint16_t wait_mask,
	 uint8_t flush_id,
	 uint8_t signal_slot)
{
	struct cs_instr instr = {
		.flush = {
			.l2_mode = l2_mode,
			.lsc_mode = lsc_mode,
			.other_mode = other_mode,
			.wait_mask = wait_mask,
			.flush_id = flush_id,
			.signal_slot = signal_slot,
			.opcode = CS_OPCODE_FLUSH_CACHE,
		},
	};

	return instr.raw;
}

static inline uint64_t
cs_error_barrier(void)
{
	struct cs_instr instr = {
		.error_barrier = {
			.opcode = CS_OPCODE_ERROR_BARRIER,
		},
	};

	return instr.raw;
}

struct gpu_src_reg {
	uint8_t reg;
	bool last;
	uint8_t rep;
};

struct gpu_dst_reg {
	uint8_t reg;
};

enum gpu_flow_ctrl {
	FLOW_NONE = 0,
	FLOW_WAIT0 = 0x1,
	FLOW_WAIT1 = 0x2,
	FLOW_WAIT01 = 0x3,
	FLOW_WAIT2 = 0x4,
	FLOW_WAIT02 = 0x5,
	FLOW_WAIT12 = 0x6,
	FLOW_WAIT012 = 0x7,
	FLOW_WAIT0126 = 0x8,
	FLOW_WAIT = 0x9,
	FLOW_RECONVERGE = 0xa,
	FLOW_WAIT_RESOURCE = 0xc,
	FLOW_DISCARD = 0xd,
	FLOW_END = 0xf,
};

enum gpu_reg_width {
	REG_WIDTH_LO16 = 0x1,
	REG_WIDTH_HI16 = 0x2,
	REG_WIDTH_32 = 0x3,
};

static inline struct gpu_dst_reg
gpu_dst_reg(uint8_t reg)
{
	struct gpu_dst_reg ret = {
		.reg = reg
	};
	return ret;
}

static inline struct gpu_src_reg
gpu_src_zero(void)
{
	struct gpu_src_reg ret = {
		.reg = 255, /* architecturally invalid */
		.last = false,
		.rep = (0x3 << 6),
	};
	return ret;
}

static inline struct gpu_src_reg
gpu_src_reg(uint8_t reg)
{
	struct gpu_src_reg ret = {
		.reg = reg,
		.last = false,
		.rep = reg,
	};
	return ret;
}

static inline struct gpu_src_reg
gpu_src_reg_last(uint8_t reg)
{
	struct gpu_src_reg ret = {
		.reg = reg,
		.last = true,
		.rep = (reg | (1 << 6)),
	};
	return ret;
}

#define ins(x, v, s) x |= ((uint64_t) v << s)

static inline uint64_t
gpu_mov_imm_i32(struct gpu_dst_reg dst, int32_t imm, enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	ins(ret, gpu_src_zero().rep, 0);
	ins(ret, imm, 8);
	ins(ret, dst.reg, 40);
	ins(ret, REG_WIDTH_32, 46);
	ins(ret, 0x0, 48); /* opcode */
	ins(ret, 0x11, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

static inline uint64_t
gpu_lshift_or_i32(struct gpu_dst_reg dst, struct gpu_src_reg src,
		  struct gpu_src_reg shift, struct gpu_src_reg or,
		  enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	ins(ret, src.rep, 0);
	ins(ret, shift.rep, 8);
	ins(ret, or.rep, 16);
	ins(ret, 0x1, 24); /* logicop = or */
	ins(ret, 0x4, 26); /* lanes = b0 */
	ins(ret, 0x0, 30); /* dst.not = false */
	ins(ret, 0x1, 32); /* shift = left */
	ins(ret, 0x0, 35); /* src2.not = false */
	ins(ret, 0x0, 36); /* widen = false */
	ins(ret, dst.reg, 40);
	ins(ret, REG_WIDTH_32, 46);
	ins(ret, 0x4, 48); /* opcode */
	ins(ret, 0xb, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

static inline uint64_t
gpu_iadd_u32(struct gpu_dst_reg dst, struct gpu_src_reg left,
	     struct gpu_src_reg right, enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	ins(ret, left.rep, 0);
	ins(ret, right.rep, 8);
	ins(ret, 0x0, 16); /* opcode */
	ins(ret, 0x0, 26); /* widen1 */
	ins(ret, 0x0, 30); /* saturate */
	ins(ret, 0x0, 36); /* widen0 */
	ins(ret, dst.reg, 40);
	ins(ret, REG_WIDTH_32, 46);
	ins(ret, 0x0, 48); /* data type */
	ins(ret, 0xa, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

static inline uint64_t
gpu_imul_u32(struct gpu_dst_reg dst, struct gpu_src_reg left,
	     struct gpu_src_reg right, enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	ins(ret, left.rep, 0);
	ins(ret, right.rep, 8);
	ins(ret, 0xa, 16); /* opcode */
	ins(ret, 0x0, 26); /* widen1 */
	ins(ret, 0x0, 30); /* saturate */
	ins(ret, 0x0, 36); /* widen0 */
	ins(ret, dst.reg, 40);
	ins(ret, REG_WIDTH_32, 46);
	ins(ret, 0x0, 48); /* data type */
	ins(ret, 0xa, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

static inline uint64_t
gpu_load_i32(struct gpu_dst_reg dst, struct gpu_src_reg addr_lo,
	     struct gpu_src_reg addr_hi, uint16_t offset,
	     uint8_t sb_slot, enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	igt_assert_eq(addr_hi.reg, addr_lo.reg + 1);
	ins(ret, addr_lo.rep, 0);
	ins(ret, offset, 8);
	ins(ret, 0x0, 24); /* access mode */
	ins(ret, REG_WIDTH_32, 27);
	ins(ret, sb_slot, 30);
	ins(ret, 0x1, 33); /* staging register count */
	ins(ret, 0x0, 36); /* load 32 */
	ins(ret, 0x1, 39); /* no sign extension */
	ins(ret, dst.reg, 40); /* base staging register */
	ins(ret, 0x2, 46); /* write staging registers */
	ins(ret, 0x0, 48); /* opcode */
	ins(ret, 0x6, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

static inline uint64_t
gpu_store_i32(struct gpu_src_reg addr_lo, struct gpu_src_reg addr_hi,
	      struct gpu_src_reg val, uint16_t offset,
	      uint8_t sb_slot, enum gpu_flow_ctrl flow)
{
	uint64_t ret = 0;
	igt_assert_eq(addr_hi.reg, addr_lo.reg + 1);
	ins(ret, addr_lo.rep, 0);
	ins(ret, offset, 8);
	ins(ret, 0x0, 24); /* access mode */
	ins(ret, REG_WIDTH_32, 27);
	ins(ret, sb_slot, 30);
	ins(ret, 0x1, 33); /* staging register count */
	ins(ret, val.reg, 40); /* base staging register */
	ins(ret, 0x1, 46); /* read staging registers */
	ins(ret, 0x1, 48); /* opcode */
	ins(ret, 0x6, 52); /* class */
	ins(ret, 0x0, 57); /* FAU */
	ins(ret, flow, 59);
	return ret;
}

#undef ins

#endif /* IGT_PANTHOR_H */
