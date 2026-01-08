// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: Test Xe PMU(Performance Monitoring Unit) functionality
 * Category: Metrics
 * Functionality: Power/Perf
 * Mega feature: Performance Monitoring Unit
 * Sub-category: Telemetry
 * Test category: Functional tests
 *
 * SUBTEST: gt-c6-idle
 * Description: Basic residency test to validate idle residency
 *		measured over a time interval is within the tolerance
 *
 * SUBTEST: engine-activity-idle
 * Description: Test to validate engine activity shows no load when idle
 *
 * SUBTEST: engine-activity-load-idle
 * Description: Test to validate engine activity with full load and trailing idle
 *
 * SUBTEST: engine-activity-load
 * Description: Test to validate engine activity stats by running a workload
 *
 * SUBTEST: engine-activity-single-load
 * Description: Test to validate engine activity by running workload on one engine and check if
 *		all other engines are idle
 *
 * SUBTEST: engine-activity-single-load-idle
 * Description: Test to validate engine activity by running workload and trailing idle on one engine
 *		and check if all other engines are idle
 *
 * SUBTEST: engine-activity-all-load
 * Description: Test to validate engine activity by running workload on all engines
 *		simultaneously
 *
 * SUBTEST: engine-activity-all-load-idle
 * Description: Test to validate engine activity by running workload on all engines
 *		simultaneously and trailing idle
 *
 * SUBTEST: engine-activity-gt-reset-idle
 * Description: Test to validate engine activity is idle after gt reset
 *
 * SUBTEST: engine-activity-gt-reset
 * Description: Test to validate engine activity on all engines before and after gt reset
 *
 * SUBTEST: engine-activity-suspend
 * Description: Test to validate engine activity on all engines before and after s2idle
 *
 * SUBTEST: engine-activity-multi-client
 * Description: Test to validate engine activity with multiple PMU clients and check that
 *		they do not interfere with each other
 *
 * SUBTEST: engine-activity-after-load-start
 * Description: Validates engine activity when PMU is opened after load started
 *
 * SUBTEST: engine-activity-most-load
 * Description: Test to validate engine activity by running workload on all engines except one
 *
 * SUBTEST: engine-activity-most-load-idle
 * Description: Test to validate engine activity by running workload and trailing idle on all engines
 * 		except one
 *
 * SUBTEST: engine-activity-render-node-idle
 * Description: Test to validate engine activity on render node shows no load when idle
 *
 * SUBTEST: engine-activity-render-node-load
 * Description: Test to validate engine activity on render node by running workload
 *
 * SUBTEST: engine-activity-render-node-load-idle
 * Description: Test to validate engine activity on render node by running workload and trailing idle
 *
 * SUBTEST: engine-activity-accuracy-2
 * Description: Test to validate accuracy of engine activity for 2% by running workload
 *
 * SUBTEST: engine-activity-accuracy-50
 * Description: Test to validate accuracy of engine activity for 50% by running workload
 *
 * SUBTEST: engine-activity-accuracy-90
 * Description: Test to validate accuracy of engine activity for 90% by running workload
 *
 * SUBTEST: all-fn-engine-activity-load
 * Description: Test to validate engine activity by running load on all functions simultaneously
 *
 * SUBTEST: fn-engine-activity-load
 * Description: Test to validate engine activity by running load on a function
 *
 * SUBTEST: fn-engine-activity-sched-if-idle
 * Description: Test to validate engine activity by running load on a function
 *
 * SUBTEST: gt-frequency
 * Description: Validate we can collect accurate frequency PMU stats while running a workload.
 */

#include "igt.h"
#include "igt_perf.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"

#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"
#include "xe/xe_sriov_provisioning.h"

#define SLEEP_DURATION 2 /* in seconds */
/* flag masks */
#define TEST_LOAD		BIT(0)
#define TEST_TRAILING_IDLE	BIT(1)
#define TEST_IDLE		BIT(2)
#define TEST_GT_RESET		BIT(3)

const double tolerance = 0.1;
static char xe_device[NAME_MAX];
static bool autoprobe;
static int total_exec_quantum;
static bool has_engine_active_ticks;

#define test_each_engine(test, fd, hwe) \
	igt_subtest_with_dynamic(test) \
		xe_for_each_engine(fd, hwe) \
			igt_dynamic_f("engine-%s%d", xe_engine_class_string(hwe->engine_class), \
				      hwe->engine_instance)

static bool has_event(const char *device, const char *event)
{
	char buf[512];

	snprintf(buf, sizeof(buf),
		 "/sys/bus/event_source/devices/%s/events/%s",
		 device,
		 event);
	return (!!access(buf, F_OK));
}

static int open_pmu(int xe, uint64_t config)
{
	int fd;

	fd = perf_xe_open(xe, config);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

static int open_group(int xe, uint64_t config, int group)
{
	int fd;

	fd = igt_perf_open_group(xe_perf_type_id(xe), config, group);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));
	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_multi(int fd, unsigned int num, uint64_t *val)
{
	uint64_t buf[2 + num];
	unsigned int i;

	igt_assert_eq(read(fd, buf, sizeof(buf)), sizeof(buf));

	for (i = 0; i < num; i++)
		val[i] = buf[2 + i];

	return buf[1];
}

static unsigned long read_idle_residency(int fd, int gt)
{
	unsigned long residency = 0;
	int gt_fd;

	gt_fd = xe_sysfs_gt_open(fd, gt);
	igt_assert(gt_fd >= 0);
	igt_assert(igt_sysfs_scanf(gt_fd, "gtidle/idle_residency_ms", "%lu", &residency) == 1);
	close(gt_fd);

	return residency;
}

static uint64_t add_format_config(const char *format, uint64_t val)
{
	int ret;
	uint32_t shift;
	uint64_t config;

	ret = perf_event_format(xe_device, format, &shift);
	igt_assert(ret >= 0);
	config = val << shift;

	return config;
}

static uint64_t get_event_config(unsigned int gt, struct drm_xe_engine_class_instance *eci,
				 const char *event)
{
	uint64_t pmu_config = 0;
	int ret;

	igt_skip_on(has_engine_active_ticks);

	ret = perf_event_config(xe_device, event, &pmu_config);
	igt_assert(ret >= 0);
	pmu_config |= add_format_config("gt", gt);

	if (eci) {
		pmu_config |= add_format_config("engine_class", eci->engine_class);
		pmu_config |= add_format_config("engine_instance", eci->engine_instance);
	}

	return pmu_config;
}

static uint64_t get_event_config_fn(unsigned int gt, int function,
				    struct drm_xe_engine_class_instance *eci, const char *event)
{
	return get_event_config(gt, eci, event) | add_format_config("function", function);
}

static void end_cork(int fd, struct xe_cork *cork)
{
	if (cork && !cork->ended)
		xe_cork_sync_end(fd, cork);
}

static void check_all_engines(int num_engines, int *flag, uint64_t *before, uint64_t *after)
{
	uint64_t engine_active_ticks, engine_total_ticks;
	int engine_idx = 0;

	for (int idx = 0; idx < num_engines * 2; idx += 2) {
		engine_idx = idx >> 1;

		engine_active_ticks = after[idx] - before[idx];
		engine_total_ticks = after[idx + 1] - before[idx + 1];

		igt_debug("[%d] Engine active ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", engine_idx,
			  after[idx], before[idx], engine_active_ticks);
		igt_debug("[%d] Engine total ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", engine_idx,
			  after[idx + 1], before[idx + 1], engine_total_ticks);

		if (flag[engine_idx] == TEST_LOAD)
			assert_within_epsilon(engine_active_ticks, engine_total_ticks, tolerance);
		else if (flag[engine_idx] == TEST_IDLE)
			assert_within_epsilon(engine_active_ticks, 0.0f, tolerance);
	}
}

static void engine_activity(int fd, struct drm_xe_engine_class_instance *eci, unsigned int flags)
{
	uint64_t config, engine_active_ticks, engine_total_ticks, before[2], after[2];
	struct xe_cork *cork = NULL;
	uint32_t vm;
	int pmu_fd[2];

	config = get_event_config(eci->gt_id, eci, "engine-active-ticks");
	pmu_fd[0] = open_group(fd, config, -1);

	config = get_event_config(eci->gt_id, eci, "engine-total-ticks");
	pmu_fd[1] = open_group(fd, config, pmu_fd[0]);

	vm = xe_vm_create(fd, 0, 0);

	if (flags & TEST_LOAD) {
		cork = xe_cork_create_opts(fd, eci, vm, 1, 1);
		xe_cork_sync_start(fd, cork);
	}

	pmu_read_multi(pmu_fd[0], 2, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	if (flags & TEST_TRAILING_IDLE)
		end_cork(fd, cork);
	pmu_read_multi(pmu_fd[0], 2, after);

	if (flags & TEST_GT_RESET)
		xe_force_gt_reset_sync(fd, eci->gt_id);
	else
		end_cork(fd, cork);

	engine_active_ticks = after[0] - before[0];
	engine_total_ticks = after[1] - before[1];

	igt_debug("Engine active ticks:  after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", after[0], before[0],
		  engine_active_ticks);
	igt_debug("Engine total ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", after[1], before[1],
		  engine_total_ticks);

	if (flags & TEST_LOAD)
		assert_within_epsilon(engine_active_ticks, engine_total_ticks, tolerance);
	else
		igt_assert(!engine_active_ticks);

	if (flags & TEST_GT_RESET) {
		pmu_read_multi(pmu_fd[0], 2, before);
		usleep(SLEEP_DURATION * USEC_PER_SEC);
		pmu_read_multi(pmu_fd[0], 2, after);

		engine_active_ticks = after[0] - before[0];

		igt_debug("Engine active ticks after gt reset:  after %ld, before %ld delta %ld\n",
			  after[0], before[0], engine_active_ticks);

		igt_assert(!engine_active_ticks);
	}

	if (cork)
		xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);

	close(pmu_fd[0]);
	close(pmu_fd[1]);
}

static void engine_activity_load_single(int fd, int num_engines,
					struct drm_xe_engine_class_instance *eci,
					unsigned int flags)
{
	uint64_t ahnd, before[2 * num_engines], after[2 * num_engines], config;
	struct drm_xe_engine_class_instance *eci_;
	struct xe_cork *cork = NULL;
	int pmu_fd[num_engines * 2], flag[num_engines];
	int idx = 0, engine_idx;
	uint32_t vm;

	pmu_fd[0] = -1;
	xe_for_each_engine(fd, eci_) {
		engine_idx = idx >> 1;
		flag[engine_idx] = TEST_IDLE;

		if (eci_->engine_class == eci->engine_class &&
		    eci_->engine_instance == eci->engine_instance &&
		    eci_->gt_id == eci->gt_id)
			flag[engine_idx] = TEST_LOAD;

		config = get_event_config(eci_->gt_id, eci_, "engine-active-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);

		config = get_event_config(eci_->gt_id, eci_, "engine-total-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);
	}

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	if (flags & TEST_LOAD) {
		cork = xe_cork_create_opts(fd, eci, vm, 1, 1, .ahnd = ahnd);
		xe_cork_sync_start(fd, cork);
	}

	pmu_read_multi(pmu_fd[0], 2 * num_engines, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	if (flags & TEST_TRAILING_IDLE)
		end_cork(fd, cork);
	pmu_read_multi(pmu_fd[0], 2 * num_engines, after);

	end_cork(fd, cork);

	if (cork)
		xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);

	for (idx = 0; idx < num_engines * 2; idx += 2) {
		close(pmu_fd[idx]);
		close(pmu_fd[idx + 1]);
	}

	check_all_engines(num_engines, flag, before, after);
}

static void engine_activity_load_most(int fd, int num_engines, struct drm_xe_engine_class_instance *eci,
				      unsigned int flags)
{
	uint64_t ahnd, config, before[2 * num_engines], after[2 * num_engines];
	struct drm_xe_engine_class_instance *eci_;
	struct xe_cork *cork[num_engines];
	int pmu_fd[num_engines * 2], flag[num_engines];
	int idx = 0, engine_idx = 0;
	uint32_t vm;

	pmu_fd[0] = -1;
	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	xe_for_each_engine(fd, eci_) {
		engine_idx = idx >> 1;
		flag[engine_idx] = TEST_LOAD;

		if (eci_->engine_class == eci->engine_class &&
		    eci_->engine_instance == eci->engine_instance &&
		    eci_->gt_id == eci->gt_id) {
			flag[engine_idx] = TEST_IDLE;
			cork[engine_idx] = NULL;
		} else {
			cork[engine_idx] = xe_cork_create_opts(fd, eci_, vm, 1, 1, .ahnd = ahnd);
			xe_cork_sync_start(fd, cork[engine_idx]);
		}

		config = get_event_config(eci_->gt_id, eci_, "engine-active-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);

		config = get_event_config(eci_->gt_id, eci_, "engine-total-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);
	}

	pmu_read_multi(pmu_fd[0], 2 * num_engines, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	if (flags & TEST_TRAILING_IDLE) {
		for (idx = 0; idx < num_engines; idx++)
			end_cork(fd, cork[engine_idx]);
	}
	pmu_read_multi(pmu_fd[0], 2 * num_engines, after);

	for (idx = 0; idx < num_engines * 2; idx += 2) {
		engine_idx = idx >> 1;
		end_cork(fd, cork[engine_idx]);
		if (cork[engine_idx])
			xe_cork_destroy(fd, cork[engine_idx]);
		close(pmu_fd[idx]);
		close(pmu_fd[idx + 1]);
	}

	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);

	check_all_engines(num_engines, flag, before, after);
}

static void engine_activity_load_all(int fd, int num_engines, unsigned int flags)
{
	uint64_t ahnd, config, before[2 * num_engines], after[2 * num_engines];
	struct drm_xe_engine_class_instance *eci;
	struct xe_cork *cork[num_engines];
	int idx = 0, engine_idx = 0;
	int pmu_fd[2 * num_engines], flag[num_engines];
	uint32_t vm;

	pmu_fd[0] = -1;
	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	xe_for_each_engine(fd, eci) {
		engine_idx = idx >> 1;
		flag[engine_idx] = TEST_LOAD;

		config = get_event_config(eci->gt_id, eci, "engine-active-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);

		config = get_event_config(eci->gt_id, eci, "engine-total-ticks");
		pmu_fd[idx++] = open_group(fd, config, pmu_fd[0]);

		cork[engine_idx] = xe_cork_create_opts(fd, eci, vm, 1, 1, .ahnd = ahnd);
		xe_cork_sync_start(fd, cork[engine_idx]);
	}

	pmu_read_multi(pmu_fd[0], 2 * num_engines, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	if (flags & TEST_TRAILING_IDLE) {
		for (idx = 0; idx < num_engines; idx++)
			end_cork(fd, cork[idx]);
	}
	pmu_read_multi(pmu_fd[0], 2 * num_engines, after);

	for (idx = 0; idx < num_engines * 2; idx += 2) {
		engine_idx = idx >> 1;
		end_cork(fd, cork[engine_idx]);
		xe_cork_destroy(fd, cork[engine_idx]);
		close(pmu_fd[idx]);
		close(pmu_fd[idx + 1]);
	}

	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);

	check_all_engines(num_engines, flag, before, after);
}

#define assert_within(x, ref, tol) \
	igt_assert_f((double)(x) <= ((double)(ref) + (tol)) && \
		     (double)(x) >= ((double)(ref) - (tol)), \
		     "%f not within +%f/-%f of %f! ('%s' vs '%s')\n", \
		     (double)(x), (double)(tol), (double)(tol), \
		     (double)(ref), #x, #ref)

static void accuracy(int fd, struct drm_xe_engine_class_instance *eci,
		     unsigned long target_percentage, unsigned long target_iter)
{
	unsigned long active_us, cycle_us, calibration_us, idle_us, test_us;
	const unsigned long min_test_us = 1e6;
	uint64_t config, before[2], after[2];
	int link[2], pmu_fd[2];
	double engine_activity, expected;

	cycle_us = min_test_us / target_iter;
	active_us = cycle_us * target_percentage / 100;
	idle_us = cycle_us - active_us;
	test_us = cycle_us * target_iter;
	calibration_us = test_us / 2;

	while (idle_us < 2500 || active_us < 2500) {
		active_us *= 2;
		idle_us *= 2;
	}

	igt_info("calibration=%lums, test=%lums, cycle=%lums; ratio=%.2f%% (%luus/%luus)\n",
		 calibration_us / 1000, test_us / 1000, cycle_us / 1000,
		 ((double)active_us / cycle_us) * 100.0, active_us, idle_us);

	igt_assert(pipe(link) == 0);

	igt_fork(child, 1) {
		const unsigned int timeout[] = { calibration_us * 1000, test_us * 1000 };
		uint64_t total_active_ns = 0, total_ns = 0;
		igt_spin_t *spin;
		uint64_t vm, ahnd;

		vm = xe_vm_create(fd, 0, 0);
		intel_allocator_init();
		ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

		for (int pass = 0; pass < ARRAY_SIZE(timeout); pass++) {
			unsigned int target_idle_us = idle_us;
			struct timespec start = { };
			uint64_t pass_active_ns = 0;
			unsigned long pass_ns = 0;
			double avg = 0.0, var = 0.0;
			int n = 0;

			igt_nsec_elapsed(&start);

			while (pass_ns < timeout[pass]) {
				unsigned long loop_ns, loop_active_ns, loop_idle_ns, now;
				double err, prev_avg, cur_val;

				/* idle sleep */
				igt_measured_usleep(target_idle_us);

				/* start spinner */
				spin = igt_spin_new(fd, .ahnd = ahnd, .vm = vm, .hwe = eci);
				loop_idle_ns = igt_nsec_elapsed(&start);
				igt_measured_usleep(active_us);
				igt_spin_free(fd, spin);

				now = igt_nsec_elapsed(&start);
				loop_active_ns = now - loop_idle_ns;
				loop_ns = now - pass_ns;
				pass_ns = now;

				pass_active_ns += loop_active_ns;
				total_active_ns += loop_active_ns;
				total_ns += loop_ns;

				/* Re-calibrate according to err */
				err = (double)total_active_ns / total_ns -
				      (double)target_percentage / 100.0;

				target_idle_us = (double)target_idle_us * (1.0 + err);

				/* Running average and variance for debug. */
				cur_val = 100.0 * total_active_ns / total_ns;
				prev_avg = avg;
				avg += (cur_val - avg) / ++n;
				var += (cur_val - avg) * (cur_val - prev_avg);
			}

			expected = (double)pass_active_ns / pass_ns;

			igt_info("%u: %d cycles, busy %" PRIu64 " us, idle %" PRIu64 " us -> %.2f%% "
				 "(target: %lu%%; average = %.2f ± %.3f%%)\n",
				 pass, n, pass_active_ns / 1000, (pass_ns - pass_active_ns) / 1000,
				 100 * expected, target_percentage, avg, sqrt(var / n));

			igt_assert_eq(write(link[1], &expected, sizeof(expected)),
				      sizeof(expected));
		}

		xe_vm_destroy(fd, vm);
		put_ahnd(ahnd);
	}

	config = get_event_config(eci->gt_id, eci, "engine-active-ticks");
	pmu_fd[0] = open_group(fd, config, -1);

	config = get_event_config(eci->gt_id, eci, "engine-total-ticks");
	pmu_fd[1] = open_group(fd, config, pmu_fd[0]);

	/* wait for calibration cycle to complete */
	igt_assert_eq(read(link[0], &expected, sizeof(expected)),
		      sizeof(expected));

	pmu_read_multi(pmu_fd[0], 2, before);
	igt_assert_eq(read(link[0], &expected, sizeof(expected)),
		      sizeof(expected));
	pmu_read_multi(pmu_fd[0], 2, after);

	close(pmu_fd[0]);
	close(pmu_fd[1]);

	close(link[1]);
	close(link[0]);

	igt_waitchildren();

	engine_activity = (double)(after[0] - before[0]) / (after[1] - before[1]);

	igt_info("error=%.2f%% (%.2f%% vs %.2f%%)\n",
		 (engine_activity - expected) * 100, 100 * engine_activity, 100 * expected);

	assert_within(100.0 * engine_activity, 100.0 * expected, 3);
}

static void engine_activity_all_fn(int fd, struct drm_xe_engine_class_instance *eci, int num_fns)
{
	uint64_t config, engine_active_ticks, engine_total_ticks;
	uint64_t after[2 * num_fns], before[2 * num_fns];
	struct pmu_function {
		struct xe_cork *cork;
		uint32_t vm;
		uint64_t pmu_fd[2];
		int fd;
	} fn[num_fns];
	struct pmu_function *f;
	int i;

	fn[0].pmu_fd[0] = -1;
	for (i = 0; i < num_fns; i++) {
		f = &fn[i];

		config = get_event_config_fn(eci->gt_id, i, eci, "engine-active-ticks");
		f->pmu_fd[0] = open_group(fd, config, fn[0].pmu_fd[0]);

		config = get_event_config_fn(eci->gt_id, i, eci, "engine-total-ticks");
		f->pmu_fd[1] = open_group(fd, config, fn[0].pmu_fd[0]);

		if (i > 0)
			f->fd = igt_sriov_open_vf_drm_device(fd, i);
		else
			f->fd = fd;

		igt_assert_fd(f->fd);

		f->vm = xe_vm_create(f->fd, 0, 0);
		f->cork = xe_cork_create_opts(f->fd, eci, f->vm, 1, 1);
		xe_cork_sync_start(f->fd, f->cork);
	}

	pmu_read_multi(fn[0].pmu_fd[0], 2 * num_fns, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	pmu_read_multi(fn[0].pmu_fd[0], 2 * num_fns, after);

	for (i = 0; i < num_fns; i++) {
		int idx = i * 2;

		f = &fn[i];
		end_cork(f->fd, f->cork);
		engine_active_ticks = after[idx] - before[idx];
		engine_total_ticks = after[idx + 1] - before[idx + 1];

		igt_debug("[%d] Engine active ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", i,
			  after[idx], before[idx], engine_active_ticks);
		igt_debug("[%d] Engine total ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", i,
			  after[idx + 1], before[idx + 1], engine_total_ticks);

		if (f->cork)
			xe_cork_destroy(f->fd, f->cork);

		xe_vm_destroy(f->fd, f->vm);

		close(f->pmu_fd[0]);
		close(f->pmu_fd[1]);

		if (i > 0)
			close(f->fd);

		assert_within_epsilon(engine_active_ticks, engine_total_ticks, tolerance);
	}
}

static void engine_activity_fn(int fd, struct drm_xe_engine_class_instance *eci,
			       int function, bool sched_if_idle)
{
	uint64_t config, engine_active_ticks, engine_total_ticks, before[2], after[2];
	double busy_percent, exec_quantum_ratio;
	struct xe_cork *cork = NULL;
	int pmu_fd[2], fn_fd;
	uint32_t vm;

	if (function > 0) {
		fn_fd = igt_sriov_open_vf_drm_device(fd, function);
		igt_assert_fd(fn_fd);
	} else {
		fn_fd = fd;
	}

	config = get_event_config_fn(eci->gt_id, function, eci, "engine-active-ticks");
	pmu_fd[0] = open_group(fd, config, -1);

	config = get_event_config_fn(eci->gt_id, function, eci, "engine-total-ticks");
	pmu_fd[1] = open_group(fd, config, pmu_fd[0]);

	vm = xe_vm_create(fn_fd, 0, 0);
	cork = xe_cork_create_opts(fn_fd, eci, vm, 1, 1);
	xe_cork_sync_start(fn_fd, cork);

	pmu_read_multi(pmu_fd[0], 2, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	pmu_read_multi(pmu_fd[0], 2, after);

	end_cork(fn_fd, cork);

	engine_active_ticks = after[0] - before[0];
	engine_total_ticks = after[1] - before[1];

	igt_debug("[%d] Engine active ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", function,
		  after[0], before[0], engine_active_ticks);
	igt_debug("[%d] Engine total ticks: after %" PRIu64 ", before %" PRIu64 " delta %" PRIu64 "\n", function,
		  after[1], before[1], engine_total_ticks);

	busy_percent = (double)engine_active_ticks / engine_total_ticks;
	exec_quantum_ratio = (double)total_exec_quantum / xe_sriov_get_exec_quantum_ms(fd, function, eci->gt_id);

	igt_debug("Percent %lf\n", busy_percent * 100);

	if (cork)
		xe_cork_destroy(fn_fd, cork);

	xe_vm_destroy(fn_fd, vm);

	close(pmu_fd[0]);
	close(pmu_fd[1]);

	if (function > 0)
		close(fn_fd);

	if (sched_if_idle)
		assert_within_epsilon(engine_active_ticks, engine_total_ticks, tolerance);
	else
		assert_within_epsilon(busy_percent, exec_quantum_ratio, tolerance);
}

static void engine_activity_load_start(int fd, struct drm_xe_engine_class_instance *eci)
{
	uint64_t ahnd, config, engine_active_ticks, engine_total_ticks, before[2], after[2];
	struct xe_cork *cork = NULL;
	uint32_t vm;
	int pmu_fd[2];

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	cork = xe_cork_create_opts(fd, eci, vm, 1, 1, .ahnd = ahnd);
	xe_cork_sync_start(fd, cork);

	config = get_event_config(eci->gt_id, eci, "engine-active-ticks");
	pmu_fd[0] = open_group(fd, config, -1);

	config = get_event_config(eci->gt_id, eci, "engine-total-ticks");
	pmu_fd[1] = open_group(fd, config, pmu_fd[0]);

	pmu_read_multi(pmu_fd[0], 2, before);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	pmu_read_multi(pmu_fd[0], 2, after);
	end_cork(fd, cork);

	engine_active_ticks = after[0] - before[0];
	engine_total_ticks = after[1] - before[1];

	igt_debug("Engine active ticks:  after %ld, before %ld delta %ld\n", after[0], before[0],
		  engine_active_ticks);
	igt_debug("Engine total ticks: after %ld, before %ld delta %ld\n", after[1], before[1],
		  engine_total_ticks);

	xe_cork_destroy(fd, cork);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
	close(pmu_fd[0]);
	close(pmu_fd[1]);

	assert_within_epsilon(engine_active_ticks, engine_total_ticks, tolerance);
}

static void engine_activity_multi_client(int fd, struct drm_xe_engine_class_instance *eci)
{
#define NUM_CLIENTS 2
	struct pmu_client {
		uint64_t before[2];
		uint64_t after[2];
		int pmu_fd[2];
	} client[NUM_CLIENTS];
	uint64_t ahnd, config, engine_active_ticks, engine_total_ticks;
	struct xe_cork *cork = NULL;
	uint32_t vm;
	int i = 0;

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	for (i = 0; i < NUM_CLIENTS; i++) {
		config = get_event_config(eci->gt_id, eci, "engine-active-ticks");
		client[i].pmu_fd[0] = open_group(fd, config, -1);
		config = get_event_config(eci->gt_id, eci, "engine-total-ticks");
		client[i].pmu_fd[1] = open_group(fd, config, client[i].pmu_fd[0]);
	}

	cork = xe_cork_create_opts(fd, eci, vm, 1, 1, .ahnd = ahnd);
	xe_cork_sync_start(fd, cork);

	for (i = 0; i < NUM_CLIENTS; i++)
		pmu_read_multi(client[i].pmu_fd[0], 2, client[i].before);

	usleep(SLEEP_DURATION * USEC_PER_SEC);

	for (i = 0; i < NUM_CLIENTS; i++)
		pmu_read_multi(client[i].pmu_fd[0], 2, client[i].after);

	end_cork(fd, cork);
	xe_cork_destroy(fd, cork);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);

	for (i = 0; i < NUM_CLIENTS; i++) {
		engine_active_ticks = client[i].after[0] - client[i].before[0];
		engine_total_ticks = client[i].after[1] - client[i].before[1];

		igt_debug("Client %d: Engine active ticks:  after %ld, before %ld delta %ld\n",
			  i + 1, client[i].after[0], client[i].before[0], engine_active_ticks);

		igt_debug("Client %d Engine total ticks: after %ld, before %ld delta %ld\n",
			  i + 1, client[i].after[1], client[i].before[1], engine_total_ticks);

		close(client[i].pmu_fd[0]);
		close(client[i].pmu_fd[1]);

		assert_within_epsilon(engine_active_ticks, engine_active_ticks, tolerance);
	}
}

static void test_gt_c6_idle(int xe, unsigned int gt)
{
	int pmu_fd;
	uint64_t pmu_config;
	uint64_t ts[2];
	unsigned long slept, start, end;
	uint64_t val;

	/* Get the PMU config for the gt-c6 event */
	pmu_config = get_event_config(gt, NULL, "gt-c6-residency");

	pmu_fd = open_pmu(xe, pmu_config);

	igt_require_f(igt_wait(xe_gt_is_in_c6(xe, gt), 1000, 10), "GT %d should be in C6\n", gt);

	/* While idle check full RC6. */
	start = read_idle_residency(xe, gt);
	val = __pmu_read_single(pmu_fd, &ts[0]);
	slept = igt_measured_usleep(SLEEP_DURATION * USEC_PER_SEC) / 1000;
	end = read_idle_residency(xe, gt);
	val = __pmu_read_single(pmu_fd, &ts[1]) - val;

	igt_debug("gt%u: slept=%lu, perf=%"PRIu64"\n",
		  gt, slept,  val);

	igt_debug("Start res: %lu, end_res: %lu", start, end);

	assert_within_epsilon(val,
			      (ts[1] - ts[0])/USEC_PER_SEC,
			      tolerance);
	close(pmu_fd);
}

static void test_gt_frequency(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct xe_cork *cork = NULL;
	uint64_t end[2], start[2];
	unsigned long config_rq_freq, config_act_freq;
	double min[2], max[2];
	uint32_t gt = eci->gt_id;
	uint32_t orig_min = xe_gt_get_freq(fd, eci->gt_id, "min");
	uint32_t orig_max = xe_gt_get_freq(fd, eci->gt_id, "max");
	uint32_t current_min;
	uint32_t orig_rpe;
	uint32_t vm;
	int pmu_fd[2];

	config_rq_freq = get_event_config(gt, NULL, "gt-requested-frequency");
	pmu_fd[0] = open_group(fd, config_rq_freq, -1);

	config_act_freq = get_event_config(gt, NULL, "gt-actual-frequency");
	pmu_fd[1] = open_group(fd, config_act_freq, pmu_fd[0]);

	vm = xe_vm_create(fd, 0, 0);

	cork = xe_cork_create_opts(fd, eci, vm, 1, 1);
	xe_cork_sync_start(fd, cork);

	/*
	 * Set GPU to min frequency and read PMU counters.
	 */
	igt_assert(xe_gt_set_freq(fd, gt, "min", orig_min) > 0);
	igt_assert(xe_gt_set_freq(fd, gt, "max", orig_min) > 0);
	igt_assert(xe_gt_get_freq(fd, gt, "max") == orig_min);

	pmu_read_multi(pmu_fd[0], 2, start);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	pmu_read_multi(pmu_fd[0], 2, end);

	min[0] = (end[0] - start[0]);
	min[1] = (end[1] - start[1]);

	/*
	 * Set GPU to max frequency and read PMU counters.
	 */
	igt_assert(xe_gt_set_freq(fd, gt, "max", orig_max) > 0);
	igt_assert(xe_gt_get_freq(fd, gt, "max") == orig_max);
	igt_assert(xe_gt_set_freq(fd, gt, "min", orig_max) > 0);
	igt_assert(xe_gt_get_freq(fd, gt, "min") == orig_max);

	pmu_read_multi(pmu_fd[0], 2, start);
	usleep(SLEEP_DURATION * USEC_PER_SEC);
	pmu_read_multi(pmu_fd[0], 2, end);

	max[0] = (end[0] - start[0]);
	max[1] = (end[1] - start[1]);

	end_cork(fd, cork);

	/*
	 * Restore min/max.
	 */
	igt_assert(xe_gt_set_freq(fd, gt, "min", orig_min) > 0);
	orig_rpe = xe_gt_get_freq(fd, gt, "rpe");
	current_min = xe_gt_get_freq(fd, gt, "min");
	igt_assert(current_min == orig_min || current_min == orig_rpe);

	igt_info("Minimum frequency: requested %.1f, actual %.1f\n",
		 min[0], min[1]);
	igt_info("Maximum frequency: requested %.1f, actual %.1f\n",
		 max[0], max[1]);

	close(pmu_fd[0]);
	close(pmu_fd[1]);

	if (cork)
		xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);

	close(pmu_fd[0]);
	close(pmu_fd[1]);

	assert_within_epsilon(min[0], orig_min, tolerance);
	/*
	 * On thermally throttled devices we cannot be sure maximum frequency
	 * can be reached so use larger tolerance downwards.
	 */
	assert_within_epsilon_up_down(max[0], orig_max, tolerance, 0.15f);
}

static unsigned int enable_and_provision_vfs(int fd)
{
	unsigned int gt, num_vfs;
	int pf_exec_quantum = 64, vf_exec_quantum = 32, vf;

	igt_require(igt_sriov_is_pf(fd));
	igt_require(igt_sriov_get_enabled_vfs(fd) == 0);
	xe_sriov_require_default_scheduling_attributes(fd);
	autoprobe = igt_sriov_is_driver_autoprobe_enabled(fd);

	/* Enable VF's */
	igt_sriov_disable_driver_autoprobe(fd);
	igt_sriov_enable_vfs(fd, 2);
	num_vfs = igt_sriov_get_enabled_vfs(fd);
	igt_require(num_vfs == 2);

	/* Set 32ms for VF execution quantum and 64ms for PF execution quantum */
	xe_for_each_gt(fd, gt) {
		xe_sriov_set_sched_if_idle(fd, gt, 0);
		for (int fn = 0; fn <= num_vfs; fn++)
			xe_sriov_set_exec_quantum_ms(fd, fn, gt, fn ? vf_exec_quantum :
						     pf_exec_quantum);
	}

	/* probe VFs */
	igt_sriov_enable_driver_autoprobe(fd);
	for (vf = 1; vf <= num_vfs; vf++)
		igt_sriov_bind_vf_drm_driver(fd, vf);

	total_exec_quantum = pf_exec_quantum + (num_vfs * vf_exec_quantum);

	return num_vfs;
}

static void unprovision_and_disable_vfs(int fd)
{
	unsigned int gt, num_vfs = igt_sriov_get_enabled_vfs(fd);

	xe_for_each_gt(fd, gt) {
		xe_sriov_set_sched_if_idle(fd, gt, 0);
		for (int fn = 0; fn <= num_vfs; fn++)
			xe_sriov_set_exec_quantum_ms(fd, fn, gt, 0);
	}

	xe_sriov_disable_vfs_restore_auto_provisioning(fd);
	/* abort to avoid execution of next tests with enabled VFs */
	igt_abort_on_f(igt_sriov_get_enabled_vfs(fd) > 0,
		       "Failed to disable VF(s)");
	autoprobe ? igt_sriov_enable_driver_autoprobe(fd) :
		    igt_sriov_disable_driver_autoprobe(fd);

	igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(fd),
		       "Failed to restore sriov_drivers_autoprobe value\n");
}

static void stash_gt_freq(int fd, uint32_t **stash_min, uint32_t **stash_max)
{
	int num_gts, gt;

	num_gts = xe_number_gt(fd);

	*stash_min = (uint32_t *) malloc(sizeof(uint32_t) * num_gts);
	*stash_max = (uint32_t *) malloc(sizeof(uint32_t) * num_gts);

	igt_skip_on(*stash_min == NULL || *stash_max == NULL);

	xe_for_each_gt(fd, gt) {
		(*stash_min)[gt] = xe_gt_get_freq(fd, gt, "min");
		(*stash_max)[gt] = xe_gt_get_freq(fd, gt, "max");
	}
}

static void restore_gt_freq(int fd, uint32_t *stash_min, uint32_t *stash_max)
{
	int gt;

	xe_for_each_gt(fd, gt) {
		xe_gt_set_freq(fd, gt, "max", stash_max[gt]);
		xe_gt_set_freq(fd, gt, "min", stash_min[gt]);
	}
	free(stash_min);
	free(stash_max);
}

int igt_main()
{
	int fd, gt, num_engines;
	struct drm_xe_engine_class_instance *eci;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		xe_perf_device(fd, xe_device, sizeof(xe_device));
		num_engines = xe_number_engines(fd);
		has_engine_active_ticks = has_event(xe_device, "engine-active-ticks");
	}

	igt_describe("Validate PMU gt-c6 residency counters when idle");
	igt_subtest("gt-c6-idle") {
		igt_require(!IS_PONTEVECCHIO(xe_dev_id(fd)));
		xe_for_each_gt(fd, gt)
			test_gt_c6_idle(fd, gt);
	}

	igt_describe("Validate there is no engine activity when idle");
	test_each_engine("engine-activity-idle", fd, eci)
		engine_activity(fd, eci, 0);

	igt_describe("Validate engine activity with load and trailing idle");
	test_each_engine("engine-activity-load-idle", fd, eci)
		engine_activity(fd, eci, TEST_LOAD | TEST_TRAILING_IDLE);

	igt_describe("Validate engine activity with workload");
	test_each_engine("engine-activity-load", fd, eci)
		engine_activity(fd, eci, TEST_LOAD);

	igt_describe("Validate engine activity of all engines when one engine is loaded");
	test_each_engine("engine-activity-single-load", fd, eci)
		engine_activity_load_single(fd, num_engines, eci, TEST_LOAD);

	igt_describe("Validate engine activity of all engines with one engine loaded and trailing idle");
	test_each_engine("engine-activity-single-load-idle", fd, eci)
		engine_activity_load_single(fd, num_engines, eci, TEST_LOAD | TEST_TRAILING_IDLE);

	igt_describe("Validate engine activity when all except one engine is loaded");
	test_each_engine("engine-activity-most-load", fd, eci)
		engine_activity_load_most(fd, num_engines, eci, TEST_LOAD);

	igt_describe("Validate engine activity when all except one engine is loaded and trailing idle");
	test_each_engine("engine-activity-most-load-idle", fd, eci)
		engine_activity_load_most(fd, num_engines, eci, TEST_LOAD | TEST_TRAILING_IDLE);

	igt_describe("Validate engine activity by loading all engines simultaenously");
	igt_subtest("engine-activity-all-load")
		engine_activity_load_all(fd, num_engines, TEST_LOAD);

	igt_describe("Validate engine activity by loading all engines simultaenously and trailing idle");
	igt_subtest("engine-activity-all-load-idle")
		engine_activity_load_all(fd, num_engines, TEST_LOAD | TEST_TRAILING_IDLE);

	igt_describe("Validate engine activity is idle after gt reset");
	test_each_engine("engine-activity-gt-reset-idle", fd, eci)
		engine_activity(fd, eci, TEST_LOAD | TEST_GT_RESET);

	igt_describe("Validate engine activity before and after gt reset");
	igt_subtest("engine-activity-gt-reset") {
		engine_activity_load_all(fd, num_engines, TEST_LOAD);
		xe_for_each_gt(fd, gt)
			xe_force_gt_reset_sync(fd, gt);
		engine_activity_load_all(fd, num_engines, TEST_LOAD);
	}

	igt_describe("Validate engine activity before and after s2idle");
	igt_subtest("engine-activity-suspend") {
		engine_activity_load_all(fd, num_engines, TEST_LOAD);
		igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE, SUSPEND_TEST_NONE);
		engine_activity_load_all(fd, num_engines, TEST_LOAD);
	}

	igt_subtest_group() {
		const unsigned int percent[] = { 2, 50, 90 };

		for (unsigned int i = 0; i < ARRAY_SIZE(percent); i++) {
			char test_name[NAME_MAX];

			igt_describe_f("Validate accuracy of engine activity for %u%%"
				       " workload", percent[i]);
			snprintf(test_name, NAME_MAX, "engine-activity-accuracy-%u", percent[i]);
			test_each_engine(test_name, fd, eci)
				accuracy(fd, eci, percent[i], 10);
		}
	}

	igt_describe("Validate engine activity when PMU is opened after load");
	test_each_engine("engine-activity-after-load-start", fd, eci)
		engine_activity_load_start(fd, eci);

	igt_describe("Validate multiple PMU clients do not interfere with each other");
	test_each_engine("engine-activity-multi-client", fd, eci)
		engine_activity_multi_client(fd, eci);

	igt_subtest_group() {
		int render_fd;

		igt_fixture() {
			render_fd = __drm_open_driver_render(DRIVER_XE);
			igt_require(render_fd);
		}

		igt_describe("Validate engine activity on render node when idle");
		test_each_engine("engine-activity-render-node-idle", render_fd, eci)
			engine_activity(render_fd, eci, 0);

		igt_describe("Validate engine activity on render node when loaded");
		test_each_engine("engine-activity-render-node-load", render_fd, eci)
			engine_activity(render_fd, eci, TEST_LOAD);

		igt_describe("Validate engine activity on render node with load and trailing idle");
		test_each_engine("engine-activity-render-node-load-idle", render_fd, eci)
			engine_activity(render_fd, eci, TEST_LOAD | TEST_TRAILING_IDLE);

		igt_fixture()
			drm_close_driver(render_fd);
	}

	igt_subtest_group() {
		unsigned int num_fns;

		igt_fixture()
			num_fns = enable_and_provision_vfs(fd) + 1;

		igt_describe("Validate engine activity on all functions");
		test_each_engine("all-fn-engine-activity-load", fd, eci)
			engine_activity_all_fn(fd, eci, num_fns);

		igt_describe("Validate per-function engine activity");
		test_each_engine("fn-engine-activity-load", fd, eci)
			for (int fn = 0; fn < num_fns; fn++)
				engine_activity_fn(fd, eci, fn, false);

		igt_describe("Validate per-function engine activity when sched-if-idle is set");
		test_each_engine("fn-engine-activity-sched-if-idle", fd, eci) {
			xe_sriov_set_sched_if_idle(fd, eci->gt_id, 1);
			for (int fn = 0; fn < num_fns; fn++)
				engine_activity_fn(fd, eci, fn, true);
		}

		igt_fixture()
			unprovision_and_disable_vfs(fd);
	}

	igt_subtest_group() {
		bool has_freq0_node, needs_freq_restore = false;
		uint32_t *stash_min, *stash_max;

		igt_fixture() {
			has_freq0_node = xe_sysfs_gt_has_node(fd, 0, "freq0");
		}

		igt_describe("Validate PMU GT freq measured is within the tolerance");
		igt_subtest_with_dynamic("gt-frequency") {
			igt_skip_on(!has_freq0_node);
			stash_gt_freq(fd, &stash_min, &stash_max);
			needs_freq_restore = true;
			xe_for_each_gt(fd, gt) {
				igt_dynamic_f("gt%u", gt)
				xe_for_each_engine(fd, eci) {
					if (gt == eci->gt_id) {
						test_gt_frequency(fd, eci);
						break;
					}
				}
			}
		}

		igt_fixture() {
			if (needs_freq_restore)
				restore_gt_freq(fd, stash_min, stash_max);
		}
	}

	igt_fixture() {
		close(fd);
	}
}
