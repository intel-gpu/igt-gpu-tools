// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Tests for eudebug online functionality
 * Category: Core
 * Mega feature: EUdebug
 * Sub-category: EUdebug online
 * Functionality: eu kernel debug
 * Test category: functionality test
 */

#include "xe/xe_eudebug.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "intel_pat.h"
#include "intel_mocs.h"
#include "gpgpu_shader.h"
#include <sys/wait.h>

#define SHADER_NOP			(0 << 0)
#define SHADER_BREAKPOINT		(1 << 0)
#define SHADER_LOOP			(1 << 1)
#define SHADER_SINGLE_STEP		(1 << 2)
#define SIP_SINGLE_STEP			(1 << 3)
#define DISABLE_DEBUG_MODE		(1 << 4)
#define SHADER_N_NOOP_BREAKPOINT	(1 << 5)
#define SHADER_CACHING_SRAM		(1 << 6)
#define SHADER_CACHING_VRAM		(1 << 7)
#define SHADER_MIN_THREADS		(1 << 8)
#define DO_NOT_EXPECT_CANARIES		(1 << 9)
#define BB_IN_SRAM			(1 << 10)
#define BB_IN_VRAM			(1 << 11)
#define TARGET_IN_SRAM			(1 << 12)
#define TARGET_IN_VRAM			(1 << 13)
#define SHADER_PAGEFAULT_READ		(1 << 14)
#define SHADER_PAGEFAULT_WRITE		(1 << 15)
#define FAULTABLE_VM			(1 << 16)
#define PAGEFAULT_STRESS_TEST		(1 << 17)
#define SHADER_PAGEFAULT_ONE_OF_MANY	(1 << 18)
#define TRIGGER_UFENCE_SET_BREAKPOINT	(1 << 24)
#define TRIGGER_RESUME_SINGLE_WALK	(1 << 25)
#define TRIGGER_RESUME_PARALLEL_WALK	(1 << 26)
#define TRIGGER_RECONNECT		(1 << 27)
#define TRIGGER_RESUME_SET_BP		(1 << 28)
#define TRIGGER_RESUME_DELAYED		(1 << 29)
#define TRIGGER_RESUME_DSS		(1 << 30)
#define TRIGGER_RESUME_ONE		(1 << 31)

#define SHADER_PAGEFAULT	(SHADER_PAGEFAULT_READ | SHADER_PAGEFAULT_WRITE | \
				 SHADER_PAGEFAULT_ONE_OF_MANY)
#define BB_REGION_BITMASK	(BB_IN_SRAM | BB_IN_VRAM)
#define TARGET_REGION_BITMASK	(TARGET_IN_SRAM | TARGET_IN_VRAM)

#define DEBUGGER_REATTACHED	1

#define SHADER_LOOP_N		3
#define SINGLE_STEP_COUNT	16
#define STEERING_SINGLE_STEP	0
#define STEERING_CONTINUE	0x00c0ffee
#define STEERING_END_LOOP	0xdeadca11

#define CACHING_INIT_VALUE	0xcafe0000
#define CACHING_POISON_VALUE	0xcafedead
#define CACHING_VALUE(n)	(CACHING_INIT_VALUE + (n))

#define SIGTERM_COUNT		50

#define SHADER_CANARY 0x01010101
#define BAD_CANARY 0xf1f1f1f
#define BAD_OFFSET (0x12345678ull << 12)

#define WALKER_X_DIM		4
#define WALKER_ALIGNMENT	16
#define SIMD_SIZE		16

#define STARTUP_TIMEOUT_MS	3000
#define WORKLOAD_DELAY_US	(5000 * 1000)

#define PAGE_SIZE 4096

struct dim_t {
	uint32_t x;
	uint32_t y;
	uint32_t alignment;
};

static struct dim_t walker_dimensions(int threads)
{
	uint32_t x_dim = min_t(x_dim, threads, WALKER_X_DIM);
	struct dim_t ret = {
		.x = x_dim,
		.y = threads / x_dim,
		.alignment = WALKER_ALIGNMENT
	};

	return ret;
}

static struct dim_t surface_dimensions(int threads)
{
	struct dim_t ret = walker_dimensions(threads);

	ret.y = max_t(ret.y, threads / ret.x, 4);
	ret.x *= SIMD_SIZE;
	ret.alignment *= SIMD_SIZE;

	return ret;
}

static uint32_t steering_offset(int threads)
{
	struct dim_t w = walker_dimensions(threads);

	return ALIGN(w.x, w.alignment) * w.y * 4;
}

static struct intel_buf *create_uc_buf(int fd, int width, int height, uint64_t region)
{
	struct intel_buf *buf;

	buf = intel_buf_create_full(buf_ops_create(fd), 0, width / 4, height,
				    32, 0, I915_TILING_NONE, 0, 0, 0, region,
				    DEFAULT_PAT_INDEX, DEFAULT_MOCS_INDEX);

	return buf;
}

static int get_maximum_number_of_threads(int fd)
{
	uint32_t subslices = xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_MAX_SUBSLICE);
	uint32_t eus_per_subslice =
		xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_MAX_EU_PER_SUBSLICE);
	uint32_t threads_per_eu =
		xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_NUM_THREADS_PER_EU);

	return subslices * eus_per_subslice * threads_per_eu;
}

struct online_debug_data {
	pthread_mutex_t mutex;
	/* client in */
	int drm_fd;
	struct drm_xe_engine_class_instance hwe;
	uint64_t flags;
	int thread_count;
	/* client out */
	int thread_hit_count;
	/* debugger internals */
	uint64_t client_handle;
	uint64_t exec_queue_handle;
	uint64_t lrc_handle;
	uint64_t target_offset;
	size_t target_size;
	uint64_t bb_offset;
	size_t bb_size;
	int vm_fd;
	uint32_t kernel_offset;
	uint32_t first_aip;
	uint64_t *aips_offset_table;
	uint32_t steps_done;
	uint8_t *single_step_bitmask;
	int stepped_threads_count;
	struct timespec exception_arrived;
	int last_eu_control_seqno;
	struct drm_xe_eudebug_event *exception_event;
	int att_event_counter;
	uint32_t pf_thread_number;
	int num_threads_per_eu;
	int max_subslices_per_slice;
	struct dim_t w_dim;
};

static int get_number_of_threads(struct online_debug_data *data)
{
	if (data->flags & SHADER_PAGEFAULT_ONE_OF_MANY)
		return xe_query_eu_thread_count(data->drm_fd, 0);

	if (data->flags & (PAGEFAULT_STRESS_TEST))
		return get_maximum_number_of_threads(data->drm_fd);

	if (data->flags & (SHADER_MIN_THREADS | SHADER_PAGEFAULT))
		return 16;

	if (data->flags & (TRIGGER_RESUME_ONE | TRIGGER_RESUME_SINGLE_WALK |
	    TRIGGER_RESUME_PARALLEL_WALK | SHADER_CACHING_SRAM | SHADER_CACHING_VRAM))
		return 32;

	return 512;
}

static int caching_get_instruction_count(int fd, uint32_t s_dim__x, int flags)
{
	uint64_t memory;

	igt_assert((flags & SHADER_CACHING_SRAM) || (flags & SHADER_CACHING_VRAM));

	if (flags & SHADER_CACHING_SRAM)
		memory = system_memory(fd);
	else
		memory = vram_memory(fd, 0);

	/* each instruction writes to given y offset */
	return (2 * xe_min_page_size(fd, memory)) / s_dim__x;
}

static struct gpgpu_shader *get_shader(struct online_debug_data *data)
{
	struct dim_t w_dim = walker_dimensions(data->thread_count);
	struct dim_t s_dim = surface_dimensions(data->thread_count);
	static struct gpgpu_shader *shader;

	shader = gpgpu_shader_create(data->drm_fd);

	if (shader->gen_ver == 3000)
		gpgpu_shader_set_vrt(shader, VRT_96);

	shader->simd_size = SIMD_SIZE;

	if (data->flags & PAGEFAULT_STRESS_TEST)
		shader->num_threads_in_tg = gpgpu_shader__get_max_threads_in_tg(shader);

	gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
	if (data->flags & SHADER_BREAKPOINT) {
		gpgpu_shader__nop(shader);
		gpgpu_shader__breakpoint(shader);
	} else if (data->flags & SHADER_LOOP) {
		gpgpu_shader__label(shader, 0);
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
		gpgpu_shader__jump_neq(shader, 0, w_dim.y, STEERING_END_LOOP);
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
	} else if (data->flags & SHADER_SINGLE_STEP) {
		gpgpu_shader__nop(shader);
		gpgpu_shader__breakpoint(shader);
		for (int i = 0; i < SINGLE_STEP_COUNT; i++)
			gpgpu_shader__nop(shader);
	} else if (data->flags & SHADER_N_NOOP_BREAKPOINT) {
		for (int i = 0; i < SHADER_LOOP_N; i++) {
			gpgpu_shader__nop(shader);
			gpgpu_shader__breakpoint(shader);
		}
	} else if ((data->flags & SHADER_CACHING_SRAM) || (data->flags & SHADER_CACHING_VRAM)) {
		int  count = caching_get_instruction_count(data->drm_fd, s_dim.x, data->flags);

		gpgpu_shader__nop(shader);
		gpgpu_shader__breakpoint(shader);
		for (int i = 0; i < count; i++)
			gpgpu_shader__common_target_write_u32(shader, s_dim.y + i, CACHING_VALUE(i));
		gpgpu_shader__nop(shader);
		gpgpu_shader__breakpoint(shader);
	} else if (data->flags & SHADER_PAGEFAULT) {
		if (data->flags & SHADER_PAGEFAULT_READ)
			gpgpu_shader__read_a64_d32(shader, BAD_OFFSET);
		else if (data->flags & SHADER_PAGEFAULT_WRITE)
			gpgpu_shader__write_a64_d32(shader, BAD_OFFSET, BAD_CANARY);
		else if (data->flags & SHADER_PAGEFAULT_ONE_OF_MANY)
			emit_iga64_code(shader, pagefault_one_of_many, R"(
#if GEN_VER >= 2000
	// prepare load descriptor for page-faulting address
	mov (8) r30.0<1>:uq 0x0:uq
	mov (1) r30.0<1>:uq 0x12345678000:uq // PF address
	mov (1) r30.2<1>:ud 0x3f:ud
	mov (1) r30.4<1>:ud 0x3f:ud
	mov (1) r30.7<1>:ud 0x3:ud // 4 bytes
	// calculate thread id: r20.0 = dim.x * tgid.y + tgid.x
	mad (1) r20.0<1>:ud r0.1<0;0>:ud r0.6<0;0>:ud r1.4<0>:ud
	// page-fault only for arbitrary thread
	cmp (1) (eq)f0.0 null<1>:ud r20.0<0;1,0>:ud ARG(0):ud
(f0.0)	send.ugm (1) r31 r30 null 0x0 0x2128403 // load_block2d.ugm.d32t.a64.uc.uc
#endif
			)", data->pf_thread_number);

		gpgpu_shader__label(shader, 0);
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
		gpgpu_shader__jump_neq(shader, 0, w_dim.y, STEERING_END_LOOP);
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
	}

	gpgpu_shader__eot(shader);

	return shader;
}

static struct gpgpu_shader *get_sip(struct online_debug_data *data)
{
	struct dim_t w_dim = walker_dimensions(data->thread_count);
	static struct gpgpu_shader *sip;

	sip = gpgpu_shader_create(data->drm_fd);
	if (!(data->flags & SHADER_PAGEFAULT_ONE_OF_MANY))
		gpgpu_shader__write_aip(sip, 0);
	else
		emit_iga64_code(sip, store_sr0_0, R"(
#if GEN_VER >= 2000
	mov (1) r5.0<1>:ud sr0.0:ud
	SET_THREAD_SPACE_ADDR(r4, 0, 0:ud, 4)
	STORE_SPACE_DW(r4, r5)
#endif
			)");

	gpgpu_shader__wait(sip);
	if (data->flags & SIP_SINGLE_STEP)
		gpgpu_shader__end_system_routine_step_if_eq(sip, w_dim.y, 0);
	else
		gpgpu_shader__end_system_routine(sip, true);

	return sip;
}

static int eu_attentions_xor_count(const uint32_t *a, const uint32_t *b, uint32_t size)
{
	int count = 0;

	for (int i = 0; i < size / 4 ; i++)
		count += igt_hweight(a[i] ^ b[i]);

	return count;
}

static int count_canaries_eq(uint32_t *ptr, struct dim_t w_dim, uint32_t value)
{
	int count = 0;
	int x, y;

	for (x = 0; x < w_dim.x; x++)
		for (y = 0; y < w_dim.y; y++)
			if (READ_ONCE(ptr[x + ALIGN(w_dim.x, w_dim.alignment) * y]) == value)
				count++;

	return count;
}

static int count_canaries_neq(uint32_t *ptr, struct dim_t w_dim, uint32_t value)
{
	return w_dim.x * w_dim.y - count_canaries_eq(ptr, w_dim, value);
}

static const char *td_ctl_cmd_to_str(uint32_t cmd)
{
	switch (cmd) {
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL:
		return "interrupt all";
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED:
		return "stopped";
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME:
		return "resume";
	default:
		return "unknown command";
	}
}

static int __eu_ctl(int debugfd, uint64_t client,
		    uint64_t exec_queue, uint64_t lrc,
		    uint8_t *bitmask, uint32_t *bitmask_size,
		    uint32_t cmd, uint64_t *seqno)
{
	struct drm_xe_eudebug_eu_control control = {
		.client_handle = lower_32_bits(client),
		.exec_queue_handle = exec_queue,
		.lrc_handle = lrc,
		.cmd = cmd,
		.bitmask_ptr = to_user_pointer(bitmask),
	};
	int ret;

	if (bitmask_size)
		control.bitmask_size = *bitmask_size;

	ret = igt_ioctl(debugfd, DRM_XE_EUDEBUG_IOCTL_EU_CONTROL, &control);

	if (ret < 0)
		return -errno;

	igt_debug("EU CONTROL[%llu]: %s\n", control.seqno, td_ctl_cmd_to_str(cmd));

	if (bitmask_size)
		*bitmask_size = control.bitmask_size;

	if (seqno)
		*seqno = control.seqno;

	return 0;
}

static uint64_t eu_ctl(int debugfd, uint64_t client,
		       uint64_t exec_queue, uint64_t lrc,
		       uint8_t *bitmask, uint32_t *bitmask_size, uint32_t cmd)
{
	uint64_t seqno;

	igt_assert_eq(__eu_ctl(debugfd, client, exec_queue, lrc, bitmask,
			       bitmask_size, cmd, &seqno), 0);

	return seqno;
}

static bool intel_gen_needs_resume_wa(int fd)
{
	const uint32_t id = intel_get_drm_devid(fd);

	return intel_gen(id) == 12 && intel_graphics_ver(id) < IP_VER(12, 55);
}

static uint64_t eu_ctl_resume(int fd, int debugfd, uint64_t client,
			      uint64_t exec_queue, uint64_t lrc,
			      uint8_t *bitmask, uint32_t bitmask_size)
{
	int i;

	/*  Wa_14011332042 */
	if (intel_gen_needs_resume_wa(fd)) {
		uint32_t *att_reg_half = (uint32_t *)bitmask;

		for (i = 0; i < bitmask_size / sizeof(uint32_t); i += 2) {
			att_reg_half[i] |= att_reg_half[i + 1];
			att_reg_half[i + 1] |= att_reg_half[i];
		}
	}

	return eu_ctl(debugfd, client, exec_queue, lrc, bitmask, &bitmask_size,
		      DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME);
}

static inline uint64_t eu_ctl_stopped(int debugfd, uint64_t client,
				      uint64_t exec_queue, uint64_t lrc,
				      uint8_t *bitmask, uint32_t *bitmask_size)
{
	return eu_ctl(debugfd, client, exec_queue, lrc, bitmask, bitmask_size,
		      DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED);
}

static inline uint64_t eu_ctl_interrupt_all(int debugfd, uint64_t client,
					    uint64_t exec_queue, uint64_t lrc)
{
	return eu_ctl(debugfd, client, exec_queue, lrc, NULL, 0,
		      DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL);
}

static struct online_debug_data *
online_debug_data_create(int drm_fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	struct online_debug_data *data;

	data = mmap(0, ALIGN(sizeof(*data), PAGE_SIZE),
		    PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(data);

	data->drm_fd = drm_fd;
	memcpy(&data->hwe, hwe, sizeof(*hwe));
	data->flags = flags;
	data->thread_count = get_number_of_threads(data);
	pthread_mutex_init(&data->mutex, NULL);
	data->client_handle = -1ULL;
	data->exec_queue_handle = -1ULL;
	data->lrc_handle = -1ULL;
	data->vm_fd = -1;
	data->stepped_threads_count = -1;
	data->w_dim = walker_dimensions(data->thread_count);

	return data;
}

static void online_debug_data_destroy(struct online_debug_data *data)
{
	free(data->aips_offset_table);
	munmap(data, ALIGN(sizeof(*data), PAGE_SIZE));
}

static void wait_for_workloads_start(struct online_debug_data **data, int n)
{
	int val;
	int count;
	int i;

	igt_for_milliseconds(n * STARTUP_TIMEOUT_MS) {
		count = 0;
		for (i = 0; i < n; i++) {
			if (READ_ONCE(data[i]->vm_fd) == -1 ||
			    READ_ONCE(data[i]->target_size) == 0)
				continue;

			if (pread(data[i]->vm_fd, &val, sizeof(val),
				  data[i]->target_offset) == sizeof(val) &&
			    val != 0)
				count++;
		}

		if (count == n)
			break;
	}
	igt_assert_eq(count, n);
}

static void wait_for_workload_start(struct online_debug_data *data)
{
	struct online_debug_data *array[] = { data };

	wait_for_workloads_start(array, 1);
}

static void eu_attention_debug_trigger(struct xe_eudebug_debugger *d,
				       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *)e;
	uint32_t *ptr = (uint32_t *)att->bitmask;

	igt_debug("EVENT[%llu] eu-attenttion; threads=%d "
		 "client[%llu], exec_queue[%llu], lrc[%llu], bitmask_size[%d]\n",
		 att->base.seqno, igt_bitmap_hweight(att->bitmask, att->bitmask_size * 8),
				att->client_handle, att->exec_queue_handle,
				att->lrc_handle, att->bitmask_size);

	for (uint32_t i = 0; i < att->bitmask_size / 4; i += 2)
		igt_debug("bitmask[%d] = 0x%08x%08x\n", i / 2, ptr[i], ptr[i + 1]);
}

static void eu_attention_reset_trigger(struct xe_eudebug_debugger *d,
				       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *)e;
	uint32_t *ptr = (uint32_t *)att->bitmask;
	struct online_debug_data *data = d->ptr;

	igt_debug("EVENT[%llu] eu-attention with reset; threads=%d "
		 "client[%llu], exec_queue[%llu], lrc[%llu], bitmask_size[%d]\n",
		 att->base.seqno, igt_bitmap_hweight(att->bitmask, att->bitmask_size * 8),
				att->client_handle, att->exec_queue_handle,
				att->lrc_handle, att->bitmask_size);

	for (uint32_t i = 0; i < att->bitmask_size / 4; i += 2)
		igt_debug("bitmask[%d] = 0x%08x%08x\n", i / 2, ptr[i], ptr[i + 1]);

	xe_force_gt_reset_async(d->master_fd, data->hwe.gt_id);
}

static void only_nth_set_bit(uint8_t *dst, uint8_t *src, int size, int n)
{
	int count = 0;

	for (int i = 0; i < size; i++) {
		if (count < n) {
			uint8_t tmp = src[i];

			for (int j = 0; j < 8; j++) {
				if (tmp & (1 << j)) {
					count++;
					if (count == n)
						dst[i] |= (1 << j);
					else
						dst[i] &= ~(1 << j);
				} else {
					dst[i] &= ~(1 << j);
				}
			}
		} else {
			dst[i] = 0;
		}
	}
}

static void only_first_set_bit(uint8_t *dst, uint8_t *src, int size)
{
	return only_nth_set_bit(dst, src, size, 1);
}

/*
 * Searches for the first instruction. It stands on assumption,
 * that shader kernel is placed before sip within the bb.
 */
static uint32_t find_kernel_in_bb(struct gpgpu_shader *kernel,
				  struct online_debug_data *data)
{
	uint32_t *p = kernel->code;
	uint8_t *buf, *ptr;
	uint32_t offset;

	buf = malloc(data->bb_size);

	igt_assert_eq(pread(data->vm_fd, buf, data->bb_size, data->bb_offset), data->bb_size);

	ptr = memmem(buf, data->bb_size, p, kernel->size * sizeof(uint32_t));
	igt_assert(ptr);

	offset = ptr - buf;

	free(buf);

	return offset;
}

static bool set_breakpoint_once(struct xe_eudebug_debugger *d,
				struct online_debug_data *data)
{
	const uint32_t breakpoint_bit = 1 << 30;
	size_t sz = sizeof(uint32_t);
	bool breakpoint_set = false;
	struct gpgpu_shader *kernel;
	uint32_t aip;

	kernel = get_shader(data);

	if (!data->kernel_offset) {
		uint32_t instr_usdw;

		igt_assert(data->vm_fd != -1);
		igt_assert(data->target_size != 0);
		igt_assert(data->bb_size != 0);

		data->kernel_offset = find_kernel_in_bb(kernel, data);

		/* set breakpoint on last instruction */
		aip = data->kernel_offset + kernel->size * 4 - 0x10;
		igt_assert_eq(pread(data->vm_fd, &instr_usdw, sz,
				    data->bb_offset + aip), sz);
		instr_usdw |= breakpoint_bit;
		igt_assert_eq(pwrite(data->vm_fd, &instr_usdw, sz,
				     data->bb_offset + aip), sz);
		fsync(data->vm_fd);

		breakpoint_set = true;
	}

	gpgpu_shader_destroy(kernel);

	return breakpoint_set;
}

static void get_aips_offset_table(struct online_debug_data *data, int threads)
{
	size_t sz = sizeof(uint32_t);
	uint32_t aip;
	uint32_t first_aip;
	int table_index = 0;

	if (data->aips_offset_table)
		return;

	data->aips_offset_table = malloc(threads * sizeof(uint64_t));
	igt_assert(data->aips_offset_table);

	igt_assert_eq(pread(data->vm_fd, &first_aip, sz, data->target_offset), sz);
	data->first_aip = first_aip;
	data->aips_offset_table[table_index++] = 0;

	fsync(data->vm_fd);
	for (int i = sz; i < data->target_size; i += sz) {
		igt_assert_eq(pread(data->vm_fd, &aip, sz, data->target_offset + i), sz);
		if (aip == first_aip)
			data->aips_offset_table[table_index++] = i;
	}

	igt_assert_eq(threads, table_index);

	igt_debug("AIPs offset table:\n");
	for (int i = 0; i < threads; i++)
		igt_debug("%" PRIx64 "\n", data->aips_offset_table[i]);
}

static int get_stepped_threads_count(struct online_debug_data *data, int threads)
{
	int count = 0;
	size_t sz = sizeof(uint32_t);
	uint32_t aip;

	fsync(data->vm_fd);
	for (int i = 0; i < threads; i++) {
		igt_assert_eq(pread(data->vm_fd, &aip, sz,
				    data->target_offset + data->aips_offset_table[i]), sz);
		if (aip != data->first_aip) {
			igt_assert(aip == data->first_aip + 0x10);
			count++;
		}
	}

	return count;
}

static void save_first_exception_trigger(struct xe_eudebug_debugger *d,
					 struct drm_xe_eudebug_event *e)
{
	struct online_debug_data *data = d->ptr;

	pthread_mutex_lock(&data->mutex);
	if (!data->exception_event) {
		igt_gettime(&data->exception_arrived);
		data->exception_event = igt_memdup(e, e->len);
	}
	pthread_mutex_unlock(&data->mutex);
}

#define MAX_PREEMPT_TIMEOUT 10ull
static void eu_attention_resume_trigger(struct xe_eudebug_debugger *d,
					struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *) e;
	struct online_debug_data *data = d->ptr;
	uint32_t bitmask_size = att->bitmask_size;
	uint8_t *bitmask;
	int i;

	if (data->last_eu_control_seqno > att->base.seqno)
		return;

	bitmask = calloc(1, att->bitmask_size);
	igt_assert(bitmask);

	eu_ctl_stopped(d->fd, att->client_handle, att->exec_queue_handle,
		       att->lrc_handle, bitmask, &bitmask_size);
	igt_assert(bitmask_size == att->bitmask_size);

	/* No guarantee that all pagefaulting eu threads will raise attention */
	if (!(d->flags & SHADER_PAGEFAULT))
		igt_assert(memcmp(bitmask, att->bitmask, att->bitmask_size) == 0);

	pthread_mutex_lock(&data->mutex);
	if (igt_nsec_elapsed(&data->exception_arrived) < (MAX_PREEMPT_TIMEOUT + 1) * NSEC_PER_SEC &&
	    d->flags & TRIGGER_RESUME_DELAYED) {
		pthread_mutex_unlock(&data->mutex);
		free(bitmask);
		return;
	} else if (d->flags & TRIGGER_RESUME_ONE) {
		only_first_set_bit(bitmask, bitmask, bitmask_size);
	} else if (d->flags & TRIGGER_RESUME_DSS) {
		uint64_t *event = (uint64_t *)att->bitmask;
		uint64_t *resume = (uint64_t *)bitmask;

		memset(bitmask, 0, bitmask_size);
		for (i = 0; i < att->bitmask_size / sizeof(uint64_t); i++) {
			if (!event[i])
				continue;

			resume[i] = event[i];
			break;
		}
	} else if (d->flags & TRIGGER_RESUME_SET_BP) {
		if (!set_breakpoint_once(d, data)) {
			/* breakpoint already set, check if the first thread managed to hit it */
			uint32_t expected, aip;
			struct gpgpu_shader *kernel;

			kernel = get_shader(data);
			expected = data->kernel_offset + kernel->size * 4 - 0x10;

			igt_assert_eq(pread(data->vm_fd, &aip, sizeof(aip),
					    data->target_offset), sizeof(aip));
			igt_assert_eq_u32(aip, expected);

			gpgpu_shader_destroy(kernel);
		}
	}

	if (d->flags & (SHADER_LOOP | SHADER_PAGEFAULT)) {
		uint32_t threads = data->thread_count;
		uint32_t val = STEERING_END_LOOP;

		igt_assert_eq(pwrite(data->vm_fd, &val, sizeof(uint32_t),
				     data->target_offset + steering_offset(threads)),
			      sizeof(uint32_t));
		fsync(data->vm_fd);
	}
	pthread_mutex_unlock(&data->mutex);

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, att->client_handle,
						    att->exec_queue_handle, att->lrc_handle,
						    bitmask, att->bitmask_size);

	free(bitmask);
}

static void eu_attention_resume_single_step_trigger(struct xe_eudebug_debugger *d,
						    struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *) e;
	struct online_debug_data *data = d->ptr;
	const int threads = data->thread_count;
	uint32_t val;
	size_t sz = sizeof(uint32_t);

	get_aips_offset_table(data, threads);

	if (data->last_eu_control_seqno > att->base.seqno)
		return;

	if (d->flags & TRIGGER_RESUME_PARALLEL_WALK) {
		if (data->stepped_threads_count != -1)
			if (data->steps_done < SINGLE_STEP_COUNT) {
				int stepped_threads_count_after_resume =
						get_stepped_threads_count(data, threads);
				igt_debug("Stepped threads after: %d\n",
					  stepped_threads_count_after_resume);

				if (stepped_threads_count_after_resume == threads) {
					data->first_aip += 0x10;
					data->steps_done++;
				}

				igt_debug("Shader steps: %d\n", data->steps_done);
				igt_assert(data->stepped_threads_count == 0);
				igt_assert(stepped_threads_count_after_resume == threads);
			}

		if (data->steps_done < SINGLE_STEP_COUNT) {
			data->stepped_threads_count = get_stepped_threads_count(data, threads);
			igt_debug("Stepped threads before: %d\n", data->stepped_threads_count);
		}

		val = data->steps_done < SINGLE_STEP_COUNT ? STEERING_SINGLE_STEP :
							     STEERING_CONTINUE;
	} else if (d->flags & TRIGGER_RESUME_SINGLE_WALK) {
		if (data->stepped_threads_count != -1)
			if (data->steps_done < 2) {
				int stepped_threads_count_after_resume =
						get_stepped_threads_count(data, threads);
				igt_debug("Stepped threads after: %d\n",
					  stepped_threads_count_after_resume);

				if (stepped_threads_count_after_resume == threads) {
					data->first_aip += 0x10;
					data->steps_done++;
					free(data->single_step_bitmask);
					data->single_step_bitmask = 0;
				}

				igt_debug("Shader steps: %d\n", data->steps_done);
				igt_assert(data->stepped_threads_count +
					   (intel_gen_needs_resume_wa(d->master_fd) ? 2 : 1) ==
					   stepped_threads_count_after_resume);
			}

		if (data->steps_done < 2) {
			data->stepped_threads_count = get_stepped_threads_count(data, threads);
			igt_debug("Stepped threads before: %d\n", data->stepped_threads_count);
			if (intel_gen_needs_resume_wa(d->master_fd)) {
				if (!data->single_step_bitmask) {
					data->single_step_bitmask = malloc(att->bitmask_size *
									   sizeof(uint8_t));
					igt_assert(data->single_step_bitmask);
					memcpy(data->single_step_bitmask, att->bitmask,
					       att->bitmask_size);
				}

				only_first_set_bit(att->bitmask, data->single_step_bitmask,
						   att->bitmask_size);
			} else
				only_nth_set_bit(att->bitmask, att->bitmask, att->bitmask_size,
						 data->stepped_threads_count + 1);
		}

		val = data->steps_done < 2 ? STEERING_SINGLE_STEP : STEERING_CONTINUE;
	}

	igt_assert_eq(pwrite(data->vm_fd, &val, sz,
			     data->target_offset + steering_offset(threads)), sz);
	fsync(data->vm_fd);

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, att->client_handle,
						    att->exec_queue_handle, att->lrc_handle,
						    att->bitmask, att->bitmask_size);

	if (data->single_step_bitmask)
		for (int i = 0; i < att->bitmask_size; i++)
			data->single_step_bitmask[i] &= ~att->bitmask[i];
}

static void open_trigger(struct xe_eudebug_debugger *d,
			 struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_client *client = (void *)e;
	struct online_debug_data *data = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_DESTROY)
		return;

	pthread_mutex_lock(&data->mutex);
	data->client_handle = client->client_handle;
	pthread_mutex_unlock(&data->mutex);
}

static void exec_queue_trigger(struct xe_eudebug_debugger *d,
			       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_exec_queue *eq = (void *)e;
	struct online_debug_data *data = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_DESTROY)
		return;

	pthread_mutex_lock(&data->mutex);
	data->exec_queue_handle = eq->exec_queue_handle;
	data->lrc_handle = eq->lrc_handle[0];
	pthread_mutex_unlock(&data->mutex);
}

static void vm_open_trigger(struct xe_eudebug_debugger *d,
			    struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm *vm = (void *)e;
	struct online_debug_data *data = d->ptr;
	struct drm_xe_eudebug_vm_open vo = {
		.client_handle = vm->client_handle,
		.vm_handle = vm->vm_handle,
	};
	int fd;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
		igt_assert_lte(0, fd);

		pthread_mutex_lock(&data->mutex);
		igt_assert(data->vm_fd == -1);
		data->vm_fd = fd;
		pthread_mutex_unlock(&data->mutex);
		return;
	}

	pthread_mutex_lock(&data->mutex);
	close(data->vm_fd);
	data->vm_fd = -1;
	pthread_mutex_unlock(&data->mutex);
}

static void read_metadata(struct xe_eudebug_debugger *d,
			  uint64_t client_handle,
			  uint64_t metadata_handle,
			  uint64_t type,
			  uint64_t len)
{
	struct drm_xe_eudebug_read_metadata rm = {
		.client_handle = client_handle,
		.metadata_handle = metadata_handle,
		.size = len,
	};
	struct online_debug_data *data = d->ptr;
	uint64_t *metadata;

	metadata = malloc(len);
	igt_assert(metadata);

	rm.ptr = to_user_pointer(metadata);
	igt_assert_eq(igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_READ_METADATA, &rm), 0);

	pthread_mutex_lock(&data->mutex);
	switch (type) {
	case DRM_XE_DEBUG_METADATA_ELF_BINARY:
		data->bb_offset = metadata[0];
		data->bb_size = metadata[1];
		break;
	case DRM_XE_DEBUG_METADATA_PROGRAM_MODULE:
		data->target_offset = metadata[0];
		data->target_size = metadata[1];
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&data->mutex);

	free(metadata);
}

static void create_metadata_trigger(struct xe_eudebug_debugger *d, struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_metadata *em = (void *)e;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE)
		read_metadata(d, em->client_handle, em->metadata_handle, em->type, em->len);
}

static void overwrite_immediate_value_in_common_target_write(int vm_fd, uint64_t offset,
							     uint32_t old_val, uint32_t new_val)
{
	uint64_t addr = offset;
	int vals_changed = 0;
	uint32_t val;

	while (vals_changed < 4) {
		igt_assert_eq(pread(vm_fd, &val, sizeof(uint32_t), addr), sizeof(uint32_t));
		if (val == old_val) {
			igt_debug("val_before_write[%d]: %08x\n", vals_changed, val);
			igt_assert_eq(pwrite(vm_fd, &new_val, sizeof(uint32_t), addr),
				      sizeof(uint32_t));
			igt_assert_eq(pread(vm_fd, &val, sizeof(uint32_t), addr),
				      sizeof(uint32_t));
			igt_debug("val_before_fsync[%d]: %08x\n", vals_changed, val);
			fsync(vm_fd);
			igt_assert_eq(pread(vm_fd, &val, sizeof(uint32_t), addr),
				      sizeof(uint32_t));
			igt_debug("val_after_fsync[%d]: %08x\n", vals_changed, val);
			igt_assert_eq_u32(val, new_val);
			vals_changed++;
		}
		addr += sizeof(uint32_t);
	}
}

static void eu_attention_resume_caching_trigger(struct xe_eudebug_debugger *d,
						struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *)e;
	struct online_debug_data *data = d->ptr;
	struct dim_t s_dim = surface_dimensions(data->thread_count);
	uint32_t *kernel_offset = &data->kernel_offset;
	int *counter = &data->att_event_counter;
	int val;
	uint32_t instr_usdw;
	struct gpgpu_shader *kernel;
	const uint32_t breakpoint_bit = 1 << 30;
	struct gpgpu_shader *shader_preamble;
	struct gpgpu_shader *shader_write_instr;
	const unsigned int instruction_count =
			caching_get_instruction_count(d->master_fd, s_dim.x, d->flags);
	uint64_t seqno = 0;
	int ret;

	shader_preamble = gpgpu_shader_create(d->master_fd);
	gpgpu_shader__write_dword(shader_preamble, SHADER_CANARY, 0);
	gpgpu_shader__nop(shader_preamble);
	gpgpu_shader__breakpoint(shader_preamble);

	shader_write_instr = gpgpu_shader_create(d->master_fd);
	gpgpu_shader__common_target_write_u32(shader_write_instr, 0, 0);

	if (!*kernel_offset) {
		kernel = get_shader(data);
		*kernel_offset = find_kernel_in_bb(kernel, data);
		gpgpu_shader_destroy(kernel);
	}

	/* set breakpoint on next write instruction */
	if (*counter < instruction_count) {
		igt_assert_eq(pread(data->vm_fd, &instr_usdw, sizeof(instr_usdw),
				    data->bb_offset + *kernel_offset + shader_preamble->size * 4 +
				    shader_write_instr->size * 4 * *counter),
				    sizeof(instr_usdw));
		instr_usdw |= breakpoint_bit;
		igt_assert_eq(pwrite(data->vm_fd, &instr_usdw, sizeof(instr_usdw),
				     data->bb_offset + *kernel_offset + shader_preamble->size * 4 +
				     shader_write_instr->size * 4 * *counter),
				     sizeof(instr_usdw));
		fsync(data->vm_fd);
	}

	/* restore current instruction */
	if (*counter && *counter <= instruction_count)
		overwrite_immediate_value_in_common_target_write(data->vm_fd,
								 data->bb_offset + *kernel_offset +
								 shader_preamble->size * 4 +
								 shader_write_instr->size * 4 * (*counter - 1),
								 CACHING_POISON_VALUE,
								 CACHING_VALUE(*counter - 1));

	/* poison next instruction */
	if (*counter < instruction_count)
		overwrite_immediate_value_in_common_target_write(data->vm_fd,
								 data->bb_offset + *kernel_offset +
								 shader_preamble->size * 4 +
								 shader_write_instr->size * 4 * *counter,
								 CACHING_VALUE(*counter),
								 CACHING_POISON_VALUE);

	gpgpu_shader_destroy(shader_write_instr);
	gpgpu_shader_destroy(shader_preamble);

	/* check surface at each breakpoint that is after write instruction */
	if (*counter > 1 && *counter <= instruction_count + 1)
		for (int i = 0; i < data->target_size; i += sizeof(uint32_t)) {
			igt_assert_eq(pread(data->vm_fd, &val, sizeof(val),
					    data->target_offset + i), sizeof(val));
			igt_assert_f(val != CACHING_POISON_VALUE,
				     "Poison value found at %04d!\n", i);
		}

	ret = __eu_ctl(d->fd, att->client_handle, att->exec_queue_handle, att->lrc_handle,
		       att->bitmask, &att->bitmask_size, DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME,
		       &seqno);

	/*
	 * XXX: build a better sync between workload lifetime vs resume.
	 *
	 * Right now, it is possible to get attention after the workload has vanished - in result,
	 * eu_ctl above fails. Band-aid it by checking the eu_ctl return value only n times it is
	 * actually expected - that is, instruction_count of writes + 2 nops.
	 */
	if (*counter < instruction_count + 2)
		igt_assert_eq(ret, 0);

	(*counter)++;
}

static struct intel_bb *xe_bb_create_on_offset(int fd, uint32_t exec_queue, uint32_t vm,
					       uint64_t offset, uint32_t size, uint64_t region)
{
	struct intel_bb *ibb;

	ibb = intel_bb_create_with_context_in_region(fd, exec_queue, vm, NULL, size, region);

	/* update intel bb offset */
	intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset, ibb->size);
	intel_bb_add_object(ibb, ibb->handle, ibb->size, offset, ibb->alignment, false);
	ibb->batch_offset = offset;

	return ibb;
}

static size_t get_bb_size(int fd, struct gpgpu_shader *shader)
{
	size_t shader_size = shader->size * sizeof(uint32_t);

	return ALIGN(shader_size, PAGE_SIZE) + xe_cs_prefetch_size(fd);
}

static uint64_t get_memory_region(int fd, int flags, int region_bitmask)
{
	flags &= region_bitmask;

	if (flags & BB_IN_SRAM || flags & TARGET_IN_SRAM)
		return system_memory(fd);
	if (flags & BB_IN_VRAM || flags & TARGET_IN_VRAM)
		return vram_memory(fd, 0);
	return vram_if_possible(fd, 0);
}

static void run_online_client(struct xe_eudebug_client *c)
{
	int threads;
	const uint64_t target_offset = 0x1a000000;
	const uint64_t bb_offset = 0x1b000000;
	size_t bb_size;
	struct online_debug_data *data = c->ptr;
	struct drm_xe_engine_class_instance hwe = data->hwe;
	struct drm_xe_ext_set_property ext = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
		.value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE,
	};
	struct drm_xe_exec_queue_create create = {
		.instances = to_user_pointer(&hwe),
		.width = 1,
		.num_placements = 1,
		.extensions = c->flags & DISABLE_DEBUG_MODE ? 0 : to_user_pointer(&ext)
	};
	struct dim_t w_dim;
	struct dim_t s_dim;
	struct timespec ts = { };
	struct gpgpu_shader *sip, *shader;
	uint32_t metadata_id[2];
	uint64_t *metadata[2];
	struct intel_bb *ibb;
	struct intel_buf *buf;
	uint32_t *ptr;
	int fd, vm_flags;

	metadata[0] = calloc(2, sizeof(**metadata));
	metadata[1] = calloc(2, sizeof(**metadata));
	igt_assert(metadata[0]);
	igt_assert(metadata[1]);

	fd = xe_eudebug_client_open_driver(c);

	threads = data->thread_count;
	w_dim = walker_dimensions(threads);
	s_dim = surface_dimensions(threads);

	shader = get_shader(data);
	bb_size = get_bb_size(fd, shader);

	/* Additional memory for steering control */
	if (c->flags & SHADER_LOOP || c->flags & SHADER_SINGLE_STEP || c->flags & SHADER_PAGEFAULT)
		s_dim.y++;
	/* Additional memory for caching check */
	if ((c->flags & SHADER_CACHING_SRAM) || (c->flags & SHADER_CACHING_VRAM))
		s_dim.y += caching_get_instruction_count(fd, s_dim.x, c->flags);
	buf = create_uc_buf(fd, s_dim.x, s_dim.y,
			    get_memory_region(fd, c->flags, TARGET_REGION_BITMASK));

	buf->addr.offset = target_offset;

	metadata[0][0] = bb_offset;
	metadata[0][1] = bb_size;
	metadata[1][0] = target_offset;
	metadata[1][1] = buf->size;
	metadata_id[0] = xe_eudebug_client_metadata_create(c, fd, DRM_XE_DEBUG_METADATA_ELF_BINARY,
							   2 * sizeof(**metadata), metadata[0]);
	metadata_id[1] = xe_eudebug_client_metadata_create(c, fd,
							   DRM_XE_DEBUG_METADATA_PROGRAM_MODULE,
							   2 * sizeof(**metadata), metadata[1]);

	vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	vm_flags |= c->flags & (SHADER_PAGEFAULT | FAULTABLE_VM) ?
			DRM_XE_VM_CREATE_FLAG_FAULT_MODE : 0;

	create.vm_id = xe_eudebug_client_vm_create(c, fd, vm_flags, 0);

	xe_eudebug_client_exec_queue_create(c, fd, &create);

	ibb = xe_bb_create_on_offset(fd, create.exec_queue_id, create.vm_id, bb_offset, bb_size,
				     get_memory_region(fd, c->flags, BB_REGION_BITMASK));
	intel_bb_set_lr_mode(ibb, true);

	sip = get_sip(data);

	igt_nsec_elapsed(&ts);
	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, sip, 0, 0);

	gpgpu_shader_destroy(sip);
	gpgpu_shader_destroy(shader);

	intel_bb_sync(ibb);

	if (c->flags & TRIGGER_RECONNECT)
		xe_eudebug_client_wait_stage(c, DEBUGGER_REATTACHED);
	else
		/* Make sure it wasn't the timeout. */
		igt_assert(igt_nsec_elapsed(&ts) < XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC);

	if (!(c->flags & DO_NOT_EXPECT_CANARIES)) {
		ptr = xe_bo_mmap_ext(fd, buf->handle, buf->size, PROT_READ);
		data->thread_hit_count = count_canaries_neq(ptr, w_dim, 0);
		igt_assert_f(data->thread_hit_count, "No canaries found, nothing executed?\n");

		if ((c->flags & SHADER_BREAKPOINT || c->flags & TRIGGER_RESUME_SET_BP ||
		     c->flags & SHADER_N_NOOP_BREAKPOINT) && !(c->flags & DISABLE_DEBUG_MODE)) {
			uint32_t aip = ptr[0];

			igt_assert_f(aip != SHADER_CANARY,
				     "Workload executed but breakpoint not hit!\n");
			igt_assert_eq(count_canaries_eq(ptr, w_dim, aip), data->thread_hit_count);
			igt_debug("Breakpoint hit in %d threads, AIP=0x%08x\n",
				  data->thread_hit_count,
				  aip);
		}

		munmap(ptr, buf->size);
	}

	intel_bb_destroy(ibb);

	xe_eudebug_client_exec_queue_destroy(c, fd, &create);
	xe_eudebug_client_vm_destroy(c, fd, create.vm_id);

	xe_eudebug_client_metadata_destroy(c, fd, metadata_id[0], DRM_XE_DEBUG_METADATA_ELF_BINARY,
					   2 * sizeof(**metadata));
	xe_eudebug_client_metadata_destroy(c, fd, metadata_id[1],
					   DRM_XE_DEBUG_METADATA_PROGRAM_MODULE,
					   2 * sizeof(**metadata));

	intel_buf_destroy(buf);

	xe_eudebug_client_close_driver(c, fd);
}

static bool intel_gen_has_lockstep_eus(int fd)
{
	const uint32_t id = intel_get_drm_devid(fd);

	/*
	 * Lockstep (or in some parlance, fused) EUs are pair of EUs
	 * that work in sync, supposedly same clock and same control flow.
	 * Thus for attentions, if the control has breakpoint, both will be
	 * excepted into SIP. In this level, the hardware has only one attention
	 * thread bit for units. PVC is the first one without lockstepping.
	 */
	return !(intel_graphics_ver(id) == IP_VER(12, 60) || intel_gen(id) >= 20);
}

static int query_attention_bitmask_size(int fd, int gt)
{
	uint32_t threads_per_eu = xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_NUM_THREADS_PER_EU);
	struct drm_xe_query_topology_mask *c_dss = NULL, *g_dss = NULL, *eu_per_dss = NULL;
	struct drm_xe_query_topology_mask *topology, *topo;
	uint32_t size;
	int i, max_eu_count;

	topology = xe_query_device(fd, DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, &size);

	xe_for_each_topology_mask(topology, size, topo) {
		if (topo->gt_id != gt)
			continue;

		if (topo->type == DRM_XE_TOPO_DSS_GEOMETRY)
			g_dss = topo;
		else if (topo->type == DRM_XE_TOPO_DSS_COMPUTE)
			c_dss = topo;
		else if (topo->type == DRM_XE_TOPO_EU_PER_DSS ||
			 topo->type == DRM_XE_TOPO_SIMD16_EU_PER_DSS)
			eu_per_dss = topo;
	}

	igt_assert(g_dss && c_dss && eu_per_dss);
	igt_assert_eq_u32(c_dss->num_bytes, g_dss->num_bytes);

	for (i = 0; i < c_dss->num_bytes; i++)
		c_dss->mask[i] |= g_dss->mask[i];

	max_eu_count = igt_bitmap_fls(c_dss->mask, c_dss->num_bytes * 8) *
		       igt_bitmap_hweight(eu_per_dss->mask, eu_per_dss->num_bytes * 8);

	if (intel_gen_has_lockstep_eus(fd))
		max_eu_count /= 2;

	free(topology);

	return max_eu_count * DIV_ROUND_UP(threads_per_eu, 8);
}

static struct drm_xe_eudebug_event_exec_queue *
match_attention_with_exec_queue(struct xe_eudebug_event_log *log,
				struct drm_xe_eudebug_event_eu_attention *ea)
{
	struct drm_xe_eudebug_event_exec_queue *ee;
	struct drm_xe_eudebug_event *event = NULL, *current = NULL, *matching_destroy = NULL;
	int lrc_idx;

	xe_eudebug_for_each_event(event, log) {
		if (event->type == DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE &&
		    event->flags == DRM_XE_EUDEBUG_EVENT_CREATE) {
			ee = (struct drm_xe_eudebug_event_exec_queue *)event;

			if (ee->exec_queue_handle != ea->exec_queue_handle)
				continue;

			if (ee->client_handle != ea->client_handle)
				continue;

			for (lrc_idx = 0; lrc_idx < ee->width; lrc_idx++) {
				if (ee->lrc_handle[lrc_idx] == ea->lrc_handle)
					break;
			}

			if (lrc_idx >= ee->width) {
				igt_debug("No matching lrc handle within matching exec_queue!");
				continue;
			}

			/* event logs are sorted, every found next would not be present. */
			if (ea->base.seqno < ee->base.seqno)
				break;

			/* sanity check whether attention did
			 * not appear yet on already destroyed exec_queue
			 */
			current = event;
			xe_eudebug_for_each_event(current, log) {
				if (current->type == DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE &&
				    current->flags == DRM_XE_EUDEBUG_EVENT_DESTROY) {
					uint8_t offset = sizeof(struct drm_xe_eudebug_event);

					if (memcmp((uint8_t *)current + offset,
						   (uint8_t *)event + offset,
						   current->len - offset) == 0) {
						matching_destroy = current;
					}
				}
			}

			if (!matching_destroy || ea->base.seqno > matching_destroy->seqno)
				continue;

			return ee;
		}
	}

	return NULL;
}

static void online_session_check(struct xe_eudebug_session *s, int flags)
{
	struct drm_xe_eudebug_event_eu_attention *ea = NULL;
	struct drm_xe_eudebug_event_pagefault *pf = NULL;
	struct drm_xe_eudebug_event *event = NULL;
	struct online_debug_data *data = s->client->ptr;
	bool expect_exception = flags & DISABLE_DEBUG_MODE ? false : true;
	int sum = 0;
	int bitmask_size;
	int pagefault_threads = 0;

	xe_eudebug_session_check(s, true, XE_EUDEBUG_FILTER_EVENT_VM_BIND |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);

	bitmask_size = query_attention_bitmask_size(s->debugger->master_fd, data->hwe.gt_id);

	xe_eudebug_for_each_event(event, s->debugger->log) {
		if (event->type == DRM_XE_EUDEBUG_EVENT_EU_ATTENTION) {
			ea = (struct drm_xe_eudebug_event_eu_attention *)event;

			igt_assert(event->flags == DRM_XE_EUDEBUG_EVENT_STATE_CHANGE);
			igt_assert_eq(ea->bitmask_size, bitmask_size);
			sum += igt_bitmap_hweight(ea->bitmask, bitmask_size * 8);
			igt_assert(match_attention_with_exec_queue(s->debugger->log, ea));
		} else if (event->type == DRM_XE_EUDEBUG_EVENT_PAGEFAULT) {
			uint32_t after_offset = bitmask_size / sizeof(uint32_t);
			uint32_t resolved_offset = bitmask_size / sizeof(uint32_t) * 2;
			uint32_t *ptr = NULL;

			pf = igt_container_of(event, pf, base);
			ptr = (uint32_t *) pf->bitmask;
			igt_assert_eq(pf->bitmask_size, bitmask_size * 3);
			pagefault_threads += eu_attentions_xor_count(ptr + after_offset,
								     ptr + resolved_offset,
								     bitmask_size);
		}
	}

	/*
	 * We can expect attention to sum up only
	 * if we have a breakpoint set and we resume all threads always.
	 */
	if (flags == SHADER_BREAKPOINT || flags == TRIGGER_UFENCE_SET_BREAKPOINT)
		igt_assert_eq(sum, data->thread_hit_count);

	if (expect_exception)
		igt_assert(sum > 0);
	else
		igt_assert(sum == 0);

	if (flags & SHADER_PAGEFAULT)
		igt_assert(pagefault_threads > 0);

	if (flags & SHADER_PAGEFAULT_ONE_OF_MANY) {
		igt_assert_eq(pagefault_threads, 1);
		igt_assert_eq(data->thread_hit_count, 1);
	}
}

static void ufence_ack_trigger(struct xe_eudebug_debugger *d,
			       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE)
		xe_eudebug_ack_ufence(d->fd, ef);
}

static void ufence_ack_set_bp_trigger(struct xe_eudebug_debugger *d,
				      struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;
	struct online_debug_data *data = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		set_breakpoint_once(d, data);
		xe_eudebug_ack_ufence(d->fd, ef);
	}
}

static uint32_t attn_to_sr0_0(struct online_debug_data *data, int att_nr)
{
	uint32_t tid, eu, dss, sl, ss;
	bool extended = data->num_threads_per_eu > 8;

	/* Calculate dss/eu/tid from attention number, Bspec: 56831, 73459. */
	/* Return sr0_0 register corresponding fields, Bspec: 56623. */
	tid = (att_nr & 7) | (extended ? (att_nr & 64) >> 3 : 0);
	eu = (att_nr >> 3) & 7;
	dss = att_nr >> (extended ? 7 : 6);
	ss = dss % data->max_subslices_per_slice;
	sl = dss / data->max_subslices_per_slice;
	return tid + (eu << 4) + (ss << 8) + (sl << (extended ? 14 : 11));
}

static uint32_t get_thread_space_address(struct online_debug_data *data, int thread)
{
	int x = thread % data->w_dim.x, y = thread / data->w_dim.x;

	return data->target_offset + 4 * (y * ALIGN(data->w_dim.x, data->w_dim.alignment) + x);
}

static void pagefault_trigger(struct xe_eudebug_debugger *d,
			      struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_pagefault *pf = igt_container_of(e, pf, base);
	struct online_debug_data *data = d->ptr;
	uint32_t attn_size = pf->bitmask_size / 3;
	int attn_size_as_u32 = attn_size / sizeof(uint32_t);
	uint32_t *ptr = (uint32_t *) pf->bitmask;
	uint32_t *ptrs[3] = {ptr, ptr + attn_size_as_u32, ptr + 2 * attn_size_as_u32};
	const char * const name[3] = {"before", "after", "resolved"};
	int threads[3], pagefault_threads, idx;
	uint32_t sr0_0, offset;

	for (idx = 0; idx < 3; idx++)
		threads[idx] = igt_bitmap_hweight(ptrs[idx], attn_size * 8);

	pagefault_threads = eu_attentions_xor_count(ptrs[1], ptrs[2], attn_size);

	igt_debug("EVENT[%llu] pagefault; threads[before=%d, after=%d, "
		  "resolved=%d, pagefault=%d] "
		  "client[%llu], exec_queue[%llu], lrc[%llu], bitmask_size[%d], "
		  "pagefault_address[0x%llx]\n",
		  pf->base.seqno, threads[0], threads[1], threads[2],
		  pagefault_threads, pf->client_handle, pf->exec_queue_handle,
		  pf->lrc_handle, pf->bitmask_size,
		  pf->pagefault_address);

	for (idx = 0; idx < 3; idx++) {
		igt_debug("=== Attentions %s ===\n", name[idx]);

		for (uint32_t i = 0; i < attn_size_as_u32; i += 2)
			igt_debug("bitmask[%d] = 0x%08x%08x\n", i / 2,
				  ptrs[idx][i], ptrs[idx][i + 1]);
	}

	igt_assert(pagefault_threads > 0);
	igt_assert_eq_u64(pf->pagefault_address, BAD_OFFSET);

	if (!(data->flags & SHADER_PAGEFAULT_ONE_OF_MANY))
		return;

	offset = get_thread_space_address(data, data->pf_thread_number);

	igt_for_milliseconds(500) {
		igt_assert_eq(pread(data->vm_fd, &sr0_0, sizeof(sr0_0), offset), sizeof(sr0_0));
		if (sr0_0)
			break;
		usleep(1000);
	}
	sr0_0 &= 0xffff; /* we need only thread coords */

	for (uint32_t att_dw = 0; att_dw < attn_size_as_u32; att_dw++) {
		uint32_t att_sr0_0, att_mask = ~ptrs[1][att_dw] & ptrs[2][att_dw];

		for (int att_nr, att_bit = 0; att_bit < BITS_PER_TYPE(att_mask); ++att_bit) {
			if (!(att_mask & (1ULL << att_bit)))
				continue;
			att_nr = 32 * att_dw + att_bit;
			att_sr0_0 = attn_to_sr0_0(data, att_nr);
			if (att_sr0_0 == sr0_0) {
				igt_debug("Thread%d: matched pagefault, attn=%#x, sr0_0=%#x\n",
					  data->pf_thread_number, att_nr, sr0_0);
				++data->thread_hit_count;
			} else {
				igt_debug("Thread%d: unmatched pagefault, attn=%#x, th_sr0_0=%#x, attn_sr0_0=%#x\n",
					  data->pf_thread_number, att_nr, sr0_0, att_sr0_0);
			}
		}
	}
}

/**
 * SUBTEST: basic-breakpoint
 * Functionality: EU attention event
 * Description:
 *	Check whether KMD sends attention events
 *	for workload in debug mode stopped on breakpoint.
 *
 * SUBTEST: breakpoint-not-in-debug-mode
 * Functionality: EU attention event
 * Description:
 *	Check whether KMD resets the GPU when it spots an attention
 *	coming from workload not in debug mode.
 *
 * SUBTEST: stopped-thread
 * Functionality: EU attention event
 * Description:
 *	Hits breakpoint on runalone workload and
 *	reads attention for fixed time.
 *
 * SUBTEST: resume-%s
 * Functionality: EU control
 * Description:
 *	Workload stopped on a breakpoint is resumed
 *	with granularity of %arg[1].
 *
 *
 * arg[1]:
 *
 * @one:	one thread
 * @dss:	threads running on one subslice
 */
static void test_basic_online(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s, s->flags);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: set-breakpoint
 * Functionality: dynamic breakpoint
 * Description:
 *	Checks for attention after setting a dynamic breakpoint in the ufence event.
 *
 * SUBTEST: set-breakpoint-faultable
 * Functionality: dynamic breakpoint with FAULTABLE_VM
 * Description:
 *	Faultable variation of test set-breakpoint.
 */

static void test_set_breakpoint_online(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	igt_require(!(flags & FAULTABLE_VM) || !xe_supports_faults(fd));

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_set_bp_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s, s->flags);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: set-breakpoint-sigint-debugger
 * Functionality: SIGINT
 * Description:
 *	A variant of set-breakpoint that sends SIGINT to the debugger thread with random timing
 *	and checks if nothing breaks, exercising the scenario multiple times.
 */
static void test_set_breakpoint_online_sigint_debugger(int fd,
						       struct drm_xe_engine_class_instance *hwe,
						       int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;
	struct timespec ts = { };
	int loop_count = 0;
	uint64_t sleep_time;
	uint64_t set_breakpoint_time;
	uint64_t max_sleep_time;
	uint64_t events_max = 0;
	int sigints_during_test = 0;

	/*
	 * Measure the average time required for basic set-breakpoint variant,
	 * so sleep_time range is correct.
	 */
	igt_nsec_elapsed(&ts);
	for (int i = 0; i < 10; i++)
		test_set_breakpoint_online(fd, hwe, SHADER_NOP | TRIGGER_UFENCE_SET_BREAKPOINT);
	set_breakpoint_time = igt_nsec_elapsed(&ts) / (NSEC_PER_MSEC / USEC_PER_MSEC) / 10;
	igt_info("Average set-breakpoint execution time: %" PRIu64 " us\n", set_breakpoint_time);
	max_sleep_time = set_breakpoint_time * 11 / 10;
	igt_info("Maximum sleep_time: %" PRIu64 " us\n", max_sleep_time);

	/* Skip checking canaries as they can be invalid due to signal timing */
	flags |= DO_NOT_EXPECT_CANARIES;

	ts = (struct timespec) { };
	igt_nsec_elapsed(&ts);

	while (igt_seconds_elapsed(&ts) < 60) {
		uint64_t event_count;

		sleep_time = rand() % max_sleep_time;
		igt_debug("Loop %d: SIGINT after %" PRIu64 " us\n", loop_count, sleep_time);

		data = online_debug_data_create(fd, hwe, flags);
		s = xe_eudebug_session_create(fd, run_online_client, flags, data);
		s->client->allow_dead_client = true;
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
						open_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
						exec_queue_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM,
						vm_open_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
						create_metadata_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
						ufence_ack_set_bp_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
						eu_attention_resume_trigger);

		igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
		xe_eudebug_debugger_start_worker(s->debugger);
		igt_assert_eq(READ_ONCE(s->debugger->event_count), 0);
		xe_eudebug_client_start(s->client);

		/* Sample max events without SIGINT */
		if (!loop_count++)
			xe_eudebug_client_wait_done(s->client);
		else
			usleep(sleep_time);

		event_count = READ_ONCE(s->debugger->event_count);
		if (event_count > events_max)
			events_max = event_count;
		else if (event_count > 0 && event_count < events_max)
			sigints_during_test++;

		/*
		 * Issue some SIGTERM signals in quick succession before SIGINT
		 * to raise the odds of hitting the ioctl
		 */
		for (int i = 0; i < SIGTERM_COUNT; i++) {
			xe_eudebug_debugger_kill(s->debugger, SIGTERM);
			usleep(rand() % 1000);
		}
		xe_eudebug_debugger_kill(s->debugger, SIGINT);
		/* Don't close debugger fd before it dies */
		while (!s->debugger->handled_sigint)
			usleep(1000);
		close(s->debugger->fd);

		igt_assert_eq(READ_ONCE(s->debugger->worker_state), DEBUGGER_WORKER_ACTIVE);
		WRITE_ONCE(s->debugger->worker_state, DEBUGGER_WORKER_INACTIVE);

		xe_eudebug_client_wait_done(s->client);

		xe_eudebug_event_log_print(s->debugger->log, true);
		xe_eudebug_event_log_print(s->client->log, true);

		xe_eudebug_session_destroy(s);
		online_debug_data_destroy(data);
	}

	igt_info("%d correctly timed SIGINTs in %d loops\n", sigints_during_test, loop_count);
	igt_assert_lt(0, sigints_during_test);
}

static int getenv_int(const char *var, int def_val)
{
	char *env = getenv(var);

	return env ? atoi(env) : def_val;
}
/**
 * SUBTEST: pagefault-read
 * Functionality: page faults
 * Description:
 *     Check whether KMD sends pagefault event for workload in debug mode that
 *     triggers a read pagefault.
 *
 * SUBTEST: pagefault-write
 * Functionality: page faults
 * Description:
 *     Check whether KMD sends pagefault event for workload in debug mode that
 *     triggers a write pagefault.
 *
 * SUBTEST: pagefault-read-stress
 * Functionality: page faults
 * Description:
 *     Check whether KMD sends read pagefault event for workload in debug mode
 *     with many threads.
 *
 * SUBTEST: pagefault-write-stress
 * Functionality: page faults
 * Description:
 *     Check whether KMD sends write pagefault event for workload in debug mode
 *     with many threads.
 *
 * SUBTEST: pagefault-one-of-many
 * Description:
 *     Check whether read (EU thread's load instruction) pagefault memory
 *     exception handling reports correct thread, if only one thread causes exception
 *     and other threads are spinning.
 */
static void test_pagefault_online(int fd, struct drm_xe_engine_class_instance *hwe,
				  int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);
	if (flags & SHADER_PAGEFAULT_ONE_OF_MANY) {
		uint32_t max_ss, max_sl;

		data->flags |= DO_NOT_EXPECT_CANARIES;
		data->pf_thread_number = getenv_int("IGT_PF_THREAD_NUMBER", 0);
		data->num_threads_per_eu =
			xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_NUM_THREADS_PER_EU);

		max_ss = xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_MAX_SUBSLICE);
		if (!max_ss)
			max_ss = xe_hwconfig_lookup_value_u32(fd,
				INTEL_HWCONFIG_MAX_DUAL_SUBSLICES_SUPPORTED);
		max_sl = xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_MAX_SLICES_SUPPORTED);
		igt_debug("HWCONFIG: %d threads per EU, max %d (dual)subslices, max %d slices\n",
			  data->num_threads_per_eu, max_ss, max_sl);
		igt_assert(data->num_threads_per_eu && max_ss && max_sl);

		data->max_subslices_per_slice = DIV_ROUND_UP(max_ss, max_sl);
	}
	s = xe_eudebug_session_create(fd, run_online_client, data->flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_PAGEFAULT,
					pagefault_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s, s->flags);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: preempt-breakpoint
 * Functionality: EUdebug preemption timeout
 * Description:
 *	Verify that eu debugger disables preemption timeout to
 *	prevent reset of workload stopped on breakpoint.
 */
static void test_preemption(int fd, struct drm_xe_engine_class_instance *hwe)
{
	int flags = SHADER_BREAKPOINT | TRIGGER_RESUME_DELAYED;
	struct xe_eudebug_session *s;
	struct online_debug_data *data;
	struct xe_eudebug_client *other;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);
	other = xe_eudebug_client_create(fd, run_online_client, SHADER_NOP, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);

	xe_eudebug_client_start(s->client);
	sleep(1); /* make sure s->client starts first */
	xe_eudebug_client_start(other);

	xe_eudebug_client_wait_done(s->client);
	xe_eudebug_client_wait_done(other);

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_session_destroy(s);
	xe_eudebug_client_destroy(other);

	igt_assert_f(data->last_eu_control_seqno != 0,
		     "Workload with breakpoint has ended without resume!\n");

	online_debug_data_destroy(data);
}

/**
 * SUBTEST: reset-with-attention
 * Functionality: EUdebug preemption timeout
 * Description:
 *	Check whether GPU is usable after resetting with attention raised
 *	(stopped on breakpoint) by running the same workload again.
 */
static void test_reset_with_attention_online(int fd, struct drm_xe_engine_class_instance *hwe,
					     int flags)
{
	struct xe_eudebug_session *s1, *s2;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);
	s1 = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s1->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_reset_trigger);
	xe_eudebug_debugger_add_trigger(s1->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s1);
	xe_eudebug_session_destroy(s1);

	s2 = xe_eudebug_session_create(fd, run_online_client, flags, data);
	xe_eudebug_debugger_add_trigger(s2->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s2->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s2);

	online_session_check(s2, s2->flags);

	xe_eudebug_session_destroy(s2);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: interrupt-all
 * Functionality: EU control
 * Description:
 *	Schedules EU workload which should last about a few seconds, then
 *	interrupts all threads, checks whether attention event came, and
 *	resumes stopped threads back.
 *
 * SUBTEST: interrupt-all-set-breakpoint
 * Functionality: dynamic breakpoint
 * Description:
 *	Schedules EU workload which should last about a few seconds, then
 *	interrupts all threads, once attention event come it sets breakpoint on
 *	the very next instruction and resumes stopped threads back. It expects
 *	that every thread hits the breakpoint.
 *
 * SUBTEST: interrupt-all-set-breakpoint-faultable
 * Functionality: dynamic breakpoint with FAULTABLE_VM
 * Description:
 *	Faultable variation of test interrupt-all-set-breakpoint.
 */
static void test_interrupt_all(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	igt_require(!(flags & FAULTABLE_VM) || !xe_supports_faults(fd));

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	wait_for_workload_start(data);

	pthread_mutex_lock(&data->mutex);
	igt_assert(data->client_handle != -1);
	igt_assert(data->exec_queue_handle != -1);
	eu_ctl_interrupt_all(s->debugger->fd, data->client_handle,
			     data->exec_queue_handle, data->lrc_handle);
	pthread_mutex_unlock(&data->mutex);

	xe_eudebug_client_wait_done(s->client);

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(s->client->log, true);

	online_session_check(s, s->flags);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

static void reset_debugger_log(struct xe_eudebug_debugger *d)
{
	unsigned int max_size;
	char log_name[80];

	/* Don't pull the rug out from under an active debugger */
	igt_assert(d->target_pid == 0);

	max_size = d->log->max_size;
	strncpy(log_name, d->log->name, sizeof(d->log->name) - 1);
	log_name[79] = '\0';
	xe_eudebug_event_log_destroy(d->log);
	d->log = xe_eudebug_event_log_create(log_name, max_size);
}

/**
 * SUBTEST: interrupt-other-debuggable
 * Functionality: EU control
 * Description:
 *	Schedules EU workload in runalone mode with never ending loop, while
 *	it is not under debug, tries to interrupt all threads using the different
 *	client attached to debugger.
 *
 * SUBTEST: interrupt-other
 * Functionality: EU control
 * Description:
 *	Schedules EU workload with a never ending loop and, while it is not
 *	configured for debugging, tries to interrupt all threads using the client
 *	attached to debugger.
 */
static void test_interrupt_other(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct online_debug_data *data;
	struct online_debug_data *debugee_data;
	struct xe_eudebug_session *s;
	struct xe_eudebug_client *debugee;
	int debugee_flags = SHADER_LOOP | DO_NOT_EXPECT_CANARIES;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN, open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	wait_for_workload_start(data);

	xe_eudebug_debugger_detach(s->debugger);
	reset_debugger_log(s->debugger);

	debugee_data = online_debug_data_create(fd, hwe, flags);
	s->debugger->ptr = debugee_data;
	debugee = xe_eudebug_client_create(fd, run_online_client, debugee_flags, debugee_data);
	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, debugee), 0);
	xe_eudebug_client_start(debugee);

	igt_for_milliseconds(STARTUP_TIMEOUT_MS) {
		if (READ_ONCE(debugee_data->vm_fd) == -1 || READ_ONCE(debugee_data->target_size) == 0)
			continue;
	}

	pthread_mutex_lock(&debugee_data->mutex);
	igt_assert(debugee_data->client_handle != -1);
	igt_assert(debugee_data->exec_queue_handle != -1);

	/*
	 * Interrupting the other client should return invalid state
	 * as it is running in runalone mode
	 */
	igt_assert_eq(__eu_ctl(s->debugger->fd, debugee_data->client_handle,
		      debugee_data->exec_queue_handle, debugee_data->lrc_handle, NULL, 0,
		      DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL, NULL), -EINVAL);
	pthread_mutex_unlock(&debugee_data->mutex);

	xe_force_gt_reset_async(s->debugger->master_fd, debugee_data->hwe.gt_id);

	/* First client should complete because of the reset */
	xe_eudebug_client_wait_done(s->client);

	/* Check if second workload was started and is running */
	wait_for_workload_start(debugee_data);

	/* Terminate debugee and mark it as cleaned up */
	igt_assert_eq(kill(debugee->pid, 0), 0);
	igt_assert_eq(debugee->done, 0);
	kill(debugee->pid, SIGKILL);
	igt_assert_eq(waitpid(debugee->pid, NULL, 0), debugee->pid);
	debugee->done = 1;
	debugee->pid = 0;

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(debugee->log, true);

	/*
	 * Skip xe_eudebug_session_check() because we forcibly killed the debugee
	 * with SIGKILL, so its event log is incomplete and validation would fail.
	 */

	xe_eudebug_client_destroy(debugee);
	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
	online_debug_data_destroy(debugee_data);
}

/**
 * SUBTEST: tdctl-parameters
 * Functionality: EU control
 * Description:
 *	Schedules EU workload which should last about a few seconds, then
 *	checks negative scenarios of EU_THREADS ioctl usage, interrupts all threads,
 *	checks whether attention event came, and resumes stopped threads back.
 */
static void test_tdctl_parameters(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;
	uint32_t random_command;
	uint32_t bitmask_size = query_attention_bitmask_size(fd, hwe->gt_id);
	uint8_t *attention_bitmask = malloc(bitmask_size * sizeof(uint8_t));

	igt_assert(attention_bitmask);

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	wait_for_workload_start(data);

	pthread_mutex_lock(&data->mutex);
	igt_assert(data->client_handle != -1);
	igt_assert(data->exec_queue_handle != -1);
	igt_assert(data->lrc_handle != -1);

	/* fail on invalid lrc_handle */
	igt_assert(__eu_ctl(s->debugger->fd, data->client_handle,
			    data->exec_queue_handle, data->lrc_handle + 1,
			    attention_bitmask, &bitmask_size,
			    DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL, NULL) == -EINVAL);

	/* fail on invalid exec_queue_handle */
	igt_assert(__eu_ctl(s->debugger->fd, data->client_handle,
			    data->exec_queue_handle + 1, data->lrc_handle,
			    attention_bitmask, &bitmask_size,
			    DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL, NULL) == -EINVAL);

	/* fail on invalid client */
	igt_assert(__eu_ctl(s->debugger->fd, data->client_handle + 1,
			    data->exec_queue_handle, data->lrc_handle,
			    attention_bitmask, &bitmask_size,
			    DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL, NULL) == -EINVAL);

	/*
	 * bitmask size must be aligned to sizeof(u32) for all commands
	 * and be zero for interrupt all
	 */
	bitmask_size = sizeof(uint32_t) - 1;
	igt_assert(__eu_ctl(s->debugger->fd, data->client_handle,
			    data->exec_queue_handle, data->lrc_handle,
			    attention_bitmask, &bitmask_size,
			    DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED, NULL) == -EINVAL);
	bitmask_size = 0;

	/* fail on invalid command */
	random_command = random() | (DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME + 1);
	igt_assert(__eu_ctl(s->debugger->fd, data->client_handle,
			    data->exec_queue_handle, data->lrc_handle,
			    attention_bitmask, &bitmask_size, random_command, NULL) == -EINVAL);

	free(attention_bitmask);

	eu_ctl_interrupt_all(s->debugger->fd, data->client_handle,
			     data->exec_queue_handle, data->lrc_handle);
	pthread_mutex_unlock(&data->mutex);

	xe_eudebug_client_wait_done(s->client);

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(s->client->log, true);

	online_session_check(s, s->flags);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

static void eu_attention_debugger_detach_trigger(struct xe_eudebug_debugger *d,
						 struct drm_xe_eudebug_event *event)
{
	struct online_debug_data *data = d->ptr;
	uint64_t c_pid;
	int ret;

	c_pid = d->target_pid;

	/* Reset VM data so the re-triggered VM open handler works properly */
	data->vm_fd = -1;

	xe_eudebug_debugger_detach(d);

	/* Let the KMD scan function notice unhandled EU attention */
	if (!(d->flags & SHADER_N_NOOP_BREAKPOINT))
		sleep(1);

	/*
	 * New session that is created by EU debugger on reconnect restarts
	 * seqno, causing isses with log sorting. To avoid that, create
	 * a new event log.
	 */
	reset_debugger_log(d);

	ret = xe_eudebug_debugger_reattach(d, c_pid);
	igt_assert_eq(ret, 0);

	/* Discovery worker will replay events that have occurred, which leads to
	 * a vm event being sent and vm_open_trigger being re-run, which would lead
	 * to us trying to open a removed vm. Thus, remove this trigger from list.
	 */
	xe_eudebug_debugger_remove_trigger(d, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);

	/* Let the discovery worker discover resources */
	sleep(2);

	if (!(d->flags & SHADER_N_NOOP_BREAKPOINT))
		xe_eudebug_debugger_signal_stage(d, DEBUGGER_REATTACHED);
}

/**
 * SUBTEST: interrupt-reconnect
 * Functionality: reopen connection
 * Description:
 *	Schedules EU workload which should last about a few seconds,
 *	interrupts all threads and detaches debugger when attention is
 *	raised. The test checks if KMD resets the workload when there's
 *	no debugger attached and does the event playback on discovery.
 */
static void test_interrupt_reconnect(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct drm_xe_eudebug_event *e = NULL;
	struct online_debug_data *data;
	struct xe_eudebug_session *s;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debugger_detach_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	wait_for_workload_start(data);

	pthread_mutex_lock(&data->mutex);
	igt_assert(data->client_handle != -1);
	igt_assert(data->exec_queue_handle != -1);
	eu_ctl_interrupt_all(s->debugger->fd, data->client_handle,
			     data->exec_queue_handle, data->lrc_handle);
	pthread_mutex_unlock(&data->mutex);

	xe_eudebug_client_wait_done(s->client);

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(s->client->log, true);

	xe_eudebug_session_check(s, true, XE_EUDEBUG_FILTER_EVENT_VM_BIND |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);

	/* We expect workload reset, so no attention should be raised */
	xe_eudebug_for_each_event(e, s->debugger->log)
		igt_assert(e->type != DRM_XE_EUDEBUG_EVENT_EU_ATTENTION);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: single-step
 * Functionality: EU control
 * Description:
 *	Schedules EU workload with 16 nops after breakpoint, then single-steps
 *	through the shader, advances all threads each step, checking if all
 *	threads advanced every step.
 *
 * SUBTEST: single-step-one
 * Functionality: EU control
 * Description:
 *	Schedules EU workload with 16 nops after breakpoint, then single-steps
 *	through the shader, advances one thread each step, checking if one
 *	thread advanced every step. Due to the time constraint, only first two
 *	shader instructions after breakpoint are validated.
 */
static void test_single_step(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_single_step_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s, s->flags);
	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

static void eu_attention_debugger_ndetach_trigger(struct xe_eudebug_debugger *d,
						  struct drm_xe_eudebug_event *event)
{
	struct online_debug_data *data = d->ptr;
	static int debugger_detach_count;

	if (debugger_detach_count < (SHADER_LOOP_N - 1)) {
		/* Make sure the resume command was issued before detaching the debugger */
		if (data->last_eu_control_seqno > event->seqno)
			return;
		eu_attention_debugger_detach_trigger(d, event);
		debugger_detach_count++;
	} else {
		igt_debug("Reached Nth breakpoint hence preventing the debugger detach\n");
	}
}

/**
 * SUBTEST: debugger-reopen
 * Functionality: reopen connection
 * Description:
 *	Check whether the debugger is able to reopen the connection and
 *	capture the events of already running client.
 */
static void test_debugger_reopen(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);

	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debugger_ndetach_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

/**
 * SUBTEST: writes-caching-%s-bb-%s-target-%s
 * Functionality: cache coherency
 * Description:
 *	Write incrementing values to 2-page-long target surface, poisoning the data one breakpoint
 *	before each write instruction and restoring it when the poisoned instruction breakpoint
 *	is hit. Expect to never see poison values in target surface.
 *
 *
 * arg[1]:
 *
 * @sram:	Use page size of SRAM
 * @vram:	Use page size of VRAM
 *
 * arg[2]:
 *
 * @sram:	Batchbuffer in SRAM
 * @vram:	Batchbuffer in VRAM
 *
 * arg[3]:
 *
 * @sram:	Target surface in SRAM
 * @vram:	Target surface in VRAM
 */
static void test_caching(int fd, struct drm_xe_engine_class_instance *hwe, int flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	if (flags & SHADER_CACHING_VRAM || flags & BB_IN_VRAM || flags & TARGET_IN_VRAM)
		igt_skip_on_f(!xe_has_vram(fd), "Device does not have VRAM.\n");

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_caching_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s, s->flags);
	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

#define is_compute_on_gt(__e, __gt) (((__e)->engine_class == DRM_XE_ENGINE_CLASS_RENDER || \
				      (__e)->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) && \
				      (__e)->gt_id == (__gt))

struct xe_engine_list_entry {
	struct igt_list_head link;
	struct drm_xe_engine_class_instance *hwe;
};

#define MAX_TILES	2
static int find_suitable_engines(struct drm_xe_engine_class_instance **hwes,
				 int fd, bool many_tiles)
{
	struct xe_device *xe_dev;
	struct drm_xe_engine_class_instance *e;
	struct xe_engine_list_entry *en, *tmp;
	struct igt_list_head compute_engines[MAX_TILES];
	int gt_id;
	int tile_id, i, engine_count = 0, tile_count = 0;

	xe_dev = xe_device_get(fd);

	for (i = 0; i < MAX_TILES; i++)
		IGT_INIT_LIST_HEAD(&compute_engines[i]);

	xe_for_each_gt(fd, gt_id) {
		xe_for_each_engine(fd, e) {
			if (is_compute_on_gt(e, gt_id)) {
				tile_id = xe_dev->gt_list->gt_list[gt_id].tile_id;

				en = malloc(sizeof(struct xe_engine_list_entry));
				en->hwe = e;

				igt_list_add_tail(&en->link, &compute_engines[tile_id]);
			}
		}
	}

	for (i = 0; i < MAX_TILES; i++) {
		if (igt_list_empty(&compute_engines[i]))
			continue;

		if (many_tiles) {
			en = igt_list_first_entry(&compute_engines[i], en, link);
			hwes[engine_count++] = en->hwe;
			tile_count++;
		} else {
			if (igt_list_length(&compute_engines[i]) > 1) {
				igt_list_for_each_entry(en, &compute_engines[i], link)
					hwes[engine_count++] = en->hwe;
				break;
			}
		}
	}

	for (i = 0; i < MAX_TILES; i++) {
		igt_list_for_each_entry_safe(en, tmp, &compute_engines[i], link) {
			igt_list_del(&en->link);
			free(en);
		}
	}

	if (many_tiles)
		igt_require_f(tile_count > 1, "Mulit-tile scenario requires more tiles\n");

	return engine_count;
}

static uint64_t timespecs_diff_us(struct timespec *ts1, struct timespec *ts2)
{
	return (uint64_t)(fabs(igt_time_elapsed(ts1, ts2)) * USEC_PER_SEC);
}

/**
 * SUBTEST: breakpoint-many-sessions-single-tile
 * Functionality: multisession
 * Description:
 *	Schedules EU workload with preinstalled breakpoint on every compute engine
 *	available on the tile. Checks if the contexts hit breakpoint in sequence
 *	and resumes them.
 *
 * SUBTEST: breakpoint-many-sessions-tiles
 * Functionality: multisession multiTile
 * Description:
 *	Schedules EU workload with preinstalled breakpoint on selected compute
 *      engines, with one per tile. Checks if each context hit breakpoint and
 *      resumes them.
 */
static void test_many_sessions_on_tiles(int fd, bool multi_tile)
{
	int n = 0, flags = SHADER_BREAKPOINT | SHADER_MIN_THREADS;
	struct xe_eudebug_session **s;
	struct online_debug_data **data;
	struct drm_xe_engine_class_instance **hwe;
	struct drm_xe_eudebug_event_eu_attention *eus;
	uint64_t diff;
	int attempt_mask = 0, final_mask, should_break;
	int i;

	s = malloc(sizeof(*s) * xe_number_engines(fd));
	data = malloc(sizeof(*data) * xe_number_engines(fd));
	hwe = malloc(sizeof(*hwe) * xe_number_engines(fd));
	n = find_suitable_engines(hwe, fd, multi_tile);

	igt_require_f(n > 1, "Test requires at least two parallel compute engines!\n");

	for (i = 0; i < n; i++) {
		data[i] = online_debug_data_create(fd, hwe[i], flags);
		s[i] = xe_eudebug_session_create(fd, run_online_client, flags, data[i]);

		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
						eu_attention_debug_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
						save_first_exception_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
						ufence_ack_trigger);

		igt_assert_eq(xe_eudebug_debugger_attach(s[i]->debugger, s[i]->client), 0);

		xe_eudebug_debugger_start_worker(s[i]->debugger);
		xe_eudebug_client_start(s[i]->client);
	}

	final_mask = pow(2, n) - 1;
	igt_for_milliseconds(s[0]->client->timeout_ms) {
		if (attempt_mask == final_mask)
			break;

		for (i = 0; i < n; i++) {
			if (attempt_mask & BIT(i))
				continue;

			should_break = 0;

			pthread_mutex_lock(&(data[i]->mutex));
			if ((data[i]->exception_arrived.tv_sec |
			     data[i]->exception_arrived.tv_nsec) != 0) {
				attempt_mask |= BIT(i);
				should_break = 1;

				usleep(WORKLOAD_DELAY_US);
				eus = (struct drm_xe_eudebug_event_eu_attention *)data[i]->exception_event;
				eu_ctl_resume(s[i]->debugger->master_fd, s[i]->debugger->fd,
					      eus->client_handle, eus->exec_queue_handle,
					      eus->lrc_handle, eus->bitmask, eus->bitmask_size);
				free(eus);

			}
			pthread_mutex_unlock(&(data[i]->mutex));

			if (should_break)
				break;

		}
	}

	igt_assert_eq(attempt_mask, final_mask);

	for (i = 0; i < n - 1; i++) {
		diff = timespecs_diff_us(&data[i]->exception_arrived,
					 &data[i + 1]->exception_arrived);

		if (multi_tile)
			igt_assert_f(diff < WORKLOAD_DELAY_US,
				     "Expected to execute workloads concurrently. Actual delay: %" PRIu64 " us\n",
				     diff);
		else
			igt_assert_f(diff >= WORKLOAD_DELAY_US,
				     "Expected a serialization of workloads. Actual delay: %" PRIu64 " us\n",
				     diff);
	}

	for (i = 0; i < n; i++) {
		xe_eudebug_client_wait_done(s[i]->client);
		xe_eudebug_debugger_stop_worker(s[i]->debugger);

		xe_eudebug_event_log_print(s[i]->debugger->log, true);
		online_session_check(s[i], flags);

		xe_eudebug_session_destroy(s[i]);
		online_debug_data_destroy(data[i]);
	}

	free(s);
	free(data);
	free(hwe);
}

static struct drm_xe_engine_class_instance *pick_compute(int fd, int gt)
{
	struct drm_xe_engine_class_instance *hwe;
	int count = 0;

	xe_for_each_engine(fd, hwe)
		if (is_compute_on_gt(hwe, gt))
			count++;

	xe_for_each_engine(fd, hwe)
		if (is_compute_on_gt(hwe, gt) && rand() % count-- == 0)
			return hwe;

	return NULL;
}

static bool set_preempt_timeout_to_max(int fd, uint16_t engine_class, uint32_t *preempt_timeout)
{
	uint32_t preempt_timeout_max;

	if (!xe_sysfs_engine_class_get_property(fd, 0, engine_class, "preempt_timeout_max",
						&preempt_timeout_max))
		return false;

	return xe_sysfs_engine_class_set_property(fd, 0, engine_class, "preempt_timeout_us",
						  preempt_timeout_max, preempt_timeout);
}

static bool restore_preempt_timeout(int fd, uint16_t engine_class, uint32_t preempt_timeout)
{
	return xe_sysfs_engine_class_set_property(fd, 0, engine_class, "preempt_timeout_us",
						  preempt_timeout, NULL);
}

#define test_gt_render_or_compute(t, fd, __hwe) \
	igt_subtest_with_dynamic(t) \
		for (int gt = 0; (__hwe = pick_compute(fd, gt)); gt++) \
			igt_dynamic_f("%s%d", xe_engine_class_string(__hwe->engine_class), \
				      hwe->engine_instance)

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	bool was_enabled;
	int fd;
	uint16_t engine_class = 0xFFFF;
	uint32_t preempt_timeout = 0xFFFFFFFF;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		intel_allocator_multiprocess_start();
		igt_srandom();
		was_enabled = xe_eudebug_enable(fd, true);
	}

	test_gt_render_or_compute("basic-breakpoint", fd, hwe)
		test_basic_online(fd, hwe, SHADER_BREAKPOINT);

	test_gt_render_or_compute("preempt-breakpoint", fd, hwe)
		test_preemption(fd, hwe);

	test_gt_render_or_compute("set-breakpoint", fd, hwe)
		test_set_breakpoint_online(fd, hwe, SHADER_NOP | TRIGGER_UFENCE_SET_BREAKPOINT);

	test_gt_render_or_compute("set-breakpoint-faultable", fd, hwe)
		test_set_breakpoint_online(fd, hwe,
					   SHADER_NOP | TRIGGER_UFENCE_SET_BREAKPOINT | FAULTABLE_VM);

	test_gt_render_or_compute("set-breakpoint-sigint-debugger", fd, hwe)
		test_set_breakpoint_online_sigint_debugger(fd, hwe,
							   SHADER_NOP | TRIGGER_UFENCE_SET_BREAKPOINT);

	test_gt_render_or_compute("breakpoint-not-in-debug-mode", fd, hwe)
		test_basic_online(fd, hwe, SHADER_BREAKPOINT | DISABLE_DEBUG_MODE);

	test_gt_render_or_compute("stopped-thread", fd, hwe)
		test_basic_online(fd, hwe, SHADER_BREAKPOINT | TRIGGER_RESUME_DELAYED);

	test_gt_render_or_compute("resume-one", fd, hwe)
		test_basic_online(fd, hwe, SHADER_BREAKPOINT | TRIGGER_RESUME_ONE);

	test_gt_render_or_compute("resume-dss", fd, hwe)
		test_basic_online(fd, hwe, SHADER_BREAKPOINT | TRIGGER_RESUME_DSS);

	test_gt_render_or_compute("interrupt-all", fd, hwe)
		test_interrupt_all(fd, hwe, SHADER_LOOP);

	test_gt_render_or_compute("interrupt-other-debuggable", fd, hwe)
		test_interrupt_other(fd, hwe, SHADER_LOOP);

	igt_subtest_group() {
		test_gt_render_or_compute("interrupt-other", fd, hwe) {
			engine_class = hwe->engine_class;

			igt_skip_on(!set_preempt_timeout_to_max(fd, engine_class,
								&preempt_timeout));

			test_interrupt_other(fd, hwe, SHADER_LOOP | DISABLE_DEBUG_MODE);
		}

		igt_fixture() {
			if ((uint16_t)~engine_class && ~preempt_timeout)
				if (!restore_preempt_timeout(fd, engine_class, preempt_timeout))
					igt_warn("Cleanup of preempt_timeout failed!\n");
		}
	}

	test_gt_render_or_compute("interrupt-all-set-breakpoint", fd, hwe)
		test_interrupt_all(fd, hwe, SHADER_LOOP | TRIGGER_RESUME_SET_BP);

	test_gt_render_or_compute("interrupt-all-set-breakpoint-faultable", fd, hwe)
		test_interrupt_all(fd, hwe, SHADER_LOOP | TRIGGER_RESUME_SET_BP | FAULTABLE_VM);

	test_gt_render_or_compute("tdctl-parameters", fd, hwe)
		test_tdctl_parameters(fd, hwe, SHADER_LOOP);

	test_gt_render_or_compute("reset-with-attention", fd, hwe)
		test_reset_with_attention_online(fd, hwe, SHADER_BREAKPOINT);

	test_gt_render_or_compute("interrupt-reconnect", fd, hwe)
		test_interrupt_reconnect(fd, hwe, SHADER_LOOP | TRIGGER_RECONNECT);

	test_gt_render_or_compute("single-step", fd, hwe)
		test_single_step(fd, hwe, SHADER_SINGLE_STEP | SIP_SINGLE_STEP |
				 TRIGGER_RESUME_PARALLEL_WALK);

	test_gt_render_or_compute("single-step-one", fd, hwe)
		test_single_step(fd, hwe, SHADER_SINGLE_STEP | SIP_SINGLE_STEP |
				 TRIGGER_RESUME_SINGLE_WALK);

	test_gt_render_or_compute("debugger-reopen", fd, hwe)
		test_debugger_reopen(fd, hwe, SHADER_N_NOOP_BREAKPOINT);

	test_gt_render_or_compute("writes-caching-sram-bb-sram-target-sram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_SRAM | BB_IN_SRAM | TARGET_IN_SRAM);

	test_gt_render_or_compute("writes-caching-sram-bb-sram-target-vram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_SRAM | BB_IN_SRAM | TARGET_IN_VRAM);

	test_gt_render_or_compute("writes-caching-sram-bb-vram-target-sram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_SRAM | BB_IN_VRAM | TARGET_IN_SRAM);

	test_gt_render_or_compute("writes-caching-sram-bb-vram-target-vram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_SRAM | BB_IN_VRAM | TARGET_IN_VRAM);

	test_gt_render_or_compute("writes-caching-vram-bb-sram-target-sram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_VRAM | BB_IN_SRAM | TARGET_IN_SRAM);

	test_gt_render_or_compute("writes-caching-vram-bb-sram-target-vram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_VRAM | BB_IN_SRAM | TARGET_IN_VRAM);

	test_gt_render_or_compute("writes-caching-vram-bb-vram-target-sram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_VRAM | BB_IN_VRAM | TARGET_IN_SRAM);

	test_gt_render_or_compute("writes-caching-vram-bb-vram-target-vram", fd, hwe)
		test_caching(fd, hwe, SHADER_CACHING_VRAM | BB_IN_VRAM | TARGET_IN_VRAM);

	igt_subtest("breakpoint-many-sessions-single-tile")
		test_many_sessions_on_tiles(fd, false);

	igt_subtest("breakpoint-many-sessions-tiles")
		test_many_sessions_on_tiles(fd, true);

	test_gt_render_or_compute("pagefault-read", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_READ);
	test_gt_render_or_compute("pagefault-write", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_WRITE);
	test_gt_render_or_compute("pagefault-read-stress", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_READ | PAGEFAULT_STRESS_TEST);
	test_gt_render_or_compute("pagefault-write-stress", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_WRITE | PAGEFAULT_STRESS_TEST);
	test_gt_render_or_compute("pagefault-one-of-many", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_ONE_OF_MANY);

	igt_fixture() {
		xe_eudebug_enable(fd, was_enabled);

		intel_allocator_multiprocess_stop();
		drm_close_driver(fd);
	}
}
