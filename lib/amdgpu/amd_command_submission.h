/* SPDX-License-Identifier: MIT
 * Copyright 2012 Advanced Micro Devices, Inc.
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

#ifndef AMD_COMMAND_SUBMISSION
#define AMD_COMMAND_SUBMISSION

#include "amd_ip_blocks.h"

/**
 * Packet types supported by the command submission library
 */
typedef enum {
    CMD_PACKET_WRITE_LINEAR,
    CMD_PACKET_WRITE_ATOMIC,
    CMD_PACKET_COPY_LINEAR,
    CMD_PACKET_COPY_ATOMIC,
    CMD_PACKET_CONST_FILL,
    CMD_PACKET_FENCE,
    CMD_PACKET_TIMESTAMP,
} cmd_packet_type_t;

/**
 * Command submission context structure
 */
typedef struct {
    amdgpu_device_handle device;
    struct amdgpu_ring_context *ring_ctx;
    const struct amdgpu_ip_block_version *ip_block;
    enum amd_ip_block_type ip_type;
    bool initialized;
    bool user_queue;
    uint64_t last_submit_seq; /* Last submission sequence number */

    /* IB shared by the user-queue and CS paths; created in cmd_context_create,
     * freed in cmd_context_destroy, reusable across multiple submits.
     */
    amdgpu_bo_handle ib_bo;
    void *ib_cpu;
    uint64_t ib_mc;
    amdgpu_va_handle ib_va;
    bool submit_pending;   /* a submit is in flight; wait before reuse/free */
} cmd_context_t;

/**
 * Packet building parameters
 */
typedef struct {
    cmd_packet_type_t type;
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;
    uint32_t data;
    uint32_t *custom_data;
    uint32_t custom_data_size;
} cmd_packet_params_t;

int amdgpu_test_exec_cs_helper(amdgpu_device_handle device,
				unsigned int ip_type, struct amdgpu_ring_context *ring_context,
				int expect);

void amdgpu_command_submission_write_linear_helper(amdgpu_device_handle device,
						   const struct amdgpu_ip_block_version *ip_block,
						   bool secure, bool user_queue);

void amdgpu_command_submission_write_linear_helper2(amdgpu_device_handle device, unsigned type,
						    bool secure, bool user_queue);

void amdgpu_command_submission_const_fill_helper(amdgpu_device_handle device,
						 const struct amdgpu_ip_block_version *ip_block,
						 bool user_queue);

void amdgpu_command_submission_copy_linear_helper(amdgpu_device_handle device,
						 const struct amdgpu_ip_block_version *ip_block,
						 bool user_queue);

void  amdgpu_command_ce_write_fence(amdgpu_device_handle dev,
					  amdgpu_context_handle ctx);

/*
 * cmd_* command-submission API
 * ============================
 *
 * A small wrapper around raw amdgpu command submission that hides the
 * context / IB / bo_list / fence plumbing and lets a test issue DMA-style
 * operations (write, copy, const-fill, atomic) on an IP ring (SDMA, GFX or
 * Compute) by GPU virtual address.
 *
 * Lifecycle
 * ---------
 *   1. cmd_context_create()  - create a context bound to one IP ring. It
 *      allocates a reusable IB/PM4 buffer for the emitted packets.
 *   2. issue one or more operations (see the two API layers below).
 *   3. cmd_context_destroy()  - waits for any in-flight submission, then frees
 *      the IB and the context.
 *
 * Two API layers
 * --------------
 *   High-level convenience helpers build one packet and submit it in a single
 *   call:
 *       cmd_submit_write_linear(ctx, dst_va, size, data)
 *       cmd_submit_copy_linear (ctx, src_va, dst_va, size)
 *       cmd_submit_const_fill  (ctx, dst_va, size, data)
 *       cmd_submit_atomic      (ctx, dst_va, data)
 *
 *   Low-level path, for full control over cmd_packet_params_t (custom_data,
 *   fence/timestamp packets, etc.):
 *       cmd_place_packet(ctx, &params)          - build the PM4 stream
 *       cmd_submit_packet(ctx)                  - submit it
 *       cmd_place_and_submit_packet(ctx, &p)    - both of the above
 *
 * Semantics
 * ---------
 *   - Submission is asynchronous: cmd_submit_* returns after the CS is queued.
 *     The next submit on the same ctx, or cmd_context_destroy(), transparently
 *     waits for the previous one. Call cmd_wait_completion(ctx) to block
 *     explicitly (e.g. before reading back the destination).
 *   - Operands are plain GPU virtual addresses, not BOs. For every BO-backed
 *     address you pass (source, destination, fence/timestamp target, ...) call
 *     cmd_context_add_resource(ctx, bo) to add the owning BO to the residency
 *     set. The residency set is per-submit: it is cleared automatically once
 *     the packet has been captured for submission, so you must (re-)register
 *     every required BO immediately before each submit.
 *   - One call emits exactly one hardware packet and does NOT chunk. A transfer
 *     larger than the per-packet maximum (see amdgpu_dma_limits_query) will be
 *     rejected; the caller must split it. Return values are 0 on success or a
 *     negative errno (e.g. -ENOTSUP if the IP lacks the requested op).
 *
 * Example: fill then copy 4 KiB over the SDMA ring
 * ------------------------------------------------
 *   cmd_context_t *ctx;
 *   int r;
 *
 *   ctx = cmd_context_create(device, AMD_IP_DMA, 0, false);
 *   if (!ctx)
 *           return -ENODEV;
 *
 *   // Residency is per-submit, so register before every submit.
 *   cmd_context_add_resource(ctx, dst_bo);  // owns dst_va
 *   r = cmd_submit_const_fill(ctx, dst_va, 4096, 0xdeadbeef);
 *   if (!r) {
 *           cmd_context_add_resource(ctx, dst_bo);    // owns dst_va
 *           cmd_context_add_resource(ctx, dst2_bo);   // owns dst2_va
 *           r = cmd_submit_copy_linear(ctx, dst_va, dst2_va, 4096);
 *   }
 *
 *   cmd_wait_completion(ctx);   // ensure the copy has landed
 *   cmd_context_destroy(ctx);   // (would also wait) release resources
 *   return r;
 */
cmd_context_t* cmd_context_create(amdgpu_device_handle device,
                                    enum amd_ip_block_type ip_type,
                                    uint32_t ring_id,
                                    bool user_queue);
void cmd_context_destroy(cmd_context_t *ctx);

/*
 * Register a buffer object in the context's bo_list residency set so it is
 * resident when the next submission executes. See the cmd_* API overview above
 * cmd_context_create() for how this fits into the submission flow.
 *
 * Returns 0 on success, -EINVAL on bad arguments,
 * or -ENOSPC when the residency set (resources[]) is full.
 */
int cmd_context_add_resource(cmd_context_t *ctx, amdgpu_bo_handle bo);

int cmd_place_packet(cmd_context_t *ctx, const cmd_packet_params_t *params);

int cmd_submit_packet(cmd_context_t *ctx);

int cmd_place_and_submit_packet(cmd_context_t *ctx, const cmd_packet_params_t *params);

int cmd_wait_completion(cmd_context_t *ctx);

int cmd_submit_write_linear(cmd_context_t *ctx, uint64_t dst_addr, uint32_t size, uint32_t data);

int cmd_submit_copy_linear(cmd_context_t *ctx, uint64_t src_addr, uint64_t dst_addr, uint32_t size);

int cmd_submit_const_fill(cmd_context_t *ctx, uint64_t dst_addr, uint32_t size, uint32_t data);

int cmd_submit_atomic(cmd_context_t *ctx, uint64_t dst_addr, uint32_t data);

bool cmd_ring_available(amdgpu_device_handle device,
                        enum amd_ip_block_type ip_type,
                       uint32_t ring_id,
                       bool user_queue);

uint32_t cmd_get_available_rings(amdgpu_device_handle device,
                                 enum amd_ip_block_type ip_type,
                                bool user_queue);

/**
 * Query HW and populate DMA transfer limits.
 * All fields in the returned struct are in BYTES.
 */
void amdgpu_dma_limits_query(amdgpu_device_handle device,
                             struct amdgpu_dma_limits *limits);

#endif
