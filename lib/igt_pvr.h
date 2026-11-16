/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#ifndef IGT_PVR_H
#define IGT_PVR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "imagination/pvr_device_info.h"
#include "pvr_drm.h"

#define DRM_PVR_EMPTY_OBJ_ARRAY(type) \
	{ .stride = sizeof(type), .count = 0, .array = 0 }

#define STREAM_ADD_ITEM(condition, type, data, stream) \
	do { \
		if (condition) { \
			type *item = (type *)(stream); \
			*item = data; \
			stream += sizeof(type); \
		} \
	} while (0)

struct igt_pvr_allocation {
	uint32_t bo_handle;
	size_t size;
	void *cpu_addr;
	uint64_t gpu_addr;
	uint32_t vm_ctx;
};

uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size);
uint32_t igt_pvr_ioctl_create_bo_ex(int fd, size_t *size, uint64_t flags);
off_t igt_pvr_ioctl_get_bo_mmap_offset(int fd, uint32_t handle);

struct drm_pvr_ioctl_dev_query_args
igt_pvr_ioctl_dev_query(int fd, enum drm_pvr_dev_query type, uint64_t size,
			void *pointer, int expect_err);

struct drm_pvr_heap *
igt_pvr_get_heap_info(int fd, uint32_t *array_len_out);
struct drm_pvr_heap igt_pvr_find_general_heap(int fd);

struct drm_pvr_static_data_area *
igt_pvr_get_static_data_areas(int fd, uint32_t *array_len_out);

uint32_t igt_pvr_ioctl_create_vm_context(int fd, int expect_err);
void igt_pvr_ioctl_destroy_vm_context(int fd, uint32_t handle, int expect_err);

void igt_pvr_ioctl_vm_map(int fd, uint32_t vm_ctx_handle, uint32_t handle,
			  uint64_t device_addr, uint64_t offset, uint64_t size);
void igt_pvr_ioctl_vm_unmap(int fd, uint32_t vm_ctx_handle,
			    uint64_t device_addr, uint64_t size);

uint32_t igt_pvr_ioctl_create_context(int fd, enum drm_pvr_ctx_type type,
				      uint32_t vm_ctx_handle);
void igt_pvr_ioctl_destroy_context(int fd, uint32_t ctx_handle);

uint32_t igt_pvr_ioctl_create_free_list(int fd, uint32_t vm_ctx_handle,
					uint64_t gpu_addr);
void igt_pvr_ioctl_destroy_free_list(int fd, uint32_t free_list_handle);

void igt_pvr_init_allocators(int fd);
struct igt_pvr_allocation *igt_pvr_allocate_addr(int fd, uint32_t vm_ctx, size_t size,
						 uint64_t flags, uint64_t gpu_addr);
struct igt_pvr_allocation *igt_pvr_allocate(int fd, uint32_t vm_ctx, size_t size, uint64_t flags,
					    uint32_t heap_index);
struct igt_pvr_allocation *igt_pvr_allocate_general(int fd, uint32_t vm_ctx, size_t size,
					    uint64_t flags);
void *igt_pvr_get_cpu_addr(int fd, struct igt_pvr_allocation *alloc);
uint64_t igt_pvr_get_gpu_addr(struct igt_pvr_allocation *alloc);
size_t igt_pvr_get_size(struct igt_pvr_allocation *alloc);
void igt_pvr_free_all(int fd);

struct pvr_device_info *igt_pvr_get_device_info(int fd);

uint32_t igt_pvr_ioctl_create_hwrt_dataset(int fd, uint32_t vm_ctx, uint32_t *free_list_handles,
					   uint32_t num_free_lists);
void igt_pvr_ioctl_destroy_hwrt_dataset(int fd, uint32_t hwrt_handle);

#endif /* IGT_PVR_H */
