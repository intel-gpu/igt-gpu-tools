// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

int igt_main()
{
	const enum drm_pvr_dev_query types[] = {
		DRM_PVR_DEV_QUERY_GPU_INFO_GET,
		DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET,
		DRM_PVR_DEV_QUERY_QUIRKS_GET,
		DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET,
		DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
		DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
		DRM_PVR_DEV_QUERY_FW_INFO_GET,
	};

	const uint64_t sizes[] = {
		[DRM_PVR_DEV_QUERY_GPU_INFO_GET] =
			sizeof(struct drm_pvr_dev_query_gpu_info),
		[DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET] =
			sizeof(struct drm_pvr_dev_query_runtime_info),
		[DRM_PVR_DEV_QUERY_QUIRKS_GET] =
			sizeof(struct drm_pvr_dev_query_quirks),
		[DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET] =
			sizeof(struct drm_pvr_dev_query_enhancements),
		[DRM_PVR_DEV_QUERY_HEAP_INFO_GET] =
			sizeof(struct drm_pvr_dev_query_heap_info),
		[DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET] =
			sizeof(struct drm_pvr_dev_query_static_data_areas),
		[DRM_PVR_DEV_QUERY_FW_INFO_GET] =
			sizeof(struct drm_pvr_dev_query_fw_info),
	};

	struct drm_pvr_dev_query_gpu_info gpu_info = {0};
	struct drm_pvr_dev_query_runtime_info runtime_info = {0};
	struct drm_pvr_dev_query_quirks quirks = {0};
	struct drm_pvr_dev_query_enhancements enhancements = {0};
	struct drm_pvr_dev_query_heap_info heap_info = {0};
	struct drm_pvr_dev_query_static_data_areas static_data_areas = {0};
	struct drm_pvr_dev_query_fw_info fw_info = {0};

	void *const containers[] = {
		[DRM_PVR_DEV_QUERY_GPU_INFO_GET] = &gpu_info,
		[DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET] = &runtime_info,
		[DRM_PVR_DEV_QUERY_QUIRKS_GET] = &quirks,
		[DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET] = &enhancements,
		[DRM_PVR_DEV_QUERY_HEAP_INFO_GET] = &heap_info,
		[DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET] = &static_data_areas,
		[DRM_PVR_DEV_QUERY_FW_INFO_GET] = &fw_info,
	};

	int fd, version_major, version_minor;

	static_assert(ARRAY_SIZE(types) == ARRAY_SIZE(sizes));
	static_assert(ARRAY_SIZE(types) == ARRAY_SIZE(containers));

	igt_fixture()
	{
		drmVersionPtr version;
		fd = drm_open_driver(DRIVER_POWERVR);

		for (int i = 0; i < ARRAY_SIZE(types); i++)
			memset(containers[i], 0, sizes[i]);

		version = drmGetVersion(fd);
		version_major = version->version_major;
		version_minor = version->version_minor;
		drmFreeVersion(version);
	}

	igt_describe("Test invalid dev query");
	igt_subtest("dev-query-invalid") {
		igt_pvr_ioctl_dev_query(fd, (enum drm_pvr_dev_query)-1, 0, NULL,
					EINVAL);
	}

	igt_describe("Test valid dev query");
	igt_subtest("dev-query-success-all") {
		struct drm_pvr_ioctl_dev_query_args args;
		for (int i = 0; i < ARRAY_SIZE(types); i++) {

			/* DRM_PVR_DEV_QUERY_FW_INFO_GET only supported
			 * from major version 1 and minor version 1 onwards.
			 * Skip query on older version.
			 */
			if (types[i] == DRM_PVR_DEV_QUERY_FW_INFO_GET &&
				version_major == 1 && version_minor < 1) {
				igt_log("igt-pvr", IGT_LOG_INFO, "Skipping FW_INFO_GET query due to no support\n");
				continue;
			}

			args = igt_pvr_ioctl_dev_query(fd, types[i], 0, NULL, 0);

			igt_assert(args.size);
			igt_assert(args.size <= sizes[i]);

			igt_pvr_ioctl_dev_query(fd, types[i], args.size, containers[i], 0);
		}
	}

	igt_describe("Test dev query with too small size");
	igt_subtest("dev-query-fail-too-small") {
		for (int i = 0; i < ARRAY_SIZE(types); i++) {
			igt_pvr_ioctl_dev_query(fd, types[i], 0, containers[i], EINVAL);
			igt_pvr_ioctl_dev_query(fd, types[i], 1, containers[i],	EINVAL);
		}
	}

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
