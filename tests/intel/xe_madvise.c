// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: Validate madvise functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: madvise, purgeable, atomic
 */

#include "igt.h"
#include "xe_drm.h"

#include "intel_gpu_commands.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "intel_pat.h"

/* Purgeable test constants */
#define PURGEABLE_ADDR		0x1a0000
#define PURGEABLE_ADDR2		0x2b0000
#define PURGEABLE_BATCH_ADDR	0x3c0000
#define PURGEABLE_BO_SIZE	4096
#define PURGEABLE_FENCE_VAL	0xbeef
#define PURGEABLE_TEST_PATTERN	0xc0ffee
#define PURGEABLE_DEAD_PATTERN	0xdead

/* Atomic test constants */
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
#define FIVE_SEC		(5LL * NSEC_PER_SEC)
#define QUARTER_SEC		(NSEC_PER_SEC / 4)

static bool xe_has_purgeable_support(int fd)
{
	struct drm_xe_query_config *config = xe_config(fd);

	return config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		DRM_XE_QUERY_CONFIG_FLAG_HAS_PURGING_SUPPORT;
}

/**
 * purgeable_setup_simple_bo - Setup VM and bind a single BO
 * @fd: DRM file descriptor
 * @vm: Output VM handle
 * @bo: Output BO handle
 * @addr: Virtual address to bind at
 * @size: Size of the BO
 * @use_scratch: Whether to use scratch page flag
 *
 * Helper to create VM, BO, and bind it at the specified address.
 */
static void purgeable_setup_simple_bo(int fd, uint32_t *vm, uint32_t *bo,
				      uint64_t addr, size_t size, bool use_scratch)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = 1,
	};
	uint64_t sync_val = 0;

	*vm = xe_vm_create(fd, use_scratch ? DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE : 0, 0);
	*bo = xe_bo_create(fd, *vm, size, vram_if_possible(fd, 0),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	sync.addr = to_user_pointer(&sync_val);
	xe_vm_bind_async(fd, *vm, 0, *bo, 0, addr, size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, 1, 0, NSEC_PER_SEC);
}

/**
 * trigger_memory_pressure - Force kernel to reclaim DONTNEED BOs
 * @fd: DRM file descriptor
 *
 * dGPU: fill VRAM and then over-fill it so TTM has to evict. The DONTNEED
 * BOs under test were created before the pressure BOs, so they are the
 * oldest on the LRU and get evicted (purged) first. Only the over-fill part
 * ends up in system RAM (VRAM-resident pressure BOs do not consume RAM), so
 * the over-fill is capped by available RAM, which lets the test run on
 * systems where RAM is much smaller than VRAM.
 * iGPU: poke the shrinker via igt_purge_vm_caches() (avoids OOM).
 */
static void trigger_memory_pressure(int fd)
{
	uint64_t mem_size, overpressure, ram_limit;
	const uint64_t chunk = 8ull << 20; /* 8 MiB */
	int max_objs, n = 0;
	uint32_t *handles;
	uint64_t total;
	void *p;
	uint32_t handle, vm;

	/* iGPU: use the shrinker, no need to flood system RAM */
	if (!xe_has_vram(fd)) {
		igt_purge_vm_caches(fd);
		return;
	}

	/* dGPU: fill VRAM + 50 % to force TTM eviction */
	mem_size = xe_visible_vram_size(fd, 0);
	ram_limit = (igt_get_avail_ram_mb() << 20) / 4;
	overpressure = min_t(uint64_t, mem_size / 2, ram_limit);
	if (overpressure < (64 << 20))
		overpressure = 64 << 20;
	max_objs = DIV_ROUND_UP(mem_size + overpressure, chunk);

	igt_debug("memory pressure: vram %" PRIu64 " MiB, over-fill %" PRIu64 " MiB\n",
		  mem_size >> 20, overpressure >> 20);

	/* Only the over-fill is evicted to system RAM, the rest stays in VRAM */
	igt_require_memory(DIV_ROUND_UP(overpressure, chunk), chunk, CHECK_RAM);

	/* Separate VM so pressure BOs don't interfere with the test */
	vm = xe_vm_create(fd, 0, 0);

	handles = malloc(max_objs * sizeof(*handles));
	igt_assert(handles);

	total = 0;
	while (total < mem_size + overpressure && n < max_objs) {
		uint32_t err;

		err = __xe_bo_create(fd, vm, chunk,
				     vram_if_possible(fd, 0),
				     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
				     NULL, &handle);
		if (err) /* Out of VRAM — sufficient pressure achieved */
			break;

		handles[n++] = handle;
		total += chunk;

		p = xe_bo_map(fd, handle, chunk);
		igt_assert(p != MAP_FAILED);

		/* Fault in all pages so they actually consume VRAM */
		memset(p, 0xCD, chunk);
		munmap(p, chunk);
	}

	/* Allow shrinker time to process pressure */
	usleep(100000);

	for (int i = 0; i < n; i++)
		gem_close(fd, handles[i]);

	free(handles);

	xe_vm_destroy(fd, vm);
}

/**
 * purgeable_mark_and_verify_purged - Mark DONTNEED, pressure, check purged
 * @fd: DRM file descriptor
 * @vm: VM handle
 * @addr: Virtual address of the BO
 * @size: Size of the BO
 *
 * Returns true if the BO was purged under memory pressure.
 */
static bool purgeable_mark_and_verify_purged(int fd, uint32_t vm, uint64_t addr, size_t size)
{
	uint32_t retained;

	/* Mark as DONTNEED */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	if (retained == 0)
		return true; /* Already purged */

	/* Trigger memory pressure */
	trigger_memory_pressure(fd);

	/* Verify purged */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);
	return retained == 0;
}

static jmp_buf jmp;

__noreturn static void sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

/**
 * SUBTEST: purged-mmap-blocked
 * Description: After BO is purged, verify mmap() fails with -EINVAL
 * Test category: functionality test
 */
static void test_purged_mmap_blocked(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	struct drm_xe_gem_mmap_offset mmo = {};
	void *ptr;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, false);
	if (!purgeable_mark_and_verify_purged(fd, vm, addr, bo_size)) {
		gem_close(fd, bo);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	/*
	 * Getting the mmap offset is always allowed regardless of purgeable
	 * state - the blocking happens at mmap() time (xe_gem_object_mmap).
	 * For a purged BO, mmap() must fail with -EINVAL (no backing store).
	 */
	mmo.handle = bo;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo), 0);

	ptr = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmo.offset);
	igt_assert_eq_ptr(ptr, MAP_FAILED);
	igt_assert_eq(errno, EINVAL);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * purgeable_setup_batch_and_data - Setup VM with batch and data BOs for GPU exec
 * @fd: DRM file descriptor
 * @vm: Output VM handle
 * @bind_engine: Output bind engine handle
 * @batch_bo: Output batch BO handle
 * @data_bo: Output data BO handle
 * @batch: Output batch buffer pointer
 * @data: Output data buffer pointer
 * @batch_addr: Batch virtual address
 * @data_addr: Data virtual address
 * @batch_size: Batch buffer size
 * @data_size: Data buffer size
 *
 * Helper to create VM, bind engine, batch and data BOs, and bind them.
 */
static void purgeable_setup_batch_and_data(int fd, uint32_t *vm,
					   uint32_t *bind_engine,
					   uint32_t *batch_bo,
					   uint32_t *data_bo,
					   uint32_t **batch,
					   uint32_t **data,
					   uint64_t batch_addr,
					   uint64_t data_addr,
					   size_t batch_size,
					   size_t data_size)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = PURGEABLE_FENCE_VAL,
	};
	uint64_t vm_sync = 0;

	*vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, 0);
	*bind_engine = xe_bind_exec_queue_create(fd, *vm, 0);

	/* Create and bind batch BO */
	*batch_bo = xe_bo_create(fd, *vm, batch_size, vram_if_possible(fd, 0),
				 DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	*batch = xe_bo_map(fd, *batch_bo, batch_size);

	sync.addr = to_user_pointer(&vm_sync);
	xe_vm_bind_async(fd, *vm, *bind_engine, *batch_bo, 0, batch_addr, batch_size, &sync, 1);
	xe_wait_ufence(fd, &vm_sync, PURGEABLE_FENCE_VAL, 0, NSEC_PER_SEC);

	/* Create and bind data BO */
	*data_bo = xe_bo_create(fd, *vm, data_size, vram_if_possible(fd, 0),
				DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	*data = xe_bo_map(fd, *data_bo, data_size);

	vm_sync = 0;
	xe_vm_bind_async(fd, *vm, *bind_engine, *data_bo, 0, data_addr, data_size, &sync, 1);
	xe_wait_ufence(fd, &vm_sync, PURGEABLE_FENCE_VAL, 0, NSEC_PER_SEC);
}

/**
 * purgeable_setup_two_vms_shared_bo - Setup two VMs with one shared BO
 * @fd: DRM file descriptor
 * @vm1: Output first VM handle
 * @vm2: Output second VM handle
 * @bo: Output shared BO handle
 * @addr1: Virtual address in VM1
 * @addr2: Virtual address in VM2
 * @size: Size of the BO
 * @use_scratch: Whether to use scratch page flag for VMs
 *
 * Helper to create two VMs and bind one shared BO in both VMs.
 * Returns mapped pointer to the BO.
 */
static void *purgeable_setup_two_vms_shared_bo(int fd, uint32_t *vm1, uint32_t *vm2,
					       uint32_t *bo, uint64_t addr1,
					       uint64_t addr2, size_t size,
					       bool use_scratch)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = 1,
	};
	uint64_t sync_val = 0;
	void *map;

	/* Create two VMs */
	*vm1 = xe_vm_create(fd, use_scratch ? DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE : 0, 0);
	*vm2 = xe_vm_create(fd, use_scratch ? DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE : 0, 0);

	/* Create shared BO */
	*bo = xe_bo_create(fd, 0, size, vram_if_possible(fd, 0),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	map = xe_bo_map(fd, *bo, size);
	memset(map, 0xAB, size);

	/* Bind BO in VM1 */
	sync.addr = to_user_pointer(&sync_val);
	sync_val = 0;
	xe_vm_bind_async(fd, *vm1, 0, *bo, 0, addr1, size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, 1, 0, NSEC_PER_SEC);

	/* Bind BO in VM2 */
	sync_val = 0;
	xe_vm_bind_async(fd, *vm2, 0, *bo, 0, addr2, size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, 1, 0, NSEC_PER_SEC);

	return map;
}

/**
 * SUBTEST: dontneed-before-mmap
 * Description: Mark BO as DONTNEED before mmap, verify mmap() fails with -EBUSY
 * Test category: functionality test
 */
static void test_dontneed_before_mmap(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	struct drm_xe_gem_mmap_offset mmo = {};
	uint32_t retained;
	void *ptr;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, false);

	/* Mark BO as DONTNEED - new mmap operations must be blocked */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/* Ioctl succeeds even for DONTNEED BO; blocking happens at mmap() time. */
	mmo.handle = bo;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo), 0);

	/* mmap() on a DONTNEED BO must fail with EBUSY. */
	ptr = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmo.offset);
	igt_assert_eq_ptr(ptr, MAP_FAILED);
	igt_assert_eq(errno, EBUSY);

	/* Restore to WILLNEED before cleanup */
	xe_vm_madvise_purgeable(fd, vm, addr, bo_size,
				DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: dontneed-after-mmap
 * Description: Mark BO as DONTNEED after mmap, verify SIGBUS on accessing purged mapping
 * Test category: functionality test
 */
static void test_dontneed_after_mmap(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	void *map;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, true);

	map = xe_bo_map(fd, bo, bo_size);
	igt_assert(map != MAP_FAILED);
	memset(map, 0xAB, bo_size);

	if (!purgeable_mark_and_verify_purged(fd, vm, addr, bo_size)) {
		munmap(map, bo_size);
		gem_close(fd, bo);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	/* Access purged mapping - should trigger SIGBUS/SIGSEGV */
	{
		sighandler_t old_sigsegv, old_sigbus;
		char *ptr = (char *)map;
		int sig;

		old_sigsegv = signal(SIGSEGV, (sighandler_t)sigtrap);
		old_sigbus = signal(SIGBUS, (sighandler_t)sigtrap);

		sig = sigsetjmp(jmp, 1); /* savemask=1: save/restore signal mask */
		switch (sig) {
		case SIGBUS:
		case SIGSEGV:
			/* Expected - purged mapping access failed */
			break;
		case 0:
			*ptr = 0;
		default:
			igt_assert_f(false,
				     "Access to purged mapping should trigger SIGBUS, got sig=%d\n",
				     sig);
			break;
		}

		signal(SIGBUS, old_sigbus);
		signal(SIGSEGV, old_sigsegv);
	}

	munmap(map, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: dontneed-before-exec
 * Description: Mark BO as DONTNEED before GPU exec, verify GPU behavior with SCRATCH_PAGE
 * Test category: functionality test
 */
static void test_dontneed_before_exec(int fd, struct drm_xe_engine_class_instance *hwe)
{
	uint32_t vm, exec_queue, bo, batch_bo, bind_engine;
	uint64_t data_addr = PURGEABLE_ADDR;
	uint64_t batch_addr = PURGEABLE_BATCH_ADDR;
	size_t data_size = PURGEABLE_BO_SIZE;
	size_t batch_size = PURGEABLE_BO_SIZE;
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = PURGEABLE_FENCE_VAL },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t *data, *batch;
	uint64_t vm_sync = 0;
	int b, ret;

	purgeable_setup_batch_and_data(fd, &vm, &bind_engine, &batch_bo,
				       &bo, &batch, &data, batch_addr,
				       data_addr, batch_size, data_size);

	/* Prepare batch */
	b = 0;
	batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	batch[b++] = data_addr;
	batch[b++] = data_addr >> 32;
	batch[b++] = PURGEABLE_DEAD_PATTERN;
	batch[b++] = MI_BATCH_BUFFER_END;

	/* Phase 1: Purge data BO, batch BO still valid */
	if (!purgeable_mark_and_verify_purged(fd, vm, data_addr, data_size)) {
		munmap(data, data_size);
		munmap(batch, batch_size);
		gem_close(fd, bo);
		gem_close(fd, batch_bo);
		xe_exec_queue_destroy(fd, bind_engine);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;

	vm_sync = 0;
	sync[0].addr = to_user_pointer(&vm_sync);

	/*
	 * VM has SCRATCH_PAGE — exec may succeed with the GPU write
	 * landing on scratch instead of the purged data BO.
	 */
	ret = __xe_exec(fd, &exec);
	if (ret == 0) {
		int64_t timeout = NSEC_PER_SEC;

		__xe_wait_ufence(fd, &vm_sync, PURGEABLE_FENCE_VAL,
				 exec_queue, &timeout);
	}

	/*
	 * Don't purge the batch BO — GPU would fetch zeroed scratch
	 * instructions and trigger an engine reset.
	 */

	munmap(data, data_size);
	munmap(batch, batch_size);
	gem_close(fd, bo);
	gem_close(fd, batch_bo);
	xe_exec_queue_destroy(fd, bind_engine);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: dontneed-after-exec
 * Description: Mark BO as DONTNEED after GPU exec, verify memory becomes inaccessible
 * Test category: functionality test
 */
static void test_dontneed_after_exec(int fd, struct drm_xe_engine_class_instance *hwe)
{
	uint32_t vm, exec_queue, bo, batch_bo, bind_engine;
	uint64_t data_addr = PURGEABLE_ADDR;
	uint64_t batch_addr = PURGEABLE_BATCH_ADDR;
	size_t data_size = PURGEABLE_BO_SIZE;
	size_t batch_size = PURGEABLE_BO_SIZE;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = PURGEABLE_FENCE_VAL },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t *data, *batch;
	uint32_t syncobj;
	int b, ret;

	purgeable_setup_batch_and_data(fd, &vm, &bind_engine, &batch_bo,
				       &bo, &batch, &data, batch_addr,
				       data_addr, batch_size, data_size);
	memset(data, 0, data_size);

	syncobj = syncobj_create(fd, 0);

	/* Prepare batch to write to data BO */
	b = 0;
	batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	batch[b++] = data_addr;
	batch[b++] = data_addr >> 32;
	batch[b++] = 0xfeed0001;
	batch[b++] = MI_BATCH_BUFFER_END;

	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;

	/* Use only syncobj for exec (not USER_FENCE) */
	sync[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	sync[0].handle = syncobj;
	exec.num_syncs = 1;
	exec.syncs = to_user_pointer(&sync[0]);

	ret = __xe_exec(fd, &exec);
	igt_assert_eq(ret, 0);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	munmap(data, data_size);
	data = xe_bo_map(fd, bo, data_size);
	igt_assert(data != MAP_FAILED);
	igt_assert_eq(data[0], 0xfeed0001);

	if (!purgeable_mark_and_verify_purged(fd, vm, data_addr, data_size)) {
		munmap(data, data_size);
		munmap(batch, batch_size);
		gem_close(fd, bo);
		gem_close(fd, batch_bo);
		syncobj_destroy(fd, syncobj);
		xe_exec_queue_destroy(fd, bind_engine);
		xe_exec_queue_destroy(fd, exec_queue);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	/* Prepare second batch (different value) */
	b = 0;
	batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	batch[b++] = data_addr;
	batch[b++] = data_addr >> 32;
	batch[b++] = 0xfeed0002;
	batch[b++] = MI_BATCH_BUFFER_END;

	/*
	 * Second exec with purged BO - may succeed (scratch rebind) or fail.
	 * Either is valid, so don't check results.
	 */
	ret = __xe_exec(fd, &exec);
	if (ret == 0)
		syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL);

	munmap(data, data_size);
	munmap(batch, batch_size);
	gem_close(fd, bo);
	gem_close(fd, batch_bo);
	syncobj_destroy(fd, syncobj);
	xe_exec_queue_destroy(fd, bind_engine);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: per-vma-tracking
 * Description: One BO in two VMs becomes purgeable only when both VMAs are DONTNEED
 * Test category: functionality test
 */
static void test_per_vma_tracking(int fd)
{
	uint32_t bo, vm1, vm2;
	uint64_t addr1 = PURGEABLE_ADDR;
	uint64_t addr2 = PURGEABLE_ADDR2;
	size_t bo_size = PURGEABLE_BO_SIZE;
	uint32_t retained;
	void *map;

	map = purgeable_setup_two_vms_shared_bo(fd, &vm1, &vm2, &bo,
						addr1, addr2,
						bo_size, false);

	/* Mark VMA1 as DONTNEED */
	retained = xe_vm_madvise_purgeable(fd, vm1, addr1, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/* Verify BO NOT purgeable (VMA2 still WILLNEED) */
	retained = xe_vm_madvise_purgeable(fd, vm1, addr1, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);
	igt_assert_eq(retained, 1);

	/* Mark both VMAs as DONTNEED */
	retained = xe_vm_madvise_purgeable(fd, vm1, addr1, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	retained = xe_vm_madvise_purgeable(fd, vm2, addr2, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/*
	 * Trigger pressure and verify BO was purged.
	 * Using vm1 is sufficient since both VMAs are DONTNEED - kernel can purge the BO.
	 */
	trigger_memory_pressure(fd);

	retained = xe_vm_madvise_purgeable(fd, vm1, addr1, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);
	munmap(map, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm1);
	xe_vm_destroy(fd, vm2);

	if (retained != 0)
		igt_skip("Unable to induce purge on this platform/config");

}

/**
 * SUBTEST: per-vma-protection
 * Description: WILLNEED VMA protects BO from purging; both DONTNEED makes BO purgeable
 * Test category: functionality test
 */
static void test_per_vma_protection(int fd, struct drm_xe_engine_class_instance *hwe)
{
	uint32_t vm1, vm2, exec_queue, bo, batch_bo, bind_engine;
	uint64_t data_addr1 = PURGEABLE_ADDR;
	uint64_t data_addr2 = PURGEABLE_ADDR2;
	uint64_t batch_addr = PURGEABLE_BATCH_ADDR;
	size_t data_size = PURGEABLE_BO_SIZE;
	size_t batch_size = PURGEABLE_BO_SIZE;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = PURGEABLE_FENCE_VAL },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync[1]),
	};
	uint32_t *data, *batch;
	uint64_t vm_sync = 0;
	uint32_t retained, syncobj;
	int b, ret;

	/* Create two VMs and bind shared data BO */
	data = purgeable_setup_two_vms_shared_bo(fd, &vm1, &vm2, &bo,
						 data_addr1, data_addr2,
						 data_size, true);
	memset(data, 0, data_size);
	bind_engine = xe_bind_exec_queue_create(fd, vm2, 0);

	/* Create and bind batch BO in VM2 */
	batch_bo = xe_bo_create(fd, vm2, batch_size, vram_if_possible(fd, 0),
				DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	batch = xe_bo_map(fd, batch_bo, batch_size);
	igt_assert(batch != MAP_FAILED);

	sync[0].addr = to_user_pointer(&vm_sync);
	vm_sync = 0;
	xe_vm_bind_async(fd, vm2, bind_engine, batch_bo, 0, batch_addr, batch_size, sync, 1);
	xe_wait_ufence(fd, &vm_sync, PURGEABLE_FENCE_VAL, 0, NSEC_PER_SEC);

	/* Mark VMA1 as DONTNEED, VMA2 stays WILLNEED */
	retained = xe_vm_madvise_purgeable(fd, vm1, data_addr1, data_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/* Trigger pressure - BO should survive */
	trigger_memory_pressure(fd);

	retained = xe_vm_madvise_purgeable(fd, vm2, data_addr2, data_size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);
	igt_assert_eq(retained, 1);

	/* GPU workload - should succeed */
	b = 0;
	batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	batch[b++] = data_addr2;
	batch[b++] = data_addr2 >> 32;
	batch[b++] = PURGEABLE_TEST_PATTERN;
	batch[b++] = MI_BATCH_BUFFER_END;

	syncobj = syncobj_create(fd, 0);
	sync[1].handle = syncobj;
	exec_queue = xe_exec_queue_create(fd, vm2, hwe, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;

	ret = __xe_exec(fd, &exec);
	igt_assert_eq(ret, 0);
	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));

	munmap(data, data_size);
	data = xe_bo_map(fd, bo, data_size);
	igt_assert(data != MAP_FAILED);
	igt_assert_eq(data[0], PURGEABLE_TEST_PATTERN);

	/* Mark both VMAs DONTNEED */
	retained = xe_vm_madvise_purgeable(fd, vm2, data_addr2, data_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/* Trigger pressure - BO should be purged */
	trigger_memory_pressure(fd);

	retained = xe_vm_madvise_purgeable(fd, vm2, data_addr2, data_size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);

	if (retained != 0)
		goto out;

	/* GPU workload - should fail or succeed with NULL rebind */
	batch[3] = PURGEABLE_DEAD_PATTERN;

	ret = __xe_exec(fd, &exec);
	if (ret == 0) {
		/* Exec succeeded, wait for completion before cleanup */
		syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL);
	}

out:
	munmap(data, data_size);
	munmap(batch, batch_size);
	gem_close(fd, bo);
	gem_close(fd, batch_bo);
	syncobj_destroy(fd, syncobj);
	xe_exec_queue_destroy(fd, bind_engine);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm1);
	xe_vm_destroy(fd, vm2);

	if (retained != 0)
		igt_skip("Unable to induce purge on this platform/config");
}

/*
 * Atomic madvise subtests — validate DRM_XE_MEM_RANGE_ATTR_ATOMIC
 * modes (DEVICE, GLOBAL, CPU) on fault-mode SVM VMAs.
 */

struct atomic_data {
	uint32_t batch[32];
	uint64_t vm_sync;
	uint64_t exec_sync;
	uint32_t data;
};

static void atomic_build_batch(struct atomic_data *d, uint64_t gpu_addr)
{
	uint64_t data_offset = (char *)&d->data - (char *)d;
	uint64_t sdi_addr = gpu_addr + data_offset;
	int b = 0;

	d->batch[b++] = MI_ATOMIC | MI_ATOMIC_INC;
	d->batch[b++] = sdi_addr;
	d->batch[b++] = sdi_addr >> 32;
	d->batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(d->batch));
}

/**
 * SUBTEST: atomic-device
 * Description: madvise atomic device supports only GPU atomic operations,
 *		test executes GPU MI_ATOMIC_INC on SVM memory via fault handler
 * Test category: functionality test
 */
static void test_atomic_device(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	struct atomic_data *data;
	uint32_t vm, exec_queue;
	uint64_t addr;
	size_t bo_size;
	int va_bits;
	int pf_count_before, pf_count_after;

	va_bits = xe_va_bits(fd);
	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);

	bo_size = xe_bb_size(fd, sizeof(*data));
	data = aligned_alloc(bo_size, bo_size);
	igt_assert(data);
	memset(data, 0, bo_size);

	addr = to_user_pointer(data);

	/* Bind entire VA space as CPU_ADDR_MIRROR */
	sync[0].addr = to_user_pointer(&data->vm_sync);
	__xe_vm_bind_assert(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
			    DRM_XE_VM_BIND_OP_MAP,
			    DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,
			    sync, 1, 0, 0);
	xe_wait_ufence(fd, &data->vm_sync, USER_FENCE_VALUE, 0, FIVE_SEC);
	data->vm_sync = 0;

	xe_vm_madvise(fd, vm, addr, bo_size, 0,
		      DRM_XE_MEM_RANGE_ATTR_ATOMIC, DRM_XE_ATOMIC_DEVICE, 0, 0);

	atomic_build_batch(data, addr);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = addr + ((char *)&data->batch - (char *)data);

	pf_count_before = xe_gt_stats_get_count(fd, eci->gt_id,
						"svm_pagefault_count");

	sync[0].addr = to_user_pointer(&data->exec_sync);
	xe_exec(fd, &exec);
	xe_wait_ufence(fd, &data->exec_sync, USER_FENCE_VALUE,
		       exec_queue, FIVE_SEC);

	pf_count_after = xe_gt_stats_get_count(fd, eci->gt_id,
					       "svm_pagefault_count");
	if (pf_count_before != pf_count_after)
		igt_info("Pagefault count: before=%d, after=%d\n",
			 pf_count_before, pf_count_after);

	igt_assert_eq(data->data, 1);

	xe_exec_queue_destroy(fd, exec_queue);
	__xe_vm_bind(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
		     DRM_XE_VM_BIND_OP_UNMAP, 0, NULL, 0, 0, 0, 0);
	free(data);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: atomic-global
 * Description: madvise atomic global supports both CPU and GPU atomic operations,
 *		test does CPU atomic increments on SMEM then GPU MI_ATOMIC_INC
 *		which triggers fault-driven migration to VRAM
 * Test category: functionality test
 */
static void test_atomic_global(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	struct atomic_data *data;
	uint32_t vm, exec_queue;
	uint64_t addr;
	size_t bo_size;
	int va_bits, i;
	int n_cpu_ops = 100;
	int pf_count_before, pf_count_after;

	va_bits = xe_va_bits(fd);
	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);

	bo_size = xe_bb_size(fd, sizeof(*data));
	data = aligned_alloc(bo_size, bo_size);
	igt_assert(data);
	memset(data, 0, bo_size);

	addr = to_user_pointer(data);

	sync[0].addr = to_user_pointer(&data->vm_sync);
	__xe_vm_bind_assert(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
			    DRM_XE_VM_BIND_OP_MAP,
			    DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,
			    sync, 1, 0, 0);
	xe_wait_ufence(fd, &data->vm_sync, USER_FENCE_VALUE, 0, FIVE_SEC);
	data->vm_sync = 0;

	xe_vm_madvise(fd, vm, addr, bo_size, 0,
		      DRM_XE_MEM_RANGE_ATTR_ATOMIC, DRM_XE_ATOMIC_GLOBAL, 0, 0);

	for (i = 0; i < n_cpu_ops; i++)
		__atomic_fetch_add(&data->data, 1, __ATOMIC_SEQ_CST);

	igt_assert_eq(data->data, n_cpu_ops);

	atomic_build_batch(data, addr);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = addr + ((char *)&data->batch - (char *)data);

	pf_count_before = xe_gt_stats_get_count(fd, eci->gt_id,
						"svm_pagefault_count");

	sync[0].addr = to_user_pointer(&data->exec_sync);
	xe_exec(fd, &exec);
	xe_wait_ufence(fd, &data->exec_sync, USER_FENCE_VALUE,
		       exec_queue, FIVE_SEC);

	pf_count_after = xe_gt_stats_get_count(fd, eci->gt_id,
					       "svm_pagefault_count");
	if (pf_count_before != pf_count_after)
		igt_info("Pagefault count: before=%d, after=%d\n",
			 pf_count_before, pf_count_after);

	igt_assert_eq(data->data, n_cpu_ops + 1);

	xe_exec_queue_destroy(fd, exec_queue);
	__xe_vm_bind(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
		     DRM_XE_VM_BIND_OP_UNMAP, 0, NULL, 0, 0, 0, 0);
	free(data);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: atomic-cpu
 * Description: madvise atomic cpu supports only CPU atomic operations,
 *		test verifies GPU MI_ATOMIC_INC is rejected by fault handler
 * Test category: functionality test
 */
static void test_atomic_cpu(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	struct atomic_data *data;
	uint32_t vm, exec_queue;
	uint64_t addr;
	size_t bo_size;
	int va_bits, err;
	int64_t timeout = QUARTER_SEC;

	va_bits = xe_va_bits(fd);
	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);

	bo_size = xe_bb_size(fd, sizeof(*data));
	data = aligned_alloc(bo_size, bo_size);
	igt_assert(data);
	memset(data, 0, bo_size);

	addr = to_user_pointer(data);

	sync[0].addr = to_user_pointer(&data->vm_sync);
	__xe_vm_bind_assert(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
			    DRM_XE_VM_BIND_OP_MAP,
			    DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,
			    sync, 1, 0, 0);
	xe_wait_ufence(fd, &data->vm_sync, USER_FENCE_VALUE, 0, FIVE_SEC);
	data->vm_sync = 0;

	xe_vm_madvise(fd, vm, addr, bo_size, 0,
		      DRM_XE_MEM_RANGE_ATTR_ATOMIC, DRM_XE_ATOMIC_CPU, 0, 0);

	atomic_build_batch(data, addr);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = addr + ((char *)&data->batch - (char *)data);

	/*
	 * GPU MI_ATOMIC_INC must fail: page-fault handler returns -EACCES
	 * for ATOMIC_CPU mode, causing engine reset.  Wait with a short
	 * timeout — the fence should not signal.
	 */
	sync[0].addr = to_user_pointer(&data->exec_sync);
	xe_exec(fd, &exec);
	err = __xe_wait_ufence(fd, &data->exec_sync, USER_FENCE_VALUE,
			       exec_queue, &timeout);

	igt_assert_neq(err, 0);
	igt_assert_eq(data->data, 0);

	xe_exec_queue_destroy(fd, exec_queue);
	__xe_vm_bind(fd, vm, 0, 0, 0, 0, 0x1ull << va_bits,
		     DRM_XE_VM_BIND_OP_UNMAP, 0, NULL, 0, 0, 0, 0);
	free(data);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: partial-unbind-dontneed
 * Description: Partial unbind on a DONTNEED BO must succeed; a fresh bind
 *              on the same DONTNEED BO must be rejected with -EBUSY.
 * Test category: functionality test
 */
static void test_partial_unbind_dontneed(int fd)
{
	uint32_t vm, bo;
	uint64_t addr = PURGEABLE_ADDR;
	uint64_t addr2 = PURGEABLE_ADDR2;
	size_t page_size = PURGEABLE_BO_SIZE;
	size_t bo_size = 2 * PURGEABLE_BO_SIZE;
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = PURGEABLE_FENCE_VAL,
	};
	uint64_t sync_val = 0;
	uint32_t retained;
	int ret;

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	sync.addr = to_user_pointer(&sync_val);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, PURGEABLE_FENCE_VAL, 0, NSEC_PER_SEC);

	/* Mark BO as DONTNEED. */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/*
	 * Partial unbind of the first page must succeed. The remap split
	 * inherits the parent VMA's DONTNEED attr and must not be rejected.
	 */
	sync_val = 0;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, page_size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, PURGEABLE_FENCE_VAL, 0, NSEC_PER_SEC);

	/*
	 * A fresh bind on the same DONTNEED BO must be rejected with -EBUSY.
	 * The kernel must not silently promote a DONTNEED BO back to WILLNEED
	 * through a new bind.
	 */
	sync_val = 0;
	ret = __xe_vm_bind(fd, vm, 0, bo, 0, addr2, bo_size,
			   DRM_XE_VM_BIND_OP_MAP, 0, &sync, 1, 0,
			   DEFAULT_PAT_INDEX, 0);
	igt_assert_eq(ret, -EBUSY);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest_group() {
		igt_fixture()
			igt_require_f(xe_has_purgeable_support(fd),
				      "Kernel does not support purgeable buffer objects\n");

		igt_subtest("dontneed-before-mmap")
			xe_for_each_engine(fd, hwe) {
				test_dontneed_before_mmap(fd);
				break;
			}

		igt_subtest("purged-mmap-blocked")
			xe_for_each_engine(fd, hwe) {
				test_purged_mmap_blocked(fd);
				break;
			}

		igt_subtest("dontneed-after-mmap")
			xe_for_each_engine(fd, hwe) {
				test_dontneed_after_mmap(fd);
				break;
			}

		igt_subtest("dontneed-before-exec")
			xe_for_each_engine(fd, hwe) {
				test_dontneed_before_exec(fd, hwe);
				break;
			}

		igt_subtest("dontneed-after-exec")
			xe_for_each_engine(fd, hwe) {
				test_dontneed_after_exec(fd, hwe);
				break;
			}

		igt_subtest("per-vma-tracking")
			xe_for_each_engine(fd, hwe) {
				test_per_vma_tracking(fd);
				break;
			}

		igt_subtest("per-vma-protection")
			xe_for_each_engine(fd, hwe) {
				test_per_vma_protection(fd, hwe);
				break;
			}

		igt_subtest("partial-unbind-dontneed")
			xe_for_each_engine(fd, hwe) {
				test_partial_unbind_dontneed(fd);
				break;
			}
	}

	igt_subtest_group() {
		igt_fixture() {
			igt_require(xe_has_vram(fd));
			igt_require(!xe_supports_faults(fd));
		}

		igt_subtest("atomic-device")
			xe_for_each_engine(fd, hwe)
				test_atomic_device(fd, hwe);

		igt_subtest("atomic-global")
			xe_for_each_engine(fd, hwe)
				test_atomic_global(fd, hwe);

		/* Run on a single engine — each rejection triggers an engine
		 * reset and CAT error, running on all engines would generate
		 * redundant resets without adding coverage.
		 */
		igt_subtest("atomic-cpu")
			xe_for_each_engine(fd, hwe) {
				test_atomic_cpu(fd, hwe);
				break;
			}
	}

	igt_fixture() {
		xe_device_put(fd);
		drm_close_driver(fd);
	}
}
