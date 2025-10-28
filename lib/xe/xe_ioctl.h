/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Jason Ekstrand <jason@jlekstrand.net>
 *    Maarten Lankhorst <maarten.lankhorst@linux.intel.com>
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_IOCTL_H
#define XE_IOCTL_H

#include <stddef.h>
#include <stdint.h>
#include <xe_drm.h>

#define DRM_XE_UFENCE_WAIT_MASK_U64    0xffffffffffffffffu

uint32_t xe_cs_prefetch_size(int fd);
uint64_t xe_bb_size(int fd, uint64_t reqsize);
uint32_t xe_vm_create(int fd, uint32_t flags, uint64_t ext);
int  ___xe_vm_bind(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
		   uint64_t offset, uint64_t addr, uint64_t size, uint32_t op,
		   uint32_t flags, struct drm_xe_sync *sync, uint32_t num_syncs,
		   uint32_t prefetch_region, uint8_t pat_index, uint64_t ext,
		   uint64_t op_ext);
int  __xe_vm_bind(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
		  uint64_t offset, uint64_t addr, uint64_t size, uint32_t op,
		  uint32_t flags, struct drm_xe_sync *sync, uint32_t num_syncs,
		  uint32_t region, uint8_t pat_index, uint64_t ext);
void  __xe_vm_bind_assert(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
			  uint64_t offset, uint64_t addr, uint64_t size,
			  uint32_t op, uint32_t flags, struct drm_xe_sync *sync,
			  uint32_t num_syncs, uint32_t prefetch_region, uint64_t ext);
void xe_vm_prefetch_async(int fd, uint32_t vm, uint32_t exec_queue,
			  uint64_t offset, uint64_t addr, uint64_t size,
			  struct drm_xe_sync *sync, uint32_t num_syncs,
			  uint32_t region);
void xe_vm_bind_async(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
		      uint64_t offset, uint64_t addr, uint64_t size,
		      struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_userptr_async(int fd, uint32_t vm, uint32_t exec_queue,
			      uint64_t userptr, uint64_t addr, uint64_t size,
			      struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_async_flags(int fd, uint32_t vm, uint32_t exec_queue, uint32_t bo,
			    uint64_t offset, uint64_t addr, uint64_t size,
			    struct drm_xe_sync *sync, uint32_t num_syncs,
			    uint32_t flags);
void xe_vm_bind_userptr_async_flags(int fd, uint32_t vm, uint32_t exec_queue,
				    uint64_t userptr, uint64_t addr,
				    uint64_t size, struct drm_xe_sync *sync,
				    uint32_t num_syncs, uint32_t flags);
void xe_vm_unbind_async(int fd, uint32_t vm, uint32_t exec_queue,
			uint64_t offset, uint64_t addr, uint64_t size,
			struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_sync(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
		     uint64_t addr, uint64_t size);
void xe_vm_unbind_sync(int fd, uint32_t vm, uint64_t offset,
		       uint64_t addr, uint64_t size);
void xe_vm_bind_array(int fd, uint32_t vm, uint32_t exec_queue,
		      struct drm_xe_vm_bind_op *bind_ops,
		      uint32_t num_bind, struct drm_xe_sync *sync,
		      uint32_t num_syncs);
void xe_vm_unbind_all_async(int fd, uint32_t vm, uint32_t exec_queue,
			    uint32_t bo, struct drm_xe_sync *sync,
			    uint32_t num_syncs);
void xe_vm_destroy(int fd, uint32_t vm);
uint32_t __xe_bo_create(int fd, uint32_t vm, uint64_t size, uint32_t placement,
			uint32_t flags, void *ext, uint32_t *handle);
uint32_t xe_bo_create(int fd, uint32_t vm, uint64_t size, uint32_t placement,
		      uint32_t flags);
uint32_t __xe_bo_create_caching(int fd, uint32_t vm, uint64_t size, uint32_t placement,
				uint32_t flags, uint16_t cpu_caching, uint32_t *handle);
uint32_t xe_bo_create_caching(int fd, uint32_t vm, uint64_t size, uint32_t placement,
			      uint32_t flags, uint16_t cpu_caching);
uint16_t __xe_default_cpu_caching(int fd, uint32_t placement, uint32_t flags);
int __xe_exec_queue_create(int fd, uint32_t vm, uint16_t width, uint16_t num_placements,
			   struct drm_xe_engine_class_instance *instance,
			   uint64_t ext, uint32_t *exec_queue_id);
uint32_t xe_exec_queue_create(int fd, uint32_t vm,
			  struct drm_xe_engine_class_instance *instance,
			  uint64_t ext);
uint32_t xe_bind_exec_queue_create(int fd, uint32_t vm, uint64_t ext);
uint32_t xe_exec_queue_create_class(int fd, uint32_t vm, uint16_t class);
void xe_exec_queue_destroy(int fd, uint32_t exec_queue);
uint64_t xe_bo_mmap_offset(int fd, uint32_t bo);
void *xe_bo_map(int fd, uint32_t bo, size_t size);
void *xe_bo_map_fixed(int fd, uint32_t bo, size_t size, uint64_t addr);
void *xe_bo_mmap_ext(int fd, uint32_t bo, size_t size, int prot);
int __xe_exec(int fd, struct drm_xe_exec *exec);
void xe_exec(int fd, struct drm_xe_exec *exec);
void xe_exec_sync(int fd, uint32_t exec_queue, uint64_t addr,
		  struct drm_xe_sync *sync, uint32_t num_syncs);
int xe_exec_sync_failable(int fd, uint32_t exec_queue, uint64_t addr,
		  struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_exec_wait(int fd, uint32_t exec_queue, uint64_t addr);
int __xe_wait_ufence(int fd, uint64_t *addr, uint64_t value,
		     uint32_t exec_queue, int64_t *timeout);
int64_t xe_wait_ufence(int fd, uint64_t *addr, uint64_t value,
		       uint32_t exec_queue, int64_t timeout);

#endif /* XE_IOCTL_H */
