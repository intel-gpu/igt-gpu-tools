// SPDX-License-Identifier: MIT
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <errno.h>

#include "igt.h"
#include "igt_msm.h"

IGT_TEST_DESCRIPTION("MSM buffer object management");

#define DEFAULT_BUFFER_SIZE 4096

int igt_main()
{
	struct msm_device *dev = NULL;

	igt_fixture() {
		dev = igt_msm_dev_open();
	}

	/* Buffer Allocation Tests */
	igt_describe("Test basic buffer object allocation");
	igt_subtest("bo-alloc-basic") {
		struct msm_bo *bo;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");
		igt_assert_f(bo->handle != 0, "Buffer object handle is 0\n");
		igt_assert_f(bo->size == DEFAULT_BUFFER_SIZE,
			     "Buffer size mismatch: expected %d, got %u\n",
			     (unsigned int)DEFAULT_BUFFER_SIZE, (unsigned int)bo->size);

		igt_msm_bo_free(bo);
	}

	igt_describe("Test write-combine buffer object allocation");
	igt_subtest("bo-alloc-writecombine") {
		struct msm_bo *bo;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate WC buffer object\n");
		igt_assert_f(bo->handle != 0, "Buffer object handle is 0\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Test uncached buffer object allocation (deprecated flag)");
	igt_subtest("bo-alloc-uncached") {
		struct drm_msm_gem_new req = { 0 };
		int ret;

		req.size = DEFAULT_BUFFER_SIZE;
		req.flags = MSM_BO_UNCACHED;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req);
		if (ret == 0) {
			igt_info("MSM_BO_UNCACHED supported on this kernel\n");
			gem_close(dev->fd, req.handle);
		} else {
			igt_assert_eq(errno, EINVAL);
			igt_info("MSM_BO_UNCACHED not supported (deprecated)\n");
		}
	}

	igt_describe("Test allocation with zero size (should fail)");
	igt_subtest("bo-alloc-zero-size") {
		struct drm_msm_gem_new req = { 0 };

		req.size = 0;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req, EINVAL);
	}

	igt_describe("Test allocation with invalid flags (should fail)");
	igt_subtest("bo-alloc-invalid-flags") {
		struct drm_msm_gem_new req = { 0 };

		req.size = DEFAULT_BUFFER_SIZE;
		req.flags = (1 << 25); /* Invalid flag bit outside MSM_BO_FLAGS */

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req, EINVAL);
	}

	igt_describe("Test allocation with unaligned size");
	igt_subtest("bo-alloc-unaligned-size") {
		struct msm_bo *bo;
		size_t size = DEFAULT_BUFFER_SIZE - 3; /* Unaligned size */

		bo = igt_msm_bo_new(dev, size, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer with unaligned size\n");
		igt_assert_f(bo->handle != 0, "Buffer object handle is 0\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Test MSM_BO_CACHED_COHERENT on A6xx+ GPUs");
	igt_subtest("bo-alloc-cached-coherent") {
		struct drm_msm_gem_new req = { 0 };
		int ret;

		/* MSM_BO_CACHED_COHERENT is only supported on A6xx and newer GPUs */
		igt_require(dev->gen >= 6);

		req.size = DEFAULT_BUFFER_SIZE;
		req.flags = MSM_BO_CACHED_COHERENT;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req);

		if (ret == 0) {
			struct drm_msm_gem_info info_req = { 0 };

			igt_info("MSM_BO_CACHED_COHERENT supported on A%dxx GPU\n", dev->gen);

			/* Verify flags */
			info_req.handle = req.handle;
			info_req.info = MSM_INFO_GET_FLAGS;
			ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &info_req);
			igt_assert_eq(ret, 0);

			igt_assert_f((info_req.value & MSM_BO_CACHE_MASK) == MSM_BO_CACHED_COHERENT,
				     "Cache mode mismatch for CACHED_COHERENT\n");

			igt_info("CACHED_COHERENT BO flags: 0x%llx (expected 0x%x)\n",
				 (unsigned long long)info_req.value,
				 MSM_BO_CACHED_COHERENT);

			gem_close(dev->fd, req.handle);
		} else {
			igt_assert_eq(errno, EINVAL);
			igt_info("MSM_BO_CACHED_COHERENT not supported on A%dxx GPU "
				 "(EINVAL)\n", dev->gen);
		}
	}

	/* Buffer Information Tests - Positive Cases */
	igt_describe("Get offset for valid BO");
	igt_subtest("bo-get-offset-valid") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.info = MSM_INFO_GET_OFFSET;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);
		igt_assert_f(req.value != 0, "Got zero offset for valid BO\n");

		igt_info("BO handle %u: offset = 0x%llx\n", bo->handle,
			 (unsigned long long)req.value);

		igt_msm_bo_free(bo);
	}

	igt_describe("Get IOVA for valid BO");
	igt_subtest("bo-get-iova-valid") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.info = MSM_INFO_GET_IOVA;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);
		igt_assert_f(req.value != 0, "Got zero IOVA for valid BO\n");

		igt_info("BO handle %u: IOVA = 0x%llx\n", bo->handle,
			 (unsigned long long)req.value);

		/* Verify IOVA matches what's stored in bo structure */
		igt_assert_eq_u64(req.value, bo->iova);

		igt_msm_bo_free(bo);
	}

	igt_describe("Get flags for valid BO");
	igt_subtest("bo-get-flags") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		int ret;
		uint32_t test_flags = MSM_BO_WC | MSM_BO_GPU_READONLY;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, test_flags);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.info = MSM_INFO_GET_FLAGS;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);

		igt_info("BO handle %u: flags = 0x%llx (expected 0x%x)\n",
			 bo->handle, (unsigned long long)req.value, test_flags);

		/* Verify flags match what we requested */
		igt_assert_eq_u64(req.value & test_flags, test_flags);

		igt_msm_bo_free(bo);
	}

	igt_describe("Verify offset consistency across multiple queries");
	igt_subtest("bo-get-offset-consistency") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req1 = { 0 }, req2 = { 0 };
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* First query */
		req1.handle = bo->handle;
		req1.info = MSM_INFO_GET_OFFSET;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req1);
		igt_assert_eq(ret, 0);

		/* Second query */
		req2.handle = bo->handle;
		req2.info = MSM_INFO_GET_OFFSET;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req2);
		igt_assert_eq(ret, 0);

		/* Verify consistency */
		igt_assert_eq_u64(req1.value, req2.value);
		igt_info("Offset consistent across queries: 0x%llx\n",
			 (unsigned long long)req1.value);

		igt_msm_bo_free(bo);
	}

	igt_describe("Verify IOVA consistency across multiple queries");
	igt_subtest("bo-get-iova-consistency") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req1 = { 0 }, req2 = { 0 };
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* First query */
		req1.handle = bo->handle;
		req1.info = MSM_INFO_GET_IOVA;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req1);
		igt_assert_eq(ret, 0);

		/* Second query */
		req2.handle = bo->handle;
		req2.info = MSM_INFO_GET_IOVA;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req2);
		igt_assert_eq(ret, 0);

		/* Verify consistency */
		igt_assert_eq_u64(req1.value, req2.value);
		igt_info("IOVA consistent across queries: 0x%llx\n",
			 (unsigned long long)req1.value);

		igt_msm_bo_free(bo);
	}

	igt_describe("Test getting info for multiple BOs");
	igt_subtest("bo-get-info-multiple") {
		struct msm_bo *bo1, *bo2, *bo3;
		struct drm_msm_gem_info req = { 0 };
		int ret;

		/* Allocate multiple BOs */
		bo1 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		bo2 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE * 2, MSM_BO_WC);
		bo3 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE * 4, MSM_BO_CACHED);

		igt_assert_f(bo1 && bo2 && bo3, "Failed to allocate BOs\n");

		/* Get info for each BO */
		req.handle = bo1->handle;
		req.info = MSM_INFO_GET_IOVA;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);
		igt_info("BO1 IOVA: 0x%llx\n", (unsigned long long)req.value);

		req.handle = bo2->handle;
		req.info = MSM_INFO_GET_IOVA;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);
		igt_info("BO2 IOVA: 0x%llx\n", (unsigned long long)req.value);

		req.handle = bo3->handle;
		req.info = MSM_INFO_GET_IOVA;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);
		igt_info("BO3 IOVA: 0x%llx\n", (unsigned long long)req.value);

		/* Verify IOVAs are different */
		igt_assert_neq_u64(bo1->iova, bo2->iova);
		igt_assert_neq_u64(bo2->iova, bo3->iova);
		igt_assert_neq_u64(bo1->iova, bo3->iova);

		igt_msm_bo_free(bo1);
		igt_msm_bo_free(bo2);
		igt_msm_bo_free(bo3);
	}

	igt_describe("Test getting flags for BOs with different cache modes");
	igt_subtest("bo-get-flags-cache-modes") {
		struct {
			uint32_t flags;
			const char *name;
			bool may_fail; /* Some flags may not be supported */
		} test_cases[] = {
			{ MSM_BO_CACHED, "CACHED", false },
			{ MSM_BO_WC, "WC", false },
			{ MSM_BO_UNCACHED, "UNCACHED", true }, /* Deprecated */
			{ MSM_BO_CACHED_COHERENT, "CACHED_COHERENT", true }, /* May not be supported */
		};

		for (unsigned int i = 0; i < ARRAY_SIZE(test_cases); i++) {
			struct drm_msm_gem_new alloc_req = { 0 };
			struct drm_msm_gem_info info_req = { 0 };
			int ret;

			alloc_req.size = DEFAULT_BUFFER_SIZE;
			alloc_req.flags = test_cases[i].flags;

			ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &alloc_req);

			if (ret != 0) {
				if (test_cases[i].may_fail && errno == EINVAL) {
					igt_info("%s BO not supported (EINVAL)\n", test_cases[i].name);
					continue;
				}
				igt_assert_f(false, "Failed to allocate %s BO: %s\n",
					     test_cases[i].name, strerror(errno));
			}

			info_req.handle = alloc_req.handle;
			info_req.info = MSM_INFO_GET_FLAGS;

			ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &info_req);
			igt_assert_eq(ret, 0);

			igt_info("%s BO flags: 0x%llx (expected 0x%x)\n",
				 test_cases[i].name,
				 (unsigned long long)info_req.value,
				 test_cases[i].flags);

			/*
			 * MSM_BO_UNCACHED is deprecated in favour of MSM_BO_WC, so a
			 * kernel that still accepts it may report either cache mode.
			 */
			if (test_cases[i].flags == MSM_BO_UNCACHED) {
				uint64_t mode = info_req.value & MSM_BO_CACHE_MASK;

				igt_assert_f(mode == MSM_BO_UNCACHED || mode == MSM_BO_WC,
					     "Cache mode mismatch for %s\n", test_cases[i].name);
				gem_close(dev->fd, alloc_req.handle);
				continue;
			}

			/* Verify the cache mode flag is set */
			igt_assert_f((info_req.value & MSM_BO_CACHE_MASK) == test_cases[i].flags,
				     "Cache mode mismatch for %s\n", test_cases[i].name);

			gem_close(dev->fd, alloc_req.handle);
		}
	}

	/* Buffer Information Tests - Negative Cases */
	igt_describe("Get offset for invalid handle (expects ENOENT)");
	igt_subtest("bo-get-offset-invalid") {
		struct drm_msm_gem_info req = { 0 };

		req.handle = 0xDEADBEEF; /* Invalid handle */
		req.info = MSM_INFO_GET_OFFSET;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, ENOENT);

		igt_info("Correctly rejected invalid handle with ENOENT\n");
	}

	igt_describe("Get IOVA for invalid handle (expects ENOENT)");
	igt_subtest("bo-get-iova-invalid") {
		struct drm_msm_gem_info req = { 0 };

		req.handle = 0xDEADBEEF; /* Invalid handle */
		req.info = MSM_INFO_GET_IOVA;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, ENOENT);

		igt_info("Correctly rejected invalid handle with ENOENT\n");
	}

	igt_describe("Use invalid info type (expects EINVAL)");
	igt_subtest("bo-invalid-info-type") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.info = 0xFFFFFFFF; /* Invalid info type */

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, EINVAL);

		igt_info("Correctly rejected invalid info type with EINVAL\n");

		igt_msm_bo_free(bo);
	}

	/* Memory Advice Tests - Positive Cases */
	igt_describe("Set WILLNEED advice and verify that the buffer is retained");
	igt_subtest("bo-madvise-willneed") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_WILLNEED;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);
		igt_assert_eq(req.retained, 1);

		igt_msm_bo_free(bo);
	}

	igt_describe("Set DONTNEED advice on a buffer object");
	igt_subtest("bo-madvise-dontneed") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_DONTNEED;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);

		igt_info("Buffer retained status after DONTNEED: %d\n", req.retained);
		igt_msm_bo_free(bo);
	}

	igt_describe("Apply WILLNEED and DONTNEED advice to the same buffer");
	igt_subtest("bo-madvise-multiple-calls") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_WILLNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);
		igt_assert_eq(req.retained, 1);

		req.madv = MSM_MADV_DONTNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);

		igt_info("Buffer retained status after DONTNEED: %d\n", req.retained);
		igt_msm_bo_free(bo);
	}

	igt_describe("Apply DONTNEED advice to a mapped buffer object");
	igt_subtest("bo-madvise-mapped-buffer") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		void *map;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_DONTNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);

		igt_info("Mapped buffer retained status: %d\n", req.retained);
		igt_msm_bo_free(bo);
	}

	igt_describe("Apply WILLNEED advice to a cached buffer object");
	igt_subtest("bo-madvise-cached-buffer") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_CACHED);
		igt_assert_f(bo, "Failed to allocate cached buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_WILLNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);
		igt_assert_eq(req.retained, 1);

		igt_msm_bo_free(bo);
	}

	igt_describe("Apply DONTNEED advice to a large buffer object");
	igt_subtest("bo-madvise-large-buffer") {
		struct drm_msm_gem_madvise req = { 0 };
		const size_t large_size = 64 * 1024 * 1024;
		struct msm_bo *bo;
		int ret;

		bo = igt_msm_bo_new(dev, large_size, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate large buffer object\n");

		req.handle = bo->handle;
		req.madv = MSM_MADV_DONTNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);

		igt_info("Large buffer retained status: %d\n", req.retained);
		igt_msm_bo_free(bo);
	}

	igt_describe("Apply memory advice to multiple buffer objects");
	igt_subtest("bo-madvise-multiple-buffers") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo1, *bo2;
		int ret;

		bo1 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		bo2 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE * 2, MSM_BO_CACHED);
		igt_assert_f(bo1 && bo2, "Failed to allocate buffer objects\n");

		req.handle = bo1->handle;
		req.madv = MSM_MADV_WILLNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);
		igt_assert_eq(req.retained, 1);

		req.handle = bo2->handle;
		req.madv = MSM_MADV_DONTNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
		igt_assert_eq(ret, 0);

		igt_info("Second buffer retained status: %d\n", req.retained);
		igt_msm_bo_free(bo1);
		igt_msm_bo_free(bo2);
	}

	igt_describe("Apply DONTNEED advice after balanced CPU access");
	igt_subtest("bo-madvise-after-cpu-prep") {
		struct drm_msm_gem_cpu_prep prep_req = { 0 };
		struct drm_msm_gem_cpu_fini fini_req = { 0 };
		struct drm_msm_gem_madvise madv_req = { 0 };
		struct msm_bo *bo;
		uint32_t *ptr;
		void *map;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer object\n");
		ptr = map;

		prep_req.handle = bo->handle;
		prep_req.op = MSM_PREP_READ | MSM_PREP_WRITE | MSM_PREP_NOSYNC;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_CPU_PREP, &prep_req);
		igt_assert_eq(ret, 0);

		ptr[0] = 0xDEADBEEF;

		fini_req.handle = bo->handle;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_CPU_FINI, &fini_req);
		igt_assert_eq(ret, 0);

		madv_req.handle = bo->handle;
		madv_req.madv = MSM_MADV_DONTNEED;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &madv_req);
		igt_assert_eq(ret, 0);

		igt_info("Buffer retained status: %d\n", madv_req.retained);
		igt_msm_bo_free(bo);
	}

	/* Memory Advice Tests - Negative Cases */
	igt_describe("Reject memory advice for an invalid buffer handle");
	igt_subtest("bo-madvise-invalid-bo") {
		struct drm_msm_gem_madvise req = { 0 };

		req.handle = 0xDEADBEEF;
		req.madv = MSM_MADV_WILLNEED;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req, ENOENT);
	}

	igt_describe("Reject memory advice for handle zero");
	igt_subtest("bo-madvise-zero-handle") {
		struct drm_msm_gem_madvise req = { 0 };

		req.handle = 0;
		req.madv = MSM_MADV_WILLNEED;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req, ENOENT);
	}

	igt_describe("Reject an invalid memory advice value");
	igt_subtest("bo-madvise-invalid-advice") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		req.handle = bo->handle;
		req.madv = 0xDEADBEEF;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req, EINVAL);
		igt_msm_bo_free(bo);
	}

	igt_describe("Reject memory advice after a buffer has been freed");
	igt_subtest("bo-madvise-after-free") {
		struct drm_msm_gem_madvise req = { 0 };
		struct msm_bo *bo;
		uint32_t handle;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");
		handle = bo->handle;

		igt_msm_bo_free(bo);

		req.handle = handle;
		req.madv = MSM_MADV_WILLNEED;

		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req, ENOENT);
	}

	igt_fixture() {
		igt_msm_dev_close(dev);
	}
}
