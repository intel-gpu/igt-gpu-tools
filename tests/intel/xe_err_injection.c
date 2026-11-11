// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: Test Xe RAS (Reliability, Availability, Serviceability) MMIO based Error Injection
 * Category: Core
 * Mega feature: RAS
 * Sub-category: RAS tests
 * Functionality: Error Injection
 * Test category: Functional tests
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "lib/igt_drm_ras_nl.h"
#include "lib/intel_chipset.h"
#include "lib/intel_reg.h"
#include "lib/intel_compute.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_mmio.h"
#include "xe/xe_query.h"

/* Steering Control Register (MC_PKT_CTRL) */
#define XE_MC_PKT_CTRL_MGSR_3D_ADDRESS 0x00FD4
#define XE_MC_PKT_CTRL_MGSR_3D_VALUE 0x80000000

/* Core Compute Uncorrectable error injection */
#define XE_WKR_FABRIC_ERR_INJ_GAMWALK_3D_ADDRESS 0xF310
#define XE_WKR_FABRIC_ERR_INJ_GAMWALK_3D_VALUE 0x1

#define POST_RECOVERY_WL 1
#define POST_ARMING_INJECTION_WL 0

/* STEER_SEMAPHORE_MGSR_3D */
#define XE_STEER_SEMAPHORE_MGSR_3D 0x00FD0
#define XE_STEER_SEMAPHORE_MGSR_3D_VALUE 0x1

/* Node types for error counters*/
#define NODE_ID_CORRECTABLE_ERROR 0
#define NODE_ID_UNCORRECTABLE_ERROR 1

/* Error types for error counter */
#define ERROR_ID_CORE_COMPUTE 1

/* Core Compute Correctable error injection */
#define XE_L2_BANK_ERR_INJ 0xB1B4
#define XE_L2_BANK_DATA_1BIT_ERR_INJ_CONFIG 0xC000C

enum {
	SUCCESS = 1,
	FAILED,
	TIMEOUT,
};

#define INJ_ONCE 1
#define INJ_MANY 16
#define THRESHOLD_NONE 0
#define THRESHOLD_ONE 1

time_t event_timeout = 10 * 1000; /* 10 seconds */

static bool run_xe_compute_on_all_engines(int fd, bool state)
{
	struct drm_xe_engine_class_instance *hwe;
	bool found_engine = false;
	bool all_succeeded = true;

	igt_info("Running compute-square on all engines post %s\n",
		 state == POST_ARMING_INJECTION_WL ? "injection" : "recovery");

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
			continue;

		found_engine = true;
		if (!xe_run_intel_compute_kernel_on_engine(fd, hwe, NULL,
							   EXECENV_PREF_SYSTEM))
			all_succeeded = false;
	}

	return found_engine && all_succeeded;
}

static void modify_reg_bit(struct xe_mmio *mmio, uint32_t reg_addr,
			   uint32_t bit_mask, bool set)
{
	uint32_t regval;

	regval = xe_mmio_read32(mmio, reg_addr);
	if (set)
		regval |= bit_mask;
	else
		regval &= ~bit_mask;

	xe_mmio_write32(mmio, reg_addr, regval);
}

static bool acquire_steering_semaphore(struct xe_mmio *mmio)
{
	uint32_t regval;
	time_t start_time = time(NULL);
	time_t timeout = 10; /* 10 secs */

	while (time(NULL) - start_time < timeout) {
		regval = xe_mmio_read32(mmio, XE_STEER_SEMAPHORE_MGSR_3D);
		if (regval == XE_STEER_SEMAPHORE_MGSR_3D_VALUE)
			return true;
		sleep(1);
	}

	return false;
}

static void release_steering_semaphore(struct xe_mmio *mmio)
{
	xe_mmio_write32(mmio, XE_STEER_SEMAPHORE_MGSR_3D,
			XE_STEER_SEMAPHORE_MGSR_3D_VALUE);
}

static int acquire_forcewake(int fd)
{
	int fw_handle;

	fw_handle = igt_debugfs_open(fd, "forcewake_all", O_RDONLY);
	igt_assert_lte(0, fw_handle);
	return fw_handle;
}

static void release_forcewake(int fw_handle)
{
	if (fw_handle >= 0)
		close(fw_handle);
}

static void cleanup_on_error(struct igt_drm_ras_nl_context *ctx, int fw_handle)
{
	igt_drm_ras_nl_cleanup(ctx);
	release_forcewake(fw_handle);
}

static int get_counter(uint32_t node_id, uint32_t error_id)
{
	struct igt_drm_ras_nl_context ctx;
	uint32_t error_value = UINT32_MAX;
	int ret;

	ret = igt_drm_ras_nl_init(&ctx);
	if (ret < 0) {
		igt_warn("Failed to initialize socket for get_error command (ret=%d)\n", ret);
		return ret;
	}

	ctx.node_id = node_id;
	ctx.error_id = error_id;

	ret = igt_drm_ras_nl_get_error_counter(&ctx);
	if (ret < 0) {
		igt_warn("get_error_counter failed from error command path (ret=%d)\n", ret);
		igt_drm_ras_nl_cleanup(&ctx);
		return ret;
	} else {
		error_value = ctx.error_value;
		igt_info("get_error_counter: node_id=%u error_id=%u value=%u\n",
			 ctx.node_id, ctx.error_id, ctx.error_value);
	}

	igt_drm_ras_nl_cleanup(&ctx);

	return error_value;
}

static int check_dmesg(const char *marker, int elapsed_secs)
{
	static const char * const dmesg_status[] = {
		[SUCCESS] = "AER: device recovery successful",
		[FAILED] = "AER: device recovery failed",
	};
	char *buff = NULL;
	size_t buff_size = 0;
	size_t i;
	ssize_t line_len;
	FILE *fp;
	bool marker_seen = false;

	fp = popen("dmesg", "r");
	if (!fp) {
		igt_warn("Unable to open dmesg to check recovery status\n");
		return -1;
	}

	while ((line_len = getline(&buff, &buff_size, fp)) != -1) {
		if (!marker_seen) {
			if (strstr(buff, marker))
				marker_seen = true;
			continue;
		}

		for (i = SUCCESS; i < ARRAY_SIZE(dmesg_status); i++) {
			if (!dmesg_status[i] || !strstr(buff, dmesg_status[i]))
				continue;

			igt_debug("Found \"%s\" in dmesg after %d secs\n",
				  dmesg_status[i], elapsed_secs);
			free(buff);
			pclose(fp);
			return i;
		}
	}

	free(buff);
	pclose(fp);
	return TIMEOUT;
}

/*
 * TODO: AER device recovery logic is pending to be implemented using
 * "uevents emitted by the aer driver" instead of relying on the dmesg
 * logs.
 */

static int check_err_recovery(void)
{
	time_t start_time = time(NULL);
	time_t timeout = 10 * 60; /* 10 mins */
	int time_interval = 30; /* 30 secs */
	time_t elapsed_time;
	char marker[128];
	int status;

	snprintf(marker, sizeof(marker),
		 "IGT xe_err_injection recovery marker pid=%d start=%lld",
		 getpid(), (long long)start_time);
	igt_kmsg(KMSG_INFO "%s\n", marker);

	while (1) {
		elapsed_time = time(NULL) - start_time;
		status = check_dmesg(marker, elapsed_time);
		if (status < 0) {
			igt_warn("Failed to query dmesg for recovery status\n");
			return FAILED;
		}
		if (status == SUCCESS || status == FAILED)
			return status;

		if (elapsed_time >= timeout) {
			igt_warn("Timed out while waiting for error recovery\n");
			return TIMEOUT;
		}

		sleep(time_interval);
	}
}

static void print_aer_recovery_status(void)
{
	int recovery_ret;

	recovery_ret = check_err_recovery();
	igt_assert_f(recovery_ret != TIMEOUT,
		     "AER error recovery timed out\n");
	igt_assert_f(recovery_ret != FAILED,
		     "AER device recovery failed\n");
	if (recovery_ret == SUCCESS)
		igt_info("AER device recovery successful\n");
}

static bool uncorr_error_recovered;

/**
 * SUBTEST: inject_core_compute_uncorr_error
 * Description: Inject a Core Compute Uncorrectable error and verify
 *		AER-based device recovery.
 *		The dynamic subtests are:
 *		inject            - Injects Core Compute Uncorrectable error
 *				    and verifies device recovery.
 *		post-recovery-wl  - Runs a compute workload on all compute
 *				    engines after recovery to confirm the GPU
 *				    remains functional.
 * Functionality: error injection
 */
static void test_inject_core_compute_uncorr_error(struct xe_mmio *mmio, int fd)
{
	int counter_before;
	int counter_after;
	int fw_handle;
	bool ret;

	/** TODO
	 *  Fetching node_id and error_id dynamically is pending to implement.
	 *  For now, using
	 *  node_id=1 (uncorrectable_errors) and
	 *  error_id=1 (core_compute)
	 */
	uint32_t node_id = NODE_ID_UNCORRECTABLE_ERROR;
	uint32_t error_id = ERROR_ID_CORE_COMPUTE;

	fw_handle = acquire_forcewake(fd);

	counter_before = get_counter(node_id, error_id);
	if (counter_before < 0) {
		release_forcewake(fw_handle);
		igt_assert_f(false, "Failed to read error counter before injection\n");
	}

	if (!acquire_steering_semaphore(mmio)) {
		release_forcewake(fw_handle);
		igt_assert_f(false, "Timed out acquiring steering semaphore\n");
	}

	/* Arm the injection sequence. */
	xe_mmio_write32(mmio, XE_MC_PKT_CTRL_MGSR_3D_ADDRESS, 0x0);
	modify_reg_bit(mmio,
		       XE_WKR_FABRIC_ERR_INJ_GAMWALK_3D_ADDRESS,
		       XE_WKR_FABRIC_ERR_INJ_GAMWALK_3D_VALUE,
		       true);
	modify_reg_bit(mmio,
		       XE_MC_PKT_CTRL_MGSR_3D_ADDRESS,
		       XE_MC_PKT_CTRL_MGSR_3D_VALUE,
		       true);
	release_steering_semaphore(mmio);
	igt_debug("Injected Core Compute Uncorrectable error\n");

	/* Run traffic to trigger the injected error */
	ret = run_xe_compute_on_all_engines(fd, POST_ARMING_INJECTION_WL);
	if (!ret) {
		release_forcewake(fw_handle);
		igt_assert_f(false, "Failed to run Xe compute on all engines post injection\n");
	}

	release_forcewake(fw_handle);
	print_aer_recovery_status();

	counter_after = get_counter(node_id, error_id);
	igt_assert_f(counter_after >= 0, "Failed to read error counter after injection\n");

	igt_assert_f(counter_before != UINT32_MAX && counter_after != UINT32_MAX,
		     "Failed to fetch valid error counters: before=%u after=%u\n",
		     counter_before, counter_after);

	igt_info("error counter: before injection=%u after injection=%u\n",
		 counter_before, counter_after);

	igt_assert_f(counter_after > counter_before,
		     "Core Compute Uncorrectable error injection failed: "
		     "before=%u after=%u\n", counter_before, counter_after);
	uncorr_error_recovered = true;
}

static void inject_core_compute_corr_error(struct xe_mmio *mmio, int fd)
{
	/* Disable Multicast for unicast error injection */
	acquire_steering_semaphore(mmio);
	xe_mmio_write32(mmio, XE_MC_PKT_CTRL_MGSR_3D_ADDRESS, 0x0);

	/* Arm the Injection */
	xe_mmio_write32(mmio, XE_L2_BANK_ERR_INJ, XE_L2_BANK_DATA_1BIT_ERR_INJ_CONFIG);

	/* Rollback to Multicast */
	modify_reg_bit(mmio,
		       XE_MC_PKT_CTRL_MGSR_3D_ADDRESS,
		       XE_MC_PKT_CTRL_MGSR_3D_VALUE,
		       true);
	release_steering_semaphore(mmio);
}

/**
 * SUBTEST: inject_core_compute_corr_error_threshold_one
 * Description: Inject one Core Compute Correctable Error with the
 *		threshold set to 1 and verify exceed-threshold reporting.
 * Functionality: error injection
 *
 * SUBTEST: inject_core_compute_corr_error_threshold_default
 * Description: Inject enough Core Compute Correctable Errors to cross
 *		the current threshold and verify exceed-threshold reporting.
 * Functionality: error injection
 *
 * SUBTEST: inject_core_compute_corr_error_once
 * Description: Inject one Core Compute Correctable Error and verify the
 *		error counter increments by 1.
 * Functionality: error injection
 */
static void test_inject_core_compute_corr_error(struct xe_mmio *mmio, int fd,
						uint32_t ntimes, uint32_t threshold,
						bool check_event)
{
	uint32_t counter_before = 0, counter_after = 0;
	uint32_t current_threshold = UINT32_MAX;
	struct igt_drm_ras_nl_context ctx;
	int fw_handle, ret;

	fw_handle = acquire_forcewake(fd);

	ret = igt_drm_ras_nl_init(&ctx);
	if (ret < 0) {
		release_forcewake(fw_handle);
		igt_assert_f(false, "Failed to initialize netlink socket (ret=%d)\n", ret);
	}

	/** TODO
	 *  Fetching node_id and error_id dynamically is pending to implement.
	 *  For now, using
	 *  node_id=0 (correctable_errors) and
	 *  error_id=1 (core_compute)
	 */
	ctx.node_id = NODE_ID_CORRECTABLE_ERROR;
	ctx.error_id = ERROR_ID_CORE_COMPUTE;

	/* Query the error counter before injection */
	counter_before = get_counter(ctx.node_id, ctx.error_id);
	if (counter_before < 0) {
		cleanup_on_error(&ctx, fw_handle);
		igt_assert_f(false,
			     "Failed to query Core Compute Correctable error counter before injection\n");
	}

	/* Query the current error threshold before injection */
	ret = igt_drm_ras_nl_get_error_threshold(&ctx);
	if (ret < 0) {
		cleanup_on_error(&ctx, fw_handle);
		igt_assert_f(false,
			     "Failed to query Core Compute correctable error threshold before injection\n");
	}
	current_threshold = ctx.error_threshold;

	if (threshold == THRESHOLD_NONE && ntimes != INJ_ONCE) {
		/* set ntimes to current_threshold for injecting enough times to exceed threshold*/
		ntimes = current_threshold;
	}

	if (threshold != THRESHOLD_NONE) {
		/* Set the error threshold to the specified value */
		ctx.error_threshold = threshold;
		ret = igt_drm_ras_nl_set_error_threshold(&ctx);
		if (ret < 0) {
			cleanup_on_error(&ctx, fw_handle);
			igt_assert_f(false, "Failed to set Core Compute correctable error threshold before injection\n");
		}
	}

	if (check_event) {
		/* Subscribe to error event */
		ret = igt_drm_ras_nl_subscribe_error_report(&ctx, DRM_RAS_MCGRP_ERROR_REPORT);
		if (ret < 0) {
			cleanup_on_error(&ctx, fw_handle);
			igt_assert_f(false, "Failed to subscribe to error event (ret=%d)\n", ret);
		}
	}

	for (uint32_t i = 0; i < ntimes; i++) {
		igt_debug("****** Injection iteration: %u ***********\n", i + 1);
		inject_core_compute_corr_error(mmio, fd);
	}

	if (check_event) {
		/* wait 10 secs for error report event */
		ret = igt_drm_ras_nl_wait_for_error_report_event(&ctx, event_timeout);
		if (ret == 0 && ctx.event_error_value >= ctx.error_threshold) {
			igt_info("Success: Received error event notification from: "
				 "group=%s node_id=%u error_id=%u error_value=%u match=%d\n",
				 DRM_RAS_MCGRP_ERROR_REPORT, ctx.event_node_id, ctx.event_error_id,
				 ctx.event_error_value, ctx.event_received);
		} else {
			if (threshold != THRESHOLD_NONE) {
				/* restore threshold to original value before exit */
				ctx.error_threshold = current_threshold;
				igt_drm_ras_nl_set_error_threshold(&ctx);
			}
			cleanup_on_error(&ctx, fw_handle);
			igt_assert_f(false,
				     "Failed: NOT received error event notification from: "
				     "group=%s node_id=%u error_id=%u error_value=%u match=%d\n",
				     DRM_RAS_MCGRP_ERROR_REPORT, ctx.event_node_id,
				     ctx.event_error_id, ctx.event_error_value,
				     ctx.event_received);
		}
	}

	if (threshold != THRESHOLD_NONE) {
		/* restore Core Compute Correctable error threshold to original value */
		ctx.error_threshold = current_threshold;
		ret = igt_drm_ras_nl_set_error_threshold(&ctx);
		if (ret < 0)
			igt_warn("Failed to restore Core Compute Correctable threshold to original value\n");
	}

	ret = igt_drm_ras_nl_get_error_counter(&ctx);
	if (ret < 0) {
		cleanup_on_error(&ctx, fw_handle);
		igt_assert_f(false,
			     "Failed to query Core Compute Correctable error counter after injection\n");
	}
	counter_after = ctx.error_value;

	igt_info("error counter: before injection=%u after injection=%u\n",
		 counter_before, counter_after);
	igt_assert_f(counter_after - counter_before == ntimes,
		     "Core Compute Correctable error injection Failed\n");

	cleanup_on_error(&ctx, fw_handle);
}

static void require_cri_gpu(int fd)
{
	uint16_t dev_id = xe_dev_id(fd);

	igt_require_f(IS_CRESCENTISLAND(dev_id),
		      "This test requires CRI GPU, but found device ID 0x%04x\n", dev_id);
}

enum xe_err_injection_type {
	XE_CORE_COMPUTE_UNCORR_ERR_INJ,
	XE_CORE_COMPUTE_UNCORR_ERR_INJ_POST_RECOVERY_WL,
	XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_ONE,
	XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_DEFAULT,
	XE_CORE_COMPUTE_CORR_ERR_INJ_ONCE,
};

static void inject_error(enum xe_err_injection_type injection, struct xe_mmio *mmio, int fd)
{
	switch (injection) {
	case XE_CORE_COMPUTE_UNCORR_ERR_INJ:
		test_inject_core_compute_uncorr_error(mmio, fd);
		break;
	case XE_CORE_COMPUTE_UNCORR_ERR_INJ_POST_RECOVERY_WL:
		run_xe_compute_on_all_engines(fd, POST_RECOVERY_WL);
		break;
	case XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_ONE:
		test_inject_core_compute_corr_error(mmio, fd, INJ_ONCE, THRESHOLD_ONE, true);
		break;
	case XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_DEFAULT:
		test_inject_core_compute_corr_error(mmio, fd, INJ_MANY, THRESHOLD_NONE, true);
		break;
	case XE_CORE_COMPUTE_CORR_ERR_INJ_ONCE:
		test_inject_core_compute_corr_error(mmio, fd, INJ_ONCE, THRESHOLD_NONE, false);
		break;
	default:
		igt_info("Invalid Error Injection specified\n");
	}
}

int igt_main()
{
	int fd;
	struct xe_mmio mmio;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require_f(igt_debugfs_exists(fd, "forcewake_all", O_RDONLY),
			      "forcewake_all is not available in debugfs\n");
		require_cri_gpu(fd);
		/*
		 * TODO: Add firmware query for error injection support.
		 * Should check: EOM (End of Manufacturing) status and error injection jumper state.
		 */
		xe_mmio_access_init(fd, &mmio);
		igt_require_f(xe_mmio_is_initialized(&mmio),
			      "Failed to initialize Xe MMIO access\n");
	}

	igt_describe("Inject core compute uncorrectable error and run post-recovery workload.");
	igt_subtest_with_dynamic("inject_core_compute_uncorr_error") {
		igt_dynamic("inject")
			inject_error(XE_CORE_COMPUTE_UNCORR_ERR_INJ, &mmio, fd);

		igt_dynamic("post-recovery-wl") {
			igt_require_f(uncorr_error_recovered,
				      "Run the \"inject\" dynamic subtest first\n");
			inject_error(XE_CORE_COMPUTE_UNCORR_ERR_INJ_POST_RECOVERY_WL, &mmio, fd);
		}
	}

	igt_describe("Inject one Core Compute Correctable Error with the "
		     "threshold set to 1 and verify exceed-threshold reporting");
	igt_subtest("inject_core_compute_corr_error_threshold_one")
		inject_error(XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_ONE, &mmio, fd);

	igt_describe("Inject enough Core Compute Correctable Errors to cross"
		     " the current threshold and verify exceed-threshold reporting");
	igt_subtest("inject_core_compute_corr_error_threshold_default")
		inject_error(XE_CORE_COMPUTE_CORR_ERR_INJ_THRESHOLD_DEFAULT, &mmio, fd);

	igt_describe("Inject one Core Compute Correctable Error and verify the "
		     "error counter increments by 1");
	igt_subtest("inject_core_compute_corr_error_once")
		inject_error(XE_CORE_COMPUTE_CORR_ERR_INJ_ONCE, &mmio, fd);

	igt_fixture() {
		xe_mmio_access_fini(&mmio);
		drm_close_driver(fd);
	}
}
