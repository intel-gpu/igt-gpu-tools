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
#include <poll.h>
#include <sys/ioctl.h>

#include "xe/xe_eudebug.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "intel_pat.h"
#include "intel_mocs.h"
#include "gpgpu_shader.h"
#include <sys/wait.h>

#define SHADER_NOP			0
#define SHADER_BREAKPOINT		BIT(0)
#define SHADER_LOOP			BIT(1)
#define SHADER_SINGLE_STEP		BIT(2)
#define SIP_SINGLE_STEP			BIT(3)
#define DISABLE_DEBUG_MODE		BIT(4)
#define SHADER_N_NOOP_BREAKPOINT	BIT(5)
#define SHADER_CACHING_SRAM		BIT(6)
#define SHADER_CACHING_VRAM		BIT(7)
#define SHADER_MIN_THREADS		BIT(8)
#define DO_NOT_EXPECT_CANARIES		BIT(9)
#define BB_IN_SRAM			BIT(10)
#define BB_IN_VRAM			BIT(11)
#define TARGET_IN_SRAM			BIT(12)
#define TARGET_IN_VRAM			BIT(13)
#define DISABLE_EXCEPTIONS		BIT(14)
#define SHADER_PAGEFAULT_READ		BIT(15)
#define SHADER_PAGEFAULT_WRITE		BIT(16)
#define SHADER_PAGEFAULT_ATOMIC_READ	BIT(17)
#define SHADER_PAGEFAULT_ATOMIC_WRITE	BIT(18)
#define FAULTABLE_VM			BIT(19)
#define TRIGGER_UFENCE_SET_BREAKPOINT	BIT(24)
#define TRIGGER_RESUME_SINGLE_WALK	BIT(25)
#define TRIGGER_RESUME_PARALLEL_WALK	BIT(26)
#define TRIGGER_RECONNECT		BIT(27)
#define TRIGGER_RESUME_SET_BP		BIT(28)
#define TRIGGER_RESUME_DELAYED		BIT(29)
#define TRIGGER_RESUME_DSS		BIT(30)
#define TRIGGER_RESUME_ONE		BIT(31)
#define PAGEFAULT_STRESS_TEST		BIT(38)
#define SHADER_PAGEFAULT_ONE_OF_MANY	BIT(39)

#define SHADER_PAGEFAULT	(SHADER_PAGEFAULT_READ | SHADER_PAGEFAULT_WRITE |\
				 SHADER_PAGEFAULT_ATOMIC_READ | SHADER_PAGEFAULT_ATOMIC_WRITE |\
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

#define XE_EUDEBUG_DEFAULT_CACHING_TIMEOUT_SEC  60ULL

struct dim_t {
	uint32_t x;
	uint32_t y;
	uint32_t alignment;
};

struct online_debug_data {
	pthread_mutex_t mutex;
	/* client in */
	int drm_fd;
	struct drm_xe_engine_class_instance hwe;
	uint64_t flags;
	int thread_count;
	uint32_t gfx_ver;
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
	int thread_resumed;
	struct single_step {
		struct sip_arf_sso *arfs;
		uint32_t current_thread;
		uint32_t current_step;
		uint32_t threads_checked;
		uint64_t *thread_last_aip;
	} sso;
	bool acked;
};

struct sip_arf_dump {
	/*
	 * EU Thread's load/store has limitation on vector size variation.
	 * (D32 / v8) bspec: 72013
	 * dw0 - dw7
	 */
#ifndef BITRANGE
#define BITRANGE(start, end) (end - start + 1)
#endif
	/* bspec: 56624 */
	union {
		struct {
			uint32_t rsvd1:					BITRANGE(0, 9);
			uint32_t memory_exception_enable:		BITRANGE(10, 10);
			uint32_t oob_grf_translation_exception_enable:	BITRANGE(11, 11);
			uint32_t illegal_opcode_exception_enable:	BITRANGE(12, 12);
			uint32_t software_exception_enable:		BITRANGE(13, 13);
			uint32_t external_halt_exception_enable:	BITRANGE(14, 14);
			uint32_t breakpoint_enable:			BITRANGE(15, 15);
			uint32_t rsvd0:					BITRANGE(16, 22);
			uint32_t memory_exception_status:		BITRANGE(23, 23);
			uint32_t restore_exception_status:		BITRANGE(24, 24);
			uint32_t preemption_exception_status:		BITRANGE(25, 25);
			uint32_t force_exception_status:		BITRANGE(26, 26);
			uint32_t oob_grf_translation_exception_status:	BITRANGE(27, 27);
			uint32_t illegal_opcode_exception_status:	BITRANGE(28, 28);
			uint32_t software_exception_control:		BITRANGE(29, 29);
			uint32_t external_halt_exception_status:	BITRANGE(30, 30);
			uint32_t breakpoint_status:			BITRANGE(31, 31);
		} cr0_1; // dw0

		struct {
			uint32_t raw;
		} dw00;
	};

	union {
		struct {
			uint32_t rsvd0:					BITRANGE(0, 2);
			uint32_t aip_low:				BITRANGE(3, 31);
		} cr0_2; // dw1

		struct {
			uint32_t raw;
		} dw01;
	};

	union {
		struct {
			uint32_t aip_high:				BITRANGE(0, 31);
		} cr0_3;  // dw2

		struct {
			uint32_t raw;
		} dw02;
	};

	/* bspec: 56622 */
	union {
		struct {
			uint32_t slm_size:				BITRANGE(0, 3);
			uint32_t slm_offset:				BITRANGE(4, 12);
			uint32_t rsvd1:					BITRANGE(13, 16);
			uint32_t pagefault_exception_enable:		BITRANGE(17, 17);
			uint32_t rsvd0:					BITRANGE(18, 19);
			uint32_t number_of_barriers:			BITRANGE(20, 23);
			uint32_t barrier_id:				BITRANGE(24, 31);
		} msg0_1;  //dw3

		struct {
			uint32_t raw;
		} dw03;
	};

	/* bspec: 56630 */
	union {
		struct {
			uint32_t memory_exception_type:			BITRANGE(0, 2);
			uint32_t rsvd1:					BITRANGE(3, 3);
			uint32_t pagefault_status_subtype:		BITRANGE(4, 7);
			uint32_t rsvd0:					BITRANGE(8, 26);
			uint32_t sbid:					BITRANGE(27, 31);
		} dbg0_4; // dw4

		struct {
			uint32_t raw;
		} dw04;
	};

	union {
		struct {
			uint32_t thread_halted;
		} rsvd0; // dw5

		struct {
			uint32_t raw;
		} dw05;
	};

	union {
		struct {
			uint32_t thread_resume;
		} rsvd1; // dw6

		struct {
			uint32_t raw;
		} dw06;
	};

	/* bspec: 56623 */
	union {
		struct {
			uint32_t tid:					BITRANGE(0, 3);
			uint32_t euid:					BITRANGE(4, 6);
			uint32_t rsvd3:					BITRANGE(7, 7);
			uint32_t sub_slice_id:				BITRANGE(8, 11);
			uint32_t rsvd2:					BITRANGE(12, 13);
			uint32_t slice_id:				BITRANGE(14, 17);
			uint32_t rsvd1:					BITRANGE(18, 18);
			uint32_t priority:				BITRANGE(19, 22);
			uint32_t priority_class:			BITRANGE(23, 23);
			uint32_t ffid:					BITRANGE(24, 27);
			uint32_t rsvd0:					BITRANGE(28, 31);
		} sr0_0; // dw7

		struct {
			uint32_t raw;
		} dw07;
	};

};

/*
 * Breakpoint exception status bit for
 * Xe3: cr0.1 (bspec 56624)
 */
#define SSO_CTRL_BREAKPOINT_STATUS	BIT(31)
/*
 * Remaining exception statuses
 * Xe3: cr0.1 (bspec 56624)
 */
#define SSO_CTRL_EXCEPTION_STATUSES	0x7f800000

#define SSO_THREAD_STEP		0x3 /* resume with single-step on */
#define SSO_THREAD_RESUME	0x1 /* resume without single-step on */

/*
 * Single-step-one dedicated ARF structure
 * Fields populated by SIP for each stopped thread.
 */
struct sip_arf_sso {
	/* DW0: value 0xdead means thread is halted.*/
	uint32_t thread_halted;
	/*
	 * DW1: control register with exception statuses
	 * Xe3 bspec: 56624, cr0.1
	 * bit 31 - breakpoint status
	 * bits 23-31 exception statuses
	 */
	uint32_t exctrl;
	/* DW2 & 3:
	 * aip (l - low, h - high)
	 * For Xe3 bspec 56624
	 *   aipl - cr0.2
	 *   aiph - cr0.3
	 */
	uint32_t aipl;
	uint32_t aiph;

	/* DW4-6: reserved */
	uint32_t rsvd0;
	uint32_t rsvd1;
	uint32_t rsvd2;

	/*
	 * DW7:
	 * Before thread resume ioctl write to this field:
	 * - SSO_THREAD_STEP to continue single stepping
	 * - other non zero value to resume
	 * - 0 value will make tread jump back to sync.host.
	 */
	uint32_t resume;
};

static uint32_t get_thread_space_address(struct online_debug_data *data, int thread)
{
	int x = thread % data->w_dim.x, y = thread / data->w_dim.x;

	return 4 * (y * ALIGN(data->w_dim.x, data->w_dim.alignment) + x);
}

static uint32_t get_shared_space_address(struct online_debug_data *data)
{
	return get_thread_space_address(data, data->thread_count);
}

static void print_sip_arf_dump(struct sip_arf_dump *arf_dump, int x, int y)
{
	igt_debug("=========================================================\n");
	igt_debug("\tarf_dump[%d][%d].cr0_1\n", y, x);
	igt_debug("\t\tmemory_exception_enable: %d\n", arf_dump->cr0_1.memory_exception_enable);
	igt_debug("\t\toob_grf_translation_exception_enable: %d\n", arf_dump->cr0_1.oob_grf_translation_exception_enable);
	igt_debug("\t\tillegal_opcode_exception_enable: %d\n", arf_dump->cr0_1.illegal_opcode_exception_enable);
	igt_debug("\t\tsoftware_exception_enable: %d\n", arf_dump->cr0_1.software_exception_enable);
	igt_debug("\t\texternal_halt_exception_enable: %d\n", arf_dump->cr0_1.external_halt_exception_enable);
	igt_debug("\t\tbreakpoint_enable: %d\n", arf_dump->cr0_1.breakpoint_enable);
	igt_debug("\t\trestore_exception_status: %d\n", arf_dump->cr0_1.restore_exception_status);
	igt_debug("\t\tpreemption_exception_status: %d\n", arf_dump->cr0_1.preemption_exception_status);
	igt_debug("\t\tforce_exception_status: %d\n", arf_dump->cr0_1.force_exception_status);
	igt_debug("\t\toob_grf_translation_exception_status: %d\n", arf_dump->cr0_1.oob_grf_translation_exception_status);
	igt_debug("\t\tillegal_opcode_exception_status: %d\n", arf_dump->cr0_1.illegal_opcode_exception_status);
	igt_debug("\t\tsoftware_exception_control: %d\n", arf_dump->cr0_1.software_exception_control);
	igt_debug("\t\texternal_halt_exception_status: %d\n", arf_dump->cr0_1.external_halt_exception_status);
	igt_debug("\t\tbreakpoint_status: %d\n", arf_dump->cr0_1.breakpoint_status);
	igt_debug("\t-------------------------------------------------\n");
	igt_debug("\tarf_dump[%d][%d].cr0_2, cr0_3\n", y, x);
	igt_debug("\t\tAIP: 0x%lx\n", (((uint64_t)arf_dump->cr0_3.aip_high) << 32) + (((uint64_t)arf_dump->cr0_2.aip_low) << 3));
	igt_debug("\t-------------------------------------------------\n");
	igt_debug("\tarf_dump[%d][%d].msg0_1\n", y, x);
	igt_debug("\t\tslm_size: %d\n", arf_dump->msg0_1.slm_size);
	igt_debug("\t\tslm_offset: %d\n", arf_dump->msg0_1.slm_offset);
	igt_debug("\t\tpagefault_exception_enable: %d\n", arf_dump->msg0_1.pagefault_exception_enable);
	igt_debug("\t\tnumber_of_barriers: %d\n", arf_dump->msg0_1.number_of_barriers);
	igt_debug("\t\tbarrier_id: %d\n", arf_dump->msg0_1.barrier_id);
	igt_debug("\t-------------------------------------------------\n");
	igt_debug("\tarf_dump[%d][%d].dbg0_4\n", y, x);
	igt_debug("\t\tmemory_exception_type: %d\n", arf_dump->dbg0_4.memory_exception_type);
	igt_debug("\t\tpagefault_status_subtype: %d\n", arf_dump->dbg0_4.pagefault_status_subtype);
	igt_debug("\t\tsbid: %d\n", arf_dump->dbg0_4.sbid);
	igt_debug("\t-------------------------------------------------\n");
	igt_debug("\t\tthread_halted: %d, thread_resume: %d\n", arf_dump->rsvd0.thread_halted, arf_dump->rsvd1.thread_resume);
	igt_debug("\t-------------------------------------------------\n");
	igt_debug("\tarf_dump[%d][%d].sr0_0\n", y, x);
	igt_debug("\t\ttid: %d\n", arf_dump->sr0_0.tid);
	igt_debug("\t\teuid: %d\n", arf_dump->sr0_0.euid);
	igt_debug("\t\tsub_slice_id: %d\n", arf_dump->sr0_0.sub_slice_id);
	igt_debug("\t\tslice_id: %d\n", arf_dump->sr0_0.slice_id);
	igt_debug("\t\tpriority: %d\n", arf_dump->sr0_0.priority);
	igt_debug("\t\tpriority_class: %d\n", arf_dump->sr0_0.priority_class);
	igt_debug("\t\tffid: %d\n", arf_dump->sr0_0.ffid);
}

static void print_debug_surface(uint32_t *ptr, struct dim_t w_dim)
{
	for (int y = 0; y < w_dim.y ; y++) {
		for (int x = 0; x < w_dim.x ; x++) {
			print_sip_arf_dump((void *)ptr, x, y);
			ptr += sizeof(struct sip_arf_dump) / sizeof(*ptr);
		}
	}
	igt_debug("=========================================================\n");
}

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

static struct intel_buf *create_uc_buf(int fd, int width, int height, uint64_t region)
{
	struct intel_buf *buf;

	buf = intel_buf_create_full(buf_ops_create(fd), 0, width / 4, height,
				    32, 0, I915_TILING_NONE, 0, 0, 0, region,
				    DEFAULT_PAT_INDEX, DEFAULT_MOCS_INDEX);

	return buf;
}

static struct intel_buf *create_uc_buf_for_e64(int fd, int width, int height, int element_size)
{
	struct intel_buf *buf;
	buf = intel_buf_create_full(buf_ops_create(fd), 0, width * element_size,
				    height, 8, 0, I915_TILING_NONE, 0, 0, 0,
				    vram_if_possible(fd, 0), DEFAULT_PAT_INDEX,
				    DEFAULT_MOCS_INDEX);

	return buf;
}

static void vm_read(int fd, void *ptr, size_t count, off_t offset)
{
	igt_assert(fd >= 0);
	igt_assert_eq(pread(fd, ptr, count, offset), count);
}

static void vm_write(int fd, void *ptr, size_t count, off_t offset)
{
	igt_assert(fd >= 0);
	igt_assert_eq(pwrite(fd, ptr, count, offset), count);
}

static void vm_read_target(struct online_debug_data *data, void *ptr, size_t count, off_t offset)
{
	vm_read(data->vm_fd, ptr, count, data->target_offset + offset);
}

static void vm_write_target(struct online_debug_data *data, void *ptr, size_t count, off_t offset)
{
	vm_write(data->vm_fd, ptr, count, data->target_offset + offset);
}

static uint32_t vm_read_target_u32(struct online_debug_data *data, off_t offset)
{
	uint32_t ret;

	vm_read_target(data, &ret, sizeof(ret), offset);
	return ret;
}

static void vm_write_target_u32(struct online_debug_data *data, uint32_t value, off_t offset)
{
	vm_write_target(data, &value, sizeof(value), offset);
}

static int get_number_of_threads(struct online_debug_data *data)
{
	if (data->flags & (PAGEFAULT_STRESS_TEST | SHADER_PAGEFAULT_ONE_OF_MANY))
		return xe_query_eu_thread_count(data->drm_fd, 0);

	if (data->flags & (SHADER_MIN_THREADS | SHADER_PAGEFAULT))
		return 16;

	if (data->flags & (TRIGGER_RESUME_ONE | TRIGGER_RESUME_SINGLE_WALK |
	    TRIGGER_RESUME_PARALLEL_WALK | SHADER_CACHING_SRAM | SHADER_CACHING_VRAM))
		return 32;

	return 512;
}

static int caching_get_instruction_count(int fd, uint32_t s_dim__x, uint64_t flags)
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

static bool intel_gen_per_context_eudebug(int fd)
{
	const uint32_t id = intel_get_drm_devid(fd);

	return intel_gen(id) >= 35;
}

static void emit_e64b_read_page_fault(struct gpgpu_shader *shdr, uint64_t addr)
{
	igt_assert(shdr->gfx_ver >= 3500);
	igt_assert_f((addr & 0x3) == 0, "address must be aligned to DWord!\n");

	emit_iga64_code(shdr, e64b_read_page_fault, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)	mov (1)		s0.0<1>:ud ARG(0):ud
(W)	mov (1)		s0.1<1>:ud ARG(1):ud
// A64 offset
(W)	mov (8)		r30.0<1>:uq 0x0:uq
// load.ugm.d64t.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	r31 r30:1 null:0 s0.0 0x29c00
#endif
	)", lower_32_bits(addr), upper_32_bits(addr));
}

static void emit_e64b_write_page_fault(struct gpgpu_shader *shdr, uint64_t addr)
{
	igt_assert(shdr->gfx_ver >= 3500);
	igt_assert_f((addr & 0x3) == 0, "address must be aligned to DWord!\n");

	emit_iga64_code(shdr, e64b_write_page_fault, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)	mov (1)		s0.0<1>:ud ARG(0):ud
(W)	mov (1)		s0.1<1>:ud ARG(1):ud
// initialize register
(W)	mov (8)		r20.0<1>:uq 0x0:uq
(W)	mov (1)		r20.0<1>:ud 0xdeadbeaf:ud
// A64 offset
(W)	mov (8)		r30.0<1>:uq 0x0:uq
// store.ugm.d32t.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	null r30:1 r20:1 s0.0 0x29404
#endif
	)", lower_32_bits(addr), upper_32_bits(addr));
}

static void emit_e64b_atomic_read_page_fault(struct gpgpu_shader *shdr, uint64_t addr)
{
	igt_assert(shdr->gfx_ver >= 3500);
	igt_assert_f((addr & 0x3) == 0, "address must be aligned to DWord!\n");

	emit_iga64_code(shdr, e64b_atomic_read_page_fault, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)	mov (1)		s0.0<1>:ud ARG(0):ud
(W)	mov (1)		s0.1<1>:ud ARG(1):ud
// initialize register
(W)	mov (8)		r30.0<1>:uq 0x0:uq
(W)	mov (8)		r31.0<1>:uq 0x0:uq
// atomic_load.ugm.d32.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	r32 r30:2 null:0 s0.0 0x2900A
#endif
	)", lower_32_bits(addr), upper_32_bits(addr));
}

static void emit_e64b_atomic_write_page_fault(struct gpgpu_shader *shdr, uint64_t addr)
{
	igt_assert(shdr->gfx_ver >= 3500);
	igt_assert_f((addr & 0x3) == 0, "address must be aligned to DWord!\n");

	emit_iga64_code(shdr, e64b_atomic_write_page_fault, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)	mov (1)		s0.0<1>:ud ARG(0):ud
(W)	mov (1)		s0.1<1>:ud ARG(1):ud
// initialize register
(W)	mov (8)		r20.0<1>:uq 0x0:uq
(W)	mov (1)		r20.0<1>:ud 0xdeadbeaf:ud
// Prepare atomic_store A64 address payload for SIMT16
(W)	mov (8)		r30.0<1>:uq 0x0:uq
(W)	mov (8)		r31.0<1>:uq 0x0:uq
// Prepare atomic_store D32 dest data payload for SIMT16
(W)	mov (8)		r32.0<1>:ud r20.0<0;1,0>:ud
// atomic_store.ugm.d32.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	null r30:2 r32:1 s0.0 0x2900b
#endif
	)", lower_32_bits(addr), upper_32_bits(addr));
}

/**
 * emit_e64b_store_arf:
 * @shdr: shader to be modified
 *
 * Stores the ARF registers of the currently running eu thread in dedicated
 * thread space. The ARF registers to be saved are programmed with the eu thread
 * instruction. When changing the shader, struct *_sip_arf_dump must also be
 * changed. Struct *_sip_arf_dump is used to print out the ARF information stored
 * in memory.
 *
 * Please note that @gpgpu_shader__end_system_routine_step_arf() shader
 * for single-stepping test depends on this shader.
 */
static void emit_e64b_store_arf(struct gpgpu_shader *shdr)
{
	igt_assert(shdr->gfx_ver >= 3500);

	emit_iga64_code(shdr, e64b_store_arf, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)	mov (1)		s0.0<1>:uq R1_TGT_ADDRESS
// initialize register for store
(W)	mov (8)		r10.0<1>:uq 0x0:uq
// initialize register for load
(W)	mov (8)		r11.0<1>:uq 0x0:uq
// Prepare Data store
(W)	mov (1)		r10.0<1>:ud cr0.1<0;1,0>:ud
(W)	mov (1)		r10.1<1>:ud cr0.2<0;1,0>:ud
(W)	mov (1)		r10.2<1>:ud cr0.3<0;1,0>:ud
(W)	mov (1)		r10.3<1>:ud msg0.1<0;1,0>:ud
(W)	mov (1)		r10.4<1>:ud dbg0.4<0;1,0>:ud
(W)	mov (1)		r10.5<1>:ud 0x1:ud // set halted flag
(W)	mov (1)		r10.6<1>:ud 0x0:ud // unset resume flag
(W)	mov (1)		r10.7<1>:ud sr0.0<0;1,0>:ud
// A64 offset initialize
(W)	mov (8)		r20.0<1>:uq 0x0:uq
// Calculate the address using Thread Group ID X, Thread Group ID Y,
// and the size of the data block is 0x20
// Configure Structure_INTERFACE_DESCRIPTOR_DATA_2 to have only 1 thread per thread group
// Calculate Address offset
// ((tgid y * x_dim ) + tgid x ) * data_size_to_save (0x20)
// Structure_GPGPU_R0Payload (bspec: 56587) holds Thread Group ID X and Thread Group ID Y
// Thread Group ID Y =>  r0.6<0;1,0>:ud
// Thread Group DIM_X => DIM_X from inline data
(W)	mul (1)		r20.0<1>:ud r0.6<0;1,0>:ud R1_DIM_X
// Thread Group ID X =>  r0.1<0;1,0>:ud
(W)	add (1)		r20.0<1>:ud r20.0<0;1,0>:ud r0.1<0;1,0>:ud
// Data block size: 8 x D32 => 32 bytes => 0x20
(W)	mul (1)		r20.0<1>:ud r20.0<0;1,0>:ud 0x20:ud
// store.ugm.d32x8t.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	null r20:1 r10:1 s0.0 0x29604

WAIT_HOST:
(W)	sync.host	null
// Load memory and if resume is not 1, jump to sync.host line with jmpi to execute again
// load.ugm.d32x8t.a64.uc.uc.uc [src0]
(W)	sendg.ugm (1)	r11 r20:1 null:0 s0.0 0x29600
// If the r11.6<1>:ud does have value 0, then the sip should wait again with sync.host
(W)	mov (1)		f0.0<1>:ud 0x0:ud
(W)	cmp (1) (eq)f0.0 null<1>:ud r11.6<0;1,0>:ud 0x0:ud
(W&f0.0) jmpi		WAIT_HOST
#endif
	)");
}

static void emit_e64b_single_step_one_sip(struct gpgpu_shader *shdr)
{
	igt_assert(shdr->gfx_ver >= 3500);

	emit_iga64_code(shdr, e64b_sso_arf, R"(
#if GFX_VER >= 3500
// Set base address with scalar register
(W)		mov (1)		s0.0<1>:uq		R1_TGT_ADDRESS

// initialize register for store
(W)		mov (8)		r10.0<1>:uq		0x0:uq
// initialize register for load
(W)		mov (8)		r11.0<1>:uq		0x0:uq

// Prepare Data store
(W)		mov (1)		r10.0<1>:ud		0xdead:ud // set halted flag
(W)		mov (1)		r10.1<1>:ud		cr0.1<0;1,0>:ud
(W)		mov (1)		r10.2<1>:ud		cr0.2<0;1,0>:ud
(W)		mov (1)		r10.3<1>:ud		cr0.3<0;1,0>:ud
(W)		mov (1)		r10.4<1>:ud		0x0:ud
(W)		mov (1)		r10.5<1>:ud		0x0:ud
(W)		mov (1)		r10.6<1>:ud		0x0:ud
(W)		mov (1)		r10.7<1>:ud		0x0:ud // resume

// A64 offset initialize
(W)		mov (8)		r20.0<1>:uq		0x0:uq

// Calculate the address using Thread Group ID X, Thread Group ID Y,
// and the size of the data block is 0x20
// Configure Structure_INTERFACE_DESCRIPTOR_DATA_2 to have only 1 thread per thread group
// Calculate Address offset
// ((tgid y * x_dim ) + tgid x ) * data_size_to_save (0x20)
// Structure_GPGPU_R0Payload (bspec: 56587) holds Thread Group ID X and Thread Group ID Y
// Thread Group ID Y =>  r0.6<0;1,0>:ud
// Thread Group DIM_X => DIM_X from inline data
(W)		mul (1)		r20.0<1>:ud		R0_TGIDY R1_DIM_X
// Thread Group ID X =>  r0.1<0;1,0>:ud
(W)		add (1)		r20.0<1>:ud		r20.0<0;1,0>:ud	R0_TGIDX
// Data block size: 8 x D32 => 32 bytes => 0x20
(W)		mul (1)		r20.0<1>:ud		r20.0<0;1,0>:ud	0x20:ud

// efficient 64bit Store with Uncached L1, Uncached L3
// sendg ugm store with SBID 5
// Message Descriptor
//      bspec:71885
//      0x29604 =>
//      [45:44] Offset Scaling: 0(disable)
//      [43:22] Global Offset: 0
//      [21] Overfetch: 0 (disable)
//      [19:16] Cache: 2 (L1 uncached, L3 uncached)
//      [15:14] Address Type and Size: 2 (Flat A64 Base, A64 Index)
//      [13:11] Data Size: 2 (D32)
//      [10:10] Transpose : 1 (enable)
//      [9:7] Vector Size: 4 (Vector length 8)
//      [5:0] Opcode: 4 (Store)
(W)		sendg.ugm (1|M0)	null	r20:1	r10:1	s0.0	0x29604

WAIT_HOST:
(W)		sync.host		null

// Load memory and if resume is not 1, jump to sync.host line with jmpi to execute again
// efficient 64bit Load with Uncached L1, Uncached L3
// sendg ugm load with SBID 7
// Message Descriptor
//      bspec:71885
//      0x29600 =>
//      [45:44] Offset Scaling: 0(disable)
//      [43:22] Global Offset: 0
//      [21] Overfetch: 0 (disable)
//      [19:16] Cache: 2 (L1 uncached, L3 uncached)
//      [15:14] Address Type and Size: 2 (Flat A64 Base, A64 Index)
//      [13:11] Data Size: 2 (D32)
//      [10:10] Transpose : 1 (enable)
//      [9:7] Vector Size: 4 (Vector length 8)
//      [5:0] Opcode: 0 (Load)
(W)		sendg.ugm (1|M0)	r11	r20:1	null:0	s0.0	0x29600

// If the r11.7<1>:ud does have value 0, then the sip should wait again with sync.host
(W)		mov (1|M0)	f0.0<1>:ud	0x0:ud
(W)		cmp (1|M0)	(eq)f0.0	null<1>:ud	 r11.7<0;1,0>:ud	0x0:ud
(W&f0.0)	jmpi		WAIT_HOST

// Set Breakpoint Suppress
(W)	or  (1|M0)                      cr0.0<1>:ud   cr0.0<0;1,0>:ud   0x8000:ud
// Clear all the exceptions in cr0.1 including Breakpoint
(W)		and (1|M0)              cr0.1<1>:ud     cr0.1<0;1,0>:ud   0x047fffff:ud
// r11.7 is not 0. Check if r11.7 is set for continuing the single stepping.
(W)		mov (1|M0)		f0.0<1>:ud	0x0:ud
(W)		cmp (1|M0)     (eq)f0.0	null<1>:ud		r11.7<0;1,0>:ud 0x3:ud
// set Breakpoint Exception Status and Control in cr0.1 to continue single stepping.
(W&f0.0)	or  (1|M0)               cr0.1<1>:ud   cr0.1<0;1,0>:ud   0x80000000:ud
// return to an application
(W)		and (1|M0)               cr0.0<1>:ud   cr0.0<0;1,0>:ud   0x7FFFFFFD:ud
#endif
	)");
}

static struct gpgpu_shader *get_shader(struct online_debug_data *data)
{
	struct dim_t w_dim = walker_dimensions(data->thread_count);
	struct dim_t s_dim = surface_dimensions(data->thread_count);
	static struct gpgpu_shader *shader;
	uint64_t pf_addr = xe_canonical_va(data->drm_fd, 0x1f000000);

	shader = gpgpu_shader_create(data->drm_fd);

	if (shader->gfx_ver == 3000)
		shader->grfs_per_thread = 96;

	shader->simd_size = SIMD_SIZE;

	if (data->flags & PAGEFAULT_STRESS_TEST)
		shader->num_threads_in_tg = gpgpu_shader__get_max_threads_in_tg(shader);

	if (intel_gen_per_context_eudebug(data->drm_fd)) {
		if (data->flags & (SHADER_BREAKPOINT | TRIGGER_RESUME_SET_BP | SHADER_SINGLE_STEP |
		    SHADER_N_NOOP_BREAKPOINT | TRIGGER_UFENCE_SET_BREAKPOINT | SHADER_CACHING_SRAM |
		    SHADER_CACHING_VRAM))
			shader->exception_config |= SHADER_EXCEPTION_BREAKPOINT;
		if (data->flags & SHADER_LOOP)
			shader->exception_config |= SHADER_EXCEPTION_FE_FEH;
		if (data->flags & SHADER_PAGEFAULT)
			shader->exception_config |= SHADER_EXCEPTION_PAGEFAULT;

		if (data->flags & DISABLE_EXCEPTIONS)
			shader->exception_config = 0;
	}

	if ((data->flags & SHADER_PAGEFAULT) &&
	    (shader->exception_config & SHADER_EXCEPTION_PAGEFAULT)) {
		if (data->flags & SHADER_PAGEFAULT_READ)
			emit_e64b_read_page_fault(shader, pf_addr);
		else if (data->flags & SHADER_PAGEFAULT_WRITE)
			emit_e64b_write_page_fault(shader, pf_addr);
		else if (data->flags & SHADER_PAGEFAULT_ATOMIC_READ)
			emit_e64b_atomic_read_page_fault(shader, pf_addr);
		else if (data->flags & SHADER_PAGEFAULT_ATOMIC_WRITE)
			emit_e64b_atomic_write_page_fault(shader, pf_addr);
	} else if (!(data->flags & TRIGGER_RESUME_SINGLE_WALK)) {
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
	}

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
	} else if ((data->flags & SHADER_PAGEFAULT) &&
		   !(shader->exception_config & SHADER_EXCEPTION_PAGEFAULT)) {
		if (data->flags & SHADER_PAGEFAULT_READ)
			gpgpu_shader__read_a64_d32(shader, BAD_OFFSET);
		else if (data->flags & SHADER_PAGEFAULT_WRITE)
			gpgpu_shader__write_a64_d32(shader, BAD_OFFSET, BAD_CANARY);
		else if (data->flags & SHADER_PAGEFAULT_ONE_OF_MANY)
			emit_iga64_code(shader, pagefault_one_of_many, R"(
#if GFX_VER >= 2000
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

	if (sip->gfx_ver >= 3500 &&
	    (data->flags & (SHADER_PAGEFAULT | TRIGGER_RESUME_SINGLE_WALK))) {
		if (data->flags & SHADER_PAGEFAULT)
			emit_e64b_store_arf(sip);
		else if (data->flags & TRIGGER_RESUME_SINGLE_WALK)
			emit_e64b_single_step_one_sip(sip);
		else
			igt_assert_f(0, "Invalid SIP flags for GEN >= 3500");
	} else {
		if (!(data->flags & SHADER_PAGEFAULT_ONE_OF_MANY))
			gpgpu_shader__write_aip(sip, 0);
		else
			emit_iga64_code(sip, store_sr0_0, R"(
#if GFX_VER >= 2000
	mov (1) r5.0<1>:ud sr0.0:ud
	SET_THREAD_SPACE_ADDR(r4, 0, 0:ud, 4)
	STORE_SPACE_DW(r4, r5)
#endif
			)");

		/*
		* As gpgpu_shader__store_arf() shader implements its own
		* sync.host call routine, therefore XE3p's SHADER_PAGEFAULT case
		* should not call gpgpu_shader__wait() shader function.
		*/
		gpgpu_shader__wait(sip);
	}

	if (data->flags & SIP_SINGLE_STEP) {
		/* Single-step-one for gen >= 3500 does not need ending code */
		if (!(sip->gfx_ver >= 3500 && data->flags & TRIGGER_RESUME_SINGLE_WALK))
			gpgpu_shader__end_system_routine_step_if_eq(sip, w_dim.y, 0);
	} else {
		gpgpu_shader__end_system_routine(sip);
	}

	return sip;
}

static int eu_attentions_xor_count(const uint32_t *a, const uint32_t *b, uint32_t size)
{
	int count = 0;

	for (int i = 0; i < size / 4 ; i++)
		count += igt_hweight(a[i] ^ b[i]);

	return count;
}

static bool eu_attention_bitmask_is_subset(const uint8_t *sub, const uint8_t *super, int size)
{
	for (int i = 0; i < size; i++)
		if (sub[i] & ~super[i])
			return false;

	return true;
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

static int count_canaries_eq_for_e64(uint32_t *ptr, struct dim_t w_dim, uint32_t elm_count,
			      uint32_t offset, uint32_t value)
{
	int count = 0;
	int i;

	for (i = 0; i < w_dim.x * w_dim.y; i++)
		if (READ_ONCE(ptr[i * elm_count + offset]) == value)
			count++;

	return count;
}

static int count_canaries_neq_for_e64(uint32_t *ptr, struct dim_t w_dim, uint32_t elm_count,
			       uint32_t offset, uint32_t value)
{
	return w_dim.x * w_dim.y - count_canaries_eq_for_e64(ptr, w_dim, elm_count, offset, value);
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
	case DRM_XE_EUDEBUG_EU_CONTROL_CMD_UNLOCK:
		return "unlock";
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

static int __eu_ctl_from_event(int debugfd, struct drm_xe_eudebug_event *e, uint32_t cmd,
			       uint64_t *seqno)
{
	switch (e->type) {
	case DRM_XE_EUDEBUG_EVENT_EU_ATTENTION: {
		struct drm_xe_eudebug_event_eu_attention *at = igt_container_of(e, at, base);

		return __eu_ctl(debugfd, at->client_handle, at->exec_queue_handle, at->lrc_handle,
				at->bitmask, &at->bitmask_size, cmd, seqno);
	}
	case DRM_XE_EUDEBUG_EVENT_SYNC_HOST: {
		struct drm_xe_eudebug_event_sync_host *es = igt_container_of(e, es, base);

		return __eu_ctl(debugfd, es->client_handle, es->exec_queue_handle, es->lrc_handle,
				NULL, 0, cmd, seqno);
	}
	}
	igt_assert_f(0, "%s: unsupported event type: %d\n", __func__, e->type);
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

static inline uint64_t eu_ctl_unlock(int debugfd, uint64_t client,
				     uint64_t exec_queue, uint64_t lrc)
{
	return eu_ctl(debugfd, client, exec_queue, lrc, NULL, 0,
		      DRM_XE_EUDEBUG_EU_CONTROL_CMD_UNLOCK);
}

static struct online_debug_data *
online_debug_data_create(int drm_fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	const struct intel_device_info *info;
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
	info = intel_get_device_info(intel_get_drm_devid(drm_fd));
	data->gfx_ver = 100 * info->graphics_ver + info->graphics_rel;

	return data;
}

static void online_debug_data_destroy(struct online_debug_data *data)
{
	free(data->aips_offset_table);
	munmap(data, ALIGN(sizeof(*data), PAGE_SIZE));
}

static void wait_for_workloads_start(struct online_debug_data **data, int n)
{
	int count;
	bool acked;

	igt_for_milliseconds(n * STARTUP_TIMEOUT_MS) {
		count = 0;
		for (int i = 0; i < n; i++) {
			pthread_mutex_lock(&data[i]->mutex);
			acked = data[i]->acked;
			pthread_mutex_unlock(&data[i]->mutex);
			if (!acked)
				continue;

			if (vm_read_target_u32(data[i], 0) != 0)
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

static void sync_host_debug_trigger(struct xe_eudebug_debugger *d,
				    struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *s = (void *) e;

	igt_debug("EVENT[%llu] sync-host; client[%llu], exec_queue[%llu], "
		  "lrc[%llu]\n", s->base.seqno,
		  s->client_handle, s->exec_queue_handle, s->lrc_handle);

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

static void sync_host_reset_trigger(struct xe_eudebug_debugger *d,
				    struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *s = (void *) e;
	struct online_debug_data *data = d->ptr;

	igt_debug("EVENT[%llu] sync-host with sync reset; client[%llu], exec_queue[%llu], "
		  "lrc[%llu]\n", s->base.seqno,
		  s->client_handle, s->exec_queue_handle, s->lrc_handle);

	xe_force_gt_reset_sync(d->master_fd, data->hwe.gt_id);
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

	vm_read(data->vm_fd, buf, data->bb_size, data->bb_offset);

	ptr = memmem(buf, data->bb_size, p, kernel->size * sizeof(uint32_t));
	igt_assert(ptr);

	offset = ptr - buf;

	free(buf);

	return offset;
}

static bool set_breakpoint_once(struct xe_eudebug_debugger *d,
				struct online_debug_data *data)
{
	const uint32_t breakpoint_bit = GENISA_BF_DBG_EXCEPTION;
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
		vm_read(data->vm_fd, &instr_usdw, sz, data->bb_offset + aip);
		instr_usdw |= breakpoint_bit;
		vm_write(data->vm_fd, &instr_usdw, sz, data->bb_offset + aip);
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

	first_aip = vm_read_target_u32(data, 0);
	data->first_aip = first_aip;
	data->aips_offset_table[table_index++] = 0;

	fsync(data->vm_fd);
	for (int i = sz; i < data->target_size; i += sz) {
		aip = vm_read_target_u32(data, i);
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
	uint32_t aip;

	fsync(data->vm_fd);
	for (int i = 0; i < threads; i++) {
		aip = vm_read_target_u32(data, data->aips_offset_table[i]);
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

static void set_steering_flag(struct online_debug_data *data, uint32_t val)
{
	igt_debug("Setting steering flag for workload to %#x.\n", val);
	vm_write_target_u32(data, val, get_shared_space_address(data));
	fsync(data->vm_fd);
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
	if (!(data->flags & SHADER_PAGEFAULT))
		igt_assert(memcmp(bitmask, att->bitmask, att->bitmask_size) == 0);

	pthread_mutex_lock(&data->mutex);
	if (igt_nsec_elapsed(&data->exception_arrived) < (MAX_PREEMPT_TIMEOUT + 1) * NSEC_PER_SEC &&
	    data->flags & TRIGGER_RESUME_DELAYED) {
		pthread_mutex_unlock(&data->mutex);
		free(bitmask);
		return;
	} else if (data->flags & TRIGGER_RESUME_ONE) {
		only_first_set_bit(bitmask, bitmask, bitmask_size);
	} else if (data->flags & TRIGGER_RESUME_DSS) {
		uint64_t *event = (uint64_t *)att->bitmask;
		uint64_t *resume = (uint64_t *)bitmask;

		memset(bitmask, 0, bitmask_size);
		for (i = 0; i < att->bitmask_size / sizeof(uint64_t); i++) {
			if (!event[i])
				continue;

			resume[i] = event[i];
			break;
		}
	} else if (data->flags & TRIGGER_RESUME_SET_BP) {
		if (!set_breakpoint_once(d, data)) {
			/* breakpoint already set, check if the first thread managed to hit it */
			uint32_t expected, aip;
			struct gpgpu_shader *kernel;

			kernel = get_shader(data);
			expected = data->kernel_offset + kernel->size * 4 - 0x10;

			aip = vm_read_target_u32(data, 0);
			igt_assert_eq_u32(aip, expected);

			gpgpu_shader_destroy(kernel);
		}
	}

	if (data->flags & (SHADER_LOOP | SHADER_PAGEFAULT))
		set_steering_flag(data, STEERING_END_LOOP);
	pthread_mutex_unlock(&data->mutex);

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, att->client_handle,
						    att->exec_queue_handle, att->lrc_handle,
						    bitmask, att->bitmask_size);

	free(bitmask);
}

static void sync_host_resume_trigger(struct xe_eudebug_debugger *d,
				     struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *es = (void *) e;
	struct online_debug_data *data = d->ptr;

	if (data->last_eu_control_seqno > es->base.seqno)
		return;

	pthread_mutex_lock(&data->mutex);
	igt_gettime(&data->exception_arrived);
	pthread_mutex_unlock(&data->mutex);

	if (d->flags & TRIGGER_RESUME_DELAYED) {
		sleep(MAX_PREEMPT_TIMEOUT / 2);
	} else if (d->flags & TRIGGER_RESUME_SET_BP) {
		set_breakpoint_once(d, data);
	}

	/*
	 * Make sure that all exceptions triggered by the same
	 * breakpoint has been queued before calling eu control.
	 */
	sleep(1);

	/* workload was stopped by interrupt all */
	if (d->flags & SHADER_LOOP) {
		int threads = data->thread_count;
		struct dim_t w_dim = walker_dimensions(threads);
		uint32_t *target;
		int tc;

		igt_assert(data->vm_fd != -1);
		igt_assert(data->target_size != 0);
		target = calloc(1, data->target_size);

		/* accept some delay */
		igt_for_milliseconds(STARTUP_TIMEOUT_MS) {
			fsync(data->vm_fd);
			vm_read_target(data, target, data->target_size, 0);

			/* that would mean some dispatched threads were not stopped */
			if (count_canaries_eq(target, w_dim, SHADER_CANARY) == 0)
				break;
		}

		tc = count_canaries_neq(target, w_dim, 0);
		igt_info("%d threads were interrupted.\n", tc);

		igt_assert_f(count_canaries_eq(target, w_dim, SHADER_CANARY) == 0,
			     "Some threads were not affected by interrupt request!\n");
		free(target);

		set_steering_flag(data, STEERING_END_LOOP);

		eu_ctl_unlock(d->fd, es->client_handle,
			      es->exec_queue_handle, es->lrc_handle);
	}

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, es->client_handle,
						    es->exec_queue_handle, es->lrc_handle,
						    NULL, 0);
}

static void eu_attention_resume_single_step_trigger(struct xe_eudebug_debugger *d,
						    struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_eu_attention *att = (void *) e;
	struct online_debug_data *data = d->ptr;
	const int threads = data->thread_count;
	uint32_t val;

	get_aips_offset_table(data, threads);

	if (data->last_eu_control_seqno > att->base.seqno)
		return;

	if (data->flags & TRIGGER_RESUME_PARALLEL_WALK) {
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
	} else if (data->flags & TRIGGER_RESUME_SINGLE_WALK) {
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

	set_steering_flag(data, val);

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, att->client_handle,
						    att->exec_queue_handle, att->lrc_handle,
						    att->bitmask, att->bitmask_size);

	if (data->single_step_bitmask)
		for (int i = 0; i < att->bitmask_size; i++)
			data->single_step_bitmask[i] &= ~att->bitmask[i];
}

/*
 * Read, log, and skip all queued sync-host events.
 *
 * Intended to be called from within a sync-host trigger to
 * drain queued sync-host events which may be generated
 * in abundance.
 * With this the debugger_worker_loop will see a single
 * sync-host event and will call a single trigger for that.
 * However all sync-host event will be logged in event's log.
 */
#define MAX_EVENT_SIZE (32 * 1024)
static void read_queued_sync_host_events(struct xe_eudebug_debugger *d)
{
	struct drm_xe_eudebug_event_sync_host e = {};
	struct pollfd p = { .events = POLLIN };
	int ret;

	p.fd = d->fd;
	do {
		/* No timeout, checking for queued data */
		ret = poll(&p, 1, 0);

		if (ret == -1) {
			igt_info("%s(): poll failed with errno %d\n",
				 __func__, errno);
			break;
		}

		/* Timeout - no more sync-host queued */
		if (!ret)
			break;

		if (ret == 1 && (p.revents & POLLIN)) {
			e.base.type = DRM_XE_EUDEBUG_EVENT_READ;
			e.base.flags = 0;
			e.base.len = sizeof(e);

			/*
			 * While flood of SYNC_HOST event is expected
			 * there could be a case where other events
			 * may started to come to.
			 * In all error cases simply exit. This is recoverable.
			 */
			if (ioctl(d->fd,
				  DRM_XE_EUDEBUG_IOCTL_READ_EVENT,
				  &e.base))
				break;

			++d->event_count;
			xe_eudebug_event_log_write(d->log, &e.base);
			/* The test is screwed already. Will fail */
			igt_assert(e.base.type == DRM_XE_EUDEBUG_EVENT_SYNC_HOST);
		}
	} while(1);
}

static void sync_host_resume_single_step_trigger(struct xe_eudebug_debugger *d,
						 struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *es = (void *) e;
	struct online_debug_data *data = d->ptr;
	const int threads = data->thread_count;
	uint32_t val;

	igt_assert(d->flags & TRIGGER_RESUME_PARALLEL_WALK);

	if (data->steps_done >= SINGLE_STEP_COUNT)
		return;

	read_queued_sync_host_events(d);

	get_aips_offset_table(data, threads);

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
	set_steering_flag(data, val);

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, es->client_handle,
						    es->exec_queue_handle, es->lrc_handle,
						    NULL, 0);
}

static bool set_resume_on_halted_thread(struct online_debug_data *data)
{
	uint32_t thread_resume_pos = offsetof(struct sip_arf_dump, rsvd1.thread_resume);
	__off64_t offset = 0;
	int x, y;

	fsync(data->vm_fd);
	for (y = 0; y < data->w_dim.y ; y++) {
		for (x = 0; x < data->w_dim.x ; x++) {
			struct sip_arf_dump arf_dump;

			vm_read_target(data, &arf_dump, sizeof(arf_dump), offset);

			if (arf_dump.rsvd0.thread_halted == 1 &&
			    arf_dump.rsvd1.thread_resume != 1) {

				print_sip_arf_dump(&arf_dump, x, y);

				vm_write_target_u32(data, 1, offset + thread_resume_pos);

				fsync(data->vm_fd);

				arf_dump.rsvd1.thread_resume = vm_read_target_u32(data, offset + thread_resume_pos);

				igt_debug("\t-------------------------------------------------\n");
				igt_debug("\t\tarf_dump[%d][%d] Ater resume set => thread_halted: %d, thread_resume: %d\n",
					  y, x, arf_dump.rsvd0.thread_halted, arf_dump.rsvd1.thread_resume);
				igt_debug("\t-------------------------------------------------\n");
				return true;
			}
			offset += sizeof(arf_dump);
		}
	}
	igt_debug("=========================================================\n");

	return false;
}

static void sync_host_e64_resume_trigger(struct xe_eudebug_debugger *d,
					 struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *es = (void *) e;
	struct online_debug_data *data = d->ptr;
	static int count = 1;

	if (!(data->thread_resumed < data->w_dim.x * data->w_dim.y))
		return;

	igt_debug("sync_host_e64_resume_trigger count = %d\n", count);

	if (set_resume_on_halted_thread(data))
		data->thread_resumed++;

	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, es->client_handle,
				      es->exec_queue_handle, es->lrc_handle,
				      NULL, 0);

	count++;
}

static void sso_e64_resume_thread(struct online_debug_data *data,
			      uint32_t thread_num,
			      uint32_t flags)
{
	struct sip_arf_sso arfs = { .resume = flags };

	vm_write_target(data, &arfs, sizeof(arfs), thread_num * sizeof(arfs));
	fsync(data->vm_fd);
}

static uint64_t sso_e64_get_aip_from_arf(struct sip_arf_sso *arf)
{
	/*
	 * BSpec 56624, 77810: bits 2:0 for AIP Lower are reserved
	 *       and marked as MBZ.
	 */
	return ((uint64_t)arf->aiph << 32) + ((uint64_t)arf->aipl & ~0x7);
}

static bool sso_e64_is_thread_stopped(struct online_debug_data *data, uint32_t i)
{
	return data->sso.arfs[i].thread_halted == 0xdead;
}

static void sso_e64_copy_aip(struct online_debug_data *data)
{
	int i = 0;

	if (!data->sso.thread_last_aip)
		return;

	/* Update only when thread moved from not halted to halted */
	for (i = 0; i < data->w_dim.y * data->w_dim.x; i++)
		if (sso_e64_is_thread_stopped(data, i) &&
		    !data->sso.thread_last_aip[i])
				data->sso.thread_last_aip[i] =
					sso_e64_get_aip_from_arf(&data->sso.arfs[i]);
}

static void sso_e64_read_arf(struct online_debug_data *data)
{
	struct sip_arf_sso *arfs;
	size_t sz = data->thread_count * sizeof(*arfs);

	if (!data->sso.arfs) {
		arfs = malloc(sz);
		igt_assert(arfs);
	} else {
		arfs = data->sso.arfs;
	}
	memset(arfs, 0, sz);
	fsync(data->vm_fd);

	vm_read_target(data, arfs, sz, 0);

	data->sso.arfs = arfs;
	sso_e64_copy_aip(data);
}

static uint32_t sso_e64_get_next_stopped_thread(uint32_t current,
					struct online_debug_data *data)
{
	uint32_t thread = data->sso.current_thread;
	uint32_t count = 0;

	do {
		thread = (thread + 1) % (data->w_dim.x * data->w_dim.y);
		igt_assert(++count <= data->w_dim.x * data->w_dim.y);
	} while(!sso_e64_is_thread_stopped(data, thread));

	return thread;
}

static uint32_t sso_e64_get_first_stopped_thread(struct online_debug_data *data)
{
	uint32_t thread = 0;

	for (thread = 0; thread < data->w_dim.x * data->w_dim.y; thread++)
		if (sso_e64_is_thread_stopped(data, thread))
			return thread;

	igt_assert_f(0, "No stopped threads found!");
	return thread;
}

static void sso_e64_check_exceptions(struct online_debug_data *data, uint32_t i)
{
	/* Breakpoint exception must be reported */
	igt_assert(data->sso.arfs[i].exctrl & SSO_CTRL_BREAKPOINT_STATUS);
	/* but not the remaining exceptions */
	igt_assert(!(data->sso.arfs[i].exctrl & SSO_CTRL_EXCEPTION_STATUSES));
}

static void sso_e64_compare_aip(struct online_debug_data *data, uint32_t i)
{
	uint64_t aip_step = 0;

	if (i == data->sso.current_thread)
		aip_step = 0x10;

	igt_assert_eq_u64(data->sso.thread_last_aip[i] + aip_step,
			  sso_e64_get_aip_from_arf(&data->sso.arfs[i]));
}

/*
 * The single step checks is performed in a following way:
 * 1. Every thread is resumed with single-stepping twice in a row.
 * With every sync-host there are checks:
 * - Thread which is single-stepped has changed AIP
 * - There are no other exception than breakpoint (cr0.1)
 * - Other threads did not move (stored AIP).
 * In the end all threads are resumed without single-stepping.
 */
#define SINGLE_STEP_STEPS_VERIFY	0x2
static void sync_host_e64_single_step_resume_trigger(struct xe_eudebug_debugger *d,
						     struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_sync_host *es = (void *) e;
	struct online_debug_data *data = d->ptr;
	int thread_to_resume, i;

	/* There could be always late delivery of SYNC-HOST event. */
	if (data->sso.threads_checked >= data->w_dim.x * data->w_dim.y)
		return;

	/*
	 * For this test N-number of threads are stopped many times and this
	 * can generate large number of sync-host event per single iteration.
	 *
	 * Here we need only a single sync-host event to do what is needed so
	 * the subsequent events can be just throw away
	 */
	read_queued_sync_host_events(d);


	/* Re-read data at every sync-host processing */
	sso_e64_read_arf(data);

	thread_to_resume = data->sso.current_thread;

	/*
	 * Verify that:
	 * - there is only a breakpoint exception reported!
	 * - only the resumed thread's AIP changed (by 1 step)
	 */
	for (i = 0; i < data->w_dim.x * data->w_dim.y; i++) {

		/* Some threads may still be queued and some already released */
		if (!sso_e64_is_thread_stopped(data, i)) {
			if (i == thread_to_resume)
				goto resume;
			else
				continue;
		}

		sso_e64_check_exceptions(data, i);

		/* In the first sync-host there is nothing to compare yet */
		if (data->sso.thread_last_aip)
			sso_e64_compare_aip(data, i);
	}

	/* Initially prepare and gather data  */
	if (!data->sso.thread_last_aip) {
		data->sso.thread_last_aip = calloc(data->w_dim.x * data->w_dim.y,
							   sizeof(*data->sso.thread_last_aip));
		igt_assert(data->sso.thread_last_aip);

		data->sso.current_thread = sso_e64_get_first_stopped_thread(data);
		sso_e64_copy_aip(data);
	} else {
		/*
		 * In the first sync-host call there won't be any
		 * stepping as there a breakpoint hit.
		 * But every other call will have some thread stepped.
		 */
		data->sso.thread_last_aip[thread_to_resume] += 0x10;
	}

	if (data->sso.current_step < SINGLE_STEP_STEPS_VERIFY) {
		data->sso.current_step++;
	} else {
		/*
		 * If in step 2:
		 * - release the current thread so it may end and give space to
		 *   potentially scheduled one
		 * - move to next thread
		 */
		sso_e64_resume_thread(data, thread_to_resume, SSO_THREAD_RESUME);
		igt_debug("%s(): Thread %u verified with %u steps. Threads checked %u/%u\n",
			  __func__, thread_to_resume, data->sso.current_step,
			  data->sso.threads_checked + 1,
			  data->w_dim.x * data->w_dim.y);
		/* For last one don't look up for the next */
		if (data->sso.threads_checked < data->w_dim.x * data->w_dim.y - 1)
			data->sso.current_thread = sso_e64_get_next_stopped_thread(thread_to_resume, data);
		thread_to_resume = data->sso.current_thread;
		data->sso.current_step = 0;
		data->sso.threads_checked++;
	}

	if (data->sso.threads_checked < data->w_dim.x * data->w_dim.y) {
		sso_e64_resume_thread(data, thread_to_resume, SSO_THREAD_STEP);
	} else {
		igt_debug("%s(): All threads checked!\n", __func__);
		free(data->sso.thread_last_aip);
		data->sso.thread_last_aip = NULL;
		free(data->sso.arfs);
		data->sso.arfs = 0;
	}

 resume:
	data->last_eu_control_seqno = eu_ctl_resume(d->master_fd, d->fd, es->client_handle,
						    es->exec_queue_handle, es->lrc_handle,
						    NULL, 0);
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
		vm_read(vm_fd, &val, sizeof(val), addr);
		if (val == old_val) {
			igt_debug("val_before_write[%d]: %08x\n", vals_changed, val);
			vm_write(vm_fd, &new_val, sizeof(new_val), addr);
			vm_read(vm_fd, &val, sizeof(val), addr);
			igt_debug("val_before_fsync[%d]: %08x\n", vals_changed, val);
			fsync(vm_fd);
			vm_read(vm_fd, &val, sizeof(val), addr);
			igt_debug("val_after_fsync[%d]: %08x\n", vals_changed, val);
			igt_assert_eq_u32(val, new_val);
			vals_changed++;
		}
		addr += sizeof(uint32_t);
	}
}

static void sync_host_resume_caching_trigger(struct xe_eudebug_debugger *d,
					     struct drm_xe_eudebug_event *e)
{
	struct online_debug_data *data = d->ptr;
	struct dim_t s_dim = surface_dimensions(data->thread_count);
	uint32_t *kernel_offset = &data->kernel_offset;
	int *counter = &data->att_event_counter;
	uint32_t instr_usdw;
	struct gpgpu_shader *kernel;
	const uint32_t breakpoint_bit = 1 << 30;
	struct gpgpu_shader *shader_preamble;
	struct gpgpu_shader *shader_write_instr;
	const unsigned int instruction_count =
			caching_get_instruction_count(d->master_fd, s_dim.x, data->flags);
	uint64_t seqno = 0;
	int ret;

	if (data->last_eu_control_seqno > e->seqno)
		return;

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
		vm_read(data->vm_fd, &instr_usdw, sizeof(instr_usdw),
			data->bb_offset + *kernel_offset + shader_preamble->size * 4 +
			shader_write_instr->size * 4 * *counter);
		instr_usdw |= breakpoint_bit;
		vm_write(data->vm_fd, &instr_usdw, sizeof(instr_usdw),
			 data->bb_offset + *kernel_offset + shader_preamble->size * 4 +
			 shader_write_instr->size * 4 * *counter);
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
		for (int i = 0; i < data->target_size; i += sizeof(uint32_t))
			igt_assert_f(vm_read_target_u32(data, i) != CACHING_POISON_VALUE,
				     "Poison value found at %04d!\n", i);

	ret = __eu_ctl_from_event(d->fd, e, DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME, &seqno);
	data->last_eu_control_seqno = seqno;

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

static uint64_t get_memory_region(int fd, uint64_t flags, int region_bitmask)
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
		.extensions = data->flags & DISABLE_DEBUG_MODE ? 0 : to_user_pointer(&ext)
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
	if (data->flags & SHADER_LOOP || data->flags & SHADER_SINGLE_STEP || data->flags & SHADER_PAGEFAULT)
		s_dim.y++;
	/* Additional memory for caching check */
	if ((data->flags & SHADER_CACHING_SRAM) || (data->flags & SHADER_CACHING_VRAM))
		s_dim.y += caching_get_instruction_count(fd, s_dim.x, data->flags);
	buf = create_uc_buf(fd, s_dim.x, s_dim.y,
			    get_memory_region(fd, data->flags, TARGET_REGION_BITMASK));

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
	vm_flags |= data->flags & (SHADER_PAGEFAULT | FAULTABLE_VM) ?
			DRM_XE_VM_CREATE_FLAG_FAULT_MODE : 0;

	create.vm_id = xe_eudebug_client_vm_create(c, fd, vm_flags, 0);

	xe_eudebug_client_exec_queue_create(c, fd, &create);

	ibb = xe_bb_create_on_offset(fd, create.exec_queue_id, create.vm_id, bb_offset, bb_size,
				     get_memory_region(fd, data->flags, BB_REGION_BITMASK));
	intel_bb_set_lr_mode(ibb, true);

	sip = get_sip(data);

	igt_nsec_elapsed(&ts);
	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, sip, 0, 0);

	gpgpu_shader_destroy(sip);
	gpgpu_shader_destroy(shader);

	intel_bb_sync(ibb);

	if (data->flags & TRIGGER_RECONNECT)
		xe_eudebug_client_wait_stage(c, DEBUGGER_REATTACHED);
	else
		/* Make sure it wasn't the timeout. */
		igt_assert_lt_u64(igt_nsec_elapsed(&ts), c->timeout_ms * NSEC_PER_MSEC);

	if (!(data->flags & DO_NOT_EXPECT_CANARIES)) {
		ptr = xe_bo_mmap_ext(fd, buf->handle, buf->size, PROT_READ);
		data->thread_hit_count = count_canaries_neq(ptr, w_dim, 0);
		igt_assert_f(data->thread_hit_count, "No canaries found, nothing executed?\n");

		if ((data->flags & SHADER_BREAKPOINT || data->flags & TRIGGER_RESUME_SET_BP ||
		     data->flags & SHADER_N_NOOP_BREAKPOINT) &&
		    !(data->flags & (DISABLE_DEBUG_MODE | DISABLE_EXCEPTIONS))) {
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

static void run_online_client_for_e64(struct xe_eudebug_client *c)
{
	uint32_t elm_count = sizeof(struct sip_arf_dump) / sizeof(uint32_t);
	struct online_debug_data *data = c->ptr;
	int threads = data->thread_count;
	const uint64_t target_offset = 0x1a000000;
	const uint64_t bb_offset = 0x1b000000;
	const size_t bb_size = 4096;
	struct drm_xe_engine_class_instance hwe = data->hwe;
	struct drm_xe_ext_set_property ext = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
		.value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE |
			 DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_PAGEFAULT_ENABLE,
	};
	struct drm_xe_exec_queue_create create = {
		.instances = to_user_pointer(&hwe),
		.width = 1,
		.num_placements = 1,
		.extensions = to_user_pointer(&ext)
	};
	struct dim_t w_dim = walker_dimensions(threads);
	struct timespec ts = { };
	struct gpgpu_shader *sip, *shader;
	uint32_t metadata_id[2];
	uint64_t *metadata[2];
	struct intel_bb *ibb;
	struct intel_buf *buf;
	uint32_t vm_flags = 0;
	uint32_t *ptr;
	uint32_t aip;
	int fd;

	metadata[0] = calloc(2, sizeof(*metadata));
	metadata[1] = calloc(2, sizeof(*metadata));
	igt_assert(metadata[0]);
	igt_assert(metadata[1]);

	fd = xe_eudebug_client_open_driver(c);
	xe_device_get(fd);

	igt_debug("struct sip_arf_dump size: %ld bytes\n", sizeof(struct sip_arf_dump));
	buf = create_uc_buf_for_e64(fd, w_dim.x , w_dim.y, sizeof(struct sip_arf_dump));

	/* SIP's debug surface ppgtt address for VM_BIND */
	buf->addr.offset = target_offset;

	metadata[0][0] = bb_offset;
	metadata[0][1] = bb_size;
	metadata[1][0] = target_offset;
	metadata[1][1] = buf->size;
	metadata_id[0] = xe_eudebug_client_metadata_create(c, fd, DRM_XE_DEBUG_METADATA_ELF_BINARY,
							   2 * sizeof(*metadata), metadata[0]);
	metadata_id[1] = xe_eudebug_client_metadata_create(c, fd,
							   DRM_XE_DEBUG_METADATA_PROGRAM_MODULE,
							   2 * sizeof(*metadata), metadata[1]);

	/* Long Running mode and Pagefault mode (recoverable pagefault) */
	vm_flags |= (DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE);
	create.vm_id = xe_eudebug_client_vm_create(c, fd, vm_flags, 0);

	xe_eudebug_client_exec_queue_create(c, fd, &create);

	ibb = xe_bb_create_on_offset(fd, create.exec_queue_id, create.vm_id,
				     bb_offset, bb_size,
				     get_memory_region(fd, c->flags, BB_REGION_BITMASK));
	intel_bb_set_lr_mode(ibb, true);

	sip = get_sip(data);
	shader = get_shader(data);
	igt_nsec_elapsed(&ts);
	intel_bb_print(ibb);

	data->w_dim = w_dim;
	data->thread_resumed = 0;

	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, sip, 0, 0);

	gpgpu_shader_destroy(sip);
	gpgpu_shader_destroy(shader);

	intel_bb_sync(ibb);

	ptr = xe_bo_mmap_ext(fd, buf->handle, buf->size, PROT_READ);

	if (c->flags & SHADER_PAGEFAULT) {
		/* ptr[1] => sip_arf_dump.dw01 holds eu thread's cr0.2 (aip_low) */
		aip = ptr[1];

		print_debug_surface(ptr, w_dim);

		/* offset 1 holds cr0.2 (aip) */
		data->thread_hit_count = count_canaries_neq_for_e64(ptr, w_dim, elm_count, 1, 0);
		igt_assert_eq(count_canaries_eq_for_e64(ptr, w_dim, elm_count, 1, aip),
			      data->thread_hit_count);

		igt_debug("fault/exception hit in %d threads, AIP=0x%08x\n",
			  data->thread_hit_count, aip);

	} else if (data->flags & SIP_SINGLE_STEP) {
		igt_assert_eq(data->sso.threads_checked, data->w_dim.x * data->w_dim.y);
	}

	munmap(ptr, buf->size);

	intel_bb_destroy(ibb);

	xe_eudebug_client_exec_queue_destroy(c, fd, &create);
	xe_eudebug_client_vm_destroy(c, fd,  create.vm_id);

	xe_eudebug_client_metadata_destroy(c, fd, metadata_id[0], DRM_XE_DEBUG_METADATA_ELF_BINARY,
					   2 * sizeof(*metadata));
	xe_eudebug_client_metadata_destroy(c, fd, metadata_id[1],
					   DRM_XE_DEBUG_METADATA_PROGRAM_MODULE,
					   2 * sizeof(*metadata));

	xe_device_put(fd);
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

static void online_session_check(struct xe_eudebug_session *s)
{
	struct drm_xe_eudebug_event_eu_attention *ea = NULL;
	struct drm_xe_eudebug_event_pagefault *pf = NULL;
	struct drm_xe_eudebug_event_eu_attention *prev_ea = NULL;
	struct drm_xe_eudebug_event *event = NULL;
	struct online_debug_data *data = s->client->ptr;
	uint64_t flags = data->flags;
	bool expect_exception = flags & (DISABLE_EXCEPTIONS | DISABLE_DEBUG_MODE) ? false : true;
	int sum = 0;
	int bitmask_size;
	int pagefault_threads = 0;

	xe_eudebug_session_check(s, true, XE_EUDEBUG_FILTER_EVENT_VM_BIND |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP |
					  XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);

	if (intel_gen_per_context_eudebug(s->debugger->master_fd)) {
		bool exception_raised = false;

		xe_eudebug_for_each_event(event, s->debugger->log)
			if (event->type == DRM_XE_EUDEBUG_EVENT_SYNC_HOST) {
				exception_raised = true;
				break;
			}

		igt_assert(expect_exception == exception_raised);
		return;
	}

	bitmask_size = query_attention_bitmask_size(s->debugger->master_fd, data->hwe.gt_id);
	xe_eudebug_for_each_event(event, s->debugger->log) {
		if (event->type == DRM_XE_EUDEBUG_EVENT_EU_ATTENTION) {
			ea = (struct drm_xe_eudebug_event_eu_attention *)event;

			igt_assert(event->flags == DRM_XE_EUDEBUG_EVENT_STATE_CHANGE);
			igt_assert_eq(ea->bitmask_size, bitmask_size);
			igt_assert(match_attention_with_exec_queue(s->debugger->log, ea));

			/*
			 * Attention is level-triggered: if this event is a
			 * superset of the previous one, count only the newly
			 * stopped threads; otherwise count them all.
			 */
			if (prev_ea && eu_attention_bitmask_is_subset(prev_ea->bitmask, ea->bitmask, bitmask_size))
				sum += eu_attentions_xor_count((uint32_t *)ea->bitmask,
							       (uint32_t *)prev_ea->bitmask, bitmask_size);
			else
				sum += igt_bitmap_hweight(ea->bitmask, bitmask_size * 8);

			prev_ea = ea;
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
	struct online_debug_data *data = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		xe_eudebug_ack_ufence(d->fd, ef);
		pthread_mutex_lock(&data->mutex);
		data->acked = true;
		pthread_mutex_unlock(&data->mutex);
	}
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
		sr0_0 = vm_read_target_u32(data, offset);
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
 * SUBTEST: basic-breakpoint-exception-disabled
 * Description:
 *	Confirm breakpoint exception is not sent when it is disabled.
 *	Applies only to platforms which can enable exceptions per context.
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
static void test_basic_online(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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

	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s);

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

static void test_set_breakpoint_online(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s);

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
						       uint64_t flags)
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
		/* Per context debug */
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_debug_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_resume_trigger);


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
				  uint64_t flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;
	const uint32_t id = intel_get_drm_devid(fd);

	igt_require_f(intel_gen(id) < 35,
		      "Pagefault WA test requires older than Xe3p.\n");

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
	online_session_check(s);

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
	uint64_t flags = SHADER_BREAKPOINT | TRIGGER_RESUME_DELAYED;
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
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

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
 * SUBTEST: pagefault-read
 * Description:
 *	Check whether read (EU thread's load instruction) pagefault memory exception
 *	handling flow works or not
 *
 * SUBTEST: pagefault-write
 * Description:
 *	Check whether write (EU thread's store instruction) pagefault memory exception
 *	handling flow works or not
 *
 * SUBTEST: pagefault-atomic-read
 * Description:
 *	Check whether atomic read (EU thread's atomic inc instruction) pagefault
 *	memory exception handling flow works or not
 *
 * SUBTEST: pagefault-atomic-write
 * Description:
 *	Check whether read (EU thread's atomic store instruction) pagefault memory
 *	exception handling flow works or not
 *
 * SUBTEST: single-step-one
 * Functionality: EU control
 * Description:
 *	Schedules EU workload with 16 nops after breakpoint, then single-steps
 *	through the shader, advances one thread each step, checking if one
 *	thread advanced every step. Due to the time constraint, only first two
 *	shader instructions after breakpoint are validated.
 *
 */
static void test_basic_online_for_e64(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	const uint32_t id = intel_get_drm_devid(fd);
	igt_require_f(intel_gen(id) >= 35,
		      "Pagefault memory exception test requires Xe3p or higher.\n");

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client_for_e64, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);

	/* Per context debug */
	if (flags & SHADER_SINGLE_STEP)
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_e64_single_step_resume_trigger);
	else
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_e64_resume_trigger);

	/* for e64 pagefault read testcase */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	/* for DRM_XE_EUDEBUG_IOCTL_VM_OPEN */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	/* to get the target_offset that indicates the ppgtt address of debug surface */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);

	/* Long Running mode and Pagefault mode of vm requies to ack ufenc for vm_bind */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	xe_eudebug_session_run(s);

	online_session_check(s);

	xe_eudebug_session_destroy(s);
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
					     uint64_t flags)
{
	struct xe_eudebug_session *s1, *s2;
	struct online_debug_data *data;

	data = online_debug_data_create(fd, hwe, flags);
	s1 = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s1->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_reset_trigger);
	xe_eudebug_debugger_add_trigger(s1->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s1->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_reset_trigger);

	xe_eudebug_session_run(s1);
	xe_eudebug_session_destroy(s1);

	s2 = xe_eudebug_session_create(fd, run_online_client, flags, data);
	xe_eudebug_debugger_add_trigger(s2->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
					eu_attention_resume_trigger);
	xe_eudebug_debugger_add_trigger(s2->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s2->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

	xe_eudebug_session_run(s2);

	online_session_check(s2);

	xe_eudebug_session_destroy(s2);
	online_debug_data_destroy(data);
}

static int wait_for_exception(struct online_debug_data *data, int timeout)
{
	int ret = -ETIMEDOUT;

	igt_for_milliseconds(timeout) {
		pthread_mutex_lock(&data->mutex);
		if ((data->exception_arrived.tv_sec |
		     data->exception_arrived.tv_nsec) != 0)
			ret = 0;
		pthread_mutex_unlock(&data->mutex);

		if (!ret)
			break;
		usleep(1000);
	}

	return ret;
}

/**
 * SUBTEST: interrupt-all
 * Functionality: EU control
 * Description:
 *	Schedules EU workload which should last about a few seconds, then
 *	interrupts all threads, checks whether attention event came, and
 *	resumes stopped threads back.
 *
 * SUBTEST: interrupt-all-exception-disabled
 * Description:
 *	Confirm an exception is not sent on interrupt-all
 *	when forcing exception is disabled. Applies only to platforms
 *      which can enable exceptions per context.
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
static void test_interrupt_all(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

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

	/* Mainly for negative testcase, try to terminate cleanly when exception did not arrive. */
	if (wait_for_exception(data, STARTUP_TIMEOUT_MS))
		set_steering_flag(data, STEERING_END_LOOP);
	xe_eudebug_client_wait_done(s->client);

	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(s->client->log, true);

	online_session_check(s);

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
static void test_interrupt_other(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	struct online_debug_data *data;
	struct online_debug_data *debugee_data;
	struct xe_eudebug_session *s;
	struct xe_eudebug_client *debugee;
	uint64_t debugee_flags = SHADER_LOOP | DO_NOT_EXPECT_CANARIES;
	int ret;

	data = online_debug_data_create(fd, hwe, flags);
	s = xe_eudebug_session_create(fd, run_online_client, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN, open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
					exec_queue_trigger);

	if (intel_gen_per_context_eudebug(fd)) {
		/* Per context debug */
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_debug_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_resume_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						save_first_exception_trigger);
	}

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

	debugee_data = online_debug_data_create(fd, hwe, debugee_flags);
	s->debugger->ptr = debugee_data;
	debugee = xe_eudebug_client_create(fd, run_online_client, debugee_flags, debugee_data);
	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, debugee), 0);
	xe_eudebug_client_start(debugee);

	igt_debug("Waiting for debugee.\n");
	igt_for_milliseconds(3 * STARTUP_TIMEOUT_MS) {
		pthread_mutex_lock(&debugee_data->mutex);
		ret = debugee_data->acked;
		pthread_mutex_unlock(&debugee_data->mutex);
		if (ret)
			break;
		usleep(10000);
	}
	igt_assert_f(ret, "Timeout waiting for debugee.\n");

	if (intel_gen_per_context_eudebug(fd)) {
		/*
		 * eu_ctl should succeed, workload will be interrupted once it will be scheduled,
		 * ie. after end of previous runalone workload.
		 */
		eu_ctl(s->debugger->fd, debugee_data->client_handle,
		       debugee_data->exec_queue_handle, debugee_data->lrc_handle, NULL, 0,
		       DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL);
		ret = wait_for_exception(data, STARTUP_TIMEOUT_MS);
		set_steering_flag(data, STEERING_END_LOOP);
		igt_assert_f(ret, "Exception arrived for wrong context.\n");
	} else {
		/*
		 * Interrupting the other client should return invalid state
		 * as it is running in runalone mode
		 */
		igt_assert_eq(__eu_ctl(s->debugger->fd, debugee_data->client_handle,
				       debugee_data->exec_queue_handle, debugee_data->lrc_handle, NULL, 0,
				       DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL, NULL), -EINVAL);
		xe_force_gt_reset_async(s->debugger->master_fd, debugee_data->hwe.gt_id);
	}

	xe_eudebug_client_wait_done(s->client);

	/* Check if second workload was started and is running */
	wait_for_workload_start(debugee_data);
	if (intel_gen_per_context_eudebug(fd)) {
		ret = wait_for_exception(debugee_data, 3 * STARTUP_TIMEOUT_MS);
		igt_assert_f(!ret, "Timeout waiting for exception.\n");
	}
	set_steering_flag(debugee_data, STEERING_END_LOOP);
	xe_eudebug_client_wait_done(debugee);

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
static void test_tdctl_parameters(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);

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

	online_session_check(s);

	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

static void eu_debugger_detach_trigger(struct xe_eudebug_debugger *d,
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
	if (!(data->flags & SHADER_N_NOOP_BREAKPOINT))
		sleep(1);

	/*
	 * New session that is created by EU debugger on reconnect restarts
	 * seqno, causing isses with log sorting. To avoid that, create
	 * a new event log.
	 */
	reset_debugger_log(d);

	/* interrupt-reconnect:
	* 1st eudebug will disconnect causing GT reset.
	* When 2nd eudebug is connected client exits.
	*
	* Discovery process (2nd eudebug) in KMD blocks
	* client exit. This will result in full set of
	* create events.
	* However as soon as discovery is done, client will
	* exit and there will be destroy events in the queue.
	*
	* Our test framework reads events one by one and
	* can perform dedicated action.
	* For interrupt-reconnect for the second debugger remove
	* triggers that are performing actions as we know the
	* resources may be already removed.
	*/
	xe_eudebug_debugger_remove_trigger(d, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_remove_trigger(d, DRM_XE_EUDEBUG_EVENT_METADATA, create_metadata_trigger);

	ret = xe_eudebug_debugger_reattach(d, c_pid);
	igt_assert_eq(ret, 0);

	/* Let the discovery worker discover resources */
	sleep(2);

	if (!(data->flags & SHADER_N_NOOP_BREAKPOINT))
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
static void test_interrupt_reconnect(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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
					eu_debugger_detach_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					eu_debugger_detach_trigger);


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

	/* We expect workload reset, so no attention/sync_host should be raised */
	xe_eudebug_for_each_event(e, s->debugger->log) {
		igt_assert(e->type != DRM_XE_EUDEBUG_EVENT_EU_ATTENTION);
		igt_assert(e->type != DRM_XE_EUDEBUG_EVENT_SYNC_HOST);
	}

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
static void test_single_step(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	if (flags & TRIGGER_RESUME_SINGLE_WALK)
		igt_require_f(intel_gen(intel_get_drm_devid(fd)) < 35,
			      "single-step-one is not yet ready for this platform.\n");

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

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_single_step_trigger);

	xe_eudebug_session_run(s);
	online_session_check(s);
	xe_eudebug_session_destroy(s);
	online_debug_data_destroy(data);
}

static void eu_debugger_ndetach_trigger(struct xe_eudebug_debugger *d,
					struct drm_xe_eudebug_event *event)
{
	struct online_debug_data *data = d->ptr;
	static int debugger_detach_count;

	if (debugger_detach_count < (SHADER_LOOP_N - 1)) {
		/* Make sure the resume command was issued before detaching the debugger */
		if (data->last_eu_control_seqno > event->seqno)
			return;
		eu_debugger_detach_trigger(d, event);
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
static void test_debugger_reopen(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
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
					eu_debugger_ndetach_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);
	/* Per context debug */
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_debug_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					sync_host_resume_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
					eu_debugger_ndetach_trigger);


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
static void test_caching(int fd, struct drm_xe_engine_class_instance *hwe, uint64_t flags)
{
	struct xe_eudebug_session *s;
	struct online_debug_data *data;

	if (flags & SHADER_CACHING_VRAM || flags & BB_IN_VRAM || flags & TARGET_IN_VRAM)
		igt_skip_on_f(!xe_has_vram(fd), "Device does not have VRAM.\n");

	data = online_debug_data_create(fd, hwe, flags);

	s = calloc(1, sizeof(*s));
	igt_assert(s);

	s->client = xe_eudebug_client_create_timeout(fd, run_online_client, flags, data, XE_EUDEBUG_DEFAULT_CACHING_TIMEOUT_SEC);
	s->debugger = xe_eudebug_debugger_create(fd, flags, data);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
					open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM, vm_open_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
					create_metadata_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					ufence_ack_trigger);

	if (intel_gen_per_context_eudebug(fd)) {
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_debug_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_resume_caching_trigger);
	} else {
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
						eu_attention_debug_trigger);
		xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_EU_ATTENTION,
						sync_host_resume_caching_trigger);
	}

	xe_eudebug_session_run(s);
	online_session_check(s);
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

			if (!wait_for_exception(data[i], 1)) {
				attempt_mask |= BIT(i);
				should_break = 1;

				usleep(WORKLOAD_DELAY_US);
				eus = (struct drm_xe_eudebug_event_eu_attention *)data[i]->exception_event;
				eu_ctl_resume(s[i]->debugger->master_fd, s[i]->debugger->fd,
					      eus->client_handle, eus->exec_queue_handle,
					      eus->lrc_handle, eus->bitmask, eus->bitmask_size);
				free(eus);

			}

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
		online_session_check(s[i]);

		xe_eudebug_session_destroy(s[i]);
		online_debug_data_destroy(data[i]);
	}

	free(s);
	free(data);
	free(hwe);
}

/**
 * SUBTEST: breakpoint-many-contexts
 * Description:
 *	Schedules EU workload with preinstalled breakpoint on each available engine.
 *	Checks if every context hit breakpoint exception and resume.
 */
static void test_breakpoint_many_contexts(int *fd)
{
	int n = 0, flags = SHADER_BREAKPOINT | SHADER_MIN_THREADS;
	struct xe_eudebug_session *s[GEM_MAX_ENGINES] = {};
	struct online_debug_data *data[GEM_MAX_ENGINES] = {};
	struct drm_xe_engine_class_instance *hwe[GEM_MAX_ENGINES] = {};
	struct drm_xe_engine_class_instance *__e;
	int i;

	igt_require(intel_gen_per_context_eudebug(*fd));

	xe_sysfs_enable_ccs_mode(fd);

	xe_for_each_engine(*fd, __e)
		if (__e->engine_class == DRM_XE_ENGINE_CLASS_RENDER || \
		    __e->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE)
			hwe[n++] = __e;

	igt_require_f(n > 1, "Test requires at least two parallel compute engines!\n");

	for (i = 0; i < n; i++) {
		data[i] = online_debug_data_create(*fd, hwe[i], flags);
		s[i] = xe_eudebug_session_create(*fd, run_online_client, flags, data[i]);

		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
						ufence_ack_trigger);
		/* Per context debug */
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_debug_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						save_first_exception_trigger);

		igt_assert_eq(xe_eudebug_debugger_attach(s[i]->debugger, s[i]->client), 0);

		xe_eudebug_debugger_start_worker(s[i]->debugger);
		xe_eudebug_client_start(s[i]->client);
	}

	for (i = 0; i < n; i++)
		igt_assert(!wait_for_exception(data[i], STARTUP_TIMEOUT_MS));

	for (i = n - 1; i >= 0; i--) {
		struct drm_xe_eudebug_event_sync_host *eus =
		(struct drm_xe_eudebug_event_sync_host *)data[i]->exception_event;

		eu_ctl_resume(s[i]->debugger->master_fd, s[i]->debugger->fd, eus->client_handle,
			      eus->exec_queue_handle, eus->lrc_handle, NULL, 0);
		free(eus);

		xe_eudebug_client_wait_done(s[i]->client);
		xe_eudebug_debugger_stop_worker(s[i]->debugger);

		xe_eudebug_event_log_print(s[i]->debugger->log, true);
		online_session_check(s[i]);

		xe_eudebug_session_destroy(s[i]);
		online_debug_data_destroy(data[i]);
	}
}

/**
 * SUBTEST: interrupt-one-of-many-contexts
 * Description:
 *	Schedules EU spinner on each available engine. Then it interrupts one of
 *	the contexts and checks if the rest of the contexts are not affected.
 */
static void test_interrupt_one_of_many_contexts(int *fd)
{
	int n = 0, flags = SHADER_LOOP | SHADER_MIN_THREADS;
	struct xe_eudebug_session *s[GEM_MAX_ENGINES] = {};
	struct online_debug_data *data[GEM_MAX_ENGINES] = {};
	struct drm_xe_engine_class_instance *hwe[GEM_MAX_ENGINES] = {};
	struct drm_xe_engine_class_instance *__e;
	int i, to_interrupt;

	igt_require(intel_gen_per_context_eudebug(*fd));

	xe_sysfs_enable_ccs_mode(fd);

	xe_for_each_engine(*fd, __e)
		if (__e->engine_class == DRM_XE_ENGINE_CLASS_RENDER || \
		    __e->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE)
			hwe[n++] = __e;

	igt_require_f(n > 1, "Test requires at least two parallel compute engines!\n");

	for (i = 0; i < n; i++) {
		data[i] = online_debug_data_create(*fd, hwe[i], flags);
		s[i] = xe_eudebug_session_create(*fd, run_online_client, flags, data[i]);

		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_OPEN,
						open_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
						exec_queue_trigger);

		/* Per context debug */
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_debug_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_SYNC_HOST,
						sync_host_resume_trigger);

		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_VM,
						vm_open_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_METADATA,
						create_metadata_trigger);
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
						ufence_ack_trigger);

		igt_assert_eq(xe_eudebug_debugger_attach(s[i]->debugger, s[i]->client), 0);

		xe_eudebug_debugger_start_worker(s[i]->debugger);
		xe_eudebug_client_start(s[i]->client);
	}

	wait_for_workloads_start(data, n);

	to_interrupt = random() % n;
	pthread_mutex_lock(&data[to_interrupt]->mutex);
	igt_assert(data[to_interrupt]->exec_queue_handle != -1);
	eu_ctl_interrupt_all(s[to_interrupt]->debugger->fd, data[to_interrupt]->client_handle,
				data[to_interrupt]->exec_queue_handle, data[to_interrupt]->lrc_handle);
	pthread_mutex_unlock(&data[to_interrupt]->mutex);

	for (i = 0; i < n; i++) {
		struct drm_xe_eudebug_event *event = NULL;

		set_steering_flag(data[i], STEERING_END_LOOP);

		xe_eudebug_client_wait_done(s[i]->client);
		xe_eudebug_debugger_stop_worker(s[i]->debugger);
		xe_eudebug_event_log_print(s[i]->debugger->log, true);

		if (i == to_interrupt) {
			online_session_check(s[i]);
		} else {
			xe_eudebug_for_each_event(event, s[i]->debugger->log)
				if (event->type == DRM_XE_EUDEBUG_EVENT_SYNC_HOST)
					igt_fail_on_f(true, "Unexpected sync-host event!\n");
		}

		xe_eudebug_session_destroy(s[i]);
		online_debug_data_destroy(data[i]);
	}
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
	int gen;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		intel_allocator_multiprocess_start();
		igt_srandom();
		was_enabled = xe_eudebug_enable(fd, true);
		gen = intel_gen(intel_get_drm_devid(fd));
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

	test_gt_render_or_compute("single-step-one", fd, hwe) {
		if (gen < 35)
			test_single_step(fd, hwe, SHADER_SINGLE_STEP | SIP_SINGLE_STEP |
					 TRIGGER_RESUME_SINGLE_WALK);
		else
			test_basic_online_for_e64(fd, hwe, SHADER_SINGLE_STEP | SIP_SINGLE_STEP |
						  TRIGGER_RESUME_SINGLE_WALK);
	}

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

	igt_subtest_group() {
		igt_fixture() {
			igt_require(intel_gen_per_context_eudebug(fd));
		}

		test_gt_render_or_compute("basic-breakpoint-exception-disabled", fd, hwe)
			test_basic_online(fd, hwe, SHADER_BREAKPOINT | DISABLE_EXCEPTIONS);

		test_gt_render_or_compute("interrupt-all-exception-disabled", fd, hwe)
			test_interrupt_all(fd, hwe, SHADER_LOOP | DISABLE_EXCEPTIONS);

		igt_subtest("breakpoint-many-contexts")
			test_breakpoint_many_contexts(&fd);

		igt_subtest("interrupt-one-of-many-contexts")
			test_interrupt_one_of_many_contexts(&fd);
	}

	test_gt_render_or_compute("pagefault-read", fd, hwe) {
		if (gen < 35)
			test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_READ);
		else
			test_basic_online_for_e64(fd, hwe, SHADER_PAGEFAULT_READ);
	}

	test_gt_render_or_compute("pagefault-write", fd, hwe) {
		if (gen < 35)
			test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_WRITE);
		else
			test_basic_online_for_e64(fd, hwe, SHADER_PAGEFAULT_WRITE);
	}

	test_gt_render_or_compute("pagefault-atomic-read", fd, hwe)
		test_basic_online_for_e64(fd, hwe, SHADER_PAGEFAULT_ATOMIC_READ);

	test_gt_render_or_compute("pagefault-atomic-write", fd, hwe)
		test_basic_online_for_e64(fd, hwe, SHADER_PAGEFAULT_ATOMIC_WRITE);

	test_gt_render_or_compute("pagefault-read-stress", fd, hwe)
		if (gen < 35)
			test_pagefault_online(fd, hwe,
					      SHADER_PAGEFAULT_READ | PAGEFAULT_STRESS_TEST);
		else
			test_basic_online_for_e64(fd, hwe,
						  SHADER_PAGEFAULT_READ | PAGEFAULT_STRESS_TEST);
	test_gt_render_or_compute("pagefault-write-stress", fd, hwe)
		if (gen < 35)
			test_pagefault_online(fd, hwe,
					      SHADER_PAGEFAULT_WRITE | PAGEFAULT_STRESS_TEST);
		else
			test_basic_online_for_e64(fd, hwe,
						  SHADER_PAGEFAULT_WRITE | PAGEFAULT_STRESS_TEST);
	test_gt_render_or_compute("pagefault-one-of-many", fd, hwe)
		test_pagefault_online(fd, hwe, SHADER_PAGEFAULT_ONE_OF_MANY);

	igt_fixture() {
		xe_eudebug_enable(fd, was_enabled);

		intel_allocator_multiprocess_stop();
		drm_close_driver(fd);
	}
}
