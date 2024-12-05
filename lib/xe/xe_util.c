// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "igt.h"
#include "igt_core.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "intel_pat.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

static bool __region_belongs_to_regions_type(struct drm_xe_mem_region *region,
					     uint32_t *mem_regions_type,
					     int num_regions)
{
	for (int i = 0; i < num_regions; i++)
		if (mem_regions_type[i] == region->mem_class)
			return true;
	return false;
}

struct igt_collection *
__xe_get_memory_region_set(int xe, uint32_t *mem_regions_type, int num_regions)
{
	struct drm_xe_mem_region *memregion;
	struct igt_collection *set = NULL;
	uint64_t memreg = all_memory_regions(xe), region;
	int count = 0, pos = 0;

	xe_for_each_mem_region(xe, memreg, region) {
		memregion = xe_mem_region(xe, region);
		if (__region_belongs_to_regions_type(memregion,
						     mem_regions_type,
						     num_regions))
			count++;
	}

	set = igt_collection_create(count);

	xe_for_each_mem_region(xe, memreg, region) {
		memregion = xe_mem_region(xe, region);
		igt_assert(region < (1ull << 31));
		if (__region_belongs_to_regions_type(memregion,
						     mem_regions_type,
						     num_regions)) {
			igt_collection_set_value(set, pos++, (int)region);
		}
	}

	igt_assert(count == pos);

	return set;
}

/**
 * xe_memregion_dynamic_subtest_name:
 * @xe: drm fd of Xe device
 * @igt_collection: memory region collection
 *
 * Function iterates over all memory regions inside the collection (keeped
 * in the value field) and generates the name which can be used during dynamic
 * subtest creation.
 *
 * Returns: newly allocated string, has to be freed by caller. Asserts if
 * caller tries to create a name using empty collection.
 */
char *xe_memregion_dynamic_subtest_name(int xe, struct igt_collection *set)
{
	struct igt_collection_data *data;
	char *name, *p;
	uint32_t region, len;

	igt_assert(set && set->size);
	/* enough for "name%d-" * n */
	len = set->size * 8;
	p = name = malloc(len);
	igt_assert(name);

	for_each_collection_data(data, set) {
		struct drm_xe_mem_region *memreg;
		int r;

		region = data->value;
		memreg = xe_mem_region(xe, region);

		if (XE_IS_CLASS_VRAM(memreg))
			r = snprintf(p, len, "%s%d-",
				     xe_region_name(region),
				     memreg->instance);
		else
			r = snprintf(p, len, "%s-",
				     xe_region_name(region));

		igt_assert(r > 0);
		p += r;
		len -= r;
	}

	/* remove last '-' */
	*(p - 1) = 0;

	return name;
}

#ifdef XEBINDDBG
#define bind_info igt_info
#define bind_debug igt_debug
#else
#define bind_info(...) {}
#define bind_debug(...) {}
#endif

static struct drm_xe_vm_bind_op *xe_alloc_bind_ops(int xe,
						   struct igt_list_head *obj_list,
						   uint32_t *num_ops)
{
	struct drm_xe_vm_bind_op *bind_ops, *ops;
	struct xe_object *obj;
	uint32_t num_objects = 0, i = 0, op, flags = 0;

	igt_list_for_each_entry(obj, obj_list, link)
		num_objects++;

	*num_ops = num_objects;
	if (!num_objects) {
		bind_info(" [nothing to bind]\n");
		return NULL;
	}

	bind_ops = calloc(num_objects, sizeof(*bind_ops));
	igt_assert(bind_ops);

	igt_list_for_each_entry(obj, obj_list, link) {
		ops = &bind_ops[i];

		if (obj->bind_op == XE_OBJECT_BIND) {
			op = DRM_XE_VM_BIND_OP_MAP;
			ops->obj = obj->handle;
		} else {
			op = DRM_XE_VM_BIND_OP_UNMAP;
		}

		ops->op = op;
		ops->flags = flags;
		ops->obj_offset = 0;
		ops->addr = obj->offset;
		ops->range = ALIGN(obj->size, 4096);
		ops->prefetch_mem_region_instance = 0;
		if (obj->pat_index == DEFAULT_PAT_INDEX)
			ops->pat_index = intel_get_pat_idx_wb(xe);
		else
			ops->pat_index = obj->pat_index;

		bind_info("  [%d]: [%6s] handle: %u, offset: %llx, size: %llx\n",
			  i, obj->bind_op == XE_OBJECT_BIND ? "BIND" : "UNBIND",
			  ops->obj, (long long)ops->addr, (long long)ops->range);
		i++;
	}

	return bind_ops;
}

/**
 * xe_bind_unbind_async:
 * @xe: drm fd of Xe device
 * @vm: vm to bind/unbind objects to/from
 * @bind_engine: bind engine, 0 if default
 * @obj_list: list of xe_object
 * @sync_in: sync object (fence-in), 0 if there's no input dependency
 * @sync_out: sync object (fence-out) to signal on bind/unbind completion,
 *            if 0 wait for bind/unbind completion.
 *
 * Function iterates over xe_object @obj_list, prepares binding operation
 * and does bind/unbind in one step. Providing sync_in / sync_out allows
 * working in pipelined mode. With sync_in and sync_out set to 0 function
 * waits until binding operation is complete.
 */
void xe_bind_unbind_async(int xe, uint32_t vm, uint32_t bind_engine,
			  struct igt_list_head *obj_list,
			  uint32_t sync_in, uint32_t sync_out)
{
	struct drm_xe_vm_bind_op *bind_ops;
	struct drm_xe_sync tabsyncs[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .handle = sync_in },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sync_out },
	};
	struct drm_xe_sync *syncs;
	uint32_t num_binds = 0;
	int num_syncs;

	bind_info("[Binding to vm: %u]\n", vm);
	bind_ops = xe_alloc_bind_ops(xe, obj_list, &num_binds);

	if (!num_binds) {
		if (sync_out)
			syncobj_signal(xe, &sync_out, 1);
		return;
	}

	if (sync_in) {
		syncs = tabsyncs;
		num_syncs = 2;
	} else {
		syncs = &tabsyncs[1];
		num_syncs = 1;
	}

	/* User didn't pass sync out, create it and wait for completion */
	if (!sync_out)
		tabsyncs[1].handle = syncobj_create(xe, 0);

	bind_info("[Binding syncobjs: (in: %u, out: %u)]\n",
		  tabsyncs[0].handle, tabsyncs[1].handle);

	if (num_binds == 1) {
		if ((bind_ops[0].op & 0xffff) == DRM_XE_VM_BIND_OP_MAP)
			igt_assert_eq(__xe_vm_bind(xe, vm, bind_engine, bind_ops[0].obj,
						   0, bind_ops[0].addr, bind_ops[0].range,
						   DRM_XE_VM_BIND_OP_MAP, 0,
						   syncs, num_syncs, 0,
						   bind_ops[0].pat_index, 0), 0);
		else
			igt_assert_eq(__xe_vm_bind(xe, vm, bind_engine, 0,
						   0, bind_ops[0].addr, bind_ops[0].range,
						   DRM_XE_VM_BIND_OP_UNMAP, 0,
						   syncs, num_syncs, 0,
						   bind_ops[0].pat_index, 0), 0);
	} else {
		xe_vm_bind_array(xe, vm, bind_engine, bind_ops,
				 num_binds, syncs, num_syncs);
	}

	if (!sync_out) {
		igt_assert_eq(syncobj_wait_err(xe, &tabsyncs[1].handle, 1, INT64_MAX, 0), 0);
		syncobj_destroy(xe, tabsyncs[1].handle);
	}

	free(bind_ops);
}

static uint32_t reference_clock(int fd, int gt_id)
{
	struct xe_device *dev = xe_device_get(fd);
	const struct drm_xe_gt *gt;
	uint32_t refclock;

	igt_assert(dev && dev->gt_mask & BIT(gt_id));

	gt = drm_xe_get_gt(dev, gt_id);
	refclock = gt->reference_clock;

	igt_assert_lt(0, refclock);

	return refclock;
}

static uint64_t div64_u64_round_up(const uint64_t x, const uint64_t y)
{
	igt_assert(y > 0);
	igt_assert_lte_u64(x, UINT64_MAX - (y - 1));

	return (x + y - 1) / y;
}

/**
 * xe_nsec_to_ticks: convert time in nanoseconds to timestamp ticks
 * @fd: opened device
 * @gt_id: tile id
 * @nsec: time in nanoseconds
 *
 * Return: Time converted to context timestamp ticks.
 */
uint32_t xe_nsec_to_ticks(int fd, int gt_id, uint64_t nsec)
{
	uint32_t refclock = reference_clock(fd, gt_id);

	return div64_u64_round_up(nsec * refclock, NSEC_PER_SEC);
}

/**
 * xe_fast_copy: simplify fast-copy from src to dst bo
 * @fd: opened device
 * @src_bo: src bo handle
 * @src_region: src region
 * @src_pat_index: src pat index
 * @dst_bo: dst bo handle
 * @dst_region: dst region
 * @dst_pat_index: dst pat index
 * @size: size in bytes for copy. Currently has limitation that it requires
 * size aligned to 4KiB and must be <= 16MiB.
 *
 * Simplify common fast-copy from one bo to another. See @size regarding
 * operation limitation.
 */
void xe_fast_copy(int fd,
		  uint32_t src_bo, uint32_t src_region, uint8_t src_pat_index,
		  uint32_t dst_bo, uint32_t dst_region, uint8_t dst_pat_index,
		  uint64_t size)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	struct blt_copy_data blt = {};
	struct blt_copy_object src = {};
	struct blt_copy_object dst = {};
	intel_ctx_t *ctx;
	uint64_t bb_size, stride, width, height;
	uint32_t vm, exec_queue, bb;
	uint32_t alignment = xe_get_default_alignment(fd);
	uint64_t ahnd;
	int cpp = 4;

	igt_require(size % SZ_4K == 0);
	igt_require(size <= SZ_16M);
	igt_require(blt_has_fast_copy(fd));

	width = size >= SZ_16K ? SZ_4K : SZ_4K / cpp;
	height = size / width / cpp;
	stride = width * cpp;

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open_full(fd, ctx->vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH,
					 alignment);

	blt_copy_init(fd, &blt);
	blt.color_depth = CD_32bit;

	bb_size = xe_bb_size(fd, SZ_4K);
	bb = xe_bo_create(fd, 0, bb_size, system_memory(fd), 0);

	blt_set_object(&src, src_bo, size, src_region, intel_get_uc_mocs_index(fd),
		       src_pat_index, T_LINEAR,
		       COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
	blt_set_geom(&src, stride, 0, 0, width, height, 0, 0);

	blt_set_object(&dst, dst_bo, size, dst_region, intel_get_uc_mocs_index(fd),
		       dst_pat_index, T_LINEAR,
		       COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
	blt_set_geom(&dst, stride, 0, 0, width, height, 0, 0);

	blt_set_copy_object(&blt.src, &src);
	blt_set_copy_object(&blt.dst, &dst);
	blt_set_batch(&blt.bb, bb, bb_size, system_memory(fd));

	blt_fast_copy(fd, ctx, NULL, ahnd, &blt);

	gem_close(fd, bb);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
	intel_ctx_destroy(fd, ctx);
}

/**
 * xe_sysfs_enable_ccs_mode:
 * @fd: pointer to xe drm fd, can be reopened multiple times
 *
 * Enables ccs_mode on all GTs, it must succeed for at least one GT.
 * Since function requires the driver to be closed during ccs_mode change
 * @fd will be closed then re-opened.
 */
void xe_sysfs_enable_ccs_mode(int *fd)
{
	int gt, gt_fd, num_slices, ccs_mode, num_gt;
	int enabled_count = 0;

	num_gt = xe_number_gt(*fd);

	for (gt = 0; gt < num_gt; gt++) {
		gt_fd = xe_sysfs_gt_open(*fd, gt);
		drm_close_driver(*fd);

		if (!igt_debug_on(igt_sysfs_scanf(gt_fd, "num_cslices", "%u", &num_slices) <= 0) &&
		    !igt_debug_on(igt_sysfs_printf(gt_fd, "ccs_mode", "%u", num_slices) <= 0) &&
		    !igt_debug_on(igt_sysfs_scanf(gt_fd, "ccs_mode", "%u", &ccs_mode) <= 0) &&
		    !igt_debug_on(num_slices != ccs_mode))
			enabled_count++;
		close(gt_fd);
		*fd = drm_open_driver(DRIVER_XE);
	}

	if (!enabled_count)
		igt_require_f(0, "Cannot enable ccs mode for any GT\n");
}
