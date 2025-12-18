// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: Basic tests for exec functionality with multi-queue feature
 * Category: Hardware building block
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: multi-queue
 */

#include "igt.h"
#include "xe_drm.h"
#include "lib/igt_syncobj.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define XE_EXEC_QUEUE_PRIORITY_LOW	0
#define XE_EXEC_QUEUE_PRIORITY_NORMAL	1
#define XE_EXEC_QUEUE_PRIORITY_HIGH	2
#define XE_EXEC_QUEUE_NUM_PRIORITIES	3

#define MAX_N_EXEC_QUEUES	64

#define USERPTR			(0x1 << 0)
#define PRIORITY		(0x1 << 1)
#define CLOSE_FD		(0x1 << 2)

#define MAX_INSTANCE 9

#define XE_MULTI_GROUP_VALID_FLAGS   (DRM_XE_MULTI_GROUP_CREATE)

#define BASE_ADDRESS	0x1a0000

static void
__test_sanity(int fd, int gt, int class)
{
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	struct drm_xe_ext_set_property multi_queue = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
		.value = DRM_XE_MULTI_GROUP_CREATE,
	};
	struct drm_xe_ext_set_property mq_priority = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY,
	};
	struct drm_xe_ext_set_property priority = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_PRIORITY,
		.value = XE_EXEC_QUEUE_PRIORITY_NORMAL,
	};
	struct drm_xe_engine_class_instance vm_bind_eci = {
		.engine_class = DRM_XE_ENGINE_CLASS_VM_BIND,
	};
	uint64_t invalid_flag = 0, ext = to_user_pointer(&multi_queue);
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	struct drm_xe_engine_class_instance *hwe;
	uint32_t vm, vm2, val;
	int i, n = 0;

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[n++] = *hwe;
	}

	if (!n)
		return;

	vm = xe_vm_create(fd, 0, 0);

	/* Invalid flags */
	while (!invalid_flag)
		invalid_flag = (1ull << (rand() % 63)) & ~XE_MULTI_GROUP_VALID_FLAGS;
	multi_queue.value |= invalid_flag;
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);
	multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;

	/* Queues can't be a vm_bind queues */
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, &vm_bind_eci, ext, &val), -EINVAL);
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, 0);
	multi_queue.value = exec_queues[0];
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, &vm_bind_eci, ext, &val), -EINVAL);
	xe_exec_queue_destroy(fd, exec_queues[0]);
	exec_queues[0] = xe_bind_exec_queue_create(fd, vm, 0);
	multi_queue.value = exec_queues[0];
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);
	xe_exec_queue_destroy(fd, exec_queues[0]);

	/* Q0 can't be a regular queue */
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, 0);
	multi_queue.value = exec_queues[0];
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);
	xe_exec_queue_destroy(fd, exec_queues[0]);

	/* Multi-Queue can't be a parallel queue */
	multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;
	if (n > 1)
		igt_assert_eq(__xe_exec_queue_create(fd, vm, 2, 1, eci, ext, &val), -EINVAL);

	/* Specifying multiple MULTI_GROUP property is invalid */
	multi_queue.base.next_extension = to_user_pointer(&multi_queue);
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	/* Setting other queue properties are valid for Q0 */
	multi_queue.base.next_extension = to_user_pointer(&priority);
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, ext);
	xe_exec_queue_destroy(fd, exec_queues[0]);

	/* Adding queues to group after primary is destroyed is invalid */
	multi_queue.base.next_extension = 0;
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, ext);
	xe_exec_queue_destroy(fd, exec_queues[0]);
	multi_queue.value = exec_queues[0];
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -ENOENT);

	multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, ext);

	/* Upper 32 bits must be 0 while adding secondary queues */
	multi_queue.value = exec_queues[0] | (1ull << (32 + (rand() % 32)));
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	/* Invalid Q0 */
	multi_queue.value = exec_queues[0] + 1;
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -ENOENT);

	/* Queues in a queue group must share the same address space (vm) */
	multi_queue.value = exec_queues[0];
	vm2 = xe_vm_create(fd, 0, 0);
	igt_assert_eq(__xe_exec_queue_create(fd, vm2, 1, 1, eci, ext, &val), -EINVAL);
	xe_vm_destroy(fd, vm2);

	/* Secondary queues must map to same engine instances as primary queue */
	if (n > 1)
		igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, n, eci, ext, &val), -EINVAL);

	/* Setting properties (other than MULTI_QUEUE_PRIORITY) is invalid for secondary queues */
	multi_queue.base.next_extension = to_user_pointer(&priority);
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	multi_queue.base.next_extension = 0;
	for (i = 1; i < MAX_N_EXEC_QUEUES; i++)
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, ext);

	/* Queue group limit check */
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	/* Secondary queues can't be replaced once successfully created */
	xe_exec_queue_destroy(fd, exec_queues[1]);
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	/* Primary queue can be destroyed before all secondary queues are destroyed */
	xe_exec_queue_destroy(fd, exec_queues[0]);

	for (i = 2; i < MAX_N_EXEC_QUEUES; i++)
		xe_exec_queue_destroy(fd, exec_queues[i]);

	/* Validate with multiple num_placements */
	if (n > 1) {
		multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;
		multi_queue.base.next_extension = 0;
		igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, n, eci, ext, &exec_queues[0]), 0);

		multi_queue.value = exec_queues[0];
		for (i = 1; i < MAX_N_EXEC_QUEUES; i++)
			igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, n, eci, ext, &exec_queues[i]), 0);

		for (i = 0; i < MAX_N_EXEC_QUEUES; i++)
			xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	/* MQ priority is not valid for regular queues */
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci,
					     to_user_pointer(&mq_priority), &val), -EINVAL);

	/* MQ priority validation */
	multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;
	multi_queue.base.next_extension = to_user_pointer(&mq_priority);
	mq_priority.value = XE_EXEC_QUEUE_NUM_PRIORITIES;
	igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, 1, eci, ext, &val), -EINVAL);

	mq_priority.value = XE_EXEC_QUEUE_PRIORITY_HIGH;
	exec_queues[0] = xe_exec_queue_create(fd, vm, eci, ext);
	multi_queue.value = exec_queues[0];
	exec_queues[1] = xe_exec_queue_create(fd, vm, eci, ext);
	xe_exec_queue_destroy(fd, exec_queues[1]);
	xe_exec_queue_destroy(fd, exec_queues[0]);

	igt_fork(child, 1) {
		igt_drop_root();

		/* Tests MULTI_QUEUE_PRIORITY property by dropping root permissions */
		multi_queue.value = DRM_XE_MULTI_GROUP_CREATE;
		mq_priority.value = XE_EXEC_QUEUE_PRIORITY_HIGH;
		exec_queues[0] = xe_exec_queue_create(fd, vm, eci, ext);
		multi_queue.value = exec_queues[0];
		exec_queues[1] = xe_exec_queue_create(fd, vm, eci, ext);
		xe_exec_queue_destroy(fd, exec_queues[1]);
		xe_exec_queue_destroy(fd, exec_queues[0]);
	}
	igt_waitchildren();

	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: sanity
 * Description: Run sanity test
 * Test category: functionality test
 */
static void
test_sanity(int fd, int gt, int class)
{
	__test_sanity(fd, gt, class);
}

static void
test_legacy_mode(int fd, struct drm_xe_engine_class_instance *eci, int num_placement,
		 int n_exec_queues, int n_execs, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_TYPE_SYNCOBJ | DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .flags = DRM_XE_SYNC_TYPE_SYNCOBJ | DRM_XE_SYNC_FLAG_SIGNAL, },
	};

	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t vm;
	uint64_t addr = BASE_ADDRESS;
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	uint32_t bind_syncobj;
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	if (flags & CLOSE_FD)
		fd = drm_open_driver(DRIVER_XE);

	igt_assert(n_exec_queues <= MAX_N_EXEC_QUEUES);
	vm = xe_vm_create(fd, 0, 0);
	bo_size = xe_bb_size(fd, sizeof(*data) * n_execs);

	if (flags & USERPTR) {
		data = aligned_alloc(xe_get_default_alignment(fd), bo_size);
		igt_assert(data);

		memset(data, 0, bo_size);
	} else {
		bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, eci[0].gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);
	}

	for (i = 0; i < n_exec_queues; i++) {
		struct drm_xe_ext_set_property multi_queue = {
			.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
			.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
		};
		struct drm_xe_ext_set_property mq_priority = {
			.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
			.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY,
		};
		uint64_t ext = to_user_pointer(&multi_queue);

		if (flags & PRIORITY) {
			multi_queue.base.next_extension = to_user_pointer(&mq_priority);
			mq_priority.value = XE_EXEC_QUEUE_PRIORITY_NORMAL + (rand() % 2);
		}

		multi_queue.value = i ? exec_queues[0] : DRM_XE_MULTI_GROUP_CREATE;
		igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, num_placement, eci,
						     ext, &exec_queues[i]), 0);

		syncobjs[i] = syncobj_create(fd, 0);
	};

	bind_syncobj = syncobj_create(fd, 0);
	sync[0].handle = bind_syncobj;
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data),
					 addr, bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_exec_queues;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[0].handle = bind_syncobj;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		if (e != i)
			syncobj_reset(fd, &syncobjs[e], 1);

		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_exec_queues && i < n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0, NULL));

	igt_assert(syncobj_wait(fd, &bind_syncobj, 1, INT64_MAX, 0, NULL));
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	syncobj_reset(fd, &sync[0].handle, 1);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1,	INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		if (!(flags & CLOSE_FD))
			xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else {
		free(data);
	}

	syncobj_destroy(fd, bind_syncobj);

	if (!(flags & CLOSE_FD))
		xe_vm_destroy(fd, vm);
	else
		drm_close_driver(fd);
}

/**
 * SUBTEST: one-queue-%s
 * Description: Run %arg[1] test with one exec queue
 * Test category: functionality test
 *
 * SUBTEST: two-queues-%s
 * Description: Run %arg[1] test with two exec queues
 * Test category: functionality test
 *
 * SUBTEST: many-queues-%s
 * Description: Run %arg[1] test with many exec queues
 * Test category: stress test
 *
 * SUBTEST: max-queues-%s
 * Description: Run %arg[1] test with max exec queues
 * Test category: stress test
 *
 * SUBTEST: many-execs-%s
 * Description: Run %arg[1] test with many exec submissions per exec queue
 * Test category: functionality test
 *
 * SUBTEST: few-execs-%s
 * Description: Run %arg[1] test with exec submissions only on few exec queues
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @basic:					basic
 * @userptr:					userptr
 * @priority:					priority
 * @close-fd:					close fd without destroying exec queues
 */
static void
test_exec(int fd, struct drm_xe_engine_class_instance *eci, int num_placement,
	  int n_exec_queues, int n_execs, unsigned int flags)
{
	test_legacy_mode(fd, eci, num_placement, n_exec_queues, n_execs, flags);
}

/**
 * SUBTEST: virtual
 * Description: Validate virtual queues with multiple placements
 * Test category: functionality test
 */
static void
test_exec_virtual(int fd, int gt, int class)
{
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	struct drm_xe_engine_class_instance *hwe;
	int n = 0;

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[n++] = *hwe;
	}
	igt_assert(n);

	test_exec(fd, eci, n, n, n, 0);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "userptr", USERPTR },
		{ "priority", PRIORITY },
		{ "close-fd", CLOSE_FD },
		{ NULL },
	};
	int fd, gt, class;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(intel_graphics_ver(intel_get_drm_devid(fd)) >= IP_VER(35, 0));
	}

	igt_subtest_f("sanity")
		xe_for_each_gt(fd, gt)
			xe_for_each_multi_queue_engine_class(class)
				test_sanity(fd, gt, class);

	igt_subtest_f("virtual")
		xe_for_each_gt(fd, gt)
			xe_for_each_multi_queue_engine_class(class)
				test_exec_virtual(fd, gt, class);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("one-queue-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, 1, 1, s->flags);

		igt_subtest_f("two-queues-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, 2, 2, s->flags);

		igt_subtest_f("many-queues-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, 16, 16, s->flags);

		igt_subtest_f("max-queues-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, MAX_N_EXEC_QUEUES,
					  MAX_N_EXEC_QUEUES, s->flags);

		igt_subtest_f("many-execs-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, 16, 64, s->flags);

		igt_subtest_f("few-execs-%s", s->name)
			xe_for_each_multi_queue_engine(fd, hwe)
				test_exec(fd, hwe, 1, 16, 8, s->flags);
	}

	igt_fixture()
		drm_close_driver(fd);
}
