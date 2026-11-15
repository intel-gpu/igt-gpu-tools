// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2014 Advanced Micro Devices, Inc.
 */

#include "amdgpu/compute_utils/amd_dispatch_helpers.h"
#include "amdgpu/amd_memory.h"
#include <amdgpu_drm.h>
#include "amdgpu/amd_PM4.h"
#include "amdgpu/amd_ip_blocks.h"
#ifdef AMDGPU_LLVM_ENABLED
#include "amdgpu/shaders/amd_llvm_asm.h"
#else
#include "amdgpu/shaders/amd_llvm_asm_stub.h"
#endif
#include "amdgpu/shaders/amd_shader_store.h"
#include "igt.h"

int
amdgpu_dispatch_init(uint32_t ip_type, struct amdgpu_cmd_base *base, uint32_t version)
{
	int i = base->cdw;

	/* Write context control and load shadowing register if necessary */
	if (ip_type == AMDGPU_HW_IP_GFX) {
		base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
		base->emit(base, 0x80000000);
		base->emit(base, 0x80000000);
	}

	/* Issue commands to set default compute state. */
	/* clear mmCOMPUTE_START_Z - mmCOMPUTE_START_X */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 3));
	base->emit(base, 0x204);
	base->emit(base, 0);
	base->emit(base, 0);
	base->emit(base, 0);

	/* clear mmCOMPUTE_TMPRING_SIZE */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base->emit(base, 0x218);
	base->emit(base, 0);
	if (version == 10) {
		/* mmCOMPUTE_SHADER_CHKSUM */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x22a);
		base->emit(base, 0);
		/* mmCOMPUTE_REQ_CTRL */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 6));
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		/* mmCP_COHER_START_DELAY */
		base->emit(base, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
		base->emit(base, 0x7b);
		base->emit(base, 0x20);
	} else if (version == 11) {
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x222);
		base->emit(base, 0);
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
		base->emit(base, 0x224);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x22a);
		base->emit(base, 0);
	}
	return base->cdw - i;
}

int
amdgpu_dispatch_write_cumask(struct amdgpu_cmd_base *base, uint32_t version)
{
	int offset_prev = base->cdw;

	if (version == 9) {
	/*  Issue commands to set cu mask used in current dispatch */
	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
		base->emit(base, 0x216);
		base->emit(base, 0xffffffff);
		base->emit(base, 0xffffffff);
	} else if ((version == 10) || (version == 11)) {
		/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
		base->emit(base, 0x30000216);
		base->emit(base, 0xffffffff);
		base->emit(base, 0xffffffff);
	}
	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE3 - mmCOMPUTE_STATIC_THREAD_MGMT_SE2 */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
	base->emit(base, 0x219);
	base->emit(base, 0xffffffff);
	base->emit(base, 0xffffffff);

	return base->cdw - offset_prev;
}

/*
 * The fence the kernel emits after a job on a kernel queue waits for the
 * dispatched waves and writes the caches back. A user queue only signals its
 * fence after the IB, so the IB has to do that itself before the results can be
 * read: wait for the dispatches to finish, then write back and invalidate the
 * caches like the kernel's emit_mem_sync does.
 */
int
amdgpu_dispatch_write_sync(struct amdgpu_cmd_base *base, uint32_t version)
{
	int offset_prev = base->cdw;

	/* EVENT_WRITE: CS_PARTIAL_FLUSH, wait for the dispatches to finish */
	base->emit(base, PACKET3_COMPUTE(PACKET3_EVENT_WRITE, 0));
	base->emit(base, 0x7 | (4 << 8));

	if (version == 9) {
		/* ACQUIRE_MEM: CP_COHER_CNTL SH_ICACHE, SH_KCACHE, TC, TCL1 and TC_WB actions */
		base->emit(base, PACKET3_COMPUTE(PACKET3_ACQUIRE_MEM, 5));
		base->emit(base, (1 << 29) | (1 << 27) | (1 << 23) | (1 << 22) | (1 << 18));
		base->emit(base, 0xffffffff);	/* CP_COHER_SIZE */
		base->emit(base, 0xffffff);	/* CP_COHER_SIZE_HI */
		base->emit(base, 0);		/* CP_COHER_BASE */
		base->emit(base, 0);		/* CP_COHER_BASE_HI */
		base->emit(base, 0xa);		/* POLL_INTERVAL */
	} else {
		/*
		 * ACQUIRE_MEM: GCR_CNTL GL2 and GLM write back and invalidate,
		 * GL1, GLV, GLK and GLI invalidate
		 */
		base->emit(base, PACKET3_COMPUTE(PACKET3_ACQUIRE_MEM, 6));
		base->emit(base, 0);		/* CP_COHER_CNTL */
		base->emit(base, 0xffffffff);	/* CP_COHER_SIZE */
		base->emit(base, 0xffffff);	/* CP_COHER_SIZE_HI */
		base->emit(base, 0);		/* CP_COHER_BASE */
		base->emit(base, 0);		/* CP_COHER_BASE_HI */
		base->emit(base, 0xa);		/* POLL_INTERVAL */
		base->emit(base, (1 << 15) | (1 << 14) | (1 << 5) | (1 << 4) |
			   (1 << 9) | (1 << 8) | (1 << 7) | (1 << 0));
	}

	return base->cdw - offset_prev;
}


int amdgpu_dispatch_write2hw(struct amdgpu_cmd_base *base, uint64_t shader_addr, uint32_t version, enum  cmd_error_type hang)
{
	static const uint32_t bufferclear_cs_shader_registers_gfx9[][2] = {
		{0x2e12, 0x000C0041},	//{ mmCOMPUTE_PGM_RSRC1,	0x000C0041 },
		{0x2e13, 0x00000090},	//{ mmCOMPUTE_PGM_RSRC2,	0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X,	0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y,	0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z,	0x00000001 }
	};

	static uint32_t bufferclear_cs_shader_registers_gfx11[][2] = {
		{0x2e12, 0x600C0041},	//{ mmCOMPUTE_PGM_RSRC1,	  0x600C0041 },
		{0x2e13, 0x00000090},	//{ mmCOMPUTE_PGM_RSRC2,	  0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X, 0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y, 0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z, 0x00000001 }
	};

	static uint32_t bufferclear_cs_shader_invalid_registers[][2] = {
		{0x2e12, 0xffffffff},	//{ mmCOMPUTE_PGM_RSRC1,	  0x600C0041 },
		{0x2e13, 0xffffffff},	//{ mmCOMPUTE_PGM_RSRC2,	  0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X, 0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y, 0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z, 0x00000001 }
	};

	static const uint32_t bufferclear_cs_shader_registers_num_gfx9 = ARRAY_SIZE(bufferclear_cs_shader_registers_gfx9);
	static const uint32_t bufferclear_cs_shader_registers_num_gfx11 = ARRAY_SIZE(bufferclear_cs_shader_registers_gfx11);
	int offset_prev = base->cdw;
	int j;

	/* Writes shader state to HW */
	/* set mmCOMPUTE_PGM_HI - mmCOMPUTE_PGM_LO */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
	base->emit(base, 0x20c);
	base->emit(base, shader_addr >> 8);
	base->emit(base, shader_addr >> 40);
	/* write sh regs */
	if ((version == 11) || (version == 12)) {
		for (j = 0; j < bufferclear_cs_shader_registers_num_gfx11; j++) {
			base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
			if (hang == BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING) {
				/* - Gfx11ShRegBase */
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][0] - 0x2c00);
				if (bufferclear_cs_shader_invalid_registers[j][0] == 0x2E12)
					bufferclear_cs_shader_invalid_registers[j][1] &= ~(1<<29);

				base->emit(base, bufferclear_cs_shader_invalid_registers[j][1]);
			} else {
				/* - Gfx11ShRegBase */
				base->emit(base, bufferclear_cs_shader_registers_gfx11[j][0] - 0x2c00);
				if (bufferclear_cs_shader_registers_gfx11[j][0] == 0x2E12)
					bufferclear_cs_shader_registers_gfx11[j][1] &= ~(1<<29);

				base->emit(base, bufferclear_cs_shader_registers_gfx11[j][1]);
			}
		}
	} else {
		for (j = 0; j < bufferclear_cs_shader_registers_num_gfx9; j++) {
			base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
			/* - Gfx9ShRegBase */
			if (hang == BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING) {
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][0] - 0x2c00);
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][1]);
			} else {
				base->emit(base, bufferclear_cs_shader_registers_gfx9[j][0] - 0x2c00);
				base->emit(base, bufferclear_cs_shader_registers_gfx9[j][1]);
			}
		}
	}
	if (version == 10) {
		/* mmCOMPUTE_PGM_RSRC3 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x228);
		base->emit(base, 0);
	} else if (version == 11) {
		/* mmCOMPUTE_PGM_RSRC3 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x228);
		base->emit(base, 0x3f0);
	} else if (version == 12) {
		/*
		 * mmCOMPUTE_PGM_RSRC3: whatever is left in INST_PREF_SIZE makes
		 * the instruction prefetch run past the shader, limit it to the
		 * 4 KiB shader BO (in 128 byte units)
		 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x228);
		base->emit(base, (4096 / 128) << 4);
	}
	return base->cdw - offset_prev;
}

/**
 * execute_compute_dispatch - Execute a single compute dispatch for memory copy
 * @device: AMDGPU device handle
 * @version: GPU version for register programming
 * @shader_mc_addr: Shader buffer GPU address
 * @src_mc_addr: Source memory GPU address to read from
 * @dst_mc_addr: Destination buffer GPU address to write to
 * @expected_value: Expected 32-bit value for verification
 *
 * This function sets up and executes a compute dispatch that copies a single DWORD
 * from source to destination using a pre-compiled shader. Each dispatch uses
 * completely independent resources to avoid state pollution.
 *
 * Returns: 0 on success, -1 on failure
 */
int execute_compute_dispatch(amdgpu_device_handle device,
				   uint32_t version,
				   uint64_t shader_mc_addr,
				   uint64_t src_mc_addr,
				   uint64_t dst_mc_addr,
				   uint32_t expected_value)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle bo_cmd = NULL, bo_shader = NULL, bo_dst = NULL;
	uint32_t *ptr_cmd = NULL;
	void *ptr_shader = NULL;
	volatile uint32_t *ptr_dst = NULL;
	uint64_t mc_cmd = 0, mc_shader = 0, mc_dst = 0;
	amdgpu_va_handle va_cmd = 0, va_shader = 0, va_dst = 0;
	struct amdgpu_cs_request ib_req = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_fence fence = {0};
	uint32_t expired = 0;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	amdgpu_bo_handle bos[3];
	int ret = 0;

	/* Create fresh context for each dispatch to avoid state pollution */
	if (amdgpu_cs_ctx_create(device, &context_handle) != 0) {
		igt_info("Failed to create context\n");
		return -1;
	}

	/* Command buffer for PM4 packets */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
	    AMDGPU_GEM_DOMAIN_GTT, 0, &bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd) != 0) {
		igt_info("Failed to allocate command buffer\n");
		ret = -1;
		goto cleanup_context;
	}
	memset(ptr_cmd, 0, 4096);
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);

	/* Shader buffer containing compiled ISA */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
			AMDGPU_GEM_DOMAIN_VRAM, 0,
			&bo_shader, &ptr_shader, &mc_shader, &va_shader) != 0) {
		igt_info("Failed to allocate shader buffer\n");
		ret = -1;
		goto cleanup_cmd;
	}
	memset(ptr_shader, 0, 4096);
	/* Note: shader content should be copied by caller */

	/* Destination buffer for shader output (CPU-visible) */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
			AMDGPU_GEM_DOMAIN_GTT, 0,
			&bo_dst, (void **)&ptr_dst, &mc_dst, &va_dst) != 0) {
		igt_info("Failed to allocate destination buffer\n");
		ret = -1;
		goto cleanup_shader;
	}
	*ptr_dst = 0; /* Initialize for verification */

	/* Initialize compute dispatch state */
	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, version);
	amdgpu_dispatch_write_cumask(base_cmd, version);
	amdgpu_dispatch_write2hw(base_cmd, shader_mc_addr, version, 0);

	/* Program user data registers:
	* s[0:1] = source address (64-bit)
	* s[2:3] = destination address (64-bit)
	*/
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240); /* SH0 register offset */
	base_cmd->emit(base_cmd, src_mc_addr & 0xFFFFFFFF);
	base_cmd->emit(base_cmd, src_mc_addr >> 32);
	base_cmd->emit(base_cmd, mc_dst & 0xFFFFFFFF);
	base_cmd->emit(base_cmd, mc_dst >> 32);

	/* Dispatch compute shader with 1x1x1 workgroup */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10); /* Dispatch initiator */
	base_cmd->emit(base_cmd, 1);    /* dim_x */
	base_cmd->emit(base_cmd, 1);    /* dim_y */
	base_cmd->emit(base_cmd, 1);    /* dim_z */
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	/* Submit command buffer */
	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_dst;
	if (amdgpu_bo_list_create(device, 3, bos, NULL, &bo_list) != 0) {
		igt_info("Failed to create BO list\n");
		ret = -1;
		goto cleanup_dst;
	}

	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	ib_req.fence_info.handle = NULL;

	if (amdgpu_cs_submit(context_handle, 0, &ib_req, 1) != 0) {
		igt_info("Failed to submit CS\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	/* Wait for completion */
	fence.context = context_handle;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;

	if (amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired) != 0) {
		igt_info("Fence wait failed\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	if (!expired) {
		igt_info("Fence not expired\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	/* Verify result */
	if (*ptr_dst != expected_value) {
		igt_info("Result mismatch: got 0x%x, expected 0x%x\n", *ptr_dst, expected_value);
		ret = -1;
	} else {
		igt_info("Dispatch successful: got expected value 0x%x\n", *ptr_dst);
	}

cleanup_bo_list:
	amdgpu_bo_list_destroy(bo_list);
cleanup_dst:
	amdgpu_bo_unmap_and_free(bo_dst, va_dst, mc_dst, 4096);
cleanup_shader:
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_shader, 4096);
cleanup_cmd:
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
cleanup_context:
	amdgpu_cs_ctx_free(context_handle);

	return ret;
}

int amdgpu_dispatch_shader(amdgpu_device_handle device,
			   const struct amdgpu_dispatch_shader_req *req)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle bo_cmd = NULL;
	uint32_t *ptr_cmd = NULL;
	uint64_t mc_cmd = 0;
	amdgpu_va_handle va_cmd = 0;
	struct amdgpu_cs_request ib_req = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_fence fence = {0};
	uint32_t expired = 0;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	amdgpu_bo_handle *resources = NULL;
	uint32_t num_resources = 0;
	uint32_t i;
	int ret = -1;

	if (!req || !req->shader_mc_addr) {
		igt_info("Invalid dispatch request\n");
		goto cleanup_base;
	}

	/* Create fresh context for each dispatch to avoid state pollution */
	if (amdgpu_cs_ctx_create(device, &context_handle) != 0) {
		igt_info("Failed to create context\n");
		goto cleanup_base;
	}

	/* Command buffer for PM4 packets */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
	    AMDGPU_GEM_DOMAIN_GTT, 0, &bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd) != 0) {
		igt_info("Failed to allocate command buffer\n");
		goto cleanup_context;
	}
	memset(ptr_cmd, 0, 4096);
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);

	/* Generic compute state init (version-specific defaults) */
	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, req->version);
	amdgpu_dispatch_write_cumask(base_cmd, req->version);

	/*
	 * The SET_SH_REG offsets below are hardcoded (0x20c, 0x212, 0x213,
	 * 0x207, 0x228, 0x240). These values are stable across gfx9..gfx12,
	 * so hardcoding is correct today.
	 *
	 * The intended abstraction for offsets is
	 * ip_block->funcs->get_reg_offset(): fetch the compute IP block via
	 * get_ip_block(device, AMDGPU_HW_IP_COMPUTE) and call
	 * ip_block->funcs->get_reg_offset(COMPUTE_PGM_LO) etc. instead of the
	 * literals. It is deliberately NOT used here for two reasons:
	 *   1. Coverage - enum general_reg (amd_registers.h) only defines
	 *      COMPUTE_PGM_LO, PGM_RSRC1, TMPRING_SIZE, USER_DATA_0/1,
	 *      RESOURCE_LIMITS and NUM_THREAD_X. RSRC2 (0x213), RSRC3 (0x228)
	 *      and NUM_THREAD_Y/Z (0x208/0x209) have no enum, so the dispatch
	 *      path cannot be expressed through get_reg_offset alone.
	 *   2. No per-gen adaptation - there is a single compute IP block
	 *      (compute_v8_x_ip_block) wired to gfx_v8_0_get_reg_offset for all
	 *      generations, so it returns the same gfx8/VI offsets regardless
	 *      of the running IP; adopting it would centralize but not future-
	 *      proof the offsets.
	 * To fully adopt get_reg_offset, first extend enum general_reg with the
	 * missing registers and add real per-generation offset tables/IP blocks.
	 */

	/* Shader program address (COMPUTE_PGM_LO/HI) */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
	base_cmd->emit(base_cmd, 0x20c);
	base_cmd->emit(base_cmd, req->shader_mc_addr >> 8);
	base_cmd->emit(base_cmd, req->shader_mc_addr >> 40);

	/* Caller-provided shader resource state (COMPUTE_PGM_RSRC1/RSRC2) */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base_cmd->emit(base_cmd, 0x212);
	base_cmd->emit(base_cmd, req->rsrc1);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base_cmd->emit(base_cmd, 0x213);
	base_cmd->emit(base_cmd, req->rsrc2);

	/* Workgroup dimensions (COMPUTE_NUM_THREAD_X/Y/Z, consecutive regs) */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 3));
	base_cmd->emit(base_cmd, 0x207);
	base_cmd->emit(base_cmd, req->thread_x);
	base_cmd->emit(base_cmd, req->thread_y);
	base_cmd->emit(base_cmd, req->thread_z);

	/* COMPUTE_PGM_RSRC3 exists on gfx10/gfx11 */
	if (req->version == 10 || req->version == 11) {
		base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base_cmd->emit(base_cmd, 0x228);
		base_cmd->emit(base_cmd, req->rsrc3);
	}

	/* User data / kernel arguments (COMPUTE_USER_DATA_0..) */
	if (req->user_data_count) {
		base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, req->user_data_count));
		base_cmd->emit(base_cmd, 0x240);
		for (i = 0; i < req->user_data_count; i++)
			base_cmd->emit(base_cmd, req->user_data[i]);
	}

	/* Dispatch: dim_x, dim_y, dim_z, dispatch_initiator */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, req->grid_x);
	base_cmd->emit(base_cmd, req->grid_y);
	base_cmd->emit(base_cmd, req->grid_z);
	base_cmd->emit(base_cmd, req->dispatch_initiator);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	/* Resource list = internal command buffer + caller-provided BOs */
	num_resources = req->num_bos + 1;
	resources = malloc(num_resources * sizeof(*resources));
	if (!resources) {
		igt_info("Failed to allocate resource list\n");
		goto cleanup_cmd;
	}
	resources[0] = bo_cmd;
	for (i = 0; i < req->num_bos; i++)
		resources[i + 1] = req->bos[i];

	if (amdgpu_bo_list_create(device, num_resources, resources, NULL, &bo_list) != 0) {
		igt_info("Failed to create BO list\n");
		goto cleanup_resources;
	}

	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	ib_req.fence_info.handle = NULL;

	if (amdgpu_cs_submit(context_handle, 0, &ib_req, 1) != 0) {
		igt_info("Failed to submit CS\n");
		goto cleanup_bo_list_run;
	}

	/* Wait for completion */
	fence.context = context_handle;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;

	if (amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired) != 0) {
		igt_info("Fence wait failed\n");
		goto cleanup_bo_list_run;
	}

	if (!expired) {
		igt_info("Fence not expired\n");
		goto cleanup_bo_list_run;
	}

	ret = 0;

cleanup_bo_list_run:
	amdgpu_bo_list_destroy(bo_list);
cleanup_resources:
	free(resources);
cleanup_cmd:
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
cleanup_context:
	amdgpu_cs_ctx_free(context_handle);
cleanup_base:
	free_cmd_base(base_cmd);

	return ret;
}

struct copy_dword_shader {
	amdgpu_device_handle device;
	amdgpu_bo_handle bo_shader;
	amdgpu_va_handle va_shader;
	uint64_t mc_shader;
	uint32_t version;
	bool ready;
};

/*
 * Assemble the CopyDword compute shader for @device and populate @sh.
 *
 * Returns true on success (sh->ready set). Returns false if the shader could
 * not be prepared -- whether unavailable (no compute IP block, unsupported gfx
 * version, no LLVM assembler) or a hard error (query / BO alloc failed).
 */
static bool copy_dword_shader_assemble(amdgpu_device_handle device,
				       struct copy_dword_shader *sh)
{
	struct amdgpu_gpu_info info = {0};
	struct drm_amdgpu_info_hw_ip hw_ip = {0};
	char mcpu[32];
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	void *ptr_shader = NULL;
	int r;

	if (get_ip_block(device, AMD_IP_COMPUTE) == NULL) {
		igt_info("Compute IP block not available\n");
		return false;
	}

	if (amdgpu_query_gpu_info(device, &info) != 0)
		return false;

	if (amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_COMPUTE, 0, &hw_ip) != 0)
		return false;
	sh->version = hw_ip.hw_ip_version_major;
	/*
	 * Only gfx9..gfx12 have verified register-programming tables here
	 * (see amdgpu_dispatch_write2hw() and amdgpu_compute_copy_dword()).
	 * Fail fast on anything outside that range instead of silently
	 * reusing the gfx9 defaults, which would emit wrong RSRC1/offsets
	 * on a future IP.
	 */
	if (sh->version < 9 || sh->version > 12) {
		igt_info("Compute copy unsupported on gfx version %u (need GFX9..GFX12)\n",
			 sh->version);
		return false;
	}

	amdgpu_family_id_to_mcpu(info.family_id, mcpu, sizeof(mcpu));

	if (amdgpu_llvm_asm_init() != 0)
		return false;

	r = amdgpu_llvm_assemble(mcpu, CopyDwordIsa, isa_bin,
				 sizeof(isa_bin), &isa_sz);
	if (r != 0 || isa_sz == 0) {
		amdgpu_llvm_asm_shutdown();
		igt_info("Failed to assemble CopyDwordIsa\n");
		return false;
	}

	sh->device = device;

	/* Upload the assembled ISA into a dedicated shader BO */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096, AMDGPU_GEM_DOMAIN_GTT, 0,
				    &sh->bo_shader, &ptr_shader, &sh->mc_shader,
				    &sh->va_shader) != 0) {
		amdgpu_llvm_asm_shutdown();
		return false;
	}
	memset(ptr_shader, 0, 4096);
	memcpy(ptr_shader, isa_bin, isa_sz);
	amdgpu_llvm_asm_shutdown();

	sh->ready = true;
	return true;
}

/*
 * Lazily assemble (once per process) and return the cached CopyDword shader for
 * @device, or NULL if the shader could not be prepared. The shader BO is
 * intentionally left mapped until process exit; the test binary is short-lived.
 */
static struct copy_dword_shader *copy_dword_shader_get(amdgpu_device_handle device)
{
	static struct copy_dword_shader cache;

	if (cache.ready && cache.device == device)
		return &cache;

	/* A device change is not expected; drop any stale shader. */
	if (cache.ready) {
		if (cache.bo_shader)
			amdgpu_bo_unmap_and_free(cache.bo_shader, cache.va_shader,
						 cache.mc_shader, 4096);
		memset(&cache, 0, sizeof(cache));
	}

	if (!copy_dword_shader_assemble(device, &cache)) {
		memset(&cache, 0, sizeof(cache));
		return NULL;
	}

	return &cache;
}

int amdgpu_compute_copy_dword(amdgpu_device_handle device,
			      amdgpu_bo_handle src_bo, uint64_t src_va,
			      amdgpu_bo_handle dst_bo, uint64_t dst_va)
{
	struct copy_dword_shader *sh;
	struct amdgpu_dispatch_shader_req req = {0};
	uint32_t user_data[4];
	amdgpu_bo_handle bos[3];
	uint32_t num_bos = 0;

	sh = copy_dword_shader_get(device);
	if (!sh) {
		igt_info("Compute shader unavailable (no compute IP / LLVM)\n");
		return -1;
	}

	/* CopyDword ABI: s[0:1] = source pointer, s[2:3] = destination pointer */
	user_data[0] = src_va & 0xFFFFFFFF;
	user_data[1] = src_va >> 32;
	user_data[2] = dst_va & 0xFFFFFFFF;
	user_data[3] = dst_va >> 32;

	req.version = sh->version;
	req.shader_mc_addr = sh->mc_shader;
	/* Register state mirrors amdgpu_dispatch_write2hw() for CopyDword */
	if (sh->version == 11 || sh->version == 12)
		req.rsrc1 = 0x600C0041 & ~(1u << 29);
	else
		req.rsrc1 = 0x000C0041;
	req.rsrc2 = 0x00000090;
	req.rsrc3 = (sh->version == 11) ? 0x3f0 : 0;
	req.thread_x = 0x40;
	req.thread_y = 1;
	req.thread_z = 1;
	req.user_data = user_data;
	req.user_data_count = 4;
	req.grid_x = 1;
	req.grid_y = 1;
	req.grid_z = 1;
	req.dispatch_initiator = 0x1;	/* COMPUTE_SHADER_EN */

	/*
	 * Register the shader BO plus any caller-supplied source/destination
	 * BOs so the driver keeps every buffer the shader accesses resident for
	 * the dispatch. The command buffer BO is added internally by
	 * amdgpu_dispatch_shader().
	 */
	bos[num_bos++] = sh->bo_shader;
	if (src_bo)
		bos[num_bos++] = src_bo;
	if (dst_bo)
		bos[num_bos++] = dst_bo;
	req.bos = bos;
	req.num_bos = num_bos;

	return amdgpu_dispatch_shader(device, &req);
}
