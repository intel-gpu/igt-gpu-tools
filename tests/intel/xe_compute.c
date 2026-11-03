// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * TEST: Check compute-related functionality
 * Category: Core
 * Mega feature: Compute
 * Sub-category: Compute tests
 * Test category: functionality test
 */

#include <string.h>

#include "igt.h"
#include "igt_sysfs.h"
#include "intel_compute.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#include "igt_perf.h"
#include "igt_sriov_device.h"

#define LOOP_DURATION_2s	(1000000ull * 2)
#define DURATION_MARGIN		0.2
#define MIN_BUSYNESS		95.0

bool sriov_enabled;

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int class;
	int fd;
	int gt;
	struct user_execenv *execenv;
	struct drm_xe_engine_class_instance *eci;
	bool *go;
};

static int gt_sysfs_open(int gt)
{
	int fd, gt_fd;

	fd = drm_open_driver(DRIVER_XE);
	gt_fd = xe_sysfs_gt_open(fd, gt);
	drm_close_driver(fd);

	return gt_fd;
}

static bool get_num_cslices(u32 gt, u32 *num_slices)
{
	int gt_fd, ret;

	gt_fd = gt_sysfs_open(gt);
	ret = igt_sysfs_scanf(gt_fd, "num_cslices", "%u", num_slices);
	close(gt_fd);

	return ret > 0;
}

/* Grab GT mask in places where we don't have or want to maintain an open fd */
static uint64_t get_gt_mask(void)
{
	int fd = drm_open_driver(DRIVER_XE);
	uint64_t mask;

	mask = xe_device_get(fd)->gt_mask;
	drm_close_driver(fd);

	return mask;
}

#define for_each_bit(__mask, __bit) \
	for ( ; __bit = ffsll(__mask) - 1, __mask != 0; __mask &= ~(1ull << __bit))

/**
 * SUBTEST: ccs-mode-basic
 * GPU requirement: PVC
 * Description: Validate 'ccs_mode' sysfs uapi
 * Functionality: ccs mode
 */
static void
test_ccs_mode(void)
{
	struct drm_xe_engine_class_instance *hwe;
	u32 gt, m, ccs_mode, vm, q, num_slices;
	int fd, gt_fd, num_gt_with_ccs_mode = 0;
	uint64_t gt_mask = get_gt_mask();

	/*
	 * The loop body needs to run without any open file descriptors so we
	 * can't use xe_for_each_gt() which uses an open fd.
	 */
	for_each_bit(gt_mask, gt) {
		if (!get_num_cslices(gt, &num_slices))
			continue;

		num_gt_with_ccs_mode++;
		gt_fd = gt_sysfs_open(gt);
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", 0) < 0);
		for (m = 1; m <= num_slices; m++) {
			/* compute slices are to be equally distributed among enabled engines */
			if (num_slices % m) {
				igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", m) < 0);
				continue;
			}

			/* Validate allowed ccs modes by setting them and reading back */
			igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", m) > 0);
			igt_assert(igt_sysfs_scanf(gt_fd, "ccs_mode", "%u", &ccs_mode) > 0);
			igt_assert(m == ccs_mode);

			/* Validate exec queues creation with enabled ccs engines */
			fd = drm_open_driver(DRIVER_XE);
			vm = xe_vm_create(fd, 0, 0);
			xe_for_each_engine(fd, hwe) {
				if (hwe->gt_id != gt ||
				    hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
					continue;

				q = xe_exec_queue_create(fd, vm, hwe, 0);
				xe_exec_queue_destroy(fd, q);
			}

			/* Ensure exec queue creation fails for disabled ccs engines */
			hwe->gt_id = gt;
			hwe->engine_class = DRM_XE_ENGINE_CLASS_COMPUTE;
			hwe->engine_instance = m;
			igt_assert_neq(__xe_exec_queue_create(fd, vm, 1, 1, hwe, 0, &q), 0);

			xe_vm_destroy(fd, vm);
			drm_close_driver(fd);
		}

		/* Ensure invalid ccs mode setting is rejected */
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", m) < 0);

		/* Can't change ccs mode with an open drm clients */
		fd = drm_open_driver(DRIVER_XE);
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", 1) < 0);
		drm_close_driver(fd);

		/* Set ccs mode back to default value */
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", 1) > 0);

		close(gt_fd);
	}

	igt_require(num_gt_with_ccs_mode > 0);
}

/**
 * SUBTEST: ccs-mode-compute-kernel
 * GPU requirement: PVC
 * Description: Validate 'ccs_mode' by running compute kernel
 * Functionality: ccs mode
 */
static void
test_compute_kernel_with_ccs_mode(void)
{
	struct drm_xe_engine_class_instance *hwe;
	u32 gt, m, num_slices;
	int fd, gt_fd, num_gt_with_ccs_mode = 0;
	uint64_t gt_mask = get_gt_mask();

	/*
	 * The loop body needs to run without any open file descriptors so we
	 * can't use xe_for_each_gt() which uses an open fd.
	 */
	for_each_bit(gt_mask, gt) {
		if (!get_num_cslices(gt, &num_slices))
			continue;

		num_gt_with_ccs_mode++;
		gt_fd = gt_sysfs_open(gt);
		for (m = 1; m <= num_slices; m++) {
			if (num_slices % m)
				continue;

			igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", m) > 0);

			/* Run compute kernel on enabled ccs engines */
			fd = drm_open_driver(DRIVER_XE);
			xe_for_each_engine(fd, hwe) {
				if (hwe->gt_id != gt ||
				    hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
					continue;

				igt_info("GT-%d: Running compute kernel with ccs_mode %d on ccs engine %d\n",
					 gt, m, hwe->engine_instance);
				igt_assert_f(xe_run_intel_compute_kernel_on_engine(fd, hwe, NULL, EXECENV_PREF_SYSTEM),
					     "Unable to run compute kernel successfully\n");
			}
			drm_close_driver(fd);
		}

		/* Set ccs mode back to default value */
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", 1) > 0);

		close(gt_fd);
	}

	igt_require(num_gt_with_ccs_mode > 0);
}

static double elapsed(const struct timeval *start,
		      const struct timeval *end)
{
	return (end->tv_sec - start->tv_sec) + 1e-6*(end->tv_usec - start->tv_usec);
}

/**
 * SUBTEST: loop-duration-2s
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL loop Kernel that for duration,
 *	set in loop_kernel_duration ..
 */
static void
test_compute_kernel_loop(uint64_t loop_duration)
{
	int fd;
	unsigned int ip_ver;
	const struct intel_compute_kernels *kernels;
	struct user_execenv execenv = { 0 };
	struct drm_xe_engine_class_instance *hwe;
	struct timeval start, end;
	double elapse_time, lower_bound, upper_bound;

	fd = drm_open_driver(DRIVER_XE);
	ip_ver = intel_graphics_ver(intel_get_drm_devid(fd));
	kernels = intel_compute_square_kernels;

	while (kernels->kernel) {
		if (ip_ver == kernels->ip_ver)
			break;
		kernels++;
	}

	/* loop_kernel_duration used as sleep to make EU busy for loop_duration */
	execenv.loop_kernel_duration = loop_duration;
	execenv.kernel = kernels->loop_kernel;
	execenv.kernel_size = kernels->loop_kernel_size;

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
			continue;

		igt_info("Running loop_kernel on ccs engine %d\n", hwe->engine_instance);
		gettimeofday(&start, NULL);
		igt_assert_f(xe_run_intel_compute_kernel_on_engine(fd, hwe, &execenv,
								   EXECENV_PREF_SYSTEM),
			     "Unable to run compute kernel successfully\n");
		gettimeofday(&end, NULL);
		elapse_time = elapsed(&start, &end);
		lower_bound = loop_duration / 1e6 - DURATION_MARGIN;
		upper_bound = loop_duration / 1e6 + DURATION_MARGIN;

		igt_assert(lower_bound < elapse_time && elapse_time < upper_bound);
	}
	drm_close_driver(fd);
}

static void
*intel_compute_thread(void *data)
{
	struct thread_data *t = (struct thread_data *)data;
	char device[30];
	uint64_t type;
	uint32_t engine_class, engine_instance, gt_shift;
	uint64_t engine_active_config, engine_total_config;
	int ret, fd1, fd2;
	uint64_t val[4];
	uint64_t param_config;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	type = igt_perf_type_id(xe_perf_device(t->fd, device, sizeof(device)));
	igt_debug("type: %lx, device: %s\n", type, device);

	perf_event_format(device, "gt", &gt_shift);
	perf_event_format(device, "engine_class", &engine_class);
	perf_event_format(device, "engine_instance", &engine_instance);

	ret = perf_event_config(device, "engine-active-ticks", &engine_active_config);
	igt_assert_eq(ret, 0);
	ret = perf_event_config(device, "engine-total-ticks", &engine_total_config);
	igt_assert_eq(ret, 0);

	igt_debug("gt_id: %x, class: %x, instance: %x\n",
		  t->eci->gt_id, t->eci->engine_class, t->eci->engine_instance);

	/* Setting collective counters for compute engine available in t->eci */
	param_config = (uint64_t)t->eci->gt_id << gt_shift |
		       t->eci->engine_class << engine_class |
		       t->eci->engine_instance << engine_instance;

	fd1 = igt_perf_open_group(type, engine_active_config | param_config, -1);
	fd2 = igt_perf_open_group(type, engine_total_config | param_config, fd1);

	ret = read(fd1, val, sizeof(val));
	igt_assert_eq(ret, sizeof(val));
	igt_info("start - active: %ld, total: %ld, busyness: %.1f before scheduling on engine_instance :%d\n",
		 val[2], val[3], (float)val[2] / (float)val[3] * 100.0, t->eci->engine_instance);

	igt_assert_f(xe_run_intel_compute_kernel_on_engine(t->fd, t->eci, t->execenv,
							   EXECENV_PREF_VRAM_IF_POSSIBLE),
		     "Unable to run compute kernel successfully\n");

	ret = read(fd1, val, sizeof(val));
	igt_assert_eq(ret, sizeof(val));
	igt_info("end - active: %ld, total: %ld, busyness: %.1f after scheduling on engine_instance :%d\n",
		 val[2], val[3], (float)val[2] / (float)val[3] * 100.0, t->eci->engine_instance);

	igt_assert_f(((float)val[2] / (float)val[3] * 100.0) > MIN_BUSYNESS,
		     "Engines are under utilizated\n");

	close(fd1);
	close(fd2);
	return NULL;
}

static void
igt_check_supported_pipeline(void)
{
	int fd;
	unsigned int ip_ver;
	const struct intel_compute_kernels *kernels;

	fd = drm_open_driver(DRIVER_XE);
	ip_ver = intel_graphics_ver(intel_get_drm_devid(fd));
	kernels = intel_compute_square_kernels;
	drm_close_driver(fd);

	while (kernels->kernel) {
		if (ip_ver == kernels->ip_ver)
			break;
		kernels++;
	}

	/* skip if loop_kernel is not supported by pipeline */
	if (!kernels->kernel || !kernels->loop_kernel)
		igt_skip("loop_kernel not supported by pipeline\n");
}

static bool is_sriov_mode(int fd)
{
	bool is_sriov = false;

	if (igt_sriov_is_pf(fd) && igt_sriov_vfs_supported(fd))
		is_sriov = true;

	return is_sriov;
}

static void
igt_store_ccs_mode(int ccs_mode[], int size)
{
	uint64_t gt_mask;
	uint32_t gt, num_slices;
	int gt_fd;

	gt_mask = get_gt_mask();
	for_each_bit(gt_mask, gt) {
		if (!get_num_cslices(gt, &num_slices))
			continue;
		igt_assert(gt < size);

		gt_fd = gt_sysfs_open(gt);
		igt_sysfs_scanf(gt_fd, "ccs_mode", "%u", &ccs_mode[gt]);
		close(gt_fd);
	}
}

static void
igt_restore_ccs_mode(int ccs_mode[], int size)
{
	uint64_t gt_mask = get_gt_mask();
	uint32_t gt, num_slices;
	int gt_fd;

	for_each_bit(gt_mask, gt) {
		if (!get_num_cslices(gt, &num_slices))
			continue;
		igt_assert(gt < size);

		gt_fd = gt_sysfs_open(gt);
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", ccs_mode[gt]) > 0);
		close(gt_fd);
	}
}

/**
 * SUBTEST: eu-busy-10s
 * Functionality: OpenCL kernel
 * Description: Run loop_kernel for duration_sec and observe EU business
 */
static void
test_eu_busy(uint64_t duration_sec)
{
	struct user_execenv execenv = { 0 };
	struct thread_data *threads_data;
	struct drm_xe_engine_class_instance *hwe;
	const struct intel_compute_kernels *kernels;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	u32 gt, n_instances = 0, i;
	bool go = false;
	int ccs_mode, gt_fd, fd;
	u32 num_slices, ip_ver;
	uint64_t gt_mask = get_gt_mask();

	for_each_bit(gt_mask, gt) {
		if (!get_num_cslices(gt, &num_slices))
			continue;

		gt_fd = gt_sysfs_open(gt);
		igt_assert(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", num_slices) > 0);
		igt_assert(igt_sysfs_scanf(gt_fd, "ccs_mode", "%u", &ccs_mode) > 0);
		close(gt_fd);
	}

	igt_skip_on_f(ccs_mode <= 1, "Skipping test as ccs_mode <=1 not matching criteria :%d\n",
		      ccs_mode);

	fd = drm_open_driver(DRIVER_XE);

	ip_ver = intel_graphics_ver(intel_get_drm_devid(fd));
	kernels = intel_compute_square_kernels;
	while (kernels->kernel) {
		if (ip_ver == kernels->ip_ver)
			break;
		kernels++;
	}
	if (!kernels->loop_kernel_size)
		drm_close_driver(fd);
	igt_assert(kernels->loop_kernel_size);

	/*
	 * User should use different kernel if loop_kernel_duration not set
	 * With loop kernel and loop duration it assumes we stop it via memory write
	 *
	 */
	execenv.loop_kernel_duration = duration_sec;
	execenv.kernel = kernels->loop_kernel;
	execenv.kernel_size = kernels->loop_kernel_size;

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE)
			++n_instances;
	}

	threads_data = calloc(n_instances, sizeof(*threads_data));
	if (!threads_data)
		drm_close_driver(fd); /* drop reference for retrieve ccs_mode */
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	i = 0;
	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
			continue;

		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
		threads_data[i].fd = fd;
		threads_data[i].eci = hwe;
		threads_data[i].go = &go;
		threads_data[i].execenv = &execenv;
		pthread_create(&threads_data[i].thread, 0, intel_compute_thread,
			       &threads_data[i]);
		++i;
	}
	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (int n = 0; n < i; ++n)
		pthread_join(threads_data[n].thread, NULL);

	free(threads_data);
	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&cond);

	drm_close_driver(fd);
}

/**
 * SUBTEST: compute-square
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * GPU requirement: TGL, PVC, LNL, PTL
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	for an input dataset..
 */
static void
test_compute_square(int fd)
{
	igt_require_f(run_intel_compute_kernel(fd, NULL, EXECENV_PREF_SYSTEM),
		      "GPU not supported\n");
}

/**
 * SUBTEST: compute-square-userenv
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	taking buffers from userenv.
 *
 * SUBTEST: compute-square-userenv-large-buffer
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	taking large buffers from userenv.
 *
 * SUBTEST: compute-square-userenv-isvm
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	taking buffers from userenv where input is svm buffer.
 *
 * SUBTEST: compute-square-userenv-osvm
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	taking buffers from userenv where output buffer is svm buffer.
 *
 * SUBTEST: compute-square-userenv-iosvm
 * Mega feature: Compute
 * Sub-category: compute tests
 * Functionality: OpenCL kernel
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	taking buffers from userenv where input and output buffers are svm buffer.
 */

#define INPUT_IN_SVM		(1 << 0)
#define OUTPUT_IN_SVM		(1 << 1)
#define LARGE_BUFFER		(1 << 2)
#define LARGE_BUFFER_SIZE	(SZ_2M + SZ_1M)
#define INPUT_BO_ADDR		0x30000000
#define OUTPUT_BO_ADDR		0x31000000
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
#define FIVE_SEC		(5LL * NSEC_PER_SEC)

#define bind_system_allocator(__sync, __num_sync)			\
	__xe_vm_bind_assert(fd, vm, 0,					\
			    0, 0, 0, 0x1ull << va_bits,			\
			    DRM_XE_VM_BIND_OP_MAP,			\
			    DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,	\
			    (__sync), (__num_sync), 0, 0)

static void
test_compute_square_userenv(int fd, uint32_t flags)
{
	struct user_execenv env = {};
	uint32_t input_bo, output_bo, vm, size = SZ_4K;
	uint32_t vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	int va_bits = xe_va_bits(fd);
	float *input, *output;
	int i;

	if (flags & LARGE_BUFFER)
		size = LARGE_BUFFER_SIZE;
	size = ALIGN(size, xe_get_default_alignment(fd));
	env.array_size = size / sizeof(float);

	if ((flags & INPUT_IN_SVM) || (flags & OUTPUT_IN_SVM))
		vm_flags |= DRM_XE_VM_CREATE_FLAG_FAULT_MODE;
	vm = env.vm = xe_vm_create(fd, vm_flags, 0);

	if ((flags & INPUT_IN_SVM) || (flags & OUTPUT_IN_SVM)) {
		struct drm_xe_sync sync = {
			.type = DRM_XE_SYNC_TYPE_USER_FENCE,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL,
			.timeline_value = USER_FENCE_VALUE,
		};
		struct bo_sync {
			uint64_t sync;
		} *bo_sync;

		bo_sync = aligned_alloc(xe_get_default_alignment(fd), sizeof(*bo_sync));
		igt_assert(bo_sync);
		sync.addr = to_user_pointer(&bo_sync->sync);
		bind_system_allocator(&sync, 1);
		xe_wait_ufence(fd, &bo_sync->sync, USER_FENCE_VALUE, 0, FIVE_SEC);
		free(bo_sync);
	}

	if (flags & INPUT_IN_SVM) {
		input = aligned_alloc(xe_get_default_alignment(fd), size);
		env.input_addr = to_user_pointer(input);

	} else {
		input_bo = xe_bo_create(fd, env.vm, size, vram_if_possible(fd, 0),
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		input = xe_bo_map(fd, input_bo, size);
		env.input_bo = input_bo;
		env.input_addr = INPUT_BO_ADDR;
	}

	if (flags & OUTPUT_IN_SVM) {
		output = aligned_alloc(xe_get_default_alignment(fd), size);
		env.output_addr = to_user_pointer(output);
	} else {
		output_bo = xe_bo_create(fd, env.vm, size, vram_if_possible(fd, 0),
					 DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		output = xe_bo_map(fd, output_bo, size);
		env.output_bo = output_bo;
		env.output_addr = OUTPUT_BO_ADDR;
	}

	if (flags == LARGE_BUFFER)
		env.loop_count = env.array_size;
	else
		env.loop_count = env.array_size / 2;

	/* Skip check in the library and verify user controls data locally */
	env.skip_results_check = true;

	for (i = 0; i < env.array_size; i++)
		input[i] = (float)i + 2.0f;

	run_intel_compute_kernel(fd, &env, EXECENV_PREF_SYSTEM);

	for (i = 0; i < env.loop_count; i++) {
		float expected_output = input[i] * input[i];

		if (output[i] != expected_output || output[i] == 0.0f)
			igt_debug("[%4d] input:%f output:%f expected_output:%f\n",
				  i, input[i], output[i], expected_output);
		igt_assert_eq_double(output[i], expected_output);
	}

	if (flags & INPUT_IN_SVM) {
		free(input);
	} else {
		munmap(input, size);
		gem_close(fd, input_bo);
	}

	if (flags & OUTPUT_IN_SVM) {
		free(output);
	} else {
		munmap(output, size);
		gem_close(fd, output_bo);
	}

	xe_vm_destroy(fd, env.vm);
}

int igt_main()
{
	int xe, ccs_mode[4];
	unsigned int ip_ver;

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		sriov_enabled = is_sriov_mode(xe);
		ip_ver = intel_graphics_ver(intel_get_drm_devid(xe));
		igt_store_ccs_mode(ccs_mode, ARRAY_SIZE(ccs_mode));
	}

	igt_subtest("compute-square")
		test_compute_square(xe);

	igt_subtest("compute-square-userenv")
		test_compute_square_userenv(xe, 0);

	igt_subtest("compute-square-userenv-large-buffer")
		test_compute_square_userenv(xe, LARGE_BUFFER);

	igt_subtest_group() {
		igt_fixture() {
			igt_require(!xe_supports_faults(xe));
		}
		igt_subtest("compute-square-userenv-isvm")
			test_compute_square_userenv(xe, INPUT_IN_SVM);

		igt_subtest("compute-square-userenv-osvm")
			test_compute_square_userenv(xe, OUTPUT_IN_SVM);

		igt_subtest("compute-square-userenv-iosvm")
			test_compute_square_userenv(xe, INPUT_IN_SVM | OUTPUT_IN_SVM);
	}

	igt_fixture()
		drm_close_driver(xe);

	/* ccs mode tests should be run without open gpu file handles */
	igt_subtest("ccs-mode-basic") {
		/* skip if sriov enabled */
		if (sriov_enabled)
			igt_skip("Skipping test when SRIOV is enabled\n");
		test_ccs_mode();
	}

	igt_subtest("ccs-mode-compute-kernel") {
		/* skip if sriov enabled */
		if (sriov_enabled)
			igt_skip("Skipping test when SRIOV is enabled\n");
		test_compute_kernel_with_ccs_mode();
	}

	/* To test compute function stops after loop_kernel_duration */
	igt_subtest("loop-duration-2s") {
		/* skip test if loop_kernel not supported in pipeline */
		if (ip_ver < IP_VER(20, 0))
			igt_check_supported_pipeline();

		test_compute_kernel_loop(LOOP_DURATION_2s);
	}

	/* test to check available EU utilisation in multi-ccs case */
	igt_subtest("eu-busy-10s") {
		/* skip if sriov enabled */
		if (sriov_enabled)
			igt_skip("Skipping test when SRIOV is enabled\n");

		/* skip test if loop_kernel not supported in pipeline */
		if (ip_ver < IP_VER(20, 0))
			igt_check_supported_pipeline();

		test_eu_busy(5 * LOOP_DURATION_2s);
	}

	igt_fixture() {
		if (!sriov_enabled)
			igt_restore_ccs_mode(ccs_mode, ARRAY_SIZE(ccs_mode));
	}
}
