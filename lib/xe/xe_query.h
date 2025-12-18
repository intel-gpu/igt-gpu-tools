/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_QUERY_H
#define XE_QUERY_H

#include <stdint.h>
#include <xe_drm.h>

#include "igt_aux.h"
#include "igt_list.h"
#include "igt_sizes.h"
#include "intel_hwconfig_types.h"

#define XE_DEFAULT_ALIGNMENT           SZ_4K
#define XE_DEFAULT_ALIGNMENT_64K       SZ_64K

struct xe_device {
	/** @fd: xe fd */
	int fd;

	/** @config: xe configuration */
	struct drm_xe_query_config *config;

	/** @hwconfig: xe hwconfig table data */
	uint32_t *hwconfig;

	/** @hwconfig_size: size of hwconfig in bytes */
	uint32_t hwconfig_size;

	/** @gt_list: gt info */
	struct drm_xe_query_gt_list *gt_list;

	/** @gt_mask: bitmask of GT IDs */
	uint64_t gt_mask;

	/** @tile_mask: bitmask of Tile IDs */
	uint64_t tile_mask;

	/** @memory_regions: bitmask of all memory regions */
	uint64_t memory_regions;

	/** @engines: hardware engines */
	struct drm_xe_query_engines *engines;

	/** @eu_stall: information about EU stall data */
	struct drm_xe_query_eu_stall *eu_stall;

	/** @mem_regions: regions memory information and usage */
	struct drm_xe_query_mem_regions *mem_regions;

	/** @oa_units: information about OA units */
	struct drm_xe_query_oa_units *oa_units;

	/** @vram_size: array of vram sizes for all gt_list */
	uint64_t *vram_size;

	/** @visible_vram_size: array of visible vram sizes for all gt_list */
	uint64_t *visible_vram_size;

	/** @default_alignment: safe alignment regardless region location */
	uint32_t default_alignment;

	/** @has_vram: true if gpu has vram, false if system memory only */
	bool has_vram;

	/** @va_bits: va length in bits */
	uint32_t va_bits;

	/** @dev_id: Device id of xe device */
	uint16_t dev_id;
};

#define xe_for_each_engine(__fd, __hwe) \
	for (int igt_unique(__i) = 0; igt_unique(__i) < xe_number_engines(__fd) && \
	     (__hwe = &xe_engine(__fd, igt_unique(__i))->instance); ++igt_unique(__i))
#define xe_for_each_engine_class(__class) \
	for (__class = 0; __class < DRM_XE_ENGINE_CLASS_COMPUTE + 1; \
	     ++__class)
#define xe_for_each_gt(__fd, __gt) \
	for (uint64_t igt_unique(__mask) = xe_device_get(__fd)->gt_mask; \
	     __gt = ffsll(igt_unique(__mask)) - 1, igt_unique(__mask) != 0; \
	     igt_unique(__mask) &= ~(1ull << __gt))
#define xe_for_each_tile(__fd, __tile) \
	for (uint64_t igt_unique(__mask) = xe_device_get(__fd)->tile_mask; \
	     __tile = ffsll(igt_unique(__mask)) - 1, igt_unique(__mask) != 0; \
	     igt_unique(__mask) &= ~(1ull << __tile))
#define xe_for_each_mem_region(__fd, __memreg, __r) \
	for (uint64_t igt_unique(__i) = 0; igt_unique(__i) < igt_fls(__memreg); igt_unique(__i)++) \
		for_if(__r = (__memreg & (1ull << igt_unique(__i))))

#define XE_IS_CLASS_SYSMEM(__region) ((__region)->mem_class == DRM_XE_MEM_REGION_CLASS_SYSMEM)
#define XE_IS_CLASS_VRAM(__region) ((__region)->mem_class == DRM_XE_MEM_REGION_CLASS_VRAM)

#define xe_for_each_topology_mask(__masks, __size, __mask) \
	for (__mask = (__masks); \
	     (void *)__mask->mask - (void *)(__masks) < (__size) && \
	     (void *)&__mask->mask[__mask->num_bytes] - (void *)(__masks) <= (__size); \
	     __mask = (void *)&__mask->mask[__mask->num_bytes])

/*
 * Max possible engine instance in drm_xe_engine_class_instance::engine_instance. Only
 * used to declare arrays of drm_xe_engine_class_instance
 */
#define XE_MAX_ENGINE_INSTANCE	9

unsigned int xe_number_gt(int fd);
unsigned int xe_dev_max_gt(int fd);
uint8_t xe_tiles_count(int fd);
uint64_t all_memory_regions(int fd);
uint64_t system_memory(int fd);
const struct drm_xe_gt *drm_xe_get_gt(struct xe_device *xe_dev, int gt_id);
int xe_get_tile(struct xe_device *xe_dev, int gt_id);
uint64_t vram_memory(int fd, int gt);
uint64_t vram_if_possible(int fd, int gt);
struct drm_xe_engine *xe_engines(int fd);
struct drm_xe_engine *xe_engine(int fd, int idx);
struct drm_xe_mem_region *xe_mem_region(int fd, uint64_t region);
const char *xe_region_name(uint64_t region);
uint16_t xe_region_class(int fd, uint64_t region);
uint32_t xe_min_page_size(int fd, uint64_t region);
struct drm_xe_query_config *xe_config(int fd);
struct drm_xe_query_gt_list *xe_gt_list(int fd);
struct drm_xe_query_oa_units *xe_oa_units(int fd);
unsigned int xe_number_engines(int fd);
bool xe_has_vram(int fd);
uint64_t xe_vram_size(int fd, int gt);
uint64_t xe_visible_vram_size(int fd, int gt);
uint64_t xe_available_vram_size(int fd, int gt);
uint64_t xe_visible_available_vram_size(int fd, int gt);
uint32_t xe_get_default_alignment(int fd);
uint32_t xe_va_bits(int fd);
uint16_t xe_dev_id(int fd);
int xe_supports_faults(int fd);
const char *xe_engine_class_string(uint32_t engine_class);
const char *xe_engine_class_short_string(uint32_t engine_class);
bool xe_has_engine_class(int fd, uint16_t engine_class);
struct drm_xe_engine *xe_find_engine_by_class(int fd, uint16_t engine_class);
bool xe_has_media_gt(int fd);
uint16_t xe_gt_type(int fd, int gt);
bool xe_is_media_gt(int fd, int gt);
bool xe_is_main_gt(int fd, int gt);
uint16_t xe_gt_get_tile_id(int fd, int gt);
uint16_t xe_tile_get_main_gt_id(int fd, uint8_t tile);
uint32_t *xe_hwconfig_lookup_value(int fd, enum intel_hwconfig attribute, uint32_t *len);
uint32_t xe_hwconfig_lookup_value_u32(int fd, enum intel_hwconfig attribute);
void *xe_query_device_may_fail(int fd, uint32_t type, uint32_t *size);
int xe_query_pxp_status(int fd);
int xe_wait_for_pxp_init(int fd);

/**
 * xe_query_device:
 * @fd: xe device fd
 * @type: query type, one of DRM_XE_DEVICE_QUERY_* values
 * @size: pointer to get size of returned data, can be NULL
 *
 * Calls DRM_IOCTL_XE_DEVICE_QUERY ioctl to query device information
 * about specified @type. Returns pointer to malloc'ed data, which
 * should be freed later by the user. If @query is not supported
 * or on any other error it asserts.
 */
static inline void *xe_query_device(int fd, uint32_t type, uint32_t *size)
{
	void *data = xe_query_device_may_fail(fd, type, size);

	igt_assert(data);
	return data;
}

struct xe_device *xe_device_get(int fd);
void xe_device_put(int fd);

int xe_query_eu_count(int fd, int gt);
int xe_query_eu_thread_count(int fd, int gt);

#endif	/* XE_QUERY_H */
