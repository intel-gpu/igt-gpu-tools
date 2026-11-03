// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check dmabuf functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: dmabuf
 * Test category: functionality test
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>
#include <linux/dma-buf.h>
#include <lib/dmabuf_sync_file.h>
#include <poll.h>

#define MAX_N_BO	16
#define N_FD		2

#define WRITE_SYNC	(0x1 << 0)
#define READ_SYNC	(0x1 << 1)
#define READ_WRITE_SYNC	(0x1 << 2)
#define WRITE_READ_SYNC	(0x1 << 3)

/**
 * SUBTEST: export-dma-buf-once-write-sync
 * Description: Test exporting a sync file from a dma-buf with write deps once
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-once-read-sync
 * Description: Test exporting a sync file from a dma-buf with read deps once
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-once-read-write-sync
 * Description: Test exporting a sync file from a dma-buf with read followed by write deps once
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-once-write-read-sync
 * Description: Test exporting a sync file from a dma-buf with write followed by read deps once
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-many-write-sync
 * Description: Test exporting a sync file from a dma-buf with write deps many times
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-many-read-sync
 * Description: Test exporting a sync file from a dma-buf with read deps many times
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-many-read-write-sync
 * Description: Test exporting a sync file from a dma-buf with read followed by write deps many times
 * Functionality: export
 *
 * SUBTEST: export-dma-buf-many-write-read-sync
 * Description: Test exporting a sync file from a dma-buf with write followed by read deps many times
 * Functionality: export
 */

static void
test_export_dma_buf(struct drm_xe_engine_class_instance *hwe0,
		    struct drm_xe_engine_class_instance *hwe1,
		    int n_bo, int flags)
{
	uint64_t addr = 0x1a0000, base_addr = 0x1a0000;
	int fd[N_FD];
	uint32_t bo[MAX_N_BO];
	int dma_buf_fd[MAX_N_BO];
	uint32_t import_bo[MAX_N_BO];
	uint32_t vm[N_FD];
	uint32_t exec_queue[N_FD];
	size_t bo_size;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data [MAX_N_BO];
	int i;

	igt_assert_lte(n_bo, MAX_N_BO);

	for (i = 0; i < N_FD; ++i) {
		fd[i] = drm_open_driver(DRIVER_XE);
		vm[i] = xe_vm_create(fd[i], 0, 0);
		exec_queue[i] = xe_exec_queue_create(fd[i], vm[i], !i ? hwe0 : hwe1, 0);
	}

	bo_size = sizeof(*data[0]) * N_FD;
	bo_size = xe_bb_size(fd[0], bo_size);
	for (i = 0; i < n_bo; ++i) {
		bo[i] = xe_bo_create(fd[0], 0, bo_size,
				     vram_if_possible(fd[0], hwe0->gt_id),
				     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		dma_buf_fd[i] = prime_handle_to_fd(fd[0], bo[i]);
		import_bo[i] = prime_fd_to_handle(fd[1], dma_buf_fd[i]);

		if (i & 1)
			data[i] = xe_bo_map(fd[1], import_bo[i], bo_size);
		else
			data[i] = xe_bo_map(fd[0], bo[i], bo_size);
		memset(data[i], 0, bo_size);

		xe_vm_bind_sync(fd[0], vm[0], bo[i], 0, addr, bo_size);
		xe_vm_bind_sync(fd[1], vm[1], import_bo[i], 0, addr, bo_size);
		addr += bo_size;
	}
	addr = base_addr;

	for (i = 0; i < n_bo; ++i) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t spin_offset = (char *)&data[i]->spin - (char *)data[i];
		struct drm_xe_sync sync[2] = {
			{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
			{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		};
		struct drm_xe_exec exec = {
			.num_batch_buffer = 1,
			.syncs = to_user_pointer(sync),
		};
		struct xe_spin_opts spin_opts = { .addr = addr + spin_offset, .preempt = true };
		uint32_t syncobj, syncobj_signal;
		int b = 0;
		int sync_fd, syncobj_fd;

		/* Write spinner on FD[0] */
		xe_spin_init(&data[i]->spin, &spin_opts);
		syncobj_signal = syncobj_create(fd[0], 0);
		exec.exec_queue_id = exec_queue[0];
		exec.address = spin_opts.addr;
		exec.num_syncs = 1;
		sync[0].handle = syncobj_signal;
		xe_exec(fd[0], &exec);


		syncobj_fd = syncobj_handle_to_fd(fd[0], syncobj_signal,
						  DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE);

		/* Set read / write deps */
		if (flags & (READ_SYNC | READ_WRITE_SYNC))
			dmabuf_import_sync_file(dma_buf_fd[i],
						DMA_BUF_SYNC_READ, syncobj_fd);
		else
			dmabuf_import_sync_file(dma_buf_fd[i],
						DMA_BUF_SYNC_WRITE, syncobj_fd);

		/* Export prime BO as sync file and veify business */
		if (flags & (READ_SYNC | WRITE_READ_SYNC))
			sync_fd = dmabuf_export_sync_file(dma_buf_fd[i],
							  DMA_BUF_SYNC_READ);
		else
			sync_fd = dmabuf_export_sync_file(dma_buf_fd[i],
							  DMA_BUF_SYNC_WRITE);
		xe_spin_wait_started(&data[i]->spin);
		if (!(flags & READ_SYNC))
			igt_assert(sync_file_busy(sync_fd));

		/* Convert sync file to syncobj */
		syncobj = syncobj_create(fd[1], 0);
		syncobj_import_sync_file(fd[1], syncobj, sync_fd);

		/* Do an exec with syncobj as in fence on FD[1] */
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));
		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[0].handle = syncobj;
		sync[1].handle = syncobj_create(fd[1], 0);
		exec.exec_queue_id = exec_queue[1];
		exec.address = batch_addr;
		exec.num_syncs = 2;
		xe_exec(fd[1], &exec);

		/* Verify exec blocked on spinner / prime BO */
		usleep(5000);
		if (flags & READ_SYNC) {
			igt_assert(syncobj_wait(fd[1], &sync[1].handle, 1, INT64_MAX,
						0, NULL));
			igt_assert_eq(data[i]->data, 0xc0ffee);
		} else {
			igt_assert(!syncobj_wait(fd[1], &sync[1].handle, 1, 1, 0,
						 NULL));
			igt_assert_eq(data[i]->data, 0x0);
		}

		/* End spinner and verify exec complete */
		xe_spin_end(&data[i]->spin);
		igt_assert(syncobj_wait(fd[1], &sync[1].handle, 1, INT64_MAX,
					0, NULL));
		igt_assert(syncobj_wait(fd[0], &syncobj_signal, 1, INT64_MAX,
					0, NULL));
		igt_assert_eq(data[i]->data, 0xc0ffee);

		/* Clean up */
		syncobj_destroy(fd[0], syncobj_signal);
		syncobj_destroy(fd[1], sync[0].handle);
		syncobj_destroy(fd[1], sync[1].handle);
		close(sync_fd);
		close(syncobj_fd);
		addr += bo_size;
	}

	for (i = 0; i < n_bo; ++i) {
		munmap(data[i], bo_size);
		gem_close(fd[0], bo[i]);
		gem_close(fd[1], import_bo[i]);
		close(dma_buf_fd[i]);
	}

	for (i = 0; i < N_FD; ++i) {
		xe_exec_queue_destroy(fd[i], exec_queue[i]);
		xe_vm_destroy(fd[i], vm[i]);
		drm_close_driver(fd[i]);
	}
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe, *hwe0 = NULL, *hwe1;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);

		xe_for_each_engine(fd, hwe)
			if (hwe0 == NULL) {
				hwe0 = hwe;
			} else {
				hwe1 = hwe;
				break;
			}
	}

	igt_subtest("export-dma-buf-once-write-sync")
		test_export_dma_buf(hwe0, hwe1, 1, WRITE_SYNC);

	igt_subtest("export-dma-buf-many-write-sync")
		test_export_dma_buf(hwe0, hwe1, 16, WRITE_SYNC);

	igt_subtest("export-dma-buf-once-read-sync")
		test_export_dma_buf(hwe0, hwe1, 1, READ_SYNC);

	igt_subtest("export-dma-buf-many-read-sync")
		test_export_dma_buf(hwe0, hwe1, 16, READ_SYNC);

	igt_subtest("export-dma-buf-once-read-write-sync")
		test_export_dma_buf(hwe0, hwe1, 1, READ_WRITE_SYNC);

	igt_subtest("export-dma-buf-many-read-write-sync")
		test_export_dma_buf(hwe0, hwe1, 16, READ_WRITE_SYNC);

	igt_subtest("export-dma-buf-once-write-read-sync")
		test_export_dma_buf(hwe0, hwe1, 1, WRITE_READ_SYNC);

	igt_subtest("export-dma-buf-many-write-read-sync")
		test_export_dma_buf(hwe0, hwe1, 16, WRITE_READ_SYNC);

	igt_fixture()
		drm_close_driver(fd);
}
