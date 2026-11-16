// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

#include <errno.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define PVR_JOB_STREAM_SIZE 256
#define PVR_SYNCOBJ_WAIT_TIMEOUT_NS NSEC_PER_SEC

#define PVR_BO_SIZE 0x2000
#define PVR_NUM_FREE_LISTS 2
#define PVR_INVALID_HANDLE 999
#define PVR_STREAM_TERMINATE_BLOCK_TYPE 0xc0000000
#define PVR_CDM_STREAM_TERMINATE_BLOCK_TYPE 0x80000000
#define PVR_UNUSED_JOB_FLAG (1u << 15)

#define ROGUE_PDSINST_OPCODEC_SP            (0x0000000dU)
#define ROGUE_PDSINST_HALT_OPCODE_SHIFT     (28U)
#define ROGUE_PDSINST_OPCODESP_HALT         (0x00000006U)
#define ROGUE_PDSINST_HALT_OP_SHIFT         (23U)

#define TRUNCATE_64BITS_TO_32BITS(expr) ((uint32_t)(expr))
#define ROGUE_VDMCTRL_BLOCK_HEADER_BLOCK_TYPE_PPP_STATE_UPDATE                        (0x00000000U)
#define ROGUE_VDMCTRL_PPP_STATE0_WORD_COUNT_SHIFT                                     (8U)
#define ROGUE_VDMCTRL_PPP_STATE0_ADDRMSB_ALIGNSHIFT                                   (32U)
#define ROGUE_VDMCTRL_PPP_STATE0_ADDRMSB_SHIFT                                        (0U)
#define ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_ALIGNSHIFT                                   (2U)
#define ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_SHIFT                                        (2U)
#define ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_CLRMSK                                       (0x00000003U)

#define ROGUE_TA_STATE_HEADER_PRES_TERMINATE_EN                                       (0x02000000U)

#define ROGUE_TA_STATE_TERMINATE1_CLIP_LEFT_SHIFT           (23U)
#define ROGUE_TA_STATE_TERMINATE0_CLIP_RIGHT_SHIFT          (18U)
#define ROGUE_TA_STATE_TERMINATE0_CLIP_TOP_SHIFT            (9U)
#define ROGUE_TA_STATE_TERMINATE0_CLIP_BOTTOM_SHIFT         (0U)

static uint32_t init_compute_cmd_stream(int fd, uint32_t vm_ctx,
					uint8_t *stream,
					struct pvr_device_info *device_info)
{
	uint8_t *stream_start = stream;
	uint64_t cdm_stream_gpu_addr;
	void *cdm_stream_map;
	uint32_t stream_length;

	struct igt_pvr_allocation *cdm_stream_alloc =
		igt_pvr_allocate_general(fd, vm_ctx, 0x1000,
				         DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
				         DRM_PVR_BO_BYPASS_DEVICE_CACHE);

	cdm_stream_gpu_addr = igt_pvr_get_gpu_addr(cdm_stream_alloc);

	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // length
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // padding

	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // tpu_border_colour_table
	STREAM_ADD_ITEM(true, uint64_t, cdm_stream_gpu_addr, stream); // cdm_ctrl_stream_base
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdm_context_state_base_addr
	STREAM_ADD_ITEM(device_info->quirks.has_brn49927, uint32_t, 0, stream); // tpu
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // cdm_resume_pds1
	STREAM_ADD_ITEM(device_info->features.has_compute_morton_capable,
			uint32_t, 0, stream); // cdm_item
	STREAM_ADD_ITEM(device_info->features.has_cluster_grouping,
			uint32_t, 0, stream); // compute_cluster
	STREAM_ADD_ITEM(device_info->features.has_tpu_dm_global_registers,
			uint32_t, 0, stream); // tpu_tag_cdm_ctrl
	STREAM_ADD_ITEM(device_info->features.has_gpu_multicore_support,
			uint32_t, 0, stream); // execute_count
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // padding

	stream_length = stream - stream_start;

	STREAM_ADD_ITEM(true, uint32_t, stream_length, stream_start); // update length

	cdm_stream_map = igt_pvr_get_cpu_addr(fd, cdm_stream_alloc);

	((uint32_t *)cdm_stream_map)[0] =
			PVR_CDM_STREAM_TERMINATE_BLOCK_TYPE;
	return stream_length;
}

static int64_t syncobj_wait_timeout(void)
{
	struct timespec now;

	igt_assert_eq(clock_gettime(CLOCK_MONOTONIC, &now), 0);

	return (int64_t)now.tv_sec * NSEC_PER_SEC + now.tv_nsec +
	       PVR_SYNCOBJ_WAIT_TIMEOUT_NS;
}

static uint32_t init_geometry_cmd_stream(uint8_t *stream, struct pvr_device_info *device_info,
					 uint64_t vdm_gpu_addr)
{
	uint8_t *stream_start = stream;
	uint32_t stream_length;

	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // length
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // padding

	STREAM_ADD_ITEM(true, uint64_t, vdm_gpu_addr, stream); // vdm_ctrl_stream_base
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // tpu_border_colour_table
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // ppp_ctrl
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // te_psg
	STREAM_ADD_ITEM(device_info->quirks.has_brn49927, uint32_t, 0, stream); // tpu
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // vdm_context_resume_task0_size
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // view_idx
	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // padding

	stream_length = stream - stream_start;

	STREAM_ADD_ITEM(true, uint32_t, stream_length, stream_start); // update length
	return stream_length;
}

static uint32_t pds_inst_encode_halt(void)
{
	uint32_t encoded = 0;

	encoded |=
		(uint32_t)((uint32_t)ROGUE_PDSINST_OPCODEC_SP << ROGUE_PDSINST_HALT_OPCODE_SHIFT);
	encoded |=
		(uint32_t)((uint32_t)ROGUE_PDSINST_OPCODESP_HALT << ROGUE_PDSINST_HALT_OP_SHIFT);
	return encoded;
}

int igt_main()
{
	const uint32_t num_free_lists = PVR_NUM_FREE_LISTS;
	uint32_t free_list_handles[num_free_lists];
	uint8_t compute_cmd_stream[PVR_JOB_STREAM_SIZE]	__attribute__((aligned(8)));
	uint8_t geometry_cmd_stream[PVR_JOB_STREAM_SIZE] __attribute__((aligned(8)));

	uint32_t compute_vm_ctx_handle;
	uint32_t render_vm_ctx_handle;
	uint32_t transfer_vm_ctx_handle;

	uint32_t compute_stream_length;
	uint32_t geometry_stream_length;

	uint32_t compute_ctx_handle;
	uint32_t render_ctx_handle;
	uint32_t transfer_ctx_handle;

	size_t size = PVR_BO_SIZE;
	uint32_t hwrt_handle;
	int fd;

	struct pvr_device_info *device_info;

	struct igt_pvr_allocation *free_lists_obj_alloc;
	struct igt_pvr_allocation *ppp_alloc;
	struct igt_pvr_allocation *vdm_alloc;
	struct igt_pvr_allocation *compute_pds_alloc;

	uint32_t *ppp_buffer_base;
	uint32_t *vdm_buffer;
	uint64_t *pds_buffer_base;

	igt_fixture()
	{
		fd = drm_open_driver(DRIVER_POWERVR);

		igt_pvr_init_allocators(fd);

		device_info = igt_pvr_get_device_info(fd);

		compute_vm_ctx_handle = igt_pvr_ioctl_create_vm_context(fd, 0);
		igt_assert(compute_vm_ctx_handle);

		render_vm_ctx_handle = igt_pvr_ioctl_create_vm_context(fd, 0);
		igt_assert(render_vm_ctx_handle);

		transfer_vm_ctx_handle = igt_pvr_ioctl_create_vm_context(fd, 0);
		igt_assert(transfer_vm_ctx_handle);

		compute_ctx_handle =
			igt_pvr_ioctl_create_context(fd,
						     DRM_PVR_CTX_TYPE_COMPUTE,
						     compute_vm_ctx_handle);
		igt_assert(compute_ctx_handle);

		transfer_ctx_handle =
			igt_pvr_ioctl_create_context(fd,
						     DRM_PVR_CTX_TYPE_TRANSFER_FRAG,
						     transfer_vm_ctx_handle);
		igt_assert(transfer_ctx_handle);

		render_ctx_handle =
			igt_pvr_ioctl_create_context(fd,
						     DRM_PVR_CTX_TYPE_RENDER,
						     render_vm_ctx_handle);
		igt_assert(render_ctx_handle);

		compute_stream_length =
			init_compute_cmd_stream(fd, compute_vm_ctx_handle,
						(uint8_t *)&compute_cmd_stream, device_info);

		/* PDS for compute */
		compute_pds_alloc =
			igt_pvr_allocate(fd, compute_vm_ctx_handle, 0x4000,
					 DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS,
					 DRM_PVR_HEAP_PDS_CODE_DATA);
		pds_buffer_base = igt_pvr_get_cpu_addr(fd, compute_pds_alloc);

		pds_buffer_base[0x20] = pds_inst_encode_halt();

		vdm_alloc =
			igt_pvr_allocate_general(fd, render_vm_ctx_handle, size,
						 DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
						 DRM_PVR_BO_BYPASS_DEVICE_CACHE);

		vdm_buffer = igt_pvr_get_cpu_addr(fd, vdm_alloc);

		size = PVR_BO_SIZE;

		ppp_alloc =
			igt_pvr_allocate_general(fd, render_vm_ctx_handle, size,
						 DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
						 DRM_PVR_BO_BYPASS_DEVICE_CACHE);

		ppp_buffer_base = igt_pvr_get_cpu_addr(fd, ppp_alloc);

		/* PPP state for terminate */
		ppp_buffer_base[0] = ROGUE_TA_STATE_HEADER_PRES_TERMINATE_EN;
		ppp_buffer_base[1] = (256 << ROGUE_TA_STATE_TERMINATE0_CLIP_RIGHT_SHIFT) |
					(0 << ROGUE_TA_STATE_TERMINATE0_CLIP_TOP_SHIFT) |
					(256 << ROGUE_TA_STATE_TERMINATE0_CLIP_BOTTOM_SHIFT);
		ppp_buffer_base[2] = (0 << ROGUE_TA_STATE_TERMINATE1_CLIP_LEFT_SHIFT);

		*vdm_buffer++ =
			ROGUE_VDMCTRL_BLOCK_HEADER_BLOCK_TYPE_PPP_STATE_UPDATE |
			(3 << ROGUE_VDMCTRL_PPP_STATE0_WORD_COUNT_SHIFT) |
			((igt_pvr_get_gpu_addr(ppp_alloc)
			>> ROGUE_VDMCTRL_PPP_STATE0_ADDRMSB_ALIGNSHIFT)
			<< ROGUE_VDMCTRL_PPP_STATE0_ADDRMSB_SHIFT);
		*vdm_buffer++ =
			TRUNCATE_64BITS_TO_32BITS(((igt_pvr_get_gpu_addr(ppp_alloc)
						  >> ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_ALIGNSHIFT)
						  << ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_SHIFT) &
						  ~ROGUE_VDMCTRL_PPP_STATE1_ADDRLSB_CLRMSK);

		*vdm_buffer++ =	PVR_STREAM_TERMINATE_BLOCK_TYPE;
		geometry_stream_length =
			init_geometry_cmd_stream((uint8_t *)&geometry_cmd_stream,
						 device_info,
						 igt_pvr_get_gpu_addr(vdm_alloc));

		/* Also need free lists and hwrt data for render jobs. */
		free_lists_obj_alloc =
			igt_pvr_allocate(fd, render_vm_ctx_handle,
					 size * 4, DRM_PVR_BO_PM_FW_PROTECT,
					 DRM_PVR_HEAP_GENERAL);

		for (int i = 0; i < num_free_lists; i++) {
			uint64_t free_list_gpu_addr = igt_pvr_get_gpu_addr(free_lists_obj_alloc);

			free_list_handles[i] =
				igt_pvr_ioctl_create_free_list(fd,
							       render_vm_ctx_handle,
							       free_list_gpu_addr + i * size);
		}

		hwrt_handle =
			igt_pvr_ioctl_create_hwrt_dataset(fd, render_vm_ctx_handle,
							  free_list_handles,
							  num_free_lists);
		igt_assert(hwrt_handle);
	}

	/* Invalid job and context combinations. */
	igt_subtest_group() {
		igt_subtest("submit-jobs-fail-zero-length")
		{
			struct drm_pvr_ioctl_submit_jobs_args args = {0};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-compute-no-cmd-stream")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = 0,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);

			job.cmd_stream = 0;
			job.cmd_stream_len = sizeof(compute_cmd_stream);
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-compute-with-hwrt")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.hwrt.set_handle = 1,
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);

			job.hwrt.set_handle = 0;
			job.hwrt.data_index = 1;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-compute-bad-context")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = PVR_INVALID_HANDLE,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);

			job.context_handle = 0;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-bad-flags")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = PVR_UNUSED_JOB_FLAG,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			/* Check the invalid flag is still unused! */
			igt_assert(job.flags & ~DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK);
			igt_assert(job.flags & ~DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK);
			igt_assert(job.flags & ~DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK);
			igt_assert(job.flags & ~DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK);

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);

			job.context_handle = transfer_ctx_handle;
			job.type = DRM_PVR_JOB_TYPE_TRANSFER_FRAG;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);

			job.hwrt.set_handle = hwrt_handle;
			job.context_handle = render_ctx_handle;
			job.type = DRM_PVR_JOB_TYPE_GEOMETRY;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
			job.type = DRM_PVR_JOB_TYPE_FRAGMENT;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-compute-wrong-context")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = transfer_ctx_handle,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
			job.context_handle = render_ctx_handle;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-transfer-wrong-context")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_TRANSFER_FRAG,
				.context_handle = compute_ctx_handle,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
			job.context_handle = render_ctx_handle;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-compute-too-big")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = UINT32_MAX,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, ENOMEM);
		}
	}

	/* Compute jobs with sync operations. */
	igt_subtest_group() {
		igt_subtest("submit-jobs-compute-single-wait")
		{
			struct drm_pvr_sync_op wait = {
				.flags = DRM_PVR_SYNC_OP_FLAG_WAIT |
					DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
			};
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_OBJ_ARRAY(1, &wait),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			drmSyncobjCreate(fd, 0, &wait.handle);
			igt_assert_neq(wait.handle, 0);

			/* Clear the wait! */
			drmSyncobjSignal(fd, &wait.handle, 1);

			do_ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args);
		}

		igt_subtest("submit-jobs-fail-compute-no-syncop-handle")
		{
			struct drm_pvr_sync_op wait = {
				.flags = DRM_PVR_SYNC_OP_FLAG_WAIT |
					DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
			};
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_OBJ_ARRAY(1, &wait),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, ENOENT);
		}

		igt_subtest("submit-jobs-fail-compute-bad-syncop-handle")
		{
			struct drm_pvr_sync_op wait = {
				.flags = DRM_PVR_SYNC_OP_FLAG_WAIT |
					DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
				.handle = PVR_INVALID_HANDLE,
			};
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_OBJ_ARRAY(1, &wait),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, ENOENT);
		}

		igt_subtest("submit-jobs-compute-single")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_EMPTY_OBJ_ARRAY(struct drm_pvr_sync_op),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args);
		}

		igt_subtest("submit-jobs-compute-single-signal")
		{
			struct drm_pvr_sync_op signal = {
				.flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL |
					DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
			};
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_OBJ_ARRAY(1, &signal),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			drmSyncobjCreate(fd, 0, &signal.handle);
			igt_assert_neq(signal.handle, 0);

			do_ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args);
			igt_assert_eq(drmSyncobjWait(fd, &signal.handle, 1, syncobj_wait_timeout(),
						     0, NULL), 0);
		}

		igt_subtest("submit-jobs-compute-single-wait-and-signal")
		{
			struct drm_pvr_sync_op syncs[] = {
				{
					.flags = DRM_PVR_SYNC_OP_FLAG_WAIT |
						DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
				},
				{
					.flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL |
						DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,
				},
			};
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_COMPUTE,
				.context_handle = compute_ctx_handle,
				.flags = 0,
				.cmd_stream_len = compute_stream_length,
				.cmd_stream = to_user_pointer(&compute_cmd_stream),
				.sync_ops = DRM_PVR_OBJ_ARRAY(ARRAY_SIZE(syncs), syncs),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			drmSyncobjCreate(fd, 0, &syncs[0].handle);
			igt_assert_neq(syncs[0].handle, 0);
			drmSyncobjCreate(fd, 0, &syncs[1].handle);
			igt_assert_neq(syncs[1].handle, 0);

			/* Clear the wait! */
			drmSyncobjSignal(fd, &syncs[0].handle, 1);

			do_ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args);
			igt_assert_eq(drmSyncobjWait(fd, &syncs[1].handle,
						     1, syncobj_wait_timeout(),
						     0, NULL), 0);
		}
	}

	/* Graphics-specific jobs and contexts. */
	igt_subtest_group() {
		igt_subtest("submit-jobs-fail-render-without-hwrt")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_GEOMETRY,
				.context_handle = render_ctx_handle,
				.flags = DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST |
						DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST,
				.cmd_stream_len = geometry_stream_length,
				.cmd_stream = to_user_pointer(&geometry_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-geometry-wrong-context")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_GEOMETRY,
				.context_handle = compute_ctx_handle,
				.hwrt.set_handle = hwrt_handle,
				.cmd_stream_len = geometry_stream_length,
				.cmd_stream = to_user_pointer(&geometry_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
			job.context_handle = transfer_ctx_handle;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}

		igt_subtest("submit-jobs-fail-fragment-wrong-context")
		{
			struct drm_pvr_job job = {
				.type = DRM_PVR_JOB_TYPE_FRAGMENT,
				.context_handle = compute_ctx_handle,
				.hwrt.set_handle = hwrt_handle,
				.cmd_stream_len = geometry_stream_length,
				.cmd_stream = to_user_pointer(&geometry_cmd_stream),
			};
			struct drm_pvr_ioctl_submit_jobs_args args = {
				.jobs = DRM_PVR_OBJ_ARRAY(1, &job),
			};

			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
			job.context_handle = transfer_ctx_handle;
			do_ioctl_err(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &args, EINVAL);
		}
	}

	igt_fixture()
	{
		igt_pvr_ioctl_destroy_hwrt_dataset(fd, hwrt_handle);

		for (int i = 0; i < num_free_lists; i++)
			igt_pvr_ioctl_destroy_free_list(fd, free_list_handles[i]);

		igt_pvr_free_all(fd);

		igt_pvr_ioctl_destroy_context(fd, compute_ctx_handle);
		igt_pvr_ioctl_destroy_context(fd, render_ctx_handle);
		igt_pvr_ioctl_destroy_context(fd, transfer_ctx_handle);

		igt_pvr_ioctl_destroy_vm_context(fd, compute_vm_ctx_handle, 0);
		igt_pvr_ioctl_destroy_vm_context(fd, render_vm_ctx_handle, 0);
		igt_pvr_ioctl_destroy_vm_context(fd, transfer_vm_ctx_handle, 0);

		drm_close_driver(fd);
	}
}
