/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 */

#ifndef XE_UTIL_H
#define XE_UTIL_H

#include <linux_scaffold.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xe_drm.h>

#include "xe_query.h"

#define XE_IS_SYSMEM_MEMORY_REGION(fd, region) \
	(xe_region_class(fd, region) == DRM_XE_MEM_REGION_CLASS_SYSMEM)
#define XE_IS_VRAM_MEMORY_REGION(fd, region) \
	(xe_region_class(fd, region) == DRM_XE_MEM_REGION_CLASS_VRAM)

struct igt_collection *
__xe_get_memory_region_set(int xe, uint32_t *mem_regions_type, int num_regions);

#define xe_get_memory_region_set(regions, mem_region_types...) ({ \
	unsigned int arr__[] = { mem_region_types }; \
	__xe_get_memory_region_set(regions, arr__, ARRAY_SIZE(arr__)); \
})

char *xe_memregion_dynamic_subtest_name(int xe, struct igt_collection *set);

enum xe_bind_op {
	XE_OBJECT_BIND,
	XE_OBJECT_UNBIND,
};

struct xe_object {
	uint32_t handle;
	uint64_t offset;
	uint64_t size;
	uint8_t pat_index;
	enum xe_bind_op bind_op;
	struct igt_list_head link;
};

void xe_bind_unbind_async(int fd, uint32_t vm, uint32_t bind_engine,
			  struct igt_list_head *obj_list,
			  uint32_t sync_in, uint32_t sync_out);

uint32_t xe_nsec_to_ticks(int fd, int gt_id, uint64_t ns);

void xe_fast_copy(int fd,
		  uint32_t src_bo, uint32_t src_region, uint8_t src_pat_index,
		  uint32_t dst_bo, uint32_t dst_region, uint8_t dst_pat_index,
		  uint64_t size);

static inline uint64_t xe_canonical_va(int fd, uint64_t offset) {
	return sign_extend64(offset, xe_va_bits(fd) - 1);
}
void xe_sysfs_enable_ccs_mode(int *fd);

#endif /* XE_UTIL_H */
