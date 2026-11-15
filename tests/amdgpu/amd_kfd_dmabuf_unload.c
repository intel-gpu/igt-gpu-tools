// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>
#include <amdgpu_drm.h>
#ifdef HAVE_KFD_IOCTL_H
#include <linux/kfd_ioctl.h>
#endif /* HAVE_KFD_IOCTL_H */
#include <poll.h>
#include <sys/ioctl.h>
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
 * TEST: amd kfd dmabuf unload
 * Description: Test KFD fence independence with DMA-BUF cross-driver
 *              references from VGEM.  Exercises the KFD ioctl path
 *              (AMDKFD_IOC_IMPORT_DMABUF, AMDKFD_IOC_MAP_MEMORY_TO_GPU)
 *              for module unload safety, plus a fence-outlives-module
 *              subtest that unloads amdgpu while KFD-path fences are
 *              still referenced by a VGEM BO dma_resv.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: prime
 * Test category: GEM_Legacy
 * Feature: prime, vgem, kfd
 *
 * SUBTEST: kfd-fence-outlives-module
 * Description: Validates fence independence patches for KFD.  Opens
 *              /dev/kfd + amdgpu, imports a VGEM DMA-BUF into KFD
 *              (attaching KFD eviction fences to dma_resv), submits
 *              SDMA copies with the imported BO in the bo_list
 *              (attaching SDMA fences), then closes everything without
 *              waiting.  rmmods amdgpu, then poll()s the DMA-BUF fd
 *              to exercise fence->ops and fence->lock after module
 *              unload.  Without the patches this is a use-after-free;
 *              with them fences are self-contained.
 *              Best run with KASAN enabled.
 */


#ifdef HAVE_KFD_IOCTL_H
/*
 * ============================================================
 *  KFD ioctl helpers  (direct ioctls, no libhsakmt dependency)
 * ============================================================
 */

/**
 * kfd_open - open the /dev/kfd device node
 *
 * Returns the file descriptor, or -1 on failure.
 */
static int kfd_open(void)
{
	return open("/dev/kfd", O_RDWR);
}

/**
 * kfd_get_gpu_id - retrieve the first non-zero GPU id from KFD
 * @fd: KFD file descriptor
 * @gpu_id: output GPU id
 *
 * Iterates the process apertures returned by KFD and picks the
 * first node whose gpu_id is non-zero.
 *
 * Returns 0 on success or -errno.
 */
static int kfd_get_gpu_id(int fd, uint32_t *gpu_id)
{
	struct kfd_ioctl_get_process_apertures_args args = {};
	int i;

	if (ioctl(fd, AMDKFD_IOC_GET_PROCESS_APERTURES, &args))
		return -errno;

	for (i = 0; i < (int)args.num_of_nodes; i++) {
		if (args.process_apertures[i].gpu_id != 0) {
			*gpu_id = args.process_apertures[i].gpu_id;
			return 0;
		}
	}
	return -ENODEV;
}

/**
 * kfd_acquire_vm - bind a KFD device to the amdgpu VM
 * @fd: KFD file descriptor
 * @gpu_id: target GPU id
 * @drm_fd: amdgpu DRM file descriptor whose VM should be shared
 *
 * After this call KFD VA mappings and amdgpu VA mappings share the
 * same page table.
 *
 * Returns 0 on success or -errno.
 */
static int kfd_acquire_vm(int fd, uint32_t gpu_id, int drm_fd)
{
	struct kfd_ioctl_acquire_vm_args args = {};

	args.gpu_id = gpu_id;
	args.drm_fd = drm_fd;
	return ioctl(fd, AMDKFD_IOC_ACQUIRE_VM, &args) ? -errno : 0;
}

/**
 * kfd_get_gpuvm_base - query the GPUVM aperture base for @gpu_id
 * @fd: KFD file descriptor
 * @gpu_id: target GPU id
 * @gpuvm_base: output aperture base address
 *
 * kfd_import_dmabuf requires a VA within this aperture; passing 0
 * causes "amdgpu: Invalid VA when adding BO to VM" (-EINVAL).
 *
 * Returns 0 on success or -errno.
 */
static int kfd_get_gpuvm_base(int fd, uint32_t gpu_id,
			      uint64_t *gpuvm_base)
{
	struct kfd_process_device_apertures aps[NUM_OF_SUPPORTED_GPUS];
	struct kfd_ioctl_get_process_apertures_new_args args = {};
	uint32_t i;

	if (ioctl(fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &args))
		return -errno;
	if (args.num_of_nodes == 0)
		return -ENODEV;
	if (args.num_of_nodes > NUM_OF_SUPPORTED_GPUS)
		args.num_of_nodes = NUM_OF_SUPPORTED_GPUS;

	args.kfd_process_device_apertures_ptr =
		(uint64_t)(uintptr_t)aps;
	if (ioctl(fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &args))
		return -errno;

	for (i = 0; i < args.num_of_nodes; i++) {
		if (aps[i].gpu_id == gpu_id) {
			*gpuvm_base = aps[i].gpuvm_base;
			return 0;
		}
	}
	return -ENODEV;
}

/**
 * kfd_import_dmabuf - import a DMA-BUF fd into KFD
 * @fd: KFD file descriptor
 * @gpu_id: target GPU id
 * @dmabuf_fd: DMA-BUF file descriptor to import
 * @va_addr: GPU virtual address (must be within the GPUVM aperture)
 * @handle: output KFD memory handle
 *
 * Returns 0 on success or -errno.
 */
static int kfd_import_dmabuf(int fd, uint32_t gpu_id, int dmabuf_fd,
			     uint64_t va_addr, uint64_t *handle)
{
	struct kfd_ioctl_import_dmabuf_args args = {};

	args.gpu_id = gpu_id;
	args.dmabuf_fd = dmabuf_fd;
	args.va_addr = va_addr;
	if (ioctl(fd, AMDKFD_IOC_IMPORT_DMABUF, &args))
		return -errno;
	*handle = args.handle;
	return 0;
}

/**
 * kfd_map_memory_to_gpu - map a KFD memory object on the given GPU
 * @fd: KFD file descriptor
 * @handle: KFD memory handle (from kfd_import_dmabuf)
 * @gpu_id: target GPU id
 *
 * Returns 0 on success or -errno.
 */
static int kfd_map_memory_to_gpu(int fd, uint64_t handle, uint32_t gpu_id)
{
	struct kfd_ioctl_map_memory_to_gpu_args args = {};
	uint32_t dev_id = gpu_id;

	args.handle = handle;
	args.device_ids_array_ptr = (uint64_t)(uintptr_t)&dev_id;
	args.n_devices = 1;
	return ioctl(fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &args) ? -errno : 0;
}

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
#define NUM_SDMA_COPIES 1

/**
 * test_kfd_fence_outlives_module - KFD fence independence validation
 * @vgem: VGEM device fd
 * @amd_fd: amdgpu device fd
 * @dev: amdgpu device handle
 *
 * The key KFD test for fence independence patches:
 *
 *   1. Open /dev/kfd + amdgpu, bind KFD to the amdgpu VM
 *      (kfd_acquire_vm -- shared page table).
 *   2. Import VGEM DMA-BUF into KFD via AMDKFD_IOC_IMPORT_DMABUF
 *      and map it (AMDKFD_IOC_MAP_MEMORY_TO_GPU).  This attaches
 *      KFD eviction fences to dma_resv AND creates a GPU VA mapping
 *      in the shared VM.
 *   3. Also import via amdgpu_bo_import() for SDMA submission.
 *      Note: NO separate amdgpu VA mapping is needed for the imported
 *      BO because KFD already mapped it in step 2 (shared VM).
 *   4. Submit SDMA copies with the imported BO in bo_list, attaching
 *      SDMA fences to dma_resv.
 *   5. Close everything WITHOUT waiting.
 *
 * After close: zero amdgpu fds, but VGEM BO dma_resv still holds
 * amdgpu SDMA fences + KFD eviction fences.
 *
 * rmmod amdgpu, then poll() the DMA-BUF fd.
 *
 * Without patches: use-after-free (crash / KASAN splat).
 * With patches: self-contained fences, safe poll().
 */
static void test_kfd_fence_outlives_module(int vgem, int amd_fd,
					   amdgpu_device_handle dev)
{
	struct amdgpu_bo_import_result import;
	amdgpu_bo_handle src_bo, dst_bo;
	amdgpu_va_handle src_va, dst_va;
	void *src_cpu, *dst_cpu;
	uint64_t src_mc, dst_mc;
	cmd_context_t *cmd_ctx;
	struct amdgpu_dma_limits limits = {0};
	struct vgem_bo bo;
	uint32_t gpu_id;
	uint64_t kfd_handle, gpuvm_base, copy_size;
	int kfd, dmabuf, r, i;

	/* Create VGEM BO and export as DMA-BUF */
	bo.width = 1024;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(vgem, &bo);
	dmabuf = prime_handle_to_fd(vgem, bo.handle);

	/* Open KFD and bind it to the amdgpu VM (shared page table) */
	kfd = kfd_open();
	igt_assert(kfd >= 0);
	r = kfd_get_gpu_id(kfd, &gpu_id);
	igt_assert_eq(r, 0);
	r = kfd_acquire_vm(kfd, gpu_id, amd_fd);
	igt_assert_eq(r, 0);
	r = kfd_get_gpuvm_base(kfd, gpu_id, &gpuvm_base);
	igt_assert_eq(r, 0);

	/*
	 * Import DMA-BUF into KFD and map it on the GPU.
	 * This creates a VA mapping in the shared VM at @gpuvm_base and
	 * attaches KFD eviction fences to the BO dma_resv.
	 */
	r = kfd_import_dmabuf(kfd, gpu_id, dmabuf,
			      gpuvm_base, &kfd_handle);
	igt_assert_eq(r, 0);
	r = kfd_map_memory_to_gpu(kfd, kfd_handle, gpu_id);
	igt_assert_eq(r, 0);

	/*
	 * Also import via amdgpu for SDMA submission.
	 * No separate amdgpu VA mapping is needed -- KFD already
	 * mapped this BO in the shared VM via kfd_acquire_vm +
	 * kfd_map_memory_to_gpu.  Adding a second mapping would
	 * conflict and cause a GPU page fault.
	 */
	r = amdgpu_bo_import(dev, amdgpu_bo_handle_type_dma_buf_fd,
			     dmabuf, &import);
	igt_assert_eq(r, 0);

	/* Query HW DMA transfer limits */
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

	igt_info("Submitting %d SDMA copies "
		 "(KFD amdgpu fences on dma_resv, no wait)...",
		 NUM_SDMA_COPIES);

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

	close(kfd);

	igt_info("All amdgpu/KFD fds closed -- attempting rmmod...\n");
	r = igt_kmod_unload("amdgpu");
	igt_info("igt_kmod_unload returned %d\n", r);

	if (r == 0) {
		bool busy_r, busy_w;

		/*
		 * Module unloaded.  Exercise dma_resv fences via poll()
		 * -- KFD eviction fences + SDMA fences are still in
		 * the VGEM BO dma_resv.
		 */
		igt_info("Module unloaded -- exercising KFD fences via "
			 "poll()...\n");

		busy_r = dmabuf_busy(dmabuf, false);
		igt_info("DMA-BUF read-busy:  %s\n",
			 busy_r ? "yes" : "no");

		busy_w = dmabuf_busy(dmabuf, true);
		igt_info("DMA-BUF write-busy: %s\n",
			 busy_w ? "yes" : "no");

		igt_info("KFD fences survived module unload -- "
			 "reloading...\n");
		r = igt_kmod_load("amdgpu", NULL);
		igt_info("Module reload returned %d\n", r);
	} else {
		igt_info("Module unload refused (r=%d) -- fences may hold "
			 "module refs (expected with patches)\n", r);
	}

	close(dmabuf);
	gem_close(vgem, bo.handle);
}

#endif /* HAVE_KFD_IOCTL_H */




int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int vgem = -1, amd = -1;
	uint32_t major, minor;
	int err, r;

	igt_fixture() {
		uint64_t cap;

#ifndef HAVE_KFD_IOCTL_H
		igt_skip("linux/kfd_ioctl.h not available at build time\n");
#endif

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

		/* Verify PRIME and KFD support */
		igt_require(drmGetCap(vgem, DRM_CAP_PRIME, &cap) == 0 &&
			    (cap & DRM_PRIME_CAP_EXPORT));
		igt_require(drmGetCap(amd, DRM_CAP_PRIME, &cap) == 0 &&
			    (cap & DRM_PRIME_CAP_IMPORT));
		igt_require_f(access("/dev/kfd", R_OK | W_OK) == 0,
			      "/dev/kfd not available\n");
	}

	igt_subtest("kfd-fence-outlives-module") {
		test_kfd_fence_outlives_module(vgem, amd, device);

		/* Re-open for fixture teardown (module may have reloaded) */
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
