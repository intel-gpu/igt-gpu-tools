// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <poll.h>
#include <unistd.h>

#include "igt.h"
#include "igt_kmod.h"
#include "igt_vgem.h"
#include "drmtest.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_memory.h"

/**
 * TEST: amd dmabuf unload
 * Description: Test fence independence during amdgpu module unload with
 *              outstanding SDMA fences on cross-driver (VGEM) DMA-BUF
 *              shared reservations.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: prime
 * Test category: GEM_Legacy
 * Feature: prime, vgem
 *
 * SUBTEST: fence-outlives-module
 * Description: Validates fence independence patches.  Submits 64 max-size
 *              SDMA copy_linear operations on a bo_list that includes an
 *              imported VGEM DMA-BUF, attaching SDMA fences to the VGEM
 *              BO dma_resv.  Closes all amdgpu fds WITHOUT waiting for
 *              completion, then rmmods amdgpu.  After unload, poll()s the
 *              DMA-BUF fd to iterate dma_resv fences whose ops/spinlock/
 *              names pointed into the now-unloaded module.  Without the
 *              fence independence patches this is a use-after-free; with
 *              them the fences are self-contained.
 *              Best run with KASAN enabled to catch the UAF.
 */

IGT_TEST_DESCRIPTION("Test amdgpu module unload safety with imported DMA-BUF "
		      "references from VGEM driver.");

/**
 * dmabuf_busy - check whether a DMA-BUF fd has unsignaled fences
 * @fd: DMA-BUF file descriptor
 * @write: true to check for exclusive (write) fences, false for shared (read)
 *
 * Returns true if poll() indicates the fd is not yet ready (busy).
 */
static bool dmabuf_busy(int fd, bool write)
{
	struct pollfd pfd = { .fd = fd, .events = write ? POLLOUT : POLLIN };

	return poll(&pfd, 1, 0) == 0;
}

/* Number of back-to-back SDMA copies to submit without waiting. */
#define NUM_SDMA_COPIES 64

/**
 * test_fence_outlives_module - fence independence validation
 * @vgem: VGEM device fd
 * @amd_fd: amdgpu device fd (will be closed inside this function)
 * @dev: amdgpu device handle
 *
 * Create amdgpu SDMA fences on a cross-driver shared buffer, then close
 * every amdgpu fd so the module can actually be unloaded.  After rmmod,
 * the fences survive in the VGEM BO dma_resv.  Accessing them (via poll)
 * exercises fence->ops, fence->lock, and the driver/timeline name strings
 * -- all of which used to live in the module .text/.data.
 *
 * Without the patches: use-after-free (KASAN splat or crash).
 * With the patches: fences are self-contained and safe.
 */
static void test_fence_outlives_module(int vgem, int amd_fd,
				       amdgpu_device_handle dev)
{
	struct amdgpu_bo_import_result import;
	struct amdgpu_dma_limits limits = {0};
	amdgpu_bo_handle src_bo, dst_bo;
	amdgpu_va_handle src_va, dst_va, imp_va;
	void *src_cpu, *dst_cpu;
	uint64_t src_mc, dst_mc, imp_mc, copy_size;
	cmd_context_t *cmd_ctx;
	struct vgem_bo bo;
	int dmabuf, r, i;

	/* Create VGEM BO and export as DMA-BUF */
	bo.width = 1024;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(vgem, &bo);
	dmabuf = prime_handle_to_fd(vgem, bo.handle);

	/* Import the VGEM DMA-BUF into amdgpu */
	r = amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd,
			     dmabuf, &import);
	igt_assert_eq(r, 0);

	/* Query max SDMA copy size from HW limits */
	amdgpu_dma_limits_query(dev, &limits);
	copy_size = limits.sdma_max_bytes;
	igt_assert(copy_size > 0);

	/* Allocate source BO and fill with pattern */
	r = amdgpu_bo_alloc_and_map(dev, copy_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &src_bo, &src_cpu, &src_mc, &src_va);
	igt_assert_eq(r, 0);
	memset(src_cpu, 0xDE, copy_size);

	/* Allocate destination BO */
	r = amdgpu_bo_alloc_and_map(dev, copy_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &dst_bo, &dst_cpu, &dst_mc, &dst_va);
	igt_assert_eq(r, 0);

	/* Create SDMA command context */
	cmd_ctx = cmd_context_create(dev, AMD_IP_DMA, 0, false);
	igt_assert(cmd_ctx != NULL);

	/*
	 * Map the imported VGEM BO into GPU VA space so the kernel can
	 * validate it during amdgpu_cs_vm_handling().  Without a VA
	 * mapping the BO in the bo_list causes a GPU page fault.
	 */
	r = amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general,
				  4096, 4096, 0, &imp_mc, &imp_va, 0);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_va_op(import.buf_handle, 0, 4096, imp_mc, 0,
			     AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	igt_info("Submitting %d x %llu-byte SDMA copies (no wait)...\n",
		 NUM_SDMA_COPIES, (unsigned long long)copy_size);

	for (i = 0; i < NUM_SDMA_COPIES; i++) {
		igt_assert_eq(cmd_context_add_resource(cmd_ctx, src_bo), 0);
		igt_assert_eq(cmd_context_add_resource(cmd_ctx, dst_bo), 0);
		igt_assert_eq(cmd_context_add_resource(cmd_ctx, import.buf_handle), 0);

		r = cmd_submit_copy_linear(cmd_ctx,
					   src_mc, dst_mc,
					   (uint32_t)copy_size);
		igt_assert_eq(r, 0);
	}

	igt_info("All %d copies submitted -- closing immediately "
		 "(no wait)...\n", NUM_SDMA_COPIES);

	/*
	 * Do NOT wait -- close everything immediately.
	 * SDMA fences stay in the VGEM BO dma_resv.
	 */
	drm_close_driver(amd_fd);

	/*
	 * At this point:
	 *   - Zero open amdgpu file descriptors
	 *   - The VGEM BO dma_resv still holds references to amdgpu
	 *     SDMA fences (likely still unsignaled -- 64 x max-SDMA
	 *     copies take seconds)
	 *   - We hold the DMA-BUF fd and VGEM BO handle
	 *
	 * Unload the module.  With no open amdgpu fds this should
	 * succeed.
	 */
	igt_info("All amdgpu fds closed, attempting module unload...\n");
	r = igt_kmod_unload("amdgpu");
	igt_info("igt_kmod_unload returned %d\n", r);

	if (r == 0) {
		bool busy_r, busy_w;

		/*
		 * Module unloaded.  amdgpu .text/.data is unmapped.
		 * Exercise dma_resv fences via poll() -- this calls
		 * fence->ops->signaled and touches fence->lock, which
		 * used to point into module memory.
		 */
		igt_info("Module unloaded -- exercising fences via "
			 "poll()...\n");

		busy_r = dmabuf_busy(dmabuf, false);
		igt_info("DMA-BUF read-busy:  %s\n",
			 busy_r ? "yes" : "no");

		busy_w = dmabuf_busy(dmabuf, true);
		igt_info("DMA-BUF write-busy: %s\n",
			 busy_w ? "yes" : "no");

		igt_info("Fences survived module unload -- reloading...\n");
		r = igt_kmod_load("amdgpu", NULL);
		igt_info("Module reload returned %d\n", r);
	} else {
		igt_info("Module unload refused (r=%d) -- fences may hold "
			 "module refs (expected with patches)\n", r);
	}

	close(dmabuf);
	gem_close(vgem, bo.handle);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int vgem = -1, amd = -1;
	uint32_t major, minor;
	int err, r;

	igt_fixture() {
		uint64_t cap;

		vgem = drm_open_driver(DRIVER_VGEM);
		igt_require(vgem >= 0);

		amd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(amd >= 0);

		err = amdgpu_device_initialize(amd, &major, &minor, &device);
		igt_require(err == 0);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);

		/* Verify PRIME import/export support */
		igt_require(drmGetCap(vgem, DRM_CAP_PRIME, &cap) == 0 &&
			    (cap & DRM_PRIME_CAP_EXPORT));
		igt_require(drmGetCap(amd, DRM_CAP_PRIME, &cap) == 0 &&
			    (cap & DRM_PRIME_CAP_IMPORT));
	}

	/*
	 * This subtest closes the fixture amdgpu fd so that rmmod can
	 * succeed, then re-opens it for fixture teardown.
	 */
	igt_subtest("fence-outlives-module") {
		test_fence_outlives_module(vgem, amd, device);

		/* Re-open for fixture teardown */
		amd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(amd, &major, &minor, &device);
		igt_assert_eq(err, 0);
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(amd);
		drm_close_driver(vgem);
	}
}
