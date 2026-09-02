// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality for virtual and parallel exec_queues
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: reset
 * Test category: functionality test
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_gt.h"
#include "xe/xe_legacy.h"
#include "xe/xe_spin.h"
#include <string.h>

#define SYNC_OBJ_SIGNALED	(0x1 << 0)
#define LEGACY_MODE_ADDR	0x1a0000
#define PRESSURE_COUNT		32

static void ignore_gt_reset_warnings_in_dmesg(void)
{
	igt_emit_ignore_dmesg_regex("Wait for CGP_SYNC_DONE response failed");
}

/**
 * SUBTEST: spin
 * Description: test spin
 *
 * SUBTEST: spin-signaled
 * Description: test spin with signaled sync obj
 */
static void test_spin(int fd, struct drm_xe_engine_class_instance *eci,
		      unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queue;
	uint32_t syncobj;
	size_t bo_size;
	uint32_t bo = 0;
	struct xe_spin *spin;
	struct xe_spin_opts spin_opts = { .addr = addr, .preempt = false };
	int i;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*spin);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	syncobj = syncobj_create(fd, (flags & SYNC_OBJ_SIGNALED) ?
				 DRM_SYNCOBJ_CREATE_SIGNALED : 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

#define N_TIMES 4
	for (i = 0; i < N_TIMES; ++i) {
		xe_spin_init(spin, &spin_opts);

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobj;

		exec.exec_queue_id = exec_queue;
		exec.address = addr;
		xe_exec(fd, &exec);

		xe_spin_wait_started(spin);
		usleep(50000);
		igt_assert(!syncobj_wait(fd, &syncobj, 1, 1, 0, NULL));
		xe_spin_end(spin);

		igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	}
#undef N_TIMES

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	xe_exec_queue_destroy(fd, exec_queue);

	munmap(spin, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define MAX_N_EXECQUEUES	16
#define GT_RESET			(0x1 << 0)
#define CLOSE_FD			(0x1 << 1)
#define CLOSE_EXEC_QUEUES		(0x1 << 2)
#define VIRTUAL				(0x1 << 3)
#define PARALLEL			(0x1 << 4)
#define CAT_ERROR			(0x1 << 5)
#define PREEMPT				(0x1 << 6)
#define CANCEL				(0x1 << 7)
#define LONG_SPIN			(0x1 << 8)
#define GT0				(0x1 << 9)
#define GT1				(0x1 << 10)
#define LONG_SPIN_REUSE_QUEUE		(0x1 << 11)
#define SYSTEM				(0x1 << 12)
#define COMPRESSION			(0x1 << 13)
#define MULTI_QUEUE			(0x1 << 14)
#define SECONDARY_QUEUE			(0x1 << 15)
#define RESET_SYNC_MODE			(0x1 << 16)
#define MEM_PRESSURE_STRESS		(0x1 << 17)
#define INVALIDATE_BO_STRESS		(0x1 << 18)
#define RAPID_RESET_STRESS		(0x1 << 19)
#define DESTROY_VM_CTX_STRESS		(0x1 << 20)
#define MIXED_ENGINE_STRESS		(0x1 << 21)
#define PM_TRANSITION_STRESS		(0x1 << 22)
#define FAULT_INJECT_STRESS		(0x1 << 23)

/**
 * SUBTEST: %s-cat-error
 * Description: Test %arg[1] cat error
 *
 * SUBTEST: %s-gt-reset
 * Description: Test %arg[1] GT reset
 *
 * SUBTEST: virtual-close-fd-no-exec
 * Description: Test virtual close fd no-exec
 *
 * SUBTEST: parallel-close-fd-no-exec
 * Description: Test parallel close fd no-exec
 *
 * SUBTEST: %s-close-fd
 * Description: Test %arg[1] close fd
 *
 * SUBTEST: %s-close-execqueues-close-fd
 * Description: Test %arg[1] close exec_queues close fd
 *
 * arg[1]:
 *
 * @virtual:	virtual
 * @parallel:	parallel
 */

static void
wait_gt_idle(int gt_id)
{
	int fd = drm_open_driver(DRIVER_XE);

	if (xe_sysfs_gt_has_node(fd, gt_id, "gtidle/idle_status"))
		igt_wait(xe_gt_is_in_c6(fd, gt_id), 1000, 10);
	else
		/* GT C6 idle status not available (e.g. VF), fall back to fixed delay */
		usleep(150000);

	drm_close_driver(fd);
}

static void
test_balancer(int fd, int gt, int class, int n_exec_queues, int n_execs,
	      unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXECQUEUES];
	uint32_t syncobjs[MAX_N_EXECQUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = false };
	struct drm_xe_engine_class_instance eci[XE_MAX_ENGINE_INSTANCE];
	int i, j, b, num_placements, bad_batches = 1;

	igt_assert_lte(n_exec_queues, MAX_N_EXECQUEUES);

	if (flags & CLOSE_FD)
		fd = drm_open_driver(DRIVER_XE);

	num_placements = xe_gt_fill_engines_by_class(fd, gt, class, eci);
	if (num_placements < 2 ||
	    ((flags & PARALLEL) && !xe_engine_class_supports_multi_lrc(fd, class)))
		return;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, gt),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_exec_queues; i++) {
		igt_assert_eq(__xe_exec_queue_create(fd, vm,
						     flags & PARALLEL ? num_placements : 1,
						     flags & PARALLEL ? 1 : num_placements,
						     eci, 0, &exec_queues[i]), 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	if (flags & VIRTUAL && (flags & CAT_ERROR || flags & GT_RESET))
		bad_batches = num_placements;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && i < bad_batches ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		uint64_t batches[XE_MAX_ENGINE_INSTANCE];
		int e = i % n_exec_queues;

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = batch_addr;

		if (i < bad_batches) {
			spin_opts.addr = base_addr + spin_offset;
			xe_spin_init(&data[i].spin, &spin_opts);
			exec_addr = spin_opts.addr;
		} else {
			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			exec_addr = batch_addr;
		}

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = exec_addr;

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = flags & PARALLEL ?
			to_user_pointer(batches) : exec_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (i < bad_batches && !(flags & CAT_ERROR))
			xe_spin_wait_started(&data[i].spin);

	}

	if (flags & GT_RESET)
		xe_force_gt_reset_async(fd, gt);

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_EXEC_QUEUES) {
			for (i = 0; i < n_exec_queues; i++)
				xe_exec_queue_destroy(fd, exec_queues[i]);
		}
		drm_close_driver(fd);
		wait_gt_idle(gt);
		return;
	}

	for (i = 0; i < n_exec_queues && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	if (!(flags & GT_RESET)) {
		for (i = bad_batches; i < n_execs; i++)
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: cat-error
 * Description: Test cat error
 *
 * SUBTEST: cancel
 * Description: Test job cancel
 *
 * SUBTEST: cancel-preempt
 * Description: Test job cancel with a preemptable job
 *
 * SUBTEST: cancel-timeslice-preempt
 * Description: Test job cancel with 2 preemptable jobs
 *
 * SUBTEST: cancel-timeslice-many-preempt
 * Description: Test job cancel with many preemptable jobs
 *
 * SUBTEST: long-spin-many-preempt
 * Description: Test long spinners with many preemptable jobs
 *
 * SUBTEST: long-spin-many-preempt-media
 * Description: Test long spinners with many preemptable jobs on media GT
 *
 * SUBTEST: long-spin-reuse-many-preempt
 * Description: Test long spinners with many preemptable jobs, use queues again spinners complete
 *
 * SUBTEST: long-spin-reuse-many-preempt-media
 * Description: Test long spinners with many preemptable jobs, use queues again spinners complete on media GT
 *
 * SUBTEST: multi-queue-long-spin-many-queue-switch
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test long spinners with many jobs with multi-queue switching
 *
 * SUBTEST: multi-queue-long-spin-reuse-many-queue-switch
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test long spinners with many jobs with multi-queue switching, use queues again once spinners complete
 *
 * SUBTEST: gt-reset
 * Description: Test GT reset
 *
 * SUBTEST: close-fd-no-exec
 * Description: Test close fd no-exec
 *
 * SUBTEST: close-fd
 * Description: Test close fd
 *
 * SUBTEST: close-execqueues-close-fd
 * Description: Test close exec_queues close fd
 *
 * SUBTEST: cm-cat-error
 * Description: Test compute mode cat-error
 *
 * SUBTEST: cm-gt-reset
 * Description: Test compute mode GT reset
 *
 * SUBTEST: cm-close-fd-no-exec
 * Description: Test compute mode close fd no-exec
 *
 * SUBTEST: cm-close-fd
 * Description: Test compute mode close fd
 *
 * SUBTEST: cm-close-execqueues-close-fd
 * Description: Test compute mode close exec_queues close fd
 *
 * SUBTEST: multi-queue-cat-error
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test cat error with Multi-Queue
 *
 * SUBTEST: multi-queue-cat-error-on-secondary
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test cat error with Multi-Queue
 *              on a secondary queue
 *
 * SUBTEST: multi-queue-gt-reset
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test GT reset with Multi-Queue
 *
 * SUBTEST: multi-queue-cancel
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test engine reset with Multi-Queue
 *
 * SUBTEST: multi-queue-cancel-on-secondary
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test engine reset with Multi-Queue
 *              on a secondary queue
 *
 * SUBTEST: multi-queue-close-fd
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test close fd with Multi-Queue
 *
 * SUBTEST: multi-queue-close-execqueues
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test close execqueues with Multi-Queue
 *
 * SUBTEST: cm-multi-queue-cat-error
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test compute mode cat error with Multi-Queue
 *
 * SUBTEST: cm-multi-queue-cat-error-on-secondary
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test compute mode cat error with Multi-Queue
 *              on a secondary queue
 *
 * SUBTEST: cm-multi-queue-gt-reset
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test compute mode GT reset with Multi-Queue
 *
 * SUBTEST: cm-multi-queue-close-fd
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test compute mode close fd with Multi-Queue
 *
 * SUBTEST: cm-multi-queue-close-execqueues
 * Mega feature: Multi-Queue
 * Sub-category: Multi-Queue tests
 * Description: Test compute mode close execqueues with Multi-Queue
 */

static void
test_compute_mode(int fd, struct drm_xe_engine_class_instance *eci,
		  int n_exec_queues, int n_execs, unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
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
	uint32_t exec_queues[MAX_N_EXECQUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = flags & PREEMPT };
	int i, b;
	int hang_position = flags & SECONDARY_QUEUE ? 1 : 0;

	igt_assert_lte(n_exec_queues, MAX_N_EXECQUEUES);

	igt_assert_f(!(flags & SECONDARY_QUEUE) ||
		     ((flags & MULTI_QUEUE) && (flags & CAT_ERROR)),
		     "SECONDARY_QUEUE requires MULTI_QUEUE and CAT_ERROR to be set");

	if (flags & CLOSE_FD)
		fd = drm_open_driver(DRIVER_XE);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	memset(data, 0, bo_size);

	for (i = 0; i < n_exec_queues; i++) {
		if (flags & MULTI_QUEUE) {
			struct drm_xe_ext_set_property multi_queue = {
				.base.next_extension = 0,
				.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
				.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
			};

			uint64_t ext = to_user_pointer(&multi_queue);

			multi_queue.value = i ? exec_queues[0] : DRM_XE_MULTI_GROUP_CREATE;
			exec_queues[i] = xe_exec_queue_create(fd, vm, eci, ext);
		} else {
			exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		}
	};

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, 3 * NSEC_PER_SEC);
	data[0].vm_sync = 0;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = (flags & CAT_ERROR && i == hang_position) ?
				     (addr + bo_size * 128) : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		int err, e = i % n_exec_queues;

		/*
		 * For cat fault on a secondary queue the fault will
		 * be on the spinner.
		 */
		if (i == hang_position) {
			spin_opts.addr = base_addr + spin_offset;
			xe_spin_init(&data[i].spin, &spin_opts);
			exec_addr = spin_opts.addr;
		} else {
			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			exec_addr = batch_addr;
		}

		sync[0].addr = base_addr +
			(char *)&data[i].exec_sync - (char *)data;

		exec.exec_queue_id = exec_queues[e];
		exec.address = exec_addr;

		/*
		 * Secondary queues are reset when the primary queue
		 * is reset. The submission can race here and it is
		 * expected for those to fail submission if the primary
		 * reset has already happened.
		 */
		err = __xe_exec(fd, &exec);
		igt_assert(!err || ((flags & MULTI_QUEUE) && err == -ECANCELED));

		if (i == hang_position && !(flags & CAT_ERROR))
			xe_spin_wait_started(&data[i].spin);
	}

	if (flags & GT_RESET) {
		xe_spin_wait_started(&data[0].spin);
		xe_force_gt_reset_sync(fd, eci->gt_id);
	}

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_EXEC_QUEUES) {
			for (i = 0; i < n_exec_queues; i++)
				xe_exec_queue_destroy(fd, exec_queues[i]);
		}
		drm_close_driver(fd);
		wait_gt_idle(eci->gt_id);
		return;
	}

	for (i = 0; i < n_execs; i++) {
		int64_t timeout = 3 * NSEC_PER_SEC;
		int err;

		err = __xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
				       exec_queues[i % n_exec_queues], &timeout);
		if (i == hang_position) {
			igt_assert(err == -ETIME || err == -EIO);
		} else if (flags & MULTI_QUEUE) {
			/*
			 * Currently any time GuC resets a queue submitted
			 * by the KMD, the KMD will tear down the entire
			 * queue group. This means we don't know whether
			 * a particular queue submitted prior to the hanging
			 * queue will complete or not. So we have to check
			 * all possible return values here.
			 */
			igt_assert(err == -ETIME || err == -EIO || !err);
		} else if (flags & GT_RESET || flags & CAT_ERROR) {
			/* exec races with reset: may return -EIO or complete */
			igt_assert(err == -EIO || !err);
		} else {
			igt_assert_eq(err, 0);
		}
	}

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, 3 * NSEC_PER_SEC);

	if (!(flags & (GT_RESET))) {
		for (i = 0; i < n_execs; i++) {
			/*
			 * For multi-queue there is no guarantee which
			 * queue will be scheduled first as they are all
			 * submitted at the same priority in this test.
			 * So we can't guarantee any data integrity here.
			 */
			if (i == hang_position || flags & MULTI_QUEUE)
				continue;

			igt_assert_eq(data[i].data, 0xc0ffee);
		}
	}

	for (i = 0; i < n_exec_queues; i++)
		xe_exec_queue_destroy(fd, exec_queues[i]);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

struct gt_thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int fd;
	int gt;
	int id;
	int *go;
	int *exit;
	int *num_reset;
	int *num_submit;
	int *num_submit_fail;
	int *num_vm_recreate;
	unsigned int flags;
	bool do_reset;
};

static void bo_ctx_destroy(int fd, uint32_t *vm, uint32_t *bo,
			   uint32_t **data, size_t bo_size)
{
	if (*data) {
		munmap(*data, bo_size);
		*data = NULL;
	}

	if (*bo) {
		gem_close(fd, *bo);
		*bo = 0;
	}

	if (*vm) {
		xe_vm_destroy(fd, *vm);
		*vm = 0;
	}
}

static void bo_ctx_create(int fd, int gt, uint64_t addr, size_t bo_size,
			  uint32_t *vm, uint32_t *bo, uint32_t **data)
{
	*vm = xe_vm_create(fd, 0, 0);
	*bo = xe_bo_create(fd, *vm, bo_size, vram_if_possible(fd, gt),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	*data = xe_bo_map(fd, *bo, bo_size);
	(*data)[0] = MI_BATCH_BUFFER_END;
	xe_vm_bind_sync(fd, *vm, *bo, 0, addr, bo_size);
}

static void fill_mixed_engine(struct gt_thread_data *t, int iter,
			      struct drm_xe_engine_class_instance *instance)
{
	const uint16_t classes[] = {
		DRM_XE_ENGINE_CLASS_COPY,
		DRM_XE_ENGINE_CLASS_RENDER,
		DRM_XE_ENGINE_CLASS_COMPUTE,
		DRM_XE_ENGINE_CLASS_VIDEO_DECODE,
		DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE,
	};

	instance->gt_id = t->gt;
	instance->engine_instance = 0;
	instance->engine_class = classes[(iter + t->id) % ARRAY_SIZE(classes)];
}

static void pressure_bo_create(int fd, uint32_t vm, int gt,
			       uint32_t *bos, int *count)
{
	int i;

	*count = 0;
	for (i = 0; i < PRESSURE_COUNT; i++) {
		bos[i] = xe_bo_create(fd, vm, xe_bb_size(fd, SZ_2M),
				      vram_if_possible(fd, gt),
				      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		if (!bos[i])
			break;
		(*count)++;
	}
}

static void pressure_bo_destroy(int fd, uint32_t *bos, int count)
{
	int i;

	for (i = 0; i < count; i++)
		gem_close(fd, bos[i]);
}

static void do_resets(struct gt_thread_data *t)
{
	int interval_us = (t->flags & RAPID_RESET_STRESS) ? 1000 : 250000;

	while (!*(t->exit)) {
		usleep(interval_us);
		(*t->num_reset)++;
		if (t->flags & RESET_SYNC_MODE)
			xe_force_gt_reset_sync(t->fd, t->gt);
		else
			xe_force_gt_reset_async(t->fd, t->gt);
	}
}

static void submit_jobs(struct gt_thread_data *t)
{
	int fd = t->fd;
	uint32_t vm = xe_vm_create(fd, 0, 0);
	uint64_t addr = 0x1a0000;
	size_t bo_size = xe_bb_size(fd, SZ_4K);
	uint32_t bo;
	uint32_t pressure_bos[PRESSURE_COUNT];
	uint32_t *data;
	int pressure_count;
	int i = 0, exec_ret;

	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, t->gt),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	data[0] = MI_BATCH_BUFFER_END;

	xe_vm_bind_sync(fd, vm, bo, 0, addr, bo_size);

	if (t->flags & MEM_PRESSURE_STRESS)
		pressure_bo_create(fd, vm, t->gt, pressure_bos, &pressure_count);

	while (!*(t->exit)) {
		struct drm_xe_engine_class_instance instance = {
			.engine_class = DRM_XE_ENGINE_CLASS_COPY,
			.engine_instance = 0,
			.gt_id = t->gt,
		};
		struct drm_xe_exec exec = {
			.address = addr,
			.num_batch_buffer = 1,
		};
		int ret;

		if (t->flags & MIXED_ENGINE_STRESS)
			fill_mixed_engine(t, i, &instance);

		/* GuC IDs can get exhausted */
		ret = __xe_exec_queue_create(fd, vm, 1, 1, &instance, 0, &exec.exec_queue_id);
		if (ret) {
			(*t->num_submit_fail)++;
			continue;
		}

		/*
		 * Once an injected GT reset failure wedges the device, exec and
		 * queue teardown return -ECANCELED. That is the expected outcome
		 * for the fault-injection stress
		 */
		if (t->flags & FAULT_INJECT_STRESS) {
			struct drm_xe_exec_queue_destroy destroy = {
				.exec_queue_id = exec.exec_queue_id,
			};

			exec_ret = __xe_exec(fd, &exec);
			igt_assert_f(exec_ret == 0 || exec_ret == -ECANCELED,
				     "exec returned unexpected error %d (expected 0 or -ECANCELED)\n",
				     exec_ret);
			igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &destroy);
		} else {
			xe_exec(fd, &exec);
			xe_exec_queue_destroy(fd, exec.exec_queue_id);
		}

		(*t->num_submit)++;

		if ((t->flags & MEM_PRESSURE_STRESS) && !(i % 128)) {
			pressure_bo_destroy(fd, pressure_bos, pressure_count);
			pressure_count = 0;
			pressure_bo_create(fd, vm, t->gt, pressure_bos, &pressure_count);
		}

		if (((t->flags & INVALIDATE_BO_STRESS) && !(i % 96)) ||
		    ((t->flags & DESTROY_VM_CTX_STRESS) && !(i % 128)) ||
		     ((t->flags & PM_TRANSITION_STRESS) && !(i % 160))) {
			if (t->flags & PM_TRANSITION_STRESS)
				usleep(20000);

			if (t->flags & MEM_PRESSURE_STRESS) {
				pressure_bo_destroy(fd, pressure_bos, pressure_count);
				pressure_count = 0;
			}

			bo_ctx_destroy(fd, &vm, &bo, &data, bo_size);
			bo_ctx_create(fd, t->gt, addr, bo_size, &vm, &bo, &data);
			if (t->flags & MEM_PRESSURE_STRESS)
				pressure_bo_create(fd, vm, t->gt, pressure_bos, &pressure_count);
			(*t->num_vm_recreate)++;
		}

		i++;
	}

	if (t->flags & MEM_PRESSURE_STRESS)
		pressure_bo_destroy(fd, pressure_bos, PRESSURE_COUNT);

	munmap(data, bo_size);
	/*
	 * On a device wedged by injected GT reset failures, BO close and VM
	 * destroy also return -ECANCELED.
	 */
	if (t->flags & FAULT_INJECT_STRESS) {
		struct drm_gem_close close_bo = { .handle = bo };
		struct drm_xe_vm_destroy vm_destroy = { .vm_id = vm };

		igt_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
		igt_ioctl(fd, DRM_IOCTL_XE_VM_DESTROY, &vm_destroy);
	} else {
		gem_close(fd, bo);
		xe_vm_destroy(fd, vm);
	}
}

static void *gt_reset_thread(void *data)
{
	struct gt_thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->do_reset)
		do_resets(t);
	else
		submit_jobs(t);

	return NULL;
}

static int fault_inject_fd = -1;

static void gt_reset_disable_fault_injection(int sig)
{
	if (fault_inject_fd < 0)
		return;

	igt_debugfs_write(fault_inject_fd, "fail_gt_reset/probability", "0");
	igt_debugfs_write(fault_inject_fd, "fail_gt_reset/times", "1");
	fault_inject_fd = -1;
}

static void gt_reset_enable_fault_injection(int fd)
{
	static bool exit_handler_installed;

	fault_inject_fd = fd;

	if (!exit_handler_installed) {
		igt_install_exit_handler(gt_reset_disable_fault_injection);
		exit_handler_installed = true;
	}

	igt_debugfs_write(fd, "fail_gt_reset/probability", "100");
	igt_debugfs_write(fd, "fail_gt_reset/times", "2");
}

static int try_vm_create(int fd)
{
	struct drm_xe_vm_create create = { 0 };
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &create))
		err = -errno;
	else
		xe_vm_destroy(fd, create.vm_id);

	return err;
}

static void ignore_gt_reset_fault_dmesg(void)
{
	igt_emit_ignore_dmesg_regex("reset failed \\(-ECANCELED\\)"
				    "|declared device .* as wedged"
				    "|GPU HANG"
				    "|Failed to reset");
}

/**
 * SUBTEST: gt-reset-stress
 * Description: Stress GT reset
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-concurrent-submit
 * Description: Stress concurrent GT resets and job submissions
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-concurrent-submit-sync
 * Description: Stress concurrent GT resets and job submissions with sync resets
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-memory-pressure
 * Description: Stress concurrent GT resets and job submissions under memory pressure
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-bo-invalidation
 * Description: Stress concurrent GT resets and job submissions while invalidating BO CPU mappings
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-rapid-cycle
 * Description: Stress concurrent GT resets and job submissions with rapid-cycle resets
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-destroy-vm-context
 * Description: Stress concurrent GT resets and job submissions while destroying and recreating VM context
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-mixed-engine-usage
 * Description: Stress concurrent GT resets and mixed-engine job submissions
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-pm-transition
 * Description: Stress concurrent GT resets and job submissions during PM transition windows
 * Test category: stress test
 *
 * SUBTEST: gt-stress-reset-engine-hang
 * Description: Test GT reset while long spinner workload is active
 * Test category: stress test
 *
 * SUBTEST: gt-reset-fault-injection
 * Description: Stress concurrent GT resets and job submissions with GT reset failures injected via debugfs
 * Test category: fault injection
 */
static void
gt_reset(int fd, int gt, int n_threads, int n_sec, unsigned int flags)
{
	struct gt_thread_data *threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int go = 0, exit = 0, num_reset = 0, i;
	int num_submit = 0, num_submit_fail = 0, num_vm_recreate = 0;

	threads = calloc(n_threads, sizeof(struct gt_thread_data));
	igt_assert(threads);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	if (flags & FAULT_INJECT_STRESS)
		gt_reset_enable_fault_injection(fd);

	for (i = 0; i < n_threads; ++i) {
		threads[i].mutex = &mutex;
		threads[i].cond = &cond;
		threads[i].fd = fd;
		threads[i].gt = gt;
		threads[i].id = i;
		threads[i].flags = flags;
		threads[i].go = &go;
		threads[i].exit = &exit;
		threads[i].num_reset = &num_reset;
		threads[i].num_submit = &num_submit;
		threads[i].num_submit_fail = &num_submit_fail;
		threads[i].num_vm_recreate = &num_vm_recreate;
		threads[i].do_reset = (i == 0);

		pthread_create(&threads[i].thread, 0, gt_reset_thread,
			       &threads[i]);
	}

	pthread_mutex_lock(&mutex);
	go = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	sleep(n_sec);
	exit = 1;

	for (i = 0; i < n_threads; i++)
		pthread_join(threads[i].thread, NULL);

	igt_info("number of resets %d, submissions %d, submit fails %d vm_recreate %d\n",
		 num_reset, num_submit, num_submit_fail, num_vm_recreate);

	if (flags & FAULT_INJECT_STRESS)
		gt_reset_disable_fault_injection(0);

	igt_assert_neq(num_reset, 0);
	igt_assert_neq(num_submit, 0);
	free(threads);
}

/**
 * SUBTEST: gt-mocs-reset
 * Description: Validate mocs register contents over GT reset
 * Test category: mocs
 *
 */
static void
gt_mocs_reset(int fd, int gt)
{
	char path[256];
	char *mocs_content_pre, *mocs_contents_post;

	sprintf(path, "gt%d/mocs", gt);
	igt_require_f(igt_debugfs_exists(fd, path, O_RDONLY),
		      "Failed to open required debugfs entry: %s\n", path);

	/* Mocs debugfs contents before and after GT reset.
	 * Allocate memory to store 10k characters sufficient enough
	 * to store global mocs and lncf mocs data.
	 */
	mocs_content_pre = (char *)malloc(10000 * sizeof(char));
	mocs_contents_post = (char *)malloc(10000 * sizeof(char));

	igt_assert(mocs_content_pre);
	igt_assert(mocs_contents_post);

	igt_debugfs_dump(fd, path);
	igt_debugfs_read(fd, path, mocs_content_pre);

	xe_force_gt_reset_sync(fd, gt);

	igt_assert(igt_debugfs_exists(fd, path, O_RDONLY));
	igt_debugfs_dump(fd, path);
	igt_debugfs_read(fd, path, mocs_contents_post);

	igt_assert(strcmp(mocs_content_pre, mocs_contents_post) == 0);

	free(mocs_content_pre);
	free(mocs_contents_post);
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int fd;
	struct drm_xe_engine_class_instance *hwe;
	int n_exec_queue;
	int n_exec;
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

	xe_legacy_test_mode(t->fd, t->hwe, t->n_exec_queue, t->n_exec,
			    t->flags, LEGACY_MODE_ADDR, false);

	return NULL;
}

/**
 * SUBTEST: long-spin-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, both GTs
 *
 * SUBTEST: long-spin-many-preempt-gt0-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, primary GT
 *
 * SUBTEST: long-spin-many-preempt-gt1-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, media GT
 *
 * SUBTEST: long-spin-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs
 *
 * SUBTEST: long-spin-sys-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs, use system memory
 *
 * SUBTEST: long-spin-comp-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs, use compressed memory
 *
 * SUBTEST: long-spin-reuse-many-preempt-gt0-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, primary GT
 *
 * SUBTEST: long-spin-reuse-many-preempt-gt1-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete,  media GT
 */

static void threads(int fd, int n_exec_queues, int n_execs, unsigned int flags)
{
	struct thread_data *threads_data;
	struct drm_xe_engine_class_instance *hwe;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int n_engines = 0, i = 0;
	bool go = false;

	if (flags & COMPRESSION)
		igt_require(HAS_FLATCCS(intel_get_drm_devid(fd)));

	xe_for_each_engine(fd, hwe) {
		if (hwe->gt_id && (flags & GT0))
			continue;
		if (!hwe->gt_id && (flags & GT1))
			continue;

		++n_engines;
	}

	threads_data = calloc(n_engines, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	xe_for_each_engine(fd, hwe) {
		if (hwe->gt_id && (flags & GT0))
			continue;
		if (!hwe->gt_id && (flags & GT1))
			continue;

		threads_data[i].fd = fd;
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
		threads_data[i].hwe = hwe;
		threads_data[i].n_exec_queue = n_exec_queues;
		threads_data[i].n_exec = n_execs;
		threads_data[i].flags = flags;
		threads_data[i].go = &go;

		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
		++i;
	}

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_engines; ++i)
		pthread_join(threads_data[i].thread, NULL);

	free(threads_data);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "virtual", VIRTUAL },
		{ "parallel", PARALLEL },
		{ NULL },
	};
	const struct section ssections[] = {
		{ "reset-concurrent-submit", 0 },
		{ "reset-concurrent-submit-sync", RESET_SYNC_MODE },
		{ "reset-memory-pressure", MEM_PRESSURE_STRESS },
		{ "reset-bo-invalidation", INVALIDATE_BO_STRESS },
		{ "reset-rapid-cycle", RAPID_RESET_STRESS },
		{ "reset-destroy-vm-context", DESTROY_VM_CTX_STRESS },
		{ "reset-mixed-engine-usage", MIXED_ENGINE_STRESS | RAPID_RESET_STRESS },
		{ "reset-pm-transition", PM_TRANSITION_STRESS | DESTROY_VM_CTX_STRESS },
		{ NULL },
	};
	int gt;
	int class;
	int fd;
	char pci_slot[NAME_MAX];

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	igt_subtest("spin")
		xe_for_each_engine(fd, hwe)
			test_spin(fd, hwe, 0);

	igt_subtest("spin-signaled")
		xe_for_each_engine(fd, hwe)
			test_spin(fd, hwe, SYNC_OBJ_SIGNALED);

	igt_subtest("cat-error")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 2, 2, CAT_ERROR,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("cancel")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 1, 1, 0,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 1, 1, PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-timeslice-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 2, 2, CANCEL | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-timeslice-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 4, CANCEL | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt-media")
		xe_for_each_engine(fd, hwe) {
			if (!hwe->gt_id)
				continue;
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-reuse-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT |
					    LONG_SPIN_REUSE_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-reuse-many-preempt-media")
		xe_for_each_engine(fd, hwe) {
			if (!hwe->gt_id)
				continue;
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT |
					    LONG_SPIN_REUSE_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT);

	igt_subtest("long-spin-many-preempt-gt0-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT0);

	igt_subtest("long-spin-many-preempt-gt1-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT1);

	igt_subtest("long-spin-reuse-many-preempt-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-sys-reuse-many-preempt-threads")
		threads(fd, 2, 16, SYSTEM | LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-comp-reuse-many-preempt-threads")
		threads(fd, 2, 16, COMPRESSION | LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-reuse-many-preempt-gt0-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT0 |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-reuse-many-preempt-gt1-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT1 |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("gt-reset")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 2, 2, GT_RESET,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-fd-no-exec")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 0, CLOSE_FD,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-fd")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256, CLOSE_FD,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-execqueues-close-fd")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256, CLOSE_FD |
					    CLOSE_EXEC_QUEUES,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("cm-cat-error")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, CAT_ERROR);

	igt_subtest("cm-gt-reset")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, GT_RESET);

	igt_subtest("cm-close-fd-no-exec")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 0, CLOSE_FD);

	igt_subtest("cm-close-fd")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD);

	igt_subtest("cm-close-execqueues-close-fd")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD |
					  CLOSE_EXEC_QUEUES);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("%s-cat-error", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(fd, gt, class, XE_MAX_ENGINE_INSTANCE + 1,
						      XE_MAX_ENGINE_INSTANCE + 1,
						      CAT_ERROR | s->flags);

		igt_subtest_f("%s-gt-reset", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(fd, gt, class, XE_MAX_ENGINE_INSTANCE + 1,
						      XE_MAX_ENGINE_INSTANCE + 1,
						      GT_RESET | s->flags);

		igt_subtest_f("%s-close-fd-no-exec", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 0,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-fd", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 256,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-execqueues-close-fd", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 256, CLOSE_FD |
						      CLOSE_EXEC_QUEUES | s->flags);
	}

	igt_subtest("gt-reset-stress")
		gt_reset(fd, 0, 4, 1, 0);

	for (const struct section *s = ssections; s->name; s++) {
		igt_subtest_f("gt-stress-%s", s->name)
			gt_reset(fd, 0, 8, 2, s->flags);
	}

	igt_subtest("gt-stress-reset-engine-hang")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 2, 4,
					    GT_RESET | LONG_SPIN,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("gt-reset-fault-injection") {
		igt_require_f(igt_debugfs_exists(fd, "fail_gt_reset/probability",
						 O_RDWR),
			      "GT reset fault injection not available; "
			      "CONFIG_DRM_XE_KUNIT_TEST/fault-injection must be "
			      "enabled in the KMD\n");

		igt_device_get_pci_slot_name(fd, pci_slot);
		ignore_gt_reset_fault_dmesg();

		gt_reset(fd, 0, 8, 2, FAULT_INJECT_STRESS);

		igt_assert_f(try_vm_create(fd) != 0,
			     "Device did not wedge after injected GT reset failure\n");

		gt_reset_disable_fault_injection(0);
		drm_close_driver(fd);
		igt_kmod_rebind("xe", pci_slot);
		fd = drm_open_driver(DRIVER_XE);

		igt_assert_f(try_vm_create(fd) == 0,
			     "Device not functional after rebind recovery\n");
	}

	igt_subtest("gt-mocs-reset")
		xe_for_each_gt(fd, gt)
			gt_mocs_reset(fd, gt);

	igt_subtest("multi-queue-cat-error") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 16, 16,
					    CAT_ERROR | MULTI_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-cat-error-on-secondary") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 16, 16,
					    CAT_ERROR | MULTI_QUEUE |
					    SECONDARY_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-gt-reset") {
		igt_require(xe_has_multi_queue_engine(fd));
		ignore_gt_reset_warnings_in_dmesg();
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 16, 16,
					    GT_RESET | MULTI_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-cancel") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 16, 16,
					    MULTI_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-cancel-on-secondary") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 16, 16,
					    MULTI_QUEUE | SECONDARY_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-close-fd") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256,
					    CLOSE_FD | MULTI_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("multi-queue-close-execqueues") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256,
					    CLOSE_EXEC_QUEUES | CLOSE_FD |
					    MULTI_QUEUE,
					    LEGACY_MODE_ADDR,
					    false);
	}

	igt_subtest("cm-multi-queue-cat-error") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			test_compute_mode(fd, hwe, 16, 16,
					  CAT_ERROR | MULTI_QUEUE);
	}

	igt_subtest("cm-multi-queue-cat-error-on-secondary") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			test_compute_mode(fd, hwe, 16, 16,
					  CAT_ERROR | MULTI_QUEUE |
					  SECONDARY_QUEUE);
	}

	igt_subtest("cm-multi-queue-gt-reset") {
		igt_require(xe_has_multi_queue_engine(fd));
		ignore_gt_reset_warnings_in_dmesg();
		xe_for_each_multi_queue_engine(fd, hwe)
			test_compute_mode(fd, hwe, 16, 16,
					  GT_RESET | MULTI_QUEUE);
	}

	igt_subtest("cm-multi-queue-close-fd") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256,
					  CLOSE_FD | MULTI_QUEUE);
	}

	igt_subtest("cm-multi-queue-close-execqueues") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256,
					  CLOSE_EXEC_QUEUES | CLOSE_FD |
					  MULTI_QUEUE);
	}

	igt_subtest("multi-queue-long-spin-many-queue-switch") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | MULTI_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}
	}

	igt_subtest("multi-queue-long-spin-reuse-many-queue-switch") {
		igt_require(xe_has_multi_queue_engine(fd));
		xe_for_each_multi_queue_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | MULTI_QUEUE |
					    LONG_SPIN_REUSE_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}
	}

	igt_fixture()
		drm_close_driver(fd);
}
