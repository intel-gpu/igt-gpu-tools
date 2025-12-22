// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: Tests for Compute walker post sync interrupt
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Compute walker interrupt
 * Functionality: Compute walker post sync interrupt notification
 * Test category: functionality test
 */
#include "gpgpu_shader.h"
#include "lib/gpu_cmds.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"
#include "igt.h"

#define BATCH_STATE_SPLIT 2048
#define WALKER_X_DIM	1
#define WALKER_Y_DIM	512
#define COLOR_C4 0xC4C4C4C4
#define SHADER_CANARY 0x01010101
#define STOP_CANARY 0x12345678
#define SHADER_LOOP 1
#define SHADER_SIMPLE_DWORD 2
#define POST_SYNC_ADDR  0x10000ULL
#define POST_SYNC_VALUE 0x80050004
#define USER_FENCE_VALUE   0xdeadbeefdeadbeefull
#define WRITE_IMMEDIATE_OP (1 << 16)
#define CW_INTERRUPT_ENABLE (1 << 3)
#define PIPE_CONTROL 0x7a000004
#define WIDTH 1
#define HEIGHT 512

struct dim_t {
	uint32_t x;
	uint32_t y;
} w_dim;

struct thread_data {
	struct intel_buf *post_sync;
	uint32_t *post_sync_ptr;
	uint32_t vm;
	uint32_t exec_queue_id;
	pthread_barrier_t barrier;
	int fd;
};

static struct intel_buf *
create_buf(int fd, int width, int height, uint32_t color)
{
	struct intel_buf *buf;
	uint32_t *ptr;
	size_t i;

	buf = intel_buf_create(buf_ops_create(fd), width, height, 32, 0,
			       I915_TILING_NONE, 0);

	ptr = xe_bo_map(fd, buf->handle, buf->size);
	igt_assert(ptr != MAP_FAILED);

	for (i = 0; i < buf->size / sizeof(uint32_t); i++)
		ptr[i] = color;

	munmap(ptr, buf->size);

	return buf;
}

static struct intel_bb *xe_bb_create_on_offset(int fd, uint32_t exec_queue, uint32_t vm,
					       uint64_t offset, uint32_t size)
{
	struct intel_bb *ibb;

	ibb = intel_bb_create_with_context(fd, exec_queue, vm, NULL, size);

	/* update intel bb offset */
	intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset, ibb->size);
	intel_bb_add_object(ibb, ibb->handle,
			    ibb->size, offset, ibb->alignment, false);
	ibb->batch_offset = offset;

	return ibb;
}

static struct gpgpu_shader *get_cw_post_sync_shader(int fd, const uint64_t flags)
{
	static struct gpgpu_shader *shader;

	shader = gpgpu_shader_create(fd);

	if (flags & SHADER_LOOP) {
		gpgpu_shader__label(shader, 0);
		gpgpu_shader__jump_neq(shader, 0, 0, STOP_CANARY);
	} else {
		gpgpu_shader__write_dword(shader, SHADER_CANARY, 0);
	}

	gpgpu_shader__eot(shader);

	return shader;
}

static void *thread1_fn(void *arg)
{
	const uint64_t bb_offset = 0x1b000000;
	const size_t bb_size = 4096;
	struct thread_data *t_data = arg;
	struct xe3p_interface_descriptor_data idd1, idd2;
	struct gpgpu_shader *shader1, *shader2;
	struct intel_buf *short_buf, *poll_buf;
	struct intel_bb *ibb;
	struct xe3p_cw2_interrupt_data intdata;
	uint32_t *inline_data1, *inline_data2;
	uint32_t *short_ptr, *poll_ptr;
	unsigned int width;
	uint64_t engine;
	int ret = 0;

	w_dim.x = WALKER_X_DIM;
	w_dim.y = WALKER_Y_DIM;

	ibb = xe_bb_create_on_offset(t_data->fd, t_data->exec_queue_id, t_data->vm,
				     bb_offset, bb_size);
	short_buf = create_buf(t_data->fd, WIDTH, HEIGHT, (uint8_t)COLOR_C4);
	poll_buf = create_buf(t_data->fd, WIDTH, HEIGHT, (uint8_t)COLOR_C4);

	short_ptr = xe_bo_mmap_ext(t_data->fd, short_buf->handle,
				   short_buf->size, PROT_READ | PROT_WRITE);
	poll_ptr = xe_bo_mmap_ext(t_data->fd, poll_buf->handle,
				  poll_buf->size, PROT_READ | PROT_WRITE);

	intel_bb_add_intel_buf(ibb, short_buf, true);
	intel_bb_add_intel_buf(ibb, t_data->post_sync, true);
	intel_bb_add_intel_buf(ibb, poll_buf, true);

	shader1 = get_cw_post_sync_shader(t_data->fd, SHADER_SIMPLE_DWORD);
	shader2 = get_cw_post_sync_shader(t_data->fd, SHADER_LOOP);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	xe3p_fill_interface_descriptor(ibb, shader1, &idd1);
	xe3p_fill_interface_descriptor(ibb, shader2, &idd2);

	intel_bb_ptr_set(ibb, 0);
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | GEN9_PIPELINE_SELECTION_MASK |
		     PIPELINE_SELECT_GPGPU);

	xe3p_emit_state_base_address(ibb);
	xehp_emit_state_compute_mode(ibb, shader1->vrt != VRT_DISABLED, 0);

	inline_data1 = intel_bb_ptr(ibb) + 4 * 32;
	width = w_dim.x * 16;

	intdata.post_sync_op = WRITE_IMMEDIATE_OP | CW_INTERRUPT_ENABLE;
	intdata.post_sync_addr = xe_canonical_va(t_data->fd,
						 t_data->post_sync->addr.offset);
	intdata.post_sync_val = POST_SYNC_VALUE;
	xe3p_emit_compute_walk2(ibb, 0, 0, width, w_dim.y, &idd1,
				w_dim.x * w_dim.y, &intdata);
	fill_inline_data(inline_data1,
			 xe_canonical_va(t_data->fd, short_buf->addr.offset),
			 short_buf, w_dim.x);

	xehp_emit_state_compute_mode(ibb, shader2->vrt != VRT_DISABLED, 0);
	inline_data2 = intel_bb_ptr(ibb) + 4 * 32;
	xe3p_emit_compute_walk2(ibb, 0, 0, width, w_dim.y, &idd2,
				w_dim.x * w_dim.y, NULL);
	fill_inline_data(inline_data2,
			 xe_canonical_va(t_data->fd, poll_buf->addr.offset),
			 poll_buf, w_dim.x);

	intel_bb_out(ibb, PIPE_CONTROL);
	intel_bb_out(ibb, 0x00100000);
	intel_bb_out(ibb, 0x00000000);
	intel_bb_out(ibb, 0x00000000);
	intel_bb_out(ibb, 0x00000000);
	intel_bb_out(ibb, 0x00000000);
	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	engine = I915_EXEC_DEFAULT;

	intel_bb_exec(ibb, engine | I915_EXEC_NO_RELOC, false, false);

	gpgpu_shader_destroy(shader1);
	gpgpu_shader_destroy(shader2);

	igt_assert_neq(poll_ptr[0], STOP_CANARY);

	/* Wait for the signal from thread 2 to stop the CW loop.
	 * Also ensure User interrupt is not generated before
	 * Compute walker interrupt.
	 */
	ret = pthread_barrier_wait(&t_data->barrier);
	igt_assert_f(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD,
		     "Barrier synchronization failed: %s\n", strerror(ret));
	/* Stop the CW loop. */
	poll_ptr[0] = STOP_CANARY;

	intel_bb_sync(ibb);

	igt_assert_eq(short_ptr[0], SHADER_CANARY);
	igt_assert_eq(poll_ptr[0], STOP_CANARY);
	igt_assert_eq(t_data->post_sync_ptr[0], POST_SYNC_VALUE);

	munmap(short_ptr, short_buf->size);
	munmap(poll_ptr, poll_buf->size);

	intel_buf_destroy(short_buf);
	intel_buf_destroy(poll_buf);
	intel_bb_destroy(ibb);

	return NULL;
}

static void *thread2_fn(void *arg)
{
	struct thread_data *t_data = arg;
	int ret = 0;

	/* Wait for the signal from the compute walker interrupt handler. */
	xe_wait_ufence(t_data->fd, (uint64_t *)t_data->post_sync_ptr,
		       POST_SYNC_VALUE, t_data->exec_queue_id, NSEC_PER_MSEC * 50000);

	ret = pthread_barrier_wait(&t_data->barrier);
	igt_assert_f(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD,
		     "Barrier synchronization failed: %s\n", strerror(ret));

	return NULL;
}

/**
 * SUBTEST: walker-post-sync
 * Description: Validate walker post sync interrupt notification.
 * Run type: FULL
 */
static void test_walker_notification(int fd, struct drm_xe_engine_class_instance *hwe)
{
	pthread_t t1, t2;
	struct thread_data t_data;
	int r = 0;

	t_data.fd = fd;
	t_data.vm = xe_vm_create(fd,  0, 0);
	t_data.exec_queue_id = xe_exec_queue_create(fd, t_data.vm, hwe, 0);
	t_data.post_sync = create_buf(fd, WIDTH, HEIGHT, (uint8_t)COLOR_C4);
	t_data.post_sync_ptr = xe_bo_mmap_ext(fd, t_data.post_sync->handle,
					      t_data.post_sync->size,
					      PROT_READ | PROT_WRITE);

	r = pthread_barrier_init(&t_data.barrier, NULL, 2);
	igt_assert_eq(r, 0);

	r = pthread_create(&t1, NULL, thread1_fn, &t_data);
	igt_assert_eq(r, 0);

	r = pthread_create(&t2, NULL, thread2_fn, &t_data);
	igt_assert_eq(r, 0);

	r = pthread_join(t1, NULL);
	igt_assert_eq(r, 0);

	r = pthread_join(t2, NULL);
	igt_assert_eq(r, 0);

	pthread_barrier_destroy(&t_data.barrier);
	munmap(t_data.post_sync_ptr, t_data.post_sync->size);
	intel_buf_destroy(t_data.post_sync);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(intel_graphics_ver(intel_get_drm_devid(fd)) >= IP_VER(35, 11));
	}

	igt_subtest_with_dynamic("walker-post-sync") {
		xe_for_each_engine(fd, hwe) {
			if (hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) {
				igt_dynamic_f("%s%d", xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance)
					test_walker_notification(fd, hwe);
			}
		}
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
