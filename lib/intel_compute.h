/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Francois Dugast <francois.dugast@intel.com>
 */

#ifndef INTEL_COMPUTE_H
#define INTEL_COMPUTE_H

#include <stdbool.h>

#include "xe_drm.h"

/*
 * OpenCL Kernels are generated using:
 *
 * GPU=tgllp &&                                                         \
 *      ocloc -file opencl/compute_square_kernel.cl -device $GPU &&     \
 *      xxd -i compute_square_kernel_Gen12LPlp.bin
 *
 * For each GPU model desired. A list of supported models can be obtained with: ocloc compile --help
 */

struct intel_compute_kernels {
	int ip_ver;
	unsigned int size;
	const unsigned char *kernel;
	unsigned int sip_kernel_size;
	const unsigned char *sip_kernel;
	unsigned int long_kernel_size;
	const unsigned char *long_kernel;
	unsigned int loop_kernel_size;
	const unsigned char *loop_kernel;
};

/**
 * struct user_execenv - Container of the user-provided execution environment
 */
struct user_execenv {
	/** @vm: use this VM if provided, otherwise create one */
	uint32_t vm;
	/**
	 * @kernel: use this custom kernel if provided, otherwise use a default square kernel
	 *
	 * Custom kernel execution in lib/intel_compute has strong limitations, it does not
	 * allow running any custom kernel. "count" is the size of the input and output arrays
	 * and the provided kernel must have the following prototype:
	 *
	 *    __kernel void square(__global float* input,
	 *                         __global float* output,
	 *                         const unsigned int count)
	 */
	const unsigned char *kernel;
	/** @kernel_size: size of the custom kernel, if provided */
	unsigned int kernel_size;
	/** @skip_results_check: do not verify correctness of the results if true */
	bool skip_results_check;
	/** @array_size: size of input and output arrays */
	uint32_t array_size;
	/** @input_addr: override default address of the input array if provided */
	uint64_t input_addr;
	/** @output_addr: override default address of the output array if provided */
	uint64_t output_addr;
	/** @loop_kernel_duration: duration till kernel should execute in gpu **/
	uint64_t loop_kernel_duration;
};

enum execenv_alloc_prefs {
	EXECENV_PREF_SYSTEM,
	EXECENV_PREF_VRAM,
	EXECENV_PREF_VRAM_IF_POSSIBLE,
};

/**
 * enum xe_compute_preempt_type - Types of compute preemption supported.
 * PREEMPT_TGP: ThreadGroup Preemption
 * PREEMPT_WMTP: Walker Mid Thread Preemption
 */
enum xe_compute_preempt_type {
	PREEMPT_TGP  = 1 << 0,
	PREEMPT_WMTP  = 1 << 1,
};

extern const struct intel_compute_kernels intel_compute_square_kernels[];

bool run_intel_compute_kernel(int fd, struct user_execenv *user,
			      enum execenv_alloc_prefs alloc_prefs);
bool xe_run_intel_compute_kernel_on_engine(int fd, struct drm_xe_engine_class_instance *eci,
					   struct user_execenv *user,
					   enum execenv_alloc_prefs alloc_prefs);
bool run_intel_compute_kernel_preempt(int fd, struct drm_xe_engine_class_instance *eci,
				      bool threadgroup_preemption,
				      enum execenv_alloc_prefs alloc_prefs);
bool xe_kernel_preempt_check(int fd, enum xe_compute_preempt_type required_preempt);
#endif	/* INTEL_COMPUTE_H */
