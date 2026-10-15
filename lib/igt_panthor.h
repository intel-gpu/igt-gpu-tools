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

#endif /* IGT_PANTHOR_H */
