/*
 * Copyright © 2012-2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *    Matthew Brost <matthew.brost@intel.com>
 */

/**
 * TEST: Check whether prime import/export works on the same device
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: prime import/export
 * Test category: functionality test
 *
 * Description:
 *	Check whether prime import/export works on the same device
 *	but with different fds, i.e. the wayland usecase.
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

IGT_TEST_DESCRIPTION("Check whether prime import/export works on the same"
		     " device... but with different fds.");

static char counter;
static int g_time_out = 5;
static pthread_barrier_t g_barrier;

static size_t get_min_bo_size(int fd1, int fd2)
{
	return 4 * max(xe_get_default_alignment(fd1),
		       xe_get_default_alignment(fd2));
}

static void
check_bo(int fd1, uint32_t handle1, int fd2, uint32_t handle2)
{
	size_t bo_size = get_min_bo_size(fd1, fd2);
	char *ptr1, *ptr2;
	int i;

	ptr1 = xe_bo_map(fd1, handle1, bo_size);
	ptr2 = xe_bo_map(fd2, handle2, bo_size);

	/* TODO: Export fence for both and wait on them */
	usleep(1000);

	/* check whether it's still our old object first. */
	for (i = 0; i < bo_size; i++) {
		igt_assert(ptr1[i] == counter);
		igt_assert(ptr2[i] == counter);
	}

	counter++;

	memset(ptr1, counter, bo_size);
	igt_assert(memcmp(ptr1, ptr2, bo_size) == 0);

	munmap(ptr1, bo_size);
	munmap(ptr2, bo_size);
}

/**
 * SUBTEST: basic-with_fd_dup
 * Description: basic prime import/export with fd_dup
 */

static void test_with_fd_dup(void)
{
	int fd1, fd2;
	size_t bo_size;
	uint32_t handle, handle_import;
	int dma_buf_fd1, dma_buf_fd2;

	counter = 0;

	fd1 = drm_open_driver(DRIVER_XE);
	fd2 = drm_open_driver(DRIVER_XE);

	bo_size = get_min_bo_size(fd1, fd2);

	handle = xe_bo_create(fd1, 0, bo_size, vram_if_possible(fd1, 0),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	dma_buf_fd1 = prime_handle_to_fd(fd1, handle);
	gem_close(fd1, handle);

	dma_buf_fd2 = dup(dma_buf_fd1);
	close(dma_buf_fd1);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd2);
	check_bo(fd2, handle_import, fd2, handle_import);

	close(dma_buf_fd2);
	check_bo(fd2, handle_import, fd2, handle_import);

	drm_close_driver(fd1);
	drm_close_driver(fd2);
}

/**
 * SUBTEST: basic-with_two_bos
 * Description: basic prime import/export with two BOs
 */

static void test_with_two_bos(void)
{
	int fd1, fd2;
	size_t bo_size;
	uint32_t handle1, handle2, handle_import;
	int dma_buf_fd;

	counter = 0;

	fd1 = drm_open_driver(DRIVER_XE);
	fd2 = drm_open_driver(DRIVER_XE);

	bo_size = get_min_bo_size(fd1, fd2);

	handle1 = xe_bo_create(fd1, 0, bo_size, vram_if_possible(fd1, 0),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	handle2 = xe_bo_create(fd1, 0, bo_size, vram_if_possible(fd1, 0),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	dma_buf_fd = prime_handle_to_fd(fd1, handle1);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd);

	close(dma_buf_fd);
	gem_close(fd1, handle1);

	dma_buf_fd = prime_handle_to_fd(fd1, handle2);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd);
	check_bo(fd1, handle2, fd2, handle_import);

	gem_close(fd1, handle2);
	close(dma_buf_fd);

	check_bo(fd2, handle_import, fd2, handle_import);

	drm_close_driver(fd1);
	drm_close_driver(fd2);
}

/**
 * SUBTEST: basic-with_one_bo_two_files
 * Description: basic prime import/export with one BO and two files
 */

static void test_with_one_bo_two_files(void)
{
	int fd1, fd2;
	size_t bo_size;
	uint32_t handle_import, handle_open, handle_orig, flink_name;
	int dma_buf_fd1, dma_buf_fd2;

	fd1 = drm_open_driver(DRIVER_XE);
	fd2 = drm_open_driver(DRIVER_XE);

	bo_size = get_min_bo_size(fd1, fd2);

	handle_orig = xe_bo_create(fd1, 0, bo_size,
				   vram_if_possible(fd1, 0),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	dma_buf_fd1 = prime_handle_to_fd(fd1, handle_orig);

	flink_name = gem_flink(fd1, handle_orig);
	handle_open = gem_open(fd2, flink_name);

	dma_buf_fd2 = prime_handle_to_fd(fd2, handle_open);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd2);

	/* dma-buf self importing an flink bo should give the same handle */
	igt_assert_eq_u32(handle_import, handle_open);

	drm_close_driver(fd1);
	drm_close_driver(fd2);
	close(dma_buf_fd1);
	close(dma_buf_fd2);
}

/**
 * SUBTEST: basic-with_one_bo
 * Description: basic prime import/export with one BO
 */

static void test_with_one_bo(void)
{
	int fd1, fd2;
	size_t bo_size;
	uint32_t handle, handle_import1, handle_import2, handle_selfimport;
	int dma_buf_fd;

	fd1 = drm_open_driver(DRIVER_XE);
	fd2 = drm_open_driver(DRIVER_XE);

	bo_size = get_min_bo_size(fd1, fd2);

	handle = xe_bo_create(fd1, 0, bo_size, vram_if_possible(fd1, 0),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	dma_buf_fd = prime_handle_to_fd(fd1, handle);
	handle_import1 = prime_fd_to_handle(fd2, dma_buf_fd);

	check_bo(fd1, handle, fd2, handle_import1);

	/* reimport should give us the same handle so that userspace can check
	 * whether it has that bo already somewhere. */
	handle_import2 = prime_fd_to_handle(fd2, dma_buf_fd);
	igt_assert_eq_u32(handle_import1, handle_import2);

	/* Same for re-importing on the exporting fd. */
	handle_selfimport = prime_fd_to_handle(fd1, dma_buf_fd);
	igt_assert_eq_u32(handle, handle_selfimport);

	/* close dma_buf, check whether nothing disappears. */
	close(dma_buf_fd);
	check_bo(fd1, handle, fd2, handle_import1);

	gem_close(fd1, handle);
	check_bo(fd2, handle_import1, fd2, handle_import1);

	/* re-import into old exporter */
	dma_buf_fd = prime_handle_to_fd(fd2, handle_import1);
	/* but drop all references to the obj in between */
	gem_close(fd2, handle_import1);
	handle = prime_fd_to_handle(fd1, dma_buf_fd);
	handle_import1 = prime_fd_to_handle(fd2, dma_buf_fd);
	check_bo(fd1, handle, fd2, handle_import1);

	/* Completely rip out exporting fd. */
	drm_close_driver(fd1);
	check_bo(fd2, handle_import1, fd2, handle_import1);
	drm_close_driver(fd2);
}

static void *thread_fn_reimport_vs_close(void *p)
{
	struct drm_gem_close close_bo;
	int *fds = p;
	int fd = fds[0];
	int dma_buf_fd = fds[1];
	uint32_t handle;

	pthread_barrier_wait(&g_barrier);

	igt_until_timeout(g_time_out) {
		handle = prime_fd_to_handle(fd, dma_buf_fd);

		close_bo.handle = handle;
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
	}

	return (void *)0;
}

/**
 * SUBTEST: reimport-vs-gem_close-race
 * Description: Reimport versus gem_close race
 */

 static void test_reimport_close_race(void)
{
	pthread_t *threads;
	int r, i, num_threads;
	int fds[2];
	size_t bo_size;
	int obj_count;
	void *status;
	uint32_t handle;
	int fake;

	/* Allocate exit handler fds in here so that we dont screw
	 * up the counts */
	fake = drm_open_driver(DRIVER_XE);

	/* TODO: Read object count */
	obj_count = 0;

	num_threads = sysconf(_SC_NPROCESSORS_ONLN);
	igt_info("create %d threads\n", num_threads);

	threads = calloc(num_threads, sizeof(pthread_t));

	fds[0] = drm_open_driver(DRIVER_XE);

	bo_size = xe_get_default_alignment(fds[0]);

	handle = xe_bo_create(fds[0], 0, bo_size,
			      vram_if_possible(fds[0], 0),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	fds[1] = prime_handle_to_fd(fds[0], handle);
	pthread_barrier_init(&g_barrier, NULL, num_threads);

	for (i = 0; i < num_threads; i++) {
		r = pthread_create(&threads[i], NULL,
				   thread_fn_reimport_vs_close,
				   (void *)(uintptr_t)fds);
		igt_assert_eq(r, 0);
	}

	for (i = 0;  i < num_threads; i++) {
		pthread_join(threads[i], &status);
		igt_assert(status == 0);
	}

	pthread_barrier_destroy(&g_barrier);
	free(threads);
	drm_close_driver(fds[0]);
	close(fds[1]);

	/* TODO: Read object count */
	obj_count = 0;

	igt_info("leaked %i objects\n", obj_count);

	drm_close_driver(fake);

	igt_assert_eq(obj_count, 0);
}

static void *thread_fn_export_vs_close(void *p)
{
	struct drm_prime_handle prime_h2f;
	struct drm_gem_close close_bo;
	int fd = (uintptr_t)p;
	size_t bo_size = xe_get_default_alignment(fd);
	uint32_t handle;

	pthread_barrier_wait(&g_barrier);

	igt_until_timeout(g_time_out) {
		/* We want to race gem close against prime export on handle one.*/
		handle = xe_bo_create(fd, 0, bo_size,
				      vram_if_possible(fd, 0),
				      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		if (handle != 1)
			gem_close(fd, handle);

		/* raw ioctl since we expect this to fail */

		/* WTF: for gem_flink_race I've unconditionally used handle == 1
		 * here, but with prime it seems to help a _lot_ to use
		 * something more random. */
		prime_h2f.handle = 1;
		prime_h2f.flags = DRM_CLOEXEC;
		prime_h2f.fd = -1;

		ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_h2f);

		close_bo.handle = 1;
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);

		close(prime_h2f.fd);
	}

	return (void *)0;
}

/**
 * SUBTEST: export-vs-gem_close-race
 * Description: Export versus gem_close race test
 */

static void test_export_close_race(void)
{
	pthread_t *threads;
	int r, i, num_threads;
	int fd;
	int obj_count;
	void *status;
	int fake;

	num_threads = sysconf(_SC_NPROCESSORS_ONLN);
	igt_info("create %d threads\n", num_threads);

	threads = calloc(num_threads, sizeof(pthread_t));

	/* Allocate exit handler fds in here so that we dont screw
	 * up the counts */
	fake = drm_open_driver(DRIVER_XE);

	/* TODO: Read object count */
	obj_count = 0;

	fd = drm_open_driver(DRIVER_XE);
	pthread_barrier_init(&g_barrier, NULL, num_threads);

	for (i = 0; i < num_threads; i++) {
		r = pthread_create(&threads[i], NULL,
				   thread_fn_export_vs_close,
				   (void *)(uintptr_t)fd);
		igt_assert_eq(r, 0);
	}

	for (i = 0;  i < num_threads; i++) {
		pthread_join(threads[i], &status);
		igt_assert(status == 0);
	}

	pthread_barrier_destroy(&g_barrier);
	drm_close_driver(fd);

	/* TODO: Read object count */
	obj_count = 0;

	igt_info("leaked %i objects\n", obj_count);

	drm_close_driver(fake);

	igt_assert_eq(obj_count, 0);
}

/**
 * SUBTEST: basic-llseek-size
 * Description: basic BO llseek size test
 */

static void test_llseek_size(void)
{
	int fd, i;
	uint32_t handle;
	int dma_buf_fd;

	counter = 0;

	fd = drm_open_driver(DRIVER_XE);

	for (i = 0; i < 10; i++) {
		int bufsz = xe_get_default_alignment(fd) << i;

		handle = xe_bo_create(fd, 0, bufsz,
				      vram_if_possible(fd, 0),
				      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		dma_buf_fd = prime_handle_to_fd(fd, handle);

		gem_close(fd, handle);

		igt_assert(prime_get_size(dma_buf_fd) == bufsz);

		close(dma_buf_fd);
	}

	drm_close_driver(fd);
}

/**
 * SUBTEST: basic-llseek-bad
 * Description: basid bad BO llseek size test
 */

static void test_llseek_bad(void)
{
	int fd;
	size_t bo_size;
	uint32_t handle;
	int dma_buf_fd;

	counter = 0;

	fd = drm_open_driver(DRIVER_XE);

	bo_size = 4 * xe_get_default_alignment(fd);
	handle = xe_bo_create(fd, 0, bo_size,
			      vram_if_possible(fd, 0),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	dma_buf_fd = prime_handle_to_fd(fd, handle);

	gem_close(fd, handle);

	igt_require(lseek(dma_buf_fd, 0, SEEK_END) >= 0);

	igt_assert(lseek(dma_buf_fd, -1, SEEK_END) == -1 && errno == EINVAL);
	igt_assert(lseek(dma_buf_fd, 1, SEEK_SET) == -1 && errno == EINVAL);
	igt_assert(lseek(dma_buf_fd, bo_size, SEEK_SET) == -1 && errno == EINVAL);
	igt_assert(lseek(dma_buf_fd, bo_size + 1, SEEK_SET) == -1 && errno == EINVAL);
	igt_assert(lseek(dma_buf_fd, bo_size - 1, SEEK_SET) == -1 && errno == EINVAL);

	close(dma_buf_fd);

	drm_close_driver(fd);
}

int igt_main()
{
	struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "basic-with_one_bo", test_with_one_bo },
		{ "basic-with_one_bo_two_files", test_with_one_bo_two_files },
		{ "basic-with_two_bos", test_with_two_bos },
		{ "basic-with_fd_dup", test_with_fd_dup },
		{ "export-vs-gem_close-race", test_export_close_race },
		{ "reimport-vs-gem_close-race", test_reimport_close_race },
		{ "basic-llseek-size", test_llseek_size },
		{ "basic-llseek-bad", test_llseek_bad },
	};
	int i;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_subtest(tests[i].name)
			tests[i].fn();
	}

	igt_fixture()
		drm_close_driver(fd);
}
