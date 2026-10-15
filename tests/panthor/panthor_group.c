// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

/*
 * Record a simple compute program:
 * {
 *     extern uint32_t shared_buf[num_invocations];
 *     extern const uint32_t my_invocation_id;
 *     shared_buf[my_invocation_id] *= my_invocation_id;
 * }
 */
static void
record_compute_prog(uint8_t *csf_cmdbuf, unsigned int *csf_cmdbuf_size,
		    uint8_t *gpu_cmdbuf, unsigned int *gpu_cmdbuf_size,
		    uint32_t srt_gpuva, uint32_t fau_gpuva, uint32_t spd_gpuva,
		    uint32_t tsd_gpuva, uint32_t data_gpuva)
{
	const uint8_t reg_id_global_x = 60; /* preloaded */
	const uint8_t reg_addr_lo = 0;
	const uint8_t reg_addr_hi = 1;
	const uint8_t reg_val = 2;
	const uint8_t reg_offset = 3;
	const uint8_t reg_shift = 4;
	const uint8_t compute_sr_base = 0;
	const uint8_t zero_reg = 40;

	uint64_t gpu_instrs[] = {
		/* MOV_IMM.i32 rADDRLO, 0x10000 */
		gpu_mov_imm_i32(gpu_dst_reg(reg_addr_lo), data_gpuva, FLOW_NONE),
		/* MOV_IMM.i32 rADDRHI, 0x0 */
		gpu_mov_imm_i32(gpu_dst_reg(reg_addr_hi), 0x0, FLOW_NONE),
		/* MOV_IMM.i32 rSHIFT, 0x2 */
		gpu_mov_imm_i32(gpu_dst_reg(reg_shift), 0x2, FLOW_NONE),
		/* LSHIFT_OR.i32 rOFFSET, rGLOBALID, rSHIFT^, 0x0 */
		gpu_lshift_or_i32(gpu_dst_reg(reg_offset),
				  gpu_src_reg(reg_id_global_x),
				  gpu_src_reg_last(reg_shift),
				  gpu_src_zero(),
				  FLOW_NONE),
		/* IADD.u32 rADDRLO, rADDRLO^, rOFFSET^ */
		gpu_iadd_u32(gpu_dst_reg(reg_addr_lo),
			     gpu_src_reg_last(reg_addr_lo),
			     gpu_src_reg_last(reg_offset),
			     FLOW_NONE),
		/* LOAD.i32.slot0.wait0 @rVAL, [rADDRLO:rADDRHI], offset:0 */
		gpu_load_i32(gpu_dst_reg(reg_val),
			     gpu_src_reg(reg_addr_lo),
			     gpu_src_reg(reg_addr_hi),
			     0x0,
			     0, FLOW_WAIT0),
		/* IMUL_u32 rVAL, rVAL^, rGLOBALID^ */
		gpu_imul_u32(gpu_dst_reg(reg_val),
			     gpu_src_reg_last(reg_val),
			     gpu_src_reg_last(reg_id_global_x),
			     FLOW_NONE),
		/* STORE.i32.slot1.end @rVAL, [rADDRLO^:rADDRHI^], offset:0 */
		gpu_store_i32(gpu_src_reg_last(reg_addr_lo),
			      gpu_src_reg_last(reg_addr_hi),
			      gpu_src_reg(reg_val),
			      0x0,
			      1, FLOW_END),
	};

	uint64_t csf_instrs[] = {
		/* REQ_RESOURCE.compute */
		cs_req_resource(true, false, false, false),
		/* MOV32 r40, 0 (always force cache flush/invalidate) */
		cs_mov32(zero_reg, 0),
		/* FLUSH_CACHE.clean_invalidate_l2.clean_invalidate_lsc.invalidate_other.sb0 */
		cs_flush(CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_INVALIDATE,
			 0, zero_reg, 1),
		/* WAIT (flush) */
		cs_wait(0xff, false),
		/* ShaderResourceTable 0-3 pointers */
		cs_mov48(compute_sr_base +  0, srt_gpuva),
		cs_mov48(compute_sr_base +  2, 0x222222222222),
		cs_mov48(compute_sr_base +  4, 0x333333333333),
		cs_mov48(compute_sr_base +  6, 0x444444444444),
		/* FastAccessUniform table 0-3 pointers */
		cs_mov48(compute_sr_base +  8, fau_gpuva),
		cs_mov48(compute_sr_base + 10, 0x666666666666),
		cs_mov48(compute_sr_base + 12, 0x777777777777),
		cs_mov48(compute_sr_base + 14, 0x888888888888),
		/* ShaderProgramDescriptor 0-3 pointers */
		cs_mov48(compute_sr_base + 16, spd_gpuva),
		cs_mov48(compute_sr_base + 18, 0x999999999999),
		cs_mov48(compute_sr_base + 20, 0xaaaaaaaaaaaa),
		cs_mov48(compute_sr_base + 22, 0xbbbbbbbbbbbb),
		/* ThreadStorageDescriptor 0-3 pointers */
		cs_mov48(compute_sr_base + 24, tsd_gpuva),
		cs_mov48(compute_sr_base + 26, 0xdddddddddddd),
		cs_mov48(compute_sr_base + 28, 0xeeeeeeeeeeee),
		cs_mov48(compute_sr_base + 30, 0xffffffffffff),
		/* global ID offset = 0 */
		cs_mov32(compute_sr_base + 32, 0),
		/* workgroup size 64x1x1, allow merge */
		cs_mov32(compute_sr_base + 33, (63 << 0) | (0 << 10) | (0 << 20) | (1 << 31)),
		/* job offset (x/y/z) = { 0, 0, 0 } */
		cs_mov32(compute_sr_base + 34, 0),
		cs_mov32(compute_sr_base + 35, 0),
		cs_mov32(compute_sr_base + 36, 0),
		/* job size (x/y/z) = { 64, 1, 1 } */
		cs_mov32(compute_sr_base + 37, 64),
		cs_mov32(compute_sr_base + 38, 1),
		cs_mov32(compute_sr_base + 39, 1),
		/* SET_SB_ENTRY.async_ep1.other1 */
		cs_set_sb_entry(1, 1),
		/* RUN_COMPUTE.increment64.x_axis.no_increment.srt0.spd0.tsd0.fau0 */
		cs_run_compute(64, CS_TASK_AXIS_X, 0, 0, 0, 0, 0),
		/* WAIT (RUN_COMPUTE) */
		cs_wait(0xff, false),
		/* FLUSH_CACHE.clean_invalidate_l2.clean_invalidate_lsc.invalidate_other.sb0 */
		cs_flush(CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_INVALIDATE,
			 0, zero_reg, 1),
		/* WAIT (flush) */
		cs_wait(0xff, false),
		/* ERROR_BARRIER */
		cs_error_barrier(),
	};

	memcpy(gpu_cmdbuf, gpu_instrs, sizeof(gpu_instrs));
	*gpu_cmdbuf_size = sizeof(gpu_instrs);
	memcpy(csf_cmdbuf, csf_instrs, sizeof(csf_instrs));
	*csf_cmdbuf_size = sizeof(csf_instrs);
}

static size_t
issue_store_multiple(uint8_t *cs, uint64_t kernel_va, uint32_t constant)
{
	const uint8_t kernel_va_reg = 68;
	const uint8_t constant_reg = 70;
	uint64_t instrs[] = {
		/* MOV48: Load the source register ([r68; r69]) with the kernel address */
		cs_mov48(kernel_va_reg, kernel_va),
		/* MOV32: Load a known constant into r70 */
		cs_mov32(constant_reg, constant),
		/* STORE_MULTIPLE: Store the first register to the address pointed
		 * to by [r68; r69]
		 */
		cs_stm32(kernel_va_reg, constant_reg, 0),
		/* FLUSH all Wait for all cores */
		cs_wait(0xff, false),
		/* MOV32: Clear r70 to 0 */
		cs_mov32(constant_reg, 0),
		/* FLUSH_CACHE: Clean and invalidate all caches */
		cs_flush(CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_INVALIDATE,
			 0, constant_reg, 1),
		cs_wait(0xff, false),
	};

	memcpy(cs, instrs, sizeof(instrs));
	return sizeof(instrs);
}

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a CSF group.");
	igt_subtest("group_create") {
		struct drm_panthor_vm_create vm_create = {};
		struct drm_panthor_vm_destroy vm_destroy = {};
		uint32_t group_handle;

		vm_create.flags = 0;
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vm_create);
		igt_assert_neq(vm_create.id, 0);

		group_handle = igt_panthor_group_create_simple(fd, vm_create.id, 0);
		igt_assert_neq(group_handle, 0);

		igt_panthor_group_destroy(fd, group_handle, 0);

		vm_destroy = (struct drm_panthor_vm_destroy) { .id = vm_create.id };
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vm_destroy);
	}

	igt_describe("Submit a job to a group and wait for completion. "
		     "The job writes a known value to a buffer object that is then "
		     "mmaped and checked.");
	igt_subtest("group_submit") {
		uint32_t vm_id;
		uint32_t group_handle;
		struct panthor_bo cmd_buf_bo = {};
		struct panthor_bo result_bo = {};
		uint64_t command_stream_gpu_addr;
		uint32_t command_stream_size;
		uint64_t result_gpu_addr;
		uint32_t syncobj_handle;
		const int INITIAL_VA = 0x1000000;

		igt_panthor_vm_create(fd, &vm_id, 0);

		igt_panthor_bo_create_mapped(fd, &cmd_buf_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, cmd_buf_bo.handle, INITIAL_VA,
				    cmd_buf_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		command_stream_gpu_addr = INITIAL_VA;

		/* Create the BO to receive the result of the store. */
		igt_panthor_bo_create_mapped(fd, &result_bo, 4096, 0, 0);
		/* Also bind the result BO. */
		igt_panthor_vm_bind(fd, vm_id, result_bo.handle, INITIAL_VA + 4096,
				    result_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		result_gpu_addr = INITIAL_VA + 4096;

		command_stream_size = issue_store_multiple(cmd_buf_bo.map, result_gpu_addr, 0xdeadbeef);

		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);
		igt_assert_neq(group_handle, 0);

		syncobj_handle = syncobj_create(fd, 0);

		igt_panthor_group_submit_simple(fd, group_handle, 0, command_stream_gpu_addr, command_stream_size, syncobj_handle, 0);

		igt_assert(syncobj_wait(fd, &syncobj_handle, 1, INT64_MAX, 0, NULL));

		igt_assert_eq(*(uint32_t *)result_bo.map, 0xdeadbeef);

		syncobj_destroy(fd, syncobj_handle);

		igt_panthor_group_destroy(fd, group_handle, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);

		igt_panthor_free_bo(fd, &cmd_buf_bo);
		igt_panthor_free_bo(fd, &result_bo);
	}

	igt_describe("Submit a simple compute job with memory access.");
	igt_subtest("simple_compute") {
		uint32_t vm_id;
		uint32_t group_handle;
		struct panthor_bo csf_cmdbuf_bo = {};
		struct panthor_bo gpu_cmdbuf_bo = {};
		struct panthor_bo resource_table_bo = {};
		struct panthor_bo data_bo = {};
		uint64_t csf_cmdbuf_gpuva;
		uint32_t csf_cmdbuf_size;
		uint64_t gpu_cmdbuf_gpuva;
		uint32_t gpu_cmdbuf_size;
		uint32_t resource_table_gpuva;
		uint32_t data_gpuva;
		uint32_t syncobj_handle;
		uint32_t srt_offset;
		uint32_t fau_offset;
		uint32_t spd_offset;
		uint32_t *spd_ptr;
		uint32_t spd_gpuva;
		uint32_t tsd_offset;
		uint32_t *tsd_ptr;
		int arch = igt_panthor_get_arch(fd);

		igt_require(arch < 15);

		igt_panthor_vm_create(fd, &vm_id, 0);

		/* CSF ISA command stream */
		csf_cmdbuf_gpuva = 0x1000000;
		igt_panthor_bo_create_mapped(fd, &csf_cmdbuf_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, csf_cmdbuf_bo.handle, csf_cmdbuf_gpuva,
				    csf_cmdbuf_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		memset(csf_cmdbuf_bo.map, 0, csf_cmdbuf_bo.size);

		/* GPU ISA command stream */
		gpu_cmdbuf_gpuva = 0x2000000;
		igt_panthor_bo_create_mapped(fd, &gpu_cmdbuf_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, gpu_cmdbuf_bo.handle, gpu_cmdbuf_gpuva,
				    gpu_cmdbuf_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		memset(gpu_cmdbuf_bo.map, 0, gpu_cmdbuf_bo.size);

		/* resource tables passed to RUN_COMPUTE */
		resource_table_gpuva = 0x3000000;
		igt_panthor_bo_create_mapped(fd, &resource_table_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, resource_table_bo.handle, resource_table_gpuva,
				    resource_table_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		memset(resource_table_bo.map, 0, resource_table_bo.size);
		/* ShaderResourceTable descriptor (zeroed) */
		srt_offset = 0;
		/* FastAccessUniform descriptor (zeroed) */
		fau_offset = 128;
		/* ShaderProgramDescriptor */
		spd_offset = 256;
		spd_gpuva = resource_table_gpuva + spd_offset;
		if (arch == 14)
			spd_gpuva |= 2; /* register allocation propagated into ptr */
		spd_ptr = (uint32_t *) ((unsigned char *) resource_table_bo.map + spd_offset);
		spd_ptr[0] = 0x8 | (0x1 << 4); /* compute shader */
		spd_ptr[0] |= (0 << 16) | (0 << 17) | (0 << 19); /* no NaN suppress, FTZ, inf suppress */
		spd_ptr[0] |= (0 << 29) | (2 << 30); /* no JUMP_EX, max 32 registers */
		spd_ptr[1] |= (1 << 12); /* preload GlobalInvocationID.x into r60 */
		spd_ptr[2] = gpu_cmdbuf_gpuva; /* initial PC */
		/* ThreadStorageDescriptor */
		tsd_offset = 512;
		tsd_ptr = (uint32_t *) ((unsigned char *) resource_table_bo.map + tsd_offset);
		tsd_ptr[1] = 0x1f; /* max wls_instances */

		/* CPU<->GPU shared data */
		data_gpuva = 0x4000000;
		igt_panthor_bo_create_mapped(fd, &data_bo, 16384, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, data_bo.handle, data_gpuva,
				    data_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		for (unsigned int i = 0; i < 64 * 64; i++) {
			uint32_t *x = data_bo.map + (i * sizeof(uint32_t));
			*x = i;
		}

		record_compute_prog(csf_cmdbuf_bo.map, &csf_cmdbuf_size,
				    gpu_cmdbuf_bo.map, &gpu_cmdbuf_size,
				    resource_table_gpuva + srt_offset,
				    resource_table_gpuva + fau_offset,
				    spd_gpuva,
				    resource_table_gpuva + tsd_offset,
				    data_gpuva);

		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);
		igt_assert_neq(group_handle, 0);

		syncobj_handle = syncobj_create(fd, 0);

		igt_panthor_group_submit_simple(fd, group_handle, 0,
						csf_cmdbuf_gpuva, csf_cmdbuf_size,
						syncobj_handle, 0);
		igt_assert(syncobj_wait(fd, &syncobj_handle, 1, INT64_MAX, 0, NULL));

		for (unsigned int i = 0; i < 64 * 64; i++) {
			uint32_t *x = data_bo.map + (i * sizeof(uint32_t));
			igt_assert_eq(*x, i * i);
		}

		syncobj_destroy(fd, syncobj_handle);

		igt_panthor_group_destroy(fd, group_handle, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);

		igt_panthor_free_bo(fd, &data_bo);
		igt_panthor_free_bo(fd, &resource_table_bo);
		igt_panthor_free_bo(fd, &gpu_cmdbuf_bo);
		igt_panthor_free_bo(fd, &csf_cmdbuf_bo);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
