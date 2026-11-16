// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

enum pvr_page_size {
	PVR_PAGE_SIZE_4K = 12,
	PVR_PAGE_SIZE_16K = 14,
	PVR_PAGE_SIZE_64K = 16,
	PVR_PAGE_SIZE_256K = 18,
	PVR_PAGE_SIZE_1M = 20,
	PVR_PAGE_SIZE_2M = 21
};

int igt_main()
{
	int fd;

	igt_fixture()
	{
		fd = drm_open_driver(DRIVER_POWERVR);
	}

	/***********************************************************************
	   DRM_PVR_DEV_QUERY_HEAP_INFO_GET
	 ***********************************************************************/

	igt_describe("Test getting the size of an array from the kernel");
	igt_subtest("heap-info-get-size")
	{
		/* Check we can get the size of the array kernel side. */
		struct drm_pvr_dev_query_heap_info heap_info_get = {0};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);
		igt_assert_neq(heap_info_get.heaps.count, 0);
		igt_assert_neq(heap_info_get.heaps.stride, 0);
	}

	igt_describe("Test copying a single heap info from the kernel");
	igt_subtest("heap-info-copy-single")
	{
		struct drm_pvr_heap *heaps =
			calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));

		/* Try copying a single heap. */
		struct drm_pvr_dev_query_heap_info heap_info_get = {
			.heaps = DRM_PVR_OBJ_ARRAY(1, heaps),
		};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		/* Kernel should stop copying after a single heap. */
		igt_assert_eq(heap_info_get.heaps.count, 1);
		igt_assert_eq(heaps[1].size, 0);

		/* Kernel should respect the user side array stride. */
		igt_assert_eq(heap_info_get.heaps.stride, sizeof(*heaps));
	}

	igt_describe("Test copying all heaps info from the kernel");
	igt_subtest("heap-info-copy-all")
	{
		struct drm_pvr_dev_query_heap_info heap_info_get = {0};
		struct drm_pvr_heap *heaps =
			calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));
		uint32_t kernel_array_size = 0;

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		kernel_array_size = heap_info_get.heaps.count;

		heap_info_get.heaps =
			(struct drm_pvr_obj_array)DRM_PVR_OBJ_ARRAY(DRM_PVR_HEAP_COUNT,
								    heaps);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		igt_assert(heap_info_get.heaps.count == DRM_PVR_HEAP_COUNT ||
			   heap_info_get.heaps.count == kernel_array_size);

		/* Kernel should respect the user side array stride. */
		igt_assert_eq(heap_info_get.heaps.stride, sizeof(*heaps));
	}

	igt_describe("Test validity of all heaps");
	igt_subtest("heap-info-heaps-valid")
	{
		uint32_t heaps_count = 0;
		struct drm_pvr_heap *heaps =
			igt_pvr_get_heap_info(fd, &heaps_count);

		igt_assert(heaps);

		for (int heap_id = 0; heap_id < heaps_count; heap_id++) {
			const uint64_t page_mask =
				(1ull << heaps[heap_id].page_size_log2) - 1;

			/* This heap isn't present for all devices. */
			if (heap_id == (int)DRM_PVR_HEAP_RGNHDR)
				continue;

			igt_assert(heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_4K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_16K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_64K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_256K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_1M ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_2M);

			igt_assert_neq_u64(heaps[heap_id].size, 0);
			igt_assert(!(heaps[heap_id].size & page_mask));
			igt_assert(!(heaps[heap_id].base & page_mask));

			igt_assert(heaps[heap_id].flags == 0);
		}

		free(heaps);
	}

/* Value of stride and count as defined in the first released version of the UAPI. */
#define LEGACY_STRIDE (offsetof(struct drm_pvr_heap, page_size_log2) + sizeof(heaps->page_size_log2))
#define LEGACY_COUNT (DRM_PVR_HEAP_TRANSFER_FRAG + 1)

	igt_describe("Query, with one ioctl call, zero DRM PVR heaps");
	igt_subtest("heap-info-get-1-count-zero")
	{
		struct drm_pvr_heap heaps = { 0 };
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = DRM_PVR_OBJ_ARRAY(0, &heaps),
		};

		igt_assert(!query.heaps.count &&
			   query.heaps.stride &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count ||
			    query.heaps.stride != sizeof(heaps) ||
			    query.heaps.array != (uintptr_t)&heaps);
	}

	igt_describe("Query, with one ioctl call, one heap using an invalid stride");
	igt_subtest("heap-info-get-1-stride-too-small")
	{
		struct drm_pvr_heap heaps = { 0 };
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = DRM_PVR_OBJ_ARRAY(1, &heaps),
		};

		query.heaps.stride = 1;

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, EINVAL);
	}

	igt_describe("Query, with one ioctl call, all heaps known to userspace");
	igt_subtest("heap-info-get-1")
	{
		struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT] = { 0 };
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = DRM_PVR_OBJ_ARRAY(ARRAY_SIZE(heaps), heaps),
		};

		igt_assert(DRM_PVR_HEAP_COUNT == ARRAY_SIZE(heaps));
		igt_assert(query.heaps.count == DRM_PVR_HEAP_COUNT &&
			   query.heaps.stride == sizeof(*heaps) &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count > DRM_PVR_HEAP_COUNT ||
			    query.heaps.stride != sizeof(*heaps) ||
			    query.heaps.array != (uintptr_t)heaps);
	}

	igt_describe("Query, with one ioctl call, the legacy value of stride and count");
	igt_subtest("heap-info-get-1-legacy")
	{
		struct drm_pvr_dev_query_heap_info query = { 0 };
		struct drm_pvr_heap *heaps = NULL;

		igt_assert(!query.heaps.count &&
			   !query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count != LEGACY_COUNT ||
			    query.heaps.stride != LEGACY_STRIDE ||
			    query.heaps.array);
	}

	igt_describe("Query, with two ioctl calls, all heaps known to the kernel");
	igt_subtest("heap-info-get-2")
	{
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t ioctl1_count = ~0U, ioctl1_stride = ~0U;
		struct drm_pvr_heap *heaps = NULL;

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count == ~0U ||
			    query.heaps.stride == ~0U ||
			    query.heaps.array);

		ioctl1_stride = query.heaps.stride;
		ioctl1_count = query.heaps.count;

		heaps = calloc(ioctl1_count, ioctl1_stride);
		igt_fail_on(!heaps);

		query.heaps = (struct drm_pvr_obj_array)
			DRM_PVR_OBJ_ARRAY(query.heaps.count, heaps);

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count != ioctl1_count ||
			    query.heaps.stride != ioctl1_stride ||
			    query.heaps.array != (uintptr_t)heaps);

		free(heaps);
	}

	igt_describe("Query, with two ioctl calls, the legacy value of stride and count");
	igt_subtest("heap-info-get-2-legacy")
	{
		struct drm_pvr_dev_query_heap_info query = { 0 };
		struct drm_pvr_heap *heaps = NULL;

		igt_assert(!query.heaps.count &&
			   !query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count != LEGACY_COUNT ||
			    query.heaps.stride != LEGACY_STRIDE ||
			    query.heaps.array);

		heaps = calloc(query.heaps.count, sizeof(*heaps));
		igt_fail_on(!heaps);

		query.heaps = (struct drm_pvr_obj_array)
			DRM_PVR_OBJ_ARRAY(query.heaps.count, heaps);

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count != LEGACY_COUNT ||
			    query.heaps.stride != LEGACY_STRIDE ||
			    query.heaps.array != (uintptr_t)heaps);

		free(heaps);
	}

	igt_describe("Query, with two ioctl calls, heaps where UMD(count) < KMD(count)");
	igt_subtest("heap-info-get-2-count-umd-lt-kmd")
	{
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t umd_count, kmd_count;

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count == ~0U ||
			    query.heaps.stride == ~0U ||
			    query.heaps.array);

		kmd_count = query.heaps.count;
		umd_count = kmd_count - 1;

		igt_assert(kmd_count && umd_count < kmd_count);

		for (uint32_t count = umd_count; count; count--) {
			struct drm_pvr_heap *heaps = calloc(count, sizeof(*heaps));
			query = (struct drm_pvr_dev_query_heap_info){
				.heaps = DRM_PVR_OBJ_ARRAY(count, heaps),
			};

			igt_assert(query.heaps.count < kmd_count &&
				   query.heaps.stride &&
				   query.heaps.array);

			igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
						sizeof(query), &query, 0);

			igt_fail_on(query.heaps.count != count ||
				    query.heaps.stride != sizeof(*heaps) ||
				    query.heaps.array != (uintptr_t)heaps);

			free(heaps);
		}
	}

	igt_describe("Query, with two ioctl calls, heaps where UMD(count) > KMD(count)");
	igt_subtest("heap-info-get-2-count-umd-gt-kmd")
	{
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t umd_count, kmd_count;

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count == ~0U ||
			    query.heaps.stride == ~0U ||
			    query.heaps.array);

		kmd_count = query.heaps.count;
		umd_count = kmd_count * 2;

		igt_assert(kmd_count && umd_count > kmd_count);

		for (uint32_t count = umd_count; count > kmd_count; count--) {
			struct drm_pvr_heap *heaps = calloc(count, sizeof(*heaps));
			query = (struct drm_pvr_dev_query_heap_info){
				.heaps = DRM_PVR_OBJ_ARRAY(count, heaps),
			};

			igt_assert(query.heaps.count > kmd_count &&
				   query.heaps.stride &&
				   query.heaps.array);

			igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
						sizeof(query), &query, 0);

			igt_fail_on(query.heaps.count != kmd_count ||
				    query.heaps.stride != sizeof(*heaps) ||
				    query.heaps.array != (uintptr_t)heaps);

			free(heaps);
		}
	}

	igt_describe("Query, with two ioctl calls, heaps where UMD(stride) > KMD(stride)");
	igt_subtest("heap-info-get-2-stride-umd-gt-kmd")
	{
		struct drm_pvr_dev_query_heap_info query = {
			.heaps = { .stride = ~0U, .count = ~0U, },
		};
		struct drm_pvr_heap heaps = { 0 };
		uint32_t umd_stride, kmd_stride;

		igt_assert(query.heaps.count &&
			   query.heaps.stride &&
			   !query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count == ~0U ||
			    query.heaps.stride == ~0U ||
			    query.heaps.array);

		kmd_stride = query.heaps.stride;
		umd_stride = ALIGN(kmd_stride + 1, sizeof(uint64_t));

		igt_assert(kmd_stride && umd_stride > kmd_stride);

		query = (struct drm_pvr_dev_query_heap_info){
			.heaps = {
				.count = 1,
				.stride = umd_stride,
				.array = (uintptr_t)&heaps,
			},
		};

		igt_assert(query.heaps.count &&
			   query.heaps.stride > kmd_stride &&
			   query.heaps.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.heaps.count != 1 ||
			    query.heaps.stride != umd_stride ||
			    query.heaps.array != (uintptr_t)&heaps);
	}

#undef LEGACY_STRIDE
#undef LEGACY_COUNT

	/***********************************************************************
	   DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET
	 ***********************************************************************/

	igt_describe("Test validity of static data areas");
	igt_subtest("heap-info-static-data-areas-valid")
	{
		uint32_t heaps_count = 0;
		struct drm_pvr_heap *heaps =
			igt_pvr_get_heap_info(fd, &heaps_count);

		uint32_t sda_count = 0;
		struct drm_pvr_static_data_area *static_data_areas =
			igt_pvr_get_static_data_areas(fd, &sda_count);

		igt_assert(heaps);
		igt_assert(static_data_areas);

		for (uint32_t i = 0; i < sda_count; i++) {
			const uint64_t start_offset = static_data_areas[i].offset;
			const uint64_t end_offset =
				start_offset + static_data_areas[i].size;
			const struct drm_pvr_heap *const heap =
				&heaps[static_data_areas[i].location_heap_id];

			igt_assert_lte(static_data_areas[i].area_usage,
				       DRM_PVR_STATIC_DATA_AREA_YUV_CSC);
			igt_assert_lt(static_data_areas[i].location_heap_id,
				      DRM_PVR_HEAP_COUNT);

			if (!static_data_areas[i].size)
				/* Not present for device. */
				continue;

			/* Ensure reported range is inside the heap. */
			igt_assert_lte_u64(start_offset, heap->size);
			igt_assert_lte_u64(end_offset, heap->size);
		}

		free(heaps);
		free(static_data_areas);
	}

/* Value of stride and count as defined in the first released version of the UAPI. */
#define LEGACY_STRIDE (offsetof(struct drm_pvr_static_data_area, offset) + sizeof(areas->offset))
#define LEGACY_COUNT (5U)

	igt_describe("Query, with one ioctl call, zero static areas");
	igt_subtest("heap-info-static-data-areas-get-1-count-zero")
	{
		struct drm_pvr_static_data_area areas = { 0 };
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = DRM_PVR_OBJ_ARRAY(0, &areas),
		};

		igt_assert(!query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count ||
			    query.static_data_areas.stride != sizeof(areas) ||
			    query.static_data_areas.array != (uintptr_t)&areas);
	}

	igt_describe("Query, with one ioctl call, one static area using an invalid stride");
	igt_subtest("heap-info-static-data-areas-get-1-stride-too-small")
	{
		struct drm_pvr_static_data_area areas = { 0 };
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = DRM_PVR_OBJ_ARRAY(1, &areas),
		};

		query.static_data_areas.stride = 1;

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, EINVAL);
	}

	igt_describe("Query, with one ioctl call, the legacy value of stride and count");
	igt_subtest("heap-info-static-data-areas-get-1-legacy")
	{
		struct drm_pvr_dev_query_static_data_areas query = { 0 };
		struct drm_pvr_static_data_area *areas = NULL;

		igt_assert(!query.static_data_areas.count &&
			   !query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count != LEGACY_COUNT ||
			    query.static_data_areas.stride != LEGACY_STRIDE ||
			    query.static_data_areas.array);
	}

	igt_describe("Query, with two ioctl calls, all static areas known to the kernel");
	igt_subtest("heap-info-static-data-areas-get-2")
	{
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t ioctl1_count = ~0U, ioctl1_stride = ~0U;
		struct drm_pvr_static_data_area *areas = NULL;

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.stride == ~0U ||
			    query.static_data_areas.count == ~0U ||
			    query.static_data_areas.array);

		ioctl1_stride = query.static_data_areas.stride;
		ioctl1_count = query.static_data_areas.count;

		areas = calloc(ioctl1_count, ioctl1_stride);
		igt_fail_on(!areas);

		query.static_data_areas = (struct drm_pvr_obj_array)
			DRM_PVR_OBJ_ARRAY(query.static_data_areas.count, areas);

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count != ioctl1_count ||
			    query.static_data_areas.stride != ioctl1_stride ||
			    query.static_data_areas.array != (uintptr_t)areas);

		free(areas);
	}

	igt_describe("Query, with two ioctl calls, the legacy value of stride and count");
	igt_subtest("heap-info-static-data-areas-get-2-legacy")
	{
		struct drm_pvr_dev_query_static_data_areas query = { 0 };
		struct drm_pvr_static_data_area *areas = NULL;

		igt_assert(!query.static_data_areas.count &&
			   !query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count != LEGACY_COUNT ||
			    query.static_data_areas.stride != LEGACY_STRIDE ||
			    query.static_data_areas.array);

		areas = calloc(query.static_data_areas.count, sizeof(*areas));
		igt_fail_on(!areas);

		query.static_data_areas = (struct drm_pvr_obj_array)
			DRM_PVR_OBJ_ARRAY(query.static_data_areas.count, areas);

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count != LEGACY_COUNT ||
			    query.static_data_areas.stride != LEGACY_STRIDE ||
			    query.static_data_areas.array != (uintptr_t)areas);

		free(areas);
	}

	igt_describe("Query, with two ioctl calls, static areas where UMD(count) < KMD(count)");
	igt_subtest("heap-info-static-data-areas-get-2-count-umd-lt-kmd")
	{
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t umd_count, kmd_count;

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count == ~0U ||
			    query.static_data_areas.stride == ~0U ||
			    query.static_data_areas.array);

		kmd_count = query.static_data_areas.count;
		umd_count = kmd_count - 1;

		igt_assert(kmd_count && umd_count < kmd_count);

		for (uint32_t count = umd_count; count; count--) {
			struct drm_pvr_static_data_area *areas = calloc(count, sizeof(*areas));
			query = (struct drm_pvr_dev_query_static_data_areas){
				.static_data_areas = DRM_PVR_OBJ_ARRAY(count, areas),
			};

			igt_assert(query.static_data_areas.count < kmd_count &&
				   query.static_data_areas.stride &&
				   query.static_data_areas.array);

			igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
						sizeof(query), &query, 0);

			igt_fail_on(query.static_data_areas.count != count ||
				    query.static_data_areas.stride != sizeof(*areas) ||
				    query.static_data_areas.array != (uintptr_t)areas);

			free(areas);
		}
	}

	igt_describe("Query, with two ioctl calls, static areas where UMD(count) > KMD(count)");
	igt_subtest("heap-info-static-data-areas-get-2-count-umd-gt-kmd")
	{
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = { .stride = ~0U, .count = ~0U, },
		};
		uint32_t umd_count, kmd_count;

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count == ~0U ||
			    query.static_data_areas.stride == ~0U ||
			    query.static_data_areas.array);

		kmd_count = query.static_data_areas.count;
		umd_count = kmd_count * 2;

		igt_assert(kmd_count && umd_count > kmd_count);

		for (uint32_t count = umd_count; count > kmd_count; count--) {
			struct drm_pvr_static_data_area *areas = calloc(count, sizeof(*areas));
			query = (struct drm_pvr_dev_query_static_data_areas){
				.static_data_areas = DRM_PVR_OBJ_ARRAY(count, areas),
			};

			igt_assert(query.static_data_areas.count > kmd_count &&
				   query.static_data_areas.stride &&
				   query.static_data_areas.array);

			igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
						sizeof(query), &query, 0);

			igt_fail_on(query.static_data_areas.count != kmd_count ||
				    query.static_data_areas.stride != sizeof(*areas) ||
				    query.static_data_areas.array != (uintptr_t)areas);

			free(areas);
		}
	}

	igt_describe("Query, with two ioctl calls, static areas where UMD(stride) > KMD(stride)");
	igt_subtest("heap-info-static-data-areas-get-2-stride-umd-gt-kmd")
	{
		struct drm_pvr_dev_query_static_data_areas query = {
			.static_data_areas = { .stride = ~0U, .count = ~0U, },
		};
		struct drm_pvr_static_data_area areas = { 0 };
		uint32_t umd_stride, kmd_stride;

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride &&
			   !query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count == ~0U ||
			    query.static_data_areas.stride == ~0U ||
			    query.static_data_areas.array);

		kmd_stride = query.static_data_areas.stride;
		umd_stride = ALIGN(kmd_stride + 1, sizeof(uint64_t));

		igt_assert(kmd_stride && umd_stride > kmd_stride);

		query = (struct drm_pvr_dev_query_static_data_areas){
			.static_data_areas = {
				.count = 1,
				.stride = umd_stride,
				.array = (uintptr_t)&areas,
			},
		};

		igt_assert(query.static_data_areas.count &&
			   query.static_data_areas.stride > kmd_stride &&
			   query.static_data_areas.array);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
					sizeof(query), &query, 0);

		igt_fail_on(query.static_data_areas.count != 1 ||
			    query.static_data_areas.stride != umd_stride ||
			    query.static_data_areas.array != (uintptr_t)&areas);
	}

#undef LEGACY_STRIDE
#undef LEGACY_COUNT

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
