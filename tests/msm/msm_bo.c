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

	/* Buffer Mapping Tests - Positive Cases */
	igt_describe("Basic mapping with read/write test");
	igt_subtest("bo-map-basic") {
		struct msm_bo *bo;
		void *map;
		uint32_t *ptr;
		const uint32_t test_pattern = 0xDEADBEEF;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer object\n");
		igt_assert_f(map == bo->map, "Map pointer mismatch\n");

		/* Write test pattern */
		ptr = (uint32_t *)map;
		ptr[0] = test_pattern;
		ptr[1] = test_pattern + 1;
		ptr[DEFAULT_BUFFER_SIZE / sizeof(uint32_t) - 1] = test_pattern + 2;

		/* Read back and verify */
		igt_assert_eq_u32(ptr[0], test_pattern);
		igt_assert_eq_u32(ptr[1], test_pattern + 1);
		igt_assert_eq_u32(ptr[DEFAULT_BUFFER_SIZE / sizeof(uint32_t) - 1],
				  test_pattern + 2);

		igt_info("Successfully mapped and verified read/write to BO\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Map cached buffer");
	igt_subtest("bo-map-cached") {
		struct msm_bo *bo;
		void *map;
		uint32_t *ptr;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_CACHED);
		igt_assert_f(bo, "Failed to allocate cached buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map cached buffer object\n");

		/* Write and read test */
		ptr = (uint32_t *)map;
		for (int i = 0; i < 256; i++)
			ptr[i] = 0x12340000 + i;

		for (int i = 0; i < 256; i++)
			igt_assert_eq_u32(ptr[i], 0x12340000 + i);

		igt_info("Successfully mapped and verified cached BO\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Map write-combine buffer");
	igt_subtest("bo-map-wc") {
		struct msm_bo *bo;
		void *map;
		uint32_t *ptr;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate WC buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map WC buffer object\n");

		/* Write and read test */
		ptr = (uint32_t *)map;
		ptr[0] = 0xCAFEBABE;
		ptr[100] = 0xFEEDFACE;

		igt_assert_eq_u32(ptr[0], 0xCAFEBABE);
		igt_assert_eq_u32(ptr[100], 0xFEEDFACE);

		igt_info("Successfully mapped and verified WC BO\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Map same buffer twice");
	igt_subtest("bo-double-map") {
		struct msm_bo *bo;
		void *map1, *map2;
		uint32_t *ptr1, *ptr2;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* First map */
		map1 = igt_msm_bo_map(bo);
		igt_assert_f(map1, "First map failed\n");

		/* Second map - should return same pointer */
		map2 = igt_msm_bo_map(bo);
		igt_assert_f(map2, "Second map failed\n");
		igt_assert_f(map1 == map2, "Double map returned different pointers\n");

		/* Verify both pointers work */
		ptr1 = (uint32_t *)map1;
		ptr2 = (uint32_t *)map2;

		ptr1[0] = 0x11111111;
		igt_assert_eq_u32(ptr2[0], 0x11111111);

		ptr2[1] = 0x22222222;
		igt_assert_eq_u32(ptr1[1], 0x22222222);

		igt_info("Double map returned same pointer as expected\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Unmap and remap buffer");
	igt_subtest("bo-remap") {
		struct msm_bo *bo;
		void *map1, *map2;
		uint32_t *ptr;
		const uint32_t test_value = 0xABCDEF01;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* First map and write */
		map1 = igt_msm_bo_map(bo);
		igt_assert_f(map1, "First map failed\n");

		ptr = (uint32_t *)map1;
		ptr[0] = test_value;

		/* Manually unmap */
		munmap(bo->map, bo->size);
		bo->map = NULL;

		/* Remap */
		map2 = igt_msm_bo_map(bo);
		igt_assert_f(map2, "Remap failed\n");

		/* Verify data persists */
		ptr = (uint32_t *)map2;
		igt_assert_eq_u32(ptr[0], test_value);

		igt_info("Successfully remapped BO and verified data persistence\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Map buffer, write data, unmap, remap, verify data persists");
	igt_subtest("bo-map-write-read-consistency") {
		struct msm_bo *bo;
		void *map;
		uint32_t *ptr;
		const uint32_t test_data[] = { 0xAABBCCDD, 0x11223344, 0xDEADBEEF, 0xCAFEBABE };

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* First map and write */
		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer object\n");
		ptr = (uint32_t *)map;
		for (unsigned int i = 0; i < ARRAY_SIZE(test_data); i++)
			ptr[i] = test_data[i];

		/* Unmap */
		munmap(bo->map, bo->size);
		bo->map = NULL;

		/* Remap and verify */
		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to remap buffer object\n");
		ptr = (uint32_t *)map;
		for (unsigned int i = 0; i < ARRAY_SIZE(test_data); i++)
			igt_assert_eq_u32(ptr[i], test_data[i]);

		igt_info("Data integrity verified across map/unmap cycles\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Test mapping buffers of various sizes");
	igt_subtest("bo-map-different-sizes") {
		struct {
			size_t size;
			const char *name;
		} test_cases[] = {
			{ 4096, "4KB" },
			{ 16384, "16KB" },
			{ 65536, "64KB" },
			{ 1048576, "1MB" },
		};

		for (unsigned int i = 0; i < ARRAY_SIZE(test_cases); i++) {
			struct msm_bo *bo;
			void *map;
			uint32_t *ptr;
			size_t last;

			bo = igt_msm_bo_new(dev, test_cases[i].size, MSM_BO_WC);
			igt_assert_f(bo, "Failed to allocate %s buffer\n", test_cases[i].name);

			map = igt_msm_bo_map(bo);
			igt_assert_f(map, "Failed to map %s buffer\n", test_cases[i].name);

			/* Write and verify at start and end */
			ptr = map;
			last = test_cases[i].size / sizeof(*ptr) - 1;

			ptr[0] = 0x12345678;
			ptr[last] = 0x87654321;

			igt_assert_eq_u32(ptr[0], 0x12345678);
			igt_assert_eq_u32(ptr[last], 0x87654321);

			igt_info("Successfully mapped and verified %s buffer\n",
				 test_cases[i].name);

			igt_msm_bo_free(bo);
		}
	}

	igt_describe("Use DRM_IOCTL_MSM_GEM_CPU_PREP before mapping");
	igt_subtest("bo-map-after-cpu-prep") {
		struct msm_bo *bo;
		struct drm_msm_gem_cpu_prep prep_req = { 0 };
		void *map;
		uint32_t *ptr;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* CPU prep with zero timeout */
		prep_req.handle = bo->handle;
		prep_req.op = MSM_PREP_READ | MSM_PREP_WRITE | MSM_PREP_NOSYNC;

		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_CPU_PREP, &prep_req);
		igt_assert_eq(ret, 0);

		/* Now map and use the buffer */
		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer after CPU prep\n");

		ptr = (uint32_t *)map;
		ptr[0] = 0xABCDEF01;
		igt_assert_eq_u32(ptr[0], 0xABCDEF01);

		igt_info("Successfully mapped and used buffer after CPU prep\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Try to map using an invalid offset");
	igt_subtest("bo-map-invalid-offset") {
		void *map;

		/* Try to map with invalid/bogus offset should fail */
		map = mmap(0, DEFAULT_BUFFER_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, dev->fd, 0xDEADBEEF000);
		igt_assert_f(map == MAP_FAILED, "Mapping with invalid offset should fail\n");

		igt_info("Correctly rejected mapping with invalid offset\n");
	}

	igt_describe("Verify mapped addresses are properly aligned");
	igt_subtest("bo-map-alignment-test") {
		struct msm_bo *bo;
		void *map;
		uintptr_t addr;
		long page_size = sysconf(_SC_PAGESIZE);

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map buffer object\n");

		addr = (uintptr_t)map;
		igt_assert_f((addr % page_size) == 0,
			     "Mapped address 0x%lx is not page-aligned (page size: %ld)\n",
			     addr, page_size);

		igt_info("Mapped address 0x%lx is properly aligned to page size %ld\n",
			 addr, page_size);

		igt_msm_bo_free(bo);
	}

	igt_describe("Test different mmap protection flags");
	igt_subtest("bo-map-protection-flags") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		void *map_rw, *map_ro;
		uint32_t *ptr_rw;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* Get offset for manual mapping */
		req.handle = bo->handle;
		req.info = MSM_INFO_GET_OFFSET;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);

		/* Map with read-write */
		map_rw = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			      dev->fd, req.value);
		igt_assert_f(map_rw != MAP_FAILED, "Failed to map buffer with RW\n");

		/* Write some data */
		ptr_rw = (uint32_t *)map_rw;
		ptr_rw[0] = 0x12345678;

		/* Map same buffer as read-only */
		map_ro = mmap(0, bo->size, PROT_READ, MAP_SHARED,
			      dev->fd, req.value);
		igt_assert_f(map_ro != MAP_FAILED, "Failed to map buffer with RO\n");

		/* Verify we can read the data through read-only mapping */
		igt_assert_eq_u32(((uint32_t *)map_ro)[0], 0x12345678);

		igt_info("Successfully tested different protection flags\n");

		munmap(map_rw, bo->size);
		munmap(map_ro, bo->size);
		igt_msm_bo_free(bo);
	}

	igt_describe("Test mapping very large buffers");
	igt_subtest("bo-map-large-buffer") {
		struct msm_bo *bo;
		void *map;
		uint32_t *ptr;
		size_t large_size = 64 * 1024 * 1024; /* 64MB */

		bo = igt_msm_bo_new(dev, large_size, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate large buffer (64MB)\n");

		map = igt_msm_bo_map(bo);
		igt_assert_f(map, "Failed to map large buffer\n");

		/* Write and verify at various positions */
		ptr = (uint32_t *)map;
		ptr[0] = 0xAAAAAAAA;
		ptr[1024] = 0xBBBBBBBB;
		ptr[large_size / sizeof(uint32_t) / 2] = 0xCCCCCCCC;
		ptr[large_size / sizeof(uint32_t) - 1] = 0xDDDDDDDD;

		igt_assert_eq_u32(ptr[0], 0xAAAAAAAA);
		igt_assert_eq_u32(ptr[1024], 0xBBBBBBBB);
		igt_assert_eq_u32(ptr[large_size / sizeof(uint32_t) / 2], 0xCCCCCCCC);
		igt_assert_eq_u32(ptr[large_size / sizeof(uint32_t) - 1], 0xDDDDDDDD);

		igt_info("Successfully mapped and verified 64MB buffer\n");

		igt_msm_bo_free(bo);
	}

	igt_describe("Map less than full buffer size (partial mmap is valid)");
	igt_subtest("bo-partial-map") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		void *map;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* Get the offset for mapping */
		req.handle = bo->handle;
		req.info = MSM_INFO_GET_OFFSET;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);

		/* Try to map less than the full buffer size */
		map = mmap(0, DEFAULT_BUFFER_SIZE / 2, PROT_READ | PROT_WRITE,
			   MAP_SHARED, dev->fd, req.value);

		igt_assert_f(map != MAP_FAILED, "Partial map unexpectedly failed\n");
		igt_info("Partial map succeeded as expected\n");

		munmap(map, DEFAULT_BUFFER_SIZE / 2);
		igt_msm_bo_free(bo);
	}

	/* Buffer Mapping Tests - Negative Cases */
	igt_describe("Map more than buffer size (expects EINVAL)");
	igt_subtest("bo-overflow-map") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		void *map;
		int ret;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		/* Get the offset for mapping */
		req.handle = bo->handle;
		req.info = MSM_INFO_GET_OFFSET;
		ret = igt_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);
		igt_assert_eq(ret, 0);

		/*
		 * Try to map more than the buffer size. The fake offset node backing
		 * this BO is sized to the full buffer, so a longer length must be
		 * rejected.
		 */
		map = mmap(0, DEFAULT_BUFFER_SIZE * 2, PROT_READ | PROT_WRITE,
			   MAP_SHARED, dev->fd, req.value);

		igt_assert_f(map == MAP_FAILED, "Overflow map unexpectedly succeeded\n");
		igt_assert_eq(errno, EINVAL);
		igt_info("Correctly rejected overflow map with EINVAL\n");

		igt_msm_bo_free(bo);
	}

	/* Buffer Free Tests - Positive Cases */
	igt_describe("Free single buffer object");
	igt_subtest("bo-free-single") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		uint32_t handle;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");
		handle = bo->handle;

		igt_msm_bo_free(bo);

		/* Verify the handle is actually gone */
		req.handle = handle;
		req.info = MSM_INFO_GET_IOVA;
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, ENOENT);

		igt_info("Successfully freed single buffer object (handle %u)\n", handle);
	}

	igt_describe("Free multiple buffer objects");
	igt_subtest("bo-free-multiple") {
		struct msm_bo *bo1, *bo2, *bo3;
		uint32_t handle1, handle2, handle3;

		/* Allocate multiple buffers */
		bo1 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		bo2 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE * 2, MSM_BO_CACHED);
		bo3 = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE * 4, MSM_BO_WC);

		igt_assert_f(bo1 && bo2 && bo3, "Failed to allocate buffer objects\n");

		handle1 = bo1->handle;
		handle2 = bo2->handle;
		handle3 = bo3->handle;

		/* Free all buffers */
		igt_msm_bo_free(bo1);
		igt_msm_bo_free(bo2);
		igt_msm_bo_free(bo3);

		igt_info("Successfully freed multiple buffer objects (handles %u, %u, %u)\n",
			 handle1, handle2, handle3);
	}

	igt_describe("Free large buffer object");
	igt_subtest("bo-free-large") {
		struct msm_bo *bo;
		size_t large_size = 64 * 1024 * 1024; /* 64MB */
		uint32_t handle;

		bo = igt_msm_bo_new(dev, large_size, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate large buffer (64MB)\n");
		handle = bo->handle;

		igt_msm_bo_free(bo);

		igt_info("Successfully freed large buffer (64MB, handle %u)\n", handle);
	}

	igt_describe("Allocate and free buffers in sequence");
	igt_subtest("bo-free-sequence") {
		const int num_iterations = 10;

		for (int i = 0; i < num_iterations; i++) {
			struct msm_bo *bo;
			uint32_t handle;

			bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
			igt_assert_f(bo, "Failed to allocate buffer in iteration %d\n", i);
			handle = bo->handle;

			igt_msm_bo_free(bo);

			igt_debug("Iteration %d: allocated and freed handle %u\n", i, handle);
		}

		igt_info("Successfully completed %d allocate/free cycles\n", num_iterations);
	}

	/* Buffer Free Tests - Negative Cases */
	igt_describe("Free invalid handle (expects EINVAL)");
	igt_subtest("bo-free-invalid") {
		struct drm_gem_close close_req = { 0 };

		close_req.handle = 0xDEADBEEF;

		do_ioctl_err(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_req, EINVAL);

		igt_info("Correctly rejected freeing invalid handle with EINVAL\n");
	}

	igt_describe("Free same handle twice (expects EINVAL)");
	igt_subtest("bo-double-free") {
		struct msm_bo *bo;
		struct drm_gem_close close_req = { 0 };

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");

		close_req.handle = bo->handle;

		/*
		 * Free it once using gem_close and free bo struct manually
		 * (bypassing igt_msm_bo_free to test double-free behavior)
		 */
		gem_close(dev->fd, bo->handle);
		free(bo);

		/* Try to free it again - should fail */
		do_ioctl_err(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_req, EINVAL);

		igt_info("Correctly rejected double-free with EINVAL\n");
	}

	igt_describe("Free handle zero (expects EINVAL)");
	igt_subtest("bo-free-zero-handle") {
		struct drm_gem_close close_req = { 0 };

		close_req.handle = 0;

		do_ioctl_err(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_req, EINVAL);

		igt_info("Correctly rejected freeing handle 0 with EINVAL\n");
	}

	igt_describe("Query offset and IOVA of a freed buffer (expects ENOENT)");
	igt_subtest("bo-use-after-free") {
		struct msm_bo *bo;
		struct drm_msm_gem_info req = { 0 };
		uint32_t handle;

		bo = igt_msm_bo_new(dev, DEFAULT_BUFFER_SIZE, MSM_BO_WC);
		igt_assert_f(bo, "Failed to allocate buffer object\n");
		handle = bo->handle;

		igt_msm_bo_free(bo);

		/* Both info queries on the freed handle must fail */
		req.handle = handle;
		req.info = MSM_INFO_GET_IOVA;
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, ENOENT);

		req.info = MSM_INFO_GET_OFFSET;
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req, ENOENT);

		igt_info("Correctly rejected use-after-free with ENOENT\n");
	}

	igt_fixture() {
		igt_msm_dev_close(dev);
	}
}
