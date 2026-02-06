// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include <dirent.h>
#include <fcntl.h>
#include <libudev.h>
#include <limits.h>
#include <poll.h>

#include "igt.h"
#include "igt_configfs.h"
#include "igt_device.h"
#include "igt_fs.h"
#include "igt_kmod.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "intel_allocator.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"

/**
 * TEST: Comprehensive survivability mode testing
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Telemetry
 * Functionality: survivability mode
 * Description: Validate survivability mode functionality
 * Test category: Functional tests
 *
 * SUBTEST: i2c-functionality
 * Description: Validate i2c adapter functionality in survivability mode
 *
 * SUBTEST: runtime-survivability
 * Description: Force Xe device wedged after injecting a failure in CSC
 * to test runtime survivability mode
 */

static char bus_addr[NAME_MAX];

static void ignore_wedged_in_dmesg(void)
{
	/* this is needed for igt_runner so it will ignore it */
	igt_emit_ignore_dmesg_regex("GT[0-9A-Fa-f]*: failed to enable GuC scheduling policies: -ECANCELED"
				    "|CRITICAL: Xe has declared device [0-9A-Fa-f:.]* as wedged"
				    "|GT[0-9A-Fa-f]*: reset failed .-ECANCELED"
				    "|GT[0-9A-Fa-f]*: Failed to submit"
				    "|Modules linked in:"
				    "|__pfx___drm_");
}

static bool check_survivability_mode_sysfs(void)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/survivability_mode", bus_addr);
	fd = open(path, O_RDONLY);
	igt_assert_f(fd >= 0, "Survivability mode not set\n");
	close(fd);
	return true;
}

static void check_survivability_and_uevents(int fd, struct udev_monitor *mon)
{
	struct udev_device *dev;
	const char *prop_val;
	const char *dev_path;
	struct pollfd poll_fd = {
		.fd = udev_monitor_get_fd(mon),
		.events = POLLIN
	};
	bool event_received = false;
	int timeout_secs = 30;

	check_survivability_mode_sysfs();

	igt_until_timeout(timeout_secs) {
		if (poll(&poll_fd, 1, 1000) <= 0)
			continue;

		dev = udev_monitor_receive_device(mon);
		if (!dev)
			continue;

		prop_val = udev_device_get_property_value(dev, "WEDGED");
		dev_path = udev_device_get_property_value(dev, "DEVPATH");

		if (prop_val && !strcmp(prop_val, "vendor-specific")) {
			event_received = true;
			igt_assert_f(dev_path && strstr(dev_path, bus_addr),
				     "Expected bus address '%s' to be part of DEVPATH '%s'",
				     bus_addr, dev_path);
			udev_device_unref(dev);
			break;
		}

		udev_device_unref(dev);
	}

	igt_cleanup_uevents(mon);

	igt_assert_f(event_received,
		     "Timeout waiting for vendor-specific wedged event after %d seconds",
		     timeout_secs);
}

static void force_wedged_csc_error(int fd)
{
	igt_debugfs_write(fd, "inject_csc_hw_error/probability", "100");
	igt_debugfs_write(fd, "inject_csc_hw_error/times", "1");

	xe_force_gt_reset_sync(fd, 0);
	sleep(1);
}

static int find_i2c_adapter(struct pci_device *pci_xe)
{
	char device_path[PATH_MAX];
	struct dirent *dirent;
	int i2c_adapter = -1;
	DIR *device_dir;
	int ret;

	igt_require(igt_kmod_load("i2c-dev", NULL) == 0);

	snprintf(device_path, sizeof(device_path), "/sys/bus/pci/devices/%s/%s.%hu", bus_addr,
		 "i2c_designware", (pci_xe->bus << 8) | (pci_xe->dev));
	device_dir = opendir(device_path);

	if (!device_dir)
		return -1;

	while ((dirent = readdir(device_dir))) {
		if (strncmp(dirent->d_name, "i2c-", 4) == 0) {
			ret = sscanf(dirent->d_name, "i2c-%d", &i2c_adapter);
			igt_assert_f(ret == 1, "Failed to parse i2c adapter number");
			closedir(device_dir);
			return i2c_adapter;
		}
	}

	closedir(device_dir);
	return i2c_adapter;
}

static void restore(int sig)
{
	int configfs_fd;

	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	configfs_fd = igt_configfs_open("xe");
	if (configfs_fd >= 0)
		igt_fs_remove_dir(configfs_fd, bus_addr);
	close(configfs_fd);

	igt_kmod_bind("xe", bus_addr);
}

static void set_survivability_mode(int configfs_device_fd, bool value)
{
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);
	igt_sysfs_set_boolean(configfs_device_fd, "survivability_mode", value);
	igt_kmod_bind("xe", bus_addr);
}

static void test_i2c_functionality(int configfs_device_fd, struct pci_device *pci_xe)
{
	if (find_i2c_adapter(pci_xe) >= 0) {
		/* Enable survivability mode */
		set_survivability_mode(configfs_device_fd, true);

		/* check presence of survivability mode sysfs */
		check_survivability_mode_sysfs();

		/* Check i2c adapter after survivability mode */
		igt_assert_f(find_i2c_adapter(pci_xe) >= 0,
			     "i2c not initialized\n");

		set_survivability_mode(configfs_device_fd, false);
	}
}

static int create_device_configfs_group(int configfs_fd)
{
	mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	int configfs_device_fd;

	configfs_device_fd = igt_fs_create_dir(configfs_fd, bus_addr, mode);
	igt_assert(configfs_device_fd);

	return configfs_device_fd;
}

static void test_spinner_after_recovery(int fd)
{
	uint64_t ahnd;
	igt_spin_t *spin;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	spin = igt_spin_new(fd, .ahnd = ahnd);

	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

int igt_main()
{
	int fd, configfs_fd, configfs_device_fd;
	struct pci_device *pci_xe;
	bool vf_device;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(IS_BATTLEMAGE(intel_get_drm_devid(fd)));
		vf_device = intel_is_vf_device(fd);
		igt_require_f(!vf_device, "survivability mode not supported in VF\n");
		pci_xe = igt_device_get_pci_device(fd);
		igt_device_get_pci_slot_name(fd, bus_addr);
		configfs_fd = igt_configfs_open("xe");
		igt_require(configfs_fd != -1);
		configfs_device_fd = create_device_configfs_group(configfs_fd);
		igt_install_exit_handler(restore);
	}

	igt_describe("Validate i2c adapter functionality in survivability mode");
	igt_subtest("i2c-functionality") {
		test_i2c_functionality(configfs_device_fd, pci_xe);
		drm_close_driver(fd);
		fd = drm_open_driver(DRIVER_XE);
	}

	igt_describe("Inject CSC error to test device wedge and runtime survivability");
	igt_subtest("runtime-survivability") {
		struct udev_monitor *mon;

		igt_require(igt_debugfs_exists(fd, "inject_csc_hw_error/probability",
					       O_RDWR));

		igt_debugfs_write(fd, "inject_csc_hw_error/verbose", "1");

		ignore_wedged_in_dmesg();
		mon = igt_watch_uevents();
		force_wedged_csc_error(fd);

		check_survivability_and_uevents(fd, mon);

		drm_close_driver(fd);
		igt_kmod_rebind("xe", bus_addr);
		fd = drm_open_driver(DRIVER_XE);

		test_spinner_after_recovery(fd);

		igt_debugfs_write(fd, "inject_csc_hw_error/probability", "0");
		igt_debugfs_write(fd, "inject_csc_hw_error/times", "1");
	}

	igt_fixture() {
		igt_fs_remove_dir(configfs_fd, bus_addr);
		close(configfs_device_fd);
		close(configfs_fd);
		drm_close_driver(fd);
	}
}
