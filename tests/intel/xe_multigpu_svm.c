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

#include "intel_gpu_commands.h"
#include "time.h"

#include "xe/xe_gt.h"
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
 * SUBTEST: mgpu-coherency-basic
 * Description:
 *	Test basic cross-GPU memory coherency where one GPU writes data
 *	and another GPU reads to verify coherent memory access without prefetch
 *
 * SUBTEST: mgpu-coherency-fail-basic
 * Description:
 *	Test concurrent write race conditions between GPUs to verify coherency
 *	behavior when multiple GPUs write to the same memory location without prefetch
 *
 * SUBTEST: mgpu-coherency-prefetch
 * Description:
 *	Test cross-GPU memory coherency with explicit prefetch to validate
 *	coherent memory access and migration across GPUs
 *
 * SUBTEST: mgpu-coherency-fail-prefetch
 * Description:
 *	Test concurrent write race conditions with prefetch to verify coherency
 *	behavior and memory migration when multiple GPUs compete for same location
 *
 * SUBTEST: mgpu-latency-basic
 * Description:
 *	Measure basic cross-GPU memory access latency where one GPU writes
 *	and another GPU reads without prefetch to evaluate remote access overhead
 *
 * SUBTEST: mgpu-latency-prefetch
 * Description:
 *	Measure cross-GPU memory access latency with explicit prefetch to
 *	evaluate memory migration overhead and local access performance
 *
 * SUBTEST: mgpu-latency-copy-basic
 * Description:
 *	Measure latency of cross-GPU memory copy operations where one GPU
 *	copies data from another GPU's memory without prefetch
 *
 * SUBTEST: mgpu-latency-copy-prefetch
 * Description:
 *	Measure latency of cross-GPU memory copy operations with prefetch
 *	to evaluate copy performance with memory migration to local VRAM
 *
 * SUBTEST: mgpu-pagefault-basic
 * Description:
 *	Test cross-GPU page fault handling where one GPU writes to memory
 *	and another GPU reads, triggering page faults without prefetch to
 *	validate on-demand page migration across GPUs
 *
 * SUBTEST: mgpu-pagefault-prefetch
 * Description:
 *	Test cross-GPU memory access with prefetch to verify page fault
 *	suppression when memory is pre-migrated to target GPU's VRAM
 *
 * SUBTEST: mgpu-concurrent-access-basic
 * Description:
 *	Test concurrent atomic memory operations where multiple GPUs
 *	simultaneously access and modify the same memory location without
 *	prefetch to validate cross-GPU coherency and synchronization
 *
 * SUBTEST: mgpu-concurrent-access-prefetch
 * Description:
 *	Test concurrent atomic memory operations with prefetch where
 *	multiple GPUs simultaneously access shared memory to validate
 *	coherency with memory migration and local VRAM access
 *
 * SUBTEST: mgpu-atomic-op-conflict
 * Description:
 *	Multi-GPU atomic operation with conflicting madvise regions
 *
 * SUBTEST: mgpu-coherency-conflict
 * Description:
 *	Multi-GPU coherency test with conflicting madvise regions
 *
 * SUBTEST: mgpu-pagefault-conflict
 * Description:
 *	Multi-GPU page fault test with conflicting madvise regions
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
#define BATCH_VALUE	60
#define NUM_ITER	200

#define USER_FENCE_VALUE        0xdeadbeefdeadbeefull
#define FIVE_SEC                (5LL * NSEC_PER_SEC)

#define MULTIGPU_PREFETCH		BIT(1)
#define MULTIGPU_XGPU_ACCESS		BIT(2)
#define MULTIGPU_ATOMIC_OP		BIT(3)
#define MULTIGPU_COH_OP			BIT(4)
#define MULTIGPU_COH_FAIL		BIT(5)
#define MULTIGPU_PERF_OP		BIT(6)
#define MULTIGPU_PERF_REM_COPY		BIT(7)
#define MULTIGPU_PFAULT_OP		BIT(8)
#define MULTIGPU_CONC_ACCESS		BIT(9)
#define MULTIGPU_CONFLICT		BIT(10)

#define INIT	2
#define STORE	3
#define ATOMIC	4
#define DWORD	5

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

static void gpu_coherecy_test_wrapper(struct xe_svm_gpu_info *src,
				      struct xe_svm_gpu_info *dst,
				      struct drm_xe_engine_class_instance *eci,
				      unsigned int flags);

static void gpu_latency_test_wrapper(struct xe_svm_gpu_info *src,
				     struct xe_svm_gpu_info *dst,
				     struct drm_xe_engine_class_instance *eci,
				     unsigned int flags);

static void gpu_fault_test_wrapper(struct xe_svm_gpu_info *src,
				   struct xe_svm_gpu_info *dst,
				   struct drm_xe_engine_class_instance *eci,
				   unsigned int flags);

static void gpu_simult_test_wrapper(struct xe_svm_gpu_info *src,
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

static double time_diff(struct timespec *start, struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1e9;
}

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

static void
store_dword_batch_init(int fd, uint32_t vm, uint64_t src_addr,
		       uint32_t *bo, uint64_t *addr, int value)
{
	uint32_t batch_bo_size = BATCH_SIZE(fd);
	uint32_t batch_bo;
	uint64_t batch_addr;
	void *batch;
	uint32_t *cmd;
	int i = 0;

	batch_bo = xe_bo_create(fd, vm, batch_bo_size, vram_if_possible(fd, 0), 0);
	batch = xe_bo_map(fd, batch_bo, batch_bo_size);
	cmd = (uint32_t *) batch;

	cmd[i++] = MI_STORE_DWORD_IMM_GEN4;
	cmd[i++] = src_addr;
	cmd[i++] = src_addr >> 32;
	cmd[i++] = value;
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
store_dword_batch_init_1k(int fd, uint32_t vm, uint64_t src_addr,
			  uint32_t *bo, uint64_t *addr, int value)
{
	int max_cmds = (4096 - sizeof(uint32_t)) / 16;
	uint32_t batch_bo_size = BATCH_SIZE(fd);
	uint32_t batch_bo;
	uint64_t batch_addr;
	void *batch;
	uint32_t *cmd;
	int i = 0;

	batch_bo = xe_bo_create(fd, vm, batch_bo_size, vram_if_possible(fd, 0), 0);
	batch = xe_bo_map(fd, batch_bo, batch_bo_size);
	cmd = (uint32_t *)batch;

	for (int j = 0; j < max_cmds; j++) {
		uint64_t offset = src_addr + j * 4;

		cmd[i++] = MI_STORE_DWORD_IMM_GEN4;
		cmd[i++] = offset;
		cmd[i++] = offset >> 32;
		cmd[i++] = value;
	}
	cmd[i++] = MI_BATCH_BUFFER_END;

	batch_addr = to_user_pointer(batch);

	xe_vm_bind_lr_sync(fd, vm, batch_bo, 0, batch_addr, batch_bo_size, 0);
	*bo = batch_bo;
	*addr = batch_addr;
}

static void
gpu_madvise_exec_sync(struct xe_svm_gpu_info *gpu, struct xe_svm_gpu_info *xgpu,
		      uint32_t vm, uint32_t exec_queue,
		      uint64_t dst_addr, uint64_t *batch_addr, unsigned int flags,
		      double *perf)
{
	struct drm_xe_sync sync = {};
	struct timespec t_start, t_end;
	uint64_t *sync_addr;

	if (flags & MULTIGPU_CONFLICT) {
		xe_multigpu_madvise(gpu->fd, vm, dst_addr, SZ_4K, 0,
				    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
				    xgpu->fd, 0, xgpu->vram_regions[0], exec_queue);
	} else {
		xe_multigpu_madvise(gpu->fd, vm, dst_addr, SZ_4K, 0,
				    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
				    gpu->fd, 0, gpu->vram_regions[0], exec_queue);
	}

	setup_sync(&sync, &sync_addr, BIND_SYNC_VAL);
	xe_multigpu_prefetch(gpu->fd, vm, dst_addr, SZ_4K, &sync,
			     sync_addr, exec_queue, flags);
	free(sync_addr);

	sync_addr = (void *)((char *)*batch_addr + SZ_4K);
	sync.addr = to_user_pointer((uint64_t *)sync_addr);
	sync.timeline_value = EXEC_SYNC_VAL;
	WRITE_ONCE(*sync_addr, 0);

	if (flags & MULTIGPU_PERF_OP)
		clock_gettime(CLOCK_MONOTONIC, &t_start);

	xe_exec_sync(gpu->fd, exec_queue, *batch_addr, &sync, 1);
	if (READ_ONCE(*sync_addr) != EXEC_SYNC_VAL)
		xe_wait_ufence(gpu->fd, (uint64_t *)sync_addr, EXEC_SYNC_VAL, exec_queue,
			       NSEC_PER_SEC * 10);

	if (flags & MULTIGPU_PERF_OP) {
		clock_gettime(CLOCK_MONOTONIC, &t_end);
		if (perf)
			*perf = time_diff(&t_start, &t_end);
	}
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
	case DWORD:
		if (flags & MULTIGPU_PERF_OP) {
			store_dword_batch_init_1k(gpu->fd, vm, src_addr, batch_bo,
						  batch_addr, BATCH_VALUE);
		} else
			store_dword_batch_init(gpu->fd, vm, src_addr, batch_bo,
					       batch_addr, BATCH_VALUE);
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
	gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr, &batch_addr[0], flags, NULL);

	gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(copy_dst),
			 &batch_bo[1], &batch_addr[1], flags, INIT);

	gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], to_user_pointer(copy_dst),
			      &batch_addr[1], flags, NULL);

	final_value = *(uint32_t *)copy_dst;
	igt_assert_eq(final_value, ATOMIC_OP_VAL);

	/* GPU2: Atomic Batch create */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], to_user_pointer(copy_dst), 0,
			 &batch_bo[1], &batch_addr[1], flags, ATOMIC);

	/*GPU2: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1],
			      to_user_pointer(copy_dst),
			      &batch_addr[1], flags, NULL);

	/* GPU1 --> copy from GPU2 */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], to_user_pointer(copy_dst), addr,
			 &batch_bo[0], &batch_addr[0], flags, INIT);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr,
			      &batch_addr[0], flags, NULL);

	final_value = *(uint32_t *)addr;
	/* NOW CPU can read copy_dst (GPU1 ATOMIC op) */
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
coherency_test_multigpu(struct xe_svm_gpu_info *gpu1,
			struct xe_svm_gpu_info *gpu2,
			struct drm_xe_engine_class_instance *eci,
			unsigned int flags)
{
	uint64_t addr;
	uint32_t vm[2];
	uint32_t exec_queue[2];
	uint32_t batch_bo[2], batch1_bo[2];
	uint64_t batch_addr[2], batch1_addr[2];
	uint64_t *data1;
	void *copy_dst;
	uint32_t final_value;

	/* Skip if either GPU doesn't support faults */
	if (mgpu_check_fault_support(gpu1, gpu2))
		return;

	create_vm_and_queue(gpu1, eci, &vm[0], &exec_queue[0]);
	create_vm_and_queue(gpu2, eci, &vm[1], &exec_queue[1]);

	data1 = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(data1);
	addr = to_user_pointer(data1);

	copy_dst = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(copy_dst);

	/* GPU1: Creating batch with predefined value */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
			 &batch_bo[0], &batch_addr[0], flags, DWORD);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr, &batch_addr[0],
			      flags, NULL);

	/* GPU2 --> copy from GPU1 */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(copy_dst),
			 &batch_bo[1], &batch_addr[1], flags, INIT);

	/*GPU2: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], to_user_pointer(copy_dst),
			      &batch_addr[1], flags, NULL);

	final_value = READ_ONCE(*(uint32_t *)copy_dst);
	/* verifying copy_dst (GPU2 INIT op) have correct value */
	igt_assert_eq(final_value, BATCH_VALUE);

	if (flags & MULTIGPU_COH_FAIL) {
		struct drm_xe_sync sync0 = {}, sync1 = {};
		uint64_t *result;
		uint64_t coh_result;
		uint64_t *sync_addr0, *sync_addr1;

		igt_info("verifying concurrent write race\n");

		WRITE_ONCE(*(uint64_t *)addr, 0);

		store_dword_batch_init(gpu1->fd, vm[0], addr, &batch1_bo[0],
				       &batch1_addr[0], BATCH_VALUE + 10);
		store_dword_batch_init(gpu2->fd, vm[1], addr, &batch1_bo[1],
				       &batch1_addr[1], BATCH_VALUE + 20);

		/* Setup sync for GPU1 */
		sync_addr0 = (void *)((char *)batch1_addr[0] + SZ_4K);
		sync0.flags = DRM_XE_SYNC_FLAG_SIGNAL;
		sync0.type = DRM_XE_SYNC_TYPE_USER_FENCE;
		sync0.addr = to_user_pointer((uint64_t *)sync_addr0);
		sync0.timeline_value = EXEC_SYNC_VAL;
		WRITE_ONCE(*sync_addr0, 0);

		/* Setup sync for GPU2 */
		sync_addr1 = (void *)((char *)batch1_addr[1] + SZ_4K);
		sync1.flags = DRM_XE_SYNC_FLAG_SIGNAL;
		sync1.type = DRM_XE_SYNC_TYPE_USER_FENCE;
		sync1.addr = to_user_pointer((uint64_t *)sync_addr1);
		sync1.timeline_value = EXEC_SYNC_VAL;
		WRITE_ONCE(*sync_addr1, 0);

		/* Launch both concurrently - no wait between them */
		xe_exec_sync(gpu1->fd, exec_queue[0], batch1_addr[0], &sync0, 1);
		xe_exec_sync(gpu2->fd, exec_queue[1], batch1_addr[1], &sync1, 1);

		/* Wait for both ops to complete */
		if (READ_ONCE(*sync_addr0) != EXEC_SYNC_VAL)
			xe_wait_ufence(gpu1->fd, (uint64_t *)sync_addr0, EXEC_SYNC_VAL,
				       exec_queue[0], NSEC_PER_SEC * 10);
		if (READ_ONCE(*sync_addr1) != EXEC_SYNC_VAL)
			xe_wait_ufence(gpu2->fd, (uint64_t *)sync_addr1, EXEC_SYNC_VAL,
				       exec_queue[1], NSEC_PER_SEC * 10);

		/* Create result buffer for GPU to copy the final value */
		result = aligned_alloc(SZ_2M, SZ_4K);
		igt_assert(result);
		WRITE_ONCE(*result, 0xDEADBEEF); // Initialize with known pattern

		/* GPU2 --> copy from addr */
		gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(result),
				 &batch_bo[1], &batch_addr[1], flags, INIT);

		/*GPU2: Madvise and Prefetch Ops */
		gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], to_user_pointer(result),
				      &batch_addr[1], flags, NULL);

		/* Check which write won (or if we got a mix) */
		coh_result = READ_ONCE(*result);

		if (coh_result == (BATCH_VALUE + 10))
			igt_info("GPU1's write won the race\n");
		else if (coh_result == (BATCH_VALUE + 20))
			igt_info("GPU2's write won the race\n");
		else if (coh_result == 0)
			igt_warn("Both writes failed - coherency issue\n");
		else
			igt_warn("Unexpected value 0x%lx - possible coherency corruption\n",
				 coh_result);

		munmap((void *)batch1_addr[0], BATCH_SIZE(gpu1->fd));
		munmap((void *)batch1_addr[1], BATCH_SIZE(gpu2->fd));

		batch_fini(gpu1->fd, vm[0], batch1_bo[0], batch1_addr[0]);
		batch_fini(gpu2->fd, vm[1], batch1_bo[1], batch1_addr[1]);
		free(result);
	}

	munmap((void *)batch_addr[0], BATCH_SIZE(gpu1->fd));
	munmap((void *)batch_addr[1], BATCH_SIZE(gpu2->fd));
	batch_fini(gpu1->fd, vm[0], batch_bo[0], batch_addr[0]);
	batch_fini(gpu2->fd, vm[1], batch_bo[1], batch_addr[1]);
	free(data1);
	free(copy_dst);

	cleanup_vm_and_queue(gpu1, vm[0], exec_queue[0]);
	cleanup_vm_and_queue(gpu2, vm[1], exec_queue[1]);
}

static void
latency_test_multigpu(struct xe_svm_gpu_info *gpu1,
		      struct xe_svm_gpu_info *gpu2,
		      struct drm_xe_engine_class_instance *eci,
		      unsigned int flags)
{
	uint64_t addr;
	uint32_t vm[2];
	uint32_t exec_queue[2];
	uint32_t batch_bo[2];
	uint8_t *copy_dst;
	uint64_t batch_addr[2];
	struct test_exec_data *data;
	double gpu1_latency, gpu2_latency;
	double gpu1_bw, gpu2_bw;
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

	/* GPU1: Creating batch with predefined value */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
			 &batch_bo[0], &batch_addr[0], flags, DWORD);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr, &batch_addr[0],
			      flags, &gpu1_latency);

	gpu1_bw = (SZ_1K / (gpu1_latency / 1e9)) / (1024.0 * 1024.0); //Written 1k

	igt_info("GPU1 write with %s: Latency %.3f us, Bandwidth %.2f MB/s\n",
		 (flags & MULTIGPU_PREFETCH) ? "prefetch" : "noprefetch",
		 gpu1_latency / 1000.0, gpu1_bw);

	/* Validate GPU1 performance */
	if (flags & MULTIGPU_PREFETCH) {
		if (gpu1_latency / 1000.0 > 5.0)
			igt_warn("GPU1 write with prefetch slower than expected (%.3f us > 5us)\n",
				 gpu1_latency / 1000.0);
	}

	if (flags & MULTIGPU_PERF_REM_COPY) {
		/*GPU2: Copy data from addr (written by GPU1) to its own buffer (copy_dst) */
		gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(copy_dst),
				 &batch_bo[1], &batch_addr[1], flags, INIT);

		/*GPU2: Madvise and Prefetch Ops */
		gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], to_user_pointer(copy_dst),
				      &batch_addr[1], flags, &gpu2_latency);

		gpu2_bw = (SZ_1K / (gpu2_latency / 1e9)) / (1024.0 * 1024.0);
		final_value = *(uint32_t *)copy_dst;
		igt_assert_eq(final_value, BATCH_VALUE);
	} else {
		/* GPU1 --> Creating batch with value and executing STORE op */
		gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
				 &batch_bo[0], &batch_addr[0], flags, DWORD);

		/*GPU1: Madvise and Prefetch Ops */
		gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr,
				      &batch_addr[0], flags, &gpu1_latency);

		/*GPU2: Copy data from addr (written by GPU1) to its own buffer (copy_dst) */
		gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, to_user_pointer(copy_dst),
				 &batch_bo[1], &batch_addr[1], flags, INIT);

		/*GPU2: Madvise and Prefetch Ops */
		gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], to_user_pointer(copy_dst),
				      &batch_addr[1], flags, &gpu2_latency);

		gpu2_latency += gpu1_latency;
		gpu2_bw = (SZ_1K / (gpu2_latency / 1e9)) / (1024.0 * 1024.0);
		final_value = READ_ONCE(*(uint32_t *)copy_dst);
		igt_assert_eq(final_value, BATCH_VALUE);
	}

	igt_info("GPU2 %s copy: Latency %.3f us, Bandwidth %.2f MB/s\n",
		 (flags & MULTIGPU_PERF_REM_COPY) ? "remote" : "local",
		 gpu2_latency / 1000.0, gpu2_bw);

	/* Validate GPU2 performance based on scenario */
	if (flags & MULTIGPU_PERF_REM_COPY) {
		if (flags & MULTIGPU_PREFETCH) {
			/* Remote copy with prefetch: expect 0.2-1us */
			if (gpu2_latency / 1000.0 > 1.0)
				igt_warn("GPU2 remote copy with prefetch slower than expected (%.3f us > 1us)\n",
					 gpu2_latency / 1000.0);
		} else {
			/* Remote copy without prefetch: expect 1-4us */
			if (gpu2_latency / 1000.0 > 10.0)
				igt_warn("GPU2 P2P remote copy is very slow (%.3f us > 10us)\n",
					 gpu2_latency / 1000.0);
		}
	} else {
		if (flags & MULTIGPU_PREFETCH) {
			/* Local write with prefetch: expect 0.1-0.5us */
			if (gpu2_latency / 1000.0 > 1.0)
				igt_warn("GPU2 local write with prefetch slower than expected (%.3f us > 1us)\n",
					 gpu2_latency / 1000.0);
		}
	}

	/* Bandwidth comparison */
	if (gpu2_bw > gpu1_bw)
		igt_info("GPU2 has %.2fx better bandwidth than GPU1\n", gpu2_bw / gpu1_bw);
	else
		igt_info("GPU1 has %.2fx better bandwidth than GPU2\n", gpu1_bw / gpu2_bw);

	/* Overall prefetch effectiveness check */
	if (flags & MULTIGPU_PREFETCH) {
		if ((gpu1_latency / 1000.0) < 5.0 && (gpu2_latency / 1000.0) < 5.0)
			igt_info("Prefetch providing expected performance benefit\n");
		else
			igt_warn("Prefetch not providing expected performance benefit\n");
	}

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
pagefault_test_multigpu(struct xe_svm_gpu_info *gpu1,
			struct xe_svm_gpu_info *gpu2,
			struct drm_xe_engine_class_instance *eci,
			unsigned int flags)
{
	uint64_t addr;
	uint64_t addr1;
	uint32_t vm[2];
	uint32_t exec_queue[2];
	uint32_t batch_bo[2];
	uint64_t batch_addr[2];
	struct drm_xe_sync sync = {};
	uint64_t *sync_addr;
	void *data, *verify_result;
	const char *pf_count_stat = "svm_pagefault_count";
	int pf_count_gpu1_before, pf_count_gpu1_after;
	int pf_count_gpu2_before, pf_count_gpu2_after;
	bool prefetch_req = flags & MULTIGPU_PREFETCH;

	/* Skip if either GPU doesn't support faults */
	if (mgpu_check_fault_support(gpu1, gpu2))
		return;

	create_vm_and_queue(gpu1, eci, &vm[0], &exec_queue[0]);
	create_vm_and_queue(gpu2, eci, &vm[1], &exec_queue[1]);

	data = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(data);
	memset(data, 0, SZ_4K);
	addr = to_user_pointer(data);

	/* Allocate verification buffer for GPU2 to copy into */
	verify_result = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(verify_result);
	addr1 = to_user_pointer(verify_result);

	/* === Phase 1: GPU1 writes to addr === */
	pf_count_gpu1_before = xe_gt_stats_get_count(gpu1->fd, eci->gt_id, pf_count_stat);

	/* GPU1 --> Creating batch with value and executing STORE op */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
			 &batch_bo[0], &batch_addr[0], flags, DWORD);

	/*GPU1: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu1, gpu2, vm[0], exec_queue[0], addr, &batch_addr[0],
			      flags, NULL);

	pf_count_gpu1_after = xe_gt_stats_get_count(gpu1->fd, eci->gt_id, pf_count_stat);

	if (prefetch_req) {
		/* With prefetch: expect NO page faults */
		igt_assert_eq(pf_count_gpu1_after, pf_count_gpu1_before);
		igt_info("GPU1 write with prefetch: No page faults (as expected)\n");
	} else {
		/* Without prefetch: expect page faults */
		igt_debug("Pagefault count %s\n",
			  pf_count_gpu1_after > pf_count_gpu1_before
			  ? "increased"
			  : "not increased");
		igt_info("GPU1 write without prefetch: %d page faults\n",
			 pf_count_gpu1_after - pf_count_gpu1_before);
	}

	/* === Phase 2: GPU2 reads from addr (cross-GPU access) === */
	pf_count_gpu2_before = xe_gt_stats_get_count(gpu2->fd, eci->gt_id, pf_count_stat);

	/* GPU2 --> Create batch for GPU2 to copy from addr (GPU1's memory) to verify_result */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, addr1,
			 &batch_bo[1], &batch_addr[1], flags, INIT);

	/* Prefetch src buffer (addr) to avoid page faults */
	xe_multigpu_madvise(gpu2->fd, vm[1], addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
			    gpu2->fd, 0, gpu2->vram_regions[0], exec_queue[1]);

	setup_sync(&sync, &sync_addr, BIND_SYNC_VAL);
	xe_multigpu_prefetch(gpu2->fd, vm[1], addr, SZ_4K, &sync,
			     sync_addr, exec_queue[1], flags);

	free(sync_addr);

	/*GPU2: Madvise and Prefetch Ops */
	gpu_madvise_exec_sync(gpu2, gpu1, vm[1], exec_queue[1], addr1, &batch_addr[1],
			      flags, NULL);

	pf_count_gpu2_after = xe_gt_stats_get_count(gpu2->fd, eci->gt_id, pf_count_stat);

	if (prefetch_req) {
		/* With prefetch: expect NO page faults on GPU2 */
		igt_assert_eq(pf_count_gpu2_after, pf_count_gpu2_before);
		igt_info("GPU2 cross-GPU read with prefetch: No page faults (as expected)\n");
	} else {
		/* Without prefetch: expect cross-GPU page faults */
		igt_debug("Pagefault count %s\n",
			  pf_count_gpu2_after > pf_count_gpu2_before
			  ? "increased"
			  : "not increased");
		igt_info("GPU2 cross-GPU read without prefetch: %d page faults\n",
			 pf_count_gpu2_after - pf_count_gpu2_before);
	}

	munmap((void *)batch_addr[0], BATCH_SIZE(gpu1->fd));
	munmap((void *)batch_addr[1], BATCH_SIZE(gpu2->fd));
	batch_fini(gpu1->fd, vm[0], batch_bo[0], batch_addr[0]);
	batch_fini(gpu2->fd, vm[1], batch_bo[1], batch_addr[0]);
	free(data);
	free(verify_result);

	cleanup_vm_and_queue(gpu1, vm[0], exec_queue[0]);
	cleanup_vm_and_queue(gpu2, vm[1], exec_queue[1]);
}

static void
multigpu_access_test(struct xe_svm_gpu_info *gpu1,
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
	struct drm_xe_sync sync[2] = {};
	uint64_t *sync_addr[2];
	uint32_t verify_batch_bo;
	uint64_t verify_batch_addr;
	uint64_t *verify_result;
	uint32_t final_value;
	uint64_t final_timeline;

	/* Skip if either GPU doesn't support faults */
	if (mgpu_check_fault_support(gpu1, gpu2))
		return;

	create_vm_and_queue(gpu1, eci, &vm[0], &exec_queue[0]);
	create_vm_and_queue(gpu2, eci, &vm[1], &exec_queue[1]);

	data = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(data);
	data[0].vm_sync = 0;
	addr = to_user_pointer(data);

	WRITE_ONCE(*(uint64_t *)addr, 0);

	/* GPU1: Atomic Batch create */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, 0,
			 &batch_bo[0], &batch_addr[0], flags, ATOMIC);
	/* GPU2: Atomic Batch create */
	gpu_batch_create(gpu2, vm[1], exec_queue[1], addr, 0,
			 &batch_bo[1], &batch_addr[1], flags, ATOMIC);

	/* gpu_madvise_sync calls xe_exec() also, here intention is different */
	xe_multigpu_madvise(gpu1->fd, vm[0], addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_ATOMIC,
			    DRM_XE_ATOMIC_GLOBAL, 0, 0, exec_queue[0]);

	xe_multigpu_madvise(gpu1->fd, vm[0], addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
			    gpu1->fd, 0, gpu1->vram_regions[0], exec_queue[0]);

	xe_multigpu_madvise(gpu2->fd, vm[1], addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_ATOMIC,
			    DRM_XE_ATOMIC_GLOBAL, 0, 0, exec_queue[1]);

	xe_multigpu_madvise(gpu2->fd, vm[1], addr, SZ_4K, 0,
			    DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC,
			    gpu2->fd, 0, gpu2->vram_regions[0], exec_queue[1]);

	setup_sync(&sync[0], &sync_addr[0], BIND_SYNC_VAL);
	setup_sync(&sync[1], &sync_addr[1], BIND_SYNC_VAL);

	xe_multigpu_prefetch(gpu1->fd, vm[0], addr, SZ_4K, &sync[0],
			     sync_addr[0], exec_queue[0], flags);

	xe_multigpu_prefetch(gpu2->fd, vm[1], addr, SZ_4K, &sync[1],
			     sync_addr[1], exec_queue[1], flags);

	free(sync_addr[0]);
	free(sync_addr[1]);

	igt_info("Starting %d concurrent atomic increment iterations\n", NUM_ITER);
	for (int i = 0; i < NUM_ITER; i++) {
		bool last = (i == NUM_ITER - 1);

		if (last) {
			sync_addr[0] = (void *)((char *)batch_addr[0] + SZ_4K);
			sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
			sync[0].type = DRM_XE_SYNC_TYPE_USER_FENCE;
			sync[0].addr = to_user_pointer((uint64_t *)sync_addr[0]);
			sync[0].timeline_value = EXEC_SYNC_VAL + i;
			WRITE_ONCE(*sync_addr[0], 0);

			sync_addr[1] = (void *)((char *)batch_addr[1] + SZ_4K);
			sync[1].flags = DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].type = DRM_XE_SYNC_TYPE_USER_FENCE;
			sync[1].addr = to_user_pointer((uint64_t *)sync_addr[1]);
			sync[1].timeline_value = EXEC_SYNC_VAL + i;
			WRITE_ONCE(*sync_addr[1], 0);
		}

		/* === CONCURRENT EXECUTION: Launch both GPUs simultaneously === */
		xe_exec_sync(gpu1->fd, exec_queue[0], batch_addr[0],
			     last ? &sync[0] : NULL, last ? 1 : 0);

		xe_exec_sync(gpu2->fd, exec_queue[1], batch_addr[1],
			     last ? &sync[1] : NULL, last ? 1 : 0);
	}

	 /* NOW wait only for the last operations to complete */
	final_timeline = EXEC_SYNC_VAL + NUM_ITER - 1;
	if (NUM_ITER > 0) {
		if (READ_ONCE(*sync_addr[0]) != final_timeline)
			xe_wait_ufence(gpu1->fd, (uint64_t *)sync_addr[0], final_timeline,
				       exec_queue[0], NSEC_PER_SEC * 30);

		if (READ_ONCE(*sync_addr[1]) != final_timeline)
			xe_wait_ufence(gpu2->fd, (uint64_t *)sync_addr[1], final_timeline,
				       exec_queue[1], NSEC_PER_SEC * 30);
	}

	igt_info("Both GPUs completed execution %u\n", READ_ONCE(*(uint32_t *)addr));

	/* === Verification using GPU read (not CPU) === */
	verify_result = aligned_alloc(SZ_2M, SZ_4K);
	igt_assert(verify_result);
	memset(verify_result, 0xDE, SZ_4K);

	/* Use GPU1 to read final value */
	gpu_batch_create(gpu1, vm[0], exec_queue[0], addr, to_user_pointer(verify_result),
			 &verify_batch_bo, &verify_batch_addr, flags, INIT);

	sync_addr[0] = (void *)((char *)verify_batch_addr + SZ_4K);
	sync[0].addr = to_user_pointer((uint64_t *)sync_addr[0]);
	sync[0].timeline_value = EXEC_SYNC_VAL;
	sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	sync[0].type = DRM_XE_SYNC_TYPE_USER_FENCE;
	WRITE_ONCE(*sync_addr[0], 0);

	xe_exec_sync(gpu1->fd, exec_queue[0], verify_batch_addr, &sync[0], 1);
	if (READ_ONCE(*sync_addr[0]) != EXEC_SYNC_VAL)
		xe_wait_ufence(gpu1->fd, (uint64_t *)sync_addr[0], EXEC_SYNC_VAL,
			       exec_queue[0], NSEC_PER_SEC * 10);

	/* NOW CPU can read verify_result */
	final_value = READ_ONCE(*(uint32_t *)verify_result);

	igt_info("GPU verification batch copied value: %u\n", final_value);
	igt_info("CPU direct read shows: %u\n", (unsigned int)*(uint64_t *)addr);

	/* Expected: 0 + (NUM_ITER * 2 GPUs) = 400 */
	igt_assert_f((final_value == 2 * NUM_ITER),
		     "Expected %u value, got %u\n",
		     2 * NUM_ITER, final_value);

	munmap((void *)verify_batch_addr, BATCH_SIZE(gpu1->fd));
	batch_fini(gpu1->fd, vm[0], verify_batch_bo, verify_batch_addr);
	free(verify_result);

	munmap((void *)batch_addr[0], BATCH_SIZE(gpu1->fd));
	munmap((void *)batch_addr[1], BATCH_SIZE(gpu2->fd));
	batch_fini(gpu1->fd, vm[0], batch_bo[0], batch_addr[0]);
	batch_fini(gpu2->fd, vm[1], batch_bo[1], batch_addr[1]);
	free(data);

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
gpu_coherecy_test_wrapper(struct xe_svm_gpu_info *src,
			  struct xe_svm_gpu_info *dst,
			  struct drm_xe_engine_class_instance *eci,
			  unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	coherency_test_multigpu(src, dst, eci, flags);
}

static void
gpu_latency_test_wrapper(struct xe_svm_gpu_info *src,
			 struct xe_svm_gpu_info *dst,
			 struct drm_xe_engine_class_instance *eci,
			 unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	latency_test_multigpu(src, dst, eci, flags);
}

static void
gpu_fault_test_wrapper(struct xe_svm_gpu_info *src,
		       struct xe_svm_gpu_info *dst,
		       struct drm_xe_engine_class_instance *eci,
		       unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	pagefault_test_multigpu(src, dst, eci, flags);
}

static void
gpu_simult_test_wrapper(struct xe_svm_gpu_info *src,
			struct xe_svm_gpu_info *dst,
			struct drm_xe_engine_class_instance *eci,
			unsigned int flags)
{
	igt_assert(src);
	igt_assert(dst);

	multigpu_access_test(src, dst, eci, flags);
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
	if (flags & MULTIGPU_COH_OP)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_coherecy_test_wrapper, flags);
	if (flags & MULTIGPU_PERF_OP)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_latency_test_wrapper, flags);
	if (flags & MULTIGPU_PFAULT_OP)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_fault_test_wrapper, flags);
	if (flags & MULTIGPU_CONC_ACCESS)
		for_each_gpu_pair(gpu_cnt, gpus, eci, gpu_simult_test_wrapper, flags);
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
		{ "atomic-op-conflict", MULTIGPU_CONFLICT | MULTIGPU_ATOMIC_OP },
		{ "coherency-basic", MULTIGPU_COH_OP },
		{ "coherency-fail-basic", MULTIGPU_COH_OP | MULTIGPU_COH_FAIL },
		{ "coherency-prefetch", MULTIGPU_PREFETCH | MULTIGPU_COH_OP },
		{ "coherency-conflict", MULTIGPU_CONFLICT | MULTIGPU_COH_OP },
		{ "coherency-fail-prefetch",
		  MULTIGPU_PREFETCH | MULTIGPU_COH_OP | MULTIGPU_COH_FAIL},
		{ "latency-basic", MULTIGPU_PERF_OP },
		{ "latency-copy-basic",
		  MULTIGPU_PERF_OP | MULTIGPU_PERF_REM_COPY },
		{ "latency-prefetch", MULTIGPU_PREFETCH | MULTIGPU_PERF_OP },
		{ "latency-copy-prefetch",
		  MULTIGPU_PREFETCH | MULTIGPU_PERF_OP | MULTIGPU_PERF_REM_COPY },
		{ "pagefault-basic", MULTIGPU_PFAULT_OP },
		{ "pagefault-prefetch", MULTIGPU_PREFETCH | MULTIGPU_PFAULT_OP },
		{ "pagefault-conflict", MULTIGPU_CONFLICT | MULTIGPU_PFAULT_OP },
		{ "concurrent-access-basic", MULTIGPU_CONC_ACCESS },
		{ "concurrent-access-prefetch", MULTIGPU_PREFETCH | MULTIGPU_CONC_ACCESS },
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
