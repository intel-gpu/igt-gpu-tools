// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#include <stdlib.h>
#include <pthread.h>

#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

#define VG(x) x
#else
#define VG(x) do {} while (0)
#endif

#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "igt_map.h"

#include "xe_query.h"
#include "xe_ioctl.h"

/**
 * xe_query_device_may_fail:
 * @fd: xe device fd
 * @type: query type, one of DRM_XE_DEVICE_QUERY_* values
 * @size: pointer to get size of returned data, can be NULL
 *
 * Calls DRM_IOCTL_XE_DEVICE_QUERY ioctl to query device information
 * about specified @type. Returns pointer to malloc'ed data, which
 * should be freed later by the user. If @query is not supported
 * function returns NULL. On any other error it asserts.
 */
void *xe_query_device_may_fail(int fd, uint32_t type, uint32_t *size)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = type,
		.size = 0,
		.data = 0,
	};
	void *data = NULL;

	/* In case of unsupported query xe driver usually returns error,
	 * but in case of HWCONFIG it can also return query.size == 0
	 * on older platforms.
	 */
	if (igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) || !query.size)
		goto skip_query;

	data = malloc(query.size);
	igt_assert(data);

	query.data = to_user_pointer(data);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	VG(VALGRIND_MAKE_MEM_DEFINED(data, query.size));

skip_query:
	if (size)
		*size = query.size;

	return data;
}

static uint64_t __memory_regions(const struct drm_xe_query_gt_list *gt_list)
{
	uint64_t regions = 0;
	int i;

	for (i = 0; i < gt_list->num_gt; i++)
		regions |= gt_list->gt_list[i].near_mem_regions |
			   gt_list->gt_list[i].far_mem_regions;

	return regions;
}

static uint64_t native_region_for_gt(const struct drm_xe_gt *gt)
{
	uint64_t region;

	igt_assert(gt);
	region = gt->near_mem_regions;
	igt_assert(region);

	return region;
}

static uint64_t gt_vram_size(const struct drm_xe_query_mem_regions *mem_regions,
			     const struct drm_xe_gt *gt)
{
	int region_idx = ffsll(native_region_for_gt(gt)) - 1;

	if (XE_IS_CLASS_VRAM(&mem_regions->mem_regions[region_idx]))
		return mem_regions->mem_regions[region_idx].total_size;

	return 0;
}

static uint64_t gt_visible_vram_size(const struct drm_xe_query_mem_regions *mem_regions,
				     const struct drm_xe_gt *gt)
{
	int region_idx = ffsll(native_region_for_gt(gt)) - 1;

	if (XE_IS_CLASS_VRAM(&mem_regions->mem_regions[region_idx]))
		return mem_regions->mem_regions[region_idx].cpu_visible_size;

	return 0;
}

static bool __mem_has_vram(struct drm_xe_query_mem_regions *mem_regions)
{
	for (int i = 0; i < mem_regions->num_mem_regions; i++)
		if (XE_IS_CLASS_VRAM(&mem_regions->mem_regions[i]))
			return true;

	return false;
}

static uint32_t __mem_default_alignment(struct drm_xe_query_mem_regions *mem_regions)
{
	uint32_t alignment = XE_DEFAULT_ALIGNMENT;

	for (int i = 0; i < mem_regions->num_mem_regions; i++)
		if (alignment < mem_regions->mem_regions[i].min_page_size)
			alignment = mem_regions->mem_regions[i].min_page_size;

	return alignment;
}

/**
 * xe_engine_class_string:
 * @engine_class: engine class
 *
 * Returns engine class name or 'unknown class engine' otherwise.
 */
const char *xe_engine_class_string(uint32_t engine_class)
{
	switch (engine_class) {
	case DRM_XE_ENGINE_CLASS_RENDER:
		return "DRM_XE_ENGINE_CLASS_RENDER";
	case DRM_XE_ENGINE_CLASS_COPY:
		return "DRM_XE_ENGINE_CLASS_COPY";
	case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
		return "DRM_XE_ENGINE_CLASS_VIDEO_DECODE";
	case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return "DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE";
	case DRM_XE_ENGINE_CLASS_COMPUTE:
		return "DRM_XE_ENGINE_CLASS_COMPUTE";
	default:
		igt_warn("Engine class 0x%x unknown\n", engine_class);
		return "unknown engine class";
	}
}

/**
 * xe_engine_class_short_string:
 * @engine_class: engine class
 *
 * Returns short name for engine class or 'unknown' otherwise.
 */
const char *xe_engine_class_short_string(uint32_t engine_class)
{
	switch (engine_class) {
	case DRM_XE_ENGINE_CLASS_RENDER:
		return "rcs";
	case DRM_XE_ENGINE_CLASS_COPY:
		return "bcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
		return "vcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return "vecs";
	case DRM_XE_ENGINE_CLASS_COMPUTE:
		return "ccs";
	default:
		igt_warn("Engine class 0x%x unknown\n", engine_class);
		return "unknown";
	}
}

static struct xe_device_cache {
	pthread_mutex_t cache_mutex;
	struct igt_map *map;
} cache;

static struct xe_device *find_in_cache_unlocked(int fd)
{
	return igt_map_search(cache.map, &fd);
}

static struct xe_device *find_in_cache(int fd)
{
	struct xe_device *xe_dev;

	pthread_mutex_lock(&cache.cache_mutex);
	xe_dev = find_in_cache_unlocked(fd);
	pthread_mutex_unlock(&cache.cache_mutex);

	return xe_dev;
}

static void xe_device_free(struct xe_device *xe_dev)
{
	free(xe_dev->config);
	free(xe_dev->hwconfig);
	free(xe_dev->gt_list);
	free(xe_dev->engines);
	free(xe_dev->mem_regions);
	free(xe_dev->vram_size);
	free(xe_dev->eu_stall);
	free(xe_dev);
}

/**
 * xe_device_get:
 * @fd: xe device fd
 *
 * Function creates and caches xe_device struct which contains configuration
 * data returned in few queries. Subsequent calls returns previously
 * created xe_device. To remove this from cache xe_device_put() must be
 * called.
 */
struct xe_device *xe_device_get(int fd)
{
	struct xe_device *xe_dev, *prev;
	int max_gt;

	xe_dev = find_in_cache(fd);
	if (xe_dev)
		return xe_dev;

	xe_dev = calloc(1, sizeof(*xe_dev));
	igt_assert(xe_dev);

	xe_dev->fd = fd;
	xe_dev->config = xe_query_device(fd, DRM_XE_DEVICE_QUERY_CONFIG, NULL);
	xe_dev->hwconfig = xe_query_device_may_fail(fd, DRM_XE_DEVICE_QUERY_HWCONFIG, &xe_dev->hwconfig_size);
	xe_dev->va_bits = xe_dev->config->info[DRM_XE_QUERY_CONFIG_VA_BITS];
	xe_dev->dev_id = xe_dev->config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff;
	xe_dev->gt_list = xe_query_device(fd, DRM_XE_DEVICE_QUERY_GT_LIST, NULL);

	/* GT IDs may be non-consecutive; keep a mask of valid IDs */
	for (int gt = 0; gt < xe_dev->gt_list->num_gt; gt++)
		xe_dev->gt_mask |= (1ull << xe_dev->gt_list->gt_list[gt].gt_id);

	/* Tile IDs may be non-consecutive; keep a mask of valid IDs */
	for (int gt = 0; gt < xe_dev->gt_list->num_gt; gt++)
		xe_dev->tile_mask |= (1ull << xe_dev->gt_list->gt_list[gt].tile_id);

	xe_dev->memory_regions = __memory_regions(xe_dev->gt_list);
	xe_dev->engines = xe_query_device(fd, DRM_XE_DEVICE_QUERY_ENGINES, NULL);
	xe_dev->mem_regions = xe_query_device(fd, DRM_XE_DEVICE_QUERY_MEM_REGIONS, NULL);
	xe_dev->eu_stall = xe_query_device_may_fail(fd, DRM_XE_DEVICE_QUERY_EU_STALL, NULL);
	xe_dev->oa_units = xe_query_device_may_fail(fd, DRM_XE_DEVICE_QUERY_OA_UNITS, NULL);

	/*
	 * vram_size[] and visible_vram_size[] are indexed by uapi ID; ensure
	 * the allocation is large enough to hold the highest GT ID
	 */
	max_gt = igt_fls(xe_dev->gt_mask) - 1;
	xe_dev->vram_size = calloc(max_gt + 1, sizeof(*xe_dev->vram_size));
	xe_dev->visible_vram_size = calloc(max_gt + 1, sizeof(*xe_dev->visible_vram_size));

	for (int idx = 0; idx < xe_dev->gt_list->num_gt; idx++) {
		struct drm_xe_gt *gt = &xe_dev->gt_list->gt_list[idx];

		xe_dev->vram_size[gt->gt_id] =
			gt_vram_size(xe_dev->mem_regions, gt);
		xe_dev->visible_vram_size[gt->gt_id] =
			gt_visible_vram_size(xe_dev->mem_regions, gt);
	}
	xe_dev->default_alignment = __mem_default_alignment(xe_dev->mem_regions);
	xe_dev->has_vram = __mem_has_vram(xe_dev->mem_regions);

	/* We may get here from multiple threads, use first cached xe_dev */
	pthread_mutex_lock(&cache.cache_mutex);
	prev = find_in_cache_unlocked(fd);
	if (!prev) {
		igt_map_insert(cache.map, &xe_dev->fd, xe_dev);
	} else {
		xe_device_free(xe_dev);
		xe_dev = prev;
	}
	pthread_mutex_unlock(&cache.cache_mutex);

	return xe_dev;
}

static void delete_in_cache(struct igt_map_entry *entry)
{
	xe_device_free((struct xe_device *)entry->data);
}

/**
 * xe_device_put:
 * @fd: xe device fd
 *
 * Remove previously allocated and cached xe_device (if any).
 */
void xe_device_put(int fd)
{
	pthread_mutex_lock(&cache.cache_mutex);
	if (find_in_cache_unlocked(fd))
		igt_map_remove(cache.map, &fd, delete_in_cache);
	pthread_mutex_unlock(&cache.cache_mutex);
}

/**
 * xe_supports_faults:
 * @fd: xe device fd
 *
 * Returns the return value of the ioctl.  This can either be 0 if the
 * xe device @fd allows creating a vm in fault mode, or an error value
 * if it does not.
 *
 * NOTE: This function temporarily creates a VM in fault mode. Hence, while
 * this function is executing, no non-fault mode VMs can be created.
 */
int xe_supports_faults(int fd)
{
	int ret;

	struct drm_xe_vm_create create = {
		.flags = DRM_XE_VM_CREATE_FLAG_LR_MODE |
			 DRM_XE_VM_CREATE_FLAG_FAULT_MODE,
	};

	ret = igt_ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &create);

	if (!ret)
		xe_vm_destroy(fd, create.vm_id);

	return ret;
}

static void xe_device_destroy_cache(void)
{
	pthread_mutex_lock(&cache.cache_mutex);
	igt_map_destroy(cache.map, delete_in_cache);
	pthread_mutex_unlock(&cache.cache_mutex);
}

static void xe_device_cache_init(void)
{
	pthread_mutex_init(&cache.cache_mutex, NULL);
	xe_device_destroy_cache();
	cache.map = igt_map_create(igt_map_hash_32, igt_map_equal_32);
}

#define xe_dev_FN(_NAME, _FIELD, _TYPE) \
_TYPE _NAME(int fd)			\
{					\
	struct xe_device *xe_dev;	\
					\
	xe_dev = find_in_cache(fd);	\
	igt_assert(xe_dev);		\
	return xe_dev->_FIELD;		\
}

/**
 * xe_number_gt:
 * @fd: xe device fd
 *
 * Return number of gt_list for xe device fd.
 */
xe_dev_FN(xe_number_gt, gt_list->num_gt, unsigned int);

/**
 * xe_max_gt:
 * @fd: xe device fd
 *
 * Return maximum GT ID in xe device's GT list.
 */
unsigned int xe_dev_max_gt(int fd)
{
	struct xe_device *xe_dev = find_in_cache(fd);

	igt_assert(xe_dev);
	return igt_fls(xe_dev->gt_mask) - 1;
}

/**
 * xe_tiles_count:
 * @fd: xe device fd
 *
 * Return number of tiles for xe device fd.
 */
uint8_t xe_tiles_count(int fd)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	return igt_hweight(xe_dev->tile_mask);
}

/**
 * all_memory_regions:
 * @fd: xe device fd
 *
 * Returns memory regions bitmask for xe device @fd.
 */
xe_dev_FN(all_memory_regions, memory_regions, uint64_t);

/**
 * system_memory:
 * @fd: xe device fd
 *
 * Returns system memory bitmask for xe device @fd.
 */
uint64_t system_memory(int fd)
{
	uint64_t regions = all_memory_regions(fd);

	return regions & 0x1;
}

/*
 * Given a uapi GT ID, lookup the corresponding drm_xe_gt structure in the
 * GT list.
 */
const struct drm_xe_gt *drm_xe_get_gt(struct xe_device *xe_dev, int gt_id)
{
	for (int i = 0; i < xe_dev->gt_list->num_gt; i++)
		if (xe_dev->gt_list->gt_list[i].gt_id == gt_id)
			return &xe_dev->gt_list->gt_list[i];

	return NULL;
}

/*
 * Given a uapi GT ID, lookup the corresponding drm_xe_gt structure in the
 * GT list and return valid tile_id otherwise invalid.
 */
int xe_get_tile(struct xe_device *xe_dev, int gt_id)
{
	for (int i = 0; i < xe_dev->gt_list->num_gt; i++)
		if (xe_dev->gt_list->gt_list[i].gt_id == gt_id)
			return xe_dev->gt_list->gt_list[i].tile_id;

	return -ENOENT;
}

/**
 * vram_memory:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns vram memory bitmask for xe device @fd and @gt id.
 */
uint64_t vram_memory(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(xe_dev->gt_mask & BIT(gt));

	return xe_has_vram(fd) ? native_region_for_gt(drm_xe_get_gt(xe_dev, gt)) : 0;
}

static uint64_t __xe_visible_vram_size(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	return xe_dev->visible_vram_size[gt];
}

/**
 * vram_if_possible:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns vram memory bitmask for xe device @fd and @gt id or system memory
 * if there's no vram memory available for @gt.
 */
uint64_t vram_if_possible(int fd, int gt)
{
	return vram_memory(fd, gt) ?: system_memory(fd);
}

/**
 * xe_engines:
 * @fd: xe device fd
 *
 * Returns engines array of xe device @fd.
 */
xe_dev_FN(xe_engines, engines->engines, struct drm_xe_engine *);

/**
 * xe_engine:
 * @fd: xe device fd
 * @idx: engine index
 *
 * Returns engine info of xe device @fd and @idx.
 */
struct drm_xe_engine *xe_engine(int fd, int idx)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(idx >= 0 && idx < xe_dev->engines->num_engines);

	return &xe_dev->engines->engines[idx];
}

/**
 * xe_mem_region:
 * @fd: xe device fd
 * @region: region mask
 *
 * Returns memory region structure for @region mask.
 */
struct drm_xe_mem_region *xe_mem_region(int fd, uint64_t region)
{
	struct xe_device *xe_dev;
	int region_idx = ffs(region) - 1;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(xe_dev->mem_regions->num_mem_regions > region_idx);

	return &xe_dev->mem_regions->mem_regions[region_idx];
}

/**
 * xe_region_name:
 * @region: region mask
 *
 * Returns region string like "system" or "vramN" where N=0...62.
 */
const char *xe_region_name(uint64_t region)
{
	static char **vrams;
	int region_idx = ffs(region) - 1;

	/* Populate the array */
	if (!vrams) {
		vrams = calloc(64, sizeof(char *));
		for (int i = 0; i < 64; i++) {
			if (i != 0)
				asprintf(&vrams[i], "vram%d", i - 1);
			else
				asprintf(&vrams[i], "system");
			igt_assert(vrams[i]);
		}
	}

	return vrams[region_idx];
}

/**
 * xe_region_class:
 * @fd: xe device fd
 * @region: region mask
 *
 * Returns class of memory region structure for @region mask.
 */
uint16_t xe_region_class(int fd, uint64_t region)
{
	struct drm_xe_mem_region *memreg;

	memreg = xe_mem_region(fd, region);

	return memreg->mem_class;
}

/**
 * xe_min_page_size:
 * @fd: xe device fd
 * @region: region mask
 *
 * Returns minimum page size for @region.
 */
uint32_t xe_min_page_size(int fd, uint64_t region)
{
	return xe_mem_region(fd, region)->min_page_size;
}

/**
 * xe_config:
 * @fd: xe device fd
 *
 * Returns xe configuration of xe device @fd.
 */
xe_dev_FN(xe_config, config, struct drm_xe_query_config *);

/**
 * xe_gt_list:
 * @fd: xe device fd
 *
 * Returns query gts of xe device @fd.
 */
xe_dev_FN(xe_gt_list, gt_list, struct drm_xe_query_gt_list *);

/**
 * xe_oa_units:
 * @fd: xe device fd
 *
 * Returns query gts of xe device @fd.
 */
xe_dev_FN(xe_oa_units, oa_units, struct drm_xe_query_oa_units *);

/**
 * xe_number_engine:
 * @fd: xe device fd
 *
 * Returns number of hw engines of xe device @fd.
 */
xe_dev_FN(xe_number_engines, engines->num_engines, unsigned int);

/**
 * xe_has_vram:
 * @fd: xe device fd
 *
 * Returns true if xe device @fd has vram otherwise false.
 */
xe_dev_FN(xe_has_vram, has_vram, bool);

/**
 * xe_vram_size:
 * @fd: xe device fd
 * @gt: gt
 *
 * Returns size of vram of xe device @fd.
 */
uint64_t xe_vram_size(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	return xe_dev->vram_size[gt];
}

/**
 * xe_visible_vram_size:
 * @fd: xe device fd
 * @gt: gt
 *
 * Returns size of visible vram of xe device @fd.
 */
uint64_t xe_visible_vram_size(int fd, int gt)
{
	uint64_t visible_size;

	/*
	 * TODO: Keep it backwards compat for now. Fixup once the kernel side
	 * has landed.
	 */
	visible_size = __xe_visible_vram_size(fd, gt);
	if (!visible_size) /* older kernel */
		visible_size = xe_vram_size(fd, gt);

	return visible_size;
}

struct __available_vram {
	uint64_t total_available;
	uint64_t cpu_visible_available;
};

static void __available_vram_size_snapshot(int fd, int gt, struct __available_vram *vram)
{
	struct xe_device *xe_dev;
	int region_idx;
	struct drm_xe_mem_region *mem_region;
	struct drm_xe_query_mem_regions *mem_regions;

	igt_assert(vram);
	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	region_idx = ffsll(native_region_for_gt(drm_xe_get_gt(xe_dev, gt))) - 1;
	mem_region = &xe_dev->mem_regions->mem_regions[region_idx];

	if (XE_IS_CLASS_VRAM(mem_region)) {
		mem_regions = xe_query_device(fd, DRM_XE_DEVICE_QUERY_MEM_REGIONS, NULL);
		pthread_mutex_lock(&cache.cache_mutex);
		mem_region->used = mem_regions->mem_regions[region_idx].used;
		mem_region->cpu_visible_used =
			mem_regions->mem_regions[region_idx].cpu_visible_used;
		vram->total_available = mem_region->total_size - mem_region->used;
		vram->cpu_visible_available =
			mem_region->cpu_visible_size - mem_region->cpu_visible_used;
		pthread_mutex_unlock(&cache.cache_mutex);
		free(mem_regions);
	}
}

/**
 * xe_available_vram_size:
 * @fd: xe device fd
 * @gt: gt
 *
 * Returns size of available vram of xe device @fd and @gt.
 */
uint64_t xe_available_vram_size(int fd, int gt)
{
	struct __available_vram vram = {};

	__available_vram_size_snapshot(fd, gt, &vram);

	return vram.total_available;
}

/**
 * xe_visible_available_vram_size:
 * @fd: xe device fd
 * @gt: gt
 *
 * Returns size of visible available vram of xe device @fd and @gt.
 */
uint64_t xe_visible_available_vram_size(int fd, int gt)
{
	struct __available_vram vram = {};

	__available_vram_size_snapshot(fd, gt, &vram);

	return vram.cpu_visible_available;
}

/**
 * xe_get_default_alignment:
 * @fd: xe device fd
 *
 * Returns default alignment of objects for xe device @fd.
 */
xe_dev_FN(xe_get_default_alignment, default_alignment, uint32_t);

/**
 * xe_va_bits:
 * @fd: xe device fd
 *
 * Returns number of virtual address bits used in xe device @fd.
 */
xe_dev_FN(xe_va_bits, va_bits, uint32_t);


/**
 * xe_dev_id:
 * @fd: xe device fd
 *
 * Returns Device id of xe device @fd.
 */
xe_dev_FN(xe_dev_id, dev_id, uint16_t);

/**
 * xe_has_engine_class:
 * @fd: xe device fd
 * @engine_class: engine class
 *
 * Returns true if device @fd has hardware engine @class otherwise false.
 */
bool xe_has_engine_class(int fd, uint16_t engine_class)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	for (int i = 0; i < xe_dev->engines->num_engines; i++)
		if (xe_dev->engines->engines[i].instance.engine_class == engine_class)
			return true;

	return false;
}

/**
 * xe_find_engine_by_class
 * @fd: xe device fd
 * @engine_class: engine class
 *
 * Returns engine info of xe device @fd and @engine_class otherwise NULL.
 */
struct drm_xe_engine *xe_find_engine_by_class(int fd, uint16_t engine_class)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	for (int i = 0; i < xe_dev->engines->num_engines; i++)
		if (xe_dev->engines->engines[i].instance.engine_class == engine_class)
			return &xe_dev->engines->engines[i];

	return NULL;
}
/**
 * xe_has_media_gt:
 * @fd: xe device fd
 *
 * Returns true if device @fd has media GT otherwise false.
 */
bool xe_has_media_gt(int fd)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	for (int i = 0; i < xe_dev->gt_list->num_gt; i++)
		if (xe_dev->gt_list->gt_list[i].type == DRM_XE_QUERY_GT_TYPE_MEDIA)
			return true;

	return false;
}

/**
 * xe_gt_type:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns the type of @gt for device @fd (e.g.,
 * DRM_XE_QUERY_GT_TYPE_MAIN, DRM_XE_QUERY_GT_TYPE_MEDIA).
 */
uint16_t xe_gt_type(int fd, int gt)
{
	struct xe_device *xe_dev = find_in_cache(fd);
	const struct drm_xe_gt *xe_gt;

	igt_assert(xe_dev);
	xe_gt = drm_xe_get_gt(xe_dev, gt);
	igt_assert(xe_gt);

	return xe_gt->type;
}

/**
 * xe_is_media_gt:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns true if @gt for device @fd is MEDIA GT, otherwise false.
 */
bool xe_is_media_gt(int fd, int gt)
{
	return xe_gt_type(fd, gt) == DRM_XE_QUERY_GT_TYPE_MEDIA;
}

/**
 * xe_is_main_gt:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns true if @gt for device @fd is MAIN GT, otherwise false.
 */
bool xe_is_main_gt(int fd, int gt)
{
	return xe_gt_type(fd, gt) == DRM_XE_QUERY_GT_TYPE_MAIN;
}

/**
 * xe_gt_to_tile_id:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns tile id for given @gt.
 */
uint16_t xe_gt_get_tile_id(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);

	igt_assert(xe_dev);
	igt_assert(gt < xe_number_gt(fd));

	return xe_dev->gt_list->gt_list[gt].tile_id;
}

/**
 * xe_tile_get_main_gt_id:
 * @fd: xe device fd
 * @tile: tile id
 *
 * Returns main GT ID for given @tile.
 */
uint16_t xe_tile_get_main_gt_id(int fd, uint8_t tile)
{
	struct xe_device *xe_dev;
	int gt_id = -1;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	for (int i = 0; i < xe_dev->gt_list->num_gt; i++) {
		const struct drm_xe_gt *gt_data = &xe_dev->gt_list->gt_list[i];

		if (gt_data->tile_id == tile && gt_data->type == DRM_XE_QUERY_GT_TYPE_MAIN) {
			gt_id = gt_data->gt_id;
			break;
		}
	}

	igt_assert_f(gt_id >= 0, "No main GT found for tile %d\n", tile);

	return gt_id;
}

/**
 * xe_hwconfig_lookup_value:
 * @fd: xe device fd
 * @attribute: hwconfig attribute id
 * @len: pointer to store length of the value (in uint32_t sized elements)
 *
 * Returns a pointer to the value of the hwconfig attribute @attribute and
 * writes the number of uint32_t elements indicating the length of the value to @len.
 */
uint32_t *xe_hwconfig_lookup_value(int fd, enum intel_hwconfig attribute, uint32_t *len)
{
	struct xe_device *xe_dev;
	uint32_t *hwconfig;
	uint32_t pos, hwconfig_len;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	hwconfig = xe_dev->hwconfig;
	if (!hwconfig)
		return NULL;

	/* Extract the value from the hwconfig */
	pos = 0;
	hwconfig_len = xe_dev->hwconfig_size / sizeof(uint32_t);
	while (pos + 2 < hwconfig_len) {
		uint32_t attribute_id = hwconfig[pos];
		uint32_t attribute_len = hwconfig[pos + 1];
		uint32_t *attribute_data = &hwconfig[pos + 2];

		if (attribute_id == attribute) {
			*len = attribute_len;
			return attribute_data;
		}
		pos += 2 + attribute_len;
	}

	return NULL;
}

/**
 * xe_hwconfig_lookup_value_u32:
 * @fd: xe device fd
 * @attribute: hwconfig attribute id
 *
 * Returns the u32 value of the hwconfig attribute @attribute. Asserts if the
 * attribute is not found or if its length is not 1.
 */
uint32_t xe_hwconfig_lookup_value_u32(int fd, enum intel_hwconfig attribute)
{
	uint32_t len, *val;

	val = xe_hwconfig_lookup_value(fd, attribute, &len);
	igt_assert(val && len == 1);
	return *val;
}

/**
 * xe_query_pxp_status:
 * @fd: xe device fd
 *
 * Returns the PXP status value if PXP is supported, a negative errno otherwise.
 * See DRM_XE_DEVICE_QUERY_PXP_STATUS documentation for the possible errno
 * values and their meaning.
 */
int xe_query_pxp_status(int fd)
{
	struct drm_xe_query_pxp_status *pxp_query;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_PXP_STATUS,
		.size = 0,
		.data = 0,
	};
	int ret;

	if (igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
		return -errno;

	pxp_query = malloc(query.size);
	igt_assert(pxp_query);
	memset(pxp_query, 0, query.size);

	query.data = to_user_pointer(pxp_query);

	if (igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
		ret = -errno;
	else
		ret = pxp_query->status;

	free(pxp_query);

	return ret;
}

/**
 * xe_wait_for_pxp_init:
 * @fd: xe device fd
 *
 * Returns 0 once PXP is initialized and ready, -EINVAL if PXP is not supported
 * in the kernel, -ENODEV if PXP is not supported in HW. This function asserts
 * if something went wrong during PXP initialization.
 */
int xe_wait_for_pxp_init(int fd)
{
	int pxp_status;
	int i = 0;

	/* PXP init completes after driver init, so we might have to wait for it */
	while (i++ < 50) {
		pxp_status = xe_query_pxp_status(fd);

		/*
		 * -EINVAL and -ENODEV are both valid return values and they
		 * respectively indicate that the the PXP interface is not
		 * available (i.e., kernel too old) and that PXP is not
		 * supported or disabled in HW.
		 */
		if (pxp_status == -EINVAL || pxp_status == -ENODEV)
			return pxp_status;

		/* status 1 means pxp is ready */
		if (pxp_status == 1)
			return 0;

		/*
		 * 0 means init still in progress, any other remaining state
		 * is an unexpected error
		 */
		igt_assert_eq(pxp_status, 0);

		usleep(50*1000);
	}

	igt_assert_f(0, "PXP failed to initialize within the timeout\n");
	return -ETIMEDOUT;
}

/**
 * xe_query_eu_count:
 * @fd: xe device fd
 * @gt: GT id
 *
 * Return count of EUs for given GT.
 */
int xe_query_eu_count(int fd, int gt)
{
	struct drm_xe_query_topology_mask *c_dss = NULL, *g_dss = NULL, *eu_per_dss = NULL;
	struct drm_xe_query_topology_mask *topology, *topo;
	uint32_t size;
	int eu_count;

	topology = xe_query_device(fd, DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, &size);
	xe_for_each_topology_mask(topology, size, topo) {
		if (topo->gt_id != gt)
			continue;

		if (topo->type == DRM_XE_TOPO_DSS_GEOMETRY) {
			g_dss = topo;
		} else if (topo->type == DRM_XE_TOPO_DSS_COMPUTE) {
			c_dss = topo;
		} else if (topo->type == DRM_XE_TOPO_EU_PER_DSS ||
			 topo->type == DRM_XE_TOPO_SIMD16_EU_PER_DSS) {
			eu_per_dss = topo;
			break;
		}
	}

	igt_assert(g_dss && c_dss && eu_per_dss);
	igt_assert_eq_u32(c_dss->num_bytes, g_dss->num_bytes);

	for (int i = 0; i < c_dss->num_bytes; i++)
		c_dss->mask[i] |= g_dss->mask[i];

	eu_count = igt_bitmap_hweight(c_dss->mask, c_dss->num_bytes * 8);
	eu_count *= igt_bitmap_hweight(eu_per_dss->mask, eu_per_dss->num_bytes * 8);

	free(topology);

	return eu_count;
}

/**
 * xe_query_eu_thread_count:
 * @fd: xe device fd
 * @gt: GT id
 *
 * Return count of EU threads for given GT.
 */
int xe_query_eu_thread_count(int fd, int gt)
{
	return xe_query_eu_count(fd, gt) *
		xe_hwconfig_lookup_value_u32(fd, INTEL_HWCONFIG_NUM_THREADS_PER_EU);
}

igt_constructor
{
	xe_device_cache_init();
}
