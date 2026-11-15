/* SPDX-License-Identifier: MIT
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#ifndef AMD_DISPATCH_HELPERS_H
#define AMD_DISPATCH_HELPERS_H

#include <amdgpu.h>
#include "lib/amdgpu/amd_ip_blocks.h"

struct amdgpu_cmd_base;

int amdgpu_dispatch_init(uint32_t ip_type, struct amdgpu_cmd_base *base_cmd, uint32_t version);

int amdgpu_dispatch_write_cumask(struct amdgpu_cmd_base *base_cmd, uint32_t version);

int amdgpu_dispatch_write_sync(struct amdgpu_cmd_base *base_cmd, uint32_t version);

int amdgpu_dispatch_write2hw(struct amdgpu_cmd_base *base_cmd, uint64_t shader_addr, uint32_t version, enum cmd_error_type err);

int execute_compute_dispatch(amdgpu_device_handle device,
				   uint32_t version,
				   uint64_t shader_mc_addr,
				   uint64_t src_mc_addr,
				   uint64_t dst_mc_addr,
				   uint32_t expected_value);

/**
 * struct amdgpu_dispatch_shader_req - Description of a generic compute dispatch
 * @version: GPU version used for the compute state init (9/10/11/12)
 * @shader_mc_addr: GPU address of the shader ISA to execute
 * @rsrc1: value written to COMPUTE_PGM_RSRC1
 * @rsrc2: value written to COMPUTE_PGM_RSRC2
 * @rsrc3: value written to COMPUTE_PGM_RSRC3 (only emitted on version 10/11)
 * @thread_x: value written to COMPUTE_NUM_THREAD_X (workgroup size X)
 * @thread_y: value written to COMPUTE_NUM_THREAD_Y (workgroup size Y)
 * @thread_z: value written to COMPUTE_NUM_THREAD_Z (workgroup size Z)
 * @user_data: dwords loaded into COMPUTE_USER_DATA_0.. (kernel arguments)
 * @user_data_count: number of dwords in @user_data (0 to skip)
 * @grid_x: DISPATCH_DIRECT dimension X (number of workgroups)
 * @grid_y: DISPATCH_DIRECT dimension Y
 * @grid_z: DISPATCH_DIRECT dimension Z
 * @dispatch_initiator: COMPUTE_DISPATCH_INITIATOR flags (bit0 = COMPUTE_SHADER_EN)
 * @bos: BOs the shader accesses (inputs/outputs); the command buffer is added
 *       internally, so this list only needs the shader and data BOs
 * @num_bos: number of entries in @bos
 *
 * All shader-binding state (RSRC*, thread dims, user data, grid) is supplied by
 * the caller so that any shader can be executed without changing the emitter.
 */
struct amdgpu_dispatch_shader_req {
	uint32_t version;

	uint64_t shader_mc_addr;
	uint32_t rsrc1;
	uint32_t rsrc2;
	uint32_t rsrc3;

	uint32_t thread_x;
	uint32_t thread_y;
	uint32_t thread_z;

	const uint32_t *user_data;
	uint32_t user_data_count;

	uint32_t grid_x;
	uint32_t grid_y;
	uint32_t grid_z;
	uint32_t dispatch_initiator;

	amdgpu_bo_handle *bos;
	uint32_t num_bos;
};

/**
 * amdgpu_dispatch_shader - Execute a generic compute dispatch
 * @device: AMDGPU device handle
 * @req: Description of the dispatch to execute
 *
 * This function creates a fresh context, allocates a command buffer, emits the
 * compute state and dispatch packets, and submits it. The caller must provide
 * all shader-binding state (RSRC*, thread dims, user data, grid) so that any
 * shader can be executed without changing the emitter.
 *
 * Returns: 0 on success, -1 on failure
 */
int amdgpu_dispatch_shader(amdgpu_device_handle device,
			       const struct amdgpu_dispatch_shader_req *req);

/**
 * amdgpu_compute_copy_dword - Copy a single DWORD from src to dst via compute
 * @device: AMDGPU device handle
 * @src_bo: BO backing @src_va, or NULL if there is no BO to track
 *          (for example, an SVM/HMM address)
 * @src_va: GPU virtual address of the source DWORD
 * @dst_bo: BO backing @dst_va, or NULL if there is no BO to track
 * @dst_va: GPU virtual address of the destination DWORD
 *
 * This function assembles a small compute shader that copies a single 32-bit
 * value from @src_va to @dst_va, uploads it to a dedicated shader BO, and
 * dispatches it. Any non-NULL @src_bo / @dst_bo is registered in the dispatch
 * BO list alongside the shader BO so the driver keeps the accessed buffers
 * resident for the duration of the dispatch.
 *
 * Returns: 0 on success, -1 on failure
 */
int amdgpu_compute_copy_dword(amdgpu_device_handle device,
			      amdgpu_bo_handle src_bo, uint64_t src_va,
			      amdgpu_bo_handle dst_bo, uint64_t dst_va);
#endif
