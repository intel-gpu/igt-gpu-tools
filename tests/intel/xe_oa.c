// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <math.h>

#include "drm.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_oa.h"

/**
 * TEST: perf
 * Category: Core
 * Mega feature: Performance interface
 * Sub-category: Performance tests
 * Functionality: oa
 * Description: Test the Xe OA metrics streaming interface
 * Feature: xe streaming interface, oa
 * Test category: Perf
 */

#define OA_MI_REPORT_PERF_COUNT		((0x28 << 23) | (4 - 2))

#define OAREPORT_REASON_MASK           0x3f
#define OAREPORT_REASON_SHIFT          19
#define OAREPORT_REASON_TIMER          (1<<0)
#define OAREPORT_REASON_INTERNAL       (3<<1)
#define OAREPORT_REASON_CTX_SWITCH     (1<<3)
#define OAREPORT_REASON_GO             (1<<4)
#define OAREPORT_REASON_CLK_RATIO      (1<<5)

#define PIPE_CONTROL_GLOBAL_SNAPSHOT_COUNT_RESET	(1 << 19)
#define PIPE_CONTROL_SYNC_GFDT	  (1 << 17)
#define PIPE_CONTROL_NO_WRITE	   (0 << 14)
#define PIPE_CONTROL_WRITE_IMMEDIATE    (1 << 14)
#define PIPE_CONTROL_WRITE_DEPTH_COUNT  (2 << 14)
#define PIPE_CONTROL_RENDER_TARGET_FLUSH (1 << 12)
#define PIPE_CONTROL_INSTRUCTION_INVALIDATE (1 << 11)
#define PIPE_CONTROL_ISP_DIS	    (1 << 9)
#define PIPE_CONTROL_INTERRUPT_ENABLE   (1 << 8)
/* GT */
#define PIPE_CONTROL_DATA_CACHE_INVALIDATE      (1 << 5)
#define PIPE_CONTROL_PPGTT_WRITE	(0 << 2)
#define PIPE_CONTROL_GLOBAL_GTT_WRITE   (1 << 2)

#define RING_FORCE_TO_NONPRIV_ADDRESS_MASK 0x03fffffc
/*
 * Engine specific registers defined as offsets from engine->mmio_base. For
 * these registers, OR bit[0] with 1 so we can add the mmio_base when running
 * engine specific test.
 */
#define MMIO_BASE_OFFSET 0x1

#define OAG_OASTATUS (0xdafc)
#define OAG_PERF_COUNTER_B(idx) (0xDA94 + 4 * (idx))
#define OAG_OATAILPTR (0xdb04)
#define OAG_OATAILPTR_MASK 0xffffffc0
#define OAG_OABUFFER (0xdb08)

#define XE_OA_MAX_SET_PROPERTIES 16

#define ADD_PROPS(_head, _tail, _key, _value)	\
	do { \
		igt_assert((_tail - _head) < (XE_OA_MAX_SET_PROPERTIES * 2)); \
		*_tail++ = DRM_XE_OA_PROPERTY_##_key; \
		*_tail++ = _value; \
	} while (0)

struct accumulator {
#define MAX_RAW_OA_COUNTERS 62
	enum intel_xe_oa_format_name format;

	uint64_t deltas[MAX_RAW_OA_COUNTERS];
};

struct oa_buf_size {
	char name[12];
	uint32_t size;
} buf_sizes[] = {
	{ "128K", SZ_128K },
	{ "256K", SZ_256K },
	{ "512K", SZ_512K },
	{ "1M", SZ_1M },
	{ "2M", SZ_2M },
	{ "4M", SZ_4M },
	{ "8M", SZ_8M },
	{ "16M", SZ_16M },
	{ "32M", SZ_32M },
	{ "64M", SZ_64M },
	{ "128M", SZ_128M },
};

/* OA unit types */
enum {
	OAG,
	OAR,
	OAM,

	MAX_OA_TYPE,
};

struct oa_format {
	const char *name;
	size_t size;
	int a40_high_off; /* bytes */
	int a40_low_off;
	int n_a40;
	int a64_off;
	int n_a64;
	int a_off;
	int n_a;
	int first_a;
	int first_a40;
	int b_off;
	int n_b;
	int c_off;
	int n_c;
	int oa_type; /* of enum intel_xe_oa_format_name */
	bool report_hdr_64bit;
	int counter_select;
	int counter_size;
	int bc_report;
};

static struct oa_format gen12_oa_formats[XE_OA_FORMAT_MAX] = {
	[XE_OA_FORMAT_A32u40_A4u32_B8_C8] = {
		"A32u40_A4u32_B8_C8", .size = 256,
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 32,
		.a_off = 144, .n_a = 4, .first_a = 32,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAG,
		.counter_select = 5,
	},
};

static struct oa_format dg2_oa_formats[XE_OA_FORMAT_MAX] = {
	[XE_OAR_FORMAT_A32u40_A4u32_B8_C8] = {
		"A32u40_A4u32_B8_C8", .size = 256,
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 32,
		.a_off = 144, .n_a = 4, .first_a = 32,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAR,
		.counter_select = 5,
	},
	/* This format has A36 and A37 interleaved with high bytes of some A
	 * counters, so we will accumulate only subset of counters.
	 */
	[XE_OA_FORMAT_A24u40_A14u32_B8_C8] = {
		"A24u40_A14u32_B8_C8", .size = 256,
		/* u40: A4 - A23 */
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 20, .first_a40 = 4,
		/* u32: A0 - A3 */
		.a_off = 16, .n_a = 4,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAG,
		.counter_select = 5,
	},
	/* This format has 24 u64 counters ranging from A0 - A35. Until we come
	 * up with a better mechanism to define missing counters, we will use a
	 * subset of counters that are indexed by one-increments - A28 - A35.
	 */
	[XE_OAC_FORMAT_A24u64_B8_C8] = {
		"OAC_A24u64_B8_C8", .size = 320,
		.a64_off = 160, .n_a64 = 8,
		.b_off = 224, .n_b = 8,
		.c_off = 256, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAC,
		.report_hdr_64bit = true,
		.counter_select = 1, },
};

static struct oa_format mtl_oa_formats[XE_OA_FORMAT_MAX] = {
	[XE_OAR_FORMAT_A32u40_A4u32_B8_C8] = {
		"A32u40_A4u32_B8_C8", .size = 256,
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 32,
		.a_off = 144, .n_a = 4, .first_a = 32,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAR,
		.counter_select = 5,
	},
	/* This format has A36 and A37 interleaved with high bytes of some A
	 * counters, so we will accumulate only subset of counters.
	 */
	[XE_OA_FORMAT_A24u40_A14u32_B8_C8] = {
		"A24u40_A14u32_B8_C8", .size = 256,
		/* u40: A4 - A23 */
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 20, .first_a40 = 4,
		/* u32: A0 - A3 */
		.a_off = 16, .n_a = 4,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAG,
		.counter_select = 5,
	},

	/* Treat MPEC countes as A counters for now */
	[XE_OAM_FORMAT_MPEC8u64_B8_C8] = {
		"MPEC8u64_B8_C8", .size = 192,
		.a64_off = 32, .n_a64 = 8,
		.b_off = 96, .n_b = 8,
		.c_off = 128, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAM_MPEC,
		.report_hdr_64bit = true,
		.counter_select = 1,
	},
	[XE_OAM_FORMAT_MPEC8u32_B8_C8] = {
		"MPEC8u32_B8_C8", .size = 128,
		.a_off = 32, .n_a = 8,
		.b_off = 64, .n_b = 8,
		.c_off = 96, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAM_MPEC,
		.report_hdr_64bit = true,
		.counter_select = 2,
	},
	/* This format has 24 u64 counters ranging from A0 - A35. Until we come
	 * up with a better mechanism to define missing counters, we will use a
	 * subset of counters that are indexed by one-increments - A28 - A35.
	 */
	[XE_OAC_FORMAT_A24u64_B8_C8] = {
		"OAC_A24u64_B8_C8", .size = 320,
		.a64_off = 160, .n_a64 = 8,
		.b_off = 224, .n_b = 8,
		.c_off = 256, .n_c = 8, .oa_type = DRM_XE_OA_FMT_TYPE_OAC,
		.report_hdr_64bit = true,
		.counter_select = 1, },
};

static struct oa_format lnl_oa_formats[XE_OA_FORMAT_MAX] = {
	[XE_OAM_FORMAT_MPEC8u64_B8_C8] = {
		"MPEC8u64_B8_C8", .size = 192,
		.oa_type = DRM_XE_OA_FMT_TYPE_OAM_MPEC,
		.report_hdr_64bit = true,
		.counter_select = 1,
	},
	[XE_OAM_FORMAT_MPEC8u32_B8_C8] = {
		"MPEC8u32_B8_C8", .size = 128,
		.oa_type = DRM_XE_OA_FMT_TYPE_OAM_MPEC,
		.report_hdr_64bit = true,
		.counter_select = 2,
	},
	[XE_OA_FORMAT_PEC64u64] = {
		"PEC64u64", .size = 576,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 1,
		.counter_size = 1,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC64u64_B8_C8] = {
		"PEC64u64_B8_C8", .size = 640,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 1,
		.counter_size = 1,
		.bc_report = 1 },
	[XE_OA_FORMAT_PEC64u32] = {
		"PEC64u32", .size = 320,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 1,
		.counter_size = 0,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC32u64_G1] = {
		"PEC32u64_G1", .size = 320,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 5,
		.counter_size = 1,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC32u32_G1] = {
		"PEC32u32_G1", .size = 192,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 5,
		.counter_size = 0,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC32u64_G2] = {
		"PEC32u64_G2", .size = 320,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 6,
		.counter_size = 1,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC32u32_G2] = {
		"PEC32u32_G2", .size = 192,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 6,
		.counter_size = 0,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC36u64_G1_32_G2_4] = {
		"PEC36u64_G1_32_G2_4", .size = 320,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 3,
		.counter_size = 1,
		.bc_report = 0 },
	[XE_OA_FORMAT_PEC36u64_G1_4_G2_32] = {
		"PEC36u64_G1_4_G2_32_G2", .size = 320,
		.oa_type = DRM_XE_OA_FMT_TYPE_PEC,
		.report_hdr_64bit = true,
		.counter_select = 4,
		.counter_size = 1,
		.bc_report = 0 },
};

static bool oa_trace = false;
static uint32_t oa_trace_buf_mb = 1;
static int drm_fd = -1;
static int sysfs = -1;
static int pm_fd = -1;
static int stream_fd = -1;
static uint32_t devid;

static struct intel_xe_perf *intel_xe_perf;
static uint64_t oa_exponent_default;
static size_t default_oa_buffer_size;
static struct intel_mmio_data mmio_data;
static igt_render_copyfunc_t render_copy;
static uint32_t rc_width, rc_height;
static uint32_t max_oa_exponent;
static uint32_t min_oa_exponent;
static uint32_t buffer_fill_size;
static uint32_t num_buf_sizes;

/* OA unit names */
static const char *oa_unit_name[] = {
	[DRM_XE_OA_UNIT_TYPE_OAG] = "oag",
	[DRM_XE_OA_UNIT_TYPE_OAM] = "oam",
	[DRM_XE_OA_UNIT_TYPE_OAM_SAG] = "sag",
};

/* Wrapper to deconstify @inst for xe_exec_queue_create */
static u32 xe_exec_queue_create_deconst(int fd, uint32_t vm,
					const struct drm_xe_engine_class_instance *inst,
					uint64_t ext)
{
	return xe_exec_queue_create(fd, vm, (struct drm_xe_engine_class_instance *)inst, ext);
}

static struct intel_xe_perf_metric_set *oa_unit_metric_set(const struct drm_xe_oa_unit *oau)
{
	const char *test_set_name = NULL;
	struct intel_xe_perf_metric_set *metric_set_iter;
	struct intel_xe_perf_metric_set *test_set = NULL;

	if (oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAG)
		test_set_name = "TestOa";
	else if (HAS_OAM(devid) &&
		 (oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM ||
		  oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM_SAG))
		test_set_name = "MediaSet1";
	else
		igt_assert(!"reached");

	igt_list_for_each_entry(metric_set_iter, &intel_xe_perf->metric_sets, link) {
		if (strcmp(metric_set_iter->symbol_name, test_set_name) == 0) {
			test_set = metric_set_iter;
			break;
		}
	}

	igt_assert(test_set);

	/*
	 * configuration was loaded in init_sys_info() ->
	 * intel_xe_perf_load_perf_configs(), and test_set->perf_oa_metrics_set
	 * should point to metric id returned by the config add ioctl. 0 is
	 * invalid.
	 */
	igt_assert_neq_u64(test_set->perf_oa_metrics_set, 0);

	igt_debug("oa_unit %d:%d - %s metric set UUID = %s\n",
		  oau->oa_unit_id,
		  oau->oa_unit_type,
		  test_set->symbol_name,
		  test_set->hw_config_guid);

	return test_set;
}
#define default_test_set oa_unit_metric_set(oa_unit_by_type(drm_fd, DRM_XE_OA_UNIT_TYPE_OAG))

static void set_fd_flags(int fd, int flags)
{
	int old = fcntl(fd, F_GETFL, 0);

	igt_assert_lte(0, old);
	igt_assert_eq(0, fcntl(fd, F_SETFL, old | flags));
}

static u32 get_stream_status(int fd)
{
	struct drm_xe_oa_stream_status status;
	int _e = errno;

	do_ioctl(fd, DRM_XE_OBSERVATION_IOCTL_STATUS, &status);
	igt_debug("oa status %llx\n", status.oa_status);
	errno = _e;

	return status.oa_status;
}

static void enable_trace_log(void)
{
	char cmd[64] = {0};

	if (!oa_trace)
		return;

	snprintf(cmd, sizeof(cmd) - 1, "echo %d > /sys/kernel/debug/tracing/buffer_size_kb", oa_trace_buf_mb * 1024);
	system(cmd);
	system("echo 0 > /sys/kernel/debug/tracing/tracing_on");
	system("echo > /sys/kernel/debug/tracing/trace");
	system("echo 1 > /sys/kernel/debug/tracing/events/xe/enable");
	system("echo 1 > /sys/kernel/debug/tracing/events/xe/xe_reg_rw/enable");
	system("echo 1 > /sys/kernel/debug/tracing/tracing_on");
}

static void disable_trace_log(void)
{
	if (!oa_trace)
		return;

	system("echo 0 > /sys/kernel/debug/tracing/tracing_on");
	system("cat /sys/kernel/debug/tracing/trace");
}

static void
dump_report(const uint32_t *report, uint32_t size, const char *message) {
	uint32_t i;
	igt_debug("%s\n", message);
	for (i = 0; i < size; i += 4) {
		igt_debug("%08x %08x %08x %08x\n",
				report[i],
				report[i + 1],
				report[i + 2],
				report[i + 3]);
	}
}

static struct oa_format
get_oa_format(enum intel_xe_oa_format_name format)
{
	if (IS_DG2(devid))
		return dg2_oa_formats[format];
	else if (IS_METEORLAKE(devid))
		return mtl_oa_formats[format];
	else if (intel_graphics_ver(devid) >= IP_VER(20, 0))
		return lnl_oa_formats[format];
	else
		return gen12_oa_formats[format];
}

static u64 oa_format_fields(u64 name)
{
#define FIELD_PREP_ULL(_mask, _val) \
	(((_val) << (__builtin_ffsll(_mask) - 1)) & (_mask))

	struct oa_format f = get_oa_format(name);

	/* 0 format name is invalid */
	if (!name)
		memset(&f, 0xff, sizeof(f));

	return FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, (u64)f.oa_type) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, (u64)f.counter_select) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, (u64)f.counter_size) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, (u64)f.bc_report);
}
#define __ff oa_format_fields

static const struct drm_xe_engine_class_instance *oa_unit_engine(const struct drm_xe_oa_unit *oau)
{
	return !oau ? NULL : oau->num_engines ? &oau->eci[random() % oau->num_engines] : NULL;
}

static int __first_and_num_oa_units(const struct drm_xe_oa_unit **oau)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(drm_fd);

	*oau = (const struct drm_xe_oa_unit *)&qoa->oa_units[0];

	return qoa->num_oa_units;
}

static const struct drm_xe_oa_unit *__next_oa_unit(const struct drm_xe_oa_unit *oau)
{
	u8 *poau = (u8 *)oau;

	return (const struct drm_xe_oa_unit *)(poau + sizeof(*oau) +
					       oau->num_engines * sizeof(oau->eci[0]));
}

#define for_each_oa_unit(oau) \
	for (int _i = 0, _num_oa_units = __first_and_num_oa_units(&oau); \
	     _i < _num_oa_units; oau = __next_oa_unit(oau), _i++)

static const struct drm_xe_oa_unit *oa_unit_by_id(int fd, int id)
{
	const struct drm_xe_oa_unit *oau;

	for_each_oa_unit(oau) {
		if (oau->oa_unit_id == id)
			return oau;
	}

	return NULL;
}

static const struct drm_xe_oa_unit *oa_unit_by_type(int fd, int t)
{
	const struct drm_xe_oa_unit *oau;

	for_each_oa_unit(oau) {
		if (oau->oa_unit_type == t)
			return oau;
	}

	return NULL;
}

static char *
pretty_print_oa_period(uint64_t oa_period_ns)
{
	static char result[100];
	static const char *units[4] = { "ns", "us", "ms", "s" };
	double val = oa_period_ns;
	int iter = 0;

	while (iter < (ARRAY_SIZE(units) - 1) &&
	       val >= 1000.0f) {
		val /= 1000.0f;
		iter++;
	}

	snprintf(result, sizeof(result), "%.3f%s", val, units[iter]);
	return result;
}

static void
__perf_close(int fd)
{
	close(fd);
	stream_fd = -1;

	if (pm_fd >= 0) {
		close(pm_fd);
		pm_fd = -1;
	}
}

static int
__perf_open(int fd, struct intel_xe_oa_open_prop *param, bool prevent_pm)
{
	int ret;
	int32_t pm_value = 0;

	if (stream_fd >= 0)
		__perf_close(stream_fd);
	if (pm_fd >= 0) {
		close(pm_fd);
		pm_fd = -1;
	}

	ret = intel_xe_perf_ioctl(fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, param);

	igt_assert_lte(0, ret);
	errno = 0;

	if (prevent_pm) {
		pm_fd = open("/dev/cpu_dma_latency", O_RDWR);
		igt_assert_lte(0, pm_fd);

		igt_assert_eq(write(pm_fd, &pm_value, sizeof(pm_value)), sizeof(pm_value));
	}

	return ret;
}

static size_t get_default_oa_buffer_size(int fd)
{
	struct drm_xe_oa_stream_info info;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(fd, &param, false);
	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_INFO, &info);
	__perf_close(stream_fd);

	return info.oa_buf_size;
}

static uint64_t
read_u64_file(const char *path)
{
	FILE *f;
	uint64_t val;

	f = fopen(path, "r");
	igt_assert(f);

	igt_assert_eq(fscanf(f, "%"PRIu64, &val), 1);

	fclose(f);

	return val;
}

static void
write_u64_file(const char *path, uint64_t val)
{
	FILE *f;

	f = fopen(path, "w");
	igt_assert(f);

	igt_assert(fprintf(f, "%"PRIu64, val) > 0);

	fclose(f);
}

static bool
try_sysfs_read_u64(const char *path, uint64_t *val)
{
	return igt_sysfs_scanf(sysfs, path, "%"PRIu64, val) == 1;
}

static unsigned long rc6_residency_ms(void)
{
	unsigned long value;

	igt_assert(igt_sysfs_scanf(sysfs, "device/tile0/gt0/gtidle/idle_residency_ms", "%lu", &value) == 1);
	return value;
}

static uint64_t
read_report_ticks(const uint32_t *report, enum intel_xe_oa_format_name format)
{

	struct oa_format fmt = get_oa_format(format);

	return fmt.report_hdr_64bit ? *(uint64_t *)&report[6] : report[3];
}

/*
 * t0 is a value sampled before t1. width is number of bits used to represent
 * t0/t1. Normally t1 is greater than t0. In cases where t1 < t0 use this
 * helper. Since the size of t1/t0 is already 64 bits, no special handling is
 * needed for width = 64.
 */
static uint64_t
elapsed_delta(uint64_t t1, uint64_t t0, uint32_t width)
{
	uint32_t max_bits = sizeof(t1) * 8;

	igt_assert_lte_u32(width, max_bits);

	if (t1 < t0 && width != max_bits)
		return ((1ULL << width) - t0) + t1;

	return t1 - t0;
}

static uint64_t
oa_tick_delta(const uint32_t *report1,
	      const uint32_t *report0,
	      enum intel_xe_oa_format_name format)
{
	struct oa_format fmt = get_oa_format(format);
	uint32_t width = fmt.report_hdr_64bit ? 64 : 32;

	return elapsed_delta(read_report_ticks(report1, format),
			     read_report_ticks(report0, format), width);
}

static void
read_report_clock_ratios(const uint32_t *report,
			      uint32_t *slice_freq_mhz,
			      uint32_t *unslice_freq_mhz)
{
	uint32_t unslice_freq = report[0] & 0x1ff;
	uint32_t slice_freq_low = (report[0] >> 25) & 0x7f;
	uint32_t slice_freq_high = (report[0] >> 9) & 0x3;
	uint32_t slice_freq = slice_freq_low | (slice_freq_high << 7);

	*slice_freq_mhz = (slice_freq * 16666) / 1000;
	*unslice_freq_mhz = (unslice_freq * 16666) / 1000;
}

static uint32_t
report_reason(const uint32_t *report)
{
	return ((report[0] >> OAREPORT_REASON_SHIFT) &
		OAREPORT_REASON_MASK);
}

static const char *
read_report_reason(const uint32_t *report)
{
	uint32_t reason = report_reason(report);

	if (reason & (1<<0))
		return "timer";
	else if (reason & (1<<1))
	      return "internal trigger 1";
	else if (reason & (1<<2))
	      return "internal trigger 2";
	else if (reason & (1<<3))
	      return "context switch";
	else if (reason & (1<<4))
	      return "GO 1->0 transition (enter RC6)";
	else if (reason & (1<<5))
		return "[un]slice clock ratio change";
	else
		return "unknown";
}

static uint32_t
cs_timestamp_frequency(int fd)
{
	return xe_gt_list(drm_fd)->gt_list[0].reference_clock;
}

static uint64_t
cs_timebase_scale(uint32_t u32_delta)
{
	return ((uint64_t)u32_delta * NSEC_PER_SEC) / cs_timestamp_frequency(drm_fd);
}

static uint64_t
oa_timestamp(const uint32_t *report, enum intel_xe_oa_format_name format)
{
	struct oa_format fmt = get_oa_format(format);

	return fmt.report_hdr_64bit ? *(uint64_t *)&report[2] : report[1];
}

static uint64_t
oa_timestamp_delta(const uint32_t *report1,
		   const uint32_t *report0,
		   enum intel_xe_oa_format_name format)
{
	uint32_t width = intel_graphics_ver(devid) >= IP_VER(12, 55) ? 56 : 32;

	return elapsed_delta(oa_timestamp(report1, format),
			     oa_timestamp(report0, format), width);
}

static uint64_t
timebase_scale(uint64_t delta)
{
	return (delta * NSEC_PER_SEC) / intel_xe_perf->devinfo.timestamp_frequency;
}

/* Returns: the largest OA exponent that will still result in a sampling period
 * less than or equal to the given @period_ns.
 */
static int
max_oa_exponent_for_period_lte(uint64_t period_ns)
{
	/* NB: timebase_scale() takes a uint64_t and an exponent of 30
	 * would already represent a period of ~3 minutes so there's
	 * really no need to consider higher exponents.
	 */
	for (int i = 0; i < 30; i++) {
		uint64_t oa_period = timebase_scale(2 << i);

		if (oa_period > period_ns)
			return max(0, i - 1);
	}

	igt_assert(!"reached");
	return -1;
}

static uint64_t
oa_exponent_to_ns(int exponent)
{
       return 1000000000ULL * (2ULL << exponent) / intel_xe_perf->devinfo.timestamp_frequency;
}

static bool
oa_report_ctx_is_valid(uint32_t *report)
{
	return report[0] & (1ul << 16);
}

static uint32_t
oa_report_get_ctx_id(uint32_t *report)
{
	if (!oa_report_ctx_is_valid(report))
		return 0xffffffff;
	return report[2];
}

static int
oar_unit_default_format(void)
{
	if (IS_DG2(devid) || IS_METEORLAKE(devid))
		return XE_OAR_FORMAT_A32u40_A4u32_B8_C8;

	return default_test_set->perf_oa_format;
}

static void *buf_map(int fd, struct intel_buf *buf, bool write)
{
	void *p;

	if (is_xe_device(fd)) {
		buf->ptr = xe_bo_map(fd, buf->handle, buf->bo_size);
		p = buf->ptr;
	} else {
		if (gem_has_llc(fd))
			p = intel_buf_cpu_map(buf, write);
		else
			p = intel_buf_device_map(buf, write);
	}
	return p;
}

static void
scratch_buf_memset(struct intel_buf *buf, int width, int height, uint32_t color)
{
	buf_map(buf_ops_get_fd(buf->bops), buf, true);

	for (int i = 0; i < width * height; i++)
		buf->ptr[i] = color;

	intel_buf_unmap(buf);
}

static void
scratch_buf_init(struct buf_ops *bops,
		 struct intel_buf *buf,
		 int width, int height,
		 uint32_t color)
{
	intel_buf_init(bops, buf, width, height, 32, 0,
		       I915_TILING_NONE, I915_COMPRESSION_NONE);
	scratch_buf_memset(buf, width, height, color);
}

static void
emit_report_perf_count(struct intel_bb *ibb,
		       struct intel_buf *dst,
		       int dst_offset,
		       uint32_t report_id)
{
	intel_bb_add_intel_buf(ibb, dst, true);

	intel_bb_out(ibb, OA_MI_REPORT_PERF_COUNT);
	intel_bb_emit_reloc(ibb, dst->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			    dst_offset, dst->addr.offset);
	intel_bb_out(ibb, report_id);
}

static bool
oa_report_is_periodic(const uint32_t *report)
{
	if (report_reason(report) & OAREPORT_REASON_TIMER)
		return true;

	return false;
}

static uint64_t
read_40bit_a_counter(const uint32_t *report,
			  enum intel_xe_oa_format_name fmt, int a_id)
{
	struct oa_format format = get_oa_format(fmt);
	uint8_t *a40_high = (((uint8_t *)report) + format.a40_high_off);
	uint32_t *a40_low = (uint32_t *)(((uint8_t *)report) +
					 format.a40_low_off);
	uint64_t high = (uint64_t)(a40_high[a_id]) << 32;

	return a40_low[a_id] | high;
}

static uint64_t
xehpsdv_read_64bit_a_counter(const uint32_t *report, enum intel_xe_oa_format_name fmt, int a_id)
{
	struct oa_format format = get_oa_format(fmt);
	uint64_t *a64 = (uint64_t *)(((uint8_t *)report) + format.a64_off);

	return a64[a_id];
}

static uint64_t
get_40bit_a_delta(uint64_t value0, uint64_t value1)
{
	if (value0 > value1)
		return (1ULL << 40) + value1 - value0;
	else
		return value1 - value0;
}

static void
accumulate_uint32(size_t offset,
		  uint32_t *report0,
                  uint32_t *report1,
                  uint64_t *delta)
{
	uint32_t value0 = *(uint32_t *)(((uint8_t *)report0) + offset);
	uint32_t value1 = *(uint32_t *)(((uint8_t *)report1) + offset);

	*delta += (uint32_t)(value1 - value0);
}

static void
accumulate_uint40(int a_index,
                  uint32_t *report0,
                  uint32_t *report1,
		  enum intel_xe_oa_format_name format,
                  uint64_t *delta)
{
	uint64_t value0 = read_40bit_a_counter(report0, format, a_index),
		 value1 = read_40bit_a_counter(report1, format, a_index);

	*delta += get_40bit_a_delta(value0, value1);
}

static void
accumulate_uint64(int a_index,
		  const uint32_t *report0,
		  const uint32_t *report1,
		  enum intel_xe_oa_format_name format,
		  uint64_t *delta)
{
	uint64_t value0 = xehpsdv_read_64bit_a_counter(report0, format, a_index),
		 value1 = xehpsdv_read_64bit_a_counter(report1, format, a_index);

	*delta += (value1 - value0);
}

static void
accumulate_reports(struct accumulator *accumulator,
		   uint32_t *start,
		   uint32_t *end)
{
	struct oa_format format = get_oa_format(accumulator->format);
	uint64_t *deltas = accumulator->deltas;
	int idx = 0;

	/* timestamp */
	deltas[idx] += oa_timestamp_delta(end, start, accumulator->format);
	idx++;

	/* clock cycles */
	deltas[idx] += oa_tick_delta(end, start, accumulator->format);
	idx++;

	for (int i = 0; i < format.n_a40; i++) {
		accumulate_uint40(i, start, end, accumulator->format,
				  deltas + idx++);
	}

	for (int i = 0; i < format.n_a64; i++) {
		accumulate_uint64(i, start, end, accumulator->format,
				  deltas + idx++);
	}

	for (int i = 0; i < format.n_a; i++) {
		accumulate_uint32(format.a_off + 4 * i,
				  start, end, deltas + idx++);
	}

	for (int i = 0; i < format.n_b; i++) {
		accumulate_uint32(format.b_off + 4 * i,
				  start, end, deltas + idx++);
	}

	for (int i = 0; i < format.n_c; i++) {
		accumulate_uint32(format.c_off + 4 * i,
				  start, end, deltas + idx++);
	}
}

static void
accumulator_print(struct accumulator *accumulator, const char *title)
{
	struct oa_format format = get_oa_format(accumulator->format);
	uint64_t *deltas = accumulator->deltas;
	int idx = 0;

	igt_debug("%s:\n", title);
	igt_debug("\ttime delta = %"PRIu64"\n", deltas[idx++]);
	igt_debug("\tclock cycle delta = %"PRIu64"\n", deltas[idx++]);

	for (int i = 0; i < format.n_a40; i++)
		igt_debug("\tA%u = %"PRIu64"\n", i, deltas[idx++]);

	for (int i = 0; i < format.n_a64; i++)
		igt_debug("\tA64_%u = %"PRIu64"\n", i, deltas[idx++]);

	for (int i = 0; i < format.n_a; i++) {
		int a_id = format.first_a + i;
		igt_debug("\tA%u = %"PRIu64"\n", a_id, deltas[idx++]);
	}

	for (int i = 0; i < format.n_a; i++)
		igt_debug("\tB%u = %"PRIu64"\n", i, deltas[idx++]);

	for (int i = 0; i < format.n_c; i++)
		igt_debug("\tC%u = %"PRIu64"\n", i, deltas[idx++]);
}


/*
 * pec_sanity_check_reports() uses the following properties of the TestOa
 * metric set with the "576B_PEC64LL" or XE_OA_FORMAT_PEC64u64 format. See
 * e.g. lib/xe/oa-configs/oa-lnl.xml.
 *
 * If pec[] is the array of pec qwords following the report header (Bspec
 * 60942) then we have:
 *
 *	pec[2]  : test_event1_cycles
 *	pec[3]  : test_event1_cycles_xecore0
 *	pec[4]  : test_event1_cycles_xecore1
 *	pec[5]  : test_event1_cycles_xecore2
 *	pec[6]  : test_event1_cycles_xecore3
 *	pec[21] : test_event1_cycles_xecore4
 *	pec[22] : test_event1_cycles_xecore5
 *	pec[23] : test_event1_cycles_xecore6
 *	pec[24] : test_event1_cycles_xecore7
 *
 * test_event1_cycles_xecore* increment with every clock, so they increment
 * the same as gpu_ticks in report headers in successive reports. And
 * test_event1_cycles increment by 'gpu_ticks * num_xecores'.
 *
 * These equations are not exact due to fluctuations, but are precise when
 * averaged over long periods.
 */
static void pec_sanity_check(const u32 *report0, const u32 *report1,
			     struct intel_xe_perf_metric_set *set)
{
	u64 tick_delta = oa_tick_delta(report1, report0, set->perf_oa_format);
	int xecore_to_pec[] = {3, 4, 5, 6, 21, 22, 23, 24};
	u64 *pec0 = (u64 *)(report0 + 8);
	u64 *pec1 = (u64 *)(report1 + 8);

	/*
	 * Empirical testing revealed that when reports of different types/reasons are
	 * intermixed, this throws off gpu_ticks and test_event1_cycles_xecore* in PEC
	 * data, causing test failures. To avoid this, restrict testing to only
	 * timer/periodic reports.
	 */
	if (strcmp(read_report_reason(report0), "timer") ||
	    strcmp(read_report_reason(report1), "timer")) {
		igt_debug("Only checking timer reports: %s->%s\n",
		  read_report_reason(report0), read_report_reason(report1));
		return;
	}

	igt_debug("tick delta = %#" PRIx64 "\n", tick_delta);

	/* Difference in test_event1_cycles_xecore* values should be close to tick_delta */
	for (int i = 0; i < ARRAY_SIZE(xecore_to_pec); i++) {
		int n = xecore_to_pec[i];

		igt_debug("n %d: pec1[n] - pec0[n] %#" PRIx64 ", tick delta %#" PRIx64 "\n",
			  n, pec1[n] - pec0[n], tick_delta);

		/* Skip missing xecore's */
		if (intel_xe_perf->devinfo.subslice_mask & BIT(i)) {
			igt_assert(pec1[n] && pec0[n]);
			assert_within_epsilon(pec1[n] - pec0[n], tick_delta, 0.1);
		}
	}

	igt_debug("pec1[2] - pec0[2] %#" PRIx64 ", tick_delta * num_xecores: %#" PRIx64 "\n",
		  pec1[2] - pec0[2], tick_delta * intel_xe_perf->devinfo.n_eu_sub_slices);
	/* Difference in test_event1_cycles should be close to (tick_delta * num_xecores) */
	assert_within_epsilon(pec1[2] - pec0[2],
			      tick_delta * intel_xe_perf->devinfo.n_eu_sub_slices, 0.1);
}

/* Sanity check Xe2+ PEC reports. Note: report format must be @set->perf_oa_format */
static void pec_sanity_check_reports(const u32 *report0, const u32 *report1,
				     struct intel_xe_perf_metric_set *set)
{
	if (igt_run_in_simulation() || intel_graphics_ver(devid) < IP_VER(20, 0)) {
		igt_debug("%s: Skip checking PEC reports in simulation or Xe1\n", __func__);
		return;
	}

	if (strcmp(set->name, "TestOa")) {
		igt_debug("%s: Can't check reports for metric set %s\n", __func__, set->name);
		return;
	}

	dump_report(report0, set->perf_raw_size / 4, "pec_report0");
	dump_report(report1, set->perf_raw_size / 4, "pec_report1");

	pec_sanity_check(report0, report1, set);
}

/* The TestOa metric set is designed so */
static void
sanity_check_reports(const uint32_t *oa_report0, const uint32_t *oa_report1,
		     enum intel_xe_oa_format_name fmt)
{
	struct oa_format format = get_oa_format(fmt);
	uint64_t time_delta = timebase_scale(oa_timestamp_delta(oa_report1,
								oa_report0,
								fmt));
	uint64_t clock_delta = oa_tick_delta(oa_report1, oa_report0, fmt);
	uint64_t max_delta;
	uint64_t freq;
	uint32_t *rpt0_b = (uint32_t *)(((uint8_t *)oa_report0) +
					format.b_off);
	uint32_t *rpt1_b = (uint32_t *)(((uint8_t *)oa_report1) +
					format.b_off);
	uint32_t b;
	uint32_t ref;

	igt_debug("report type: %s->%s\n",
		  read_report_reason(oa_report0),
		  read_report_reason(oa_report1));

	freq = time_delta ? (clock_delta * 1000) / time_delta : 0;
	igt_debug("freq = %"PRIu64"\n", freq);

	igt_debug("clock delta = %"PRIu64"\n", clock_delta);

	max_delta = clock_delta * intel_xe_perf->devinfo.n_eus;

	/* Gen8+ has some 40bit A counters... */
	for (int j = format.first_a40; j < format.n_a40 + format.first_a40; j++) {
		uint64_t value0 = read_40bit_a_counter(oa_report0, fmt, j);
		uint64_t value1 = read_40bit_a_counter(oa_report1, fmt, j);
		uint64_t delta = get_40bit_a_delta(value0, value1);

		igt_debug("A40_%d: delta = %"PRIu64"\n", j, delta);
		igt_assert_f(delta <= max_delta,
			     "A40_%d: delta = %"PRIu64", max_delta = %"PRIu64"\n",
			     j, delta, max_delta);
	}

	for (int j = 0; j < format.n_a64; j++) {
		uint64_t delta = 0;

		accumulate_uint64(j, oa_report0, oa_report1, fmt, &delta);

		igt_debug("A64_%d: delta = %"PRIu64"\n", format.first_a + j, delta);
		igt_assert_f(delta <= max_delta,
			     "A64_%d: delta = %"PRIu64", max_delta = %"PRIu64"\n",
			     format.first_a + j, delta, max_delta);
	}

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.a_off);
		uint32_t *a1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.a_off);
		int a_id = format.first_a + j;
		uint32_t delta = a1[j] - a0[j];

		igt_debug("A%d: delta = %"PRIu32"\n", a_id, delta);
		igt_assert_f(delta <= max_delta,
			     "A%d: delta = %"PRIu32", max_delta = %"PRIu64"\n",
			     a_id, delta, max_delta);
	}

	/* The TestOa metric set defines all B counters to be a
	 * multiple of the gpu clock
	 */
	if (format.n_b && (format.oa_type == DRM_XE_OA_FMT_TYPE_OAG || format.oa_type == DRM_XE_OA_FMT_TYPE_OAR)) {
		if (clock_delta > 0) {
			b = rpt1_b[0] - rpt0_b[0];
			igt_debug("B0: delta = %"PRIu32"\n", b);
			igt_assert_eq(b, 0);

			b = rpt1_b[1] - rpt0_b[1];
			igt_debug("B1: delta = %"PRIu32"\n", b);
			igt_assert_eq(b, clock_delta);

			b = rpt1_b[2] - rpt0_b[2];
			igt_debug("B2: delta = %"PRIu32"\n", b);
			igt_assert_eq(b, clock_delta);

			b = rpt1_b[3] - rpt0_b[3];
			ref = clock_delta / 2;
			igt_debug("B3: delta = %"PRIu32"\n", b);
			igt_assert(b >= ref - 1 && b <= ref + 1);

			b = rpt1_b[4] - rpt0_b[4];
			ref = clock_delta / 3;
			igt_debug("B4: delta = %"PRIu32"\n", b);
			igt_assert(b >= ref - 1 && b <= ref + 1);

			b = rpt1_b[5] - rpt0_b[5];
			ref = clock_delta / 3;
			igt_debug("B5: delta = %"PRIu32"\n", b);
			igt_assert(b >= ref - 1 && b <= ref + 1);

			b = rpt1_b[6] - rpt0_b[6];
			ref = clock_delta / 6;
			igt_debug("B6: delta = %"PRIu32"\n", b);
			igt_assert(b >= ref - 1 && b <= ref + 1);

			b = rpt1_b[7] - rpt0_b[7];
			ref = clock_delta * 2 / 3;
			igt_debug("B7: delta = %"PRIu32"\n", b);
			igt_assert(b >= ref - 1 && b <= ref + 1);
		} else {
			for (int j = 0; j < format.n_b; j++) {
				b = rpt1_b[j] - rpt0_b[j];
				igt_debug("B%i: delta = %"PRIu32"\n", j, b);
				igt_assert_eq(b, 0);
			}
		}
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.c_off);
		uint32_t *c1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.c_off);
		uint32_t delta = c1[j] - c0[j];

		igt_debug("C%d: delta = %"PRIu32", max_delta=%"PRIu64"\n",
			  j, delta, max_delta);
		igt_assert_f(delta <= max_delta,
			     "C%d: delta = %"PRIu32", max_delta = %"PRIu64"\n",
			     j, delta, max_delta);
	}
}

static bool
init_sys_info(void)
{
	igt_assert_neq(devid, 0);

	intel_xe_perf = intel_xe_perf_for_fd(drm_fd, 0);
	igt_require(intel_xe_perf);

	igt_debug("n_eu_slices: %"PRIu64"\n", intel_xe_perf->devinfo.n_eu_slices);
	igt_debug("n_eu_sub_slices: %"PRIu64"\n", intel_xe_perf->devinfo.n_eu_sub_slices);
	igt_debug("n_eus: %"PRIu64"\n", intel_xe_perf->devinfo.n_eus);
	igt_debug("subslice_mask: %#"PRIx64"\n", intel_xe_perf->devinfo.subslice_mask);
	igt_debug("timestamp_frequency = %"PRIu64"\n",
		  intel_xe_perf->devinfo.timestamp_frequency);
	igt_assert_neq(intel_xe_perf->devinfo.timestamp_frequency, 0);

	intel_xe_perf_load_perf_configs(intel_xe_perf, drm_fd);

	if (igt_run_in_simulation()) {
		igt_debug("SIMULATION run\n");
		min_oa_exponent = 5;
		max_oa_exponent = 10;
		rc_width = 64;
		rc_height = 36;
		buffer_fill_size = SZ_128K;
		num_buf_sizes = 3;
		oa_exponent_default = max_oa_exponent_for_period_lte(1000);
	} else {
		igt_debug("HW run\n");
		min_oa_exponent = 5;
		max_oa_exponent = 20;
		rc_width = 1920;
		rc_height = 1080;
		buffer_fill_size = SZ_16M;
		num_buf_sizes = ARRAY_SIZE(buf_sizes);
		oa_exponent_default = max_oa_exponent_for_period_lte(1000000);
	}

	default_oa_buffer_size = get_default_oa_buffer_size(drm_fd);
	igt_debug("default_oa_buffer_size: %zu\n", default_oa_buffer_size);

	return true;
}

/**
 * SUBTEST: non-system-wide-paranoid
 * Description: CAP_SYS_ADMIN is required to open system wide metrics, unless
 *		sysctl parameter dev.xe.observation_paranoid == 0
 */
static void test_system_wide_paranoid(void)
{
	igt_fork(child, 1) {
		uint64_t properties[] = {
			DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

			/* Include OA reports in samples */
			DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
			DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
			DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		};
		struct intel_xe_oa_open_prop param = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};

		write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

		igt_drop_root();

		intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EACCES);
	}

	igt_waitchildren();

	igt_fork(child, 1) {
		uint64_t properties[] = {
			DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

			/* Include OA reports in samples */
			DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
			DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
			DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		};
		struct intel_xe_oa_open_prop param = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};
		write_u64_file("/proc/sys/dev/xe/observation_paranoid", 0);

		igt_drop_root();

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	igt_waitchildren();

	/* leave in paranoid state */
	write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);
}

/**
 * SUBTEST: invalid-oa-metric-set-id
 * Description: Test behavior for invalid metric set id's
 */
static void test_invalid_oa_metric_set_id(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, UINT64_MAX,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);

	properties[ARRAY_SIZE(properties) - 1] = 0; /* ID 0 is also be reserved as invalid */
	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);

	/* Check that we aren't just seeing false positives... */
	properties[ARRAY_SIZE(properties) - 1] = default_test_set->perf_oa_metrics_set;
	stream_fd = __perf_open(drm_fd, &param, false);
	__perf_close(stream_fd);

	/* There's no valid default OA metric set ID... */
	param.num_properties--;
	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);
}

/**
 * SUBTEST: invalid-oa-format-id
 * Description: Test behavior for invalid OA format fields
 */
static void test_invalid_oa_format_id(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_FORMAT, UINT64_MAX, /* No __ff() here */
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);

	properties[ARRAY_SIZE(properties) - 1] = __ff(0); /* ID 0 is also be reserved as invalid */
	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);

	/* Check that we aren't just seeing false positives... */
	properties[ARRAY_SIZE(properties) - 1] = __ff(default_test_set->perf_oa_format);
	stream_fd = __perf_open(drm_fd, &param, false);
	__perf_close(stream_fd);
	/* There's no valid default OA format... */
	param.num_properties--;
	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);
}

/**
 * SUBTEST: missing-sample-flags
 * Description: Test behavior for no SAMPLE_OA and no EXEC_QUEUE_ID
 */
static void test_missing_sample_flags(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* No _PROP_SAMPLE_xyz flags */

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);
}

static void
read_2_oa_reports(int format_id,
		  int exponent,
		  uint32_t *oa_report0,
		  uint32_t *oa_report1,
		  bool timer_only)
{
	size_t format_size = get_oa_format(format_id).size;
	uint32_t exponent_mask = (1 << (exponent + 1)) - 1;

	/* Note: we allocate a large buffer so that each read() iteration
	 * should scrape *all* pending records.
	 *
	 * The largest buffer the OA unit supports is 16MB.
	 *
	 * Being sure we are fetching all buffered reports allows us to
	 * potentially throw away / skip all reports whenever we see
	 * a _REPORT_LOST notification as a way of being sure are
	 * measurements aren't skewed by a lost report.
	 *
	 * Note: that is is useful for some tests but also not something
	 * applications would be expected to resort to. Lost reports are
	 * somewhat unpredictable but typically don't pose a problem - except
	 * to indicate that the OA unit may be over taxed if lots of reports
	 * are being lost.
	 */
	int max_reports = default_oa_buffer_size / format_size;
	int buf_size = format_size * max_reports * 1.5;
	uint8_t *buf = malloc(buf_size);
	ssize_t len = 0;
	int n = 0;

	for (int i = 0; i < 1000; i++) {
		u32 oa_status = 0;
		int ret;

		while ((ret = read(stream_fd, buf + len, buf_size)) < 0 && errno == EINTR)
			;
		if (ret < 0 && errno == EIO) {
			oa_status = get_stream_status(stream_fd);
			continue;
		}

		igt_assert(ret > 0);
		igt_debug("read %d bytes\n", (int)ret);

		len += ret;
		/* Need at least 2 reports */
		if (len < 2 * format_size)
			continue;

		for (size_t offset = 0; offset < len; offset += format_size) {
			const uint32_t *report = (void *)(buf + offset);

			/* Currently the only test that should ever expect to
			 * see a _BUFFER_LOST error is the buffer_fill test,
			 * otherwise something bad has probably happened...
			 */
			igt_assert(!(oa_status & DRM_XE_OASTATUS_BUFFER_OVERFLOW));

			/* At high sampling frequencies the OA HW might not be
			 * able to cope with all write requests and will notify
			 * us that a report was lost. We restart our read of
			 * two sequential reports due to the timeline blip this
			 * implies
			 */
			if (oa_status & DRM_XE_OASTATUS_REPORT_LOST) {
				igt_debug("read restart: OA trigger collision / report lost\n");
				n = 0;

				/* XXX: break, because we don't know where
				 * within the series of already read reports
				 * there could be a blip from the lost report.
				 */
				break;
			}

			dump_report(report, format_size / 4, "oa-formats");

			igt_debug("read report: reason = %x, timestamp = %"PRIx64", exponent mask=%x\n",
				  report[0], oa_timestamp(report, format_id), exponent_mask);

			/* Don't expect zero for timestamps */
			igt_assert_neq_u64(oa_timestamp(report, format_id), 0);

			if (timer_only) {
				if (!oa_report_is_periodic(report)) {
					igt_debug("skipping non timer report\n");
					continue;
				}
			}

			if (n++ == 0)
				memcpy(oa_report0, report, format_size);
			else {
				memcpy(oa_report1, report, format_size);
				free(buf);
				return;
			}
		}
	}

	free(buf);

	igt_assert(!"reached");
}

static void
open_and_read_2_oa_reports(int format_id,
			   int exponent,
			   uint32_t *oa_report0,
			   uint32_t *oa_report1,
			   bool timer_only,
			   const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(format_id),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, exponent,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(drm_fd, &param, false);
	set_fd_flags(stream_fd, O_CLOEXEC);

	read_2_oa_reports(format_id, exponent,
			  oa_report0, oa_report1, timer_only);

	__perf_close(stream_fd);
}

static void
print_reports(uint32_t *oa_report0, uint32_t *oa_report1, int fmt)
{
	struct oa_format format = get_oa_format(fmt);
	uint64_t ts0 = oa_timestamp(oa_report0, fmt);
	uint64_t ts1 = oa_timestamp(oa_report1, fmt);

	igt_debug("TIMESTAMP: 1st = %"PRIu64", 2nd = %"PRIu64", delta = %"PRIu64"\n",
		  ts0, ts1, ts1 - ts0);

	{
		uint64_t clock0 = read_report_ticks(oa_report0, fmt);
		uint64_t clock1 = read_report_ticks(oa_report1, fmt);

		igt_debug("CLOCK: 1st = %"PRIu64", 2nd = %"PRIu64", delta = %"PRIu64"\n",
			  clock0, clock1, clock1 - clock0);
	}

	{
		uint32_t slice_freq0, slice_freq1, unslice_freq0, unslice_freq1;
		const char *reason0 = read_report_reason(oa_report0);
		const char *reason1 = read_report_reason(oa_report1);

		igt_debug("CTX ID: 1st = %"PRIu32", 2nd = %"PRIu32"\n",
			  oa_report0[2], oa_report1[2]);

		read_report_clock_ratios(oa_report0,
					 &slice_freq0, &unslice_freq0);
		read_report_clock_ratios(oa_report1,
					 &slice_freq1, &unslice_freq1);

		igt_debug("SLICE CLK: 1st = %umhz, 2nd = %umhz, delta = %d\n",
			  slice_freq0, slice_freq1,
			  ((int)slice_freq1 - (int)slice_freq0));
		igt_debug("UNSLICE CLK: 1st = %umhz, 2nd = %umhz, delta = %d\n",
			  unslice_freq0, unslice_freq1,
			  ((int)unslice_freq1 - (int)unslice_freq0));

		igt_debug("REASONS: 1st = \"%s\", 2nd = \"%s\"\n", reason0, reason1);
	}

	/* Gen8+ has some 40bit A counters... */
	for (int j = 0; j < format.n_a40; j++) {
		uint64_t value0 = read_40bit_a_counter(oa_report0, fmt, j);
		uint64_t value1 = read_40bit_a_counter(oa_report1, fmt, j);
		uint64_t delta = get_40bit_a_delta(value0, value1);

		igt_debug("A%d: 1st = %"PRIu64", 2nd = %"PRIu64", delta = %"PRIu64"\n",
			  j, value0, value1, delta);
	}

	for (int j = 0; j < format.n_a64; j++) {
		uint64_t value0 = xehpsdv_read_64bit_a_counter(oa_report0, fmt, j);
		uint64_t value1 = xehpsdv_read_64bit_a_counter(oa_report1, fmt, j);
		uint64_t delta = value1 - value0;

		igt_debug("A_64%d: 1st = %"PRIu64", 2nd = %"PRIu64", delta = %"PRIu64"\n",
			  format.first_a + j, value0, value1, delta);
	}

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.a_off);
		uint32_t *a1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.a_off);
		int a_id = format.first_a + j;
		uint32_t delta = a1[j] - a0[j];

		igt_debug("A%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  a_id, a0[j], a1[j], delta);
	}

	for (int j = 0; j < format.n_b; j++) {
		uint32_t *b0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.b_off);
		uint32_t *b1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.b_off);
		uint32_t delta = b1[j] - b0[j];

		igt_debug("B%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  j, b0[j], b1[j], delta);
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.c_off);
		uint32_t *c1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.c_off);
		uint32_t delta = c1[j] - c0[j];

		igt_debug("C%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  j, c0[j], c1[j], delta);
	}
}

static bool
oau_supports_oa_type(int oa_type, const struct drm_xe_oa_unit *oau)
{
	switch (oa_type) {
	case DRM_XE_OA_FMT_TYPE_OAM:
	case DRM_XE_OA_FMT_TYPE_OAM_MPEC:
		return oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM ||
		       oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM_SAG;
	case DRM_XE_OA_FMT_TYPE_OAG:
	case DRM_XE_OA_FMT_TYPE_OAR:
	case DRM_XE_OA_FMT_TYPE_OAC:
	case DRM_XE_OA_FMT_TYPE_PEC:
		return oau->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAG;
	default:
		return false;
	}
}

/**
 * SUBTEST: oa-formats
 * Description: Test that supported OA formats work as expected
 */
static void test_oa_formats(const struct drm_xe_oa_unit *oau)
{
	for (int i = 0; i < XE_OA_FORMAT_MAX; i++) {
		struct oa_format format = get_oa_format(i);
		uint32_t oa_report0[format.size / 4];
		uint32_t oa_report1[format.size / 4];

		if (!format.name) /* sparse, indexed by ID */
			continue;

		if (!oau_supports_oa_type(format.oa_type, oau))
			continue;

		igt_debug("Checking OA format %s\n", format.name);

		open_and_read_2_oa_reports(i,
					   oa_exponent_default,
					   oa_report0,
					   oa_report1,
					   false, /* timer reports only */
					   oau);

		print_reports(oa_report0, oa_report1, i);
		sanity_check_reports(oa_report0, oa_report1, i);

		if (i == oa_unit_metric_set(oau)->perf_oa_format)
			pec_sanity_check_reports(oa_report0, oa_report1, oa_unit_metric_set(oau));
	}
}


enum load {
	LOW,
	HIGH
};

#define LOAD_HELPER_PAUSE_USEC 500

static struct load_helper {
	int devid;
	struct buf_ops *bops;
	uint32_t context_id;
	uint32_t vm;
	struct intel_bb *ibb;
	enum load load;
	bool exit;
	struct igt_helper_process igt_proc;
	struct intel_buf src, dst;
} lh = { 0, };

static void load_helper_signal_handler(int sig)
{
	if (sig == SIGUSR2)
		lh.load = lh.load == LOW ? HIGH : LOW;
	else
		lh.exit = true;
}

static void load_helper_set_load(enum load load)
{
	igt_assert(lh.igt_proc.running);

	if (lh.load == load)
		return;

	lh.load = load;
	kill(lh.igt_proc.pid, SIGUSR2);
}

static void load_helper_run(enum load load)
{
	if (!render_copy)
		return;

	/*
	 * FIXME fork helpers won't get cleaned up when started from within a
	 * subtest, so handle the case where it sticks around a bit too long.
	 */
	if (lh.igt_proc.running) {
		load_helper_set_load(load);
		return;
	}

	lh.load = load;

	igt_fork_helper(&lh.igt_proc) {
		signal(SIGUSR1, load_helper_signal_handler);
		signal(SIGUSR2, load_helper_signal_handler);

		while (!lh.exit) {
			render_copy(lh.ibb,
				    &lh.src, 0, 0, rc_width, rc_height,
				    &lh.dst, 0, 0);

			intel_bb_sync(lh.ibb);

			/* Lower the load by pausing after every submitted
			 * write. */
			if (lh.load == LOW)
				usleep(LOAD_HELPER_PAUSE_USEC);
		}
	}
}

static void load_helper_stop(void)
{
	if (!render_copy)
		return;

	kill(lh.igt_proc.pid, SIGUSR1);
	igt_assert(igt_wait_helper(&lh.igt_proc) == 0);
}

static void load_helper_init(void)
{
	if (!render_copy) {
		igt_info("Running test without render_copy\n");
		return;
	}

	lh.devid = intel_get_drm_devid(drm_fd);
	lh.bops = buf_ops_create(drm_fd);
	lh.vm = xe_vm_create(drm_fd, 0, 0);
	lh.context_id = xe_exec_queue_create(drm_fd, lh.vm, &xe_engine(drm_fd, 0)->instance, 0);
	igt_assert_neq(lh.context_id, 0xffffffff);

	lh.ibb = intel_bb_create_with_context(drm_fd, lh.context_id, lh.vm, NULL, BATCH_SZ);

	scratch_buf_init(lh.bops, &lh.dst, rc_width, rc_height, 0);
	scratch_buf_init(lh.bops, &lh.src, rc_width, rc_height, 0);
}

static void load_helper_fini(void)
{
	if (!render_copy)
		return;

	if (lh.igt_proc.running)
		load_helper_stop();

	intel_buf_close(lh.bops, &lh.src);
	intel_buf_close(lh.bops, &lh.dst);
	intel_bb_destroy(lh.ibb);
	xe_exec_queue_destroy(drm_fd, lh.context_id);
	xe_vm_destroy(drm_fd, lh.vm);
	buf_ops_destroy(lh.bops);
}

static bool expected_report_timing_delta(uint32_t delta, uint32_t expected_delta)
{
	return delta <= expected_delta;
}

/**
 * SUBTEST: oa-exponents
 * Description: Test that oa exponent values behave as expected
 */
static void test_oa_exponents(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;

	load_helper_init();
	load_helper_run(HIGH);

	/* It's asking a lot to sample with a 160 nanosecond period and the
	 * test can fail due to buffer overflows if it wasn't possible to
	 * keep up, so we don't start from an exponent of zero...
	 */
	for (int exponent = min_oa_exponent; exponent < max_oa_exponent; exponent++) {
		uint64_t properties[] = {
			DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

			/* Include OA reports in samples */
			DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
			DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
			DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, exponent,
		};
		struct intel_xe_oa_open_prop param = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};
		uint64_t expected_timestamp_delta = 2ULL << exponent;
		size_t format_size = get_oa_format(fmt).size;
		int max_reports = default_oa_buffer_size / format_size;
		int buf_size = format_size * max_reports * 1.5;
		uint8_t *buf = calloc(1, buf_size);
		int ret, n_timer_reports = 0;
		uint32_t matches = 0;
#define NUM_TIMER_REPORTS 30
		uint32_t *reports = malloc(NUM_TIMER_REPORTS * format_size);
		uint32_t *timer_reports = reports;
		void *this, *prev;

		igt_debug("testing OA exponent %d,"
			  " expected ts delta = %"PRIu64" (%"PRIu64"ns/%.2fus/%.2fms)\n",
			  exponent, expected_timestamp_delta,
			  oa_exponent_to_ns(exponent),
			  oa_exponent_to_ns(exponent) / 1000.0,
			  oa_exponent_to_ns(exponent) / (1000.0 * 1000.0));

		stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

		while (n_timer_reports < NUM_TIMER_REPORTS) {
			u32 oa_status = 0;

			while ((ret = read(stream_fd, buf, buf_size)) < 0 && errno == EINTR)
				;
			if (ret < 0 && errno == EIO) {
				oa_status = get_stream_status(stream_fd);
				continue;
			}

			/* igt_debug(" > read %i bytes\n", ret); */
			/* We should never have no data. */
			igt_assert_lt(0, ret);

			for (int offset = 0;
			     offset < ret && n_timer_reports < NUM_TIMER_REPORTS;
			     offset += format_size) {
				uint32_t *report = (void *)(buf + offset);

				if (oa_status & DRM_XE_OASTATUS_BUFFER_OVERFLOW) {
					igt_assert(!"reached");
					break;
				}

				if (oa_status & DRM_XE_OASTATUS_REPORT_LOST)
					igt_debug("report loss\n");

				if (!oa_report_is_periodic(report))
					continue;

				memcpy(timer_reports, report, format_size);
				n_timer_reports++;
				timer_reports += (format_size / 4);
			}
		}

		__perf_close(stream_fd);

		this = reports + format_size / 4;
		prev = reports;

		igt_debug("report%04i ts=%"PRIx64" hw_id=0x%08x\n", 0,
			  oa_timestamp(prev, fmt),
			  oa_report_get_ctx_id(prev));
		for (int i = 1; i < n_timer_reports; i++) {
			uint64_t delta = oa_timestamp_delta(this, prev, fmt);

			igt_debug("report%04i ts=%"PRIx64" hw_id=0x%08x delta=%"PRIu64" %s\n", i,
				  oa_timestamp(this, fmt),
				  oa_report_get_ctx_id(this),
				  delta, expected_report_timing_delta(delta,
								      expected_timestamp_delta) ? "" : "******");

			matches += expected_report_timing_delta(delta,expected_timestamp_delta);

			this += format_size;
			prev += format_size;
		}

		igt_debug("matches=%u/%u\n", matches, n_timer_reports - 1);

		/*
		 * Expect half the reports to match the timing
		 * expectation. The results are quite erratic because
		 * the condition under which the HW reaches
		 * expectations depends on memory controller pressure
		 * etc...
		 */
		igt_assert_lte(n_timer_reports / 2, matches);

		free(reports);
	}

	load_helper_stop();
	load_helper_fini();
}

/**
 * SUBTEST: invalid-oa-exponent
 * Description: Test that invalid exponent values are rejected
 */
/* The OA exponent selects a timestamp counter bit to trigger reports on.
 *
 * With a 64bit timestamp and least significant bit approx == 80ns then the MSB
 * equates to > 40 thousand years and isn't exposed via the xe oa interface.
 *
 * The max exponent exposed is expected to be 31, which is still a fairly
 * ridiculous period (>5min) but is the maximum exponent where it's still
 * possible to use periodic sampling as a means for tracking the overflow of
 * 32bit OA report timestamps.
 */
static void test_invalid_oa_exponent(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, 31, /* maximum exponent expected
						       to be accepted */
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(drm_fd, &param, false);

	__perf_close(stream_fd);

	for (int i = 32; i < 65; i++) {
		properties[7] = i;
		intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EINVAL);
	}
}

static int64_t
get_time(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/**
 * SUBTEST: blocking
 * Description: Test blocking reads
 */
/* Note: The interface doesn't currently provide strict guarantees or control
 * over the upper bound for how long it might take for a POLLIN event after
 * some OA report is written by the OA unit.
 *
 * The plan is to add a property later that gives some control over the maximum
 * latency, but for now we expect it is tuned for a fairly low latency
 * suitable for applications wanting to provide live feedback for captured
 * metrics.
 *
 * At the time of writing this test the driver was using a fixed 200Hz hrtimer
 * regardless of the OA sampling exponent.
 *
 * There is no lower bound since a stream configured for periodic sampling may
 * still contain other automatically triggered reports.
 *
 * What we try and check for here is that blocking reads don't return EAGAIN
 * and that we aren't spending any significant time burning the cpu in
 * kernelspace.
 */
static void test_blocking(uint64_t requested_oa_period,
			  bool set_kernel_hrtimer,
			  uint64_t kernel_hrtimer,
			  const struct drm_xe_oa_unit *oau)
{
	int oa_exponent = max_oa_exponent_for_period_lte(requested_oa_period);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t props[XE_OA_MAX_SET_PROPERTIES * 2];
	uint64_t *idx = props;
	struct intel_xe_oa_open_prop param;
	uint8_t buf[1024 * 1024];
	struct tms start_times;
	struct tms end_times;
	int64_t user_ns, kernel_ns;
	int64_t tick_ns = 1000000000 / sysconf(_SC_CLK_TCK);
	int64_t test_duration_ns = tick_ns * 100;
	int max_iterations = (test_duration_ns / oa_period) + 2;
	int n_extra_iterations = 0;
	int perf_fd;

	/* It's a bit tricky to put a lower limit here, but we expect a
	 * relatively low latency for seeing reports, while we don't currently
	 * give any control over this in the api.
	 *
	 * We assume a maximum latency of 6 millisecond to deliver a POLLIN and
	 * read() after a new sample is written (46ms per iteration) considering
	 * the knowledge that that the driver uses a 200Hz hrtimer (5ms period)
	 * to check for data and giving some time to read().
	 */
	int min_iterations = (test_duration_ns / (oa_period + kernel_hrtimer + kernel_hrtimer / 5));
	int64_t start, end;
	int n = 0;
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	size_t format_size = get_oa_format(test_set->perf_oa_format).size;

	ADD_PROPS(props, idx, SAMPLE_OA, true);
	ADD_PROPS(props, idx, OA_METRIC_SET, test_set->perf_oa_metrics_set);
	ADD_PROPS(props, idx, OA_FORMAT, __ff(test_set->perf_oa_format));
	ADD_PROPS(props, idx, OA_PERIOD_EXPONENT, oa_exponent);
	ADD_PROPS(props, idx, OA_DISABLED, true);
	ADD_PROPS(props, idx, OA_UNIT_ID, oau->oa_unit_id);

	param.num_properties = (idx - props) / 2;
	param.properties_ptr = to_user_pointer(props);

	perf_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
        set_fd_flags(perf_fd, O_CLOEXEC);

	times(&start_times);

	igt_debug("tick length = %dns, test duration = %"PRIu64"ns, min iter. = %d,"
		  " estimated max iter. = %d, oa_period = %s\n",
		  (int)tick_ns, test_duration_ns,
		  min_iterations, max_iterations,
		  pretty_print_oa_period(oa_period));

	/* In the loop we perform blocking polls while the HW is sampling at
	 * ~25Hz, with the expectation that we spend most of our time blocked
	 * in the kernel, and shouldn't be burning cpu cycles in the kernel in
	 * association with this process (verified by looking at stime before
	 * and after loop).
	 *
	 * We're looking to assert that less than 1% of the test duration is
	 * spent in the kernel dealing with polling and read()ing.
	 *
	 * The test runs for a relatively long time considering the very low
	 * resolution of stime in ticks of typically 10 milliseconds. Since we
	 * don't know the fractional part of tick values we read from userspace
	 * so our minimum threshold needs to be >= one tick since any
	 * measurement might really be +- tick_ns (assuming we effectively get
	 * floor(real_stime)).
	 *
	 * We Loop for 1000 x tick_ns so one tick corresponds to 0.1%
	 *
	 * Also enable the stream just before poll/read to minimize
	 * the error delta.
	 */
	start = get_time();
	do_ioctl(perf_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < test_duration_ns; /* nop */) {
		bool timer_report_read = false;
		bool non_timer_report_read = false;
		int ret;

		while ((ret = read(perf_fd, buf, sizeof(buf))) < 0 &&
		       (errno == EINTR || errno == EIO))
			;
		igt_assert_lt(0, ret);

		for (int offset = 0; offset < ret; offset += format_size) {
			uint32_t *report = (void *)(buf + offset);

			if (oa_report_is_periodic(report))
				timer_report_read = true;
			else
				non_timer_report_read = true;
		}

		if (non_timer_report_read && !timer_report_read)
			n_extra_iterations++;

		n++;
	}

	times(&end_times);

	/* Using nanosecond units is fairly silly here, given the tick in-
	 * precision - ah well, it's consistent with the get_time() units.
	 */
	user_ns = (end_times.tms_utime - start_times.tms_utime) * tick_ns;
	kernel_ns = (end_times.tms_stime - start_times.tms_stime) * tick_ns;

	igt_debug("%d blocking reads during test with %"PRIu64" Hz OA sampling (expect no more than %d)\n",
		  n, NSEC_PER_SEC / oa_period, max_iterations);
	igt_debug("%d extra iterations seen, not related to periodic sampling (e.g. context switches)\n",
		  n_extra_iterations);
	igt_debug("time in userspace = %"PRIu64"ns (+-%dns) (start utime = %d, end = %d)\n",
		  user_ns, (int)tick_ns,
		  (int)start_times.tms_utime, (int)end_times.tms_utime);
	igt_debug("time in kernelspace = %"PRIu64"ns (+-%dns) (start stime = %d, end = %d)\n",
		  kernel_ns, (int)tick_ns,
		  (int)start_times.tms_stime, (int)end_times.tms_stime);

	/* With completely broken blocking (but also not returning an error) we
	 * could end up with an open loop,
	 */
	igt_assert_lte(n, (max_iterations + n_extra_iterations));

	/* Make sure the driver is reporting new samples with a reasonably
	 * low latency...
	 */
	igt_assert_lt((min_iterations + n_extra_iterations), n);

	if (!set_kernel_hrtimer)
		igt_assert(kernel_ns <= (test_duration_ns / 100ull));

	__perf_close(perf_fd);
}

/**
 * SUBTEST: polling
 * Description: Test polled reads
 */
static void test_polling(uint64_t requested_oa_period,
			 bool set_kernel_hrtimer,
			 uint64_t kernel_hrtimer,
			 const struct drm_xe_oa_unit *oau)
{
	int oa_exponent = max_oa_exponent_for_period_lte(requested_oa_period);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t props[XE_OA_MAX_SET_PROPERTIES * 2];
	uint64_t *idx = props;
	struct intel_xe_oa_open_prop param;
	uint8_t buf[1024 * 1024];
	struct tms start_times;
	struct tms end_times;
	int64_t user_ns, kernel_ns;
	int64_t tick_ns = 1000000000 / sysconf(_SC_CLK_TCK);
	int64_t test_duration_ns = tick_ns * 100;

	int max_iterations = (test_duration_ns / oa_period) + 2;
	int n_extra_iterations = 0;

	/* It's a bit tricky to put a lower limit here, but we expect a
	 * relatively low latency for seeing reports.
	 *
	 * We assume a maximum latency of kernel_hrtimer + some margin
	 * to deliver a POLLIN and read() after a new sample is
	 * written (40ms + hrtimer + margin per iteration) considering
	 * the knowledge that that the driver uses a 200Hz hrtimer
	 * (5ms period) to check for data and giving some time to
	 * read().
	 */
	int min_iterations = (test_duration_ns / (oa_period + (kernel_hrtimer + kernel_hrtimer / 5)));
	int64_t start, end;
	int n = 0;
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	size_t format_size = get_oa_format(test_set->perf_oa_format).size;

	ADD_PROPS(props, idx, SAMPLE_OA, true);
	ADD_PROPS(props, idx, OA_METRIC_SET, test_set->perf_oa_metrics_set);
	ADD_PROPS(props, idx, OA_FORMAT, __ff(test_set->perf_oa_format));
	ADD_PROPS(props, idx, OA_PERIOD_EXPONENT, oa_exponent);
	ADD_PROPS(props, idx, OA_DISABLED, true);
	ADD_PROPS(props, idx, OA_UNIT_ID, oau->oa_unit_id);

	param.num_properties = (idx - props) / 2;
	param.properties_ptr = to_user_pointer(props);

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
	set_fd_flags(stream_fd, O_CLOEXEC | O_NONBLOCK);

	times(&start_times);

	igt_debug("tick length = %dns, oa period = %s, "
		  "test duration = %"PRIu64"ns, min iter. = %d, max iter. = %d\n",
		  (int)tick_ns, pretty_print_oa_period(oa_period), test_duration_ns,
		  min_iterations, max_iterations);

	/* In the loop we perform blocking polls while the HW is sampling at
	 * ~25Hz, with the expectation that we spend most of our time blocked
	 * in the kernel, and shouldn't be burning cpu cycles in the kernel in
	 * association with this process (verified by looking at stime before
	 * and after loop).
	 *
	 * We're looking to assert that less than 1% of the test duration is
	 * spent in the kernel dealing with polling and read()ing.
	 *
	 * The test runs for a relatively long time considering the very low
	 * resolution of stime in ticks of typically 10 milliseconds. Since we
	 * don't know the fractional part of tick values we read from userspace
	 * so our minimum threshold needs to be >= one tick since any
	 * measurement might really be +- tick_ns (assuming we effectively get
	 * floor(real_stime)).
	 *
	 * We Loop for 1000 x tick_ns so one tick corresponds to 0.1%
	 *
	 * Also enable the stream just before poll/read to minimize
	 * the error delta.
	 */
	start = get_time();
	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < test_duration_ns; /* nop */) {
		struct pollfd pollfd = { .fd = stream_fd, .events = POLLIN };
		bool timer_report_read = false;
		bool non_timer_report_read = false;
		int ret;

		while ((ret = poll(&pollfd, 1, -1)) < 0 && errno == EINTR)
			;
		igt_assert_eq(ret, 1);
		igt_assert(pollfd.revents & POLLIN);

		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       (errno == EINTR || errno == EIO))
			;

		/* Don't expect to see EAGAIN if we've had a POLLIN event
		 *
		 * XXX: actually this is technically overly strict since we do
		 * knowingly allow false positive POLLIN events. At least in
		 * the future when supporting context filtering of metrics for
		 * Gen8+ handled in the kernel then POLLIN events may be
		 * delivered when we know there are pending reports to process
		 * but before we've done any filtering to know for certain that
		 * any reports are destined to be copied to userspace.
		 *
		 * Still, for now it's a reasonable sanity check.
		 */
		if (ret < 0)
			igt_debug("Unexpected error when reading after poll = %d\n", errno);
		igt_assert_neq(ret, -1);

		/* For Haswell reports don't contain a well defined reason
		 * field we so assume all reports to be 'periodic'. For gen8+
		 * we want to to consider that the HW automatically writes some
		 * non periodic reports (e.g. on context switch) which might
		 * lead to more successful read()s than expected due to
		 * periodic sampling and we don't want these extra reads to
		 * cause the test to fail...
		 */
		for (int offset = 0; offset < ret; offset += format_size) {
			uint32_t *report = (void *)(buf + offset);

			if (oa_report_is_periodic(report))
				timer_report_read = true;
			else
				non_timer_report_read = true;
		}

		if (non_timer_report_read && !timer_report_read)
			n_extra_iterations++;

		/* At this point, after consuming pending reports (and hoping
		 * the scheduler hasn't stopped us for too long) we now expect
		 * EAGAIN on read. While this works most of the times, there are
		 * some rare failures when the OA period passed to this test is
		 * very small (say 500 us) and that results in some valid
		 * reports here. To weed out those rare occurences we assert
		 * only if the OA period is >= 40 ms because 40 ms has withstood
		 * the test of time on most platforms (ref: subtest: polling).
		 */
		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       (errno == EINTR || errno == EIO))
			;

		if (requested_oa_period >= 40000000) {
			igt_assert_eq(ret, -1);
			igt_assert_eq(errno, EAGAIN);
		}

		n++;
	}

	times(&end_times);

	/* Using nanosecond units is fairly silly here, given the tick in-
	 * precision - ah well, it's consistent with the get_time() units.
	 */
	user_ns = (end_times.tms_utime - start_times.tms_utime) * tick_ns;
	kernel_ns = (end_times.tms_stime - start_times.tms_stime) * tick_ns;

	igt_debug("%d non-blocking reads during test with %"PRIu64" Hz OA sampling (expect no more than %d)\n",
		  n, NSEC_PER_SEC / oa_period, max_iterations);
	igt_debug("%d extra iterations seen, not related to periodic sampling (e.g. context switches)\n",
		  n_extra_iterations);
	igt_debug("time in userspace = %"PRIu64"ns (+-%dns) (start utime = %d, end = %d)\n",
		  user_ns, (int)tick_ns,
		  (int)start_times.tms_utime, (int)end_times.tms_utime);
	igt_debug("time in kernelspace = %"PRIu64"ns (+-%dns) (start stime = %d, end = %d)\n",
		  kernel_ns, (int)tick_ns,
		  (int)start_times.tms_stime, (int)end_times.tms_stime);

	/* With completely broken blocking while polling (but still somehow
	 * reporting a POLLIN event) we could end up with an open loop.
	 */
	igt_assert_lte(n, (max_iterations + n_extra_iterations));

	/* Make sure the driver is reporting new samples with a reasonably
	 * low latency...
	 */
	igt_assert_lt((min_iterations + n_extra_iterations), n);

	if (!set_kernel_hrtimer)
		igt_assert(kernel_ns <= (test_duration_ns / 100ull));

	__perf_close(stream_fd);
}

/**
 * SUBTEST: polling-small-buf
 * Description: Test polled read with buffer size smaller than available data
 */
static void test_polling_small_buf(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_WAIT_NUM_REPORTS, 5,
		DRM_XE_OA_PROPERTY_OA_DISABLED, true,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	struct pollfd pollfd;
	uint8_t buf[10];
	int ret;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
	set_fd_flags(stream_fd, O_CLOEXEC | O_NONBLOCK);

	/* Kickstart the capture */
	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);

	/*
	 * Wait for number of reports specified in
	 * DRM_XE_OA_PROPERTY_WAIT_NUM_REPORTS
	 */
	pollfd.fd = stream_fd;
	pollfd.events = POLLIN;
	poll(&pollfd, 1, -1);
	igt_assert(pollfd.revents & POLLIN);

	/* Just read one report and expect ENOSPC */
	errno = 0;
	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	get_stream_status(stream_fd);
	igt_assert_eq(errno, ENOSPC);

	/* Poll with 0 timeout and expect POLLIN flag to be set */
	poll(&pollfd, 1, 0);
	igt_assert(pollfd.revents & POLLIN);

	__perf_close(stream_fd);
}

static int
num_valid_reports_captured(struct intel_xe_oa_open_prop *param,
			   int64_t *duration_ns, int fmt)
{
	uint8_t buf[1024 * 1024];
	int64_t start, end;
	int num_reports = 0;
	size_t format_size = get_oa_format(fmt).size;

	igt_debug("Expected duration = %"PRId64"\n", *duration_ns);

	stream_fd = __perf_open(drm_fd, param, true);

	start = get_time();
	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < *duration_ns; /* nop */) {
		int ret;

		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       (errno == EINTR || errno == EIO))
			;

		igt_assert_lt(0, ret);

		for (int offset = 0; offset < ret; offset += format_size) {
			uint32_t *report = (void *)(buf + offset);

			if (report_reason(report) & OAREPORT_REASON_TIMER)
				num_reports++;
		}
	}
	__perf_close(stream_fd);

	*duration_ns = end - start;

	igt_debug("Actual duration = %"PRIu64"\n", *duration_ns);

	return num_reports;
}

/**
 * SUBTEST: oa-tlb-invalidate
 * Description: Open OA stream twice to verify OA TLB invalidation
 */
static void
test_oa_tlb_invalidate(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_DISABLED, true,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	int num_reports1, num_reports2, num_expected_reports;
	int64_t duration;

	/* Capture reports for 5 seconds twice and then make sure you get around
	 * the same number of reports. In the case of failure, the number of
	 * reports will vary largely since the beginning of the OA buffer
	 * will have invalid entries.
	 */
	duration = 5LL * NSEC_PER_SEC;
	num_reports1 = num_valid_reports_captured(&param, &duration, test_set->perf_oa_format);
	num_expected_reports = duration / oa_exponent_to_ns(oa_exponent_default);
	igt_debug("expected num reports = %d\n", num_expected_reports);
	igt_debug("actual num reports = %d\n", num_reports1);
	igt_assert(num_reports1 > 0.95 * num_expected_reports);

	duration = 5LL * NSEC_PER_SEC;
	num_reports2 = num_valid_reports_captured(&param, &duration, test_set->perf_oa_format);
	num_expected_reports = duration / oa_exponent_to_ns(oa_exponent_default);
	igt_debug("expected num reports = %d\n", num_expected_reports);
	igt_debug("actual num reports = %d\n", num_reports2);
	igt_assert(num_reports2 > 0.95 * num_expected_reports);
}

static void
wait_for_oa_buffer_overflow(int fd, int poll_period_us)
{
	char buf;

	while (-1 == read(fd, &buf, 0)) {
		if (errno == EIO &&
		    get_stream_status(fd) & DRM_XE_OASTATUS_BUFFER_OVERFLOW)
			return;

		usleep(poll_period_us);
	}
}

/**
 * SUBTEST: buffer-fill
 * Description: Test filling and overflow of OA buffer
 */
static void
test_buffer_fill(const struct drm_xe_oa_unit *oau)
{
	/* ~5 micro second period */
	int oa_exponent = max_oa_exponent_for_period_lte(5000);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent,
		DRM_XE_OA_PROPERTY_OA_BUFFER_SIZE, buffer_fill_size,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	char *buf = malloc(1024);
	bool overflow_seen;
	u32 oa_status;

	igt_debug("oa_period %s\n", pretty_print_oa_period(oa_period));
	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
        set_fd_flags(stream_fd, O_CLOEXEC);

	wait_for_oa_buffer_overflow(stream_fd, 100);

	/* Make sure the buffer overflow is cleared */
	read(stream_fd, buf, 0);
	oa_status = get_stream_status(stream_fd);
	overflow_seen = oa_status & DRM_XE_OASTATUS_BUFFER_OVERFLOW;
	igt_assert_eq(overflow_seen, 0);

	__perf_close(stream_fd);
}

/**
 * SUBTEST: non-zero-reason
 * Description: Test reason field is non-zero. Can also check OA buffer wraparound issues
 */
static void
test_non_zero_reason(const struct drm_xe_oa_unit *oau, size_t oa_buffer_size)
{
	/* ~20 micro second period */
	int oa_exponent = max_oa_exponent_for_period_lte(20000);
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;
	size_t report_size = get_oa_format(fmt).size;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent,
		DRM_XE_OA_PROPERTY_OA_BUFFER_SIZE, oa_buffer_size ?: buffer_fill_size
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t buf_size = 3 * (oa_buffer_size ?: buffer_fill_size);
	uint8_t *buf = malloc(buf_size);
	uint32_t total_len = 0;
	const uint32_t *last_report;
	int len, check_idx;
	u32 oa_status;

	igt_assert(buf);

	igt_debug("Ready to read about %u bytes\n", buf_size);

	load_helper_init();
	load_helper_run(HIGH);

	if (!oa_buffer_size)
		param.num_properties = param.num_properties - 1;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
        set_fd_flags(stream_fd, O_CLOEXEC);

	while (total_len < buf_size &&
	       ((len = read(stream_fd, &buf[total_len], buf_size - total_len)) > 0 ||
		(len == -1 && (errno == EINTR || errno == EIO)))) {
		/* Assert only for default OA buffer size */
		if (len < 0 && errno == EIO && !oa_buffer_size) {
			oa_status = get_stream_status(stream_fd);
			igt_assert(!(oa_status & DRM_XE_OASTATUS_BUFFER_OVERFLOW));
		}
		if (len > 0)
			total_len += len;
	}

	__perf_close(stream_fd);

	load_helper_stop();
	load_helper_fini();

	igt_debug("Got %u bytes\n", total_len);

	check_idx = random() % (total_len / report_size);
	check_idx = check_idx ?: 1;

	last_report = NULL;
	for (uint32_t offset = 0; offset < total_len; offset += report_size) {
		const uint32_t *report = (void *) (buf + offset);
		uint32_t reason = (report[0] >> OAREPORT_REASON_SHIFT) & OAREPORT_REASON_MASK;

		igt_assert_neq(reason, 0);

		/*
		 * Only check for default OA buffer size, since non-default
		 * sizes can drop reports due to buffer overrun. Also, only
		 * check one random report to reduce test execution time.
		 */
		if (!oa_buffer_size && last_report && (offset / report_size == check_idx)) {
			sanity_check_reports(last_report, report, fmt);
			pec_sanity_check_reports(last_report, report, oa_unit_metric_set(oau));
		}

		last_report = report;
	}

	free(buf);
}

/**
 * SUBTEST: enable-disable
 * Description: Test that OA stream enable/disable works as expected
 */
static void
test_enable_disable(const struct drm_xe_oa_unit *oau)
{
	uint32_t num_reports = 5;
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_DISABLED, true,
		DRM_XE_OA_PROPERTY_WAIT_NUM_REPORTS, num_reports,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t format_size = get_oa_format(fmt).size;
	uint8_t buf[num_reports * format_size];
	struct pollfd pollfd;
	int ret;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
	set_fd_flags(stream_fd, O_CLOEXEC | O_NONBLOCK);

	errno = 0;
	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	get_stream_status(stream_fd);
	igt_assert_eq(errno, EINVAL);

	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);

	/*
	 * Wait for number of reports specified in
	 * DRM_XE_OA_PROPERTY_WAIT_NUM_REPORTS
	 */
	pollfd.fd = stream_fd;
	pollfd.events = POLLIN;
	poll(&pollfd, 1, -1);
	igt_assert(pollfd.revents & POLLIN);

	/* Ensure num_reports can be read */
	while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 && errno == EINTR)
		;
	get_stream_status(stream_fd);
	igt_assert_eq(ret, sizeof(buf));

	__perf_close(stream_fd);
}

/**
 * SUBTEST: short-reads
 * Description: Test behavior for short reads
 */
static void
test_short_reads(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t record_size = get_oa_format(default_test_set->perf_oa_format).size;
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	int zero_fd = open("/dev/zero", O_RDWR|O_CLOEXEC);
	uint8_t *pages = mmap(NULL, page_size * 2,
			      PROT_READ|PROT_WRITE, MAP_PRIVATE, zero_fd, 0);
	u8 *header;
	int ret, errnum;
	u32 oa_status;

	igt_assert_neq(zero_fd, -1);
	close(zero_fd);
	zero_fd = -1;

	igt_assert(pages);

	ret = mprotect(pages + page_size, page_size, PROT_NONE);
	igt_assert_eq(ret, 0);

	stream_fd = __perf_open(drm_fd, &param, false);

	nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 5000000 }, NULL);

	/* At this point there should be lots of pending reports to read */

	/* A read that can return at least one record should result in a short
	 * read not an EFAULT if the buffer is smaller than the requested read
	 * size...
	 *
	 * Expect to see a sample record here, but at least skip over any
	 * _RECORD_LOST notifications.
	 */
	do {
		header = (void *)(pages + page_size - record_size);
		oa_status = 0;
		ret = read(stream_fd, header, page_size);
		if (ret < 0 && errno == EIO)
			oa_status = get_stream_status(stream_fd);

	} while (oa_status & DRM_XE_OASTATUS_REPORT_LOST);

	igt_assert_eq(ret, record_size);

	/* A read that can't return a single record because it would result
	 * in a fault on buffer overrun should result in an EFAULT error...
	 *
	 * Make sure to weed out all report lost errors before verifying EFAULT.
	 */
	header = (void *)(pages + page_size - 16);
	do {
		oa_status = 0;
		ret = read(stream_fd, header, page_size);
		errnum = errno;
		if (ret < 0 && errno == EIO)
			oa_status = get_stream_status(stream_fd);
		errno = errnum;
	} while (oa_status & DRM_XE_OASTATUS_REPORT_LOST);

	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EFAULT);

	/* A read that can't return a single record because the buffer is too
	 * small should result in an ENOSPC error..
	 *
	 * Again, skip over _RECORD_LOST records (smaller than record_size/2)
	 */
	do {
		header = (void *)(pages + page_size - record_size / 2);
		oa_status = 0;
		ret = read(stream_fd, header, record_size / 2);
		errnum = errno;
		if (ret < 0 && errno == EIO)
			oa_status = get_stream_status(stream_fd);
		errno = errnum;
	} while (oa_status & DRM_XE_OASTATUS_REPORT_LOST);

	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, ENOSPC);

	__perf_close(stream_fd);

	munmap(pages, page_size * 2);
}

/**
 * SUBTEST: non-sampling-read-error
 * Description: Test that a stream without periodic sampling (no exponent) cannot be read
 */
static void
test_non_sampling_read_error(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),

		/* XXX: no sampling exponent */
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	int ret;
	uint8_t buf[1024];

	stream_fd = __perf_open(drm_fd, &param, false);
        set_fd_flags(stream_fd, O_CLOEXEC);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	get_stream_status(stream_fd);
	igt_assert_eq(errno, EINVAL);

	__perf_close(stream_fd);
}

/**
 * SUBTEST: disabled-read-error
 * Description: Test that attempts to read from a stream while it is disable
 *		will return EINVAL instead of blocking indefinitely
 */
static void
test_disabled_read_error(void)
{
	int oa_exponent = 5; /* 5 micro seconds */
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent,
		DRM_XE_OA_PROPERTY_OA_DISABLED, true,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t oa_report0[64];
	uint32_t oa_report1[64];
	uint32_t buf[128] = { 0 };
	int ret;

	stream_fd = __perf_open(drm_fd, &param, false);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	get_stream_status(stream_fd);
	igt_assert_eq(errno, EINVAL);

	__perf_close(stream_fd);

	properties[ARRAY_SIZE(properties) - 1] = false; /* Set DISABLED to false */
	stream_fd = __perf_open(drm_fd, &param, false);
        set_fd_flags(stream_fd, O_CLOEXEC);

	read_2_oa_reports(default_test_set->perf_oa_format,
			  oa_exponent,
			  oa_report0,
			  oa_report1,
			  false); /* not just timer reports */

	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_DISABLE, 0);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	get_stream_status(stream_fd);
	igt_assert_eq(errno, EINVAL);

	do_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_ENABLE, 0);

	read_2_oa_reports(default_test_set->perf_oa_format,
			  oa_exponent,
			  oa_report0,
			  oa_report1,
			  false); /* not just timer reports */

	__perf_close(stream_fd);
}

/**
 * SUBTEST: mi-rpc
 * Description: Test OAR/OAC using MI_REPORT_PERF_COUNT
 */
static void
test_mi_rpc(const struct drm_xe_oa_unit *oau)

{
	const struct drm_xe_engine_class_instance *hwe = oa_unit_engine(oau);
	uint64_t fmt = ((IS_DG2(devid) || IS_METEORLAKE(devid)) &&
			hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) ?
		XE_OAC_FORMAT_A24u64_B8_C8 : oar_unit_default_format();
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

		/* On Gen12, MI RPC uses OAR. OAR is configured only for the
		 * render context that wants to measure the performance. Hence a
		 * context must be specified in the gen12 MI RPC when compared
		 * to previous gens.
		 *
		 * Have a random value here for the context id, but initialize
		 * it once you figure out the context ID for the work to be
		 * measured
		 */
		DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID, UINT64_MAX,

		/* OA unit configuration:
		 * DRM_XE_OA_PROPERTY_SAMPLE_OA is no longer required for Gen12
		 * because the OAR unit increments counters only for the
		 * relevant context. No other parameters are needed since we do
		 * not rely on the OA buffer anymore to normalize the counter
		 * values.
		 */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_ENGINE_INSTANCE, hwe->engine_instance,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	struct buf_ops *bops;
	struct intel_bb *ibb;
	struct intel_buf *buf;
#define INVALID_CTX_ID 0xffffffff
	uint32_t ctx_id = INVALID_CTX_ID;
	uint32_t vm = 0;
	uint32_t *report32;
	size_t format_size_32;
	struct oa_format format = get_oa_format(fmt);

	/* Ensure observation_paranoid is set to 1 by default */
	write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

	bops = buf_ops_create(drm_fd);
	vm = xe_vm_create(drm_fd, 0, 0);
	ctx_id = xe_exec_queue_create_deconst(drm_fd, vm, hwe, 0);
	igt_assert_neq(ctx_id, INVALID_CTX_ID);
	properties[3] = ctx_id;

	ibb = intel_bb_create_with_context(drm_fd, ctx_id, vm, NULL, BATCH_SZ);
	buf = intel_buf_create(bops, 4096, 1, 8, 64,
			       I915_TILING_NONE, I915_COMPRESSION_NONE);

	buf_map(drm_fd, buf, true);
	memset(buf->ptr, 0x80, 4096);
	intel_buf_unmap(buf);

	stream_fd = __perf_open(drm_fd, &param, false);
        set_fd_flags(stream_fd, O_CLOEXEC);

#define REPORT_ID 0xdeadbeef
#define REPORT_OFFSET 0
	emit_report_perf_count(ibb,
			       buf,
			       REPORT_OFFSET,
			       REPORT_ID);
	intel_bb_flush_render(ibb);
	intel_bb_sync(ibb);

	buf_map(drm_fd, buf, false);
	report32 = buf->ptr;
	format_size_32 = format.size >> 2;
	dump_report(report32, format_size_32, "mi-rpc");

	/* Sanity check reports
	 * reportX_32[0]: report id passed with mi-rpc
	 * reportX_32[1]: timestamp. NOTE: wraps around in ~6 minutes.
	 *
	 * reportX_32[format.b_off]: check if the entire report was filled.
	 * B0 counter falls in the last 64 bytes of this report format.
	 * Since reports are filled in 64 byte blocks, we should be able to
	 * assure that the report was filled by checking the B0 counter. B0
	 * counter is defined to be zero, so we can easily validate it.
	 *
	 * reportX_32[format_size_32]: outside report, make sure only the report
	 * size amount of data was written.
	 */
	igt_assert_eq(report32[0], REPORT_ID);
	igt_assert(oa_timestamp(report32, test_set->perf_oa_format));
	igt_assert_neq(report32[format.b_off >> 2], 0x80808080);
	igt_assert_eq(report32[format_size_32], 0x80808080);

	intel_buf_unmap(buf);
	intel_buf_destroy(buf);
	intel_bb_destroy(ibb);
	xe_exec_queue_destroy(drm_fd, ctx_id);
	xe_vm_destroy(drm_fd, vm);
	buf_ops_destroy(bops);
	__perf_close(stream_fd);
}

static void
emit_stall_timestamp_and_rpc(struct intel_bb *ibb,
			     struct intel_buf *dst,
			     int timestamp_offset,
			     int report_dst_offset,
			     uint32_t report_id)
{
	uint32_t pipe_ctl_flags = (PIPE_CONTROL_CS_STALL |
				   PIPE_CONTROL_RENDER_TARGET_FLUSH |
				   PIPE_CONTROL_WRITE_TIMESTAMP);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL(6));
	intel_bb_out(ibb, pipe_ctl_flags);
	intel_bb_emit_reloc(ibb, dst->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			    timestamp_offset, dst->addr.offset);
	intel_bb_out(ibb, 0); /* imm lower */
	intel_bb_out(ibb, 0); /* imm upper */

	emit_report_perf_count(ibb, dst, report_dst_offset, report_id);
}

static void single_ctx_helper(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	struct drm_xe_engine_class_instance *hwe =
		&xe_find_engine_by_class(drm_fd, DRM_XE_ENGINE_CLASS_RENDER)->instance;
	uint64_t fmt = oar_unit_default_format();
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

		/* Have a random value here for the context id, but initialize
		 * it once you figure out the context ID for the work to be
		 * measured
		 */
		DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID, UINT64_MAX,

		/* OA unit configuration:
		 * DRM_XE_OA_PROPERTY_SAMPLE_OA is no longer required for Gen12
		 * because the OAR unit increments counters only for the
		 * relevant context. No other parameters are needed since we do
		 * not rely on the OA buffer anymore to normalize the counter
		 * values.
		 */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_ENGINE_INSTANCE, hwe->engine_instance,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	struct buf_ops *bops;
	struct intel_bb *ibb0, *ibb1;
	struct intel_buf src[3], dst[3], *dst_buf;
	uint32_t context0_id, context1_id, vm = 0;
	uint32_t *report0_32, *report1_32, *report2_32, *report3_32;
	uint64_t timestamp0_64, timestamp1_64;
	uint64_t delta_ts64, delta_oa32;
	uint64_t delta_ts64_ns, delta_oa32_ns;
	uint64_t delta_delta;
#define INVALID_CTX_ID 0xffffffff
	uint32_t ctx0_id = INVALID_CTX_ID;
	uint32_t ctx1_id = INVALID_CTX_ID;
	int ret;
	struct accumulator accumulator = {
		.format = fmt
	};
	uint32_t ctx_id_offset, counter_offset, dst_buf_size;
	struct oa_format format = get_oa_format(fmt);

	igt_require_f(hwe, "no render engine\n");

	if (format.report_hdr_64bit) {
		ctx_id_offset = 4;
		counter_offset = 8;
	} else {
		ctx_id_offset = 2;
		counter_offset = 4;
	}

	bops = buf_ops_create(drm_fd);

	for (int i = 0; i < ARRAY_SIZE(src); i++) {
		scratch_buf_init(bops, &src[i], rc_width, rc_height, 0xff0000ff);
		scratch_buf_init(bops, &dst[i], rc_width, rc_height, 0x00ff00ff);
	}

	vm = xe_vm_create(drm_fd, 0, 0);
	context0_id = xe_exec_queue_create(drm_fd, vm, hwe, 0);
	context1_id = xe_exec_queue_create(drm_fd, vm, hwe, 0);
	ibb0 = intel_bb_create_with_context(drm_fd, context0_id, vm, NULL, BATCH_SZ);
	ibb1 = intel_bb_create_with_context(drm_fd, context1_id, vm, NULL, BATCH_SZ);

	igt_debug("submitting warm up render_copy\n");

	/* Submit some early, unmeasured, work to the context we want */
	render_copy(ibb0,
		    &src[0], 0, 0, rc_width, rc_height,
		    &dst[0], 0, 0);

	/* Initialize the context parameter to the perf open ioctl here */
	properties[3] = context0_id;

	igt_debug("opening xe oa stream\n");
	stream_fd = __perf_open(drm_fd, &param, false);
        set_fd_flags(stream_fd, O_CLOEXEC);

#define FOUR_REPORTS (4 * format.size)
#define BO_REPORT_OFFSET(_r) (_r * format.size)

#define FOUR_TIMESTAMPS (4 * 8)
#define BO_TIMESTAMP_OFFSET(_r) (FOUR_REPORTS + (_r * 8))

	dst_buf_size = FOUR_REPORTS + FOUR_TIMESTAMPS;
	dst_buf = intel_buf_create(bops, dst_buf_size, 1, 8, 64,
				   I915_TILING_NONE,
				   I915_COMPRESSION_NONE);

	/* Set write domain to cpu briefly to fill the buffer with 80s */
	buf_map(drm_fd, dst_buf, true /* write enable */);
	memset(dst_buf->ptr, 0, dst_buf_size);
	memset(dst_buf->ptr, 0x80, FOUR_REPORTS);
	intel_buf_unmap(dst_buf);

	/* Submit an mi-rpc to context0 before measurable work */
	emit_stall_timestamp_and_rpc(ibb0,
				     dst_buf,
				     BO_TIMESTAMP_OFFSET(0),
				     BO_REPORT_OFFSET(0),
				     0xdeadbeef);
	intel_bb_flush_render(ibb0);

	/* Remove intel_buf from ibb0 added implicitly in rendercopy */
	intel_bb_remove_intel_buf(ibb0, dst_buf);

	/* This is the work/context that is measured for counter increments */
	render_copy(ibb0,
		    &src[0], 0, 0, rc_width, rc_height,
		    &dst[0], 0, 0);
	intel_bb_flush_render(ibb0);

	/* Submit an mi-rpc to context1 before work
	 *
	 * On gen12, this measurement should just yield counters that are
	 * all zeroes, since the counters will only increment for the
	 * context passed to perf open ioctl
	 */
	emit_stall_timestamp_and_rpc(ibb1,
				     dst_buf,
				     BO_TIMESTAMP_OFFSET(2),
				     BO_REPORT_OFFSET(2),
				     0x00c0ffee);
	intel_bb_flush_render(ibb1);

	/* Submit two copies on the other context to avoid a false
	 * positive in case the driver somehow ended up filtering for
	 * context1
	 */
	render_copy(ibb1,
		    &src[1], 0, 0, rc_width, rc_height,
		    &dst[1], 0, 0);

	render_copy(ibb1,
		    &src[2], 0, 0, rc_width, rc_height,
		    &dst[2], 0, 0);
	intel_bb_flush_render(ibb1);

	/* Submit an mi-rpc to context1 after all work */
	emit_stall_timestamp_and_rpc(ibb1,
				     dst_buf,
				     BO_TIMESTAMP_OFFSET(3),
				     BO_REPORT_OFFSET(3),
				     0x01c0ffee);
	intel_bb_flush_render(ibb1);

	/* Remove intel_buf from ibb1 added implicitly in rendercopy */
	intel_bb_remove_intel_buf(ibb1, dst_buf);

	/* Submit an mi-rpc to context0 after all measurable work */
	emit_stall_timestamp_and_rpc(ibb0,
				     dst_buf,
				     BO_TIMESTAMP_OFFSET(1),
				     BO_REPORT_OFFSET(1),
				     0xbeefbeef);
	intel_bb_flush_render(ibb0);
	intel_bb_sync(ibb0);
	intel_bb_sync(ibb1);

	buf_map(drm_fd, dst_buf, false);

	/* Sanity check reports
	 * reportX_32[0]: report id passed with mi-rpc
	 * reportX_32[1]: timestamp
	 * reportX_32[2]: context id
	 *
	 * report0_32: start of measurable work
	 * report1_32: end of measurable work
	 * report2_32: start of other work
	 * report3_32: end of other work
	 */
	report0_32 = dst_buf->ptr;
	igt_assert_eq(report0_32[0], 0xdeadbeef);
	igt_assert(oa_timestamp(report0_32, fmt));
	ctx0_id = report0_32[ctx_id_offset];
	igt_debug("MI_RPC(start) CTX ID: %u\n", ctx0_id);
	dump_report(report0_32, format.size / 4, "report0_32");

	report1_32 = report0_32 + format.size / 4;
	igt_assert_eq(report1_32[0], 0xbeefbeef);
	igt_assert(oa_timestamp(report1_32, fmt));
	ctx1_id = report1_32[ctx_id_offset];
	igt_debug("CTX ID1: %u\n", ctx1_id);
	dump_report(report1_32, format.size / 4, "report1_32");

	/* Verify that counters in context1 are all zeroes */
	report2_32 = report1_32 + format.size / 4;
	igt_assert_eq(report2_32[0], 0x00c0ffee);
	igt_assert(oa_timestamp(report2_32, fmt));
	dump_report(report2_32, format.size / 4, "report2_32");

	report3_32 = report2_32 + format.size / 4;
	igt_assert_eq(report3_32[0], 0x01c0ffee);
	igt_assert(oa_timestamp(report3_32, fmt));
	dump_report(report3_32, format.size / 4, "report3_32");

	for (int k = counter_offset; k < format.size / 4; k++) {
		igt_assert_f(report2_32[k] == 0, "Failed counter %d check\n", k);
		igt_assert_f(report3_32[k] == 0, "Failed counter %d check\n", k);
	}

	/* Accumulate deltas for counters - A0, A21 and A26 */
	memset(accumulator.deltas, 0, sizeof(accumulator.deltas));
	accumulate_reports(&accumulator, report0_32, report1_32);
	igt_debug("total: A0 = %"PRIu64", A21 = %"PRIu64", A26 = %"PRIu64"\n",
			accumulator.deltas[2 + 0],
			accumulator.deltas[2 + 21],
			accumulator.deltas[2 + 26]);

	igt_debug("oa_timestamp32 0 = %"PRIu64"\n", oa_timestamp(report0_32, fmt));
	igt_debug("oa_timestamp32 1 = %"PRIu64"\n", oa_timestamp(report1_32, fmt));
	igt_debug("ctx_id 0 = %u\n", report0_32[2]);
	igt_debug("ctx_id 1 = %u\n", report1_32[2]);

	/* The delta as calculated via the PIPE_CONTROL timestamp or
	 * the OA report timestamps should be almost identical but
	 * allow a 500 nanoseconds margin.
	 */
	timestamp0_64 = *(uint64_t *)(((uint8_t *)dst_buf->ptr) + BO_TIMESTAMP_OFFSET(0));
	timestamp1_64 = *(uint64_t *)(((uint8_t *)dst_buf->ptr) + BO_TIMESTAMP_OFFSET(1));

	igt_debug("ts_timestamp64 0 = %"PRIu64"\n", timestamp0_64);
	igt_debug("ts_timestamp64 1 = %"PRIu64"\n", timestamp1_64);

	delta_ts64 = timestamp1_64 - timestamp0_64;
	delta_oa32 = oa_timestamp_delta(report1_32, report0_32, fmt);

	/* Sanity check that we can pass the delta to timebase_scale */
	delta_oa32_ns = timebase_scale(delta_oa32);
	delta_ts64_ns = cs_timebase_scale(delta_ts64);

	igt_debug("oa32 delta = %"PRIu64", = %"PRIu64"ns\n",
			delta_oa32, delta_oa32_ns);
	igt_debug("ts64 delta = %"PRIu64", = %"PRIu64"ns\n",
			delta_ts64, delta_ts64_ns);

	delta_delta = delta_ts64_ns > delta_oa32_ns ?
		      (delta_ts64_ns - delta_oa32_ns) :
		      (delta_oa32_ns - delta_ts64_ns);
	if (delta_delta > 500) {
		igt_debug("delta_delta = %"PRIu64". exceeds margin, skipping..\n",
			  delta_delta);
		exit(EAGAIN);
	}

	igt_debug("n samples written = %"PRIu64"/%"PRIu64" (%ix%i)\n",
		  accumulator.deltas[2 + 21],
		  accumulator.deltas[2 + 26],
		  rc_width, rc_height);
	accumulator_print(&accumulator, "filtered");

	/* Verify that the work actually happened by comparing the src
	 * and dst buffers
	 */
	buf_map(drm_fd, &src[0], false);
	buf_map(drm_fd, &dst[0], false);

	ret = memcmp(src[0].ptr, dst[0].ptr, 4 * rc_width * rc_height);
	intel_buf_unmap(&src[0]);
	intel_buf_unmap(&dst[0]);

	if (ret != 0) {
		accumulator_print(&accumulator, "total");
		exit(EAGAIN);
	}

	/* FIXME: can we deduce the presence of A26 from get_oa_format(fmt)? */
	if (intel_graphics_ver(devid) >= IP_VER(20, 0))
		goto skip_check;

	/* Check that this test passed. The test measures the number of 2x2
	 * samples written to the render target using the counter A26. For
	 * OAR, this counter will only have increments relevant to this specific
	 * context. The value equals the rc_width * rc_height of the rendered work.
	 */
	igt_assert_eq(accumulator.deltas[2 + 26], rc_width * rc_height);

 skip_check:
	/* Clean up */
	for (int i = 0; i < ARRAY_SIZE(src); i++) {
		intel_buf_close(bops, &src[i]);
		intel_buf_close(bops, &dst[i]);
	}

	intel_buf_unmap(dst_buf);
	intel_buf_destroy(dst_buf);
	intel_bb_destroy(ibb0);
	intel_bb_destroy(ibb1);
	xe_exec_queue_destroy(drm_fd, context0_id);
	xe_exec_queue_destroy(drm_fd, context1_id);
	xe_vm_destroy(drm_fd, vm);
	buf_ops_destroy(bops);
	__perf_close(stream_fd);
}

/**
 * SUBTEST: unprivileged-single-ctx-counters
 * Description: A harder test for OAR/OAC using MI_REPORT_PERF_COUNT
 */
static void
test_single_ctx_render_target_writes_a_counter(const struct drm_xe_oa_unit *oau)
{
	int child_ret;
	struct igt_helper_process child = {};

	/* Ensure observation_paranoid is set to 1 by default */
	write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

	do {
		igt_fork_helper(&child) {
			/* A local device for local resources. */
			drm_fd = drm_reopen_driver(drm_fd);

			igt_drop_root();

			single_ctx_helper(oau);

			drm_close_driver(drm_fd);
		}
		child_ret = igt_wait_helper(&child);
		igt_assert(WEXITSTATUS(child_ret) == EAGAIN ||
			   WEXITSTATUS(child_ret) == 0);
	} while (WEXITSTATUS(child_ret) == EAGAIN);
}

/**
 * SUBTEST: rc6-disable
 * Description: Check that opening an OA stream disables RC6
 */
static void
test_rc6_disable(void)
{
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, default_test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	unsigned long rc6_start, rc6_end;

	/* Verify rc6 is functional by measuring residency while idle */
	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_require(rc6_end != rc6_start);

	/* While OA is active, we keep rc6 disabled so we don't lose metrics */
	stream_fd = __perf_open(drm_fd, &param, false);

	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_assert_eq(rc6_end - rc6_start, 0);

	__perf_close(stream_fd);

	/* But once OA is closed, we expect the device to sleep again */
	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_assert_neq(rc6_end - rc6_start, 0);
}

/**
 * SUBTEST: stress-open-close
 * Description: Open/close OA streams in a tight loop
 */
static void
test_stress_open_close(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);

	load_helper_init();
	load_helper_run(HIGH);

	igt_until_timeout(2) {
		uint64_t properties[] = {
			DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,

			/* XXX: even without periodic sampling we have to
			 * specify at least one sample layout property...
			 */
			DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
			DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
			DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
			DRM_XE_OA_PROPERTY_OA_DISABLED, true,
		};
		struct intel_xe_oa_open_prop param = {
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	load_helper_stop();
	load_helper_fini();
}

static int __xe_oa_add_config(int fd, struct drm_xe_oa_config *config)
{
	int ret = intel_xe_perf_ioctl(fd, DRM_XE_OBSERVATION_OP_ADD_CONFIG, config);
	if (ret < 0)
		ret = -errno;
	return ret;
}

static int xe_oa_add_config(int fd, struct drm_xe_oa_config *config)
{
	int config_id = __xe_oa_add_config(fd, config);

	igt_debug("config_id=%i\n", config_id);
	igt_assert_lt(0, config_id);

	return config_id;
}

static void xe_oa_remove_config(int fd, uint64_t config_id)
{
	igt_assert_eq(intel_xe_perf_ioctl(fd, DRM_XE_OBSERVATION_OP_REMOVE_CONFIG, &config_id), 0);
}

static bool has_xe_oa_userspace_config(int fd)
{
	uint64_t config = 0;
	int ret = intel_xe_perf_ioctl(fd, DRM_XE_OBSERVATION_OP_REMOVE_CONFIG, &config);
	igt_assert_eq(ret, -1);

	igt_debug("errno=%i\n", errno);

	return errno != EINVAL;
}

#define SAMPLE_MUX_REG (intel_graphics_ver(devid) >= IP_VER(20, 0) ?	\
			0x13000 /* PES* */ : 0x9888 /* NOA_WRITE */)

/**
 * SUBTEST: invalid-create-userspace-config
 * Description: Test invalid configs are rejected
 */
static void
test_invalid_create_userspace_config(void)
{
	struct drm_xe_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	const char *invalid_uuid = "blablabla-wrong";
	uint32_t mux_regs[] = { SAMPLE_MUX_REG, 0x0 };
	uint32_t invalid_mux_regs[] = { 0x12345678 /* invalid register */, 0x0 };

	igt_require(has_xe_oa_userspace_config(drm_fd));

	memset(&config, 0, sizeof(config));

	/* invalid uuid */
	strncpy(config.uuid, invalid_uuid, sizeof(config.uuid));
	config.n_regs = 1;
	config.regs_ptr = to_user_pointer(mux_regs);

	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EINVAL);

	/* invalid mux_regs */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_regs = 1;
	config.regs_ptr = to_user_pointer(invalid_mux_regs);

	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EINVAL);

	/* empty config */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_regs = 0;
	config.regs_ptr = to_user_pointer(mux_regs);

	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EINVAL);

	/* empty config with null pointer */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_regs = 1;
	config.regs_ptr = to_user_pointer(NULL);

	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EINVAL);

	/* invalid pointer */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_regs = 42;
	config.regs_ptr = to_user_pointer((void *) 0xDEADBEEF);

	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EFAULT);
}

/**
 * SUBTEST: invalid-remove-userspace-config
 * Description: Test invalid remove configs are rejected
 */
static void
test_invalid_remove_userspace_config(void)
{
	struct drm_xe_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t mux_regs[] = { SAMPLE_MUX_REG, 0x0 };
	uint64_t config_id, wrong_config_id = 999999999;
	char path[512];

	igt_require(has_xe_oa_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	/* Destroy previous configuration if present */
	if (try_sysfs_read_u64(path, &config_id))
		xe_oa_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));

	memcpy(config.uuid, uuid, sizeof(config.uuid));

	config.n_regs = 1;
	config.regs_ptr = to_user_pointer(mux_regs);

	config_id = xe_oa_add_config(drm_fd, &config);

	/* Removing configs without permissions should fail. */
	igt_fork(child, 1) {
		igt_drop_root();

		intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_REMOVE_CONFIG, &config_id, EACCES);
	}
	igt_waitchildren();

	/* Removing invalid config ID should fail. */
	intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_REMOVE_CONFIG, &wrong_config_id, ENOENT);

	xe_oa_remove_config(drm_fd, config_id);
}

/**
 * SUBTEST: create-destroy-userspace-config
 * Description: Test add/remove OA configs
 */
static void
test_create_destroy_userspace_config(void)
{
	struct drm_xe_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t mux_regs[] = { SAMPLE_MUX_REG, 0x0 };
	uint32_t regs[100];
	int i;
	uint64_t config_id;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,

		DRM_XE_OA_PROPERTY_OA_METRIC_SET, 0, /* Filled later */

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(default_test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
		DRM_XE_OA_PROPERTY_OA_DISABLED, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	char path[512];

	igt_require(has_xe_oa_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	/* Destroy previous configuration if present */
	if (try_sysfs_read_u64(path, &config_id))
		xe_oa_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));
	memcpy(config.uuid, uuid, sizeof(config.uuid));

	regs[0] = mux_regs[0];
	regs[1] = mux_regs[1];
	/* Flex EU counters */
	for (i = 1; i < ARRAY_SIZE(regs) / 2; i++) {
		regs[i * 2] = 0xe458; /* EU_PERF_CNTL0 */
		regs[i * 2 + 1] = 0x0;
	}
	config.regs_ptr = to_user_pointer(regs);
	config.n_regs = ARRAY_SIZE(regs) / 2;

	/* Creating configs without permissions shouldn't work. */
	igt_fork(child, 1) {
		igt_drop_root();

		igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EACCES);
	}
	igt_waitchildren();

	/* Create a new config */
	config_id = xe_oa_add_config(drm_fd, &config);

	/* Verify that adding the another config with the same uuid fails. */
	igt_assert_eq(__xe_oa_add_config(drm_fd, &config), -EADDRINUSE);

	/* Try to use the new config */
	properties[3] = config_id;
	stream_fd = __perf_open(drm_fd, &param, false);

	/* Verify that destroying the config doesn't yield any error. */
	xe_oa_remove_config(drm_fd, config_id);

	/* Read the config to verify shouldn't raise any issue. */
	config_id = xe_oa_add_config(drm_fd, &config);

	__perf_close(stream_fd);

	xe_oa_remove_config(drm_fd, config_id);
}

/**
 * SUBTEST: whitelisted-registers-userspace-config
 * Description: Test that an OA config constructed using whitelisted register works
 */
/* Registers required by userspace. This list should be maintained by
 * the OA configs developers and agreed upon with kernel developers as
 * some of the registers have bits used by the kernel (for workarounds
 * for instance) and other bits that need to be set by the OA configs.
 */
static void
test_whitelisted_registers_userspace_config(void)
{
	struct drm_xe_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t regs[600];
	uint32_t i;
	uint32_t oa_start_trig1, oa_start_trig8;
	uint32_t oa_report_trig1, oa_report_trig8;
	uint64_t config_id;
	char path[512];
	int ret;
	const uint32_t flex[] = {
		0xe458,
		0xe558,
		0xe658,
		0xe758,
		0xe45c,
		0xe55c,
		0xe65c
	};

	igt_require(has_xe_oa_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	if (try_sysfs_read_u64(path, &config_id))
		xe_oa_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));
	memcpy(config.uuid, uuid, sizeof(config.uuid));

	oa_start_trig1 = 0xd900;
	oa_start_trig8 = 0xd91c;
	oa_report_trig1 = 0xd920;
	oa_report_trig8 = 0xd93c;

	/* b_counters_regs: OASTARTTRIG[1-8] */
	for (i = oa_start_trig1; i <= oa_start_trig8; i += 4) {
		regs[config.n_regs * 2] = i;
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
	}
	/* b_counters_regs: OAREPORTTRIG[1-8] */
	for (i = oa_report_trig1; i <= oa_report_trig8; i += 4) {
		regs[config.n_regs * 2] = i;
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
	}

	/* Flex EU registers, only from Gen8+. */
	for (i = 0; i < ARRAY_SIZE(flex); i++) {
		regs[config.n_regs * 2] = flex[i];
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
	}

	/* Mux registers (too many of them, just checking bounds) */
	/* NOA_WRITE */
	regs[config.n_regs * 2] = SAMPLE_MUX_REG;
	regs[config.n_regs * 2 + 1] = 0;
	config.n_regs++;

	/* NOA_CONFIG */
	/* Prior to Xe2 */
	if (intel_graphics_ver(devid) < IP_VER(20, 0)) {
		regs[config.n_regs * 2] = 0xD04;
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
		regs[config.n_regs * 2] = 0xD2C;
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
	}
	/* Prior to MTLx */
	if (intel_graphics_ver(devid) < IP_VER(12, 70)) {
		/* WAIT_FOR_RC6_EXIT */
		regs[config.n_regs * 2] = 0x20CC;
		regs[config.n_regs * 2 + 1] = 0;
		config.n_regs++;
	}

	config.regs_ptr = (uintptr_t) regs;

	/* Create a new config */
	ret = intel_xe_perf_ioctl(drm_fd, DRM_XE_OBSERVATION_OP_ADD_CONFIG, &config);
	igt_assert_lt(0, ret); /* Config 0 should be used by the kernel */
	config_id = ret;

	xe_oa_remove_config(drm_fd, config_id);
}

#define OAG_OASTATUS (0xdafc)
#define OAG_MMIOTRIGGER (0xdb1c)

static const uint32_t oa_wl[] = {
	OAG_MMIOTRIGGER,
	OAG_OASTATUS,
};

static const uint32_t nonpriv_slot_offsets[] = {
	0x4d0, 0x4d4, 0x4d8, 0x4dc, 0x4e0, 0x4e4, 0x4e8, 0x4ec,
	0x4f0, 0x4f4, 0x4f8, 0x4fc, 0x010, 0x014, 0x018, 0x01c,
	0x1e0, 0x1e4, 0x1e8, 0x1ec,
};

struct test_perf {
	const uint32_t *slots;
	uint32_t num_slots;
	const uint32_t *wl;
	uint32_t num_wl;
} perf;

#define HAS_OA_MMIO_TRIGGER(__d) \
	(IS_DG2(__d) || IS_PONTEVECCHIO(__d) || IS_METEORLAKE(__d) || \
	 intel_graphics_ver(devid) >= IP_VER(20, 0))

static void perf_init_whitelist(void)
{
	perf.slots = nonpriv_slot_offsets;
	perf.num_slots = 20;
	perf.wl = oa_wl;
	perf.num_wl = ARRAY_SIZE(oa_wl);
}

static void
emit_oa_reg_read(struct intel_bb *ibb, struct intel_buf *dst, uint32_t offset,
		 uint32_t reg)
{
	intel_bb_add_intel_buf(ibb, dst, true);

	intel_bb_out(ibb, MI_STORE_REGISTER_MEM_GEN8);
	intel_bb_out(ibb, reg);
	intel_bb_emit_reloc(ibb, dst->handle,
			    I915_GEM_DOMAIN_INSTRUCTION,
			    I915_GEM_DOMAIN_INSTRUCTION,
			    offset, dst->addr.offset);
	intel_bb_out(ibb, lower_32_bits(offset));
	intel_bb_out(ibb, upper_32_bits(offset));
}

static void
emit_mmio_triggered_report(struct intel_bb *ibb, uint32_t value)
{
	intel_bb_out(ibb, MI_LOAD_REGISTER_IMM(1));
	intel_bb_out(ibb, OAG_MMIOTRIGGER);
	intel_bb_out(ibb, value);
}

static void dump_whitelist(uint32_t mmio_base, const char *msg)
{
	int i;

	igt_debug("%s\n", msg);

	for (i = 0; i < perf.num_slots; i++)
		igt_debug("FORCE_TO_NON_PRIV_%02d = %08x\n",
			  i, intel_register_read(&mmio_data,
						 mmio_base + perf.slots[i]));
}

static bool in_whitelist(uint32_t mmio_base, uint32_t reg)
{
	int i;

	if (reg & MMIO_BASE_OFFSET)
		reg = (reg & ~MMIO_BASE_OFFSET) + mmio_base;

	for (i = 0; i < perf.num_slots; i++) {
		uint32_t fpriv = intel_register_read(&mmio_data,
						     mmio_base + perf.slots[i]);

		if ((fpriv & RING_FORCE_TO_NONPRIV_ADDRESS_MASK) == reg)
			return true;
	}

	return false;
}

static void oa_regs_in_whitelist(uint32_t mmio_base, bool are_present)
{
	int i;

	if (are_present) {
		for (i = 0; i < perf.num_wl; i++)
			igt_assert(in_whitelist(mmio_base, perf.wl[i]));
	} else {
		for (i = 0; i < perf.num_wl; i++)
			igt_assert(!in_whitelist(mmio_base, perf.wl[i]));
	}
}

static u32 oa_get_mmio_base(const struct drm_xe_engine_class_instance *hwe)
{
	u32 mmio_base = 0x2000;

	switch (hwe->engine_class) {
	case DRM_XE_ENGINE_CLASS_RENDER:
		mmio_base = 0x2000;
		break;
	case DRM_XE_ENGINE_CLASS_COMPUTE:
		switch (hwe->engine_instance) {
		case 0:
			mmio_base = 0x1a000;
			break;
		case 1:
			mmio_base = 0x1c000;
			break;
		case 2:
			mmio_base = 0x1e000;
			break;
		case 3:
			mmio_base = 0x26000;
			break;
		}
	}

	return mmio_base;
}

/**
 * SUBTEST: oa-regs-whitelisted
 * Description: Verify that OA registers are whitelisted
 */
static void test_oa_regs_whitelist(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	const struct drm_xe_engine_class_instance *hwe = oa_unit_engine(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	// uint32_t mmio_base = gem_engine_mmio_base(drm_fd, e->name);
	u32 mmio_base;

	/* FIXME: Add support for OAM whitelist testing */
	if (oau->oa_unit_type != DRM_XE_OA_UNIT_TYPE_OAG)
		return;

	mmio_base = oa_get_mmio_base(hwe);

	intel_register_access_init(&mmio_data,
				   igt_device_get_pci_device(drm_fd), 0);
	stream_fd = __perf_open(drm_fd, &param, false);

	dump_whitelist(mmio_base, "oa whitelisted");

	oa_regs_in_whitelist(mmio_base, true);

	__perf_close(stream_fd);

	dump_whitelist(mmio_base, "oa remove whitelist");

	/*
	 * after perf close, check that registers are removed from the nonpriv
	 * slots
	 * FIXME if needed: currently regs remain added forever
	 */
	// oa_regs_in_whitelist(mmio_base, false);

	intel_register_access_fini(&mmio_data);
}

static void
__test_mmio_triggered_reports(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	const struct drm_xe_engine_class_instance *hwe = oa_unit_engine(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t format_size = get_oa_format(test_set->perf_oa_format).size;
	uint32_t oa_buffer, offset_tail1, offset_tail2;
	struct intel_buf src, dst, *dst_buf;
	uint32_t mmio_triggered_reports = 0;
	uint32_t *start, *end;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t context, vm;
	uint8_t *buf;

	bops = buf_ops_create(drm_fd);

	dst_buf = intel_buf_create(bops, 4096, 1, 8, 64,
				   I915_TILING_NONE,
				   I915_COMPRESSION_NONE);
	buf_map(drm_fd, dst_buf, true);
	memset(dst_buf->ptr, 0, 4096);
	intel_buf_unmap(dst_buf);

	scratch_buf_init(bops, &src, rc_width, rc_height, 0xff0000ff);
	scratch_buf_init(bops, &dst, rc_width, rc_height, 0x00ff00ff);

	vm = xe_vm_create(drm_fd, 0, 0);
	context = xe_exec_queue_create_deconst(drm_fd, vm, hwe, 0);
	igt_assert(context);
	ibb = intel_bb_create_with_context(drm_fd, context, vm, NULL, BATCH_SZ);

	stream_fd = __perf_open(drm_fd, &param, false);
        set_fd_flags(stream_fd, O_CLOEXEC);

	buf = mmap(0, default_oa_buffer_size, PROT_READ, MAP_PRIVATE, stream_fd, 0);
	igt_assert(buf != NULL);

	emit_oa_reg_read(ibb, dst_buf, 0, OAG_OABUFFER);
	emit_oa_reg_read(ibb, dst_buf, 4, OAG_OATAILPTR);
	emit_mmio_triggered_report(ibb, 0xc0ffee11);

	if (render_copy)
		render_copy(ibb,
			    &src, 0, 0, rc_width, rc_height,
			    &dst, 0, 0);

	emit_mmio_triggered_report(ibb, 0xc0ffee22);

	emit_oa_reg_read(ibb, dst_buf, 8, OAG_OATAILPTR);

	intel_bb_flush_render(ibb);
	intel_bb_sync(ibb);

	buf_map(drm_fd, dst_buf, false);

	oa_buffer = dst_buf->ptr[0] & OAG_OATAILPTR_MASK;
	offset_tail1 = (dst_buf->ptr[1] & OAG_OATAILPTR_MASK) - oa_buffer;
	offset_tail2 = (dst_buf->ptr[2] & OAG_OATAILPTR_MASK) - oa_buffer;

	igt_debug("oa_buffer = %08x, tail1 = %08x, tail2 = %08x\n",
		  oa_buffer, offset_tail1, offset_tail2);

	start = (uint32_t *)(buf + offset_tail1);
	end = (uint32_t *)(buf + offset_tail2);
	while (start < end) {
		if (!report_reason(start))
			mmio_triggered_reports++;

		if (get_oa_format(test_set->perf_oa_format).report_hdr_64bit) {
			u64 *start64 = (u64 *)start;

			igt_debug("hdr: %016"PRIx64" %016"PRIx64" %016"PRIx64" %016"PRIx64"\n",
				  start64[0], start64[1], start64[2], start64[3]);
		} else {
			igt_debug("hdr: %08x %08x %08x %08x\n",
				  start[0], start[1], start[2], start[3]);
		}

		start += format_size / 4;
	}

	igt_assert_eq(mmio_triggered_reports, 2);

	munmap(buf, default_oa_buffer_size);
	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_unmap(dst_buf);
	intel_buf_destroy(dst_buf);
	intel_bb_destroy(ibb);
	xe_exec_queue_destroy(drm_fd, context);
	xe_vm_destroy(drm_fd, vm);
	buf_ops_destroy(bops);
	__perf_close(stream_fd);
}

static void
__test_mmio_triggered_reports_read(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	const struct drm_xe_engine_class_instance *hwe = oa_unit_engine(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t format_size = get_oa_format(test_set->perf_oa_format).size;
	struct intel_buf src, dst;
	uint32_t mmio_triggered_reports = 0;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t context, vm;
	uint8_t *buf = calloc(1, default_oa_buffer_size);
	int len, total_len = 0;

	bops = buf_ops_create(drm_fd);

	scratch_buf_init(bops, &src, rc_width, rc_height, 0xff0000ff);
	scratch_buf_init(bops, &dst, rc_width, rc_height, 0x00ff00ff);

	vm = xe_vm_create(drm_fd, 0, 0);
	context = xe_exec_queue_create_deconst(drm_fd, vm, hwe, 0);
	igt_assert(context);
	ibb = intel_bb_create_with_context(drm_fd, context, vm, NULL, BATCH_SZ);

	stream_fd = __perf_open(drm_fd, &param, false);
	set_fd_flags(stream_fd, O_CLOEXEC);

	emit_mmio_triggered_report(ibb, 0xc0ffee11);

	if (render_copy)
		render_copy(ibb,
			    &src, 0, 0, rc_width, rc_height,
			    &dst, 0, 0);

	emit_mmio_triggered_report(ibb, 0xc0ffee22);

	intel_bb_flush_render(ibb);
	intel_bb_sync(ibb);

	while (total_len < default_oa_buffer_size && mmio_triggered_reports < 2 &&
		((len = read(stream_fd, &buf[total_len], format_size)) > 0 ||
		(len == -1 && (errno == EINTR || errno == EIO)))) {
		uint32_t *report = (void *)&buf[total_len];

		if (len != format_size)
			continue;

		if (!report_reason(report))
			mmio_triggered_reports++;

		if (get_oa_format(test_set->perf_oa_format).report_hdr_64bit) {
			u64 *report64 = (u64 *)report;

			igt_debug("hdr: %016"PRIx64" %016"PRIx64" %016"PRIx64" %016"PRIx64"\n",
				  report64[0], report64[1], report64[2], report64[3]);
			if (!report_reason(report))
				igt_assert(report64[2] == 0xc0ffee11 || report64[2] == 0xc0ffee22);
		} else {
			igt_debug("hdr: %08x %08x %08x %08x\n",
				  report[0], report[1], report[2], report[3]);
			if (!report_reason(report))
				igt_assert(report[2] == 0xc0ffee11 || report[2] == 0xc0ffee22);
		}

		if (len > 0)
			total_len += len;
	}

	igt_assert_eq(mmio_triggered_reports, 2);

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_bb_destroy(ibb);
	xe_exec_queue_destroy(drm_fd, context);
	xe_vm_destroy(drm_fd, vm);
	buf_ops_destroy(bops);
	free(buf);
	__perf_close(stream_fd);
}

/**
 * SUBTEST: mmio-triggered-reports
 * Description: Test MMIO trigger functionality
 *
 * SUBTEST: mmio-triggered-reports-read
 * Description: Test MMIO trigger functionality with read system call
 */
static void
test_mmio_triggered_reports(const struct drm_xe_oa_unit *oau, bool with_read)
{
	struct igt_helper_process child = {};
	int ret;

	write_u64_file("/proc/sys/dev/xe/observation_paranoid", 0);
	igt_fork_helper(&child) {
		igt_drop_root();

		if (with_read)
			__test_mmio_triggered_reports_read(oau);
		else
			__test_mmio_triggered_reports(oau);
	}
	ret = igt_wait_helper(&child);
	write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

	igt_assert(WEXITSTATUS(ret) == EAGAIN ||
		   WEXITSTATUS(ret) == 0);
}

/**
 * SUBTEST: sysctl-defaults
 * Description: Test that observation_paranoid sysctl exists
 */
static void
test_sysctl_defaults(void)
{
	int paranoid = read_u64_file("/proc/sys/dev/xe/observation_paranoid");

	igt_assert_eq(paranoid, 1);
}

/**
 * SUBTEST: oa-unit-exclusive-stream
 * Description: Check that only a single stream can be opened on an OA unit
*/
/*
 * Test if OA buffer streams can be independently opened on OA unit. Once a user
 * opens a stream, that oa unit is exclusive to the user, other users get -EBUSY on
 * trying to open a stream.
 */
static void test_oa_unit_exclusive_stream(void)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(drm_fd);
	const struct drm_xe_oa_unit *oau;
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, 0,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, 0,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(0),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t *perf_fd = calloc(qoa->num_oa_units, sizeof(u32));
	struct intel_xe_perf_metric_set *test_set;
	uint32_t i;

	/* for each oa unit, open one stream with sample OA */
	for (i = 0; i < qoa->num_oa_units; i++) {
		oau = oa_unit_by_id(drm_fd, i);
		test_set = oa_unit_metric_set(oau);

		igt_debug("Opening OA stream on OA unit id:type %d:%d\n", i, oau->oa_unit_type);

		properties[1] = oau->oa_unit_id;
		properties[5] = test_set->perf_oa_metrics_set;
		properties[7] = __ff(test_set->perf_oa_format);
		perf_fd[i] = intel_xe_perf_ioctl(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param);
		igt_assert(perf_fd[i] >= 0);
	}

	/* for each oa unit make sure no other streams can be opened */
	for (i = 0; i < qoa->num_oa_units; i++) {
		oau = oa_unit_by_id(drm_fd, i);
		test_set = oa_unit_metric_set(oau);

		igt_debug("Try on OA unit id:type %d:%d\n", i, oau->oa_unit_type);

		/* case 1: concurrent access to OAG should fail */
		properties[1] = oau->oa_unit_id;
		properties[5] = test_set->perf_oa_metrics_set;
		properties[7] = __ff(test_set->perf_oa_format);
		intel_xe_perf_ioctl_err(drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param, EBUSY);
	}

	for (i = 0; i < qoa->num_oa_units; i++)
		if (perf_fd[i])
			close(perf_fd[i]);
}

/**
 * SUBTEST: oa-unit-concurrent-oa-buffer-read
 * Description: Test that we can read streams concurrently on all OA units
 */
static void
test_oa_unit_concurrent_oa_buffer_read(void)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(drm_fd);

	igt_fork(child, qoa->num_oa_units) {
		const struct drm_xe_oa_unit *oau = oa_unit_by_id(drm_fd, child);

		/* No OAM support yet */
		if (oau->oa_unit_type != DRM_XE_OA_UNIT_TYPE_OAG)
			exit(0);

		test_blocking(40 * 1000 * 1000, false, 5 * 1000 * 1000, oau);
	}
	igt_waitchildren();
}

static void *map_oa_buffer(u32 *size)
{
	void *vaddr = mmap(0, default_oa_buffer_size, PROT_READ, MAP_PRIVATE, stream_fd, 0);

	igt_assert(vaddr != NULL);
	*size = default_oa_buffer_size;
	return vaddr;
}

static void invalid_param_map_oa_buffer(const struct drm_xe_oa_unit *oau)
{
	void *oa_vaddr = NULL;

	/* try a couple invalid mmaps */
	/* bad prots */
	oa_vaddr = mmap(0, default_oa_buffer_size, PROT_WRITE, MAP_PRIVATE, stream_fd, 0);
	igt_assert(oa_vaddr == MAP_FAILED);

	oa_vaddr = mmap(0, default_oa_buffer_size, PROT_EXEC, MAP_PRIVATE, stream_fd, 0);
	igt_assert(oa_vaddr == MAP_FAILED);

	/* bad MAPs */
	oa_vaddr = mmap(0, default_oa_buffer_size, PROT_READ, MAP_SHARED, stream_fd, 0);
	igt_assert(oa_vaddr == MAP_FAILED);

	/* bad size */
	oa_vaddr = mmap(0, default_oa_buffer_size + 1, PROT_READ, MAP_PRIVATE, stream_fd, 0);
	igt_assert(oa_vaddr == MAP_FAILED);

	/* do the right thing */
	oa_vaddr = mmap(0, default_oa_buffer_size, PROT_READ, MAP_PRIVATE, stream_fd, 0);
	igt_assert(oa_vaddr != MAP_FAILED && oa_vaddr != NULL);

	munmap(oa_vaddr, default_oa_buffer_size);
}

static void unprivileged_try_to_map_oa_buffer(void)
{
	void *oa_vaddr;

	oa_vaddr = mmap(0, default_oa_buffer_size, PROT_READ, MAP_PRIVATE, stream_fd, 0);
	igt_assert(oa_vaddr == MAP_FAILED);
	igt_assert_eq(errno, EACCES);
}

static void unprivileged_map_oa_buffer(const struct drm_xe_oa_unit *oau)
{
	igt_fork(child, 1) {
		igt_drop_root();
		unprivileged_try_to_map_oa_buffer();
	}
	igt_waitchildren();
}

static jmp_buf jmp;
static void __attribute__((noreturn)) sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

static void try_invalid_access(void *vaddr)
{
	sighandler_t old_sigsegv;
	uint32_t dummy;

	old_sigsegv = signal(SIGSEGV, sigtrap);
	switch (sigsetjmp(jmp, SIGSEGV)) {
	case SIGSEGV:
		break;
	case 0:
		dummy = READ_ONCE(*((uint32_t *)vaddr + 1));
		(void) dummy;
	default:
		igt_assert(!"reached");
		break;
	}
	signal(SIGSEGV, old_sigsegv);
}

static void map_oa_buffer_unprivilege_access(const struct drm_xe_oa_unit *oau)
{
	void *vaddr;
	uint32_t size;

	vaddr = map_oa_buffer(&size);

	igt_fork(child, 1) {
		igt_drop_root();
		try_invalid_access(vaddr);
	}
	igt_waitchildren();

	munmap(vaddr, size);
}

static void map_oa_buffer_forked_access(const struct drm_xe_oa_unit *oau)
{
	void *vaddr;
	uint32_t size;

	vaddr = map_oa_buffer(&size);

	igt_fork(child, 1) {
		try_invalid_access(vaddr);
	}
	igt_waitchildren();

	munmap(vaddr, size);
}

static void mmap_wait_for_periodic_reports(void *oa_vaddr, uint32_t n,
					   const struct drm_xe_oa_unit *oau)
{
	uint32_t period_us = oa_exponent_to_ns(oa_exponent_default) / 1000;
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;
	uint32_t num_periodic_reports = 0;
	uint32_t report_words = get_oa_format(fmt).size >> 2;
	uint32_t *reports;

	while (num_periodic_reports < n) {
		usleep(4 * n * period_us);
		num_periodic_reports = 0;
		for (reports = (uint32_t *)oa_vaddr;
		     reports[0] && oa_timestamp(reports, fmt) && oa_report_is_periodic(reports);
		     reports += report_words) {
			num_periodic_reports++;
		}
	}
}

static void mmap_check_reports(void *oa_vaddr, uint32_t oa_size,
			       const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t fmt = test_set->perf_oa_format;
	struct oa_format format = get_oa_format(fmt);
	size_t report_words = format.size >> 2;
	uint32_t *reports;
	uint32_t timer_reports = 0;

	for (reports = (uint32_t *)oa_vaddr;
	     timer_reports < 10 && reports[0] && oa_timestamp(reports, fmt);
	     reports += report_words) {
		if (!oa_report_is_periodic(reports))
			continue;

		timer_reports++;
		if (timer_reports >= 3) {
			sanity_check_reports(reports - 2 * report_words,
					     reports - report_words, fmt);
			pec_sanity_check_reports(reports - 2 * report_words,
						 reports - report_words, oa_unit_metric_set(oau));
		}
	}

	igt_assert(timer_reports >= 3);
}

static void check_reports_from_mapped_buffer(const struct drm_xe_oa_unit *oau)
{
	void *vaddr;
	uint32_t size;

	vaddr = map_oa_buffer(&size);

	mmap_wait_for_periodic_reports(vaddr, 10, oau);
	mmap_check_reports(vaddr, size, oau);

	munmap(vaddr, size);
}

/**
 * SUBTEST: closed-fd-and-unmapped-access
 * Description: Unmap buffer, close fd and try to access
 */
static void closed_fd_and_unmapped_access(const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	void *vaddr;
	uint32_t size;

	stream_fd = __perf_open(drm_fd, &param, false);
	vaddr = map_oa_buffer(&size);

	mmap_wait_for_periodic_reports(vaddr, 10, oau);
	mmap_check_reports(vaddr, size, oau);

	munmap(vaddr, size);
	__perf_close(stream_fd);

	try_invalid_access(vaddr);
}

/**
 * SUBTEST: tail-address-wrap
 * Description: Test tail address wrap on odd format sizes. Ensure that the
 * format size is not a power of 2. This means that the last report will not be
 * broken down across the OA buffer end. Instead it will be written to the
 * beginning of the OA buffer. We will check the end of the buffer to ensure it
 * has zeroes in it.
 */
static void
test_tail_address_wrap(const struct drm_xe_oa_unit *oau, size_t oa_buffer_size)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	u64 exponent = max_oa_exponent_for_period_lte(20000);
	u64 buffer_size = oa_buffer_size ?: buffer_fill_size;
	u64 fmt = test_set->perf_oa_format;
	u64 properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(fmt),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, exponent,
		DRM_XE_OA_PROPERTY_OA_BUFFER_SIZE, buffer_size,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	u32 fmt_size = get_oa_format(fmt).size;
	u32 zero_size = buffer_size % fmt_size;
	u32 *zero_area, *buffer_end, *buffer_start;

	igt_require(zero_size);

	stream_fd = __perf_open(drm_fd, &param, false);
	set_fd_flags(stream_fd, O_CLOEXEC);

	wait_for_oa_buffer_overflow(stream_fd, 100);

	buffer_start = mmap(0, buffer_size, PROT_READ, MAP_PRIVATE, stream_fd, 0);
	igt_assert(buffer_start);

	zero_area = buffer_start + (buffer_size - zero_size) / 4;
	buffer_end = buffer_start + buffer_size / 4;

	dump_report(zero_area, zero_size / 4, "zero_area");
	while (zero_area < buffer_end)
		igt_assert_eq(*zero_area++, 0);

	munmap(buffer_start, buffer_size);

	__perf_close(stream_fd);
}

/**
 * SUBTEST: map-oa-buffer
 * Description: Verify mapping of oa buffer
 *
 * SUBTEST: invalid-map-oa-buffer
 * Description: Verify invalid mappings of oa buffer
 *
 * SUBTEST: non-privileged-map-oa-buffer
 * Description: Verify if non-privileged user can map oa buffer
 *
 * SUBTEST: non-privileged-access-vaddr
 * Description: Verify if non-privileged user can map oa buffer
 *
 * SUBTEST: privileged-forked-access-vaddr
 * Description: Verify that forked access to mapped buffer fails
 */
typedef void (*map_oa_buffer_test_t)(const struct drm_xe_oa_unit *oau);
static void test_mapped_oa_buffer(map_oa_buffer_test_t test_with_fd_open,
				  const struct drm_xe_oa_unit *oau)
{
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, oa_exponent_default,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(drm_fd, &param, false);

	igt_assert(test_with_fd_open);
	test_with_fd_open(oau);

	__perf_close(stream_fd);
}


/* Return alternative config_id if available, else just return config_id */
static void find_alt_oa_config(u32 config_id, u32 *alt_config_id)
{
	struct dirent *entry;
	int metrics_fd, dir_fd;
	DIR *metrics_dir;
	bool ret;

	metrics_fd = openat(sysfs, "metrics", O_DIRECTORY);
	igt_assert_lte(0, metrics_fd);

	metrics_dir = fdopendir(metrics_fd);
	igt_assert(metrics_dir);

	while ((entry = readdir(metrics_dir))) {
		if (entry->d_type != DT_DIR)
			continue;

		dir_fd = openat(metrics_fd, entry->d_name, O_RDONLY);
		ret = __igt_sysfs_get_u32(dir_fd, "id", alt_config_id);
		close(dir_fd);
		if (!ret)
			continue;

		if (config_id != *alt_config_id)
			goto exit;
	}

	*alt_config_id = config_id;
exit:
	closedir(metrics_dir);
}

#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull

#define WAIT		(0x1 << 0)
#define CONFIG		(0x1 << 1)

enum oa_sync_type {
	OA_SYNC_TYPE_SYNCOBJ,
	OA_SYNC_TYPE_USERPTR,
	OA_SYNC_TYPE_UFENCE,
};

struct oa_sync {
	enum oa_sync_type sync_type;
	u32 syncobj;
	u32 vm;
	u32 bo;
	size_t bo_size;
	struct {
		uint64_t vm_sync;
		uint64_t pad;
		uint64_t oa_sync;
	} *data;
};

static void
oa_sync_init(enum oa_sync_type sync_type, const struct drm_xe_oa_unit *oau,
	     struct oa_sync *osync, struct drm_xe_sync *sync)
{
	const struct drm_xe_engine_class_instance *hwe = oa_unit_engine(oau);
	uint64_t addr = 0x1a0000;

	osync->sync_type = sync_type;
	sync->flags = DRM_XE_SYNC_FLAG_SIGNAL;

	switch (osync->sync_type) {
	case OA_SYNC_TYPE_SYNCOBJ:
		osync->syncobj = syncobj_create(drm_fd, 0);
		sync->handle = osync->syncobj;
		sync->type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		break;
	case OA_SYNC_TYPE_USERPTR:
	case OA_SYNC_TYPE_UFENCE:
		sync->type = DRM_XE_SYNC_TYPE_USER_FENCE;
		sync->timeline_value = USER_FENCE_VALUE;

		osync->vm = xe_vm_create(drm_fd, 0, 0);
		osync->bo_size = xe_bb_size(drm_fd, sizeof(*osync->data));
		if (osync->sync_type == OA_SYNC_TYPE_USERPTR) {
			osync->data = aligned_alloc(xe_get_default_alignment(drm_fd),
						    osync->bo_size);
			igt_assert(osync->data);
		} else {
			osync->bo = xe_bo_create(drm_fd, osync->vm, osync->bo_size,
						 vram_if_possible(drm_fd, hwe->gt_id),
						 DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			osync->data = xe_bo_map(drm_fd, osync->bo, osync->bo_size);
		}
		memset(osync->data, 0, osync->bo_size);

		sync->addr = to_user_pointer(&osync->data[0].vm_sync);
		if (osync->bo)
			xe_vm_bind_async(drm_fd, osync->vm, 0, osync->bo, 0,
					 addr, osync->bo_size, sync, 1);
		else
			xe_vm_bind_userptr_async(drm_fd, osync->vm, 0,
						 to_user_pointer(osync->data),
						 addr, osync->bo_size, sync, 1);
		xe_wait_ufence(drm_fd, &osync->data[0].vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);

		sync->addr = to_user_pointer(&osync->data[0].oa_sync);
		break;
	default:
		igt_assert(false);
	}
}

static void oa_sync_wait(struct oa_sync *osync)
{
	switch (osync->sync_type) {
	case OA_SYNC_TYPE_SYNCOBJ:
		igt_assert(syncobj_wait(drm_fd, &osync->syncobj, 1, INT64_MAX, 0, NULL));
		syncobj_reset(drm_fd, &osync->syncobj, 1);
		break;
	case OA_SYNC_TYPE_USERPTR:
	case OA_SYNC_TYPE_UFENCE:
		xe_wait_ufence(drm_fd, &osync->data[0].oa_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
		osync->data[0].oa_sync = 0;
		break;
	default:
		igt_assert(false);
	}
}

static void oa_sync_free(struct oa_sync *osync)
{
	switch (osync->sync_type) {
	case OA_SYNC_TYPE_SYNCOBJ:
		syncobj_destroy(drm_fd, osync->syncobj);
		break;
	case OA_SYNC_TYPE_USERPTR:
	case OA_SYNC_TYPE_UFENCE:
		if (osync->bo) {
			munmap(osync->data, osync->bo_size);
			gem_close(drm_fd, osync->bo);
		} else {
			free(osync->data);
		}
		xe_vm_destroy(drm_fd, osync->vm);
		break;
	default:
		igt_assert(false);
	}
}

/**
 * SUBTEST: syncs-%s-%s
 *
 * Description: Test OA syncs (with %arg[1] sync types and %arg[2] wait and
 *		reconfig flags) signal correctly in open and reconfig code
 *		paths
 *
 * arg[1]:
 *
 * @syncobj:	sync type syncobj
 *
 * arg[2]:
 *
 * @wait-cfg:	Exercise reconfig path and wait for syncs to signal
 * @wait:	Don't exercise reconfig path and wait for syncs to signal
 * @cfg:	Exercise reconfig path but don't wait for syncs to signal
 * @none:	Don't exercise reconfig path and don't wait for syncs to signal
 */

/**
 * SUBTEST: syncs-%s-%s
 *
 * Description: Test OA syncs (with %arg[1] sync types and %arg[2] wait and
 *		reconfig flags) signal correctly in open and reconfig code
 *		paths
 *
 * arg[1]:
 *
 * @userptr:	sync type userptr
 * @ufence:	sync type ufence
 *
 * arg[2]:
 *
 * @wait-cfg:	Exercise reconfig path and wait for syncs to signal
 * @wait:	Don't exercise reconfig path and wait for syncs to signal
 */
static void test_syncs(const struct drm_xe_oa_unit *oau,
		       enum oa_sync_type sync_type, int flags)
{
	struct drm_xe_ext_set_property extn[XE_OA_MAX_SET_PROPERTIES] = {};
	struct intel_xe_perf_metric_set *test_set = oa_unit_metric_set(oau);
	struct drm_xe_sync sync = {};
	struct oa_sync osync = {};
	uint64_t open_properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, oau->oa_unit_id,
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, test_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(test_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_NUM_SYNCS, 1,
		DRM_XE_OA_PROPERTY_SYNCS, to_user_pointer(&sync),
	};
	struct intel_xe_oa_open_prop open_param = {
		.num_properties = ARRAY_SIZE(open_properties) / 2,
		.properties_ptr = to_user_pointer(open_properties),
	};
	uint64_t config_properties[] = {
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, 0, /* Filled later */
		DRM_XE_OA_PROPERTY_NUM_SYNCS, 1,
		DRM_XE_OA_PROPERTY_SYNCS, to_user_pointer(&sync),
	};
	struct intel_xe_oa_open_prop config_param = {
		.num_properties = ARRAY_SIZE(config_properties) / 2,
		.properties_ptr = to_user_pointer(config_properties),
	};
	uint32_t alt_config_id;
	int ret;

	/*
	 * Necessarily wait in userptr/ufence cases, otherwise IGT process can exit
	 * after calling oa_sync_free, which results in -EFAULT when kernel signals
	 * the userptr/ufence
	 */
	if (sync_type == OA_SYNC_TYPE_USERPTR || sync_type == OA_SYNC_TYPE_UFENCE)
		flags |= WAIT;

	oa_sync_init(sync_type, oau, &osync, &sync);

	stream_fd = __perf_open(drm_fd, &open_param, false);

	/* Reset the sync object if we are going to reconfig the stream */
	if (flags & (WAIT | CONFIG))
		oa_sync_wait(&osync);

	if (!(flags & CONFIG))
		goto exit;

	/* Change stream configuration */
	find_alt_oa_config(test_set->perf_oa_metrics_set, &alt_config_id);

	config_properties[1] = alt_config_id;
	intel_xe_oa_prop_to_ext(&config_param, extn);

	ret = igt_ioctl(stream_fd, DRM_XE_OBSERVATION_IOCTL_CONFIG, extn);
	igt_assert_eq(ret, test_set->perf_oa_metrics_set);

	if (flags & WAIT)
		oa_sync_wait(&osync);
exit:
	__perf_close(stream_fd);
	oa_sync_free(&osync);
}

#define __for_oa_unit_by_type(k) \
	if ((oau = oa_unit_by_type(drm_fd, k))) \
		igt_dynamic_f("%s-%d", oa_unit_name[oau->oa_unit_type], oau->oa_unit_id)

#define __for_oa_unit_by_type_w_arg(k, str) \
	if ((oau = oa_unit_by_type(drm_fd, k))) \
		igt_dynamic_f("%s-%d-%s", oa_unit_name[oau->oa_unit_type], oau->oa_unit_id, str)

#define __for_each_oa_unit(oau) \
	for_each_oa_unit(oau) \
		igt_dynamic_f("%s-%d", oa_unit_name[oau->oa_unit_type], oau->oa_unit_id)

static int opt_handler(int opt, int opt_index, void *data)
{
	uint32_t tmp;

	switch (opt) {
	case 'b':
		tmp = strtoul(optarg, NULL, 0);
		if (tmp <= 20 && tmp >= 1)
			oa_trace_buf_mb = tmp;

		igt_debug("Trace buffer %d Mb\n", oa_trace_buf_mb);
		break;
	case 't':
		oa_trace = true;
		igt_debug("Trace enabled\n");
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const char *help_str =  "  --trace		| -t\t\tEnable ftrace\n"
			       "  --trace_buf_size_mb	| -b\t\tSet ftrace buffer size in MB (default = 1, min = 1, max = 20)\n";

static struct option long_options[] = {
	{"trace", 0, 0, 't'},
	{"trace_buf_size_mb", 0, 0, 'b'},
	{ NULL, 0, 0, 0 }
};

int igt_main_args("b:t", long_options, help_str, opt_handler, NULL)
{
	const struct sync_section {
		const char *name;
		enum oa_sync_type sync_type;
		unsigned int flags;
	} sync_sections[] = {
		{ "syncobj-wait-cfg", OA_SYNC_TYPE_SYNCOBJ, WAIT | CONFIG},
		{ "syncobj-wait", OA_SYNC_TYPE_SYNCOBJ, WAIT },
		{ "syncobj-cfg", OA_SYNC_TYPE_SYNCOBJ, CONFIG },
		{ "syncobj-none", OA_SYNC_TYPE_SYNCOBJ, 0 },
		/* userptr/ufence cases always set WAIT (see test_syncs) */
		{ "userptr-wait-cfg", OA_SYNC_TYPE_USERPTR, WAIT | CONFIG},
		{ "userptr-wait", OA_SYNC_TYPE_USERPTR, WAIT },
		{ "ufence-wait-cfg", OA_SYNC_TYPE_UFENCE, WAIT | CONFIG},
		{ "ufence-wait", OA_SYNC_TYPE_UFENCE, WAIT },
		{ NULL },
	};
	const struct drm_xe_oa_unit *oau;
	struct xe_device *xe_dev;

	igt_fixture() {
		struct stat sb;

		/*
		 * Prior tests may have unloaded the module or failed while
		 * loading/unloading the module. Load xe here before we
		 * stat the files.
		 */
		drm_load_module(DRIVER_XE);
		srandom(time(NULL));
		igt_require(!stat("/proc/sys/dev/xe/observation_paranoid", &sb));
	}

	igt_subtest("sysctl-defaults")
		test_sysctl_defaults();

	igt_fixture() {
		/* We expect that the ref count test before these fixtures
		 * should have closed drm_fd...
		 */
		igt_assert_eq(drm_fd, -1);

		enable_trace_log();
		drm_fd = drm_open_driver(DRIVER_XE);
		xe_dev = xe_device_get(drm_fd);

		/* See xe_query_oa_units_new() */
		igt_require(xe_dev->oa_units);
		igt_require(xe_dev->oa_units->num_oa_units);
		oau = oa_unit_by_id(drm_fd, 0);

		devid = intel_get_drm_devid(drm_fd);
		sysfs = igt_sysfs_open(drm_fd);

		/* Currently only run on Xe2+ */
		igt_require(intel_graphics_ver(devid) >= IP_VER(20, 0));

		igt_require(init_sys_info());

		write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

		render_copy = igt_get_render_copyfunc(drm_fd);
	}

	igt_subtest("non-system-wide-paranoid")
		test_system_wide_paranoid();

	igt_subtest("invalid-oa-metric-set-id")
		test_invalid_oa_metric_set_id();

	igt_subtest("invalid-oa-format-id")
		test_invalid_oa_format_id();

	igt_subtest("missing-sample-flags")
		test_missing_sample_flags();

	igt_subtest_with_dynamic("oa-formats")
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_oa_formats(oau);

	igt_subtest("invalid-oa-exponent")
		test_invalid_oa_exponent();

	igt_subtest_with_dynamic("oa-exponents")
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_oa_exponents(oau);

	igt_subtest_with_dynamic("buffer-fill") {
		igt_require(oau->capabilities & DRM_XE_OA_CAPS_OA_BUFFER_SIZE);
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_buffer_fill(oau);
	}

	/**
	 * SUBTEST: buffer-size
	 * Description: Test various OA buffer sizes
	 */
	igt_subtest_with_dynamic("buffer-size") {
		long k = random() % num_buf_sizes;

		igt_require(oau->capabilities & DRM_XE_OA_CAPS_OA_BUFFER_SIZE);
		__for_oa_unit_by_type_w_arg(DRM_XE_OA_UNIT_TYPE_OAG, buf_sizes[k].name)
			test_non_zero_reason(oau, buf_sizes[k].size);
	}

	igt_subtest_with_dynamic("non-zero-reason") {
		igt_require(!igt_run_in_simulation());
		igt_require(oau->capabilities & DRM_XE_OA_CAPS_OA_BUFFER_SIZE);
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_non_zero_reason(oau, 0);
	}

	/**
	 * SUBTEST: non-zero-reason-all
	 * Description: Non zero reason over all OA units
	 */
	igt_subtest_with_dynamic("non-zero-reason-all") {
		igt_require(oau->capabilities & DRM_XE_OA_CAPS_OA_BUFFER_SIZE);
		__for_each_oa_unit(oau)
			test_non_zero_reason(oau, SZ_128K);
	}

	igt_subtest("disabled-read-error")
		test_disabled_read_error();
	igt_subtest("non-sampling-read-error")
		test_non_sampling_read_error();

	igt_subtest_with_dynamic("enable-disable")
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_enable_disable(oau);

	igt_subtest_with_dynamic("blocking") {
		igt_require(!igt_run_in_simulation());
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_blocking(40 * 1000 * 1000 /* 40ms oa period */,
				      false /* set_kernel_hrtimer */,
				      5 * 1000 * 1000 /* default 5ms/200Hz hrtimer */,
				      oau);
	}

	igt_subtest_with_dynamic("polling") {
		igt_require(!igt_run_in_simulation());
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_polling(40 * 1000 * 1000 /* 40ms oa period */,
				     false /* set_kernel_hrtimer */,
				     5 * 1000 * 1000 /* default 5ms/200Hz hrtimer */,
				     oau);
	}

	igt_subtest("polling-small-buf")
		test_polling_small_buf();

	igt_subtest("short-reads")
		test_short_reads();

	igt_subtest_group() {
		igt_subtest_with_dynamic("mi-rpc")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mi_rpc(oau);

		igt_subtest_with_dynamic("oa-tlb-invalidate") {
			igt_require(intel_graphics_ver(devid) <= IP_VER(12, 70) &&
				    intel_graphics_ver(devid) != IP_VER(12, 60));
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_oa_tlb_invalidate(oau);
		}

		igt_subtest_with_dynamic("unprivileged-single-ctx-counters") {
			igt_require_f(render_copy, "no render-copy function\n");
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_single_ctx_render_target_writes_a_counter(oau);
		}
	}

	igt_subtest_group() {
		igt_subtest("oa-unit-exclusive-stream")
			test_oa_unit_exclusive_stream();

		igt_subtest("oa-unit-concurrent-oa-buffer-read") {
			igt_require(!igt_run_in_simulation());
			test_oa_unit_concurrent_oa_buffer_read();
		}
	}

	igt_subtest("rc6-disable") {
		igt_require(xe_sysfs_gt_has_node(drm_fd, 0, "gtidle"));
		test_rc6_disable();
	}

	igt_subtest_with_dynamic("stress-open-close") {
		__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
			test_stress_open_close(oau);
	}

	igt_subtest("invalid-create-userspace-config")
		test_invalid_create_userspace_config();

	igt_subtest("invalid-remove-userspace-config")
		test_invalid_remove_userspace_config();

	igt_subtest("create-destroy-userspace-config")
		test_create_destroy_userspace_config();

	igt_subtest("whitelisted-registers-userspace-config")
		test_whitelisted_registers_userspace_config();

	igt_subtest_group() {
		igt_subtest_with_dynamic("map-oa-buffer")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mapped_oa_buffer(check_reports_from_mapped_buffer, oau);

		igt_subtest_with_dynamic("invalid-map-oa-buffer")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mapped_oa_buffer(invalid_param_map_oa_buffer, oau);

		igt_subtest_with_dynamic("non-privileged-map-oa-buffer")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mapped_oa_buffer(unprivileged_map_oa_buffer, oau);

		igt_subtest_with_dynamic("non-privileged-access-vaddr")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mapped_oa_buffer(map_oa_buffer_unprivilege_access, oau);

		igt_subtest_with_dynamic("privileged-forked-access-vaddr")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mapped_oa_buffer(map_oa_buffer_forked_access, oau);

		igt_subtest_with_dynamic("closed-fd-and-unmapped-access")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				closed_fd_and_unmapped_access(oau);
	}

	igt_subtest_with_dynamic("tail-address-wrap") {
		long k = random() % num_buf_sizes;

		igt_require(oau->capabilities & DRM_XE_OA_CAPS_OA_BUFFER_SIZE);
		__for_oa_unit_by_type_w_arg(DRM_XE_OA_UNIT_TYPE_OAG, buf_sizes[k].name)
			test_tail_address_wrap(oau, buf_sizes[k].size);
	}

	igt_subtest_group() {
		igt_fixture() {
			perf_init_whitelist();
		}

		igt_subtest_with_dynamic("oa-regs-whitelisted")
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_oa_regs_whitelist(oau);

		igt_subtest_with_dynamic("mmio-triggered-reports") {
			igt_require(HAS_OA_MMIO_TRIGGER(devid));
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mmio_triggered_reports(oau, false);
		}

		igt_subtest_with_dynamic("mmio-triggered-reports-read") {
			igt_require(HAS_OA_MMIO_TRIGGER(devid));
			__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
				test_mmio_triggered_reports(oau, true);
		}
	}

	igt_subtest_group() {
		igt_fixture() {
			igt_require(oau->capabilities & DRM_XE_OA_CAPS_SYNCS);
		}

		for (const struct sync_section *s = sync_sections; s->name; s++) {
			igt_subtest_with_dynamic_f("syncs-%s", s->name) {
				__for_oa_unit_by_type(DRM_XE_OA_UNIT_TYPE_OAG)
					test_syncs(oau, s->sync_type, s->flags);
			}
		}
	}

	igt_fixture() {
		/* leave sysctl options in their default state... */
		write_u64_file("/proc/sys/dev/xe/observation_paranoid", 1);

		if (intel_xe_perf)
			intel_xe_perf_free(intel_xe_perf);

		drm_close_driver(drm_fd);
		disable_trace_log();
	}
}
