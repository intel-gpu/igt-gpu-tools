// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <cairo.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "igt.h"
#include "intel_blt.h"
#include "intel_bufops.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include "xe/xe_util.h"

/**
 * TEST: Copy memory using 3d engine
 * Category: Core
 * Mega feature: Render
 * Sub-category: 3d
 * Functionality: render_copy
 * Test category: functionality test
 *
 * SUBTEST: render-square
 * Description: Copy surface using 3d engine dividing to 2x2 squares
 *
 * SUBTEST: render-vstripes
 * Description: Copy surface using 3d engine dividing to 4x1 rectangles
 *
 * SUBTEST: render-hstripes
 * Description: Copy surface using 3d engine dividing to 1x4 rectangles
 *
 * SUBTEST: render-random
 * Description: Copy surface using 3d engine with randomized width, height and
 *              rectangles size
 *
 * SUBTEST: render-full
 * Description: Copy surface using 3d engine (1:1)
 *
 * SUBTEST: render-full-compressed
 * Description: Copy surface using 3d engine (1:1) when intermediate surface
 *              is compressed
 */
#define WIDTH	256
#define HEIGHT	256
IGT_TEST_DESCRIPTION("Exercise render-copy on xe");

static bool debug_bb;
static bool write_png;
static bool buf_info;
static uint32_t surfwidth = WIDTH;
static uint32_t surfheight = HEIGHT;

static void scratch_buf_init(struct buf_ops *bops,
			     struct intel_buf *buf,
			     int width, int height,
			     uint32_t req_tiling,
			     enum i915_compression compression)
{
	int fd = buf_ops_get_fd(bops);
	int bpp = 32;
	uint64_t region = system_memory(fd);

	if (compression && xe_has_vram(fd))
		region = vram_memory(fd, 0);

	intel_buf_init_in_region(bops, buf, width, height, bpp, 0,
				 req_tiling, compression, region);

	igt_assert(intel_buf_width(buf) == width);
	igt_assert(intel_buf_height(buf) == height);
}

#define GROUP_SIZE 4096
static int compare_detail(const uint32_t *ptr1, uint32_t *ptr2,
			  uint32_t size)
{
	int i, ok = 0, fail = 0;
	int groups = size / GROUP_SIZE;
	int *hist = calloc(GROUP_SIZE, groups);

	igt_debug("size: %d, group_size: %d, groups: %d\n",
		  size, GROUP_SIZE, groups);

	for (i = 0; i < size / sizeof(uint32_t); i++) {
		if (ptr1[i] == ptr2[i]) {
			ok++;
		} else {
			fail++;
			hist[i * sizeof(uint32_t) / GROUP_SIZE]++;
		}
	}

	for (i = 0; i < groups; i++) {
		if (hist[i])
			igt_debug("[group %4x]: %d\n", i, hist[i]);
	}
	free(hist);

	igt_debug("ok: %d, fail: %d\n", ok, fail);

	return fail;
}

static int compare_bufs(struct intel_buf *buf1, struct intel_buf *buf2,
			bool detail_compare)
{
	void *ptr1, *ptr2;
	int fd1, fd2, ret;

	/* Avoid comparison of buffers of different sizes */
	if (buf1->surface[0].size != buf2->surface[0].size)
		return 0;

	fd1 = buf_ops_get_fd(buf1->bops);
	fd2 = buf_ops_get_fd(buf2->bops);

	ptr1 = xe_bo_map(fd1, buf1->handle, buf1->surface[0].size);
	ptr2 = xe_bo_map(fd2, buf2->handle, buf2->surface[0].size);
	ret = memcmp(ptr1, ptr2, buf1->surface[0].size);
	if (detail_compare)
		ret = compare_detail(ptr1, ptr2, buf1->surface[0].size);

	munmap(ptr1, buf1->surface[0].size);
	munmap(ptr2, buf2->surface[0].size);

	return ret;
}

static bool buf_is_aux_compressed(struct buf_ops *bops, struct intel_buf *buf)
{
	int xe = buf_ops_get_fd(bops);
	unsigned int gen = intel_gen(buf_ops_get_devid(bops));
	uint32_t ccs_size;
	uint8_t *ptr;
	bool is_compressed = false;

	igt_assert_neq(buf->ccs[0].offset, 0);

	ccs_size = intel_buf_ccs_width(gen, buf) * intel_buf_ccs_height(gen, buf);
	ptr = xe_bo_map(xe, buf->handle, buf->size);
	for (int i = 0; i < ccs_size; i++)
		if (ptr[buf->ccs[0].offset + i] != 0) {
			is_compressed = true;
			break;
		}
	munmap(ptr, buf->size);

	return is_compressed;
}

static bool buf_is_compressed(struct buf_ops *bops, struct intel_buf *buf)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	int xe = buf_ops_get_fd(bops);
	struct blt_copy_object obj;
	uint64_t ahnd;
	uint32_t vm, exec_queue;
	uint32_t tiling = i915_tile_to_blt_tile(buf->tiling);
	uint32_t devid = buf_ops_get_devid(bops);
	intel_ctx_t *ctx;
	bool is_compressed;

	if (!HAS_FLATCCS(devid))
		return buf_is_aux_compressed(bops, buf);

	vm = xe_vm_create(xe, 0, 0);
	exec_queue = xe_exec_queue_create(xe, vm, &inst, 0);
	ctx = intel_ctx_xe(xe, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open(xe, ctx->vm, INTEL_ALLOCATOR_RELOC);

	blt_set_object(&obj, buf->handle,
		       buf->size, buf->region, buf->mocs_index,
		       buf->pat_index, tiling,
		       buf->compression ? COMPRESSION_ENABLED : COMPRESSION_DISABLED,
		       COMPRESSION_TYPE_3D);
	blt_set_geom(&obj, buf->surface[0].stride, 0, 0, buf->width, buf->height, 0, 0);

	is_compressed = blt_surface_is_compressed(xe, ctx, NULL, ahnd, &obj);

	xe_exec_queue_destroy(xe, exec_queue);
	xe_vm_destroy(xe, vm);
	put_ahnd(ahnd);
	free(ctx);

	return is_compressed;
}

/*
 *
 * Scenarios implemented are presented below. We copy from linear to and forth
 * linear/tiled and back manipulating x,y coordinates from source and
 * destination.
 * For render randomize width and height and randomize x,y inside.
 *
 *  <linear>        <linear/x/y/4/64>
 *
 *  Square:
 *  +---+---+       +---+---+
 *  | 1 | 2 |  ==>  | 3 | 1 |
 *  +---+---+       +---+---+
 *  | 3 | 4 |  <==  | 4 | 2 |
 *  +---+---+       +---+---+
 *
 *  VStripes:
 *  +-+-+-+-+       +-+-+-+-+
 *  | | | | |  ==>  | | | | |
 *  |1|2|3|4|       |2|4|1|3|
 *  | | | | |  ==>  | | | | |
 *  +-+-+-+-+       +-+-+-+-+
 *
 *  HStripes:
 *  +-------+       +-------+
 *  |   1   |       |   2   |
 *  +-------+  ==>  +-------+
 *  |   2   |       |   4   |
 *  +-------+       +-------+
 *  |   3   |       |   1   |
 *  +-------+  <==  +-------+
 *  |   4   |       |   3   |
 *  +-------+       +-------+
 *
 *   Full:
 *  +-------+       +-------+
 *  |       |  ==>  |       |
 *  |   1   |       |   1   |
 *  |       |  <==  |       |
 *  +-------+       +-------+
 *
 *  Random:
 *  +-+-----+       +-+-----+
 *  |1|  2  |       |1|  2  |
 *  +-+-----+  ==>  +-+-----+
 *  |3|  4  |       |3|  4  |
 *  | |     |  <==  | |     |
 *  +-+-----+       +-+-----+
 */

enum render_copy_testtype {
	COPY_SQUARE,
	COPY_VSTRIPES,
	COPY_HSTRIPES,
	COPY_RANDOM,
	COPY_FULL,
	COPY_FULL_COMPRESSED,
};

static const char * const testname[] = {
	[COPY_SQUARE]	= "square",
	[COPY_VSTRIPES]	= "vstripes",
	[COPY_HSTRIPES]	= "hstripes",
	[COPY_RANDOM]	= "random",
	[COPY_FULL]	= "full",
	[COPY_FULL_COMPRESSED] = "full-compressed",
};

static int render(struct buf_ops *bops, uint32_t tiling,
		  uint32_t width, uint32_t height,
		  enum render_copy_testtype testtype,
		  uint64_t *duration_ns)
{
	struct intel_bb *ibb;
	struct intel_buf src, dst, final, grfs;
	int xe = buf_ops_get_fd(bops);
	uint32_t fails = 0;
	igt_render_copyfunc_t render_copy = NULL;
	int compression = testtype == COPY_FULL_COMPRESSED ? I915_COMPRESSION_RENDER :
							     I915_COMPRESSION_NONE;
	bool is_compressed;
	struct timespec tv;
	struct posrc {
		uint32_t x0, y0;
		uint32_t x1, y1;
		uint32_t x2, y2;
		uint32_t x3, y3;
		uint32_t w, h;
	} xys[] = {
		/* square */
		{ .x0 = 0,		.y0 = 0,
		  .x1 = width/2,	.y1 = 0,
		  .x2 = width/2,	.y2 = height/2,
		  .x3 = 0,		.y3 = height/2,
		  .w = width/2,		.h = height/2 },

		/* vstripes */
		{ .x0 = 0,
		  .x1 = width/2,
		  .x2 = width/2 + width/4,
		  .x3 = width/4,
		  .w = width/4,		.h = height },

		/* hstripes */
		{ .y0 = 0,
		  .y1 = height/2,
		  .y2 = height/2 + height/4,
		  .y3 = height/4,
		  .w = width,		.h = height/4 },

		/* random - filled later */
		{ 0, }
	}, *p;

	if (testtype == COPY_RANDOM) {
		width = rand() % width + 1;
		height = rand() % height + 1;
	}

	ibb = intel_bb_create(xe, SZ_4K);

	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, tiling,
			 compression);
	scratch_buf_init(bops, &final, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &grfs, 64, height * 4, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);

	intel_buf_draw_pattern(bops, &src,
			       0, 0, width, height,
			       0, 0, width, height, 0);

	render_copy = igt_get_render_copyfunc(xe);
	igt_assert(render_copy);

	if (duration_ns)
		igt_gettime(&tv);
	switch (testtype) {
	case COPY_SQUARE:
	case COPY_VSTRIPES:
	case COPY_HSTRIPES:
		p = &xys[testtype];

		/* copy to intermediate surface (dst) */
		render_copy(ibb,
			    &src, p->x0, p->y0, p->w, p->h,
			    &dst, p->x1, p->y1);
		render_copy(ibb,
			    &src, p->x1, p->y1, p->w, p->h,
			    &dst, p->x2, p->y2);
		render_copy(ibb,
			    &src, p->x2, p->y2, p->w, p->h,
			    &dst, p->x3, p->y3);
		render_copy(ibb,
			    &src, p->x3, p->y3, p->w, p->h,
			    &dst, p->x0, p->y0);

		/* copy to final */
		render_copy(ibb,
			    &dst, p->x0, p->y0, p->w, p->h,
			    &final, p->x3, p->y3);
		render_copy(ibb,
			    &dst, p->x1, p->y1, p->w, p->h,
			    &final, p->x0, p->y0);
		render_copy(ibb,
			    &dst, p->x2, p->y2, p->w, p->h,
			    &final, p->x1, p->y1);
		render_copy(ibb,
			    &dst, p->x3, p->y3, p->w, p->h,
			    &final, p->x2, p->y2);
		break;

	case COPY_RANDOM:
		p = &xys[testtype];
		p->x0 = rand() % width;
		p->y0 = rand() % height;
		igt_debug("Random <width: %u, height: %u, x0: %d, y0: %d>\n",
			  width, height, p->x0, p->y0);

		/* copy to intermediate surface (dst), split is randomized */
		render_copy(ibb,
			    &src, 0, 0, p->x0, p->y0,
			    &dst, 0, 0);
		render_copy(ibb,
			    &src, p->x0, 0, width - p->x0, p->y0,
			    &dst, p->x0, 0);
		render_copy(ibb,
			    &src, 0, p->y0, p->x0, height - p->y0,
			    &dst, 0, p->y0);
		render_copy(ibb,
			    &src, p->x0, p->y0, width - p->x0, height - p->y0,
			    &dst, p->x0, p->y0);

		render_copy(ibb,
			    &dst, 0, 0, width, height,
			    &final, 0, 0);
		break;


	case COPY_FULL:
	case COPY_FULL_COMPRESSED:
		render_copy(ibb,
			    &src, 0, 0, width, height,
			    &dst, 0, 0);

		render_copy(ibb,
			    &dst, 0, 0, width, height,
			    &final, 0, 0);
		break;
	}

	intel_bb_sync(ibb);
	if (duration_ns)
		*duration_ns = igt_nsec_elapsed(&tv);
	intel_bb_destroy(ibb);

	if (write_png) {
		intel_buf_raw_write_to_png(&src, "render_src_tiling_%d_%dx%d.png",
					   tiling, width, height);
		intel_buf_raw_write_to_png(&dst, "render_dst_tiling_%d_%dx%d.png",
					   tiling, width, height);
		intel_buf_raw_write_to_png(&final, "render_final_tiling_%d_%dx%d.png",
					   tiling, width, height);
	}

	fails = compare_bufs(&src, &final, false);
	if (compression == I915_COMPRESSION_RENDER)
		is_compressed = buf_is_compressed(bops, &dst);

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_close(bops, &final);

	igt_assert_f(fails == 0, "%s: (tiling: %d) fails: %d\n",
		     __func__, tiling, fails);
	if (compression == I915_COMPRESSION_RENDER && blt_platform_has_flat_ccs_enabled(xe))
		igt_assert_f(is_compressed, "%s: (tiling: %d) buffer is not compressed\n",
			     __func__, tiling);

	return fails;
}

static void mem_copy_busy(int fd, struct drm_xe_engine_class_instance *hwe, uint32_t vm,
			  uint64_t ahnd, uint32_t region, struct xe_spin **spin,
			  pthread_mutex_t *lock_init_spin)
{
	uint32_t copy_size = SZ_256K;
	/* Keep below 5 s timeout */
	uint64_t duration_ns = NSEC_PER_SEC * 4.5;
	intel_ctx_t *ctx;
	uint32_t exec_queue;
	uint32_t width = copy_size;
	uint32_t height = 1;
	uint32_t bo_size = ALIGN(SZ_4K, xe_get_default_alignment(fd));
	uint32_t bo;
	uint64_t spin_addr;
	int32_t src_handle, dst_handle;
	struct blt_mem_object src, dst;
	struct xe_spin_mem_copy mem_copy = {
		.fd = fd,
		.src = &src,
		.dst = &dst,
	};

	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);

	/* Create source and destination objects used for the copy */
	src_handle = xe_bo_create(fd, 0, copy_size, region, 0);
	dst_handle = xe_bo_create(fd, 0, copy_size, region, 0);
	blt_set_mem_object(mem_copy.src, src_handle, copy_size, width, width, height, region,
			   intel_get_uc_mocs_index(fd), DEFAULT_PAT_INDEX,
			   COMPRESSION_DISABLED);
	blt_set_mem_object(mem_copy.dst, dst_handle, copy_size, width, width, height, region,
			   intel_get_uc_mocs_index(fd), DEFAULT_PAT_INDEX,
			   COMPRESSION_DISABLED);
	mem_copy.src->ptr = xe_bo_map(fd, src_handle, copy_size);
	mem_copy.dst->ptr = xe_bo_map(fd, dst_handle, copy_size);
	mem_copy.src_offset = get_offset_pat_index(ahnd, mem_copy.src->handle,
						   mem_copy.src->size, 0, mem_copy.src->pat_index);
	mem_copy.dst_offset = get_offset_pat_index(ahnd, mem_copy.dst->handle,
						   mem_copy.dst->size, 0, mem_copy.dst->pat_index);

	/* Create spinner */
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0), 0);
	*spin = xe_bo_map(fd, bo, bo_size);
	spin_addr = intel_allocator_alloc_with_strategy(ahnd, bo, bo_size, 0,
							ALLOC_STRATEGY_LOW_TO_HIGH);
	xe_vm_bind_sync(fd, vm, bo, 0, spin_addr, bo_size);
	xe_spin_init_opts(*spin, .addr = spin_addr, .preempt = true,
			  .ctx_ticks = xe_spin_nsec_to_ticks(fd, 0, duration_ns),
			  .mem_copy = &mem_copy);
	igt_assert_eq(pthread_mutex_unlock(lock_init_spin), 0);

	while (true) {
		src.ptr[0] = 0xdeadbeaf;
		intel_ctx_xe_exec(ctx, ahnd, spin_addr);
		/* Abort if the spinner was stopped, otherwise continue looping */
		if ((*spin)->end == 0)
			break;
		igt_assert_f(!memcmp(mem_copy.src->ptr, mem_copy.dst->ptr, mem_copy.src->size),
			     "source and destination differ\n");
		dst.ptr[0] = 0;
	}

	/* Cleanup */
	xe_vm_unbind_sync(fd, vm, 0, spin_addr, bo_size);
	gem_munmap(*spin, bo_size);
	gem_close(fd, bo);
	gem_munmap(mem_copy.dst->ptr, copy_size);
	gem_munmap(mem_copy.src->ptr, copy_size);
	gem_close(fd, dst_handle);
	gem_close(fd, src_handle);
	intel_ctx_destroy(fd, ctx);
	xe_exec_queue_destroy(fd, exec_queue);
}

typedef struct {
	int fd;
	struct drm_xe_engine_class_instance *hwe;
	uint32_t vm;
	uint64_t ahnd;
	uint32_t region;
	struct xe_spin *spin;
	pthread_mutex_t lock_init_spin;
} data_thread_mem_copy;

static void *run_thread_mem_copy(void *arg)
{
	data_thread_mem_copy *data = (data_thread_mem_copy *)arg;

	mem_copy_busy(data->fd, data->hwe, data->vm, data->ahnd, data->region,
		      &data->spin, &data->lock_init_spin);
	pthread_exit(NULL);
}

static bool has_copy_function(struct drm_xe_engine_class_instance *hwe)
{
	return hwe->engine_class == DRM_XE_ENGINE_CLASS_COPY;
}

/**
 * TEST: Render while stressing copy functions
 * Category: Core
 * Mega feature: Render
 * Sub-category: 3d
 * Functionality: copy
 * Test category: stress test
 *
 * SUBTEST: render-stress-%s-copies
 * Description: Render while running %arg[1] parallel copies per supported engine.
 *		Even under stress from concurrent memory accesses, the render buffer
 *		and the copies must all be correct.
 *
 * arg[1]:
 * @0: 0 parallel copies
 * @1: 1 parallel copies
 * @2: 2 parallel copies
 * @4: 4 parallel copies
 */
#define MAX_COPY_THREADS 64
static void render_stress_copy(int fd, struct igt_collection *set,
			       uint32_t nparallel_copies_per_engine)
{
	struct igt_collection *regions;
	struct drm_xe_engine_class_instance *hwe;
	uint32_t vm;
	uint64_t ahnd;
	data_thread_mem_copy data_mem_copy[MAX_COPY_THREADS];
	pthread_t thread_mem_copy[MAX_COPY_THREADS];
	int thread_copy_count = 0;
	struct buf_ops *bops;
	int render_timeout = 3;
	int render_count = 0;
	uint64_t render_duration_total = 0, render_duration_min = -1, render_duration_max = 0;

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open_full(fd, vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH, 0);

	for_each_variation_r(regions, 1, set) {
		xe_for_each_engine(fd, hwe) {
			if (!has_copy_function(hwe))
				continue;
			for (int i = 0; i < nparallel_copies_per_engine; i++) {
				data_thread_mem_copy *data = &data_mem_copy[thread_copy_count];

				data->fd = fd;
				data->hwe = hwe;
				data->vm = vm;
				data->ahnd = ahnd;
				data->region = igt_collection_get_value(regions, 0);
				/*
				 * lock_init_spin is held by the newly created thread until the
				 * spinner is initialized and ready to be waited on with
				 * xe_spin_wait_started().
				 */
				igt_assert_eq(pthread_mutex_init(&data->lock_init_spin, NULL), 0);
				igt_assert_eq(pthread_mutex_lock(&data->lock_init_spin), 0);
				igt_assert_eq(pthread_create(
						   &thread_mem_copy[thread_copy_count],
						   NULL,
						   run_thread_mem_copy,
						   data),
					      0);
				thread_copy_count++;
			}
		}
	}

	/* Wait for all mem copy spinners to be initialized and started */
	for (int i = 0; i < thread_copy_count; i++) {
		igt_assert_eq(pthread_mutex_lock(&data_mem_copy[i].lock_init_spin), 0);
		xe_spin_wait_started(data_mem_copy[i].spin);
		igt_assert_eq(pthread_mutex_unlock(&data_mem_copy[i].lock_init_spin), 0);
	}

	bops = buf_ops_create(fd);
	igt_until_timeout(render_timeout) {
		uint64_t duration;

		render(bops, T_LINEAR, WIDTH, HEIGHT, COPY_FULL, &duration);
		render_count++;
		render_duration_total += duration;
		if (duration < render_duration_min)
			render_duration_min = duration;
		if (duration > render_duration_max)
			render_duration_max = duration;
	}
	igt_info("%d render() loops in %d seconds\n", render_count, render_timeout);
	igt_info("Render duration: avg = %" PRIu64 " ns, min = %" PRIu64 " ns, max = %" PRIu64 " ns\n",
		 render_duration_total / render_count,
		 render_duration_min, render_duration_max);

	buf_ops_destroy(bops);

	/* End all mem copy threads */
	for (int i = 0; i < thread_copy_count; i++)
		xe_spin_end(data_mem_copy[i].spin);
	for (int i = 0; i < thread_copy_count; i++) {
		pthread_join(thread_mem_copy[i], NULL);
		pthread_mutex_destroy(&data_mem_copy[i].lock_init_spin);
	}

	put_ahnd(ahnd);
	xe_vm_destroy(fd, vm);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'd':
		debug_bb = true;
		break;
	case 'p':
		write_png = true;
		break;
	case 'i':
		buf_info = true;
		break;
	case 'W':
		surfwidth = atoi(optarg);
		break;
	case 'H':
		surfheight = atoi(optarg);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -d\tDebug bb\n"
	"  -p\tWrite surfaces to png\n"
	"  -i\tPrint buffer info\n"
	"  -W\tWidth (default 256)\n"
	"  -H\tHeight (default 256)"
	;


int igt_main_args("dpiW:H:", NULL, help_str, opt_handler, NULL)
{
	int xe;
	struct buf_ops *bops;
	const char *tiling_name;
	int tiling;
	struct igt_collection *set;
	const struct section {
		const char *name;
		unsigned int nparallel_copies_per_engine;
	} sections[] = {
		{ "0", 0 },
		{ "1", 1 },
		{ "2", 2 },
		{ "4", 4 },
		{ NULL },
	};

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);

		/* As some cards don't have render, we should skip these. */
		igt_require(xe_has_engine_class(xe, DRM_XE_ENGINE_CLASS_RENDER));

		bops = buf_ops_create(xe);
		srand(time(NULL));
		set = xe_get_memory_region_set(xe, DRM_XE_MEM_REGION_CLASS_SYSMEM);
	}

	for (int id = 0; id <= COPY_FULL_COMPRESSED; id++) {
		igt_subtest_with_dynamic_f("render-%s", testname[id]) {
			igt_require(xe_has_engine_class(xe, DRM_XE_ENGINE_CLASS_RENDER));

			for_each_tiling(tiling) {
				if (!render_supports_tiling(xe, tiling,
							    id == COPY_FULL_COMPRESSED))
					continue;

				tiling_name = blt_tiling_name(tiling);
				tiling = blt_tile_to_i915_tile(tiling);
				igt_dynamic_f("render-%s-%ux%u", tiling_name, surfwidth, surfheight)
					render(bops, tiling, surfwidth, surfheight, id, NULL);
			}
		}
	}

	for (const struct section *s = sections; s->name; s++)
		igt_subtest_f("render-stress-%s-copies", s->name) {
			igt_require(blt_has_mem_copy(xe));
			render_stress_copy(xe, set, s->nparallel_copies_per_engine);
		}

	igt_fixture() {
		buf_ops_destroy(bops);
		drm_close_driver(xe);
	}
}
