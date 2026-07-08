// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: Test EU Debugger and SR-IOV interaction
 * Category: Core
 * Mega feature: EUdebug/SR-IOV
 * Sub-category: EUdebug tests
 * Functionality: EU Debugger framework
 * Test category: functionality test
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "lib/igt_sriov_device.h"
#include "xe/xe_eudebug.h"

static bool has_vf_enable_eudebug_attr(int fd, unsigned int vf_num)
{
	char path[PATH_MAX];
	int sysfs;
	bool ret;

	igt_assert(vf_num > 0);

	sysfs = igt_sysfs_open(fd);
	igt_assert_fd(sysfs);
	/* vf_num is 1-based, but virtfn is 0-based */
	snprintf(path, sizeof(path), "device/virtfn%u/enable_eudebug", vf_num - 1);
	ret = igt_sysfs_has_attr(sysfs, path);
	close(sysfs);

	return ret;
}

/**
 * SUBTEST: deny-eudebug
 * Mega feature: EUdebug
 * Sub-category: EUdebug framework
 * Functionality: EU debug and SR-IOV
 * Description:
 *	Check that eudebug toggle is not available for VFs, and that enabling
 *	eudebug with VFs enabled is not permitted.
 */
static void test_deny_eudebug(int fd)
{
	unsigned int num_vfs = igt_sriov_get_total_vfs(fd);
	bool err = false;
	int sysfs;

	igt_debug("Testing %u VFs\n", num_vfs);

	xe_eudebug_enable(fd, false);
	igt_sriov_enable_driver_autoprobe(fd);
	igt_sriov_enable_vfs(fd, num_vfs);
	igt_assert_eq(num_vfs, igt_sriov_get_enabled_vfs(fd));

	for (int vf_num = 1; vf_num <= num_vfs; ++vf_num) {
		if (!igt_sriov_is_vf_drm_driver_probed(fd, vf_num)) {
			igt_debug("VF%u probe failed\n", vf_num);
			err = true;
		} else if (has_vf_enable_eudebug_attr(fd, vf_num)) {
			igt_debug("VF%u has enable_eudebug attribute\n", vf_num);
			err = true;
		}
	}

	igt_assert(!err);

	sysfs = igt_sysfs_open(fd);
	igt_assert_fd(sysfs);
	igt_assert_eq(igt_sysfs_printf(sysfs, "device/enable_eudebug", "1"), -EPERM);
	close(sysfs);
}

/**
 * SUBTEST: deny-sriov
 * Mega feature: EUdebug
 * Sub-category: EUdebug framework
 * Functionality: EU debug and SR-IOV
 * Description:
 *	Check that VFs cannot be enabled when eudebug is enabled.
 */
static void test_deny_sriov(int fd)
{
	unsigned int num_vfs = igt_sriov_get_total_vfs(fd);
	int sysfs = 0;

	igt_debug("Testing %u VFs\n", num_vfs);

	igt_sriov_disable_vfs(fd);
	igt_assert_eq(0, igt_sriov_get_enabled_vfs(fd));
	xe_eudebug_enable(fd, true);

	sysfs = igt_sysfs_open(fd);
	igt_assert_fd(sysfs);
	igt_assert_eq(igt_sysfs_printf(sysfs, "device/sriov_numvfs", "%u", num_vfs), -EPERM);
	close(sysfs);
}

static void restore_initial_driver_state(int fd, bool eudebug_enabled, bool vf_autoprobe)
{
	bool abort = false;

	igt_sriov_disable_vfs(fd);
	if (igt_sriov_get_enabled_vfs(fd) > 0) {
		igt_debug("Failed to disable VF(s)\n");
		abort = true;
	}

	vf_autoprobe ? igt_sriov_enable_driver_autoprobe(fd) :
		       igt_sriov_disable_driver_autoprobe(fd);
	if (vf_autoprobe != igt_sriov_is_driver_autoprobe_enabled(fd)) {
		igt_debug("Failed to restore sriov_drivers_autoprobe value\n");
		abort = true;
	}

	if (__xe_eudebug_enable_getset(fd, NULL, &eudebug_enabled) < 0) {
		igt_debug("Failed to restore eudebug state\n");
		abort = true;
	}

	/* abort to avoid execution of next tests with invalid driver state */
	igt_abort_on_f(abort, "Failed to restore initial driver state\n");
}

int igt_main()
{
	bool eudebug_enabled;
	bool vf_autoprobe;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(igt_sriov_is_pf(fd));
		igt_require(igt_sriov_vfs_supported(fd));
		igt_require(igt_sriov_get_enabled_vfs(fd) == 0);
		igt_require(__xe_eudebug_enable_getset(fd, &eudebug_enabled, NULL) == 0);
		vf_autoprobe = igt_sriov_is_driver_autoprobe_enabled(fd);
	}

	igt_subtest("deny-eudebug")
		test_deny_eudebug(fd);

	igt_subtest("deny-sriov")
		test_deny_sriov(fd);

	igt_fixture() {
		restore_initial_driver_state(fd, eudebug_enabled, vf_autoprobe);
		close(fd);
	}
}
