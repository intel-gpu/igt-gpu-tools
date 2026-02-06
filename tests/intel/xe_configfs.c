// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include <limits.h>

#include "igt.h"
#include "igt_configfs.h"
#include "igt_device.h"
#include "igt_fs.h"
#include "igt_kmod.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "xe/xe_query.h"

/**
 * TEST: Check configfs userspace API
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: configfs
 * Description: validate configfs entries
 * Test category: functionality test
 */

static char bus_addr[NAME_MAX];
static struct pci_device *pci_dev;

static bool check_registers(const uint32_t reg[], const uint32_t val[],
			    size_t max)
{
	struct intel_mmio_data mmio_data = { };
	bool ret = false;

	intel_register_access_init(&mmio_data, pci_dev, 0);

	for (int i = 0; i < max && reg[i]; i++) {
		uint32_t v = intel_register_read(&mmio_data, reg[i]);

		if (v != val[i]) {
			igt_debug("Expecting [%x]=%x but found %x\n",
				  reg[i], val[i], v);
			goto out;
		}
	}

	ret = true;

out:
	intel_register_access_fini(&mmio_data);
	return ret;
}

static void restore(int sig)
{
	int configfs_fd;

	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	/* Drop all custom configfs settings from subtests */
	configfs_fd = igt_configfs_open("xe");
	if (configfs_fd >= 0)
		igt_fs_remove_dir(configfs_fd, bus_addr);
	close(configfs_fd);

	/* Bind again a clean driver with no custom settings */
	igt_kmod_bind("xe", bus_addr);
}

static void set_survivability_mode(int configfs_device_fd, bool value)
{
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);
	igt_sysfs_set_boolean(configfs_device_fd, "survivability_mode", value);
	igt_kmod_bind("xe", bus_addr);
}

/**
 * SUBTEST: survivability-mode
 * Description: Validate survivability mode by setting configfs
 */
static void test_survivability_mode(int configfs_device_fd)
{
	char path[PATH_MAX];
	int fd;

	/* Enable survivability mode */
	set_survivability_mode(configfs_device_fd, true);

	/* check presence of survivability mode sysfs */
	snprintf(path, PATH_MAX, "/sys/bus/pci/devices/%s/survivability_mode", bus_addr);

	fd = open(path, O_RDONLY);
	igt_assert_f(fd >= 0, "Survivability mode not set\n");
	close(fd);
}

/**
 * SUBTEST: engines-allowed-invalid
 * Description: Validate engines_allowed attribute for invalid values
 */
static void test_engines_allowed_invalid(int configfs_device_fd)
{
	static const char *values[] = {
		"xcs0",
		"abcsdcs0",
		"rcs0,abcsdcs0",
		"rcs9",
		"rcs10",
		"rcs0asdf",
	};

	/*
	 * These only test if engine parsing is correct, so just make sure
	 * there's no device bound
	 */
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		const char *v = values[i];

		igt_debug("Writing '%s' to engines_allowed\n", v);
		igt_assert(!igt_sysfs_set(configfs_device_fd, "engines_allowed", v));
	}
}

/**
 * SUBTEST: engines-allowed
 * Description: Validate engines_allowed attribute
 */
static void test_engines_allowed(int configfs_device_fd)
{
	static const char *values[] = {
		"rcs0", "rcs*", "rcs0,bcs0", "bcs0,rcs0",
		"bcs0\nrcs0", "bcs0\nrcs0\n",
		"rcs000",
	};

	/*
	 * These only test if engine parsing is correct, so just make sure
	 * there's no device bound
	 */
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		const char *v = values[i];

		igt_debug("Writing '%s' to engines_allowed\n", v);
		igt_assert(igt_sysfs_set(configfs_device_fd, "engines_allowed", v));
	}
}

/**
 * SUBTEST: gt-types-allowed
 * Description: Validate gt_types_allowed attribute
 */
static void test_gt_types_allowed(int configfs_device_fd)
{
	static const char *values[] = {
		"primary,media", "primary", "media", "",
		"media,primary", "primary\n", "primary\nmedia",
		"media\n", "media\nprimary\n",
	};

	static const char *invalid_values[] = {
                "check",
        };

	/*
	 * These only test if gt type parsing is correct, so just make sure
	 * there's no device bound
	 */
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		const char *v = values[i];

		igt_debug("Writing '%s' to gt_types_allowed\n", v);
		igt_assert(igt_sysfs_set(configfs_device_fd, "gt_types_allowed", v));
	}

	for (size_t i = 0; i < ARRAY_SIZE(invalid_values); i++) {
		const char *v = invalid_values[i];

		igt_debug("Writing '%s' to gt_types_allowed\n", v);
		igt_assert(!igt_sysfs_set(configfs_device_fd, "gt_types_allowed", v));
	}
}

/**
 * SUBTEST: ctx-restore-post-bb-invalid
 * Description: Validate ctx_restore_post_bb attribute for invalid values
 *
 * SUBTEST: ctx-restore-mid-bb-invalid
 * Description: Validate ctx_restore_mid_bb attribute for invalid values
 */
static void test_ctx_restore_invalid(int configfs_device_fd, const char *type)
{
	static const struct value {
		const char *test;
		const char *in;
	} values[] = {
		{ .test = "invalid-engine",
		  .in = "foobar cmd 11000001 4F100 DEADBEEF",
		},
		{ .test = "invalid-type",
		  .in = "rcs 11000001 4F100 DEADBEEF",
		},
		{ .test = "invalid-number-start",
		  .in = "rcs cmd g1100000 4F100 DEADBEEF",
		},
		{ .test = "invalid-number-middle",
		  .in = "rcs cmd 11g00000 4F100 DEADBEEF",
		},
		{ .test = "invalid-number-end",
		  .in = "rcs cmd 1100000g 4F100 DEADBEEF",
		},
		{ .test = "invalid-reg-addr-only",
		  .in = "rcs reg 4F100",
		},
		{ .test = "invalid-engine-instance",
		  .in = "rcs0 reg 4F100 DEADBEEF",
		},
	};
	char buf[4096] = { };
	char file[64] = { };

	snprintf(file, sizeof(file), "ctx_restore_%s_bb", type);
	igt_sysfs_set(configfs_device_fd, "ctx_restore_post_bb", "");

	/*
	 * These only test if command parsing is correct,
	 * so just make sure there's no device bound
	 */
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", bus_addr);

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		const struct value *v = &values[i];

		igt_info("Test %s\n", v->test);
		igt_debug("bb '%s'\n", v->in);
		igt_assert(!igt_sysfs_set(configfs_device_fd, file, v->in));
		igt_assert(igt_sysfs_read(configfs_device_fd, file, buf,
					  sizeof(buf) - 1));
		if (strcmp(buf, "")) {
			igt_debug("Expecting empty bb, but found '%s'\n", buf);
			igt_fail(IGT_EXIT_FAILURE);
		}
	}
}

/**
 * SUBTEST: ctx-restore-post-bb
 * Description: Validate ctx_restore_post_bb attribute
 *
 * SUBTEST: ctx-restore-mid-bb
 * Description: Validate ctx_restore_mid_bb attribute
 */
static void test_ctx_restore(int configfs_device_fd, const char *type)
{
	static const struct value {
		const char *test;
		const char *in;
		const char *out;
		uint32_t reg[4];
		uint32_t reg_val[4];
	} values[] = {
		/*
		 * values for the registers just keep incrementing on different
		 * tests to avoid having tests passing just because the
		 * previous execution set a specific value in the HW
		 */
		{ .test = "cmd-single",
		  .in = "rcs cmd 11000001 4F100 DEA0BEE0",
		  .out = "rcs: 11000001 0004f100 dea0bee0\n",
		  .reg = { 0x4f100 },
		  .reg_val = { 0xdea0bee0 },
		},
		{ .test = "cmd-single-multi-values",
		  .in = "rcs cmd 11000003 4F100 DEA1BEE1 4F104 DEA2BEE2",
		  .out = "rcs: 11000003 0004f100 dea1bee1 0004f104 dea2bee2\n",
		  .reg = { 0x4f100, 0x4f104 },
		  .reg_val = { 0xdea1bee1, 0xdea2bee2 },
		},
		{ .test = "cmd-multi",
		  .in = "rcs cmd 11000001 4F100 DEA3BEE3\n"
			"rcs cmd 11000001 4F104 DEA4BEE4",
		  .out = "rcs: 11000001 0004f100 dea3bee3 11000001 0004f104 dea4bee4\n",
		  .reg = { 0x4f100, 0x4f104 },
		  .reg_val = { 0xdea3bee3, 0xdea4bee4 },
		},
		{ .test = "reg-single",
		  .in = "rcs reg 4F100 DEA5BEE5",
		  .out = "rcs: 11000001 0004f100 dea5bee5\n",
		  .reg = { 0x4f100 },
		  .reg_val = { 0xdea5bee5 },
		},
		{ .test = "reg-multi",
		  .in = "rcs reg 4F100 DEA6BEE6\n"
			"rcs reg 4F104 DEA7BEE7",
		  .out = "rcs: 11000001 0004f100 dea6bee6 11000001 0004f104 dea7bee7\n",
		  .reg = { 0x4f100, 0x4f104 },
		  .reg_val = { 0xdea6bee6, 0xdea7bee7 },
		},
	};
	char buf[4096] = { };
	char file[64] = { };

	snprintf(file, sizeof(file), "ctx_restore_%s_bb", type);

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		const struct value *v = &values[i];

		igt_audio_driver_unload(NULL);
		igt_kmod_unbind("xe", bus_addr);

		igt_info("Test %s\n", v->test);
		igt_debug("bb '%s'\n", v->in);
		igt_assert(igt_sysfs_set(configfs_device_fd, file, v->in));

		igt_assert(igt_sysfs_read(configfs_device_fd, file, buf,
					  sizeof(buf) - 1));
		if (strcmp(v->out, buf)) {
			igt_debug("Expecting '%s' but found '%s'\n", v->out, buf);
			igt_fail(IGT_EXIT_FAILURE);
		}

		igt_kmod_bind("xe", bus_addr);
		igt_assert(check_registers(v->reg, v->reg_val, sizeof(v->reg)));
	}
}

static void set_bus_addr(int fd)
{
	pci_dev = igt_device_get_pci_device(fd);
	snprintf(bus_addr, sizeof(bus_addr), "%04x:%02x:%02x.%01x",
		 pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func);
}

static int create_device_configfs_group(int configfs_fd)
{
	mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	int configfs_device_fd;

	configfs_device_fd = igt_fs_create_dir(configfs_fd, bus_addr, mode);
	igt_assert(configfs_device_fd);

	return configfs_device_fd;
}

int igt_main()
{
	int fd, configfs_fd, configfs_device_fd;
	uint32_t devid;
	bool is_vf_device;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		devid = intel_get_drm_devid(fd);
		is_vf_device = intel_is_vf_device(fd);
		set_bus_addr(fd);
		drm_close_driver(fd);

		configfs_fd = igt_configfs_open("xe");
		igt_require(configfs_fd != -1);
		configfs_device_fd = create_device_configfs_group(configfs_fd);
		igt_install_exit_handler(restore);
	}

	igt_describe("Validate survivability mode");
	igt_subtest("survivability-mode") {
		igt_require(IS_BATTLEMAGE(devid));
		igt_require_f(!is_vf_device, "survivability mode not supported in VF\n");
		test_survivability_mode(configfs_device_fd);
	}

	igt_describe("Validate engines_allowed with invalid options");
	igt_subtest("engines-allowed-invalid")
		test_engines_allowed_invalid(configfs_device_fd);

	igt_describe("Validate engines_allowed");
	igt_subtest("engines-allowed")
		test_engines_allowed(configfs_device_fd);

	igt_describe("Validate gt_types_allowed");
	igt_subtest("gt-types-allowed")
		test_gt_types_allowed(configfs_device_fd);

	igt_describe("Validate ctx_restore_post_bb with invalid options");
	igt_subtest("ctx-restore-post-bb-invalid")
		test_ctx_restore_invalid(configfs_device_fd, "post");

	igt_describe("Validate ctx_restore_post_bb");
	igt_subtest("ctx-restore-post-bb")
		test_ctx_restore(configfs_device_fd, "post");

	igt_describe("Validate ctx_restore_mid_bb with invalid options");
	igt_subtest("ctx-restore-mid-bb-invalid")
		test_ctx_restore_invalid(configfs_device_fd, "mid");

	igt_describe("Validate ctx_restore_mid_bb");
	igt_subtest("ctx-restore-mid-bb")
		test_ctx_restore(configfs_device_fd, "mid");

	igt_fixture() {
		close(configfs_device_fd);
		close(configfs_fd);
	}
}
