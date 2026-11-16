// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "igt_pvr.h"

#include "drmtest.h"
#include "ioctl_wrappers.h"

#include "pvr_drm.h"

#define POWERVR_GPU_PAGE_SIZE 4096

/**
 * SECTION:igt_pvr
 * @short_description: PowerVR support library
 * @title: pvr
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing PowerVR
 * tests.
 */

/**
 * igt_pvr_ioctl_create_bo_ex:
 * @fd: The file descriptor of the DRM device.
 * @size: On entry, the requested size of the buffer object. On return, the
 *        actual size of the buffer object.
 * @flags: Flags to control the behaviour of the buffer object.
 *
 * Function to create a buffer object.
 *
 * Returns: The handle of the created buffer object.
 */
uint32_t igt_pvr_ioctl_create_bo_ex(int fd, size_t *size, uint64_t flags)
{
	struct drm_pvr_ioctl_create_bo_args arg = {
		.size = *size,
		.flags = flags,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &arg);

	igt_assert(arg.size >= *size && arg.size <= SIZE_MAX);
	*size = (size_t)arg.size;

	return arg.handle;
}

/**
 * igt_pvr_ioctl_create_bo:
 * @fd: The file descriptor of the DRM device.
 * @size: On entry, the requested size of the buffer object. On return, the
 *        actual size of the buffer object.
 *
 * Function to create a buffer object with default flags.
 *
 * Returns: The handle of the created buffer object.
 */
uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size)
{
	return igt_pvr_ioctl_create_bo_ex(fd, size,
					  DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS);
}

/**
 * igt_pvr_ioctl_get_bo_mmap_offset:
 * @fd: The file descriptor of the DRM device.
 * @handle: The handle of the buffer object.
 *
 * Function to get the mmap offset of a buffer object.
 *
 * Returns: The mmap offset of the buffer object.
 */
off_t igt_pvr_ioctl_get_bo_mmap_offset(int fd, uint32_t handle)
{
	struct drm_pvr_ioctl_get_bo_mmap_offset_args arg = {
		.handle = handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &arg);

	/*
	 * There is no OFF_MAX equivalent to SIZE_MAX; use the identical (by
	 * definition) PTRDIFF_MAX instead.
	 */
	igt_assert(arg.offset <= PTRDIFF_MAX);

	return (off_t)arg.offset;
}

/**
 * igt_pvr_ioctl_dev_query:
 * @fd: The file descriptor of the DRM device.
 * @type: The type of the device query.
 * @size: The size of the query structure.
 * @pointer: Pointer to the query structure.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to perform a device query.
 *
 * Returns: The filled-in query arguments structure.
 */
struct drm_pvr_ioctl_dev_query_args
igt_pvr_ioctl_dev_query(int fd, enum drm_pvr_dev_query type, uint64_t size,
			void *pointer, int expect_err)
{
	struct drm_pvr_ioctl_dev_query_args args = {
		.type = type,
		.size = size,
		.pointer = to_user_pointer(pointer),
	};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_DEV_QUERY, &args, expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &args);

	return args;
}

/**
 * igt_pvr_get_heap_info:
 * @fd: The file descriptor of the DRM device.
 * @array_len_out: Pointer to store the number of heaps.
 *
 * Function to get information about the device heaps.
 *
 * Returns: An array of drm_pvr_heap structures. The caller is responsible
 * for freeing the array.
 */
struct drm_pvr_heap *
igt_pvr_get_heap_info(int fd, uint32_t *array_len_out)
{
	struct drm_pvr_heap *heaps =
		calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));
	struct drm_pvr_dev_query_heap_info heap_info_get = {
		.heaps = DRM_PVR_OBJ_ARRAY(DRM_PVR_HEAP_COUNT, heaps),
	};

	if (!heaps)
		return NULL;

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
				sizeof(heap_info_get), &heap_info_get, 0);

	if (array_len_out)
		*array_len_out = heap_info_get.heaps.count;

	return heaps;
}

/**
 * igt_pvr_find_general_heap:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to find the general heap of the device.
 *
 * Returns: The drm_pvr_heap structure representing the general heap.
 */
struct drm_pvr_heap igt_pvr_find_general_heap(int fd)
{
	struct drm_pvr_heap *heaps = igt_pvr_get_heap_info(fd, NULL);
	struct drm_pvr_heap general_heap;

	igt_assert(heaps);

	general_heap = heaps[DRM_PVR_HEAP_GENERAL];
	free(heaps);

	return general_heap;
}

/**
 * igt_pvr_get_static_data_areas:
 * @fd: The file descriptor of the DRM device.
 * @array_len_out: Pointer to store the number of static data areas.
 *
 * Function to get information about the device static data areas.
 *
 * Returns: An array of drm_pvr_static_data_area structures. The caller is responsible
 * for freeing the array.
 */
struct drm_pvr_static_data_area *
igt_pvr_get_static_data_areas(int fd, uint32_t *array_len_out)
{
	struct drm_pvr_static_data_area *sdas =
		calloc(DRM_PVR_STATIC_DATA_AREA_YUV_CSC + 1, sizeof(*sdas));
	struct drm_pvr_dev_query_static_data_areas sdas_get = {
		.static_data_areas =
			DRM_PVR_OBJ_ARRAY(DRM_PVR_STATIC_DATA_AREA_YUV_CSC + 1,
					  sdas),
	};

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
				sizeof(sdas_get), &sdas_get, 0);

	if (array_len_out)
		*array_len_out = sdas_get.static_data_areas.count;

	return sdas;
}

/**
 * igt_pvr_ioctl_create_vm_context:
 * @fd: The file descriptor of the DRM device.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to create a VM context.
 *
 * Returns: The handle of the created VM context.
 */
uint32_t igt_pvr_ioctl_create_vm_context(int fd, int expect_err)
{
	struct drm_pvr_ioctl_create_vm_context_args args = {0};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &args,
			     expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &args);

	return args.handle;
}

/**
 * igt_pvr_ioctl_destroy_vm_context:
 * @fd: The file descriptor of the DRM device.
 * @handle: The handle of the VM context to destroy.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to destroy a VM context.
 */
void igt_pvr_ioctl_destroy_vm_context(int fd, uint32_t handle, int expect_err)
{
	struct drm_pvr_ioctl_destroy_vm_context_args args = {
		.handle = handle,
	};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args,
			     expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args);
}

/**
 * igt_pvr_ioctl_vm_map:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @handle: The handle of the target buffer object to map.
 * @device_addr: Requested virtual address in the device's address space.
 * @offset: The offset within the buffer object.
 * @size: The size of the mapping.
 *
 * Function to map a buffer object into a VM context.
 */
void igt_pvr_ioctl_vm_map(int fd, uint32_t vm_ctx_handle, uint32_t handle,
			  uint64_t device_addr, uint64_t offset, uint64_t size)
{
	struct drm_pvr_ioctl_vm_map_args arg = {
		.vm_context_handle = vm_ctx_handle,
		.flags = 0,
		.device_addr = device_addr,
		.handle = handle,
		.offset = offset,
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &arg);
}

/**
 * igt_pvr_ioctl_vm_unmap:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @device_addr: The device virtual address of the mapping to unmap.
 * @size: The size of the mapping.
 *
 * Function to unmap a buffer object from a VM context.
 */
void igt_pvr_ioctl_vm_unmap(int fd, uint32_t vm_ctx_handle,
			    uint64_t device_addr, uint64_t size)
{
	struct drm_pvr_ioctl_vm_unmap_args arg = {
		.vm_context_handle = vm_ctx_handle,
		.device_addr = device_addr,
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_VM_UNMAP, &arg);
}

/**
 * init_compute_context_state_stream:
 * @stream: Pointer to the memory where the context state stream will be initialized.
 *
 * Function to initialize the compute context state stream.
 *
 * Returns: The length of the initialized stream.
 */
static uint32_t init_compute_context_state_stream(uint8_t *stream)
{
	uint8_t *stream_start = stream;
	uint32_t stream_len;

	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // length
	STREAM_ADD_ITEM(true, uint32_t, 0, stream);
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_context_pds0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_context_pds1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_terminate_pds
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_terminate_pds1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_resume_pds0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_context_pds0_b
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // cdmreg_cdm_resume_pds0_b

	stream_len = stream - stream_start;
	STREAM_ADD_ITEM(true, uint32_t, stream_len, stream_start); // update length

	return stream_len;
}

/**
 * init_render_context_state_stream:
 * @stream: Pointer to the memory where the context state stream will be initialized.
 *
 * Function to initialize the render context state stream.
 *
 * Returns: The length of the initialized stream.
 */
static uint32_t init_render_context_state_stream(uint8_t *stream)
{
	uint8_t *stream_start = stream;
	uint32_t stream_len;

	STREAM_ADD_ITEM(true, uint32_t, 0, stream); // length
	STREAM_ADD_ITEM(true, uint32_t, 0, stream);
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // geom_reg_vdm_context_state_base_addr
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // geom_reg_vdm_context_state_resume_addr
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // geom_reg_ta_context_state_base_addr
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_store_task0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_store_task1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_store_task2
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_store_task3
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_store_task4
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_resume_task0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_resume_task1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_resume_task2
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_resume_task3
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[0].geom_reg_vdm_context_resume_task4
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_store_task0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_store_task1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_store_task2
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_store_task3
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_store_task4
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_resume_task0
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_resume_task1
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_resume_task2
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_resume_task3
	STREAM_ADD_ITEM(true, uint64_t, 0, stream); // state[1].geom_reg_vdm_context_resume_task4

	stream_len = stream - stream_start;
	STREAM_ADD_ITEM(true, uint32_t, stream_len, stream_start); // update length

	return stream_len;
}

/**
 * igt_pvr_ioctl_create_context:
 * @fd: File descriptor of the DRM device.
 * @type: Type of the context to create.
 * @vm_ctx_handle: Handle to the VM context.
 *
 * Function to create a device context.
 *
 * Returns: Handle to the created context.
 */
uint32_t igt_pvr_ioctl_create_context(int fd, enum drm_pvr_ctx_type type,
				      uint32_t vm_ctx_handle)
{
	#define CONTEXT_INIT_STREAM_SIZE 1024

	struct drm_pvr_ioctl_create_context_args create_context_args = {
		.type = type,
		.priority = DRM_PVR_CTX_PRIORITY_NORMAL,
		.vm_context_handle = vm_ctx_handle,
	};

	uint8_t init_render_context_stream[CONTEXT_INIT_STREAM_SIZE] __attribute__((aligned(8)));
	uint8_t init_compute_context_stream[CONTEXT_INIT_STREAM_SIZE] __attribute__((aligned(8)));

	switch (type) {
	case DRM_PVR_CTX_TYPE_RENDER:
		create_context_args.callstack_addr =
			igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd,
								      vm_ctx_handle,
								      POWERVR_GPU_PAGE_SIZE,
								      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
								      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
		create_context_args.static_context_state = to_user_pointer(init_render_context_stream);
		create_context_args.static_context_state_len =
			init_render_context_state_stream(init_render_context_stream);
		break;

	case DRM_PVR_CTX_TYPE_COMPUTE:
		create_context_args.static_context_state = to_user_pointer(init_compute_context_stream);
		create_context_args.static_context_state_len =
			init_compute_context_state_stream(init_compute_context_stream);
		break;

	case DRM_PVR_CTX_TYPE_TRANSFER_FRAG:
		break;
	}

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &create_context_args);

	return create_context_args.handle;
}

/**
 * igt_pvr_ioctl_destroy_context:
 * @fd: File descriptor of the DRM device.
 * @ctx_handle: Handle of the context to destroy.
 *
 * Function to destroy a device context.
 */
void igt_pvr_ioctl_destroy_context(int fd, uint32_t ctx_handle)
{
	struct drm_pvr_ioctl_destroy_context_args destroy_context_args = {
		.handle = ctx_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_CONTEXT, &destroy_context_args);
}

/**
 * igt_pvr_ioctl_create_free_list:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @gpu_addr: The GPU address for the free list.
 *
 * Function to create a free list.
 */
uint32_t igt_pvr_ioctl_create_free_list(int fd, uint32_t vm_ctx_handle,
					uint64_t gpu_addr)
{
	struct drm_pvr_ioctl_create_free_list_args create_free_list_args = {
		.free_list_gpu_addr = gpu_addr,
		.initial_num_pages = 64,
		.max_num_pages = 256,
		.grow_num_pages = 16,
		.grow_threshold = 50,
		.vm_context_handle = vm_ctx_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST, &create_free_list_args);

	return create_free_list_args.handle;
}

/**
 * igt_pvr_ioctl_destroy_free_list:
 * @fd: The file descriptor of the DRM device.
 * @free_list_handle: The handle of the free list to destroy.
 *
 * Function to destroy a free list.
 */
void igt_pvr_ioctl_destroy_free_list(int fd, uint32_t free_list_handle)
{
	struct drm_pvr_ioctl_destroy_free_list_args destroy_free_list_args = {
		.handle = free_list_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST, &destroy_free_list_args);
}

struct heap_allocator {
	uint64_t base;
	uint64_t size;
	uint64_t offset;
};

/**
 * igt_pvr_init_heap_allocator:
 * @allocator: Heap allocator to initialize.
 * @heap: Heap information.
 *
 * Function to initialize heap allocator using information from the kernel.
 */
static void init_heap_allocator(struct heap_allocator *allocator, struct drm_pvr_heap *heap)
{
	igt_log("igt-pvr", IGT_LOG_DEBUG,
		"Initializing heap allocator: base=0x%llx, size=%llu\n", heap->base, heap->size);
	allocator->base = heap->base;
	allocator->size = heap->size;
	allocator->offset = 0;
}

/*
 * Common allocators for all vm contexts in the application
 * This implies no overlap in virtual address between different vm contexts.
 */

static struct heap_allocator heap_allocators[DRM_PVR_HEAP_COUNT];

/**
 * igt_pvr_init_heap_allocators:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to initialize heap allocators using information retrieved from the kernel.
 *
 * Returns: 0 on success, -1 on failure.
 */
static int igt_pvr_init_heap_allocators(int fd)
{
	uint32_t heap_count;
	struct drm_pvr_heap *heaps = igt_pvr_get_heap_info(fd, &heap_count);

	igt_assert(heap_count == DRM_PVR_HEAP_COUNT);

	for (int i = 0; i < heap_count; i++) {
		struct drm_pvr_heap *heap_info = &heaps[i];

		if (heap_info)
			init_heap_allocator(&heap_allocators[i], heap_info);
		else
			return -1; // Failed to get heap info
	}
	return 0;
}

/**
 * allocate_from_heap:
 * @heap_index: Index of the heap to allocate from.
 * @size: Size of the allocation.
 *
 * Function to allocate address range from a specific heap.
 *
 * Returns: Allocated GPU address on success, 0 on failure.
 */
static uint64_t allocate_from_heap(uint32_t heap_index, uint64_t size)
{
	struct heap_allocator *allocator;
	uint64_t allocatedAddress;

	if (heap_index >= DRM_PVR_HEAP_COUNT) {
		igt_log("igt-pvr", IGT_LOG_CRITICAL,
			"Error: Invalid heap index %u. Must be between 0 and %u.\n",
			heap_index, DRM_PVR_HEAP_COUNT - 1);
		return 0; // Invalid heap index
	}
	size = ALIGN(size, POWERVR_GPU_PAGE_SIZE);
	allocator = &heap_allocators[heap_index];
	if (allocator->offset + size > allocator->size) {
		igt_log("igt-pvr", IGT_LOG_CRITICAL,
			"Error: Not enough space in heap %u. Requested size: %" PRIu64 ", Available size: %" PRIu64 ".\n",
			heap_index, size, allocator->size - allocator->offset);
		return 0; // Not enough space
	}
	allocatedAddress = allocator->base + allocator->offset;
	allocator->offset += size;
	igt_log("igt-pvr", IGT_LOG_DEBUG,
		"Allocated %" PRIu64 " bytes from heap %u. Allocated address: 0x%" PRIx64 ", New offset: 0x%" PRIx64 ".\n",
		size, heap_index, allocatedAddress, allocator->offset);
	return allocatedAddress;
}

#define MAX_ALLOCATIONS 1024

/**
 * struct igt_pvr_allocations:
 * @allocations: Array of allocations.
 * @count: Number of allocations.
 *
 * Structure to keep track of all allocations.
 */
struct igt_pvr_allocations {
	struct igt_pvr_allocation allocations[MAX_ALLOCATIONS];
	uint32_t count;
} igt_pvr_allocations;

/**
 * get_allocation:
 *
 * Function to get a new allocation structure from the global allocations array.
 *
 * Returns: Pointer to the empty allocation structure.
 */
static struct igt_pvr_allocation *get_allocation(void)
{
	struct igt_pvr_allocation *alloc;

	if (igt_pvr_allocations.count >= MAX_ALLOCATIONS) {
		igt_log("igt-pvr", IGT_LOG_CRITICAL,
			"Error: Maximum number of allocations reached (%d).\n", MAX_ALLOCATIONS);
		igt_assert(0); // Max allocations reached
	}
	alloc = &igt_pvr_allocations.allocations[igt_pvr_allocations.count++];
	alloc->bo_handle = 0;
	alloc->size = 0;
	alloc->cpu_addr = NULL;
	alloc->gpu_addr = 0;
	return alloc;
}

/**
 * igt_pvr_init_allocators:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to initialize heap allocators and reset the global allocations array.
 */
void igt_pvr_init_allocators(int fd)
{
	igt_pvr_init_heap_allocators(fd);
	igt_pvr_allocations.count = 0;
}

/**
 * igt_pvr_allocate_addr:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The virtual memory context.
 * @size: The size of the allocation.
 * @flags: Allocation flags.
 * @gpu_addr: The GPU address for the allocation.
 *
 * Function to allocate and map GPU memory at a specific address.
 *
 * Returns: Pointer to the allocation structure.
 */
struct igt_pvr_allocation *igt_pvr_allocate_addr(int fd, uint32_t vm_ctx, size_t size,
						 uint64_t flags, uint64_t gpu_addr)
{
	struct igt_pvr_allocation *alloc = get_allocation();
	size_t out_size = size;

	alloc->gpu_addr = gpu_addr;
	igt_assert(alloc->gpu_addr != 0);

	alloc->bo_handle = igt_pvr_ioctl_create_bo_ex(fd, &out_size, flags);
	igt_assert(alloc->bo_handle);

	igt_pvr_ioctl_vm_map(fd, vm_ctx, alloc->bo_handle, alloc->gpu_addr, 0, size);

	alloc->size = size;
	alloc->vm_ctx = vm_ctx;

	return alloc;
}

/**
 * igt_pvr_allocate:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The virtual memory context.
 * @size: The size of the allocation.
 * @flags: Allocation flags.
 * @heap_index: The index of the heap to allocate from.
 *
 * Function to allocate and map GPU memory from a specific heap.
 *
 * Returns: Pointer to the allocation structure.
 */
struct igt_pvr_allocation *igt_pvr_allocate(int fd, uint32_t vm_ctx, size_t size, uint64_t flags,
					    uint32_t heap_index)
{
	return igt_pvr_allocate_addr(fd, vm_ctx, size, flags, allocate_from_heap(heap_index, size));
}

/**
 * igt_pvr_allocate_general:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The virtual memory context.
 * @size: The size of the allocation.
 * @flags: Allocation flags.
 *
 * Function to allocate and map GPU memory from the general heap.
 *
 * Returns: Pointer to the allocation structure.
 */
struct igt_pvr_allocation *igt_pvr_allocate_general(int fd, uint32_t vm_ctx, size_t size, uint64_t flags)
{
	return igt_pvr_allocate(fd, vm_ctx, size, flags, DRM_PVR_HEAP_GENERAL);
}

/**
 * igt_pvr_get_cpu_addr:
 * @fd: The file descriptor of the DRM device.
 * @alloc: The allocation structure.
 *
 * Function to get the CPU address of the allocation.
 * If the CPU address is not already mapped, it will be mapped using mmap.
 *
 * Returns: Pointer to the CPU address.
 */
void *igt_pvr_get_cpu_addr(int fd, struct igt_pvr_allocation *alloc)
{
	igt_assert(alloc);
	igt_assert(alloc->bo_handle != 0);
	igt_assert(alloc->size != 0);
	if (!alloc->cpu_addr) {
		off_t mmap_offset = igt_pvr_ioctl_get_bo_mmap_offset(fd, alloc->bo_handle);

		alloc->cpu_addr = mmap(NULL, alloc->size, PROT_READ | PROT_WRITE, MAP_SHARED,
				       fd, mmap_offset);
		igt_assert(alloc->cpu_addr != MAP_FAILED);
	}
	return alloc->cpu_addr;
}

/**
 * igt_pvr_get_gpu_addr:
 * @alloc: The allocation structure.
 *
 * Function to get the GPU virtual address of the allocation.
 *
 * Returns: The GPU virtual address.
 */
uint64_t igt_pvr_get_gpu_addr(struct igt_pvr_allocation *alloc)
{
	igt_assert(alloc);
	igt_assert(alloc->gpu_addr != 0);
	return alloc->gpu_addr;
}

/**
 * igt_pvr_get_size:
 * @alloc: The allocation structure.
 *
 * Function to get the size of the allocation.
 *
 * Returns: The size of the allocation.
 */
size_t igt_pvr_get_size(struct igt_pvr_allocation *alloc)
{
	igt_assert(alloc);
	igt_assert(alloc->size != 0);
	return alloc->size;
}

/**
 * igt_pvr_free:
 * @fd: The file descriptor of the DRM device.
 * @alloc: The allocation structure.
 *
 * Function to free the allocation and unmap it from the VM context.
 */
static void igt_pvr_free(int fd, struct igt_pvr_allocation *alloc)
{
	igt_assert(alloc);
	if (alloc->bo_handle == 0)
		return;

	if (alloc->cpu_addr) {
		munmap(alloc->cpu_addr, alloc->size);
		alloc->cpu_addr = NULL;
	}
	igt_pvr_ioctl_vm_unmap(fd, alloc->vm_ctx, alloc->gpu_addr, alloc->size);
	gem_close(fd, alloc->bo_handle);

	alloc->bo_handle = 0;
	alloc->size = 0;
	alloc->gpu_addr = 0;
	alloc->vm_ctx = 0;
}

/**
 * igt_pvr_free_all:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to free all allocations.
 */
void igt_pvr_free_all(int fd)
{
	for (uint32_t i = 0; i < igt_pvr_allocations.count; i++) {
		struct igt_pvr_allocation *alloc = &igt_pvr_allocations.allocations[i];

		if (alloc->bo_handle != 0)
			igt_pvr_free(fd, alloc);
	}
	igt_pvr_allocations.count = 0;
}

/**
 * igt_pvr_get_device_info:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to get the device information from kernel.
 */
struct pvr_device_info *igt_pvr_get_device_info(int fd)
{
	struct drm_pvr_dev_query_gpu_info dev_info_get = {0};
	static struct pvr_device_info info;

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_GPU_INFO_GET,
				sizeof(dev_info_get), &dev_info_get, 0);

	igt_assert(pvr_device_info_init(&info, dev_info_get.gpu_id) == 0);

	return &info;
}

/**
 * igt_pvr_create_hwrt_geom_data_args:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The VM context.
 * @geom_data_args: The geometry data arguments structure to initialize.
 *
 * Function to create hardware render target ioctl geometry data arguments.
 *
 */
static void igt_pvr_create_hwrt_geom_data_args(int fd, uint32_t vm_ctx,
					       struct drm_pvr_create_hwrt_geom_data_args
					       *geom_data_args)
{
	geom_data_args->rtc_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
	geom_data_args->tpc_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
	geom_data_args->vheap_table_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
	geom_data_args->tpc_size = POWERVR_GPU_PAGE_SIZE;
	geom_data_args->tpc_stride = 0x1;
}

/**
 * igt_pvr_create_hwrt_rt_data_args:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The VM context.
 * @rt_data_args: The render target data arguments structure to initialize.
 *
 * Function to create hardware render target ioctl render target data arguments.
 *
 */
static void igt_pvr_create_hwrt_rt_data_args(int fd, uint32_t vm_ctx,
					     struct drm_pvr_create_hwrt_rt_data_args *rt_data_args)
{
	rt_data_args->pm_mlist_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
	rt_data_args->macrotile_array_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
	rt_data_args->region_header_dev_addr =
		igt_pvr_get_gpu_addr(igt_pvr_allocate_general(fd, vm_ctx, POWERVR_GPU_PAGE_SIZE,
							      DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS |
							      DRM_PVR_BO_BYPASS_DEVICE_CACHE));
}

/**
 * igt_pvr_ioctl_create_hwrt_dataset:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx: The VM context.
 * @free_list_handles: Array of free list handles.
 * @num_free_lists: Number of free list handles.
 *
 * Function to create a hardware render target dataset.
 *
 * Returns: Handle of the created hardware render target dataset.
 */
uint32_t igt_pvr_ioctl_create_hwrt_dataset(int fd, uint32_t vm_ctx, uint32_t *free_list_handles,
					   uint32_t num_free_lists)
{
	struct drm_pvr_ioctl_create_hwrt_dataset_args create_hwrt_args = {
		.width = 256,
		.height = 256,
		.samples = 1,
		.layers = 1,
		.region_header_size = 64,
	};

	igt_pvr_create_hwrt_geom_data_args(fd, vm_ctx, &create_hwrt_args.geom_data_args);
	igt_pvr_create_hwrt_rt_data_args(fd, vm_ctx, &create_hwrt_args.rt_data_args[0]);
	igt_pvr_create_hwrt_rt_data_args(fd, vm_ctx, &create_hwrt_args.rt_data_args[1]);

	for (uint32_t i = 0; i < num_free_lists; i++)
		create_hwrt_args.free_list_handles[i] = free_list_handles[i];

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &create_hwrt_args);

	return create_hwrt_args.handle;
}

/**
 * igt_pvr_ioctl_destroy_hwrt_dataset:
 * @fd: The file descriptor of the DRM device.
 * @hwrt_handle: The handle of the hardware render target dataset to destroy.
 *
 * Function to destroy a hardware render target dataset.
 */
void igt_pvr_ioctl_destroy_hwrt_dataset(int fd, uint32_t hwrt_handle)
{
	struct drm_pvr_ioctl_destroy_hwrt_dataset_args destroy_hwrt_args = {
		.handle = hwrt_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &destroy_hwrt_args);
}
