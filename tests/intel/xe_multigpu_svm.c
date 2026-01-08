// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <unistd.h>

#include "drmtest.h"
#include "igt.h"
#include "igt_multigpu.h"

#include "intel_blt.h"
#include "intel_mocs.h"
#include "intel_reg.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

/**
 * TEST: Multi-GPU SVM functional and performance tests
 * Description: Validate multi-GPU Shared Virtual Memory (SVM) features,
 *		including cross-GPU access, atomic operations, coherency
 *
 * Category: Multi-GPU
 * Mega feature: SVM, GPU virtualization
 * Sub-category: SVM, memory management, performance
 * Functionality: Multi-GPU SVM, migration, coherency, atomic ops
 * Test category: functional, performance, stress
 *
 * SUBTEST: mgpu-xgpu-access-basic
 * Description:
 *	Test basic cross-GPU memory access in multi-GPU SVM configuration
 *
 * SUBTEST: mgpu-xgpu-access-prefetch
 * Description:
 *	Test cross-GPU memory access with prefetch in multi-GPU SVM configuration
 *
 * SUBTEST: mgpu-atomic-op-basic
 * Description:
 *	Test basic cross-GPU atomic increment operations in multi-GPU SVM configuration
 *	operation on GPU1 and then atomic operation on GPU2 using same
 *	address
 *
 * SUBTEST: mgpu-atomic-op-prefetch
 * Description:
 *	Tests cross-GPU atomic increment operations with explicit memory prefetch
 *	to validate SVM atomic operations in multi-GPU config
 *
 */

#define MAX_XE_REGIONS	8
#define MAX_XE_GPUS 8
#define NUM_LOOPS 1
#define BATCH_SIZE(_fd) ALIGN(SZ_8K, xe_get_default_alignment(_fd))
#define BIND_SYNC_VAL 0x686868
#define EXEC_SYNC_VAL 0x676767
#define COPY_SIZE SZ_64M
#define	ATOMIC_OP_VAL	56

#define MULTIGPU_PREFETCH		BIT(1)
#define MULTIGPU_XGPU_ACCESS		BIT(2)
#define MULTIGPU_ATOMIC_OP		BIT(3)

#define INIT	2
#define STORE	3
#define ATOMIC	4

struct xe_svm_gpu_info {
	bool supports_faults;
	int vram_regions[MAX_XE_REGIONS];
	unsigned int num_regions;
	unsigned int va_bits;
	int fd;
};

typedef void (*gpu_pair_fn)(struct xe_svm_gpu_info *src,
			    struct xe_svm_gpu_info *dst,
			    struct drm_xe_engine_class_instance *eci,
			    unsigned int flags);

struct test_exec_data {
	uint32_t batch[32];
	uint64_t pad;
	uint64_t vm_sync;
	uint64_t exec_sync;
	uint32_t data;
	uint32_t expected_data;
	uint64_t batch_addr;
};

static void for_each_gpu_pair(int num_gpus,
			      struct xe_svm_gpu_info *gpus,
			      struct drm_xe_engine_class_instance *eci,
			      gpu_pair_fn fn,
			      unsigned int flags);

static void gpu_mem_access_wrapper(struct xe_svm_gpu_info *src,
				   struct xe_svm_gpu_info *dst,
				   struct drm_xe_engine_class_instance *eci,
				   unsigned int flags);

static void gpu_atomic_inc_wrapper(struct xe_svm_gpu_info *src,
				   struct xe_svm_gpu_info *dst,
				   struct drm_xe_engine_class_instance *eci,
				   unsigned int flags);

static void
create_vm_and_queue(struct xe_svm_gpu_info *gpu, struct drm_xe_engine_class_instance *eci,
		    uint32_t *vm, uint32_t *exec_queue)
{
	*vm = xe_vm_create(gpu->fd,
			   DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);
	*exec_queue = xe_exec_queue_create(gpu->fd, *vm, eci, 0);
	xe_vm_bind_lr_sync(gpu->fd, *vm, 0, 0, 0, 1ull << gpu->va_bits,
			   DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR);
}

static void
setup_sync(struct drm_xe_sync *sync, uint64_t **sync_addr, uint64_t timeline_value)
{
	*sync_addr = malloc(sizeof(**sync_addr));
	igt_assert(*sync_addr);
	sync->flags = DRM_XE_SYNC_FLAG_SIGNAL;
	sync->type = DRM_XE_SYNC_TYPE_USER_FENCE;
	sync->addr = to_user_pointer((uint64_t *)*sync_addr);
	sync->timeline_value = timeline_value;
	**sync_addr = 0;
}

static void
cleanup_vm_and_queue(struct xe_svm_gpu_info *gpu, uint32_t vm, uint32_t exec_queue)
{
	xe_vm_unbind_lr_sync(gpu->fd, vm, 0, 0, 1ull << gpu->va_bits);
	xe_exec_queue_destroy(gpu->fd, exec_queue);
	xe_vm_destroy(gpu->fd, vm);
}

static void xe_multigpu_madvise(int src_fd, uint32_t vm, uint64_t addr, uint64_t size,
				uint64_t ext, uint32_t type, int dst_fd, uint16_t policy,
				uint32_t instance, uint32_t exec_queue)
{
	int ret;

	if (src_fd != dst_fd) {
		ret = __xe_vm_madvise(src_fd, vm, addr, size, ext, type, dst_fd, policy, instance);
		if (ret == -ENOLINK) {
			igt_info("No fast interconnect between GPU0 and GPU1, falling back to local VRAM\n");
			ret = __xe_vm_madvise(src_fd, vm, addr, size, ext, type,
					      DRM_XE_PREFERRED_LOC_DEFAULT_DEVICE,
					      policy, 0);
			if (ret) {
				igt_info("Local VRAM madvise failed, falling back to system memory\n");
				xe_vm_madvise(src_fd, vm, addr, size, ext, type,
					      DRM_XE_PREFERRED_LOC_DEFAULT_SYSTEM, policy,
					      0);
			}
		} else {
			igt_assert_eq(ret, 0);
		}
	} else {
		xe_vm_madvise(src_fd, vm, addr, size, ext, type, dst_fd, policy, instance);
	}
}

static void xe_multigpu_prefetch(int src_fd, uint32_t vm, uint64_t addr, uint64_t size,
				 struct drm_xe_sync *sync, uint64_t *sync_addr,
				 uint32_t exec_queue, unsigned int flags)
{
	if (flags & MULTIGPU_PREFETCH) {
		xe_vm_prefetch_async(src_fd, vm, 0, 0, addr, size, sync, 1,
				     DRM_XE_CONSULT_MEM_ADVISE_PREF_LOC);
		if (*sync_addr != sync->timeline_value)
			xe_wait_ufence(src_fd, (uint64_t *)sync_addr, sync->timeline_value,
				       exec_queue, NSEC_PER_SEC * 10);
	}
}

static void for_each_gpu_pair(int num_gpus, struct xe_svm_gpu_info *gpus,
			      struct drm_xe_engine_class_instance *eci,
			      gpu_pair_fn fn, unsigned int flags)
{
	for (int src = 0; src < num_gpus; src++) {
		if (!gpus[src].supports_faults)
			continue;

		for (int dst = 0; dst < num_gpus; dst++) {
			if (src == dst)
				continue;
			fn(&gpus[src], &gpus[dst], eci, flags);
		}
	}
}

static void open_pagemaps(int fd, struct xe_svm_gpu_info *info);

static void
atomic_batch_init(int fd, uint32_t vm, uint64_t src_addr,
		  uint32_t *bo, uint64_t *addr)
{
	uint32_t batch_bo_size = BATCH_SIZE(fd);
	uint32_t batch_bo;
	uint64_t batch_addr;
	void *batch;
	uint32_t *cmd;
	int i = 0;

	batch_bo = xe_bo_create(fd, vm, batch_bo_size, vram_if_possible(fd, 0), 0);
	batch = xe_bo_map(fd, batch_bo, batch_bo_size);
	cmd = (uint32_t *)batch;

	cmd[i++] = MI_ATOMIC | MI_ATOMIC_INC;
	cmd[i++] = src_addr;
	cmd[i++] = src_addr >> 32;
	cmd[i++] = MI_BATCH_BUFFER_END;

	batch_addr = to_user_pointer(batch);
	/* Punch a gap in the SVM map where we map the batch_bo */
	xe_vm_bind_lr_sync(fd, vm, batch_bo, 0, batch_addr, batch_bo_size, 0);
	*bo = batch_bo;
	*addr = batch_addr;
}

static void batch_init(int fd, uint32_t vm, uint64_t src_addr,
		       uint64_t dst_addr, uint64_t copy_size,
		       uint32_t *bo, uint64_t *addr)
{
	uint32_t width = copy_size / 256;
	uint32_t height = 1;
	uint32_t batch_bo_size = BATCH_SIZE(fd);
	uint32_t batch_bo;
	uint64_t batch_addr;
	void *batch;
	uint32_t *cmd;
	uint16_t dev_id = intel_get_drm_devid(fd);
	uint32_t mocs_index = intel_get_uc_mocs_index(fd);
	int i = 0;

	batch_bo = xe_bo_create(fd, vm, batch_bo_size, vram_if_possible(fd, 0), 0);
	batch = xe_bo_map(fd, batch_bo, batch_bo_size);
	cmd = (uint32_t *)batch;
	cmd[i++] = MEM_COPY_CMD | (1 << 19);
	cmd[i++] = width - 1;
	cmd[i++] = height - 1;
	cmd[i++] = width - 1;
	cmd[i++] = width - 1;
	cmd[i++] = src_addr & ((1UL << 32) - 1);
	cmd[i++] = src_addr >> 32;
	cmd[i++] = dst_addr & ((1UL << 32) - 1);
	cmd[i++] = dst_addr >> 32;
	if (intel_graphics_ver(dev_id) >= IP_VER(20, 0)) {
		cmd[i++] = mocs_index << XE2_MEM_COPY_SRC_MOCS_SHIFT | mocs_index;
	} else {
		cmd[i++] = mocs_index << GEN12_MEM_COPY_MOCS_SHIFT | mocs_index;
	}

	cmd[i++] = MI_BATCH_BUFFER_END;
	cmd[i++] = MI_BATCH_BUFFER_END;

	batch_addr = to_user_pointer(batch);
	/* Punch a gap in the SVM map where we map the batch_bo */
	xe_vm_bind_lr_sync(fd, vm, batch_bo, 0, batch_addr, batch_bo_size, 0);
	*bo = batch_bo;
	*addr = batch_addr;
}

static void batch_fini(int fd, uint32_t vm, uint32_t bo, uint64_t addr)
{
	/* Unmap the batch bo by re-instating the SVM binding. */
	xe_vm_bind_lr_sync(fd, vm, 0, 0, addr, BATCH_SIZE(fd),
			   DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR);
	gem_close(fd, bo);
}

static void open_pagemaps(int fd, struct xe_svm_gpu_info *info)
{
	unsigned int count = 0;
	uint64_t regions = all_memory_regions(fd);
	uint32_t region;

	xe_for_each_mem_region(fd, regions, region) {
		if (XE_IS_VRAM_MEMORY_REGION(fd, region)) {
			struct drm_xe_mem_region *mem_region =
				xe_mem_region(fd, 1ull << (region - 1));
			igt_assert(count < MAX_XE_REGIONS);
			info->vram_regions[count++] = mem_region->instance;
		}
	}

	info->num_regions = count;
}

static int get_device_info(struct xe_svm_gpu_info gpus[], int num_gpus)
{
	int cnt;
	int xe;
	int i;

	for (i = 0, cnt = 0 && i < 128; cnt < num_gpus; i++) {
		xe = __drm_open_driver_another(i, DRIVER_XE);
		if (xe < 0)
			break;

		gpus[cnt].fd = xe;
		cnt++;
	}

	return cnt;
}

static int
mgpu_check_fault_support(struct xe_svm_gpu_info *gpu1,
			 struct xe_svm_gpu_info *gpu2)
{
	int ret = 0;

	if (!gpu1->supports_faults || !gpu2->supports_faults) {
		igt_debug("GPU does not support page faults, skipping execution\n");
		ret = 1;
	}
	return ret;
}

static void
gpu_madvise_exec_sync(struct xe_svm_gpu_info *gpu, uint32_t vm, uint32_t exec_queue,
		      uint64_t dst_addr, uint64_t *batch_addr, unsigned int flags,
		      void *perf)
{
	struct drm_xe_sync sync = {};
	uint64_t *sync_addr;

	xe_multigpu_madvise(gpu->fd, vm, dst_addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
			    gpu->fd, 0, gpu->vram_regions[0], exec_queue);

	setup_sync(&sync, &sync_addr, BIND_SYNC_VAL);
	xe_multigpu_prefetch(gpu->fd, vm, dst_addr, SZ_4K, &sync,
			     sync_addr, exec_queue, flags);
	free(sync_addr);

	sync_addr = (void *)((char *)*batch_addr + SZ_4K);
	sync.addr = to_user_pointer((uint64_t *)sync_addr);
	sync.timeline_value = EXEC_SYNC_VAL;
	WRITE_ONCE(*sync_addr, 0);

	xe_exec_sync(gpu->fd, exec_queue, *batch_addr, &sync, 1);
	if (READ_ONCE(*sync_addr) != EXEC_SYNC_VAL)
		xe_wait_ufence(gpu->fd, (uint64_t *)sync_addr, EXEC_SYNC_VAL, exec_queue,
			       NSEC_PER_SEC * 10);
}

static void
gpu_batch_create(struct xe_svm_gpu_info *gpu, uint32_t vm, uint32_t exec_queue,
		 uint64_t src_addr, uint64_t dst_addr,
		 uint32_t *batch_bo, uint64_t *batch_addr,
		 unsigned int flags, int op_type)
{
	switch (op_type) {
	case ATOMIC:
		atomic_batch_init(gpu->fd, vm, src_addr, batch_bo, batch_addr);
		break;
	case INIT:
		batch_init(gpu->fd, vm, src_addr, dst_addr, SZ_4K, batch_bo, batch_addr);
		break;
	default:
		igt_assert(!"Unknown batch op_type");
	}
}

static void
copy_src_dst(struct xe_svm_gpu_info *gpu1,
	     struct xe_svm_gpu_info *gpu2,
	     struct drm_xe_engine_class_instance *eci,
	     unsigned int flags)
{
	uint32_t vm[1];
	uint32_t exec_queue[2];
	uint32_t batch_bo;
	void *copy_src, *copy_dst;
	uint64_t batch_addr;
	struct drm_xe_sync sync = {};
	uint64_t *sync_addr;

	/* Checking if GPU support page faults */
	if (mgpu_check_fault_support(gpu1, gpu2))
		return;

	create_vm_and_queue(gpu1, eci, &vm[0], &exec_queue[0]);

	/* Allocate source and destination buffers */
	copy_src = aligned_alloc(xe_get_default_alignment(gpu1->fd), SZ_64M);
	igt_assert(copy_src);
	copy_dst = aligned_alloc(xe_get_default_alignment(gpu2->fd), SZ_64M);
	igt_assert(copy_dst);

	batch_init(gpu1->fd, vm[0], to_user_pointer(copy_src), to_user_pointer(copy_dst),
		   COPY_SIZE, &batch_bo, &batch_addr);

	/* Fill the source with a pattern, clear the destination. */
	memset(copy_src, 0x67, COPY_SIZE);
	memset(copy_dst, 0x0, COPY_SIZE);

	xe_multigpu_madvise(gpu1->fd, vm[0], to_user_pointer(copy_dst), COPY_SIZE,
			    0, DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
			    gpu2->fd, 0, gpu2->vram_regions[0], exec_queue[0]);

	setup_sync(&sync, &sync_addr, BIND_SYNC_VAL);
	xe_multigpu_prefetch(gpu1->fd, vm[0], to_user_pointer(copy_dst), COPY_SIZE, &sync,
			     sync_addr, exec_queue[0], flags);
	free(sync_addr);

	sync_addr = (void *)((char *)batch_addr + SZ_4K);
	sync.addr = to_user_pointer((uint64_t *)sync_addr);
	sync.timeline_value = EXEC_SYNC_VAL;
	WRITE_ONCE(*sync_addr, 0);

	/* Execute a GPU copy. */
	xe_exec_sync(gpu1->fd, exec_queue[0], batch_addr, &sync, 1);
	if (READ_ONCE(*sync_addr) != EXEC_SYNC_VAL)
		xe_wait_ufence(gpu1->fd, (uint64_t *)sync_addr, EXEC_SYNC_VAL, exec_queue[0],
			       NSEC_PER_SEC * 10);

	igt_assert(memcmp(copy_src, copy_dst, COPY_SIZE) == 0);

	free(copy_dst);
	free(copy_src);
	munmap((void *)batch_addr, BATCH_SIZE(gpu1->fd));
	batch_fini(gpu1->fd, vm[0], batch_bo, batch_addr);
	cleanup_vm_and_queue(gpu1, vm[0], exec_queue[0]);
}

static void
atomic_inc_op(struct xe_svm_gpu_info *gpu1,
	      struct xe_svm_gpu_info *gpu2,
	      struct drm_xe_engine_class_instance *eci,
	      unsigned int flags)
{
	uint64_t addr;
	uint32_t vm[2];
	uint32_t exec_queue[2];
	uint32_t batch_bo[2];
	struct test_exec_data *data;
	uint64_t batch_addr[2];
	void *copy_dst;
	uint32_t final_value;

	/* Skip if either GPU doesn't support faults */
	if (mgpu_check_fault_support(gpu1, gpu2))
		return;

	create_vm_and_queue(gpu1, eci, &vm[0], &exec_queue[0]);
	create_vm_and_queue(gpu2, eci, &vm[1], &exec_queue[1]);

	data = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(data);
	data[0].vm_sync = 0;
	addr = to_user_pointer(data);

	copy_dst = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(copy_dst);

	WRITE_ONCE(*(uint64_t *)addr, ATOMIC_OP_VAL - 1);

	/* GPU1: Atomic Batch create */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
			 &batch_bo[0], &batch_addr[0], flags, ATOMIC);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, vm[0], exec_queue[0], addr, &batch_addr[0], flags, NULL);

	/* GPU2 --> copy from GPU1 */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(copy_dst),
			 &batch_bo[1], &batch_addr[1], flags, INIT);

	/*GPU2: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu2, vm[1], exec_queue[1], to_user_pointer(copy_dst),
			      &batch_addr[1], flags, NULL);

	/* NOW CPU can read copy_dst (GPU2 ATOMIC op) */
	final_value = *(uint32_t *)copy_dst;
	igt_assert_eq(final_value, ATOMIC_OP_VAL);

	/* GPU2: Atomic Batch create */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], to_user_pointer(copy_dst), 0,
			 &batch_bo[1], &batch_addr[1], flags, ATOMIC);

	/*GPU2: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu2, vm[1], exec_queue[1], to_user_pointer(copy_dst),
			      &batch_addr[1], flags, NULL);

	/* GPU1 --> copy from GPU2 */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], to_user_pointer(copy_dst), addr,
			 &batch_bo[0], &batch_addr[0], flags, INIT);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, vm[0], exec_queue[1], addr, &batch_addr[0], flags, NULL);

	/* NOW CPU can read addr (GPU1 ATOMIC op) */
	final_value = *(uint32_t *)addr;
	igt_assert_eq(final_value, ATOMIC_OP_VAL + 1);

	munmap((void *)batch_addr[0], BATCH_SIZE(gpu1->fd));
	munmap((void *)batch_addr[1], BATCH_SIZE(gpu2->fd));
	batch_fini(gpu1->fd, vm[0], batch_bo[0], batch_addr[0]);
	batch_fini(gpu2->fd, vm[1], batch_bo[1], batch_addr[1]);
	free(data);
	free(copy_dst);

	cleanup_vm_and_queue(gpu1, vm[0], exec_queue[0]);
	cleanup_vm_and_queue(gpu2, vm[1], exec_queue[1]);
}

static void
gpu_mem_access_wrapper(struct xe_svm_gpu_info *src,
		       struct xe_svm_gpu_info *dst,
		       struct drm_xe_engine_class_instance *eci,
		       unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	copy_src_dst(src, dst, eci, flags);
}

static void
gpu_atomic_inc_wrapper(struct xe_svm_gpu_info *src,
		       struct xe_svm_gpu_info *dst,
		       struct drm_xe_engine_class_instance *eci,
		       unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	atomic_inc_op(src, dst, eci, flags);
}

static void
test_mgpu_exec(int gpu_cnt, struct xe_svm_gpu_info *gpus,
	       struct drm_xe_engine_class_instance *eci,
	       unsigned int flags)
{
	if (flags & MULTIGPU_XGPU_ACCESS)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_mem_access_wrapper, flags);
	if (flags & MULTIGPU_ATOMIC_OP)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_atomic_inc_wrapper, flags);
}

struct section {
	const char *name;
	unsigned int flags;
};

IGT_TEST_DESCRIPTION("Validate multi-GPU SVM features including xGPU, atomic-ops");

int igt_main()
{
	struct xe_svm_gpu_info gpus[MAX_XE_GPUS];
	struct xe_device *xe;
	int gpu, gpu_cnt;

	struct drm_xe_engine_class_instance eci = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};

	const struct section msections[] = {
		{ "xgpu-access-basic", MULTIGPU_XGPU_ACCESS },
		{ "xgpu-access-prefetch", MULTIGPU_PREFETCH | MULTIGPU_XGPU_ACCESS },
		{ "atomic-op-basic", MULTIGPU_ATOMIC_OP },
		{ "atomic-op-prefetch", MULTIGPU_PREFETCH | MULTIGPU_ATOMIC_OP },
		{ NULL },
	};

	igt_fixture() {
		gpu_cnt = get_device_info(gpus, ARRAY_SIZE(gpus));
		igt_skip_on(gpu_cnt < 2);

		for (gpu = 0; gpu < gpu_cnt; ++gpu) {
			igt_assert(gpu < MAX_XE_GPUS);

			open_pagemaps(gpus[gpu].fd, &gpus[gpu]);
			/* NOTE! inverted return value. */
			gpus[gpu].supports_faults = !xe_supports_faults(gpus[gpu].fd);
			fprintf(stderr, "GPU %u has %u VRAM regions%s, and %s SVM VMs.\n",
				gpu, gpus[gpu].num_regions,
				gpus[gpu].num_regions != 1 ? "s" : "",
				gpus[gpu].supports_faults ? "supports" : "doesn't support");

			xe = xe_device_get(gpus[gpu].fd);
			gpus[gpu].va_bits = xe->va_bits;
		}
	}

	igt_describe("multigpu svm operations");
	for (const struct section *s = msections; s->name; s++) {
		igt_subtest_f("mgpu-%s", s->name)
			test_mgpu_exec(gpu_cnt, gpus, &eci, s->flags);
	}

	igt_fixture() {
		int cnt;

		for (cnt = 0; cnt < gpu_cnt; cnt++)
			drm_close_driver(gpus[cnt].fd);
	}
}
