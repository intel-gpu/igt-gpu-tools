// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_panthor.h"
#include "ioctl_wrappers.h"
#include "panthor_drm.h"

/**
 * igt_panthor_skip_on_big_endian:
 *
 * Skip Panthor test on big-endian machines.
 */
void igt_panthor_skip_on_big_endian(void)
{
	igt_skip_on_f(__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__,
		      "Panthor is unsupported on big-endian arch\n");
}

/**
 * igt_panthor_group_create:
 * @fd: device file descriptor
 * @group_create: pointer to group creation structure
 * @err: expected error code, or 0 for success
 *
 * Create a group.
 */
void igt_panthor_group_create(int fd, struct drm_panthor_group_create *group_create, int err)
{
	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, group_create, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, group_create);
}

/**
 * igt_panthor_group_destroy:
 * @fd: device file descriptor
 * @group_handle: group handle to destroy
 * @err: expected error code, or 0 for success
 *
 * Destroy a group.
 */
void igt_panthor_group_destroy(int fd, uint32_t group_handle, int err)
{
	struct drm_panthor_group_destroy group_destroy = {
		.group_handle = group_handle,
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &group_destroy, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &group_destroy);
}

/**
 * igt_panthor_group_submit:
 * @fd: device file descriptor
 * @group_submit: pointer to group submission structure
 * @err: expected error code, or 0 for success
 *
 * Submit work to a group.
 */
void igt_panthor_group_submit(int fd, struct drm_panthor_group_submit *group_submit, int err)
{
	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, group_submit, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, group_submit);
}

/**
 * igt_panthor_group_create_simple:
 * @fd: device file descriptor
 * @vm_id: VM ID to associate with the group
 * @err: expected error code, or 0 for success
 *
 * Create a group with a single queue and reasonable defaults.
 *
 * Returns: group handle on success
 */
uint32_t igt_panthor_group_create_simple(int fd, uint32_t vm_id, int err)
{
	struct drm_panthor_gpu_info gpu_info = {};
	struct drm_panthor_group_create group_create = {};
	struct drm_panthor_queue_create queue = {};
	struct drm_panthor_obj_array queues = {};

	igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, &gpu_info, sizeof(gpu_info), 0);

	queue.priority = 0;
	queue.ringbuf_size = 4096;
	queues = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, &queue);

	group_create.queues = queues;
	group_create.max_compute_cores = igt_hweight(gpu_info.shader_present);
	group_create.max_fragment_cores = igt_hweight(gpu_info.shader_present);
	group_create.max_tiler_cores = igt_hweight(gpu_info.tiler_present);
	group_create.priority = PANTHOR_GROUP_PRIORITY_MEDIUM;
	group_create.compute_core_mask = gpu_info.shader_present;
	group_create.fragment_core_mask = gpu_info.shader_present;
	group_create.tiler_core_mask = gpu_info.tiler_present;
	group_create.vm_id = vm_id;

	igt_panthor_group_create(fd, &group_create, err);
	return group_create.group_handle;
}

/**
 * igt_panthor_group_submit_simple:
 * @fd: device file descriptor
 * @group_handle: group handle to submit to
 * @queue_index: queue index within the group
 * @stream_addr: GPU address of the command stream
 * @stream_size: size of the command stream
 * @syncobj_handle: sync object handle for completion signaling
 * @err: expected error code, or 0 for success
 *
 * Submit work to a group queue with a simple interface.
 */
void igt_panthor_group_submit_simple(int fd, uint32_t group_handle,
				     uint32_t queue_index, uint64_t stream_addr,
				     uint32_t stream_size, uint32_t syncobj_handle,
				     int err)
{
	struct drm_panthor_group_submit group_submit = {};
	struct drm_panthor_queue_submit queue_submit = {};
	struct drm_panthor_sync_op sync_op = {};

	sync_op.handle = syncobj_handle;
	sync_op.flags = DRM_PANTHOR_SYNC_OP_SIGNAL;

	queue_submit.syncs = (struct drm_panthor_obj_array)DRM_PANTHOR_OBJ_ARRAY(1, &sync_op);
	queue_submit.queue_index = queue_index;
	queue_submit.stream_size = stream_size;
	queue_submit.stream_addr = stream_addr;
	queue_submit.latest_flush = 0;

	group_submit.group_handle = group_handle;
	group_submit.queue_submits = (struct drm_panthor_obj_array)
		DRM_PANTHOR_OBJ_ARRAY(1, &queue_submit);

	igt_panthor_group_submit(fd, &group_submit, err);
}

/**
 * SECTION:igt_panthor
 * @short_description: Panthor support library
 * @title: Panthor
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing Panthor
 * tests.
 */

/**
 * igt_panthor_query:
 * @fd: device file descriptor
 * @type: query type (e.g., DRM_PANTHOR_DEV_QUERY_GPU_INFO)
 * @data: pointer to a struct to store the query result
 * @size: size of the result struct
 * @err: expected error code, or 0 for success
 *
 * Query GPU information.
 *
 * Returns: the size of the type being queried.
 */
uint32_t igt_panthor_query(int fd, int32_t type, void *data, size_t size, int err)
{
	struct drm_panthor_dev_query query = {
		.type = type,
		.pointer = (uintptr_t)data,
		.size = size,
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &query, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &query);

	return query.size;
}

/**
 * igt_panthor_vm_create:
 * @fd: device file descriptor
 * @vm_id: pointer to store the created VM ID
 * @err: expected error code, or 0 for success
 *
 * Creates a VM.
 */
void igt_panthor_vm_create_userva_range(int fd, uint32_t *vm_id, int err, uint64_t *uva_range)
{
	struct drm_panthor_vm_create vm_create = {};

	if (err) {
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vm_create, err);
	} else {
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vm_create);
		*vm_id = vm_create.id;

		if (uva_range)
			*uva_range = vm_create.user_va_range;
	}
}

/**
 * igt_panthor_vm_destroy:
 * @fd: device file descriptor
 * @vm_id: VM ID to destroy
 * @err: expected error code, or 0 for success
 *
 * Destroys a VM.
 */
void igt_panthor_vm_destroy(int fd, uint32_t vm_id, int err)
{
	struct drm_panthor_vm_destroy vm_destroy = {
		.id = vm_id,
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vm_destroy, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vm_destroy);
}

/**
 * igt_panthor_vm_bind:
 * @fd: device file descriptor
 * @vm_id: VM ID to bind the buffer to
 * @bo_handle: buffer object handle to bind
 * @va: virtual address to bind at
 * @size: size of the binding
 * @flags: binding flags
 * @err: expected error code, or 0 for success
 *
 * Bind a buffer object to a virtual address in the specified VM.
 */
void igt_panthor_vm_bind_offset(int fd, uint32_t vm_id, uint32_t bo_handle, uint64_t va,
				uint64_t size, uint64_t offset, uint32_t flags, int err)
{
	struct drm_panthor_vm_bind_op bind_op = {
		.flags = flags,
		.bo_handle = bo_handle,
		.bo_offset = offset,
		.va = va,
		.size = size,
	};

	struct drm_panthor_vm_bind vm_bind = {
		.vm_id = vm_id,
		.flags = 0,
		.ops = DRM_PANTHOR_OBJ_ARRAY(1, &bind_op),
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_VM_BIND, &vm_bind, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_BIND, &vm_bind);
}

/**
 * igt_panthor_bo_create:
 * @fd: device file descriptor
 * @bo: pointer to panthor_bo structure to initialize
 * @size: requested buffer size in bytes
 * @flags: buffer object creation flags
 * @err: expected error code, or 0 for success
 *
 * Creates a new buffer object
 */
void igt_panthor_bo_create(int fd, struct panthor_bo *bo,
			   uint64_t size, uint32_t flags, int err)
{
	struct drm_panthor_bo_create bo_create = {
		.size = size,
		.flags = flags,
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bo_create, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_BO_CREATE, &bo_create);

	bo->handle = bo_create.handle;
	bo->size = bo_create.size;
	bo->offset = 0;
	bo->map = NULL;
}

/**
 * igt_panthor_bo_mmap_offset:
 * @fd: device file descriptor
 * @handle: buffer object handle
 * @err: expected error code, or 0 for success
 *
 * Get the mmap offset for a buffer object.
 *
 * Returns: the mmap offset for the buffer object
 */
uint64_t igt_panthor_bo_mmap_offset(int fd, uint32_t handle, int err)
{
	struct drm_panthor_bo_mmap_offset bo_mmap_offset = {
		.handle = handle,
	};

	if (err)
		do_ioctl_err(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &bo_mmap_offset, err);
	else
		do_ioctl(fd, DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET, &bo_mmap_offset);

	return bo_mmap_offset.offset;
}

/**
 * igt_panthor_mmap_bo:
 * @fd: device file descriptor
 * @handle: buffer object handle
 * @size: size of the buffer to map
 * @prot: memory protection flags (e.g., PROT_READ | PROT_WRITE)
 * @offset: mmap offset for the buffer object
 *
 * Map a buffer object into the process address space.
 *
 * Returns: pointer to the mapped memory, or NULL on failure
 */
void *igt_panthor_mmap_bo(int fd, uint32_t handle, uint64_t size,
			  unsigned int prot, uint64_t offset)
{
	void *ptr;

	ptr = mmap(0, size, prot, MAP_SHARED, fd, offset);
	if (ptr == MAP_FAILED)
		return NULL;
	else
		return ptr;
}

/**
 * igt_panthor_bo_create_mapped:
 * @fd: device file descriptor
 * @bo: pointer to panthor_bo structure to initialize
 * @size: requested buffer size in bytes
 * @flags: buffer object creation flags
 * @err: expected error code, or 0 for success
 *
 * Create a new buffer object on the panthor device and map it into
 * the process address space.
 */
void igt_panthor_bo_create_mapped(int fd, struct panthor_bo *bo, uint64_t size,
				  uint32_t flags, int err)
{
	igt_panthor_bo_create(fd, bo, size, flags, err);
	bo->offset = igt_panthor_bo_mmap_offset(fd, bo->handle, err);
	bo->map = igt_panthor_mmap_bo(fd, bo->handle, bo->size,
				      PROT_READ | PROT_WRITE, bo->offset);
}

/**
 * igt_panthor_free_bo:
 * @fd: panthor device file descriptor
 * @bo: pointer to panthor_bo structure to free
 *
 * Free a buffer object and unmap it if it was mapped.
 */
void igt_panthor_free_bo(int fd, struct panthor_bo *bo)
{
	if (!bo)
		return;

	if (bo->map)
		munmap(bo->map, bo->size);

	gem_close(fd, bo->handle);
}

/**
 * igt_panthor_get_arch:
 * @fd: panthor device file descriptor
 *
 * Get the architecture major version for this device.
 *
 * Returns: The Mali architecture major version
 */
int igt_panthor_get_arch(int fd)
{
	struct drm_panthor_gpu_info gpu_info = { 0 };

	igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO,
			  &gpu_info, sizeof(gpu_info), 0);
	return DRM_PANTHOR_ARCH_MAJOR(gpu_info.gpu_id);
}
