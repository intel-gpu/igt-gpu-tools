// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_gfx.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"

#include "ioctl_wrappers.h"

static const char *
amdgpu_ip_type_name(unsigned ip_type)
{
	switch (ip_type) {
	case AMDGPU_HW_IP_GFX:     return "GFX";
	case AMDGPU_HW_IP_COMPUTE: return "Compute";
	case AMDGPU_HW_IP_DMA:     return "SDMA";
	default:                    return "Unknown";
	}
}

/*
 *
 * Caller need create/release:
 * pm4_src, resources, ib_info, and ibs_request
 * submit command stream described in ibs_request and wait for this IB accomplished
 */

int amdgpu_test_exec_cs_helper(amdgpu_device_handle device, unsigned int ip_type,
				struct amdgpu_ring_context *ring_context, int expect_failure)
{
	int r;
	uint32_t expired;
	uint32_t *ring_ptr;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_fence fence_status = {0};
	amdgpu_va_handle va_handle;
	bool user_queue = ring_context->user_queue;
	const struct amdgpu_ip_block_version *ip_block = NULL;
	amdgpu_bo_handle *all_res;
	uint64_t ib_size;

	ip_block = get_ip_block(device, ip_type);
	all_res = alloca(sizeof(ring_context->resources[0]) * (ring_context->res_cnt + 1));
	ib_size = (uint64_t)ring_context->pm4_size * sizeof(*ring_context->pm4);

	if (expect_failure) {
		/* allocate IB */
		r = amdgpu_bo_alloc_and_map_sync(device, ib_size, 4096,
						 AMDGPU_GEM_DOMAIN_GTT, 0, AMDGPU_VM_MTYPE_UC,
						 &ib_result_handle, &ib_result_cpu,
						 &ib_result_mc_address, &va_handle,
						 ring_context->timeline_syncobj_handle,
						 ++ring_context->point, user_queue);
	} else {
		/* prepare CS */
		igt_assert(ring_context->pm4_dw <= 1024);
		/* allocate IB */
		r = amdgpu_bo_alloc_and_map_sync(device, ib_size, 4096,
						 AMDGPU_GEM_DOMAIN_GTT, 0, AMDGPU_VM_MTYPE_UC,
						 &ib_result_handle, &ib_result_cpu,
						 &ib_result_mc_address, &va_handle,
						 ring_context->timeline_syncobj_handle,
						 ++ring_context->point, user_queue);
	}
	igt_assert_eq(r, 0);

	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device, ring_context->timeline_syncobj_handle,
						 ring_context->point);
		igt_assert_eq(r, 0);
	}

	/* copy PM4 packet to ring from caller */
	ring_ptr = ib_result_cpu;
	memcpy(ring_ptr, ring_context->pm4, ring_context->pm4_dw * sizeof(*ring_context->pm4));

	if (user_queue) {
		if (expect_failure)
			ring_context->submit_mode = UQ_SUBMIT_NO_SYNC;
		else
			ring_context->submit_mode = UQ_SUBMIT_NORMAL;

		r = ip_block->funcs->userq_submit(device, ring_context, ip_type, ib_result_mc_address);
		if (!expect_failure)
			igt_assert_eq(r, 0);
	} else {
		ring_context->ib_info.ib_mc_address = ib_result_mc_address;
		ring_context->ib_info.size = ring_context->pm4_dw;
		if (ring_context->secure)
			ring_context->ib_info.flags |= AMDGPU_IB_FLAGS_SECURE;

		ring_context->ibs_request.ip_type = ip_type;
		ring_context->ibs_request.ring = ring_context->ring_id;
		ring_context->ibs_request.number_of_ibs = 1;
		ring_context->ibs_request.ibs = &ring_context->ib_info;
		ring_context->ibs_request.fence_info.handle = NULL;

		memcpy(all_res, ring_context->resources,
		       sizeof(ring_context->resources[0]) * ring_context->res_cnt);

		all_res[ring_context->res_cnt] = ib_result_handle;

		r = amdgpu_bo_list_create(device, ring_context->res_cnt + 1, all_res,
					  NULL, &ring_context->ibs_request.resources);
		igt_assert_eq(r, 0);

		/* submit CS */
		r = amdgpu_cs_submit(ring_context->context_handle, 0,
				     &ring_context->ibs_request, 1);

		ring_context->err_codes.err_code_cs_submit = r;
		if (expect_failure)
			igt_info("amdgpu_cs_submit %d PID %d\n", r, getpid());
		else {
			/* a good job may be collateral-cancelled by a concurrent
			 * queue reset; tolerate every reset-induced error
			 */
			if (r != -ECANCELED && r != -ENODATA && r != -EHWPOISON &&
			    r != -ETIME)
				igt_assert_eq(r, 0);
		}

		r = amdgpu_bo_list_destroy(ring_context->ibs_request.resources);
		igt_assert_eq(r, 0);

		fence_status.ip_type = ip_type;
		fence_status.ip_instance = 0;
		fence_status.ring = ring_context->ibs_request.ring;
		fence_status.context = ring_context->context_handle;
		fence_status.fence = ring_context->ibs_request.seq_no;

		/* wait for IB accomplished */
		r = amdgpu_cs_query_fence_status(&fence_status,
						 AMDGPU_TIMEOUT_INFINITE,
						 0, &expired);
		ring_context->err_codes.err_code_wait_for_fence = r;
		if (expect_failure) {
			igt_info("EXPECT FAILURE amdgpu_cs_query_fence_status %d\n"
				 "expired %d PID %d\n", r, expired, getpid());
		} else {
			/* a good job may be collateral-cancelled by a concurrent
			 * queue reset; tolerate every reset-induced error
			 */
			if (r != -ECANCELED && r != -ENODATA && r != -ETIME &&
			    r != -EHWPOISON)
				igt_assert_eq(r, 0);
		}
	}
	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, 4096);
	return r;
}

static void amdgpu_create_ip_queues(amdgpu_device_handle device,
                         const struct amdgpu_ip_block_version *ip_block,
                         bool secure, bool user_queue,
                         struct amdgpu_ring_context **ring_context_out,
                         int *available_rings_out)
{
	struct amdgpu_dma_limits limits;
	struct amdgpu_ring_context *ring_context = NULL;
	int available_rings = 0;
	int r, ring_id, pm4_dw;

	/* Get number of available queues */
	struct drm_amdgpu_info_hw_ip hw_ip_info;

	/* First get the hardware IP information */
	memset(&hw_ip_info, 0, sizeof(hw_ip_info));
	r = amdgpu_query_hw_ip_info(device, ip_block->type, 0, &hw_ip_info);
	igt_assert_eq(r, 0);


	amdgpu_dma_limits_query(device, &limits);

	if (user_queue)
		available_rings = hw_ip_info.userq_num_slots ?
			((1 << hw_ip_info.userq_num_slots) - 1) : 1;
	else
		available_rings = hw_ip_info.available_rings;

	if (available_rings <= 0) {
		*ring_context_out = NULL;
		*available_rings_out = 0;
		igt_skip("No available queues for testing\n");
		return;
	}

	/* Allocate and initialize ring_id contexts */
	ring_context = calloc(available_rings, sizeof(*ring_context));
	igt_assert(ring_context);

	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		memset(&ring_context[ring_id], 0, sizeof(ring_context[ring_id]));
		ring_context[ring_id].write_length = amdgpu_dma_default_bytes(&limits, ip_block->type);
		pm4_dw = ring_context[ring_id].write_length / 4 + 16;
		ring_context[ring_id].pm4 = calloc(pm4_dw, sizeof(*ring_context[ring_id].pm4));
		ring_context[ring_id].secure = secure;
		ring_context[ring_id].pm4_size = pm4_dw;
		ring_context[ring_id].res_cnt = 1;
		ring_context[ring_id].user_queue = user_queue;
		if (ip_block->funcs->family_id == FAMILY_GFX1150)
			ring_context[ring_id].max_num_fences_fwm = 4;
		else
			ring_context[ring_id].max_num_fences_fwm = 32;
		igt_assert(ring_context[ring_id].pm4);

		/* Copy the previously queried HW IP info instead of querying again */
		memcpy(&ring_context[ring_id].hw_ip_info, &hw_ip_info, sizeof(hw_ip_info));
	}

	/* Create all queues */
	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		if (user_queue) {
			ip_block->funcs->userq_create(device, &ring_context[ring_id], ip_block->type);
		} else {
			r = amdgpu_cs_ctx_create(device, &ring_context[ring_id].context_handle);
		}
		igt_assert_eq(r, 0);
	}

	*ring_context_out = ring_context;
	*available_rings_out = available_rings;
}

static void amdgpu_command_submission_write_linear(amdgpu_device_handle device,
                         const struct amdgpu_ip_block_version *ip_block,
                         bool secure, bool user_queue,
                         struct amdgpu_ring_context *ring_context,
                         int available_rings)
{
	uint64_t gtt_flags[2] = {0, AMDGPU_GEM_CREATE_CPU_GTT_USWC};
	int i, r, ring_id;

	/* Set encryption flags if needed */
	for (i = 0; secure && (i < 2); i++)
		gtt_flags[i] |= AMDGPU_GEM_CREATE_ENCRYPTED;

	/* Test all queues */
	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		/* Allocate buffer for this ring_id */
		r = amdgpu_bo_alloc_and_map_sync(device,
			ring_context[ring_id].write_length,
			4096, AMDGPU_GEM_DOMAIN_GTT,
			gtt_flags[0],
			AMDGPU_VM_MTYPE_UC,
			&ring_context[ring_id].bo,
			(void **)&ring_context[ring_id].bo_cpu,
			&ring_context[ring_id].bo_mc,
			&ring_context[ring_id].va_handle,
			ring_context[ring_id].timeline_syncobj_handle,
			++ring_context[ring_id].point, user_queue);
		igt_assert_eq(r, 0);

		if (user_queue) {
			r = amdgpu_timeline_syncobj_wait(device,
			ring_context[ring_id].timeline_syncobj_handle,
			ring_context[ring_id].point);
			igt_assert_eq(r, 0);
		}

		/* Clear buffer */
		memset((void *)ring_context[ring_id].bo_cpu, 0,
		       ring_context[ring_id].write_length);

		ring_context[ring_id].resources[0] = ring_context[ring_id].bo;

		/* Submit work */
		ip_block->funcs->write_linear(ip_block->funcs, &ring_context[ring_id],
			  &ring_context[ring_id].pm4_dw);

		amdgpu_test_exec_cs_helper(device, ip_block->type, &ring_context[ring_id], 0);

		/* Verification */
		if (!secure) {
			r = ip_block->funcs->compare(ip_block->funcs, &ring_context[ring_id], 4);
			igt_assert_eq(r, 0);
		} else if (ip_block->type == AMDGPU_HW_IP_GFX) {
			ip_block->funcs->write_linear_atomic(ip_block->funcs, &ring_context[ring_id], &ring_context[ring_id].pm4_dw);
			amdgpu_test_exec_cs_helper(device, ip_block->type, &ring_context[ring_id], 0);
		} else if (ip_block->type == AMDGPU_HW_IP_DMA) {
			uint32_t original_value = ring_context[ring_id].bo_cpu[0];
			ip_block->funcs->write_linear_atomic(ip_block->funcs, &ring_context[ring_id], &ring_context[ring_id].pm4_dw);
			amdgpu_test_exec_cs_helper(device, ip_block->type, &ring_context[ring_id], 0);
			igt_assert_neq(ring_context[ring_id].bo_cpu[0], original_value);

			original_value = ring_context[ring_id].bo_cpu[0];
			ip_block->funcs->write_linear_atomic(ip_block->funcs, &ring_context[ring_id], &ring_context[ring_id].pm4_dw);
			amdgpu_test_exec_cs_helper(device, ip_block->type, &ring_context[ring_id], 0);
			igt_assert_eq(ring_context[ring_id].bo_cpu[0], original_value);
		}

		/* Clean up buffer */
		amdgpu_bo_unmap_and_free(ring_context[ring_id].bo, ring_context[ring_id].va_handle,
		ring_context[ring_id].bo_mc,
		ring_context[ring_id].write_length);
	}
}

static void amdgpu_destroy_ip_queues(amdgpu_device_handle device,
                         const struct amdgpu_ip_block_version *ip_block,
                         bool secure, bool user_queue,
                         struct amdgpu_ring_context *ring_context,
                         int available_rings)
{
	int ring_id, r;

	/* Destroy all queues and free resources */
	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		if (user_queue) {
			ip_block->funcs->userq_destroy(device, &ring_context[ring_id], ip_block->type);
		} else {
			r = amdgpu_cs_ctx_free(ring_context[ring_id].context_handle);
			igt_assert_eq(r, 0);
		}
		free(ring_context[ring_id].pm4);
	}

	free(ring_context);
}

void amdgpu_command_submission_write_linear_helper2(amdgpu_device_handle device,
                            unsigned type,
                            bool secure, bool user_queue)
{
	struct amdgpu_ring_context *gfx_ring_context = NULL;
	struct amdgpu_ring_context *compute_ring_context = NULL;
	struct amdgpu_ring_context *sdma_ring_context = NULL;

	// Separate variables for each type of IP block's ring_id count
	int num_gfx_queues = 0;
	int num_compute_queues = 0;
	int num_sdma_queues = 0;

	/* Create IP slots for each block */
	if (type & AMDGPU_HW_IP_GFX)
		amdgpu_create_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_GFX), secure, user_queue, &gfx_ring_context, &num_gfx_queues);

	if (type & AMDGPU_HW_IP_COMPUTE)
		amdgpu_create_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE), secure, user_queue, &compute_ring_context, &num_compute_queues);

	if (type & AMDGPU_HW_IP_DMA)
		amdgpu_create_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_DMA), secure, user_queue, &sdma_ring_context, &num_sdma_queues);

	/* Submit commands to all IP blocks */
	if (gfx_ring_context)
		amdgpu_command_submission_write_linear(device, get_ip_block(device, AMDGPU_HW_IP_GFX), secure, user_queue,
												gfx_ring_context, num_gfx_queues);

	if (compute_ring_context)
		amdgpu_command_submission_write_linear(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE), secure, user_queue,
												compute_ring_context, num_compute_queues);

	if (sdma_ring_context)
		amdgpu_command_submission_write_linear(device, get_ip_block(device, AMDGPU_HW_IP_DMA), secure, user_queue,
												sdma_ring_context, num_sdma_queues);

	/* Clean up resources */
	if (gfx_ring_context)
		amdgpu_destroy_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_GFX), secure, user_queue,
												gfx_ring_context, num_gfx_queues);

	if (compute_ring_context)
		amdgpu_destroy_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE), secure, user_queue,
												compute_ring_context, num_compute_queues);

	if (sdma_ring_context)
		amdgpu_destroy_ip_queues(device, get_ip_block(device, AMDGPU_HW_IP_DMA), secure, user_queue,
												sdma_ring_context, num_sdma_queues);
}

void amdgpu_command_submission_write_linear_helper(amdgpu_device_handle device,
						   const struct amdgpu_ip_block_version *ip_block,
						   bool secure, bool user_queue)

{
	struct amdgpu_dma_limits limits;
	struct amdgpu_ring_context *ring_context;
	int i, r, loop, ring_id, pm4_dw;

	uint64_t gtt_flags[2] = {0, AMDGPU_GEM_CREATE_CPU_GTT_USWC};
	uint32_t available_rings = 0;

	amdgpu_dma_limits_query(device, &limits);

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);
	/* setup parameters — write_linear embeds inline data in PM4,
	 * so pm4 buffer must hold header (max 10 DWORDs) + write_length/4 DWORDs.
	 */
	ring_context->write_length = amdgpu_dma_default_bytes(&limits, ip_block->type);
	pm4_dw = ring_context->write_length / 4 + 16;
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = secure;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	ring_context->user_queue = user_queue;
	igt_assert(ring_context->pm4);

	r = amdgpu_query_hw_ip_info(device, ip_block->type, 0, &ring_context->hw_ip_info);
	igt_assert_eq(r, 0);

	if (user_queue)
		available_rings = ring_context->hw_ip_info.userq_num_slots ?
			((1 << ring_context->hw_ip_info.userq_num_slots) -1) : 1;
	else
		available_rings = ring_context->hw_ip_info.available_rings;

	for (i = 0; secure && (i < 2); i++)
		gtt_flags[i] |= AMDGPU_GEM_CREATE_ENCRYPTED;

	if (user_queue) {
		ip_block->funcs->userq_create(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		loop = 0;
		ring_context->ring_id = ring_id;
		while (loop < 2) {
			/* allocate UC bo for sDMA use */
			r = amdgpu_bo_alloc_and_map_sync(device,
							 ring_context->write_length,
							 4096, AMDGPU_GEM_DOMAIN_GTT,
							 gtt_flags[loop],
							 AMDGPU_VM_MTYPE_UC,
							 &ring_context->bo,
							 (void **)&ring_context->bo_cpu,
							 &ring_context->bo_mc,
							 &ring_context->va_handle,
							 ring_context->timeline_syncobj_handle,
							 ++ring_context->point, user_queue);

			igt_assert_eq(r, 0);

			if (user_queue) {
				r = amdgpu_timeline_syncobj_wait(device,
					ring_context->timeline_syncobj_handle,
					ring_context->point);
				igt_assert_eq(r, 0);
			}

			/* clear bo */
			memset((void *)ring_context->bo_cpu, 0,
			       ring_context->write_length);

			ring_context->resources[0] = ring_context->bo;

			ip_block->funcs->write_linear(ip_block->funcs, ring_context,
						      &ring_context->pm4_dw);

			ring_context->ring_id = ring_id;

			igt_info("%s write_linear: ring %d, size %lu bytes (%lu KB)\n",
				amdgpu_ip_type_name(ip_block->type), ring_id,
				(unsigned long)ring_context->write_length,
				(unsigned long)ring_context->write_length / 1024);
			 amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

			/* verify if SDMA test result meets with expected */
			i = 0;
			if (!secure) {
				r = ip_block->funcs->compare(ip_block->funcs, ring_context, 4);
				igt_assert_eq(r, 0);
			} else if (ip_block->type == AMDGPU_HW_IP_GFX) {
				ip_block->funcs->write_linear_atomic(ip_block->funcs, ring_context, &ring_context->pm4_dw);
				amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);
			} else if (ip_block->type == AMDGPU_HW_IP_DMA) {
				/* restore the bo_cpu to compare */
				ring_context->bo_cpu_origin = ring_context->bo_cpu[0];
				ip_block->funcs->write_linear_atomic(ip_block->funcs, ring_context, &ring_context->pm4_dw);

				amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

				igt_assert_neq(ring_context->bo_cpu[0], ring_context->bo_cpu_origin);
				/* restore again, here dest_data should be */
				ring_context->bo_cpu_origin = ring_context->bo_cpu[0];
				ip_block->funcs->write_linear_atomic(ip_block->funcs, ring_context, &ring_context->pm4_dw);

				amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);
				/* here bo_cpu[0] should be unchanged, still is 0x12345678, otherwise failed*/
				igt_assert_eq(ring_context->bo_cpu[0], ring_context->bo_cpu_origin);
			}

			amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle, ring_context->bo_mc,
						 ring_context->write_length);
			loop++;
		}
	}
	/* clean resources */
	free(ring_context->pm4);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_free(ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	free(ring_context);
}


/**
 *
 * @param device
 * @param ip_type
 * @param user_queue
 */
void amdgpu_command_submission_const_fill_helper(amdgpu_device_handle device,
						 const struct amdgpu_ip_block_version *ip_block,
						 bool user_queue)
{
	const int pm4_dw = 256;
	struct amdgpu_dma_limits limits;

	struct amdgpu_ring_context *ring_context;
	int r, loop, ring_id;
	uint32_t available_rings = 0;
	uint64_t gtt_flags[2] = {0, AMDGPU_GEM_CREATE_CPU_GTT_USWC};
	bool do_once = true;

	amdgpu_dma_limits_query(device, &limits);

	ring_context = calloc(1, sizeof(*ring_context));
	/* const_fill/copy packets have no inline data — pm4_dw=256 is ample */
	ring_context->write_length = amdgpu_dma_default_bytes(&limits, ip_block->type);
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = false;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	ring_context->user_queue = user_queue;
	igt_assert(ring_context->pm4);
	r = amdgpu_query_hw_ip_info(device, ip_block->type, 0, &ring_context->hw_ip_info);
	igt_assert_eq(r, 0);

	if (user_queue)
		available_rings = ring_context->hw_ip_info.userq_num_slots ?
			((1 << ring_context->hw_ip_info.userq_num_slots) -1) : 1;
	else
		available_rings = ring_context->hw_ip_info.available_rings;

	if (user_queue) {
		ip_block->funcs->userq_create(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		/* prepare resource */
		loop = 0;
		ring_context->ring_id = ring_id;
			while (loop < 2) {
				ring_context->write_length = (do_once == true && ring_context->ring_id == 0) ?
					amdgpu_dma_max_bytes(&limits, ip_block->type) :
					amdgpu_dma_default_bytes(&limits, ip_block->type);
				do_once = false;
				/* allocate UC bo for sDMA use */
				r = amdgpu_bo_alloc_and_map_sync(device, ring_context->write_length,
							 4096, AMDGPU_GEM_DOMAIN_GTT,
							 gtt_flags[loop],
							 AMDGPU_VM_MTYPE_UC,
							 &ring_context->bo,
							 (void **)&ring_context->bo_cpu,
							 &ring_context->bo_mc,
							 &ring_context->va_handle,
							 ring_context->timeline_syncobj_handle,
							 ++ring_context->point, user_queue);
			igt_assert_eq(r, 0);

			if (user_queue) {
				r = amdgpu_timeline_syncobj_wait(device,
					ring_context->timeline_syncobj_handle,
					ring_context->point);
				igt_assert_eq(r, 0);
			}

			/* clear bo */
			memset((void *)ring_context->bo_cpu, 0, ring_context->write_length);

			ring_context->resources[0] = ring_context->bo;

			/* fulfill PM4: test DMA const fill */
			ip_block->funcs->const_fill(ip_block->funcs, ring_context, &ring_context->pm4_dw);

			igt_info("%s const_fill: ring %d, size %lu bytes (%lu KB)\n",
				amdgpu_ip_type_name(ip_block->type), ring_id,
				(unsigned long)ring_context->write_length,
				(unsigned long)ring_context->write_length / 1024);
			amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

			/* verify if SDMA test result meets with expected */
			r = ip_block->funcs->compare(ip_block->funcs, ring_context, 4);
			igt_assert_eq(r, 0);

			amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle, ring_context->bo_mc,
					 ring_context->write_length);
			loop++;
		}
	}
	/* clean resources */
	free(ring_context->pm4);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_free(ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	free(ring_context);
}

/**
 *
 * @param device
 * @param ip_type
 * @param user_queue
 */
void amdgpu_command_submission_copy_linear_helper(amdgpu_device_handle device,
						  const struct amdgpu_ip_block_version *ip_block,
						  bool user_queue)
{
	const int pm4_dw = 256;
	struct amdgpu_dma_limits limits;

	struct amdgpu_ring_context *ring_context;
	int r, loop1, loop2, ring_id;
	uint32_t available_rings = 0;
	uint64_t gtt_flags[2] = {0, AMDGPU_GEM_CREATE_CPU_GTT_USWC};
	bool do_once = true;

	amdgpu_dma_limits_query(device, &limits);

	ring_context = calloc(1, sizeof(*ring_context));
	/* copy_linear packets have no inline data — pm4_dw=256 is ample */
	ring_context->write_length = amdgpu_dma_default_bytes(&limits, ip_block->type);
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = false;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 2;
	ring_context->user_queue = user_queue;
	igt_assert(ring_context->pm4);
	r = amdgpu_query_hw_ip_info(device, ip_block->type, 0, &ring_context->hw_ip_info);
	igt_assert_eq(r, 0);

	if (user_queue)
		available_rings = ring_context->hw_ip_info.userq_num_slots ?
			((1 << ring_context->hw_ip_info.userq_num_slots) -1) : 1;
	else
		available_rings = ring_context->hw_ip_info.available_rings;

	if (user_queue) {
		ip_block->funcs->userq_create(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	for (ring_id = 0; (1 << ring_id) & available_rings; ring_id++) {
		loop1 = loop2 = 0;
			ring_context->ring_id = ring_id;
		/* run 9 circle to test all mapping combination */
		while (loop1 < 2) {
			while (loop2 < 2) {
				/* allocate UC bo1for sDMA use */
				ring_context->write_length = (do_once && ring_context->ring_id == 0) ?
					amdgpu_dma_max_bytes(&limits, ip_block->type) :
					amdgpu_dma_default_bytes(&limits, ip_block->type);
				do_once = false;
				r = amdgpu_bo_alloc_and_map_sync(device, ring_context->write_length,
							4096, AMDGPU_GEM_DOMAIN_GTT,
							gtt_flags[loop1],
							AMDGPU_VM_MTYPE_UC,
							&ring_context->bo,
							(void **)&ring_context->bo_cpu,
							&ring_context->bo_mc,
							&ring_context->va_handle,
							ring_context->timeline_syncobj_handle,
							++ring_context->point, user_queue);
				igt_assert_eq(r, 0);

				if (user_queue) {
					r = amdgpu_timeline_syncobj_wait(device,
						ring_context->timeline_syncobj_handle,
						ring_context->point);
					igt_assert_eq(r, 0);
				}

				/* set bo_cpu */
				memset((void *)ring_context->bo_cpu, ip_block->funcs->pattern, ring_context->write_length);

				/* allocate UC bo2 for sDMA use */
				r = amdgpu_bo_alloc_and_map_sync(device,
							ring_context->write_length,
							4096, AMDGPU_GEM_DOMAIN_GTT,
							gtt_flags[loop2],
							AMDGPU_VM_MTYPE_UC,
							&ring_context->bo2,
							(void **)&ring_context->bo2_cpu,
							&ring_context->bo_mc2,
							&ring_context->va_handle2,
							ring_context->timeline_syncobj_handle,
							++ring_context->point, user_queue);
				igt_assert_eq(r, 0);

				if (user_queue) {
					r = amdgpu_timeline_syncobj_wait(device,
						ring_context->timeline_syncobj_handle,
						ring_context->point);
					igt_assert_eq(r, 0);
				}

				/* clear bo2_cpu */
				memset((void *)ring_context->bo2_cpu, 0, ring_context->write_length);

				ring_context->resources[0] = ring_context->bo;
				ring_context->resources[1] = ring_context->bo2;

				ip_block->funcs->copy_linear(ip_block->funcs, ring_context, &ring_context->pm4_dw);

				igt_info("%s copy_linear: ring %d, size %lu bytes (%lu KB)\n",
					amdgpu_ip_type_name(ip_block->type), ring_id,
					(unsigned long)ring_context->write_length,
					(unsigned long)ring_context->write_length / 1024);
				amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

				/* verify if SDMA test result meets with expected */
				r = ip_block->funcs->compare_pattern(ip_block->funcs, ring_context, 4);
				igt_assert_eq(r, 0);

				amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle, ring_context->bo_mc,
						 ring_context->write_length);
				amdgpu_bo_unmap_and_free(ring_context->bo2, ring_context->va_handle2, ring_context->bo_mc2,
						 ring_context->write_length);
				loop2++;
			}
			loop1++;
		}
	}

	/* clean resources */
	free(ring_context->pm4);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_free(ring_context->context_handle);
		igt_assert_eq(r, 0);
	}

	free(ring_context);
}

/*
 * Weak wrapper: use amdgpu_bo_cpu_cache_flush() if present (newer libdrm),
 * otherwise act as a no-op to keep older libdrm builds working.
 */
extern int amdgpu_bo_cpu_cache_flush(amdgpu_bo_handle bo)
	__attribute__((weak));

static int local_bo_cpu_cache_flush(amdgpu_bo_handle bo)
{
	if (amdgpu_bo_cpu_cache_flush)
		return amdgpu_bo_cpu_cache_flush(bo);
	return 0;
}

void
amdgpu_command_ce_write_fence(amdgpu_device_handle dev,
					  amdgpu_context_handle ctx)
{
	int r;
	const unsigned nop_dw = 256 * 1024;
	const unsigned total_dw = nop_dw + 5;	/* NOPs + WRITE_DATA(5 DW) */
	amdgpu_bo_handle dst_bo;
	amdgpu_va_handle dst_va_handle;
	uint64_t dst_mc_address;
	uint32_t *dst_cpu;
	amdgpu_bo_handle ib_bo;
	amdgpu_va_handle ib_va_handle;
	uint64_t ib_mc_address;
	uint32_t *ib_cpu;
	unsigned ib_size_bytes = total_dw * sizeof(uint32_t);
	unsigned dw = 0;
	bool do_cache_flush = true;
	bool do_timing = true;
	struct timespec ts_start;
	uint64_t fence_delta_usec = 0, visible_delta_usec = 0;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_request req;
	struct amdgpu_cs_fence fence;
	uint32_t expired = 0;
	struct amdgpu_cmd_base *base = get_cmd_base();
	const struct amdgpu_ip_block_version *ip_block =
		get_ip_block(dev, AMD_IP_GFX);

	igt_require_f(amdgpu_ce_cs_enabled(),
		      "CE CS blocked; reload amdgpu with debug_mask=0x400 to enable\n");

	/* destination buffer */
	r = amdgpu_bo_alloc_and_map(dev, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &dst_bo, (void **)&dst_cpu,
				    &dst_mc_address, &dst_va_handle);
	igt_assert_eq(r, 0);
	dst_cpu[0] = 0;
	if (do_cache_flush) {
		r = local_bo_cpu_cache_flush(dst_bo);
		igt_assert_eq(r, 0);
	}

	/* command buffer (IB) */
	r = amdgpu_bo_alloc_and_map(dev, ib_size_bytes, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_bo, (void **)&ib_cpu,
				    &ib_mc_address, &ib_va_handle);
	igt_assert_eq(r, 0);

	/* attach PM4 builder */
	base->attach_buf(base, ib_cpu, ib_size_bytes);

	/* large NOP train via hook */
	ip_block->funcs->gfx_emit_nops(base, nop_dw);

	/* CE WRITE_DATA (mem dst) via hook, write 0xdeadbeef to dst */
	ip_block->funcs->gfx_write_data_mem(ip_block->funcs, base,
					    2,			/* CE engine sel */
					    dst_mc_address,
					    0xdeadbeef,
					    true);		/* WR_CONFIRM */

	dw = base->cdw;
	igt_assert_eq(dw, total_dw);

	/* flush IB CPU caches if supported */
	r = local_bo_cpu_cache_flush(ib_bo);
	igt_assert_eq(r, 0);

	/* submit CE IB */
	memset(&ib_info, 0, sizeof(ib_info));
	ib_info.ib_mc_address = ib_mc_address;
	ib_info.size = dw;
	ib_info.flags = AMDGPU_IB_FLAG_CE;

	memset(&req, 0, sizeof(req));
	req.ip_type = AMDGPU_HW_IP_GFX;
	req.ring = 0;
	req.number_of_ibs = 1;
	req.ibs = &ib_info;
	req.resources = NULL;
	req.fence_info.handle = NULL;

	if (do_timing)
		igt_gettime(&ts_start);

	r = amdgpu_cs_submit(ctx, 0, &req, 1);
	igt_assert_eq(r, 0);

	/* wait fence */
	memset(&fence, 0, sizeof(fence));
	fence.context = ctx;
	fence.ip_type = req.ip_type;
	fence.ip_instance = 0;
	fence.ring = req.ring;
	fence.fence = req.seq_no;
	r = amdgpu_cs_query_fence_status(&fence,
					 AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert(expired);
	if (do_timing)
		fence_delta_usec = igt_nsec_elapsed(&ts_start) / 1000;

	/* poll until visible */
	for (;;) {
		if (do_cache_flush) {
			r = local_bo_cpu_cache_flush(dst_bo);
			igt_assert_eq(r, 0);
		}
		if (dst_cpu[0] == 0xdeadbeef) {
			if (do_timing)
				visible_delta_usec = igt_nsec_elapsed(&ts_start) / 1000;
			break;
		}
		usleep(1000);
	}

	if (do_timing) {
		igt_info("ce-write-fence: visible after %llu us (fence) and %llu us total\n",
			 (unsigned long long)fence_delta_usec,
			 (unsigned long long)visible_delta_usec);
	}

	igt_assert_eq(dst_cpu[0], 0xdeadbeef);

	amdgpu_bo_unmap_and_free(ib_bo, ib_va_handle, ib_mc_address, ib_size_bytes);
	amdgpu_bo_unmap_and_free(dst_bo, dst_va_handle, dst_mc_address, 4096);
	free_cmd_base(base);
}

/**
 * command context creation
 *
 * @param device AMDGPU device handle
 * @param ip_type IP block type (GFX, DMA, etc.)
 * @param ring_id Ring index to use
 * @param user_queue Whether to use user queue mode
 * @param write_length Default per-packet transfer size in bytes (overridden per
 *                     packet by cmd_packet_params_t::size)
 * @return New command context, or NULL on failure
 */
cmd_context_t* cmd_context_create(amdgpu_device_handle device,
                                    enum amd_ip_block_type ip_type,
                                    uint32_t ring_id,
                                    bool user_queue)
{
	cmd_context_t *ctx;
	int r;

	if (!device)
		return NULL;

	/* Check if the requested ring is available */
	if (!cmd_ring_available(device, ip_type, ring_id, user_queue))
		return NULL;

	/* Allocate context structure */
	ctx = calloc(1, sizeof(cmd_context_t));
	if (!ctx)
		return NULL;

	ctx->device = device;
	ctx->ip_type = ip_type;
	ctx->user_queue = user_queue;
	ctx->last_submit_seq = 0;
	ctx->initialized = false;

	/* Get IP block for the specified IP type */
	ctx->ip_block = get_ip_block(device, (unsigned int)ip_type);
	if (!ctx->ip_block) {
		free(ctx);
		return NULL;
	}

	/* Allocate ring context structure */
	ctx->ring_ctx = calloc(1, sizeof(struct amdgpu_ring_context));
	if (!ctx->ring_ctx) {
		free(ctx);
		return NULL;
	}

	/* Setup ring context parameters */
	ctx->ring_ctx->pm4_size = 256;
	ctx->ring_ctx->pm4 = calloc(ctx->ring_ctx->pm4_size, sizeof(uint32_t));
	if (!ctx->ring_ctx->pm4) {
		free(ctx->ring_ctx);
		free(ctx);
		return NULL;
	}

	ctx->ring_ctx->res_cnt = 0;
	ctx->ring_ctx->ring_id = ring_id;
	ctx->ring_ctx->secure = false;
	ctx->ring_ctx->user_queue = user_queue;

	if (user_queue) {
	/* Initialize user queue if requested */
		if (!ctx->ip_block->funcs->userq_create) {
			free(ctx->ring_ctx->pm4);
			free(ctx->ring_ctx);
			free(ctx);
			return NULL;
		}
		ctx->ip_block->funcs->userq_create(device, ctx->ring_ctx, (unsigned int)ip_type);
	} else {
		/* Create regular command submission context */
		r = amdgpu_cs_ctx_create(device, &ctx->ring_ctx->context_handle);
		if (r) {
			free(ctx->ring_ctx->pm4);
			free(ctx->ring_ctx);
			free(ctx);
			return NULL;
		}
	}

	/*
	 * Allocate a single per-context IB sized to the PM4 buffer capacity.
	 *
	 * For user queues the VA mapping is performed asynchronously against the
	 * VM timeline, so allocate via the sync/UQ path and wait for the mapping
	 * to complete before the IB is ever used by the first submit. Regular
	 * queues fall back to the synchronous map inside
	 * amdgpu_bo_alloc_and_map_sync().
	 */
	r = amdgpu_bo_alloc_and_map_sync(device,
					 ctx->ring_ctx->pm4_size * sizeof(uint32_t),
					 4096, AMDGPU_GEM_DOMAIN_GTT, 0,
					 AMDGPU_VM_MTYPE_UC,
					 &ctx->ib_bo, &ctx->ib_cpu, &ctx->ib_mc,
					 &ctx->ib_va,
					 ctx->ring_ctx->timeline_syncobj_handle,
					 ++ctx->ring_ctx->point, user_queue);
	if (r) {
		if (user_queue) {
			if (ctx->ip_block->funcs->userq_destroy)
				ctx->ip_block->funcs->userq_destroy(device, ctx->ring_ctx,
								    (unsigned int)ip_type);
		} else {
			amdgpu_cs_ctx_free(ctx->ring_ctx->context_handle);
		}
		free(ctx->ring_ctx->pm4);
		free(ctx->ring_ctx);
		free(ctx);
		return NULL;
	}

	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device,
						 ctx->ring_ctx->timeline_syncobj_handle,
						 ctx->ring_ctx->point);
		if (r) {
			amdgpu_bo_unmap_and_free(ctx->ib_bo, ctx->ib_va, ctx->ib_mc,
						 ctx->ring_ctx->pm4_size * sizeof(uint32_t));
			if (ctx->ip_block->funcs->userq_destroy)
				ctx->ip_block->funcs->userq_destroy(device, ctx->ring_ctx,
								    (unsigned int)ip_type);
			free(ctx->ring_ctx->pm4);
			free(ctx->ring_ctx);
			free(ctx);
			return NULL;
		}
	}

	ctx->initialized = true;
	return ctx;
}

/**
 * context destruction function
 *
 * @param ctx Command context to destroy
 */
void cmd_context_destroy(cmd_context_t *ctx)
{
	if (!ctx || !ctx->initialized)
		return;

	/* Free the per-context IB. */
	if (ctx->ib_bo) {
		cmd_wait_completion(ctx);
		amdgpu_bo_unmap_and_free(ctx->ib_bo, ctx->ib_va, ctx->ib_mc,
					 ctx->ring_ctx->pm4_size * sizeof(uint32_t));
		ctx->ib_bo = NULL;
	}

	if (ctx->ring_ctx) {
		/* Clean up command submission context */
		if (ctx->user_queue) {
			if (ctx->ip_block && ctx->ip_block->funcs->userq_destroy) {
				ctx->ip_block->funcs->userq_destroy(ctx->device, ctx->ring_ctx,
							  (unsigned int)ctx->ip_type);
			}
		} else {
			if (ctx->ring_ctx->context_handle) {
				amdgpu_cs_ctx_free(ctx->ring_ctx->context_handle);
			}
		}

		/* Free PM4 command buffer */
		if (ctx->ring_ctx->pm4) {
			free(ctx->ring_ctx->pm4);
		}

		free(ctx->ring_ctx);
	}

	free(ctx);
}

int cmd_context_add_resource(cmd_context_t *ctx, amdgpu_bo_handle bo)
{
	struct amdgpu_ring_context *rc;
	int i;

	if (!ctx || !ctx->initialized || !ctx->ring_ctx || !bo)
		return -EINVAL;

	rc = ctx->ring_ctx;

	/* Deduplicate: an already-registered BO needs no second slot. */
	for (i = 0; i < rc->res_cnt; i++) {
		if (rc->resources[i] == bo)
			return 0;
	}

	if (rc->res_cnt >= (int)ARRAY_SIZE(rc->resources))
		return -ENOSPC;

	rc->resources[rc->res_cnt++] = bo;
	return 0;
}

static void cmd_clear_packet_inputs(cmd_context_t *ctx)
{
	struct amdgpu_ring_context *rc = ctx->ring_ctx;

	rc->bo_mc = 0;
	rc->bo_mc2 = 0;
	rc->write_data = 0;
	rc->write_data_valid = false;
	rc->write_length = 0;

	/* Drop the per-submit residency set. */
	memset(rc->resources, 0, sizeof(rc->resources));
	rc->res_cnt = 0;
}

/**
 * Submit command packet without waiting - Fixed submission logic
 */
int cmd_submit_packet(cmd_context_t *ctx)
{
	amdgpu_bo_list_handle bo_list;
	int r;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	amdgpu_bo_handle all_res[5] = {0};
	uint32_t res_count = 0;
	uint32_t i;

	if (!ctx || !ctx->initialized || !ctx->ring_ctx) {
		igt_debug("Invalid context parameters\n");
		return -EINVAL;
	}

	/* Check required parameters: the PM4 stream must be non-empty and fit
	 * in the per-context IB allocated by cmd_context_create().
	 */
	if (ctx->ring_ctx->pm4_dw == 0 || ctx->ring_ctx->pm4_dw > ctx->ring_ctx->pm4_size) {
		igt_debug("Invalid PM4 size: %u (must be 1-%u)\n",
			  ctx->ring_ctx->pm4_dw, ctx->ring_ctx->pm4_size);
		return -EINVAL;
	}

	igt_assert_f(ctx->ib_bo && ctx->ring_ctx->pm4_dw <= ctx->ring_ctx->pm4_size,
		     "PM4 stream (%u dw) exceeds IB capacity (%u dw)\n",
		     ctx->ring_ctx->pm4_dw, ctx->ring_ctx->pm4_size);

	r = cmd_wait_completion(ctx);
	if (r) {
		igt_debug("Wait for previous submission failed: %d\n", r);
		return r;
	}

	memcpy(ctx->ib_cpu, ctx->ring_ctx->pm4,
	       ctx->ring_ctx->pm4_dw * sizeof(uint32_t));

	/* For user queues, use userq_submit */
	if (ctx->user_queue) {
		if (!ctx->ip_block || !ctx->ip_block->funcs->userq_submit) {
			igt_debug("User queue submission not supported\n");
			return -ENOTSUP;
		}

		ctx->ip_block->funcs->userq_submit(ctx->device, ctx->ring_ctx,
		      (unsigned int)ctx->ip_type,
		      ctx->ib_mc);
		ctx->last_submit_seq = ctx->ring_ctx->point;
		ctx->submit_pending = true;
		igt_info("User queue submission successful, point=%lu\n", ctx->last_submit_seq);

		cmd_clear_packet_inputs(ctx);
		return 0;
	}

	/* For regular queues, setup and submit CS */
	memset(&ibs_request, 0, sizeof(ibs_request));
	memset(&ib_info, 0, sizeof(ib_info));

	/* Point the IB info at the per-context IB */
	ib_info.ib_mc_address = ctx->ib_mc;
	ib_info.size = ctx->ring_ctx->pm4_dw;
	if (ctx->ring_ctx->secure)
		ib_info.flags |= AMDGPU_IB_FLAGS_SECURE;

	/* Setup submission request */
	ibs_request.ip_type = (unsigned int)ctx->ip_type;
	ibs_request.ring = ctx->ring_ctx->ring_id;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;

	/* Create resource list containing both original resources and IB */
	/* Add original resources */
	for (i = 0; i < ctx->ring_ctx->res_cnt && i < 4; i++) {
		if (ctx->ring_ctx->resources[i]) {
			all_res[res_count++] = ctx->ring_ctx->resources[i];
		}
	}

	/* Add IB as the last resource */
	all_res[res_count++] = ctx->ib_bo;

	/* Create BO list */
	r = amdgpu_bo_list_create(ctx->device, res_count, all_res, NULL, &bo_list);
	if (r) {
		igt_debug("amdgpu_bo_list_create failed: %d\n", r);
		return r;
	}

	ibs_request.resources = bo_list;

	/* Submit command - strict error checking */
	r = amdgpu_cs_submit(ctx->ring_ctx->context_handle, 0, &ibs_request, 1);

	if (r != 0) {
		igt_debug("amdgpu_cs_submit failed: %d\n", r);
		amdgpu_bo_list_destroy(bo_list);
		return r;
	}

	/* Record submission sequence number for waiting */
	ctx->last_submit_seq = ibs_request.seq_no;
	igt_debug("Command submitted successfully, seq_no=%lu\n", ctx->last_submit_seq);

	/* The kernel has snapshotted the bo_list; it can be released now. */
	amdgpu_bo_list_destroy(bo_list);

	ctx->submit_pending = true;
	cmd_clear_packet_inputs(ctx);

	return 0;
}

static void cmd_assert_size_within_packet_limit(cmd_context_t *ctx, uint32_t size,
						const char *op)
{
	struct amdgpu_dma_limits limits;
	uint64_t max_bytes;

	amdgpu_dma_limits_query(ctx->device, &limits);
	max_bytes = amdgpu_dma_max_bytes(&limits, (unsigned int)ctx->ip_type);

	igt_assert_f(size <= max_bytes,
		     "%s size %u exceeds single-packet max %llu bytes for this IP; split the transfer\n",
		     op, size, (unsigned long long)max_bytes);
}

/*
 * WRITE_LINEAR inlines the payload directly into the PM4 stream as
 * write_length/4 dwords (plus a small packet header), so its real limit is the
 * PM4 buffer capacity, not the DMA engine's per-packet transfer maximum. Reject
 * transfers whose inlined form would overrun pm4[] before the builder writes it.
 */
static void cmd_assert_inline_payload_fits(cmd_context_t *ctx, uint32_t size,
					   const char *op)
{
	/* Upper bound on the header across the SDMA/GFX write_linear encodings. */
	const uint32_t header_dw = 8;
	uint64_t need_dw = (uint64_t)header_dw + (size / 4);

	igt_assert_f(need_dw <= ctx->ring_ctx->pm4_size,
		     "%s size %u needs %llu dw, exceeds PM4 buffer (%u dw); split the transfer\n",
		     op, size, (unsigned long long)need_dw, ctx->ring_ctx->pm4_size);
}

int cmd_place_packet(cmd_context_t *ctx, const cmd_packet_params_t *params)
{
	int result = -EINVAL;

	if (!ctx || !ctx->initialized || !ctx->ip_block || !params)
		return -EINVAL;

	if (!ctx->ring_ctx)
		return -EINVAL;

	/* Build the command packet based on type */
	switch (params->type) {
	case CMD_PACKET_WRITE_LINEAR:
		if (!ctx->ip_block->funcs->write_linear)
			return -ENOTSUP;

		/* Destination operand comes from the caller. */
		if (params->dst_addr)
			ctx->ring_ctx->bo_mc = params->dst_addr;
		igt_assert_f(ctx->ring_ctx->bo_mc,
			     "WRITE_LINEAR requires a destination address\n");
		cmd_assert_inline_payload_fits(ctx, params->size, "WRITE_LINEAR");
		ctx->ring_ctx->write_length = params->size;
		ctx->ring_ctx->write_data = params->data;
		ctx->ring_ctx->write_data_valid = true;

		/* Build PM4 packet */
		result = ctx->ip_block->funcs->write_linear(ctx->ip_block->funcs,
						       ctx->ring_ctx,
						       &ctx->ring_ctx->pm4_dw);
		return result;

	case CMD_PACKET_WRITE_ATOMIC:
		if (!ctx->ip_block->funcs->write_linear_atomic)
			return -ENOTSUP;

		/* Destination operand comes from the caller. */
		if (params->dst_addr)
			ctx->ring_ctx->bo_mc = params->dst_addr;
		igt_assert_f(ctx->ring_ctx->bo_mc,
			     "WRITE_ATOMIC requires a destination address\n");
		ctx->ring_ctx->write_length = 1; /* Atomic operations typically work on single DWORD */

		result = ctx->ip_block->funcs->write_linear_atomic(ctx->ip_block->funcs,
						      ctx->ring_ctx,
						      &ctx->ring_ctx->pm4_dw);
		return result;

	case CMD_PACKET_COPY_LINEAR:
		if (!ctx->ip_block->funcs->copy_linear)
			return -ENOTSUP;

		/* Copy takes both operands from the caller: bo_mc = source VA,
		 * bo_mc2 = destination VA.
		 */
		if (params->src_addr)
			ctx->ring_ctx->bo_mc = params->src_addr;
		if (params->dst_addr)
			ctx->ring_ctx->bo_mc2 = params->dst_addr;
		igt_assert_f(ctx->ring_ctx->bo_mc && ctx->ring_ctx->bo_mc2,
			     "COPY_LINEAR requires source and destination addresses\n");
		cmd_assert_size_within_packet_limit(ctx, params->size, "COPY_LINEAR");
		ctx->ring_ctx->write_length = params->size;

		result = ctx->ip_block->funcs->copy_linear(ctx->ip_block->funcs,
						   ctx->ring_ctx,
						   &ctx->ring_ctx->pm4_dw);
		return result;

	case CMD_PACKET_CONST_FILL:
		if (!ctx->ip_block->funcs->const_fill)
			return -ENOTSUP;

		/* Destination operand comes from the caller; params->data is the
		 * fill pattern written to every DWORD of the range.
		 */
		if (params->dst_addr)
			ctx->ring_ctx->bo_mc = params->dst_addr;
		igt_assert_f(ctx->ring_ctx->bo_mc,
			     "CONST_FILL requires a destination address\n");
		cmd_assert_size_within_packet_limit(ctx, params->size, "CONST_FILL");
		ctx->ring_ctx->write_length = params->size;
		ctx->ring_ctx->write_data = params->data;
		ctx->ring_ctx->write_data_valid = true;

		result = ctx->ip_block->funcs->const_fill(ctx->ip_block->funcs,
							  ctx->ring_ctx,
							  &ctx->ring_ctx->pm4_dw);
		return result;

	case CMD_PACKET_COPY_ATOMIC:
		/* TODO: Implement atomic copy if supported */
		igt_info("cmd_place_packet: ATOMIC_COPY not implemented\n");
		return -ENOTSUP;

	case CMD_PACKET_FENCE:
		/* TODO: Implement fence operation */
		igt_info("cmd_place_packet: FENCE not implemented\n");
		return -ENOTSUP;

	case CMD_PACKET_TIMESTAMP:
		/* TODO: Implement timestamp operation */
		igt_info("cmd_place_packet: TIMESTAMP not implemented\n");
		return -ENOTSUP;

	default:
		igt_info("cmd_place_packet: Unknown packet type: %d\n", params->type);
		return -EINVAL;
	}
}

/**
 * Build and submit packet in one operation
 */
int cmd_place_and_submit_packet(cmd_context_t *ctx, const cmd_packet_params_t *params)
{
	int r;

	r = cmd_place_packet(ctx, params);
	if (r)
		return r;

	return cmd_submit_packet(ctx);
}

/**
 * Convenience function for write linear
 */
int cmd_submit_write_linear(cmd_context_t *ctx, uint64_t dst_addr, uint32_t size, uint32_t data)
{
	cmd_packet_params_t params = {
		.type = CMD_PACKET_WRITE_LINEAR,
		.dst_addr = dst_addr,
		.size = size,
		.data = data,
	};

	return cmd_place_and_submit_packet(ctx, &params);
}

/**
 * Convenience function for copy linear
 */
int cmd_submit_copy_linear(cmd_context_t *ctx, uint64_t src_addr, uint64_t dst_addr, uint32_t size)
{
	cmd_packet_params_t params = {
		.type = CMD_PACKET_COPY_LINEAR,
		.src_addr = src_addr,
		.dst_addr = dst_addr,
		.size = size,
	};

	return cmd_place_and_submit_packet(ctx, &params);
}

/**
 * Convenience function for constant fill
 */
int cmd_submit_const_fill(cmd_context_t *ctx, uint64_t dst_addr, uint32_t size, uint32_t data)
{
	cmd_packet_params_t params = {
		.type = CMD_PACKET_CONST_FILL,
		.dst_addr = dst_addr,
		.size = size,
		.data = data,
	};

	return cmd_place_and_submit_packet(ctx, &params);
}

/**
 * Convenience function for atomic operation
 */
int cmd_submit_atomic(cmd_context_t *ctx, uint64_t dst_addr, uint32_t data)
{
	cmd_packet_params_t params = {
		.type = CMD_PACKET_WRITE_ATOMIC,
		.dst_addr = dst_addr,
		.data = data,
	};

	return cmd_place_and_submit_packet(ctx, &params);
}

/**
 * Wait for all submitted packets to complete (equivalent to Wait4PacketConsumption)
 */
int cmd_wait_completion(cmd_context_t *ctx)
{
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t expired;
	int r;

	if (!ctx || !ctx->initialized)
		return -EINVAL;

	if (!ctx->submit_pending)
		return 0;

	/* For user queues, wait on timeline syncobj */
	if (ctx->user_queue) {
		if (!ctx->ring_ctx->timeline_syncobj_handle)
			return -EINVAL;

		r = amdgpu_timeline_syncobj_wait(ctx->device,
						 ctx->ring_ctx->timeline_syncobj_handle,
						 ctx->ring_ctx->point);
	} else {
		/* For regular queues, wait on fence */
		fence_status.ip_type = (unsigned int)ctx->ip_type;
		fence_status.ip_instance = 0;
		fence_status.ring = ctx->ring_ctx->ring_id;
		fence_status.context = ctx->ring_ctx->context_handle;
		fence_status.fence = ctx->last_submit_seq;

		r = amdgpu_cs_query_fence_status(&fence_status,
						 AMDGPU_TIMEOUT_INFINITE,
						 0, &expired);
	}

	if (r == 0)
		ctx->submit_pending = false;

	return r;
}

/**
 * Check if ring is available
 */
bool cmd_ring_available(amdgpu_device_handle device,
			enum amd_ip_block_type ip_type,
		       uint32_t ring_id,
		       bool user_queue)
{
	struct drm_amdgpu_info_hw_ip hw_ip_info;
	int r;

	r = amdgpu_query_hw_ip_info(device, (unsigned int)ip_type, 0, &hw_ip_info);
	if (r)
		return false;

	if (user_queue) {
		/* For user queues, check user queue slots */
		uint32_t userq_slots = hw_ip_info.userq_num_slots;

		return (ring_id < userq_slots);
	} else {
		/* For regular queues, check available rings */
		return !!(hw_ip_info.available_rings & (1ULL << ring_id));
	}
}

/**
 * Get number of available rings
 */
uint32_t cmd_get_available_rings(amdgpu_device_handle device,
				 enum amd_ip_block_type ip_type,
				bool user_queue)
{
	struct drm_amdgpu_info_hw_ip hw_ip_info;
	int r;

	r = amdgpu_query_hw_ip_info(device, (unsigned int)ip_type, 0, &hw_ip_info);
	if (r)
		return 0;

	if (user_queue) {
		return hw_ip_info.userq_num_slots;
	} else {
		return __builtin_popcountll(hw_ip_info.available_rings);
	}
}

/**
 * get_sdma_max_bytes - query SDMA IP version and return max transfer size
 *
 * The SDMA COUNT field width varies by generation.
 * On v4 (Vega) and newer, the COUNT field encodes COUNT-1, i.e.,
 * COUNT=0 transfers 1 byte, so the true max is 1 << field_width.
 *
 *   v1-2 (SI):       21 bits            = 0x1fffff bytes   (~2 MB)
 *   v3   (VI):       22 bits, 32B align = 0x3fffe0 bytes   (~4 MB)
 *   v4   (Vega):     22 bits, COUNT-1   = 1<<22 bytes      (4 MB)
 *   v4.4 (MI):       30 bits, COUNT-1   = 1<<30 bytes      (1 GB)
 *   v5.0 (NV1x):     22 bits, COUNT-1   = 1<<22 bytes      (4 MB)
 *   v5.2 (NV2x):     30 bits, COUNT-1   = 1<<30 bytes      (1 GB)
 *   v6   (NV3x):     30 bits, COUNT-1   = 1<<30 bytes      (1 GB)
 *   v7   (NV4x):     30 bits, COUNT-1   = 1<<30 bytes      (1 GB)
 */
static uint64_t
get_sdma_max_bytes(amdgpu_device_handle device)
{
	struct drm_amdgpu_info_hw_ip ip = {0};
	int r;

	r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_DMA, 0, &ip);
	igt_assert_eq(r, 0);

	if (ip.hw_ip_version_major <= 2)
		return 0x1fffffULL;                     /* SI, raw count */
	else if (ip.hw_ip_version_major == 3)
		return 0x3fffe0ULL;                     /* VI, 32B aligned */
	else if (ip.hw_ip_version_major == 4 && ip.hw_ip_version_minor < 4)
		return 1ULL << 22;                      /* Vega, COUNT-1 */
	else if (ip.hw_ip_version_major == 4 && ip.hw_ip_version_minor >= 4)
		return 1ULL << 30;                      /* MI, COUNT-1 */
	else if (ip.hw_ip_version_major == 5 && ip.hw_ip_version_minor < 2)
		return 1ULL << 22;                      /* NV1x, COUNT-1 */
	else if (ip.hw_ip_version_major == 5 && ip.hw_ip_version_minor >= 2)
		return 1ULL << 30;                      /* NV2x, COUNT-1 */
	else if (ip.hw_ip_version_major >= 6)
		return 1ULL << 30;                      /* NV3x/NV4x+, COUNT-1 */

	igt_skip("Unknown SDMA IP version");
	return 0;
}

/**
 * GFX/Compute use PACKET3_DMA_DATA for fill/copy whose size field is
 * 26 bits in BYTES (max 64 MB).  PACKET3_WRITE_DATA inline count
 * is 14 bits in DWORDs (max 16384 DW = 64 KB), but that limit is
 * handled by pm4 buffer sizing, not by this struct.
 */
#define GFX_CP_DMA_MAX_BYTES   ((1ULL << 26) - 4)  /* 64 MB, 26-bit byte_count, DWORD-aligned */

/**
 * get_gfx_cp_dma_max_bytes - return max CP DMA (PACKET3_DMA_DATA) transfer size
 *
 * The register spec says 26 bits (64 MB) for all generations, but pre-GFX9
 * hardware (Polaris / GFX8 and older) silently truncates at 20 bits (1 MB).
 * Observed: const_fill on Polaris writes only 2 MB, rest stays zero.
 */
static uint64_t
get_gfx_cp_dma_max_bytes(amdgpu_device_handle device)
{
	struct drm_amdgpu_info_hw_ip ip = {};
	int r;

	r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &ip);
	igt_assert_eq(r, 0);

	/* GFX9 (Vega) and newer: full 26-bit byte_count */
	if (ip.hw_ip_version_major >= 9)
		return GFX_CP_DMA_MAX_BYTES;

	/* GFX8 (Polaris) and older: effective limit is 20 bits */
	return 1ULL << 20;                         /* 1 MB */
}

void
amdgpu_dma_limits_query(amdgpu_device_handle device,
			    struct amdgpu_dma_limits *limits)
{
	memset(limits, 0, sizeof(*limits));

	limits->sdma_max_bytes    = get_sdma_max_bytes(device);
	limits->gfx_max_bytes     = get_gfx_cp_dma_max_bytes(device);
	limits->compute_max_bytes = get_gfx_cp_dma_max_bytes(device);

	/*
	 * Default safe small sizes for quick smoke tests.
	 * write_linear uses inline data so keep it small (512 bytes = 128 DW).
	 * const_fill and copy_linear have no inline data, can be larger.
	 */
	limits->sdma_default_bytes    = 512;    /* 128 DWORDs */
	limits->gfx_default_bytes     = 1024;   /* 256 DWORDs */
	limits->compute_default_bytes = 1024;   /* 256 DWORDs */
}
