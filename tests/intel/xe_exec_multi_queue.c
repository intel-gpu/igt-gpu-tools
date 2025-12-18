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

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define MAX_N_EXEC_QUEUES	64

#define MAX_INSTANCE 9

#define XE_MULTI_GROUP_VALID_FLAGS   (DRM_XE_MULTI_GROUP_CREATE)

static void
__test_sanity(int fd, int gt, int class)
{
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	struct drm_xe_ext_set_property multi_queue = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP,
		.value = DRM_XE_MULTI_GROUP_CREATE,
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

int igt_main()
{
	int fd, gt, class;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(intel_graphics_ver(intel_get_drm_devid(fd)) >= IP_VER(35, 0));
	}

	igt_subtest_f("sanity")
		xe_for_each_gt(fd, gt)
			xe_for_each_multi_queue_engine_class(class)
				test_sanity(fd, gt, class);

	igt_fixture()
		drm_close_driver(fd);
}
