// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check VMA eviction
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: VMA
 * Functionality: eviction
 * GPU requirements: GPU needs to have dedicated VRAM
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <string.h>

#define XE_EXEC_QUEUE_PRIORITY_NORMAL   1
#define MAX_N_EXEC_QUEUES	16
#define MULTI_VM			(0x1 << 0)
#define THREADED			(0x1 << 1)
#define MIXED_THREADS		(0x1 << 2)
#define LEGACY_THREAD		(0x1 << 3)
#define COMPUTE_THREAD		(0x1 << 4)
#define EXTERNAL_OBJ		(0x1 << 5)
#define BIND_EXEC_QUEUE		(0x1 << 6)
#define MULTI_QUEUE		(0x1 << 7)
#define PRIORITY		(0x1 << 8)

static void
test_evict(int fd, struct drm_xe_engine_class_instance *eci,
	   int n_exec_queues, int n_execs, size_t bo_size,
	   unsigned long flags, pthread_barrier_t *barrier)
{
	uint32_t vm, vm2, vm3;
	uint32_t bind_exec_queues[3] = { 0, 0, 0 };
	uint64_t addr = 0x100000000, base_addr = 0x100000000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	uint32_t *bo;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	bo = calloc(n_execs / 2, sizeof(*bo));
	igt_assert(bo);

	fd = drm_reopen_driver(fd);

	vm = xe_vm_create(fd, 0, 0);
	if (flags & BIND_EXEC_QUEUE)
		bind_exec_queues[0] = xe_bind_exec_queue_create(fd, vm, 0);
	if (flags & MULTI_VM) {
		vm2 = xe_vm_create(fd, 0, 0);
		vm3 = xe_vm_create(fd, 0, 0);
		if (flags & BIND_EXEC_QUEUE) {
			bind_exec_queues[1] = xe_bind_exec_queue_create(fd, vm2, 0);
			bind_exec_queues[2] = xe_bind_exec_queue_create(fd, vm3, 0);
		}
	}

	for (i = 0; i < n_exec_queues; i++) {
		if (flags & MULTI_VM)
			exec_queues[i] = xe_exec_queue_create(fd, i & 1 ? vm2 : vm ,
						      eci, 0);
		else if (flags & MULTI_QUEUE) {
			struct drm_xe_ext_set_property multi_queue = {
				.base.next_extension = 0,
				.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
				.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
			};
			uint64_t ext = to_user_pointer(&multi_queue);

			if (flags & PRIORITY) {
				struct drm_xe_ext_set_property mq_priority = {
					.base.next_extension = 0,
					.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
					.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY,
				};
				multi_queue.base.next_extension = to_user_pointer(&mq_priority);
				mq_priority.value = XE_EXEC_QUEUE_PRIORITY_NORMAL + (rand() % 2);
			}
			multi_queue.value = i ? exec_queues[0] : DRM_XE_MULTI_GROUP_CREATE;
			igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci,
							     ext, &exec_queues[i]), 0);
		} else
			exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint32_t __bo;
		int e = i % n_exec_queues;

		if (i < n_execs / 2) {
                        uint32_t _vm = (flags & EXTERNAL_OBJ) &&
                                i < n_execs / 8 ? 0 : vm;

			igt_assert((e & 1) == (i & 1));
			if (flags & MULTI_VM) {
				__bo = bo[i] = xe_bo_create(fd, 0,
							    bo_size,
							    vram_memory(fd, eci->gt_id),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			} else if (flags & THREADED) {
				__bo = bo[i] = xe_bo_create(fd, vm,
							    bo_size,
							    vram_memory(fd, eci->gt_id),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			} else {
				__bo = bo[i] = xe_bo_create(fd, _vm,
							    bo_size,
							    vram_memory(fd, eci->gt_id) |
							    system_memory(fd),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			}
		} else {
			igt_assert((e & 1) == ((i % (n_execs / 2)) & 1));
			__bo = bo[i % (n_execs / 2)];
		}
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));

		if (i < n_execs / 2) {
			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			sync[0].handle = syncobj_create(fd, 0);
			if (flags & MULTI_VM) {
				xe_vm_bind_async(fd, vm3, bind_exec_queues[2], __bo,
						 0, addr,
						 bo_size, sync, 1);
				igt_assert(syncobj_wait(fd, &sync[0].handle, 1,
							INT64_MAX, 0, NULL));
				xe_vm_bind_async(fd, i & 1 ? vm2 : vm,
						 i & 1 ? bind_exec_queues[1] :
						 bind_exec_queues[0], __bo,
						 0, addr, bo_size, sync, 1);
			} else {
				xe_vm_bind_async(fd, vm, bind_exec_queues[0],
						 __bo, 0, addr, bo_size,
						 sync, 1);
			}
		}
		addr += bo_size;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		if (i >= n_exec_queues)
			syncobj_reset(fd, &syncobjs[e], 1);
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);

		if (i + 1 == n_execs / 2) {
			addr = base_addr;
			exec.num_syncs = 1;
			exec.syncs = to_user_pointer(sync + 1);
			if (barrier)
				pthread_barrier_wait(barrier);
		}
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_exec_queues; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++) {
		uint32_t __bo;

		__bo = bo[i % (n_execs / 2)];
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		igt_assert_eq(data[i].data, 0xc0ffee);
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	for (i = 0; i < 3; i++)
		if (bind_exec_queues[i])
			xe_exec_queue_destroy(fd, bind_exec_queues[i]);

	for (i = 0; i < n_execs / 2; i++)
		gem_close(fd, bo[i]);

	xe_vm_destroy(fd, vm);
	if (flags & MULTI_VM) {
		xe_vm_destroy(fd, vm2);
		xe_vm_destroy(fd, vm3);
	}
	drm_close_driver(fd);
}

static void
test_evict_cm(int fd, struct drm_xe_engine_class_instance *eci,
	      int n_exec_queues, int n_execs, size_t bo_size, unsigned long flags,
	      pthread_barrier_t *barrier)
{
	uint32_t vm, vm2;
	uint32_t bind_exec_queues[2] = { 0, 0 };
	uint64_t addr = 0x100000000, base_addr = 0x100000000;
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t *bo;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
		uint64_t vm_sync;
		uint64_t exec_sync;
	} *data;
	int i, b;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	bo = calloc(n_execs / 2, sizeof(*bo));
	igt_assert(bo);

	fd = drm_reopen_driver(fd);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	if (flags & BIND_EXEC_QUEUE)
		bind_exec_queues[0] = xe_bind_exec_queue_create(fd, vm, 0);
	if (flags & MULTI_VM) {
		vm2 = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
		if (flags & BIND_EXEC_QUEUE)
			bind_exec_queues[1] = xe_bind_exec_queue_create(fd, vm2, 0);
	}

	for (i = 0; i < n_exec_queues; i++) {
		if (flags & MULTI_VM)
			exec_queues[i] = xe_exec_queue_create(fd, i & 1 ? vm2 :
							      vm, eci, 0);
		else if (flags & MULTI_QUEUE) {
			struct drm_xe_ext_set_property multi_queue = {
				.base.next_extension = 0,
				.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
				.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
			};
			uint64_t ext = to_user_pointer(&multi_queue);

			if (flags & PRIORITY) {
				struct drm_xe_ext_set_property mq_priority = {
					.base.next_extension = 0,
					.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
					.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY,
				};
				multi_queue.base.next_extension = to_user_pointer(&mq_priority);
				mq_priority.value = XE_EXEC_QUEUE_PRIORITY_NORMAL + (rand() % 2);
			}
			multi_queue.value = i ? exec_queues[0] : DRM_XE_MULTI_GROUP_CREATE;
			igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci,
							     ext, &exec_queues[i]), 0);
		} else
			exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
	}

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint32_t __bo;
		int e = i % n_exec_queues;

		if (i < n_execs / 2) {
                        uint32_t _vm = (flags & EXTERNAL_OBJ) &&
                                i < n_execs / 8 ? 0 : vm;

			igt_assert((e & 1) == (i & 1));
			if (flags & MULTI_VM) {
				__bo = bo[i] = xe_bo_create(fd, 0,
							    bo_size,
							    vram_memory(fd, eci->gt_id),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			} else if (flags & THREADED) {
				__bo = bo[i] = xe_bo_create(fd, vm,
							    bo_size,
							    vram_memory(fd, eci->gt_id),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			} else {
				__bo = bo[i] = xe_bo_create(fd, _vm,
							    bo_size,
							    vram_memory(fd, eci->gt_id) |
							    system_memory(fd),
							    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			}
		} else {
			igt_assert((e & 1) == ((i % (n_execs / 2)) & 1));
			__bo = bo[i % (n_execs / 2)];
		}
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		if (i < n_execs / 2)
			memset(data, 0, ALIGN(sizeof(*data) * n_execs, 0x1000));

		if (i < n_execs / 2) {
			sync[0].addr = to_user_pointer(&data[i].vm_sync);
			if (flags & MULTI_VM) {
				xe_vm_bind_async(fd, i & 1 ? vm2 : vm,
						 i & 1 ? bind_exec_queues[1] :
						 bind_exec_queues[0], __bo,
						 0, addr, bo_size, sync, 1);
			} else {
				xe_vm_bind_async(fd, vm, bind_exec_queues[0], __bo,
						 0, addr, bo_size, sync, 1);
			}
			xe_wait_ufence(fd, &data[i].vm_sync, USER_FENCE_VALUE,
				       bind_exec_queues[0], 20 * NSEC_PER_SEC);
		}
		sync[0].addr = addr + (char *)&data[i].exec_sync -
			(char *)data;
		addr += bo_size;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);

		if (i + 1 == n_execs / 2) {
			addr = base_addr;
			if (barrier)
				pthread_barrier_wait(barrier);
		}
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_execs; i++) {
		uint32_t __bo;

		__bo = bo[i % (n_execs / 2)];
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
			       exec_queues[i % n_exec_queues], 20 * NSEC_PER_SEC);
		igt_assert_eq(data[i].data, 0xc0ffee);
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_exec_queues; i++)
		xe_exec_queue_destroy(fd, exec_queues[i]);

	for (i = 0; i < 2; i++)
		if (bind_exec_queues[i])
			xe_exec_queue_destroy(fd, bind_exec_queues[i]);

	for (i = 0; i < n_execs / 2; i++)
		gem_close(fd, bo[i]);

	xe_vm_destroy(fd, vm);
	if (flags & MULTI_VM)
		xe_vm_destroy(fd, vm2);
	drm_close_driver(fd);
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	pthread_barrier_t *barrier;
	int fd;
	struct drm_xe_engine_class_instance *eci;
	int n_exec_queues;
	int n_execs;
	uint64_t bo_size;
	int flags;
	bool *go;
};

static void *thread(void *data)
{
	struct thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->flags & COMPUTE_THREAD)
		test_evict_cm(t->fd, t->eci, t->n_exec_queues, t->n_execs,
			      t->bo_size, t->flags, t->barrier);
	else
		test_evict(t->fd, t->eci, t->n_exec_queues, t->n_execs,
			   t->bo_size, t->flags, t->barrier);

	return NULL;
}

static void
threads(int fd, struct drm_xe_engine_class_instance *eci,
	int n_threads, int n_exec_queues, int n_execs, size_t bo_size,
	unsigned long flags)
{
	pthread_barrier_t barrier;
	bool go = false;
	struct thread_data *threads_data;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int i;

	threads_data = calloc(n_threads, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);
	pthread_barrier_init(&barrier, NULL, n_threads);

	for (i = 0; i < n_threads; ++i) {
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
		threads_data[i].barrier = &barrier;
		threads_data[i].fd = fd;
		threads_data[i].eci = eci;
		threads_data[i].n_exec_queues = n_exec_queues;
		threads_data[i].n_execs = n_execs;
		threads_data[i].bo_size = bo_size;
		threads_data[i].flags = flags;
		if ((i & 1 && flags & MIXED_THREADS) || flags & COMPUTE_THREAD)
			threads_data[i].flags |= COMPUTE_THREAD;
		else
			threads_data[i].flags |= LEGACY_THREAD;
		threads_data[i].go = &go;

		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
	}

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_threads; ++i)
		pthread_join(threads_data[i].thread, NULL);
}

static uint64_t calc_bo_size(uint64_t vram_size, int mul, int div)
{
	if (vram_size >= SZ_1G)
		return (ALIGN(vram_size, SZ_1G)  * mul) / div;
	else
		return (ALIGN(vram_size, SZ_256M)  * mul) / div; /* small-bar */
}

static unsigned int working_set(uint64_t vram_size, uint64_t system_size,
				uint64_t bo_size, unsigned int num_threads,
				unsigned int flags)
{
	uint64_t set_size;
	uint64_t total_size;

	igt_assert(vram_size > 0);

	set_size = (vram_size - 1) / bo_size;

	/*
	 * Working set resides also in system?
	 * Currently system graphics memory is limited to 50% of total.
	 */
	if (!(flags & (THREADED | MULTI_VM)))
		set_size += (system_size / 2) / bo_size;

	/* Set sizes are per vm. In the multi-vm case we use 2 vms. */
	if (flags & MULTI_VM)
		set_size *= 2;

	/*
	 * All bos must fit in, say 4 / 5 of memory to be sure.
	 * Assume no swap-space available. Subtract one bo per thread
	 * for an active eviction.
	 */
	total_size = ((vram_size - 1) / bo_size + system_size * 4 / 5 / bo_size) /
		num_threads - 1;

	igt_debug("num_threads: %d bo_size : %"PRIu64" total_size : %"PRIu64"\n", num_threads,
		  bo_size, total_size);

	if (set_size > total_size)
		set_size = total_size;

	/* bos are only created on half of the execs. */
	set_size *= 2;

	/*
	 * Align down to ensure the vm the bo is bound to matches the vm
	 * used by the exec_queue, fulfilling the asserts in the
	 * tests.
	 */
	return ALIGN_DOWN(set_size, 4);
}

/**
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @small:			small
 * @small-external:		small external
 * @small-multi-vm:		small multi VM
 * @beng-small:			small bind exec_queue
 * @beng-small-external:	small external bind exec_queue
 * @beng-small-multi-vm:	small multi VM bind ending
 */
/**
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Test category: stress test
 *
 * arg[1]:
 *
 * @large:			large
 * @large-external:		large external
 * @large-multi-vm:		large multi VM
 * @beng-large:			large bind exec_queue
 * @beng-large-external:	large external bind exec_queue
 * @beng-large-multi-vm:	large multi VM bind exec_queue
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: compute machine
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @small-cm:			small compute machine
 * @small-external-cm:		small external compute machine
 * @small-multi-vm-cm:		small multi VM compute machine
 * @beng-small-cm:		small bind exec_queue compute machine
 * @beng-small-external-cm:	small external bind exec_queue compute machine
 * @beng-small-multi-vm-cm:	small multi VM bind ending compute machine
 */
/**
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: compute machine
 * Test category: stress test
 *
 * arg[1]:
 *
 * @large-cm:			large compute machine
 * @large-external-cm:		large external compute machine
 * @large-multi-vm-cm:		large multi VM compute machine
 * @beng-large-cm:		large bind exec_queue compute machine
 * @beng-large-external-cm:	large external bind exec_queue compute machine
 * @beng-large-multi-vm-cm:	large multi VM bind exec_queue compute machine
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: mixted threads
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @mixed-threads-small:	mixed threads small
 * @mixed-many-threads-small:	mixed many threads small
 * @mixed-threads-small-multi-vm:
 * 				mixed threads small multi vm
 * @beng-mixed-threads-small:	bind exec_queue mixed threads small
 * @beng-mixed-many-threads-small:
 *				bind exec_queue mixed many threads small
 * @beng-mixed-threads-small-multi-vm:
 *				bind exec_queue mixed threads small multi vm
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: mixted threads
 * Test category: stress test
 *
 * arg[1]:
 *
 * @beng-mixed-threads-large-multi-vm:
 *				bind exec_queue mixed threads large multi vm
 * @mixed-threads-large:	mixed threads large
 * @mixed-many-threads-large:	mixed many threads large
 * @mixed-threads-large-multi-vm:
 *				mixed threads large multi vm
 * @beng-mixed-threads-large:	bind exec_queue mixed threads large
 * @beng-mixed-many-threads-large:
 *				bind exec_queue mixed many threads large
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: compute mode threads
 * Test category: functionality test
 *
 * arg[1]:
 * @cm-threads-small:		compute mode threads small
 * @cm-threads-small-multi-vm:	compute mode threads small multi vm
 * @beng-cm-threads-small:	bind exec_queue compute mode threads small
 * @beng-cm-threads-small-multi-vm:
 *				bind exec_queue compute mode threads small multi vm
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: compute mode threads
 * Test category: stress test
 *
 * arg[1]:
 * @cm-threads-large:		compute mode threads large
 * @cm-threads-large-multi-vm:	compute mode threads large multi vm
 * @beng-cm-threads-large:	bind exec_queue compute mode threads large
 * @beng-cm-threads-large-multi-vm:
 *				bind exec_queue compute mode threads large multi vm
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: threads
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @threads-small:		threads small
 * @beng-threads-small:		bind exec_queue threads small
 * @threads-small-multi-vm:	threads small multi vm
 * @beng-threads-small-multi-vm:
 *				bind exec_queue threads small multi vm
 *
 */
/**
 *
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Feature: threads
 * Test category: stress test
 *
 * arg[1]:
 *
 * @threads-large:		threads large
 * @threads-large-multi-vm:	threads large multi vm
 * @beng-threads-large-multi-vm:
 *				bind exec_queue threads large multi vm
 * @beng-threads-large:		bind exec_queue threads large
 *
 */
/**
 * SUBTEST: evict-%s
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description:  %arg[1] evict test.
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @small-multi-queue:		small multi queue
 * @small-multi-queue-priority:	small multi queue priority
 * @small-external-multi-queue: small external multi queue
 *
 * SUBTEST: evict-%s
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description:  %arg[1] evict test.
 * Feature: compute machine
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @small-multi-queue-cm:	small multi queue compute machine
 * @small-multi-queue-priority-cm:	small multi queue priority compute machine
 * @small-external-multi-queue-cm:	small external compute machine multi queue
 *
 * SUBTEST: evict-mixed-threads-small-multi-queue
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description:  mixed threads small multi queue evict test.
 * Feature: mixed threads
 * Test category: functionality test
 *
 * SUBTEST: evict-cm-threads-small-multi-queue
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description:  compute mode threads small multi queue evict test.
 * Feature: compute mode threads
 * Test category: functionality test
 *
 * SUBTEST: evict-threads-small-multi-queue
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description:  threads small multi queue evict test.
 * Feature: threads
 * Test category: functionality test
 *
 */

/*
 * Table driven test that attempts to cover all possible scenarios of eviction
 * (small / large objects, compute mode vs non-compute VMs, external BO or BOs
 * tied to VM, multiple VMs using over 51% of the VRAM, evicting BOs from your
 * own VM, and using a user bind or kernel VM engine to do the binds). All of
 * these options are attempted to be mixed via different table entries. Single
 * threaded sections exists for both compute and non-compute VMs, and thread
 * sections exists which cover multiple compute VM, multiple non-compute VMs,
 * and mixing of VMs.
 */
int igt_main()
{
	struct drm_xe_engine_class_instance *hwe, *multi_queue_hwe = NULL;
	const struct section {
		const char *name;
		int n_exec_queues;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections[] = {
		{ "small", 16, 448, 1, 128, 0 },
		{ "small-multi-queue", 16, 448, 1, 128, MULTI_QUEUE },
		{ "small-multi-queue-priority", 16, 448, 1, 128, MULTI_QUEUE | PRIORITY },
		{ "small-external", 16, 448, 1, 128, EXTERNAL_OBJ },
		{ "small-external-multi-queue", 16, 448, 1, 128, EXTERNAL_OBJ | MULTI_QUEUE },
		{ "small-multi-vm", 16, 256, 1, 128, MULTI_VM },
		{ "large", 4, 16, 1, 4, 0 },
		{ "large-external", 4, 16, 1, 4, EXTERNAL_OBJ },
		{ "large-multi-vm", 4, 8, 3, 8, MULTI_VM },
		{ "beng-small", 16, 448, 1, 128, BIND_EXEC_QUEUE },
		{ "beng-small-external", 16, 448, 1, 128, BIND_EXEC_QUEUE |
			EXTERNAL_OBJ },
		{ "beng-small-multi-vm", 16, 256, 1, 128, BIND_EXEC_QUEUE |
			MULTI_VM },
		{ "beng-large", 4, 16, 1, 4, BIND_EXEC_QUEUE },
		{ "beng-large-external", 4, 16, 1, 4, BIND_EXEC_QUEUE |
			EXTERNAL_OBJ },
		{ "beng-large-multi-vm", 4, 8, 3, 8, BIND_EXEC_QUEUE | MULTI_VM },
		{ NULL },
	};
	const struct section_cm {
		const char *name;
		int n_exec_queues;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections_cm[] = {
		{ "small-cm", 16, 448, 1, 128, 0 },
		{ "small-multi-queue-cm", 16, 448, 1, 128, MULTI_QUEUE },
		{ "small-multi-queue-priority-cm", 16, 448, 1, 128, MULTI_QUEUE | PRIORITY },
		{ "small-external-cm", 16, 448, 1, 128, EXTERNAL_OBJ },
		{ "small-external-multi-queue-cm", 16, 448, 1, 128, EXTERNAL_OBJ | MULTI_QUEUE },
		{ "small-multi-vm-cm", 16, 256, 1, 128, MULTI_VM },
		{ "large-cm", 4, 16, 1, 4, 0 },
		{ "large-external-cm", 4, 16, 1, 4, EXTERNAL_OBJ },
		{ "large-multi-vm-cm", 4, 8, 3, 8, MULTI_VM },
		{ "beng-small-cm", 16, 448, 1, 128, BIND_EXEC_QUEUE },
		{ "beng-small-external-cm", 16, 448, 1, 128, BIND_EXEC_QUEUE |
			EXTERNAL_OBJ },
		{ "beng-small-multi-vm-cm", 16, 256, 1, 128, BIND_EXEC_QUEUE |
			MULTI_VM },
		{ "beng-large-cm", 4, 16, 1, 4, BIND_EXEC_QUEUE },
		{ "beng-large-external-cm", 4, 16, 1, 4, BIND_EXEC_QUEUE |
			EXTERNAL_OBJ },
		{ "beng-large-multi-vm-cm", 4, 8, 3, 8, BIND_EXEC_QUEUE |
			MULTI_VM },
		{ NULL },
	};
	const struct section_threads {
		const char *name;
		int n_threads;
		int n_exec_queues;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections_threads[] = {
		{ "threads-small", 2, 16, 128, 1, 128,
			THREADED },
		{ "threads-small-multi-queue", 2, 16, 128, 1, 128,
			THREADED | MULTI_QUEUE },
		{ "cm-threads-small", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | THREADED },
		{ "cm-threads-small-multi-queue", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | THREADED | MULTI_QUEUE },
		{ "mixed-threads-small", 2, 16, 128, 1, 128,
			MIXED_THREADS | THREADED },
		{ "mixed-threads-small-multi-queue", 2, 16, 128, 1, 128,
			MIXED_THREADS | THREADED | MULTI_QUEUE },
		{ "mixed-many-threads-small", 3, 16, 128, 1, 128,
			THREADED },
		{ "threads-large", 2, 2, 16, 3, 32,
			THREADED },
		{ "cm-threads-large", 2, 2, 16, 3, 32,
			COMPUTE_THREAD | THREADED },
		{ "mixed-threads-large", 2, 2, 16, 3, 32,
			MIXED_THREADS | THREADED },
		{ "mixed-many-threads-large", 3, 2, 16, 3, 32,
			THREADED },
		{ "threads-small-multi-vm", 2, 16, 128, 1, 128,
			MULTI_VM | THREADED },
		{ "cm-threads-small-multi-vm", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | MULTI_VM | THREADED },
		{ "mixed-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MIXED_THREADS | MULTI_VM | THREADED },
		{ "threads-large-multi-vm", 2, 2, 16, 3, 32,
			MULTI_VM | THREADED },
		{ "cm-threads-large-multi-vm", 2, 2, 16, 3, 32,
			COMPUTE_THREAD | MULTI_VM | THREADED },
		{ "mixed-threads-large-multi-vm", 2, 2, 16, 3, 32,
			MIXED_THREADS | MULTI_VM | THREADED },
		{ "beng-threads-small", 2, 16, 128, 1, 128,
			THREADED | BIND_EXEC_QUEUE },
		{ "beng-cm-threads-small", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-threads-small", 2, 16, 128, 1, 128,
			MIXED_THREADS | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-many-threads-small", 3, 16, 128, 1, 128,
			THREADED | BIND_EXEC_QUEUE },
		{ "beng-threads-large", 2, 2, 16, 3, 32,
			THREADED | BIND_EXEC_QUEUE },
		{ "beng-cm-threads-large", 2, 2, 16, 3, 32,
			COMPUTE_THREAD | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-threads-large", 2, 2, 16, 3, 32,
			MIXED_THREADS | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-many-threads-large", 3, 2, 16, 3, 32,
			THREADED | BIND_EXEC_QUEUE },
		{ "beng-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ "beng-cm-threads-small-multi-vm", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MIXED_THREADS | MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ "beng-threads-large-multi-vm", 2, 2, 16, 3, 32,
			MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ "beng-cm-threads-large-multi-vm", 2, 2, 16, 3, 32,
			COMPUTE_THREAD | MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ "beng-mixed-threads-large-multi-vm", 2, 2, 16, 3, 32,
			MIXED_THREADS | MULTI_VM | THREADED | BIND_EXEC_QUEUE },
		{ NULL },
	};
	uint64_t vram_size;
	uint64_t system_size;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(xe_has_vram(fd));
		vram_size = xe_visible_vram_size(fd, 0);
		igt_assert(vram_size);
		system_size = igt_get_avail_ram_mb() << 20;

		xe_for_each_engine(fd, hwe)
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COPY)
				break;

		xe_for_each_multi_queue_engine(fd, multi_queue_hwe)
			break;
	}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("evict-%s", s->name) {
			uint64_t bo_size = calc_bo_size(vram_size, s->mul, s->div);
			int ws = working_set(vram_size, system_size, bo_size,
					     1, s->flags);

			igt_debug("Max working set %d n_execs %d\n", ws, s->n_execs);
			if (s->flags & MULTI_QUEUE) {
				igt_require(xe_has_multi_queue_engine(fd));
				igt_require(multi_queue_hwe != NULL);
				igt_assert_f(!(s->flags & MULTI_VM),
					     "MULTI_QUEUE and MULTI_VM cannot be used together.\n");
			}
			igt_skip_on_f(!ws, "System memory size is too small.\n");
			test_evict(fd, s->flags & MULTI_QUEUE ? multi_queue_hwe : hwe,
				   s->n_exec_queues, min(ws, s->n_execs), bo_size,
				   s->flags, NULL);
		}
	}

	for (const struct section_cm *s = sections_cm; s->name; s++) {
		igt_subtest_f("evict-%s", s->name) {
			uint64_t bo_size = calc_bo_size(vram_size, s->mul, s->div);
			int ws = working_set(vram_size, system_size, bo_size,
					     1, s->flags);

			igt_debug("Max working set %d n_execs %d\n", ws, s->n_execs);
			if (s->flags & MULTI_QUEUE) {
				igt_require(xe_has_multi_queue_engine(fd));
				igt_require(multi_queue_hwe != NULL);
				igt_assert_f(!(s->flags & MULTI_VM),
					     "MULTI_QUEUE and MULTI_VM cannot be used together.\n");
			}
			igt_skip_on_f(!ws, "System memory size is too small.\n");
			test_evict_cm(fd, s->flags & MULTI_QUEUE ? multi_queue_hwe : hwe, s->n_exec_queues,
				      min(ws, s->n_execs), bo_size, s->flags, NULL);
		}
	}

	for (const struct section_threads *s = sections_threads; s->name; s++) {
		igt_subtest_f("evict-%s", s->name) {
			uint64_t bo_size = calc_bo_size(vram_size, s->mul, s->div);
			int ws = working_set(vram_size, system_size, bo_size,
					     s->n_threads, s->flags);

			igt_debug("Max working set %d n_execs %d\n", ws, s->n_execs);
			if (s->flags & MULTI_QUEUE) {
				igt_require(xe_has_multi_queue_engine(fd));
				igt_require(multi_queue_hwe != NULL);
				igt_assert_f(!(s->flags & MULTI_VM),
					     "MULTI_QUEUE and MULTI_VM cannot be used together.\n");
			}
			igt_skip_on_f(!ws, "System memory size is too small.\n");
			threads(fd, s->flags & MULTI_QUEUE ? multi_queue_hwe : hwe,
				s->n_threads, s->n_exec_queues, min(ws, s->n_execs),
				bo_size, s->flags);
		}
	}

	igt_fixture()
		drm_close_driver(fd);
}
