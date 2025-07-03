/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "igt_core.h"
#include "igt_sysfs.h"
#include "lib/intel_chipset.h"
#include "xe_gt.h"
#include "xe_ioctl.h"
#include "xe_query.h"

#ifdef __linux__
#include <sys/sysmacros.h>
#else
#define minor(__v__) ((__v__) & 0xff)
#endif

/**
 * has_xe_gt_reset:
 * @fd: open xe drm file descriptor
 *
 * Check gt force reset sysfs entry is available or not
 *
 * Returns: reset sysfs entry available
 */
bool has_xe_gt_reset(int fd)
{
	int reset_fd;
	int gt;

	xe_for_each_gt(fd, gt) {
		reset_fd = igt_debugfs_gt_open(fd, gt, "force_reset", O_WRONLY);
		if (reset_fd == -1)
			return false;
		close(reset_fd);
	}

	return true;
}

static void xe_force_gt_reset(int fd, int gt, bool sync)
{
	char reset_string[128];
	struct stat st;

	igt_assert_eq(fstat(fd, &st), 0);

	snprintf(reset_string, sizeof(reset_string),
		 "cat /sys/kernel/debug/dri/%d/gt%d/force_reset%s",
		 minor(st.st_rdev), gt, sync ? "_sync" : "");
	system(reset_string);
}

/**
 * xe_force_gt_reset_async:
 * @fd: the Xe DRM file descriptor
 * @gt: the GT identifier
 *
 * This function forces a reset on the selected GT.
 * It does not wait for the reset completion.
 */
void xe_force_gt_reset_async(int fd, int gt)
{
	xe_force_gt_reset(fd, gt, false);
}

/**
 * xe_force_gt_reset_async:
 * @fd: the Xe DRM file descriptor
 * @gt: the GT identifier
 *
 * This function forces a reset on the selected GT.
 * It will wait until the reset completes.
 */
void xe_force_gt_reset_sync(int fd, int gt)
{
	xe_force_gt_reset(fd, gt, true);
}

/**
 * xe_force_gt_reset_all:
 *
 * Forces reset of all the GT's.
 */
void xe_force_gt_reset_all(int xe_fd)
{
	int gt;

	xe_for_each_gt(xe_fd, gt)
		xe_force_gt_reset_async(xe_fd, gt);
}

/**
 * xe_hang_ring:
 * @fd: open xe drm file descriptor
 * @ring: execbuf ring flag
 *
 * This helper function injects a hanging batch into @ring. It returns a
 * #igt_hang_t structure which must be passed to xe_post_hang_ring() for
 * hang post-processing (after the gpu hang interaction has been tested).
 *
 * Returns:
 * Structure with helper internal state for xe_post_hang_ring().
 */
igt_hang_t xe_hang_ring(int fd, uint64_t ahnd, uint32_t ctx, int ring,
				unsigned int flags)
{
	uint16_t class;
	uint32_t vm;
	unsigned int exec_queue;
	igt_spin_t *spin_t;

	vm = xe_vm_create(fd, 0, 0);

	switch (ring) {
	case I915_EXEC_DEFAULT:
		if (IS_PONTEVECCHIO(intel_get_drm_devid(fd)))
			class = DRM_XE_ENGINE_CLASS_COPY;
		else
			class = DRM_XE_ENGINE_CLASS_RENDER;
		break;
	case I915_EXEC_RENDER:
		if (IS_PONTEVECCHIO(intel_get_drm_devid(fd)))
			igt_skip("Render engine not supported on this platform.\n");
		else
			class = DRM_XE_ENGINE_CLASS_RENDER;
		break;
	case I915_EXEC_BLT:
		class = DRM_XE_ENGINE_CLASS_COPY;
		break;
	case I915_EXEC_BSD:
		class = DRM_XE_ENGINE_CLASS_VIDEO_DECODE;
		break;
	case I915_EXEC_VEBOX:
		class = DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE;
		break;
	default:
		igt_assert_f(false, "Unknown engine: %x", (uint32_t) flags);
	}

	exec_queue = xe_exec_queue_create_class(fd, vm, class);

	spin_t = igt_spin_new(fd, .ahnd = ahnd, .engine = exec_queue, .vm = vm,
				.flags = IGT_SPIN_NO_PREEMPTION);
	return (igt_hang_t){ spin_t, exec_queue, 0, flags };
}

/**
 * xe_post_hang_ring:
 * @fd: open xe drm file descriptor
 * @arg: hang state from xe_hang_ring()
 *
 * This function does the necessary post-processing after a gpu hang injected
 * with xe_hang_ring().
 */
void xe_post_hang_ring(int fd, igt_hang_t arg)
{
	xe_exec_queue_destroy(fd, arg.ctx);
	xe_vm_destroy(fd, arg.spin->vm);
}

/**
 * xe_gt_stats_get_count:
 * @fd: open xe drm file descriptor
 * @gt: gt_id
 * @stat: name of the stat of which counter is needed
 *
 * This function returns the counter for a given stat.
 */
int xe_gt_stats_get_count(int fd, int gt, const char *stat)
{
	FILE *f;
	struct stat st;
	char tlb_path[4096];
	char path[256];
	int count;

	igt_assert_eq(fstat(fd, &st), 0);

	sprintf(path, "/sys/kernel/debug/dri/%d/gt%d/stats",
		minor(st.st_rdev), gt);
	f = fopen(path, "r");

	if (!f) {
		igt_warn("Failed to open /sys/kernel/debug/dri/%d/gt%d/stats",
			 minor(st.st_rdev), gt);
		return -1;
	}

	while (fgets(tlb_path, sizeof(tlb_path), f)) {
		if (strstr(tlb_path, stat) != NULL) {
			sscanf(tlb_path, "%*[^:]: %d", &count);
			break;
		}
	}

	fclose(f);

	return count;
}

/**
 * xe_gt_is_in_c6:
 * @fd: pointer to xe drm fd
 * @gt: gt number
 *
 * Check if GT is in C6 state
 */
bool xe_gt_is_in_c6(int fd, int gt)
{
	char gt_c_state[16];
	int gt_fd;

	gt_fd = xe_sysfs_gt_open(fd, gt);
	igt_assert(gt_fd >= 0);
	igt_assert(igt_sysfs_scanf(gt_fd, "gtidle/idle_status", "%s", gt_c_state) == 1);
	close(gt_fd);

	return strcmp(gt_c_state, "gt-c6") == 0;
}

/**
 * xe_gt_fill_engines_by_class:
 * @fd: pointer to xe drm fd
 * @gt: gt number
 * @class: engine class to use to filter engines
 * @eci: output argument to copy engines to
 *
 * Fill out @drm_xe_engine_class_instance with all the engines in @gt that have
 * a certain @class.
 *
 * Return: number of engines that match the gt and clas
 */
int xe_gt_fill_engines_by_class(int fd, int gt, int class,
				struct drm_xe_engine_class_instance eci[static XE_MAX_ENGINE_INSTANCE])
{
	struct drm_xe_engine_class_instance *hwe;
	int n = 0;

	xe_for_each_engine(fd, hwe)
		if (hwe->engine_class == class && hwe->gt_id == gt)
			eci[n++] = *hwe;

	return n;
}

/**
 * xe_gt_count_engines_by_class:
 * @fd: pointer to xe drm fd
 * @gt: gt number
 * @class: engine class to use to filter engines
 *
 * Count number of engines in @gt that have a certain @class.
 *
 * Return: number of engines that match the gt and clas
 */
int xe_gt_count_engines_by_class(int fd, int gt, int class)
{
	struct drm_xe_engine_class_instance *hwe;
	int n = 0;

	xe_for_each_engine(fd, hwe)
		if (hwe->engine_class == class && hwe->gt_id == gt)
			n++;

	return n;
}

/**
 * xe_gt_set_freq:
 * @fd: pointer to xe drm fd
 * @gt_id: GT id
 * @freq_name: which GT freq(min, max) to change
 * @freq: value of freq to set
 *
 * Set GT min/max frequency. Function will assert if the sysfs node is
 * not found.
 *
 * Return: success or failure
 */
int xe_gt_set_freq(int fd, int gt_id, const char *freq_name, uint32_t freq)
{
	int ret = -EAGAIN;
	char freq_attr[NAME_MAX];
	int gt_fd;

	snprintf(freq_attr, sizeof(freq_attr), "freq0/%s_freq", freq_name);
	gt_fd = xe_sysfs_gt_open(fd, gt_id);
	igt_assert_lte(0, gt_fd);

	while (ret == -EAGAIN)
		ret = igt_sysfs_printf(gt_fd, freq_attr, "%u", freq);

	close(gt_fd);

	return ret;
}

/**
 * xe_gt_get_freq:
 * @fd: pointer to xe drm fd
 * @gt_id: GT id
 * @freq_name: which GT freq(min, max, act, cur) to read
 *
 * Read the min/max/act/cur/rp0/rpn/rpe GT frequencies. Function will
 * assert if the sysfs node is not found.
 *
 * Return: GT frequency value
 */
uint32_t xe_gt_get_freq(int fd, int gt_id, const char *freq_name)
{
	uint32_t freq;
	int err = -EAGAIN;
	char freq_attr[NAME_MAX];
	int gt_fd;

	snprintf(freq_attr, sizeof(freq_attr), "freq0/%s_freq", freq_name);
	gt_fd = xe_sysfs_gt_open(fd, gt_id);
	igt_assert_lte(0, gt_fd);

	while (err == -EAGAIN)
		err = igt_sysfs_scanf(gt_fd, freq_attr, "%u", &freq);

	igt_assert_eq(err, 1);

	igt_debug("gt%d: %s freq %u\n", gt_id, freq_name, freq);
	close(gt_fd);

	return freq;
}
