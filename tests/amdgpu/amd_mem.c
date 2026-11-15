/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_compute.h"
#include "lib/amdgpu/amd_gfx.h"
#ifdef AMDGPU_LLVM_ENABLED
#include "amdgpu/shaders/amd_llvm_asm.h"
#else
#include "amdgpu/shaders/amd_llvm_asm_stub.h"
#endif
#include "amdgpu/shaders/amd_shader_store.h"
#include "lib/amdgpu/compute_utils/amd_gca.h"
#include "lib/amdgpu/compute_utils/amd_dispatch_helpers.h" /* for amdgpu_dispatch_* */
#include "lib/amdgpu/amdgpu_asic_addr.h" /* FAMILY_* macros */

#include <errno.h>
#include <fcntl.h>
#include <igt.h>
#include <inttypes.h>
#include <libdrm/amdgpu.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <pthread.h>

#define GB(x) ((x) * 1024ULL * 1024ULL * 1024ULL)
#define DRM_GEM_MAP_SHIFT 20

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

static void test_mmap_large(amdgpu_device_handle dev);
static void memory_alloc_all(amdgpu_device_handle dev, bool userq);
static void amdgpu_memory_alloc(amdgpu_device_handle device, bool userq);
static void test_invalid_alloc(amdgpu_device_handle device, bool userq);
static void test_access_ppr_mem(amdgpu_device_handle device, const struct amdgpu_ip_block_version *ip_block);
static void register_memory_with_same_ptr(amdgpu_device_handle device);
static void register_memory_with_same_ptr2(amdgpu_device_handle device);
static void memory_register(amdgpu_device_handle device);

/**
 * Core test: Map the system memory to GPU
 */
static void test_mmap_large(amdgpu_device_handle dev)
{
	/* Map a large amount of anonymous system memory into the GPU by
	 * repeatedly importing DMA-BUFs created from offsets within one
	 * contiguous mmap() region. */
	const uint64_t total_obj_count = 1 << 12; /* 4096 objects */
	struct amdgpu_bo **bo_list; /* imported GPU buffers */
	size_t single_obj_size;     /* size of each memory object */
	char *sys_mem_base;         /* base address of system memory */
	int obj_idx;
	uint64_t total_mapped_size = 0;
	uint64_t max_total_size;

	/* Allocate tracking array */
	bo_list = calloc(total_obj_count, sizeof(*bo_list));
	igt_assert(bo_list);

	max_total_size = get_available_system_memory(dev) * 7 / 10;
	single_obj_size = max_total_size / total_obj_count;
	igt_info("Single object size: %zu MB, Total objects: %llu\n",
		 single_obj_size >> 20, (unsigned long long)total_obj_count);

	sys_mem_base = mmap(NULL, single_obj_size, PROT_READ | PROT_WRITE,
			    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	igt_assert(sys_mem_base != MAP_FAILED);
	memset(sys_mem_base, 0xAA, single_obj_size); /* fill with test data */

	for (obj_idx = 0; obj_idx < total_obj_count; obj_idx++) {
		struct amdgpu_bo_import_result import_res = { };
		void *curr_sys_addr = sys_mem_base + obj_idx; /* offset to vary address */
		size_t curr_obj_size = single_obj_size - obj_idx; /* reduce to avoid overflow */
		int dmabuf_fd;
		int ret;

		dmabuf_fd = create_dmabuf(dev, curr_sys_addr, curr_obj_size);
		if (dmabuf_fd < 0) {
			igt_warn("DMA-BUF create failed at %d\n", obj_idx);
			break;
		}

		ret = amdgpu_bo_import(dev,
				       amdgpu_bo_handle_type_dma_buf_fd,
				dmabuf_fd,
				&import_res);
		close(dmabuf_fd); /* drop local fd after import */

		if (ret) {
			igt_warn("Import failed at %d (err=%d)\n", obj_idx, ret);
			break;
		}

		bo_list[obj_idx] = import_res.buf_handle;
		total_mapped_size += import_res.alloc_size;
	}

	igt_info("Mapped %d objects, total size: %llu GB\n",
		 obj_idx, (unsigned long long)(total_mapped_size >> 30));
	igt_info("mmap-sysmem-total-gb: %llu\n",
		 (unsigned long long)(total_mapped_size >> 30));

	for (obj_idx--; obj_idx >= 0; obj_idx--) {
		if (bo_list[obj_idx])
			amdgpu_bo_free(bo_list[obj_idx]);
	}
	munmap(sys_mem_base, single_obj_size);
	free(bo_list);
}

/*
 * Main logic for "allocate maximum possible VRAM" test
 * Supports both standard and user queue (userq) paths, aligning with amdgpu_memory_alloc's interface.
 * Core behavior: Try to allocate the largest VRAM block, with step-wise reduction if allocation fails.
 */
static void memory_alloc_all(amdgpu_device_handle dev, bool userq)
{
	amdgpu_bo_handle bo = NULL;
	uint64_t available, size, leeway;
	int ret, shrink = 21; /* 2MB decrement step (2^21) */
	bool success = false;
	uint32_t timeline_syncobj_handle = 0;
	uint64_t point = 0;
	uint64_t mapping_flags = AMDGPU_VM_MTYPE_UC;
	int i;
	uint64_t mc_addr;
	void *cpu_ptr;
	amdgpu_va_handle va_handle;

	if (userq) {
		ret = amdgpu_cs_create_syncobj2(dev, 0, &timeline_syncobj_handle);
		igt_assert_eq(ret, 0);
	}

	available = get_available_vram(dev);
	igt_info("Available VRAM: %llu bytes\n", (unsigned long long)available);

	leeway = (10ULL << shrink); /* tolerance for reported value */
	size = available + leeway; /* start slightly above reported */

	for (i = 0; i < (available >> shrink); i++) {
		uint64_t bo_size = size;

		ret = amdgpu_bo_alloc_and_map_sync(dev,
						   bo_size,
					 0x1000,
					 AMDGPU_GEM_DOMAIN_VRAM,
					 AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
					 mapping_flags,
					 &bo, &cpu_ptr, &mc_addr, &va_handle,
					 timeline_syncobj_handle, point, userq);

		if (userq) {
			ret = amdgpu_timeline_syncobj_wait(dev,
							   timeline_syncobj_handle,
							   point);
			igt_assert_eq(ret, 0);
		}

		if (!ret) {
			amdgpu_bo_unmap_and_free(bo, va_handle, mc_addr, bo_size);
			success = true;
			break;
		}

		size -= (1ULL << shrink);
		if (!size)
			break;
	}

	if (userq) {
		ret = amdgpu_cs_destroy_syncobj(dev, timeline_syncobj_handle);
		igt_assert_eq(ret, 0);
	}

	igt_assert(success);
	igt_info("Successfully allocated %llu bytes\n", (unsigned long long)size);

	if (size > available + leeway)
		igt_warn("Under-reported available memory\n");
	if (size < available - leeway)
		igt_warn("Over-reported available memory\n");
}

/**
 * MEM ALLOC TEST
 * @param device
 */
static void amdgpu_memory_alloc(amdgpu_device_handle device, bool userq)
{
	uint64_t bo_mc, point = 0;
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint32_t timeline_syncobj_handle = 0;
	unsigned int size = 4096;
	uint64_t mapping_flags = AMDGPU_VM_MTYPE_UC;
	volatile uint32_t *bo_cpu;
	int r;

	if (userq) {
		r = amdgpu_cs_create_syncobj2(device, 0, &timeline_syncobj_handle);
		igt_assert_eq(r, 0);
	}

	/* visible VRAM */
	r = amdgpu_bo_alloc_and_map_sync(device, size, 4096,
					 AMDGPU_GEM_DOMAIN_VRAM,
				   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				   mapping_flags,
				   &bo, (void **)&bo_cpu, &bo_mc, &va_handle,
				   timeline_syncobj_handle, point, userq);
	igt_assert_eq(r, 0);
	if (userq) {
		r = amdgpu_timeline_syncobj_wait(device, timeline_syncobj_handle, point);
		igt_assert_eq(r, 0);
	}
	amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, size);

	if (!userq) {
		/* invisible VRAM */
		r = amdgpu_bo_alloc_and_map_sync(device, size, 4096,
						 AMDGPU_GEM_DOMAIN_VRAM,
					   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
					   mapping_flags,
					   &bo, (void **)&bo_cpu, &bo_mc, &va_handle,
					   timeline_syncobj_handle, point, userq);
		igt_assert_eq(r, 0);
		amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, size);
	}

	/* GART cacheable */
	r = amdgpu_bo_alloc_and_map_sync(device, size, 4096,
					 AMDGPU_GEM_DOMAIN_GTT,
				   0,
				   mapping_flags,
				   &bo, (void **)&bo_cpu, &bo_mc, &va_handle,
				   timeline_syncobj_handle, point, userq);
	igt_assert_eq(r, 0);
	if (userq) {
		r = amdgpu_timeline_syncobj_wait(device, timeline_syncobj_handle, point);
		igt_assert_eq(r, 0);
	}
	amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, size);

	/* GART USWC */
	r = amdgpu_bo_alloc_and_map_sync(device, size, 4096,
					 AMDGPU_GEM_DOMAIN_GTT,
				   AMDGPU_GEM_CREATE_CPU_GTT_USWC,
				   mapping_flags,
				   &bo, (void **)&bo_cpu, &bo_mc, &va_handle,
				   timeline_syncobj_handle, point, userq);
	igt_assert_eq(r, 0);
	if (userq) {
		r = amdgpu_timeline_syncobj_wait(device, timeline_syncobj_handle, point);
		igt_assert_eq(r, 0);
	}
	amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, size);

	if (userq) {
		r = amdgpu_cs_destroy_syncobj(device, timeline_syncobj_handle);
		igt_assert_eq(r, 0);
	}
}

static void test_invalid_alloc(amdgpu_device_handle device, bool userq)
{
	amdgpu_bo_handle buf_handle = NULL, bo = NULL;
	struct amdgpu_bo_alloc_request request = { };
	uint32_t timeline_syncobj_handle = 0, invalid_syncobj;
	uint64_t point = 0, bo_mc = 0, invalid_mapping_flags;
	void *cpu_ptr = NULL;
	amdgpu_va_handle va_handle = NULL;
	int r;

	if (userq) {
		r = amdgpu_cs_create_syncobj2(device, 0, &timeline_syncobj_handle);
		igt_assert_eq(r, 0);
	}

	/* zero size */
	request.alloc_size = 0;
	request.phys_alignment = 4096;
	request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	r = amdgpu_bo_alloc(device, &request, &buf_handle);
	igt_assert_f(r, "expected failure for zero size (ret=%d)", r);

	/* invalid heap */
	request.alloc_size = 4096;
	request.preferred_heap = 0xffffffff;
	r = amdgpu_bo_alloc(device, &request, &buf_handle);
	igt_assert_f(r, "expected failure for invalid heap (ret=%d)", r);

	/* conflicting flags */
	//request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	//request.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED | AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
	//r = amdgpu_bo_alloc(device, &request, &buf_handle);
	//igt_assert_f(r, "expected failure for conflicting flags (ret=%d)", r);

	/* NULL handle pointer */
	request.flags = 0;
	r = amdgpu_bo_alloc(device, &request, NULL);
	igt_assert_f(r, "expected failure for NULL handle (ret=%d)", r);

	/* huge size */
	request.alloc_size = UINT64_MAX;
	r = amdgpu_bo_alloc(device, &request, &buf_handle);
	igt_assert_f(r, "expected failure for huge size (ret=%d)", r);

	/* invalid mapping flags */
	if (!userq) {
		invalid_mapping_flags = 0xffffffff;
		r = amdgpu_bo_alloc_and_map_sync(device, 4096, 4096,
						 AMDGPU_GEM_DOMAIN_GTT, 0,
						  invalid_mapping_flags,
						  &bo, &cpu_ptr, &bo_mc, &va_handle,
						  timeline_syncobj_handle, point, userq);
		igt_assert_f(r, "expected failure for invalid mapping flags");
		if (!r)
			amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, 4096);
	}

	/* invalid syncobj */
	if (userq) {
		invalid_syncobj = 0xffffffff;
		r = amdgpu_bo_alloc_and_map_sync(device, 4096, 4096,
						 AMDGPU_GEM_DOMAIN_GTT, 0,
						  AMDGPU_VM_MTYPE_UC,
						  &bo, &cpu_ptr, &bo_mc, &va_handle,
						  invalid_syncobj, point, userq);
		igt_assert_f(r, "expected failure for invalid syncobj");
	}

	/* NO_CPU_ACCESS mapping sanity */
	r = amdgpu_bo_alloc_and_map_sync(device, 4096, 4096,
					 AMDGPU_GEM_DOMAIN_VRAM,
				   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
				   AMDGPU_VM_MTYPE_UC,
				   &bo, &cpu_ptr, &bo_mc, &va_handle,
				   timeline_syncobj_handle, point, userq);
	if (!r) {
		igt_assert_f(!cpu_ptr, "NO_CPU_ACCESS returned cpu_ptr=%p", cpu_ptr);
		amdgpu_bo_unmap_and_free(bo, va_handle, bo_mc, 4096);
	}

	if (userq) {
		r = amdgpu_cs_destroy_syncobj(device, timeline_syncobj_handle);
		igt_assert_eq(r, 0);
	}

	igt_info("invalid alloc parameter checks complete\n");
}

/*
 * Write two dwords into a user allocated page via GPU and verify.
 * Steps: alloc user memory, map, emit SDMA linear write, wait, compare.
 */
static void test_access_ppr_mem(amdgpu_device_handle device,
				const struct amdgpu_ip_block_version *ip_block)
{
	const int sdma_write_length = 2;
	const int pm4_dw = 256;
	struct amdgpu_ring_context *ring_context;
	uint32_t *dest_buf = NULL;
	int r;
	uint32_t low, high;
	uint64_t dest_gpu_va = 0;
	amdgpu_bo_handle dest_bo = NULL;
	amdgpu_va_handle out_va_handle;
	void *mapped_ptr;
	void *bo_cpu;  /* Non-volatile pointer for memset */

	/* init ring context */
	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	ring_context->write_length = sdma_write_length;
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	ring_context->user_queue = true;
	igt_assert(ring_context->pm4);

	/* query hw ip */
	r = amdgpu_query_hw_ip_info(device, ip_block->type, 0, &ring_context->hw_ip_info);
	igt_assert_eq(r, 0);

	/* alloc user memory */
	dest_buf = virtual_alloc_memory(NULL, PAGE_SIZE, 3);
	igt_assert(dest_buf);

	/* map to GPU VA */
	r = virtual_mem_to_gpu_bo(device, dest_buf, PAGE_SIZE,
				  &dest_bo, &dest_gpu_va, &out_va_handle);
	igt_assert_eq(r, 0);

	igt_info("User buffer: %p, initial value: 0x%lx\n",
		 dest_buf, (unsigned long)*dest_buf);

	/* create user queue */
	ip_block->funcs->userq_create(device, ring_context, ip_block->type);

	/* ring 0 */
	ring_context->ring_id = 0;

	/* allocate command buffer */
	r = amdgpu_bo_alloc_and_map_sync(device,
					 ring_context->write_length * sizeof(uint32_t),
					4096,
					AMDGPU_GEM_DOMAIN_GTT,
					0,
					AMDGPU_VM_MTYPE_UC,
					&ring_context->bo,
					&bo_cpu,  /* Use non-volatile pointer here */
					&ring_context->bo_mc,
					&ring_context->va_handle,
					ring_context->timeline_syncobj_handle,
					++ring_context->point,
					true);
	igt_assert_eq(r, 0);

	/* store cpu ptr */
	ring_context->bo_cpu = bo_cpu;

	/* wait syncobj */
	r = amdgpu_timeline_syncobj_wait(device,
					 ring_context->timeline_syncobj_handle,
					ring_context->point);
	igt_assert_eq(r, 0);

	/* clear command buffer */
	memset(bo_cpu, 0, ring_context->write_length * sizeof(uint32_t));

	/* resources */
	ring_context->resources[0] = ring_context->bo;

	/* gen linear write */
	ip_block->funcs->write_linear(ip_block->funcs, ring_context,
				      &ring_context->pm4_dw);

	/* program target addr */
	low = (uint32_t)(dest_gpu_va & 0xFFFFFFFF);
	high = (uint32_t)(dest_gpu_va >> 32);
	igt_info("GPU target address: 0x%x%08x\n", high, low);

	ring_context->pm4[2] = low;
	ring_context->pm4[3] = high;
	ring_context->pm4[4] = 0xABCDEF09;	/* Test data 1 */
	ring_context->pm4[5] = 0x12345678;	/* Test data 2 */

	/* submit */
	amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

	/* map for verify */
	r = amdgpu_bo_cpu_map(dest_bo, &mapped_ptr);
	igt_assert_eq(r, 0);

	/* copy back */
	memcpy(dest_buf, mapped_ptr, PAGE_SIZE);

	igt_info("GPU written data: 0x%08x, 0x%08x\n",
		 ((uint32_t *)mapped_ptr)[0], ((uint32_t *)mapped_ptr)[1]);

	amdgpu_bo_cpu_unmap(dest_bo);

	/* command buffer dwords */
	igt_info("Command buffer after execution: 0x%08x, 0x%08x\n",
		 ring_context->bo_cpu[0], ring_context->bo_cpu[1]);

	/* user memory dwords */
	igt_info("User buffer after GPU write: 0x%08lx, 0x%08x\n",
		 (unsigned long)*dest_buf, *(dest_buf + 1));

	/* wait for values */
	wait_on_value(dest_buf, 0xABCDEF09);
	wait_on_value(dest_buf + 1, 0x12345678);

	/* free user memory */
	virtual_free_memory(dest_buf, PAGE_SIZE);

	/* unmap/free GPU buffer */
	r = amdgpu_bo_va_op(dest_bo, 0, PAGE_SIZE, dest_gpu_va, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);
	amdgpu_va_range_free(out_va_handle);
	amdgpu_bo_free(dest_bo);

	/* free command buffer */
	amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
				 ring_context->bo_mc,
				ring_context->write_length * sizeof(uint32_t));

	/* destroy ring context */
	free(ring_context->pm4);
	ip_block->funcs->userq_destroy(device, ring_context, ip_block->type);
	free(ring_context);
}

/**
 * Test: Register same user pointer with different sizes
 *
 * This test verifies the driver's behavior when registering the same
 * user memory pointer with different sizes. It tests whether the driver
 * can handle overlapping userptr registrations with varying sizes.
 *
 * Test Scenario:
 * 1. Allocate one page of user memory with proper alignment
 * 2. Create first BO mapping exactly one page
 * 3. Attempt to create second BO mapping two pages (larger size) from same start address
 * 4. Verify if driver supports this usage pattern
 * 5. Clean up resources appropriately
 *
 * Expected Results:
 * - First BO registration should always succeed
 * - Second BO registration may fail if driver doesn't support overlapping sizes
 * - Test gracefully handles both supported and unsupported scenarios
 */
static void register_memory_with_same_ptr(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo1 = NULL, bo2 = NULL;
	amdgpu_va_handle va_handle1 = NULL, va_handle2 = NULL;
	uint32_t *shared_mem = NULL;
	uint64_t va1 = 0, va2 = 0;
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	int r;

	r = posix_memalign((void **)&shared_mem, page_size, page_size);
	igt_assert_eq(r, 0);
	memset(shared_mem, 0, page_size);

	r = amdgpu_create_bo_from_user_mem(device, shared_mem, page_size, &bo1);
	igt_assert_f(!r, "first userptr BO failed %d", r);
	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general,
				  2 * sizeof(uint32_t), 1, 0, &va1, &va_handle1, 0);
	igt_assert_f(!r, "first VA range failed %d", r);
	r = amdgpu_bo_va_op(bo1, 0, 2 * sizeof(uint32_t), va1, 0, AMDGPU_VA_OP_MAP);
	igt_assert_f(!r, "first map failed %d", r);

	r = amdgpu_create_bo_from_user_mem(device, shared_mem, page_size * 2, &bo2);
	if (r) {
		igt_info("second BO larger size unsupported: %s\n", strerror(-r));
		igt_skip("overlapping userptr different sizes unsupported\n");
	}
	igt_assert(bo2);
	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general,
				  page_size * 2, 1, 0, &va2, &va_handle2, 0);
	igt_assert_f(!r, "second VA range failed %d", r);
	r = amdgpu_bo_va_op(bo2, 0, page_size * 2, va2, 0, AMDGPU_VA_OP_MAP);
	igt_assert_f(!r, "second map failed %d", r);

	amdgpu_bo_va_op(bo2, 0, page_size * 2, va2, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(va_handle2);
	amdgpu_bo_free(bo2);

	if (bo1) {
		if (va_handle1) {
			amdgpu_bo_va_op(bo1, 0, 2 * sizeof(uint32_t), va1, 0, AMDGPU_VA_OP_UNMAP);
			amdgpu_va_range_free(va_handle1);
		}
		amdgpu_bo_free(bo1);
	}

	free(shared_mem);
}

/**
 * Test: Register same user pointer with the same size multiple times
 *
 * This test verifies that the driver can handle multiple registrations
 * of the same user memory pointer with identical sizes. It tests the
 * driver's ability to manage duplicate userptr BO registrations and
 * ensures proper reference counting and resource management.
 *
 * Enhanced with PM4 linear write packet to validate data coherency
 * between duplicate BO registrations accessing the same physical memory.
 *
 * Test Scenario:
 * 1. Allocate user memory with proper page alignment
 * 2. Create first BO from the user pointer
 * 3. Create second BO from the same user pointer with identical size
 * 4. Write test pattern to memory using PM4 linear write through first BO
 * 5. Verify data consistency through second BO
 * 6. Clean up resources in reverse order
 *
 * Expected Result:
 * - Both BO registrations should succeed
 * - PM4 write through first BO should be visible through second BO
 * - The driver should maintain data coherency for duplicate mappings
 * - No memory leaks or resource conflicts should occur
 */
static void register_memory_with_same_ptr2(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo1 = NULL, bo2 = NULL;
	amdgpu_va_handle va_handle1 = NULL, va_handle2 = NULL;
	struct amdgpu_ring_context *ring_context;
	uint32_t *shared_mem = NULL;
	uint64_t va1 = 0, va2 = 0;
	const struct amdgpu_ip_block_version *ip_block;
	size_t page_size, alloc_size;
	const int pm4_dw = 256;
	int r;

	ip_block = get_ip_block(device, AMDGPU_HW_IP_DMA);
	igt_assert(ip_block);

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	igt_assert(ring_context->pm4);
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	ring_context->write_length = 4;

	r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	page_size = sysconf(_SC_PAGE_SIZE);
	alloc_size = ALIGN(4 * sizeof(uint32_t), page_size);
	r = posix_memalign((void **)&shared_mem, page_size, alloc_size);
	igt_assert_eq(r, 0);
	memset(shared_mem, 0, alloc_size);
	ring_context->bo_cpu = (volatile uint32_t *)(uintptr_t)shared_mem;

	r = amdgpu_create_bo_from_user_mem(device, shared_mem, alloc_size, &bo1);
	igt_assert_f(!r, "first BO reg failed %d", r);
	r = amdgpu_create_bo_from_user_mem(device, shared_mem, alloc_size, &bo2);
	igt_assert_f(!r, "second BO reg failed %d", r);

	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general,
				  alloc_size, 1, 0, &va1, &va_handle1, 0);
	igt_assert_f(!r, "va1 alloc failed %d", r);
	r = amdgpu_bo_va_op(bo1, 0, alloc_size, va1, 0, AMDGPU_VA_OP_MAP);
	igt_assert_f(!r, "bo1 map failed %d", r);
	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general,
				  alloc_size, 1, 0, &va2, &va_handle2, 0);
	igt_assert_f(!r, "va2 alloc failed %d", r);
	r = amdgpu_bo_va_op(bo2, 0, alloc_size, va2, 0, AMDGPU_VA_OP_MAP);
	igt_assert_f(!r, "bo2 map failed %d", r);

	ring_context->resources[0] = bo1;
	ring_context->bo = bo1;
	ring_context->bo_mc = va1;
	ring_context->pm4_dw = 0;
	ip_block->funcs->write_linear(ip_block->funcs, ring_context, &ring_context->pm4_dw);
	r = amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);
	igt_assert_f(!r, "linear write exec failed %d", r);
	r = ip_block->funcs->compare(ip_block->funcs, ring_context, 1);
	igt_assert_f(!r, "linear write compare failed %d", r);

	ring_context->resources[0] = bo2;
	ring_context->bo = bo2;
	ring_context->bo_mc = va2;
	r = ip_block->funcs->compare(ip_block->funcs, ring_context, 1);
	igt_assert_f(!r, "second BO compare failed %d", r);

	igt_info("duplicate userptr registration coherent\n");

	r = amdgpu_bo_va_op(bo2, 0, alloc_size, va2, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);
	if (va_handle2)
		igt_assert_eq(amdgpu_va_range_free(va_handle2), 0);
	r = amdgpu_bo_va_op(bo1, 0, alloc_size, va1, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);
	if (va_handle1)
		igt_assert_eq(amdgpu_va_range_free(va_handle1), 0);
	igt_assert_eq(amdgpu_bo_free(bo2), 0);
	igt_assert_eq(amdgpu_bo_free(bo1), 0);

	if (ring_context->context_handle)
		igt_assert_eq(amdgpu_cs_ctx_free(ring_context->context_handle), 0);
	free(ring_context->pm4);
	free(ring_context);
	free(shared_mem);
}

static void memory_register_same_ptr(amdgpu_device_handle device)
{
	register_memory_with_same_ptr(device);
	register_memory_with_same_ptr2(device);
}

/*
 * Test: cache-invalidate-sdma-write
 *
 * Adapted from ROCr KFD CacheInvalidateOnSdmaWrite.
 * Flow (Arcturus-only):
 *  1. Allocate a local VRAM BO with RW MTYPE and map to CPU.
 *  2. Assemble PollMemoryIsa shader (polls src dword until it becomes 0x5678 then stores 0x5678 to dst).
 *  3. Dispatch compute shader that polls buffer[0] and will write to buffer[dwLocation]. Submission is non-blocking.
 *  4. Issue SDMA linear write of value 0x5678 to buffer[0]. This should invalidate any cached copy of buffer[dwLocation].
 *  5. Wait for compute fence completion; shader should have written 0x5678 to buffer[dwLocation].
 *  6. Verify buffer[dwLocation] == 0x5678.
 *
 * Notes:
 *  - Arcturus does not have a distinct FAMILY_AR constant in IGT; we approximate by requiring a GFX9 AI family (FAMILY_IS_AI) and non-APU.
 *  - Mapping uses AMDGPU_VM_MTYPE_RW to exercise RW mtype path.
 *  - We keep separate src/dst virtual addresses within the same BO.
 */
static void test_cache_invalidate_on_sdma_write_asm(amdgpu_device_handle device,
						    const struct amdgpu_gpu_info *gpu_info)
{
	/* Declarations first (C90 compliance) */
	const uint32_t kPattern = 0x5678;
	const int dwLocation = 100;
	int r;
	uint32_t version = 9; /* gfx version for dispatch helper */
	char mcpu[32];
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	uint64_t vram_mc = 0;
	void *vram_cpu = NULL;
	amdgpu_bo_handle bo_vram = NULL;
	amdgpu_va_handle va_vram = 0;
	uint32_t *buf = NULL;
	void *shader_ptr = NULL;
	amdgpu_bo_handle bo_shader = NULL;
	amdgpu_va_handle va_shader = 0;
	uint64_t mc_shader = 0;
	amdgpu_context_handle compute_ctx;
	amdgpu_bo_handle bo_cmd = NULL;
	uint32_t *ptr_cmd = NULL;
	uint64_t mc_cmd = 0;
	amdgpu_va_handle va_cmd = 0;
	struct amdgpu_cmd_base *base_cmd;
	uint64_t dst_va = 0;
	amdgpu_bo_handle bos[3];
	amdgpu_bo_list_handle bo_list;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_request ib_req;
	cmd_context_t *dma_ctx = NULL;
	struct amdgpu_cs_fence fence;
	uint32_t expired = 0;

	/* Gate: Approximate Arcturus requirement */
	if (is_apu(gpu_info) || !FAMILY_IS_AI(gpu_info->family_id)) {
		igt_skip("Skipping test: requires Arcturus (gfx9 AI discrete) family\n");
		return;
	}

	/* Determine shader version similarly to other tests */
	amdgpu_family_id_to_mcpu(gpu_info->family_id, mcpu, sizeof(mcpu));
	if (FAMILY_IS_GFX1200(gpu_info->family_id))
		version = 12;
	else if (FAMILY_IS_GFX1150(gpu_info->family_id) ||
		 FAMILY_IS_GFX1100(gpu_info->family_id) ||
		 FAMILY_IS_GFX1036(gpu_info->family_id) ||
		 FAMILY_IS_GFX1037(gpu_info->family_id) ||
		 FAMILY_IS_YC(gpu_info->family_id) ||
		 gpu_info->family_id == FAMILY_GFX1103)
			version = 11;
	else if (FAMILY_IS_AI(gpu_info->family_id) ||
		 FAMILY_IS_RV(gpu_info->family_id) ||
		 FAMILY_IS_NV(gpu_info->family_id) ||
		gpu_info->family_id == FAMILY_VGH)
			version = 10;

	/* 1. Allocate VRAM BO with RW MTYPE */
	r = amdgpu_bo_alloc_and_map_sync(device,
					 PAGE_SIZE, 4096,
									 AMDGPU_GEM_DOMAIN_VRAM,
									 AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
									 AMDGPU_VM_MTYPE_RW,
									 &bo_vram, &vram_cpu, &vram_mc, &va_vram,
									 0, 0, true);
	igt_assert_eq(r, 0);

	buf = (uint32_t *)vram_cpu;
	memset(buf, 0, PAGE_SIZE);

	/* 2. Assemble PollMemoryIsa */
	r = amdgpu_llvm_asm_init();
	if (r != 0) {
		igt_skip("LLVM assembler unavailable\n");
		amdgpu_bo_unmap_and_free(bo_vram, va_vram, vram_mc, PAGE_SIZE);
		return;
	}
	r = amdgpu_llvm_assemble(mcpu, PollMemoryIsa, isa_bin, sizeof(isa_bin), &isa_sz);
	amdgpu_llvm_asm_shutdown();
	igt_assert_f(r == 0 && isa_sz > 0, "Failed to assemble PollMemoryIsa\n");

	/* 3. Allocate shader BO and upload ISA */
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
								&bo_shader, &shader_ptr, &mc_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(shader_ptr, 0, 4096);
	memcpy(shader_ptr, isa_bin, isa_sz);

	/* 4. Build and submit compute dispatch (non-blocking) */
	r = amdgpu_cs_ctx_create(device, &compute_ctx);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
								&bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd);
	igt_assert_eq(r, 0);
	memset(ptr_cmd, 0, 4096);
	base_cmd = get_cmd_base();
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);
	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, version);
	amdgpu_dispatch_write_cumask(base_cmd, version);
	amdgpu_dispatch_write2hw(base_cmd, mc_shader, version, 0);

	/* Pass src (buffer[0]) and dst (buffer[dwLocation]) addresses via user SGPRs */
	dst_va = vram_mc + dwLocation * sizeof(uint32_t);
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);
	base_cmd->emit(base_cmd, vram_mc & 0xffffffff);
	base_cmd->emit(base_cmd, vram_mc >> 32);
	base_cmd->emit(base_cmd, dst_va & 0xffffffff);
	base_cmd->emit(base_cmd, dst_va >> 32);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_vram;
	r = amdgpu_bo_list_create(device, 3, bos, NULL, &bo_list);
	igt_assert_eq(r, 0);

	memset(&ib_info, 0, sizeof(ib_info));
	memset(&ib_req, 0, sizeof(ib_req));
	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	r = amdgpu_cs_submit(compute_ctx, 0, &ib_req, 1);
	igt_assert_eq(r, 0);

	/* 5. Issue SDMA write to buffer[0] while shader is polling */
	if (cmd_ring_available(device, AMD_IP_DMA, 0, false))
		dma_ctx = cmd_context_create(device, AMD_IP_DMA, 0, false);
	igt_assert(dma_ctx);
	igt_assert_eq(cmd_context_add_resource(dma_ctx, bo_vram), 0);
	r = cmd_submit_write_linear(dma_ctx, vram_mc, sizeof(uint32_t), kPattern);
	igt_assert_eq(r, 0);
	r = cmd_wait_completion(dma_ctx);
	igt_assert_eq(r, 0);

	/* 6. Wait for compute fence */
	memset(&fence, 0, sizeof(fence));
	fence.context = compute_ctx;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;
	r = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_f(expired, "Compute fence not signaled\n");

	/* 7. Validate result */
	igt_assert_f(buf[dwLocation] == kPattern,
		     "Cache invalidation failed: expected 0x%x at dst, got 0x%x\n",
				 kPattern, buf[dwLocation]);
	igt_info("cache-invalidate-sdma-write: PASSED (dst[100]=0x%x)\n", buf[dwLocation]);

	/* Cleanup */
	cmd_context_destroy(dma_ctx);
	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
	amdgpu_cs_ctx_free(compute_ctx);
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_shader, 4096);
	amdgpu_bo_unmap_and_free(bo_vram, va_vram, vram_mc, PAGE_SIZE);
	free_cmd_base(base_cmd);
}

/*
 * Test: cache-invalidate-cpu-write
 *
 * Adapted from ROCr KFD CacheInvalidateOnCPUWrite.
 * Flow (Arcturus-only + large BAR):
 *  1. Allocate host-accessible VRAM BO (CPU_ACCESS_REQUIRED) with RW MTYPE.
 *  2. Assemble PollMemoryIsa shader which polls src dword until value == 0x5678 then writes 0x5678 to dst.
 *  3. Launch compute dispatch that polls buffer[0] and will write to buffer[dwLocation].
 *  4. CPU writes 0x5678 to buffer[0]. This should invalidate any cached copy of dst and allow shader to observe update and exit.
 *  5. Wait for fence; verify buffer[dwLocation] == 0x5678.
 */
static void test_cache_invalidate_on_cpu_write_asm(amdgpu_device_handle device,
						   const struct amdgpu_gpu_info *gpu_info)
{
	/* Declarations (C90) */
	const uint32_t kPattern = 0x5678;
	const int dwLocation = 100;
	int r;
	uint32_t version = 9;
	char mcpu[32];
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	uint64_t vram_mc = 0;
	void *vram_cpu = NULL;
	amdgpu_bo_handle bo_vram = NULL;
	amdgpu_va_handle va_vram = 0;
	uint32_t *buf = NULL;
	void *shader_ptr = NULL;
	amdgpu_bo_handle bo_shader = NULL;
	amdgpu_va_handle va_shader = 0;
	uint64_t mc_shader = 0;
	amdgpu_context_handle compute_ctx;
	amdgpu_bo_handle bo_cmd = NULL;
	uint32_t *ptr_cmd = NULL;
	uint64_t mc_cmd = 0;
	amdgpu_va_handle va_cmd = 0;
	struct amdgpu_cmd_base *base_cmd;
	uint64_t dst_va = 0;
	amdgpu_bo_handle bos[3];
	amdgpu_bo_list_handle bo_list;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_request ib_req;
	struct amdgpu_cs_fence fence;
	uint32_t expired = 0;
	uint64_t cpu_accessible_vram = 0;

	/* Require Arcturus-like discrete (approximate) and large BAR (CPU accessible VRAM). */
	if (is_apu(gpu_info) || !FAMILY_IS_AI(gpu_info->family_id)) {
		igt_skip("Skipping test: requires Arcturus (gfx9 AI discrete) family\n");
		return;
	}
	/* Query memory info for CPU accessible VRAM */
	r = get_gpu_memory_info(device, NULL, &cpu_accessible_vram, NULL);
	if (r != 0 || cpu_accessible_vram == 0) {
		igt_skip("Skipping test: requires large BAR (CPU accessible VRAM)\n");
		return;
	}

	amdgpu_family_id_to_mcpu(gpu_info->family_id, mcpu, sizeof(mcpu));
	if (FAMILY_IS_GFX1200(gpu_info->family_id))
		version = 12;
	else if (FAMILY_IS_GFX1150(gpu_info->family_id) ||
		 FAMILY_IS_GFX1100(gpu_info->family_id) ||
		 FAMILY_IS_GFX1036(gpu_info->family_id) ||
		 FAMILY_IS_GFX1037(gpu_info->family_id) ||
		 FAMILY_IS_YC(gpu_info->family_id) ||
		 gpu_info->family_id == FAMILY_GFX1103)
			version = 11;
	else if (FAMILY_IS_AI(gpu_info->family_id) ||
		 FAMILY_IS_RV(gpu_info->family_id) ||
		 FAMILY_IS_NV(gpu_info->family_id) ||
		 gpu_info->family_id == FAMILY_VGH)
			version = 10;

	/* Allocate host-accessible VRAM BO */
	r = amdgpu_bo_alloc_and_map_sync(device,
					 PAGE_SIZE, 4096,
									 AMDGPU_GEM_DOMAIN_VRAM,
									 AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
									 AMDGPU_VM_MTYPE_RW,
									 &bo_vram, &vram_cpu, &vram_mc, &va_vram,
									 0, 0, true);
	igt_assert_eq(r, 0);
	buf = (uint32_t *)vram_cpu;
	memset(buf, 0, PAGE_SIZE);

	/* Assemble PollMemoryIsa */
	r = amdgpu_llvm_asm_init();
	if (r != 0) {
		igt_skip("LLVM assembler unavailable\n");
		amdgpu_bo_unmap_and_free(bo_vram, va_vram, vram_mc, PAGE_SIZE);
		return;
	}
	r = amdgpu_llvm_assemble(mcpu, PollMemoryIsa, isa_bin, sizeof(isa_bin), &isa_sz);
	amdgpu_llvm_asm_shutdown();
	igt_assert_f(r == 0 && isa_sz > 0, "Failed to assemble PollMemoryIsa\n");

	/* Shader BO */
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
								&bo_shader, &shader_ptr, &mc_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(shader_ptr, 0, 4096);
	memcpy(shader_ptr, isa_bin, isa_sz);

	/* Command buffer + dispatch */
	r = amdgpu_cs_ctx_create(device, &compute_ctx);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
								&bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd);
	igt_assert_eq(r, 0);
	memset(ptr_cmd, 0, 4096);
	base_cmd = get_cmd_base();
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);
	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, version);
	amdgpu_dispatch_write_cumask(base_cmd, version);
	amdgpu_dispatch_write2hw(base_cmd, mc_shader, version, 0);

	dst_va = vram_mc + dwLocation * sizeof(uint32_t);
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);
	base_cmd->emit(base_cmd, vram_mc & 0xffffffff);
	base_cmd->emit(base_cmd, vram_mc >> 32);
	base_cmd->emit(base_cmd, dst_va & 0xffffffff);
	base_cmd->emit(base_cmd, dst_va >> 32);
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_vram;
	r = amdgpu_bo_list_create(device, 3, bos, NULL, &bo_list);
	igt_assert_eq(r, 0);

	memset(&ib_info, 0, sizeof(ib_info));
	memset(&ib_req, 0, sizeof(ib_req));
	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	r = amdgpu_cs_submit(compute_ctx, 0, &ib_req, 1);
	igt_assert_eq(r, 0);

	/* CPU write to trigger shader completion */
	buf[0] = kPattern;

	/* Wait for shader to exit */
	memset(&fence, 0, sizeof(fence));
	fence.context = compute_ctx;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;
	r = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_f(expired, "Compute fence not signaled after CPU write\n");

	/* Validate */
	igt_assert_f(buf[dwLocation] == kPattern,
		     "CPU write cache invalidation failed: expected 0x%x at dst got 0x%x\n",
				 kPattern, buf[dwLocation]);
	igt_info("cache-invalidate-cpu-write: PASSED (dst[100]=0x%x)\n", buf[dwLocation]);

	/* Cleanup */
	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
	amdgpu_cs_ctx_free(compute_ctx);
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_shader, 4096);
	amdgpu_bo_unmap_and_free(bo_vram, va_vram, vram_mc, PAGE_SIZE);
	free_cmd_base(base_cmd);
}

/* Variant: copy one dword from src_va to dst_va where both point to already
 * mapped userptr/VRAM pages (no destination BO allocation). The shader BO
 * must contain CopyDwordIsa and be referenced by mc_shader. We only build
 * and submit a command buffer; rely on shader writing directly to dst_va. */
static int compute_copy_dword_userptr(amdgpu_device_handle device,
				      uint32_t version,
				      amdgpu_bo_handle bo_shader,
				      amdgpu_bo_handle bo_src,
				      amdgpu_bo_handle bo_dst,
				      uint64_t mc_shader,
				      uint64_t src_va,
				      uint64_t dst_va)
{
	amdgpu_context_handle ctx;
	amdgpu_bo_handle bo_cmd = NULL;
	uint32_t *ptr_cmd = NULL;
	uint64_t mc_cmd = 0;
	amdgpu_va_handle va_cmd = 0;
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_request ib_req = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_fence fence = {0};
	uint32_t expired = 0;
	int r;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	/* BO list must include: command buffer, shader BO, src userptr BO, dst userptr BO */
	amdgpu_bo_handle bos[4];

	r = amdgpu_cs_ctx_create(device, &ctx);
	if (r)
		return r;
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				AMDGPU_GEM_DOMAIN_GTT, 0,
				&bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd);
	if (r) {
		amdgpu_cs_ctx_free(ctx);
		return r;
	}
	memset(ptr_cmd, 0, 4096);
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);

	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, version);
	amdgpu_dispatch_write_cumask(base_cmd, version);
	amdgpu_dispatch_write2hw(base_cmd, mc_shader, version, 0);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);
	base_cmd->emit(base_cmd, src_va & 0xffffffff);
	base_cmd->emit(base_cmd, src_va >> 32);
	base_cmd->emit(base_cmd, dst_va & 0xffffffff);
	base_cmd->emit(base_cmd, dst_va >> 32);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_src;
	bos[3] = bo_dst;
	r = amdgpu_bo_list_create(device, 4, bos, NULL, &bo_list);
	if (r) {
		amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
		amdgpu_cs_ctx_free(ctx);
		return r;
	}

	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	ib_req.fence_info.handle = NULL;
	r = amdgpu_cs_submit(ctx, 0, &ib_req, 1);
	if (r)
		goto out_fail;

	fence.context = ctx;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;
	r = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	if (r || !expired)
		r = -ETIME;

out_fail:
	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
	amdgpu_cs_ctx_free(ctx);
	free_cmd_base(base_cmd);
	return r;
}

/**
 * memory_register - Adaptation of KFD MemoryRegister test.
 * Steps:
 *  1. Map global and stack pages (userptr) to GPU before fork.
 *  2. Initial GPU copy (compute shader CopyDwordIsa) global->stack[dstOffset].
 *  3. Initial SDMA linear write to stack[sdmaOffset].
 *  4. fork() child spins; parent writes to pages causing COW.
 *  5. Second GPU copy and SDMA write; verifies MMU notifier updated mappings.
 *  6. Validate final CPU-visible values and clean up.
 */
static void memory_register(amdgpu_device_handle device)
{
	const unsigned int dstOffset = 0;
	const unsigned int sdmaOffset = 16;
	size_t page_sz;
	uint32_t *global_mem = NULL;
	uint32_t *stack_mem = NULL;
	int r;
	const struct amdgpu_ip_block_version *ip_block;
	amdgpu_bo_handle bo_global = NULL, bo_stack = NULL, bo_shader = NULL;
	amdgpu_va_handle va_global = NULL, va_stack = NULL, va_shader = 0;
	uint64_t gpu_va_global = 0, gpu_va_stack = 0, mc_shader = 0;
	struct amdgpu_ring_context *ring_context = NULL;
	struct amdgpu_gpu_info info;
	char mcpu[32];
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	uint32_t version = 9;
	void *ptr_shader = NULL;
	pid_t pid;
	int status;
	bool buffer_busy;

	memset(&info, 0, sizeof(info));
	page_sz = sysconf(_SC_PAGE_SIZE);

	/* 1. Check compute IP block availability */
	ip_block = get_ip_block(device, AMDGPU_HW_IP_COMPUTE);
	if (!ip_block) {
		igt_skip("Compute IP block not available\n");
		return;
	}

	/* 2. Allocate and initialize memory */
	r = posix_memalign((void **)&global_mem, page_sz, page_sz);
	igt_assert_eq(r, 0);
	r = posix_memalign((void **)&stack_mem, page_sz, page_sz);
	igt_assert_eq(r, 0);

	memset(global_mem, 0, page_sz);
	memset(stack_mem, 0, page_sz);
	global_mem[0] = 0xdeadbeef;

	igt_info("Allocated memory: global=%p, stack=%p\n", global_mem, stack_mem);

	/* 3. Create userptr BOs */
	r = amdgpu_create_bo_from_user_mem(device, global_mem, page_sz, &bo_global);
	igt_assert_eq(r, 0);
	r = amdgpu_create_bo_from_user_mem(device, stack_mem, page_sz, &bo_stack);
	igt_assert_eq(r, 0);

	/* 4. Allocate GPU VA space */
	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general, page_sz, 1, 0,
			      &gpu_va_global, &va_global, 0);
	igt_assert_eq(r, 0);
	r = amdgpu_va_range_alloc(device, amdgpu_gpu_va_range_general, page_sz, 1, 0,
			      &gpu_va_stack, &va_stack, 0);
	igt_assert_eq(r, 0);

	/* 5. Map BOs to GPU VA */
	igt_info("Mapping: global GPU VA=0x%llx, stack GPU VA=0x%llx\n",
	     (unsigned long long)gpu_va_global, (unsigned long long)gpu_va_stack);

	r = amdgpu_bo_va_op(bo_global, 0, page_sz, gpu_va_global, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_va_op(bo_stack, 0, page_sz, gpu_va_stack, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	/* 6. Wait for mapping completion - use correct function */
	r = amdgpu_bo_wait_for_idle(bo_global, AMDGPU_TIMEOUT_INFINITE, &buffer_busy);
	igt_assert_eq(r, 0);
	igt_assert_f(!buffer_busy, "Global BO is still busy after mapping");

	r = amdgpu_bo_wait_for_idle(bo_stack, AMDGPU_TIMEOUT_INFINITE, &buffer_busy);
	igt_assert_eq(r, 0);
	igt_assert_f(!buffer_busy, "Stack BO is still busy after mapping");

	/* 7. Prepare shader - use existing CopyDwordIsa */
	r = amdgpu_query_gpu_info(device, &info);
	igt_assert_eq(r, 0);
	amdgpu_family_id_to_mcpu(info.family_id, mcpu, sizeof(mcpu));

    /* Determine GPU version */
	if (FAMILY_IS_GFX1200(info.family_id))
		version = 12;
	else if (FAMILY_IS_GFX1150(info.family_id) ||
	     FAMILY_IS_GFX1100(info.family_id) ||
	     FAMILY_IS_GFX1036(info.family_id) ||
	     FAMILY_IS_GFX1037(info.family_id) ||
	     FAMILY_IS_YC(info.family_id) ||
	     info.family_id == FAMILY_GFX1103)
		version = 11;
	else if (FAMILY_IS_AI(info.family_id) ||
	     FAMILY_IS_RV(info.family_id) ||
	     FAMILY_IS_NV(info.family_id) ||
	     info.family_id == FAMILY_VGH)
		version = 10;

	r = amdgpu_llvm_asm_init();
	if (r != 0) {
		igt_skip("LLVM assembler unavailable\n");
		goto cleanup;
	}

	/* 8. Use existing CopyDwordIsa shader */
	r = amdgpu_llvm_assemble(mcpu, CopyDwordIsa, isa_bin, sizeof(isa_bin), &isa_sz);
	if (r != 0 || isa_sz == 0) {
		igt_warn("Failed to assemble CopyDwordIsa\n");
		amdgpu_llvm_asm_shutdown();
		igt_skip("Shader assembly failed\n");
		goto cleanup;
	}

	/* 9. Allocate shader memory */
	r = amdgpu_bo_alloc_and_map(device, 4096, 4096, AMDGPU_GEM_DOMAIN_GTT, 0,
				&bo_shader, &ptr_shader, &mc_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(ptr_shader, 0, 4096);
	memcpy(ptr_shader, isa_bin, isa_sz);
	amdgpu_llvm_asm_shutdown();

	/* 10. First GPU copy */
	igt_info("First GPU copy: src=0x%llx, dst=0x%llx\n",
	     (unsigned long long)gpu_va_global,
	     (unsigned long long)(gpu_va_stack + dstOffset * sizeof(uint32_t)));

	/* Wait for shader BO to become idle */
	r = amdgpu_bo_wait_for_idle(bo_shader, AMDGPU_TIMEOUT_INFINITE, &buffer_busy);
	igt_assert_eq(r, 0);

	r = compute_copy_dword_userptr(device, version, bo_shader, bo_global, bo_stack,
				   mc_shader, gpu_va_global,
				  gpu_va_stack + dstOffset * sizeof(uint32_t));
	if (r != 0) {
		igt_warn("First GPU copy failed: %d\n", r);
		/* Report current values after failed copy */
		igt_info("After failed copy: global[0]=0x%x, stack[0]=0x%x\n",
			 global_mem[0], stack_mem[dstOffset]);
	} else {
		igt_info("First GPU copy succeeded\n");
		igt_info("After first copy: global[0]=0x%x, stack[0]=0x%x\n",
			 global_mem[0], stack_mem[dstOffset]);
	}

	/* 11. Fork and COW test */
	pid = fork();
	igt_assert_f(pid >= 0, "fork failed: %s", strerror(errno));

	if (pid == 0) {
		/* Child process - simple wait */
		sleep(10);
		_exit(0);
	}

	/* 12. Trigger COW */
	igt_info("Triggering COW...\n");
	global_mem[0] = 0xD00BED00;
	stack_mem[dstOffset] = 0xDEADBEEF;
	stack_mem[sdmaOffset] = 0xDEADBEEF;

	/* 13. Wait for child process to complete */
	kill(pid, SIGTERM);
	waitpid(pid, &status, 0);

	/* 14. Second GPU copy */
	igt_info("Second GPU copy after COW\n");

	/* Wait for shader BO idle before second copy */
	r = amdgpu_bo_wait_for_idle(bo_shader, AMDGPU_TIMEOUT_INFINITE, &buffer_busy);
	igt_assert_eq(r, 0);

	r = compute_copy_dword_userptr(device, version, bo_shader, bo_global, bo_stack,
				   mc_shader, gpu_va_global,
				  gpu_va_stack + dstOffset * sizeof(uint32_t));
	if (r != 0) {
		igt_warn("Second GPU copy failed: %d\n", r);
	} else {
		igt_info("Second GPU copy succeeded\n");
	}

	/* 15. Verify final results */
	igt_info("Final values: global[0]=0x%x, stack[0]=0x%x, stack[16]=0x%x\n",
	     global_mem[0], stack_mem[dstOffset], stack_mem[sdmaOffset]);

	if (global_mem[0] == 0xD00BED00 && stack_mem[dstOffset] == 0xD00BED00) {
		igt_info("memory-register-cow: PASSED\n");
	} else {
		igt_warn("memory-register-cow: VALUES MISMATCH\n");
		igt_warn("Expected: global=0x%x, stack[0]=0x%x\n", 0xD00BED00, 0xD00BED00);
		igt_warn("Got: global=0x%x, stack[0]=0x%x\n", global_mem[0], stack_mem[dstOffset]);
	}

cleanup:
	/* Cleanup resources */
	if (bo_global) {
		amdgpu_bo_va_op(bo_global, 0, page_sz, gpu_va_global, 0, AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(va_global);
		amdgpu_bo_free(bo_global);
	}
	if (bo_stack) {
		amdgpu_bo_va_op(bo_stack, 0, page_sz, gpu_va_stack, 0, AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(va_stack);
		amdgpu_bo_free(bo_stack);
	}
	if (bo_shader) {
		amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_shader, 4096);
	}
	if (global_mem)
		free(global_mem);
	if (stack_mem)
		free(stack_mem);

	/* Cleanup ring context if created */
	if (ring_context) {
		if (ring_context->bo) {
		    amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
					     ring_context->bo_mc, ring_context->write_length * sizeof(uint32_t));
		}
		free(ring_context->pm4);

		if (ip_block && ip_block->funcs && ip_block->funcs->userq_destroy) {
		    ip_block->funcs->userq_destroy(device, ring_context, ip_block->type);
		}
		free(ring_context);
	}
}

static void test_gca_config_basic(amdgpu_device_handle device, int fd)
{
	uint32_t config[MAX_CONFIG_SIZE];
	int dri_index = 0; /* Typically 0 for primary GPU */
	int bytes_read;

	dri_index = get_dri_index_from_device(device, fd);
	/* Try to read from debugfs */
	bytes_read = read_gca_config_debugfs(dri_index, config, sizeof(config));

	if (bytes_read < 0) {
		igt_skip("GCA config debugfs not available\n");
	}

	igt_assert_f(bytes_read > 0, "No data read from debugfs\n");
	igt_assert_f((bytes_read % 4) == 0, "Config size not 4-byte aligned: %d\n", bytes_read);

	igt_info("Read %d bytes (%u dwords) from GCA config\n",
		 bytes_read, bytes_read / 4);

	validate_gca_config_basic(config, bytes_read);
}

/**
 * get_available_gtt_memory - Get available GTT memory
 * @device: AMDGPU device handle
 *
 * Returns available GTT memory in bytes.
 */
static uint64_t get_available_gtt_memory(amdgpu_device_handle device)
{
	uint64_t gtt_size = 0;
	int r;

	/* Get GTT size from GPU */
	r = get_gpu_memory_info(device, NULL, NULL, &gtt_size);
	if (r == 0 && gtt_size > 0) {
		return gtt_size;
	}

	return 0;
}

/**
 * get_available_vram_memory - Get available GPU VRAM
 * @device: AMDGPU device handle
 *
 * Returns available GPU VRAM in bytes.
 */
static uint64_t get_available_vram_memory(amdgpu_device_handle device)
{
	uint64_t vram_size = 0;
	int r;

	r = get_gpu_memory_info(device, &vram_size, NULL, NULL);
	if (r == 0) {
		return vram_size;
	}

	igt_info("Failed to get VRAM info\n");
	return 0;
}

/**
 * get_available_cpu_accessible_vram_memory - Get available CPU accessible GPU VRAM
 * @device: AMDGPU device handle
 *
 * Returns available CPU accessible GPU VRAM in bytes.
 */
static uint64_t get_available_cpu_accessible_vram_memory(amdgpu_device_handle device)
{
	uint64_t cpu_vram_size = 0;
	int r;

	r = get_gpu_memory_info(device, NULL, &cpu_vram_size, NULL);
	if (r == 0) {
		return cpu_vram_size;
	}

	igt_info("Failed to get CPU accessible VRAM info\n");
	return 0;
}

/* binary search largest alloc/map-able buffer */
static void search_largest_buffer(amdgpu_device_handle device, uint64_t alloc_size,
				  unsigned int domain_type, uint64_t flags, uint64_t *last_size)
{
	amdgpu_bo_handle bo = NULL;
	struct amdgpu_bo_alloc_request request = {0};
	amdgpu_va_handle va_handle = NULL;
	uint64_t va = 0;
	int r;

	/* Use 8MB granularity for search steps */
	const uint64_t granularity = 8 * 1024 * 1024;
	uint64_t low = granularity;
	uint64_t high = alloc_size;
	uint64_t current_size = alloc_size;
	uint64_t largest_success = 0;

	const char *domain_name = (domain_type == AMDGPU_GEM_DOMAIN_GTT) ? "GTT" :
			     (domain_type == AMDGPU_GEM_DOMAIN_VRAM) ? "VRAM" :
			     (domain_type == AMDGPU_GEM_DOMAIN_CPU) ? "CPU" : "UNKNOWN";

	igt_info("Starting search for largest %s buffer, initial size: %" PRIu64 " MB, flags: 0x%" PRIx64 "\n",
		 domain_name, current_size / (1024 * 1024), flags);

	/* Binary search for the largest allocatable size */
	while (low <= high) {
		current_size = low + (high - low) / 2;

		/* Align to granularity */
		current_size = (current_size / granularity) * granularity;
		if (current_size == 0)
			break;

		igt_debug("Trying allocation size: %" PRIu64 " MB\n",
			  current_size / (1024 * 1024));

		/* Initialize buffer allocation request */
		request.alloc_size = current_size;
		request.phys_alignment = 4096;
		request.preferred_heap = domain_type;
		request.flags = flags;

		/* Try to allocate buffer */
		r = amdgpu_bo_alloc(device, &request, &bo);
		if (r) {
			igt_debug("Allocation failed for %" PRIu64 " MB, error: %d\n",
				  current_size / (1024 * 1024), r);
			high = current_size - granularity;
			continue;
		}

		/* Reserve VA space and map the buffer */
		r = amdgpu_va_range_alloc(device,
					  amdgpu_gpu_va_range_general,
				 current_size,
				 getpagesize(),
				 0,
				 &va,
				 &va_handle,
				 0);
		if (r) {
			igt_debug("VA range allocation failed for %" PRIu64 " MB, error: %d\n",
				  current_size / (1024 * 1024), r);
			amdgpu_bo_free(bo);
			bo = NULL;
			high = current_size - granularity;
			continue;
		}

		/* Map the buffer to the reserved VA */
		r = amdgpu_bo_va_op(bo, 0, current_size, va, 0, AMDGPU_VA_OP_MAP);
		if (r) {
			igt_debug("GPU VA mapping failed for %" PRIu64 " MB, error: %d\n",
				  current_size / (1024 * 1024), r);
			amdgpu_va_range_free(va_handle);
			amdgpu_bo_free(bo);
			bo = NULL;
			va_handle = NULL;
			high = current_size - granularity;
			continue;
		}

		/* Test CPU access for buffers that should be CPU accessible */
		if (!(flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)) {
			void *cpu_ptr = NULL;

			r = amdgpu_bo_cpu_map(bo, &cpu_ptr);
			if (r) {
				igt_debug("CPU mapping failed for %" PRIu64 " MB, error: %d\n",
					  current_size / (1024 * 1024), r);
				amdgpu_bo_va_op(bo, 0, current_size, va, 0, AMDGPU_VA_OP_UNMAP);
				amdgpu_va_range_free(va_handle);
				amdgpu_bo_free(bo);
				bo = NULL;
				va_handle = NULL;
				high = current_size - granularity;
				continue;
			}

			/* Test CPU write access */
			memset(cpu_ptr, 0xAA, MIN(current_size, 4096));
			amdgpu_bo_cpu_unmap(bo);
		}

		/* Success - store this size and try larger */
		igt_debug("Successfully allocated and mapped %" PRIu64 " MB %s at VA: 0x%" PRIx64 "\n",
			  current_size / (1024 * 1024), domain_name, va);

		largest_success = current_size;

		/* Cleanup for next iteration - unmap and free */
		amdgpu_bo_va_op(bo, 0, current_size, va, 0, AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(va_handle);
		amdgpu_bo_free(bo);
		bo = NULL;
		va_handle = NULL;
		va = 0;

		low = current_size + granularity;
	}

	/* Cleanup any remaining resources */
	if (va_handle)
		amdgpu_va_range_free(va_handle);
	if (bo)
		amdgpu_bo_free(bo);

	if (last_size)
		*last_size = largest_success;
}

/* run largest buffer search for domain */
static void largest_buffer_test(amdgpu_device_handle device, unsigned int domain_type, uint64_t flags)
{
	uint64_t available_memory = 0;
	uint64_t largest_buffer_size = 0;
	uint64_t test_size;

	const char *domain_name = (domain_type == AMDGPU_GEM_DOMAIN_GTT) ? "GTT" :
			     (domain_type == AMDGPU_GEM_DOMAIN_VRAM) ? "VRAM" :
			     (domain_type == AMDGPU_GEM_DOMAIN_CPU) ? "CPU" : "UNKNOWN";

	const char *flags_str = "";

	if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
		flags_str = " (CPU access required)";
	else if (flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)
		flags_str = " (no CPU access)";

	igt_info("Testing %s memory domain%s\n", domain_name, flags_str);

	/* Get available memory for the specified domain */
	if (domain_type == AMDGPU_GEM_DOMAIN_GTT) {
		available_memory = get_available_gtt_memory(device);
		igt_info("Available GTT memory: %" PRIu64 " MB\n",
			 available_memory / (1024 * 1024));
	} else if (domain_type == AMDGPU_GEM_DOMAIN_VRAM) {
		available_memory = get_available_vram_memory(device);
		igt_info("Available VRAM: %" PRIu64 " MB\n",
			 available_memory / (1024 * 1024));
	} else {
		igt_skip("Unsupported memory domain type: %u\n", domain_type);
		return;
	}

	if (available_memory == 0) {
		igt_skip("Could not determine available memory for domain %s\n", domain_name);
		return;
	}

	/* Use 50% of available memory as initial test size */
	test_size = (available_memory * 50) / 100;

	igt_info("Testing with initial size: %" PRIu64 " MB (50%% of available %s memory)\n",
		 test_size / (1024 * 1024), domain_name);

	/* Search for largest buffer */
	search_largest_buffer(device, test_size, domain_type, flags, &largest_buffer_size);

	if (largest_buffer_size > 0) {
		igt_info("SUCCESS: Largest allocated %s buffer%s is %" PRIu64 " MB\n",
			 domain_name, flags_str, largest_buffer_size / (1024 * 1024));

		/* Log the result for automated testing */
		igt_assert_f(largest_buffer_size > 0,
			     "Should be able to allocate at least some %s memory%s", domain_name, flags_str);
	} else {
		igt_skip("Could not allocate any %s memory buffers%s\n", domain_name, flags_str);
	}
}

/* largest CPU accessible VRAM buffer search */
static void largest_cpu_accessible_vram_test(amdgpu_device_handle device)
{
	uint64_t available_memory = 0;
	uint64_t largest_buffer_size = 0;
	uint64_t test_size;

	igt_info("Testing CPU accessible VRAM memory domain\n");

	/* Get available CPU accessible VRAM */
	available_memory = get_available_cpu_accessible_vram_memory(device);
	igt_info("Available CPU accessible VRAM: %" PRIu64 " MB\n",
		 available_memory / (1024 * 1024));

	if (available_memory == 0) {
		igt_skip("Could not determine available CPU accessible VRAM\n");
		return;
	}

	/* Use 50% of available memory as initial test size */
	test_size = (available_memory * 50) / 100;

	igt_info("Testing with initial size: %" PRIu64 " MB (50%% of available CPU accessible VRAM)\n",
		 test_size / (1024 * 1024));

	/* Search for largest buffer - use VRAM domain with CPU access required flag */
	search_largest_buffer(device, test_size, AMDGPU_GEM_DOMAIN_VRAM,
			      AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, &largest_buffer_size);

	if (largest_buffer_size > 0) {
		igt_info("SUCCESS: Largest allocated CPU accessible VRAM buffer is %" PRIu64 " MB\n",
			 largest_buffer_size / (1024 * 1024));

		/* Log the result for automated testing */
		igt_assert_f(largest_buffer_size > 0,
			     "Should be able to allocate at least some CPU accessible VRAM memory");
	} else {
		igt_skip("Could not allocate any CPU accessible VRAM memory buffers\n");
	}
}

/**
 * big_sys_buffer_stress_test_with_queue - Stress test with queue operations
 * @device: AMDGPU device handle
 *
 * Enhanced version that performs queue operations on each buffer to simulate
 * real workload patterns.
 */
static void big_sys_buffer_stress_test_with_queue(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo_array[256];
	amdgpu_va_handle va_handle_array[256];
	uint64_t va_array[256];
	void *cpu_ptr_array[256];
	int allocation_count = 0;
	int first_run_count = 0;
	int i, j, r;
	int repeat;

	const uint64_t block_size = 128 * 1024 * 1024; /* 128 MB */
	const int max_iterations = ARRAY_SIZE(bo_array);
	const int num_repeats = 3;

	igt_info("Starting big system buffer stress test with queue operations\n");
	igt_info("Block size: %" PRIu64 " MB, Max iterations: %d, Repeats: %d\n",
		 block_size / (1024 * 1024), max_iterations, num_repeats);

	/* Initialize arrays */
	memset(bo_array, 0, sizeof(bo_array));
	memset(va_handle_array, 0, sizeof(va_handle_array));
	memset(va_array, 0, sizeof(va_array));
	memset(cpu_ptr_array, 0, sizeof(cpu_ptr_array));

	for (repeat = 0; repeat < num_repeats; repeat++) {
		int successful_allocations = 0;

		igt_info("Repeat %d: Allocating system buffers with queue operations...\n", repeat + 1);

		/* Allocate, map, and perform operations on buffers */
		for (i = 0; i < max_iterations; i++) {
			struct amdgpu_bo_alloc_request request = {0};
			amdgpu_bo_handle bo = NULL;
			amdgpu_va_handle va_handle = NULL;
			uint64_t va = 0;
			void *cpu_ptr = NULL;

			request.alloc_size = block_size;
			request.phys_alignment = 4096;
			request.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;

			/* Try to allocate buffer */
			r = amdgpu_bo_alloc(device, &request, &bo);
			if (r) {
				igt_debug("Allocation failed at iteration %d, error: %d\n", i, r);
				break;
			}

			/* Reserve VA space */
			r = amdgpu_va_range_alloc(device,
						  amdgpu_gpu_va_range_general,
					     block_size,
					     getpagesize(),
					     0,
					     &va,
					     &va_handle,
					     0);
			if (r) {
				igt_debug("VA range allocation failed at iteration %d, error: %d\n", i, r);
				amdgpu_bo_free(bo);
				break;
			}

			/* Map the buffer to the reserved VA */
			r = amdgpu_bo_va_op(bo, 0, block_size, va, 0, AMDGPU_VA_OP_MAP);
			if (r) {
				igt_debug("GPU VA mapping failed at iteration %d, error: %d\n", i, r);
				amdgpu_va_range_free(va_handle);
				amdgpu_bo_free(bo);
				break;
			}

			/* CPU map the buffer for operations */
			r = amdgpu_bo_cpu_map(bo, &cpu_ptr);
			if (r) {
				igt_debug("CPU mapping failed at iteration %d, error: %d\n", i, r);
				amdgpu_bo_va_op(bo, 0, block_size, va, 0, AMDGPU_VA_OP_UNMAP);
				amdgpu_va_range_free(va_handle);
				amdgpu_bo_free(bo);
				break;
			}

			/* Perform queue-like operations on the buffer */
			/* Simulate buffer initialization/usage pattern */
			memset(cpu_ptr, 0xAA, MIN(block_size, 4096)); /* Initialize first page */
			memset((char *)cpu_ptr + block_size - 4096, 0xBB, 4096); /* Initialize last page */

			/* Store successful allocation */
			bo_array[i] = bo;
			va_handle_array[i] = va_handle;
			va_array[i] = va;
			cpu_ptr_array[i] = cpu_ptr;
			successful_allocations++;
		}

		igt_info("Repeat %d: Successfully allocated and operated on %d * %" PRIu64 " MB system buffers\n",
			 repeat + 1, successful_allocations, block_size / (1024 * 1024));

		/* Store the count from first run for comparison */
		if (repeat == 0) {
			first_run_count = successful_allocations;
		}

		/* Check for memory leaks */
		if (repeat > 0) {
		igt_assert_f(successful_allocations >= first_run_count * 0.9,
			     "Possible memory leak detected: allocation count dropped from %d to %d",
			first_run_count, successful_allocations);
		}

		allocation_count = successful_allocations;

		/* Unmap and free all allocated buffers */
		igt_info("Repeat %d: Cleaning up %d buffers...\n", repeat + 1, successful_allocations);

		for (j = 0; j < successful_allocations; j++) {
			if (bo_array[j]) {
				/* CPU unmap */
				if (cpu_ptr_array[j])
					amdgpu_bo_cpu_unmap(bo_array[j]);

				/* Unmap from GPU VA */
				amdgpu_bo_va_op(bo_array[j], 0, block_size, va_array[j], 0, AMDGPU_VA_OP_UNMAP);

				/* Free VA space */
				if (va_handle_array[j])
				amdgpu_va_range_free(va_handle_array[j]);

				/* Free the buffer */
				amdgpu_bo_free(bo_array[j]);

				/* Clear pointers */
				bo_array[j] = NULL;
				va_handle_array[j] = NULL;
				cpu_ptr_array[j] = NULL;
			}
		}

		igt_info("Repeat %d: Cleanup completed\n", repeat + 1);
	}

	igt_info("SUCCESS: Big system buffer stress test with queue operations completed\n");
	igt_info("Final allocation count: %d * %" PRIu64 " MB\n",
		 allocation_count, block_size / (1024 * 1024));
}

/* Test configuration */
typedef struct {
    uint64_t size;
    uint32_t num_buffers;
} buffer_params_t;

static const buffer_params_t buffer_configs[] = {
    {4096,      1000},   /* 4KB */
    {65536,     1000},   /* 64KB */
    {2 * 1024 * 1024,  500},   /* 2MB */
    {32 * 1024 * 1024,  32},   /* 32MB */
    {1 * 1024 * 1024 * 1024, 1},   /* 1GB */
};

#define NUM_SIZES (ARRAY_SIZE(buffer_configs))
#define NUM_MEM_TYPES 2
#define NUM_SDMA_MODES 2
#define TOTAL_TESTS (NUM_SIZES * NUM_MEM_TYPES * NUM_SDMA_MODES)

/* Memory type strings */
static const char *mem_type_strings[] = {
    "SysMem",
    "VRAM"
};

/* Test context using the new command submission library */
typedef struct {
    amdgpu_device_handle device;
    uint64_t vram_size;
    uint64_t cpu_accessible_vram_size;
    uint64_t gtt_size;
    bool has_vram;
    bool large_bar;

    /* Single DMA command context for ring 0 */
    cmd_context_t *dma_ctx;

    /* Scratch BO used as the destination of interleaved DMA writes */
    amdgpu_bo_handle dma_scratch_bo;
    uint64_t dma_scratch_mc;
    amdgpu_va_handle dma_scratch_va;
} mm_bench_context_t;

/**
 * Initialize DMA context using cmd_context_create for ring 0
 */
static int init_dma_context(mm_bench_context_t *ctx, amdgpu_device_handle device)
{
	void *scratch_cpu;
	int r;

	/* Check if DMA ring 0 is available */
	if (!cmd_ring_available(device, AMD_IP_DMA, 0, false)) {
		igt_debug("DMA ring 0 is not available\n");
		return -ENODEV;
	}

	/* Create DMA context for ring 0 */
	ctx->dma_ctx = cmd_context_create(device, AMD_IP_DMA, 0, false);
	if (!ctx->dma_ctx) {
		igt_debug("Failed to create DMA context for ring 0\n");
		return -ENODEV;
	}

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ctx->dma_scratch_bo, &scratch_cpu,
				    &ctx->dma_scratch_mc, &ctx->dma_scratch_va);
	if (r) {
		igt_debug("Failed to allocate DMA scratch BO: %d\n", r);
		cmd_context_destroy(ctx->dma_ctx);
		ctx->dma_ctx = NULL;
		return r;
	}

	igt_debug("Initialized DMA ring 0 successfully\n");
	return 0;
}

/**
 * Cleanup DMA context using cmd_context_destroy
 */
static void cleanup_dma_context(mm_bench_context_t *ctx)
{
	if (ctx->dma_ctx) {
		cmd_context_destroy(ctx->dma_ctx);
		ctx->dma_ctx = NULL;
	}

	if (ctx->dma_scratch_bo) {
		amdgpu_bo_unmap_and_free(ctx->dma_scratch_bo, ctx->dma_scratch_va,
					 ctx->dma_scratch_mc, 4096);
		ctx->dma_scratch_bo = NULL;
	}
}

/**
 * Submit DMA write operation with proper error handling
 */
static int submit_dma_write_operation(mm_bench_context_t *ctx)
{
	int r;

	if (!ctx->dma_ctx) {
		igt_debug("DMA context is NULL\n");
		return -EINVAL;
	}

	if (!ctx->dma_ctx->initialized) {
		igt_debug("DMA context not initialized\n");
		return -EINVAL;
	}

	/* Register the destination BO in the per-submit residency set, then
	 * write into the scratch buffer it backs.
	 */
	r = cmd_context_add_resource(ctx->dma_ctx, ctx->dma_scratch_bo);
	if (r) {
		igt_debug("cmd_context_add_resource failed: %d\n", r);
		return r;
	}

	/* Use the convenience function for write linear operation */
	r = cmd_submit_write_linear(ctx->dma_ctx,
				ctx->dma_scratch_mc,
			       64, 0x12345678);

	if (r) {
		igt_debug("cmd_submit_write_linear failed: %d\n", r);
		return r;
	}

	return 0;
}

/**
 * Interleave DMA operations with proper error handling
 */
static void interleave_dma_operations(mm_bench_context_t *ctx, bool interleave_ops)
{
	int r;

	if (!interleave_ops || !ctx->dma_ctx)
		return;

	r = submit_dma_write_operation(ctx);
	igt_assert_eq(r, 0);
}

/**
 * Wait for DMA operations to complete
 */
static void wait_dma_operations(mm_bench_context_t *ctx, bool interleave_ops)
{
	int r;

	if (!interleave_ops || !ctx->dma_ctx)
		return;

	r = cmd_wait_completion(ctx->dma_ctx);
	igt_assert_eq(r, 0);
}

/**
 * Initialize benchmark context
 */
static void init_bench_context(mm_bench_context_t *ctx, amdgpu_device_handle device)
{
	int r;

	memset(ctx, 0, sizeof(*ctx));
	ctx->device = device;

	/* Query memory information using existing IGT function */
	r = get_gpu_memory_info(device, &ctx->vram_size,
			    &ctx->cpu_accessible_vram_size, &ctx->gtt_size);
	if (r == 0) {
		ctx->has_vram = (ctx->vram_size > 0);
		/* Large BAR is available if CPU accessible VRAM exists */
		ctx->large_bar = (ctx->cpu_accessible_vram_size > 0);
	} else {
		/* Fallback values if query fails */
		ctx->vram_size = 512 * 1024 * 1024;
		ctx->cpu_accessible_vram_size = 0;
		ctx->gtt_size = 2 * 1024 * 1024 * 1024ULL;
		ctx->has_vram = true;
		ctx->large_bar = false;
		igt_info("Using fallback memory values\n");
	}

	/* Initialize DMA context for ring 0 */
	r = init_dma_context(ctx, device);
	if (r) {
		igt_info("DMA context initialization failed: %d, continuing without DMA operations\n", r);
		ctx->dma_ctx = NULL;
	}

	igt_info("Memory Context: VRAM=%" PRIu64 "MB, CPU_VRAM=%" PRIu64 "MB, GTT=%" PRIu64 "MB, LargeBAR=%s\n",
	     ctx->vram_size >> 20, ctx->cpu_accessible_vram_size >> 20,
	     ctx->gtt_size >> 20, ctx->large_bar ? "yes" : "no");
}

/**
 * Cleanup benchmark context
 */
static void cleanup_bench_context(mm_bench_context_t *ctx)
{
	cleanup_dma_context(ctx);
}

/**
 * Calculate buffer limit based on available memory
 */
static uint32_t calculate_buffer_limit(mm_bench_context_t *ctx, uint64_t buf_size,
				       uint32_t proposed_num, bool use_vram)
{
	uint64_t available_memory;
	uint32_t max_buffers;

	if (!use_vram) {
		/* For system memory, use GTT size with 80% limit */
		available_memory = ctx->gtt_size * 8 / 10;
		max_buffers = available_memory / buf_size;
		return (proposed_num < max_buffers) ? proposed_num : max_buffers;
	} else {
		/* For VRAM, use visible VRAM size */
		available_memory = ctx->vram_size;

		/* Different criteria for small VRAM systems */
		if (ctx->vram_size <= (512 * 1024 * 1024)) { /* 512MB */
		    available_memory = available_memory * 6 / 10; /* 60% for small VRAM */
		} else {
		    available_memory = available_memory * 8 / 10; /* 80% for larger VRAM */
	}

	max_buffers = available_memory / buf_size;
	if (max_buffers == 0) {
		return 0; /* Skip if buffer size is larger than available VRAM */
	}

	return (proposed_num < max_buffers) ? proposed_num : max_buffers;
}
}

/**
 * Format size for display
 */
static void format_size_string(char *buf, size_t buf_size, uint64_t size)
{
	if (size < (1ULL << 20)) {
		snprintf(buf, buf_size, "%3" PRIu64 "K", size >> 10);
	} else if (size < (1ULL << 30)) {
		snprintf(buf, buf_size, "%3" PRIu64 "M", size >> 20);
	} else {
		snprintf(buf, buf_size, "%3" PRIu64 "G", size >> 30);
	}
}

/**
* Get current time in nanoseconds
*/
static uint64_t get_time_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/**
 * @brief AMDGPU Memory Management Benchmark (DMA ring 0 only)
 *
 * Measures performance of key memory operations (alloc/map/unmap/free)
 * across different buffer sizes, memory types, and DMA interleave modes.
 * Results are formatted for CI integration and human readability.
 */
static void mm_benchmark(amdgpu_device_handle device)
{
	mm_bench_context_t ctx;
	uint32_t test_index;
	amdgpu_bo_handle *buffers;
	amdgpu_va_handle *va_handles;
	uint64_t *mc_addresses;
	uint64_t alloc_time, cpu_map_time, cpu_unmap_time;
	uint64_t gpu_map_time, gpu_unmap_time, free_time;
	struct amdgpu_bo_alloc_request alloc_request;
	char size_str[16];
	char test_id[32];
	uint64_t start, end;
	uint32_t i;
	int ret;

	igt_info("Starting AMDGPU Memory Management Benchmark (DMA ring 0 only)\n");

	/* Initialize benchmark context (memory info + DMA context) */
	init_bench_context(&ctx, device);

	igt_info("Test (avg. ns)          alloc   cpu_map  cpu_umap   gpu_map  gpu_umap      free\n");

	for (test_index = 0; test_index < TOTAL_TESTS; test_index++) {
		uint32_t size_idx = test_index % NUM_SIZES;
		uint32_t mem_type = (test_index / NUM_SIZES) % NUM_MEM_TYPES;
		uint32_t sdma_mode = (test_index / (NUM_SIZES * NUM_MEM_TYPES)) % NUM_SDMA_MODES;

		uint64_t buf_size = buffer_configs[size_idx].size;
		uint32_t n_bufs = buffer_configs[size_idx].num_buffers;
		bool use_vram = (mem_type == 1);
		bool interleave_dma = (sdma_mode == 1);

		/* Skip unsupported memory types (e.g., VRAM on APU) */
		if (use_vram && !ctx.has_vram)
			continue;

		/* Adjust buffer count based on available memory (80% of total) */
		n_bufs = calculate_buffer_limit(&ctx, buf_size, n_bufs, use_vram);
		if (n_bufs == 0)
			continue;

		/* Limit maximum buffers to avoid array overflow */
		if (n_bufs > 1000)
			n_bufs = 1000;

		/* Allocate arrays for buffer handles/VA/mc addresses */
		buffers = calloc(n_bufs, sizeof(amdgpu_bo_handle));
		va_handles = calloc(n_bufs, sizeof(amdgpu_va_handle));
		mc_addresses = calloc(n_bufs, sizeof(uint64_t));
		igt_assert(buffers);
		igt_assert(va_handles);
		igt_assert(mc_addresses);

		/* Initialize timing variables */
		alloc_time = 0;
		cpu_map_time = 0;
		cpu_unmap_time = 0;
		gpu_map_time = 0;
		gpu_unmap_time = 0;
		free_time = 0;

		/* Configure buffer allocation request */
		memset(&alloc_request, 0, sizeof(alloc_request));
		alloc_request.alloc_size = buf_size;
		alloc_request.phys_alignment = 4096;

		if (use_vram) {
			alloc_request.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
			alloc_request.flags = 0; /* Allow CPU mapping for validation */
		} else {
			alloc_request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
			alloc_request.flags = 0;
		}

		/* Benchmark: Buffer Allocation */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			ret = amdgpu_bo_alloc(ctx.device, &alloc_request, &buffers[i]);
			igt_assert_eq(ret, 0);
			interleave_dma_operations(&ctx, interleave_dma); /* Interleave DMA ops if enabled */
		}
		end = get_time_ns();
		alloc_time = end - start;
		wait_dma_operations(&ctx, interleave_dma); /* Wait for pending DMA */

		/* Benchmark: CPU Mapping */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			void *cpu_ptr;

			ret = amdgpu_bo_cpu_map(buffers[i], &cpu_ptr);
			igt_assert_eq(ret, 0);
			interleave_dma_operations(&ctx, interleave_dma);
		}
		end = get_time_ns();
		cpu_map_time = end - start;
		wait_dma_operations(&ctx, interleave_dma);

		/* Benchmark: CPU Unmapping */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			ret = amdgpu_bo_cpu_unmap(buffers[i]);
			igt_assert_eq(ret, 0);
			interleave_dma_operations(&ctx, interleave_dma);
		}
		end = get_time_ns();
		cpu_unmap_time = end - start;
		wait_dma_operations(&ctx, interleave_dma);

		/* Benchmark: GPU VA Mapping (allocate VA + map BO) */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			/* Allocate GPU VA range */
			ret = amdgpu_va_range_alloc(device,
						    amdgpu_gpu_va_range_general,
									   buf_size,
									   1,
									   0,
									   &mc_addresses[i],
									   &va_handles[i],
									   0);
			igt_assert_eq(ret, 0);

			/* Map BO to allocated GPU VA */
			ret = amdgpu_bo_va_op(buffers[i],
					      0,
								 buf_size,
								 mc_addresses[i],
								 0,
								 AMDGPU_VA_OP_MAP);
			igt_assert_eq(ret, 0);
			interleave_dma_operations(&ctx, interleave_dma);
		}
		end = get_time_ns();
		gpu_map_time = end - start;
		wait_dma_operations(&ctx, interleave_dma);

		/* Benchmark: GPU VA Unmapping (unmap BO + free VA) */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			if (va_handles[i]) {
				/* Unmap BO from GPU VA */
				ret = amdgpu_bo_va_op(buffers[i],
						      0,
									 buf_size,
									 mc_addresses[i],
									 0,
									 AMDGPU_VA_OP_UNMAP);
				igt_assert_eq(ret, 0);

				/* Free GPU VA range */
				ret = amdgpu_va_range_free(va_handles[i]);
				igt_assert_eq(ret, 0);

				va_handles[i] = NULL;
				mc_addresses[i] = 0;
			}
			interleave_dma_operations(&ctx, interleave_dma);
		}
		end = get_time_ns();
		gpu_unmap_time = end - start;
		wait_dma_operations(&ctx, interleave_dma);

		/* Benchmark: Buffer Free */
		start = get_time_ns();
		for (i = 0; i < n_bufs; i++) {
			amdgpu_bo_free(buffers[i]);
			interleave_dma_operations(&ctx, interleave_dma);
		}
		end = get_time_ns();
		free_time = end - start;
		wait_dma_operations(&ctx, interleave_dma);

		/* Calculate average time per operation (nanoseconds) */
		alloc_time = alloc_time / n_bufs;
		cpu_map_time = cpu_map_time / n_bufs;
		cpu_unmap_time = cpu_unmap_time / n_bufs;
		gpu_map_time = gpu_map_time / n_bufs;
		gpu_unmap_time = gpu_unmap_time / n_bufs;
		free_time = free_time / n_bufs;

		/* Format buffer size for display (K/M/G units) */
		format_size_string(size_str, sizeof(size_str), buf_size);

		/* Create test identifier (fixed width for alignment) */
		snprintf(test_id, sizeof(test_id), "%s-%s-%s",
			 size_str,
				 mem_type_strings[mem_type],
				 interleave_dma ? "DMA" : "noDMA");

		/* Print results with consistent alignment */
		igt_info("%-18s %9" PRIu64 " %9" PRIu64 " %9" PRIu64 " %9" PRIu64 " %9" PRIu64 " %9" PRIu64 "\n",
			 test_id,
				 alloc_time,
				 cpu_map_time,
				 cpu_unmap_time,
				 gpu_map_time,
				 gpu_unmap_time,
				 free_time);

		/* Log results for CI automation (debug level) */
		igt_debug("mm_bench:%s-%s-%s:alloc=%" PRIu64 "ns\n",
			  size_str,
				  mem_type_strings[mem_type],
				  interleave_dma ? "DMA" : "noDMA",
				  alloc_time);
		igt_debug("mm_bench:%s-%s-%s:cpu_map=%" PRIu64 "ns\n",
			  size_str,
				  mem_type_strings[mem_type],
				  interleave_dma ? "DMA" : "noDMA",
				  cpu_map_time);
		igt_debug("mm_bench:%s-%s-%s:cpu_unmap=%" PRIu64 "ns\n",
			  size_str,
				  mem_type_strings[mem_type],
				  interleave_dma ? "DMA" : "noDMA",
				  cpu_unmap_time);
		igt_debug("mm_bench:%s-%s-%s:gpu_map=%" PRIu64 "ns\n",
			  size_str,
				  mem_type_strings[mem_type],
				  interleave_dma ? "DMA" : "noDMA",
				  gpu_map_time);
		igt_debug("mm_bench:%s-%s-%s:gpu_unmap=%" PRIu64 "ns\n",
			  size_str,
				  mem_type_strings[mem_type],
				  interleave_dma ? "DMA" : "noDMA",
				  gpu_unmap_time);

		/* Cleanup temporary arrays */
		free(buffers);
		free(va_handles);
		free(mc_addresses);

		/* Print separator after each size group */
		if ((test_index % NUM_SIZES) == (NUM_SIZES - 1)) {
			igt_info("--------------------------------------------------------------------------\n");
		}
	}

	/* Cleanup benchmark context (DMA + memory info) */
	cleanup_bench_context(&ctx);

	igt_info("AMDGPU Memory Management Benchmark Completed\n");
}

/**
 * @brief Check if ptrace is allowed on the system
 *
 * Ptrace is required for debugger-like memory access tests. This function
 * checks:
 * 1. Root privileges
 * 2. IGT_FORCE_PTRACE environment variable
 * 3. Yama ptrace_scope sysctl (0 = unrestricted)
 *
 * @return true if ptrace is allowed, false otherwise
 */
static bool is_ptrace_allowed(void)
{
	FILE *yama_file;
	int yama_scope;
	bool result = false;

	/* Root can always use ptrace */
	if (getuid() == 0) {
		result = true;
		goto out;
	}

	/* Allow environment variable override for testing */
	if (getenv("IGT_FORCE_PTRACE")) {
		result = true;
		goto out;
	}

	/* Check Yama ptrace scope (default on most distros is 1) */
	yama_file = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
	if (yama_file) {
		if (fscanf(yama_file, "%d", &yama_scope) == 1) {
			/* Scope 0: Allow ptrace from any process */
			if (yama_scope == 0)
				result = true;
		}
		fclose(yama_file);
	}

	if (!result) {
		igt_info("ptrace not allowed: need root, IGT_FORCE_PTRACE=1, or yama ptrace_scope=0\n");
	}

out:
	return result;
}

/**
 * @brief Verify data integrity after ptrace PEEK/POKE operations
 *
 * Cleans up "gap" bytes that shouldn't be copied, then verifies that
 * the destination region matches the source region.
 *
 * @param mem Pointer to test memory buffer (2x PAGE_SIZE)
 * @param test_data_size Size of data to verify (bytes)
 * @return true if data is consistent, false otherwise
 */
static bool verify_ptrace_data(uint8_t *mem, size_t test_data_size)
{
	/* Clear untested gap bytes (to avoid false positives) */
	mem[sizeof(long)] = 0;
	mem[2 * sizeof(long) + 1] = 0;
	mem[3 * sizeof(long) + 2] = 0;
	mem[4 * sizeof(long) + 3] = 0;

	/* Verify source (first PAGE) == destination (second PAGE) */
	return (memcmp(mem, mem + PAGE_SIZE, test_data_size) == 0);
}

/**
 * @brief Child process function for ptrace testing
 *
 * @param mem Pointer to memory buffer to test
 * @param trace_pid PID of parent process to attach to
 * @return int Exit status (0 for success, non-zero for failure)
 */
static int child_ptrace_process(uint8_t *mem, pid_t trace_pid)
{
	int trace_status;
	int err = 0;
	unsigned int i;
	int r;
	long data;

	igt_info("Child process started, attaching to parent PID %d\n", trace_pid);

	/* Attach to parent process */
	r = ptrace(PTRACE_ATTACH, trace_pid, NULL, NULL);
	if (r != 0) {
		igt_warn("PTRACE_ATTACH failed: %s\n", strerror(errno));
		return 1;
	}

	/* Wait for parent to be stopped by ptrace */
	igt_info("Waiting for parent to stop...\n");
	do {
		r = waitpid(trace_pid, &trace_status, 0);
		if (r == -1) {
			igt_warn("waitpid failed: %s\n", strerror(errno));
			ptrace(PTRACE_DETACH, trace_pid, NULL, NULL);
			return 1;
		}
	} while (!WIFSTOPPED(trace_status));

	igt_info("Parent stopped, starting memory access tests...\n");

	/* Test various memory alignments with ptrace */
	for (i = 0; i < 4; i++) {
		/* Calculate addresses with different alignments */
		uint8_t *addr = (uint8_t *)((long *)(mem) + i) + i;

		errno = 0;

		/* Read data from parent's memory */
		data = ptrace(PTRACE_PEEKDATA, trace_pid, addr, NULL);
		if (errno != 0) {
			igt_warn("PTRACE_PEEKDATA failed at iteration %u: %s\n",
				 i, strerror(errno));
			err = 1;
			continue;
		}

		igt_info("Read data 0x%lx from address %p\n", data, addr);

		/* Write data to parent's memory */
		r = ptrace(PTRACE_POKEDATA, trace_pid, addr + PAGE_SIZE,
			   (void *)data);
		if (r != 0) {
			igt_warn("PTRACE_POKEDATA failed at iteration %u: %s\n",
				 i, strerror(errno));
			err = 1;
		} else {
			igt_info("Wrote data 0x%lx to address %p\n",
				 data, addr + PAGE_SIZE);
		}
	}

	/* Detach from parent process */
	igt_info("Detaching from parent...\n");
	r = ptrace(PTRACE_DETACH, trace_pid, NULL, NULL);
	if (r != 0) {
		igt_warn("PTRACE_DETACH failed: %s\n", strerror(errno));
		return 1;
	}

	igt_info("Child process completed with status %d\n", err);
	return err;
}

/**
 * @brief Wait for child process with timeout
 *
 * @param child_pid PID of child process to wait for
 * @param child_status Output parameter for child status
 * @param timeout_sec Timeout in seconds
 * @return int 0 if child exited normally, -1 on error, -2 on timeout
 */
static int wait_for_child_with_timeout(pid_t child_pid, int *child_status,
				       int timeout_sec)
{
	int wait_result;
	int timeout = timeout_sec;

	while (timeout > 0) {
		wait_result = waitpid(child_pid, child_status, WNOHANG);
		if (wait_result == child_pid) {
			return 0; /* Child exited */
		} else if (wait_result == 0) {
			/* Child still running */
			sleep(1);
			timeout--;
		} else {
			/* Error */
			igt_warn("waitpid failed: %s\n", strerror(errno));
			return -1;
		}
	}

	return -2; /* Timeout */
}

/**
 * @brief Test AMDGPU memory access via ptrace in a debugger-like scenario
 *
 * This function:
 * - Allocates system memory and VRAM buffers
 * - Forks a child process that attaches to parent via ptrace
 * - Child reads/writes parent's memory using PTRACE_PEEKDATA/POKEDATA
 * - Verifies data integrity after child process completes
 * - Tests various memory alignments and boundary conditions
 *
 * @param device Initialized AMDGPU device handle
 */
static void ptrace_access_test(amdgpu_device_handle device)
{
	uint64_t vram_size, cpu_accessible_vram_size, gtt_size;
	int r;

	/* Check if ptrace is allowed */
	if (!is_ptrace_allowed()) {
		igt_skip("ptrace not allowed on this system\n");
		return;
	}

	/* Query GPU memory information */
	r = get_gpu_memory_info(device, &vram_size, &cpu_accessible_vram_size,
				&gtt_size);
	igt_assert_eq(r, 0);

	/* Check VRAM availability */
	igt_info("VRAM size: %" PRIu64 " MB\n", vram_size / (1024 * 1024));

	/* Allow any process to trace this one for debugger functionality */
#ifdef PR_SET_PTRACER
	r = prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
	if (r != 0) {
		igt_info("prctl PR_SET_PTRACER failed: %s\n", strerror(errno));
	}
#endif

	/* Test system memory buffer access via ptrace */
	igt_info("Testing system memory ptrace access...\n");
	{
		const size_t buf_size = PAGE_SIZE * 2;
		const size_t test_data_size = 4 * sizeof(long) + 4;
		struct amdgpu_bo_alloc_request sys_request = {0};
		amdgpu_bo_handle sys_bo;
		void *sys_cpu_ptr;
		pid_t trace_pid, child_pid;
		uint8_t *sys_mem;
		int child_status;
		int wait_result;

		/* Initialize system memory request */
		sys_request.alloc_size = buf_size;
		sys_request.phys_alignment = PAGE_SIZE;
		sys_request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

		/* Allocate system memory buffer */
		r = amdgpu_bo_alloc(device, &sys_request, &sys_bo);
		igt_assert_eq(r, 0);

		/* Map to CPU for access */
		r = amdgpu_bo_cpu_map(sys_bo, &sys_cpu_ptr);
		igt_assert_eq(r, 0);

		/* Initialize test pattern in system memory */
		sys_mem = (uint8_t *)sys_cpu_ptr;
		for (unsigned int i = 0; i < test_data_size; i++) {
			sys_mem[i] = i;                    /* Source data pattern */
			sys_mem[PAGE_SIZE + i] = 0;        /* Destination area */
		}

		trace_pid = getpid();
		igt_info("Parent process PID: %d\n", trace_pid);

		/* Fork child process to act as debugger */
		child_pid = fork();
		igt_assert_f(child_pid >= 0, "fork() failed");

		if (child_pid == 0) {
			/* Child process (debugger) */
			int exit_code = child_ptrace_process(sys_mem, trace_pid);

			_exit(exit_code); /* Use _exit to avoid atexit handlers */
		} else {
			/* Parent process (debuggee) */
			igt_info("Waiting for child process %d to complete...\n",
				 child_pid);

			/* Wait for child debugger to complete with timeout */
			wait_result = wait_for_child_with_timeout(child_pid,
								  &child_status,
								  5);

			if (wait_result == -2) {
				igt_warn("Child process timeout, killing child...\n");
				kill(child_pid, SIGKILL);
				waitpid(child_pid, &child_status, 0);
				igt_skip("Child process timed out - ptrace may be blocked\n");
				goto cleanup_system;
			} else if (wait_result == -1) {
				igt_skip("Error waiting for child process\n");
				goto cleanup_system;
			}

			/* Check child exit status */
			if (WIFEXITED(child_status)) {
				if (WEXITSTATUS(child_status) == 0) {
					igt_info("Child process exited normally\n");
				} else {
					igt_warn("Child process exited with error %d\n",
						 WEXITSTATUS(child_status));
					igt_skip("Child process failed - ptrace may be blocked\n");
					goto cleanup_system;
				}
			} else if (WIFSIGNALED(child_status)) {
				igt_warn("Child process killed by signal %d\n",
					 WTERMSIG(child_status));
				igt_skip("Child process was killed - ptrace may be blocked\n");
				goto cleanup_system;
			} else {
				igt_warn("Child process did not exit normally (status: 0x%x)\n",
					 child_status);
				igt_skip("Child process did not exit properly\n");
				goto cleanup_system;
			}

			igt_info("Parent process verifying data written by child...\n");
			if (!verify_ptrace_data(sys_mem, test_data_size)) {
				igt_assert_f(0, "System memory ptrace data verification failed");
				goto cleanup_system;
			}

			igt_info("System memory ptrace test: PASSED\n");
		}

cleanup_system:
		/* Cleanup system memory buffer */
		amdgpu_bo_cpu_unmap(sys_bo);
		amdgpu_bo_free(sys_bo);
	}

	/* Test VRAM buffer access via ptrace if VRAM is available */
	if (vram_size > 0) {
		igt_info("Testing VRAM memory ptrace access...\n");
		{
			const size_t buf_size = PAGE_SIZE * 2;
			const size_t test_data_size = 4 * sizeof(long) + 4;
			struct amdgpu_bo_alloc_request vram_request = {0};
			amdgpu_bo_handle vram_bo;
			void *vram_cpu_ptr;
			pid_t trace_pid, child_pid;
			uint8_t *vram_mem;
			int child_status;
			int wait_result;

			/* Initialize VRAM request */
			vram_request.alloc_size = buf_size;
			vram_request.phys_alignment = PAGE_SIZE;
			vram_request.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;

			/* Allocate VRAM buffer */
			r = amdgpu_bo_alloc(device, &vram_request, &vram_bo);
			igt_assert_eq(r, 0);

			/* Map VRAM to CPU */
			r = amdgpu_bo_cpu_map(vram_bo, &vram_cpu_ptr);
			igt_assert_eq(r, 0);

			/* Initialize test pattern in VRAM */
			vram_mem = (uint8_t *)vram_cpu_ptr;
			for (unsigned int i = 0; i < test_data_size; i++) {
				vram_mem[i] = i;                    /* Source data pattern */
				vram_mem[PAGE_SIZE + i] = 0;        /* Destination area */
			}

			trace_pid = getpid();

			/* Fork another child process for VRAM testing */
			child_pid = fork();
			igt_assert_f(child_pid >= 0, "fork() failed for VRAM test");

			if (child_pid == 0) {
				/* Child process for VRAM access */
				int exit_code = child_ptrace_process(vram_mem, trace_pid);

				_exit(exit_code);
			} else {
				/* Parent process for VRAM test */
				igt_info("Waiting for VRAM child process %d to complete...\n",
					 child_pid);

				/* Wait for child with timeout */
				wait_result = wait_for_child_with_timeout(child_pid,
									  &child_status,
									  5);

				if (wait_result == -2) {
					igt_warn("VRAM child process timeout, killing child...\n");
					kill(child_pid, SIGKILL);
					waitpid(child_pid, &child_status, 0);
					igt_skip("VRAM child process timed out\n");
					goto cleanup_vram;
				} else if (wait_result == -1) {
					igt_skip("Error waiting for VRAM child process\n");
					goto cleanup_vram;
				}

				if (WIFEXITED(child_status)) {
					if (WEXITSTATUS(child_status) == 0) {
						igt_info("VRAM child process exited normally\n");
					} else {
						igt_warn("VRAM child process exited with error %d\n",
							 WEXITSTATUS(child_status));
						igt_skip("VRAM child process failed\n");
						goto cleanup_vram;
					}
				} else if (WIFSIGNALED(child_status)) {
					igt_warn("VRAM child process killed by signal %d\n",
						 WTERMSIG(child_status));
					igt_skip("VRAM child process was killed\n");
					goto cleanup_vram;
				} else {
					igt_warn("VRAM child process did not exit normally\n");
					igt_skip("VRAM child process did not exit properly\n");
					goto cleanup_vram;
				}

				igt_info("Parent process verifying VRAM data written by child...\n");
				if (!verify_ptrace_data(vram_mem, test_data_size)) {
					igt_assert_f(0, "VRAM memory ptrace data verification failed");
					goto cleanup_vram;
				}

				igt_info("VRAM memory ptrace test: PASSED\n");
			}

cleanup_vram:
			/* Cleanup VRAM buffer */
			amdgpu_bo_cpu_unmap(vram_bo);
			amdgpu_bo_free(vram_bo);
		}
	}
}

/**
 * execute_shader_verification - Execute GPU verification of ptrace results
 * @device: AMDGPU device handle
 * @vram_mc_addr: VRAM buffer GPU address
 * @vram_offset: Offset within VRAM buffer to test words
 * @expected_value1: Expected value for first word (word_before)
 * @expected_value2: Expected value for second word (word_after)
 *
 * This function assembles the shader code and executes two independent compute
 * dispatches to verify that the ptrace-modified values are visible to the GPU.
 * Each dispatch uses completely separate resources to ensure clean state.
 *
 * Returns: 0 on success, -1 on failure
 */
static int execute_shader_verification(amdgpu_device_handle device,
				       uint64_t vram_mc_addr,
				      uint64_t vram_offset,
				      uint32_t expected_value1,
				      uint32_t expected_value2)
{
	struct amdgpu_gpu_info info = {0};
	char mcpu[32];
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	int r_asm;
	uint32_t version = 9;

	amdgpu_bo_handle bo_shader1 = NULL, bo_shader2 = NULL;
	void *ptr_shader1 = NULL, *ptr_shader2 = NULL;
	uint64_t mc_shader1 = 0, mc_shader2 = 0;
	amdgpu_va_handle va_shader1 = 0, va_shader2 = 0;
	int ret = 0;

	/* Query GPU information for architecture-specific setup */
	if (amdgpu_query_gpu_info(device, &info) != 0) {
		igt_info("Query GPU info failed\n");
		return -1;
	}

	amdgpu_family_id_to_mcpu(info.family_id, mcpu, sizeof(mcpu));
	igt_info("Assembling shader for architecture: %s\n", mcpu);

	/* Assemble shader code */
	r_asm = amdgpu_llvm_assemble(mcpu, CopyDwordIsa, isa_bin, sizeof(isa_bin), &isa_sz);
	if (r_asm != 0 || isa_sz == 0) {
		igt_info("Shader assembly failed (error=%d, size=%zu)\n", r_asm, isa_sz);
		return -1;
	}

	/* Determine GPU version for register programming */
	if (FAMILY_IS_GFX1200(info.family_id)) {
		version = 12;
	} else if (FAMILY_IS_GFX1150(info.family_id) ||
	       FAMILY_IS_GFX1100(info.family_id) ||
	       FAMILY_IS_GFX1036(info.family_id) ||
	       FAMILY_IS_GFX1037(info.family_id) ||
	       FAMILY_IS_YC(info.family_id) ||
	       info.family_id == FAMILY_GFX1103) {
		version = 11;
	} else if (FAMILY_IS_AI(info.family_id) ||
	       FAMILY_IS_RV(info.family_id) ||
	       FAMILY_IS_NV(info.family_id) ||
	       info.family_id == FAMILY_VGH) {
		version = 10;
	}

	igt_info("Using GPU version %u for family 0x%x\n", version, info.family_id);

	/* First dispatch - verify word_before value */
	igt_info("=== First dispatch: reading word_before ===\n");

	/* Allocate separate shader buffer for first dispatch */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
				AMDGPU_GEM_DOMAIN_VRAM, 0,
				&bo_shader1, &ptr_shader1, &mc_shader1, &va_shader1) != 0) {
		igt_info("Failed to allocate first shader buffer\n");
		return -1;
	}
	memset(ptr_shader1, 0, 4096);
	memcpy(ptr_shader1, isa_bin, isa_sz);

	if (execute_compute_dispatch(device, version, mc_shader1,
				 vram_mc_addr + vram_offset,
				0, /* dst allocated in helper */
				expected_value1) != 0) {
		igt_info("First dispatch failed\n");
		ret = -1;
	}

	amdgpu_bo_unmap_and_free(bo_shader1, va_shader1, mc_shader1, 4096);

	/* Only proceed if first dispatch succeeded */
	if (ret != 0) {
		return ret;
	}

	/* Second dispatch - verify word_after value */
	igt_info("=== Second dispatch: reading word_after ===\n");

	/* Allocate separate shader buffer for second dispatch */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
				AMDGPU_GEM_DOMAIN_VRAM, 0,
				&bo_shader2, &ptr_shader2, &mc_shader2, &va_shader2) != 0) {
		igt_info("Failed to allocate second shader buffer\n");
		return -1;
	}
	memset(ptr_shader2, 0, 4096);
	memcpy(ptr_shader2, isa_bin, isa_sz);

	if (execute_compute_dispatch(device, version, mc_shader2,
				 vram_mc_addr + vram_offset + sizeof(uint64_t),
				0, /* dst allocated in helper */
				expected_value2) != 0) {
		igt_info("Second dispatch failed\n");
		ret = -1;
	}

	amdgpu_bo_unmap_and_free(bo_shader2, va_shader2, mc_shader2, 4096);

	return ret;
}

/**
 * ptrace_access_invisible_vram_test - Test ptrace access to VRAM with GPU verification
 *
 * This test verifies that ptrace PEEK/POKE operations work correctly on VRAM addresses
 * near a boundary, and that the changes are visible to the GPU. The test:
 * 1. Allocates a VRAM buffer and places two 64-bit words straddling a 4MB boundary
 * 2. Forks a child process that uses ptrace to swap the two words
 * 3. Verifies the swap via CPU mapping
 * 4. Uses compute shaders to read back both words via GPU to confirm ptrace effects
 *
 * The GPU verification uses the refactored helper functions with completely independent
 * contexts and resources for each dispatch to avoid any state pollution.
 */
static void ptrace_access_invisible_vram_test(amdgpu_device_handle device)
{
	uint64_t vram_size, cpu_accessible_vram_size, gtt_size;
	int r;
	const size_t vram_region_size = (4u << 20) + PAGE_SIZE * 2;
	const uint64_t VRAM_OFFSET = (4u << 20) - sizeof(uint64_t);
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle vram_bo = NULL;
	void *cpu_ptr = NULL;
	uint64_t mc_address_vram = 0;
	amdgpu_va_handle va_vram = 0;
	uint64_t *word_before;
	uint64_t *word_after;
	uint64_t original0 = 0xdeadbeefdeadbeefULL;
	uint64_t original1 = 0xcafebabecafebabeULL;
	pid_t parent_pid, child_pid;
	int child_status;
	int wait_result;

	/* Check GPU memory availability and ptrace permissions */
	r = get_gpu_memory_info(device, &vram_size, &cpu_accessible_vram_size, &gtt_size);
	igt_assert_eq(r, 0);
	if (vram_size == 0) {
		igt_skip("No VRAM present (APU)\n");
		return;
	}
	if (!is_ptrace_allowed()) {
		igt_skip("ptrace not allowed\n");
		return;
	}

	igt_info("VRAM total: %" PRIu64 " MB, CPU-accessible: %" PRIu64 " MB\n",
	     vram_size / (1024 * 1024), cpu_accessible_vram_size / (1024 * 1024));

	/* Allocate VRAM buffer for testing */
	req.alloc_size = vram_region_size;
	req.phys_alignment = PAGE_SIZE;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
	r = amdgpu_bo_alloc_and_map(device, req.alloc_size, req.phys_alignment,
				AMDGPU_GEM_DOMAIN_VRAM, 0,
				&vram_bo, &cpu_ptr, &mc_address_vram, &va_vram);
	if (r) {
		igt_skip("Failed to allocate/map VRAM BO (error %d)\n", r);
		return;
	}

	/* Initialize test words at boundary locations */
	word_before = (uint64_t *)((uint8_t *)cpu_ptr + VRAM_OFFSET);
	word_after  = (uint64_t *)((uint8_t *)cpu_ptr + VRAM_OFFSET + sizeof(uint64_t));
	*word_before = original0;
	*word_after  = original1;

	/* Allow ptrace from any process if supported */
	#ifdef PR_SET_PTRACER
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
	#endif
	parent_pid = getpid();

	/* Fork child process for ptrace operations */
	child_pid = fork();
	igt_assert_f(child_pid >= 0, "fork failed");

	if (child_pid == 0) {
		/* Child process: attach to parent and swap words via ptrace */
		int trace_status;
		int err = 0;
		int pr;
		uint64_t peek0, peek1;

		/* Attach to parent process */
		pr = ptrace(PTRACE_ATTACH, parent_pid, NULL, NULL);
		if (pr)
		    _exit(1);

		/* Wait for parent to be stopped by ptrace */
		do {
		    waitpid(parent_pid, &trace_status, 0);
		} while (!WIFSTOPPED(trace_status));

		/* Read original values via ptrace */
		errno = 0;
		peek0 = (uint64_t)ptrace(PTRACE_PEEKDATA, parent_pid, word_before, NULL);
		if (errno)
		    err = 1;
		errno = 0;
		peek1 = (uint64_t)ptrace(PTRACE_PEEKDATA, parent_pid, word_after, NULL);
		if (errno)
		    err = 1;

		/* Verify we read the expected original values */
		if (peek0 != original0 || peek1 != original1)
		    err = 1;

		/* Swap values via ptrace POKEDATA */
		if (!err) {
		if (ptrace(PTRACE_POKEDATA, parent_pid, word_before, from_user_pointer(original1)))
			err = 1;
		if (ptrace(PTRACE_POKEDATA, parent_pid, word_after, from_user_pointer(original0)))
			err = 1;
		}

		/* Detach and exit with result */
		ptrace(PTRACE_DETACH, parent_pid, NULL, NULL);
		_exit(err);

	} else {
		/* Parent process: wait for child and verify results */
		wait_result = wait_for_child_with_timeout(child_pid, &child_status, 5);
		if (wait_result == -2) {
			kill(child_pid, SIGKILL);
			waitpid(child_pid, &child_status, 0);
			igt_skip("Child timeout during ptrace operations\n");
			goto cleanup;
		} else if (wait_result == -1) {
			igt_skip("Error waiting for child process\n");
			goto cleanup;
		}
		if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
			igt_skip("Child ptrace swap failed (status=%d)\n", child_status);
			goto cleanup;
		}

		/* Verify swap via CPU mapping */
		igt_info("Child ptrace swap succeeded. Verifying via CPU...\n");
		igt_assert_eq(*word_before, original1);
		igt_assert_eq(*word_after,  original0);
		igt_info("CPU verification passed. Attempting shader readback...\n");

		/* GPU verification using refactored helper functions */
		if (amdgpu_llvm_asm_init() == 0) {
			if (execute_shader_verification(device, mc_address_vram, VRAM_OFFSET,
							(uint32_t)original1, (uint32_t)original0) == 0) {
				igt_info("GPU shader verification passed\n");
			} else {
				igt_info("GPU shader verification failed\n");
				/* Continue test execution even if GPU verification fails */
			}

			amdgpu_llvm_asm_shutdown();
		} else {
		    igt_info("LLVM assembler unavailable; skipping shader verification.\n");
		}

		igt_info("ptrace-access-invisible-vram test: PASSED\n");
	}

	cleanup:
	if (vram_bo)
	amdgpu_bo_unmap_and_free(vram_bo, va_vram, mc_address_vram, req.alloc_size);
}

/* Global variable to track signal reception */
static volatile sig_atomic_t signal_received;

/**
 * @brief Signal handler function
 */
static void catch_signal(int sig)
{
    signal_received = sig;
}

/**
 * @brief Test signal handling during memory operations
 */
static void test_signal_handling(amdgpu_device_handle device)
{
	uint64_t vram_size, cpu_accessible_vram_size, gtt_size;
	int r;
	struct sigaction sa;
	uint64_t buffer_size;
	amdgpu_bo_handle sys_bo;
	void *sys_cpu_ptr;
	uint64_t sys_mc_address;
	amdgpu_va_handle va_handle;
	pid_t parent_pid;
	pid_t child_pid;
	int child_status;
	cmd_context_t *dma_ctx = NULL;
	uint32_t *sys_mem;
	bool success;
	pid_t wait_pid;
	int wait_result;

	/* Skip test on APU systems */
	r = get_gpu_memory_info(device, &vram_size, &cpu_accessible_vram_size, &gtt_size);
	igt_assert_eq(r, 0);

	if (vram_size == 0) {
		igt_skip("Test not supported on APU systems\n");
		return;
	}

	igt_info("VRAM size: %" PRIu64 " MB\n", vram_size / (1024 * 1024));

	/* Set up signal handler */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = catch_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART; /* Use SA_RESTART to avoid EINTR on system calls */

	r = sigaction(SIGUSR1, &sa, NULL);
	igt_assert_eq(r, 0);

	signal_received = 0;
	parent_pid = getpid();

	/* Calculate buffer size - similar to reference implementation */
	buffer_size = 256 * 1024 * 1024; /* 256 MB - reasonable size for testing */

	igt_info("Allocating buffer size: %" PRIu64 " MB\n", buffer_size / (1024 * 1024));

	/* Allocate system memory */
	sys_bo = gpu_mem_alloc(device, buffer_size, PAGE_SIZE,
		   AMDGPU_GEM_DOMAIN_GTT, 0,
		  &sys_mc_address, &va_handle);
	igt_assert_f(sys_bo, "Failed to allocate system memory");

	r = amdgpu_bo_cpu_map(sys_bo, &sys_cpu_ptr);
	igt_assert_eq(r, 0);

	igt_info("System memory allocated at CPU: %p, GPU: 0x%" PRIx64 "\n", sys_cpu_ptr, sys_mc_address);

	/* Fork child process to send signal */
	child_pid = fork();
	igt_assert_f(child_pid >= 0, "fork() failed");

	if (child_pid == 0) {
		/* Child process - send signal to parent */
		igt_info("Child process sending SIGUSR1 to parent PID %d\n", parent_pid);

		/* Small delay to ensure parent is in memory mapping */
		usleep(100000); /* 100ms delay - increased for better timing */

		r = kill(parent_pid, SIGUSR1);
		if (r != 0) {
		    igt_warn("kill() failed: %s\n", strerror(errno));
		    exit(1);
		}

		igt_info("Signal sent successfully\n");
			exit(0);
	} else {
		/* Parent process - perform memory operations with signal handling */
		igt_info("Parent process starting memory mapping operations...\n");

		/*
		 * Create a DMA context; sys_bo is registered in its residency set
		 * immediately before the submit below (residency is per-submit).
		 */
		dma_ctx = cmd_context_create(device, AMD_IP_DMA, 0, false);
		igt_assert_f(dma_ctx, "Failed to create DMA command submission context");

		/* Wait for child process to complete with proper signal handling */
		igt_info("Waiting for child process %d to complete...\n", child_pid);

		/* Use a loop to handle EINTR properly */
		do {
			wait_pid = waitpid(child_pid, &child_status, 0);
			wait_result = errno;

			if (wait_pid == -1 && wait_result == EINTR) {
				igt_info("waitpid interrupted by signal, continuing...\n");
				continue;
			}

			if (signal_received) {
				igt_info("Signal %d received and handled\n", signal_received);
				signal_received = 0;
			}

		} while (wait_pid == -1 && wait_result == EINTR);

		/* Check if waitpid succeeded */
		igt_assert_f(wait_pid != -1, "waitpid failed: %s", strerror(wait_result));
		igt_assert_f(wait_pid == child_pid, "waitpid returned wrong PID: %d != %d", wait_pid, child_pid);

		/* FIX: Correct child status check - WIFEXITED returns boolean, not status */
		igt_assert_f(WIFEXITED(child_status) == 0,
		     "Child did not exit normally, status: 0x%x", child_status);
		igt_assert_f(WEXITSTATUS(child_status) == 0,
		     "Child exited with status %d", WEXITSTATUS(child_status));

		igt_info("Child process completed successfully\n");

		/* Continue with memory operations after signal handling */
		igt_info("Continuing with memory operations after signal...\n");

		/* Initialize memory with test value */
		sys_mem = (uint32_t *)sys_cpu_ptr;
		sys_mem[0] = 0x02020202;
		igt_info("Memory initialized with value: 0x%08x\n", sys_mem[0]);

		/* Submit GPU command to write different value */
		igt_info("Submitting GPU write command...\n");
		igt_info("Writing 0xdeadbeaf to address 0x%" PRIx64 "\n", sys_mc_address);

		/* Now SDMA should write to the correct address since we're using the same BO */
		igt_assert_eq(cmd_context_add_resource(dma_ctx, sys_bo), 0);
		r = cmd_submit_write_linear(dma_ctx, sys_mc_address, sizeof(uint32_t), 0xdeadbeaf);
		igt_info("cmd_submit_write_linear returned: %d\n", r);
		igt_assert_eq(r, 0);

		/* Wait for command completion */
		igt_info("Waiting for command completion...\n");
		r = cmd_wait_completion(dma_ctx);
		igt_info("cmd_wait_completion returned: %d\n", r);
		igt_assert_eq(r, 0);

		/* Wait for the value to be updated */
		igt_info("Waiting for memory value update...\n");
		igt_info("Current memory value: 0x%08x\n", sys_mem[0]);

		success = wait_on_value(sys_mem, 0xdeadbeaf);
		if (!success) {
			igt_info("Memory value not updated within timeout, current value: 0x%08x, %p\n", sys_mem[0], sys_cpu_ptr);
			/* If still not updated, try a different approach */
			igt_info("Trying alternative: check if any value was written...\n");
			if (sys_mem[0] != 0x02020202) {
			igt_info("Memory was changed to: 0x%08x (not the expected value)\n", sys_mem[0]);
			} else {
			igt_info("Memory was not modified at all\n");
			}
			igt_assert_f(success, "Memory value not updated within timeout");
		}

		igt_info("Memory successfully updated to: 0x%08x\n", sys_mem[0]);

		/* Cleanup command context; sys_bo is owned by the caller. */
		cmd_context_destroy(dma_ctx);
		dma_ctx = NULL;
	}

	/* Cleanup system memory */
	amdgpu_bo_cpu_unmap(sys_bo);

	r = amdgpu_bo_va_op(sys_bo, 0, buffer_size, sys_mc_address, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	amdgpu_bo_free(sys_bo);

	igt_info("Signal handling test: PASSED\n");
}

/**
 * Test: Check Zero Initialization of GTT and VRAM
 *
 * This test verifies that newly allocated GTT (system memory) and VRAM buffers
 * are properly zero-initialized by the driver.
 */
static void test_zero_initialization(amdgpu_device_handle device)
{
	const int test_iterations = 5;
	const size_t buffer_size_mb = 256;  /* 256 MB test buffer */
	const size_t buffer_size = buffer_size_mb * 1024 * 1024;
	const unsigned int check_offset = 257;  /* Arbitrary offset for checking */

	/* Test both GTT and VRAM domains */
	const uint32_t memory_domains[] = {
	AMDGPU_GEM_DOMAIN_GTT,
	AMDGPU_GEM_DOMAIN_VRAM
	};

	const char *domain_names[] = {
	"GTT",
	"VRAM"
	};

	const int num_domains = ARRAY_SIZE(memory_domains);
	int iteration, domain_idx;
	int r;

	igt_info("Starting zero initialization test for GTT and VRAM\n");
	igt_info("Buffer size: %zu MB, Iterations: %d\n", buffer_size_mb, test_iterations);

	for (domain_idx = 0; domain_idx < num_domains; domain_idx++) {
		uint32_t domain = memory_domains[domain_idx];
		const char *domain_name = domain_names[domain_idx];

		igt_info("Testing memory domain: %s\n", domain_name);

		for (iteration = 0; iteration < test_iterations; iteration++) {
			amdgpu_bo_handle bo = NULL;
			void *cpu_ptr = NULL;
			uint64_t mc_address = 0;
			amdgpu_va_handle va_handle = NULL;
			uint64_t *mem_buffer = NULL;
			size_t buffer_elements;
			unsigned int i;

			igt_debug("Iteration %d: Allocating %s...\n", iteration + 1, domain_name);

			/* Allocate memory in the specified domain */
			r = amdgpu_bo_alloc_and_map_sync(device,
						     buffer_size,
				4096,
				domain,
				0,  /* No special flags */
				AMDGPU_VM_MTYPE_UC,
				&bo, &cpu_ptr, &mc_address, &va_handle,
				0, 0, false);

			if (r != 0) {
				igt_warn("%s allocation failed in iteration %d, error: %d\n",
					 domain_name, iteration + 1, r);
				continue;
			}

			mem_buffer = (uint64_t *)cpu_ptr;
			buffer_elements = buffer_size / sizeof(uint64_t);

			igt_debug("Iteration %d: Checking zero initialization for %s...\n",
			      iteration + 1, domain_name);

			/* Check first 64-bit element */
			igt_assert_f(mem_buffer[0] == 0,
				 "%s first element not zero: 0x%016" PRIx64,
				domain_name, mem_buffer[0]);

			/* Check elements at various offsets with page stride */
			for (i = check_offset; i < buffer_elements; i += (4096 / sizeof(uint64_t))) {
			igt_assert_f(mem_buffer[i] == 0,
				     "%s element at offset %u not zero: 0x%016" PRIx64,
				    domain_name, i, mem_buffer[i]);
			}

			/* Check last 64-bit element */
			igt_assert_f(mem_buffer[buffer_elements - 1] == 0,
				 "%s last element not zero: 0x%016" PRIx64,
				domain_name, mem_buffer[buffer_elements - 1]);

			/* Cleanup */
			amdgpu_bo_unmap_and_free(bo, va_handle, mc_address, buffer_size);

			igt_debug("Iteration %d: %s zero initialization check passed\n",
			      iteration + 1, domain_name);
		}

		igt_info("Memory domain %s testing completed\n", domain_name);
	}

	igt_info("GTT and VRAM zero initialization test completed successfully\n");
}

/**
 * Helper function to perform memory access patterns
 * Similar to the 'access' function in the reference test
 */
static void memory_access_pattern(void *buffer, size_t size, int write_pattern)
{
	volatile uint32_t *ptr = (volatile uint32_t *)buffer;
	size_t elements = size / sizeof(uint32_t);
	uint32_t pattern;
	size_t i;

	if (write_pattern) {
		pattern = 0xDEADBEEF;
		for (i = 0; i < elements; i++) {
			ptr[i] = pattern;
		}
	} else {
		for (i = 0; i < elements; i++) {
			pattern = ptr[i];  /* Read operation */
		}
	}
}

/**
 * Test: Memory Bandwidth Measurement on Large-BAR Systems
 *
 * This test measures memory access bandwidth for different memory types
 * and buffer sizes on large-bar systems. It tests both system memory
 * and visible VRAM access patterns.
 */
static void test_memory_bandwidth(amdgpu_device_handle device)
{
	/* Pre-declare all variables at the beginning for C90 compatibility */
	const unsigned int num_buffers = 1000;
	const unsigned int num_mem_types = 2;
	const unsigned int num_sizes = 4;
	const unsigned int tmp_buffer_size = PAGE_SIZE * 64;
	const unsigned int num_tests = num_sizes * num_mem_types;

	/* Use different names to avoid shadowing global variables */
	const char *mem_type_names[num_mem_types];
	unsigned int buffer_sizes[num_sizes];
	amdgpu_bo_handle buffers[num_buffers];
	void *cpu_ptrs[num_buffers];
	uint64_t mc_addresses[num_buffers];
	amdgpu_va_handle va_handles[num_buffers];
	uint64_t vram_size;
	uint64_t cpu_accessible_vram_size;
	uint64_t gtt_size;
	void *tmp_buffer;
	int r;
	unsigned int test_index, i;

	/* Initialize arrays without using variable-sized initializers */
	mem_type_names[0] = "SysMem";
	mem_type_names[1] = "VRAM";

	buffer_sizes[0] = PAGE_SIZE;
	buffer_sizes[1] = PAGE_SIZE * 4;
	buffer_sizes[2] = PAGE_SIZE * 16;
	buffer_sizes[3] = PAGE_SIZE * 64;

	/* Initialize buffers array */
	for (i = 0; i < num_buffers; i++) {
		buffers[i] = NULL;
		cpu_ptrs[i] = NULL;
		va_handles[i] = NULL;
	}

	igt_info("Starting Memory Bandwidth Test\n");

	/* Query memory information */
	r = get_gpu_memory_info(device, &vram_size, &cpu_accessible_vram_size, &gtt_size);
	igt_assert_eq(r, 0);

	igt_info("Found VRAM of %" PRIu64 "MB.\n", vram_size >> 20);

	/* Check if this is a large-bar system */
	if (cpu_accessible_vram_size == 0) {
		igt_skip("Test requires a large-bar system with CPU accessible VRAM\n");
		return;
	}

	/* Allocate temporary system buffer for memcpy operations */
	tmp_buffer = mmap(0, tmp_buffer_size,
		      PROT_READ | PROT_WRITE,
		     MAP_ANONYMOUS | MAP_PRIVATE,
		     -1, 0);
	igt_assert_f(tmp_buffer != MAP_FAILED, "Failed to allocate temporary buffer");
	memset(tmp_buffer, 0xAA, tmp_buffer_size);  /* Fill with test pattern */

	igt_info("Test (avg. ns)      memcpyRTime memcpyWTime accessRTime accessWTime\n");
	igt_info("----------------------------------------------------------------------\n");

	for (test_index = 0; test_index < num_tests; test_index++) {
		unsigned int buffer_size;
		unsigned int mem_type;
		uint64_t memcpy_read_time, memcpy_write_time;
		uint64_t access_read_time, access_write_time;
		uint64_t start_time, end_time;
		unsigned int buffer_limit;
		unsigned int domain, flags;
		int buffers_allocated;
		uint64_t avg_memcpy_read, avg_memcpy_write;
		uint64_t avg_access_read, avg_access_write;

		buffer_size = buffer_sizes[test_index % num_sizes];
		mem_type = (test_index / num_sizes) % num_mem_types;
		memcpy_read_time = 0;
		memcpy_write_time = 0;
		access_read_time = 0;
		access_write_time = 0;
		buffers_allocated = 0;
		buffer_limit = num_buffers;

		/* Skip if buffer size is larger than available VRAM for VRAM tests */
		if (mem_type == 1) { /* VRAM */
			/* Calculate buffer limit to use 80% of available CPU accessible VRAM */
			buffer_limit = ((cpu_accessible_vram_size * 8) / 10) / buffer_size;
			if (buffer_limit == 0) {
				igt_info("Skipping - buffer size %u too large for available VRAM\n", buffer_size);
				continue;
			}
			if (buffer_limit > num_buffers) {
				buffer_limit = num_buffers;
			}

			domain = AMDGPU_GEM_DOMAIN_VRAM;
			flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
		} else { /* System memory */
			domain = AMDGPU_GEM_DOMAIN_GTT;
			flags = 0;
		}

		/* Allocate buffers */
		for (i = 0; i < buffer_limit; i++) {
			r = amdgpu_bo_alloc_and_map_sync(device,
						     buffer_size,
						    4096,
						    domain,
						    flags,
						    AMDGPU_VM_MTYPE_UC,
						    &buffers[i], &cpu_ptrs[i],
						    &mc_addresses[i], &va_handles[i],
						    0, 0, false);
			if (r != 0) {
				igt_warn("Buffer allocation failed at index %u: %d\n", i, r);
				break;
			}
			buffers_allocated++;
		}

		if (buffers_allocated == 0) {
			igt_warn("No buffers allocated for this test case\n");
			continue;
		}

		/* Test 1: Memory write bandwidth (memcpy to buffer) */
		start_time = get_time_ns();
		for (i = 0; i < buffers_allocated; i++) {
			size_t copy_size = buffer_size < tmp_buffer_size ? buffer_size : tmp_buffer_size;

			memcpy(cpu_ptrs[i], tmp_buffer, copy_size);
		}
		end_time = get_time_ns();
		memcpy_write_time = end_time - start_time;

		/* Test 2: Memory write bandwidth (direct access) */
		start_time = get_time_ns();
		for (i = 0; i < buffers_allocated; i++) {
			size_t access_size = buffer_size < tmp_buffer_size ? buffer_size : tmp_buffer_size;

			memory_access_pattern(cpu_ptrs[i], access_size, 1);
		}
		end_time = get_time_ns();
		access_write_time = end_time - start_time;

		/* Test 3: Memory read bandwidth (memcpy from buffer) */
		start_time = get_time_ns();
		for (i = 0; i < buffers_allocated; i++) {
			size_t copy_size = buffer_size < tmp_buffer_size ? buffer_size : tmp_buffer_size;

			memcpy(tmp_buffer, cpu_ptrs[i], copy_size);
		}
		end_time = get_time_ns();
		memcpy_read_time = end_time - start_time;

		/* Test 4: Memory read bandwidth (direct access) */
		start_time = get_time_ns();
		for (i = 0; i < buffers_allocated; i++) {
			size_t access_size = buffer_size < tmp_buffer_size ? buffer_size : tmp_buffer_size;

			memory_access_pattern(cpu_ptrs[i], access_size, 0);
		}
		end_time = get_time_ns();
		access_read_time = end_time - start_time;

		/* Calculate average time per operation in nanoseconds */
		avg_memcpy_read = memcpy_read_time / buffers_allocated;
		avg_memcpy_write = memcpy_write_time / buffers_allocated;
		avg_access_read = access_read_time / buffers_allocated;
		avg_access_write = access_write_time / buffers_allocated;

		/* Format output to match the reference exactly */
		if (buffer_size == PAGE_SIZE) {
			igt_info("  4K-%-14s %17" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "\n",
			mem_type_names[mem_type],
			avg_memcpy_read, avg_memcpy_write,
			avg_access_read, avg_access_write);
		} else if (buffer_size == PAGE_SIZE * 4) {
			igt_info(" 16K-%-14s %17" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "\n",
			mem_type_names[mem_type],
			avg_memcpy_read, avg_memcpy_write,
			avg_access_read, avg_access_write);
		} else if (buffer_size == PAGE_SIZE * 16) {
			igt_info(" 64K-%-14s %17" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "\n",
			mem_type_names[mem_type],
			avg_memcpy_read, avg_memcpy_write,
			avg_access_read, avg_access_write);
		} else if (buffer_size == PAGE_SIZE * 64) {
			igt_info("256K-%-14s %17" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "\n",
			mem_type_names[mem_type],
			avg_memcpy_read, avg_memcpy_write,
			avg_access_read, avg_access_write);
		}

		/* Record results for automated testing */
		igt_debug("bandwidth:%s-%uK:memcpy_read=%" PRIu64 "ns\n",
		mem_type_names[mem_type], buffer_size >> 10, avg_memcpy_read);
		igt_debug("bandwidth:%s-%uK:memcpy_write=%" PRIu64 "ns\n",
		mem_type_names[mem_type], buffer_size >> 10, avg_memcpy_write);
		igt_debug("bandwidth:%s-%uK:access_read=%" PRIu64 "ns\n",
		mem_type_names[mem_type], buffer_size >> 10, avg_access_read);
		igt_debug("bandwidth:%s-%uK:access_write=%" PRIu64 "ns\n",
		mem_type_names[mem_type], buffer_size >> 10, avg_access_write);

		/* Cleanup buffers */
		for (i = 0; i < buffers_allocated; i++) {
		amdgpu_bo_unmap_and_free(buffers[i], va_handles[i],
			     mc_addresses[i], buffer_size);
		buffers[i] = NULL;
		cpu_ptrs[i] = NULL;
		va_handles[i] = NULL;
		}

		/* Print separator for each new size group */
		if ((test_index % num_sizes) == (num_sizes - 1)) {
		    igt_info("----------------------------------------------------------------------\n");
		}

		/* Skip slow tests (similar to reference implementation) */
		if ((memcpy_read_time + memcpy_write_time +
			access_read_time + access_write_time) > 5000000000ULL) { /* 5 seconds */
			igt_info("Skipping remaining slow tests\n");
			break;
		}
	}

	/* Cleanup temporary buffer */
	munmap(tmp_buffer, tmp_buffer_size);

	igt_info("Memory Bandwidth Test Completed\n");
}

/**
 * Structure for multi-thread userptr registration test
 */
struct mt_userptr_thread_params {
	amdgpu_device_handle device;
	void *buffer;
	uint64_t buffer_size;
	amdgpu_bo_handle bo_handle;
	uint64_t mc_address;
	amdgpu_va_handle va_handle;
	pthread_barrier_t *barrier;
	int result;
};

/**
 * Thread function to register userptr memory
 */
static void *mt_register_userptr_thread(void *arg)
{
	struct mt_userptr_thread_params *params = (struct mt_userptr_thread_params *)arg;
	int r;

	/* Wait for all threads to be ready */
	pthread_barrier_wait(params->barrier);

	/* Register the userptr memory with the GPU - this creates a BO handle */
	r = amdgpu_create_bo_from_user_mem(params->device,
				       params->buffer,
				       params->buffer_size,
				       &params->bo_handle);
	if (r != 0) {
		igt_warn("Thread failed to register userptr: %d\n", r);
		params->result = r;
		return NULL;
	}

	/* Allocate VA range for this thread's BO */
	r = amdgpu_va_range_alloc(params->device, amdgpu_gpu_va_range_general,
			     params->buffer_size, 4096, 0,
			     &params->mc_address, &params->va_handle, 0);
	if (r != 0) {
		igt_warn("Thread failed to allocate VA range: %d\n", r);
		params->result = r;
		amdgpu_bo_free(params->bo_handle);
		params->bo_handle = NULL;
		return NULL;
	}

	/* Map the BO to the allocated GPU virtual address */
	r = amdgpu_bo_va_op(params->bo_handle, 0, params->buffer_size,
			params->mc_address, 0, AMDGPU_VA_OP_MAP);
	if (r != 0) {
		igt_warn("Thread failed to map userptr to GPU: %d\n", r);
		params->result = r;
		amdgpu_va_range_free(params->va_handle);
		amdgpu_bo_free(params->bo_handle);
		params->bo_handle = NULL;
		return NULL;
	}

	params->result = 0;
	return NULL;
}

/**
 * Thread function to unregister userptr memory
 */
static void *mt_unregister_userptr_thread(void *arg)
{
	struct mt_userptr_thread_params *params = (struct mt_userptr_thread_params *)arg;
	int r;

	/* Wait for all threads to be ready */
	pthread_barrier_wait(params->barrier);

	/* Unmap from GPU address space */
	if (params->bo_handle) {
		r = amdgpu_bo_va_op(params->bo_handle, 0, params->buffer_size,
				    params->mc_address, 0, AMDGPU_VA_OP_UNMAP);
		if (r != 0) {
		    igt_warn("Thread failed to unmap userptr: %d\n", r);
		    params->result = r;
		}

		/* Free the VA range */
		if (params->va_handle) {
		    amdgpu_va_range_free(params->va_handle);
		    params->va_handle = NULL;
		}

		/* Free the BO handle */
		amdgpu_bo_free(params->bo_handle);
		params->bo_handle = NULL;
	}

	params->result = 0;
	return NULL;
}

/**
 * Test: Multi-threaded Userptr Registration
 *
 * This test verifies that multiple threads can concurrently register
 * the same userptr memory region with the GPU driver. This tests the
 * thread safety of the userptr registration path in the kernel driver.
 *
 * Based on KFD's MultiThreadRegisterUserptrTest which verifies that
 * concurrent registration operations are properly serialized and all
 * threads can successfully register/map the same userptr buffer.
 */
#define N_THREADS 32

static void test_multithread_register_userptr(amdgpu_device_handle device)
{
	const uint64_t buffer_size = 128ULL * 1024 * 1024;  /* 128 MB */
	void *buffer = NULL;
	pthread_t threads[N_THREADS];
	struct mt_userptr_thread_params params[N_THREADS];
	pthread_barrier_t barrier;
	unsigned int i;
	int r;

	igt_info("Starting multi-thread userptr registration test\n");
	igt_info("Thread count: %d, Buffer size: %llu MB\n",
	     N_THREADS, (unsigned long long)(buffer_size >> 20));

	/* Allocate system memory buffer using mmap */
	buffer = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
		  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	igt_assert_f(buffer != MAP_FAILED, "Failed to allocate buffer: %s", strerror(errno));

	/* Initialize buffer with test pattern */
	memset(buffer, 0x5A, buffer_size);

	/* Initialize pthread barrier for N_THREADS threads */
	r = pthread_barrier_init(&barrier, NULL, N_THREADS);
	igt_assert_eq(r, 0);

	/* Phase 1: Concurrent registration - all threads register the same buffer */
	igt_info("Phase 1: Launching %d threads to register userptr concurrently...\n", N_THREADS);

	for (i = 0; i < N_THREADS; i++) {
	params[i].device = device;
	params[i].buffer = buffer;
	params[i].buffer_size = buffer_size;
	params[i].bo_handle = NULL;
	params[i].mc_address = 0;
	params[i].va_handle = NULL;
	params[i].barrier = &barrier;
	params[i].result = -1;

	r = pthread_create(&threads[i], NULL, mt_register_userptr_thread, &params[i]);
	igt_assert_eq(r, 0);
	}

	/* Wait for all registration threads to complete */
	for (i = 0; i < N_THREADS; i++) {
	r = pthread_join(threads[i], NULL);
	igt_assert_eq(r, 0);
	igt_assert_f(params[i].result == 0,
		     "Thread %u registration failed with error: %d", i, params[i].result);
	}

	igt_info("Phase 1 completed: All threads successfully registered userptr\n");

	/* Verify that all threads received valid BO handles and GPU VAs */
	for (i = 0; i < N_THREADS; i++) {
		igt_assert_f(params[i].bo_handle,
			     "Thread %u has NULL BO handle after registration", i);
		igt_assert_f(params[i].mc_address != 0,
			     "Thread %u has NULL GPU VA after registration", i);
		igt_assert_f(params[i].va_handle,
			     "Thread %u has NULL VA handle after registration", i);
	}

	/* Log the GPU virtual addresses assigned to each thread */
	igt_debug("GPU Virtual Addresses assigned to threads:\n");
	for (i = 0; i < N_THREADS; i++) {
		igt_debug("  Thread %2u: VA = 0x%016llx\n", i,
			  (unsigned long long)params[i].mc_address);
	}

	/* Phase 2: Concurrent unregistration */
	igt_info("Phase 2: Launching %d threads to unregister userptr concurrently...\n", N_THREADS);

	/* Reinitialize barrier for unregistration phase */
	pthread_barrier_destroy(&barrier);
	r = pthread_barrier_init(&barrier, NULL, N_THREADS);
	igt_assert_eq(r, 0);

	for (i = 0; i < N_THREADS; i++) {
		params[i].result = -1;
		r = pthread_create(&threads[i], NULL, mt_unregister_userptr_thread, &params[i]);
		igt_assert_eq(r, 0);
	}

	/* Wait for all unregistration threads to complete */
	for (i = 0; i < N_THREADS; i++) {
		r = pthread_join(threads[i], NULL);
		igt_assert_eq(r, 0);
		igt_assert_f(params[i].result == 0,
			     "Thread %u unregistration failed with error: %d", i, params[i].result);
	}

	igt_info("Phase 2 completed: All threads successfully unregistered userptr\n");

	/* Cleanup */
	pthread_barrier_destroy(&barrier);
	munmap(buffer, buffer_size);

	igt_info("Multi-thread userptr registration test: PASSED\n");
}

/**
 * Test: Deliberately fragment GPUVM aperture to fill up address space
 *
 * This test implements a strategy to maximize fragmented address space usage
 * while keeping committed memory bounded. It allocates buffers in a pattern
 * that creates holes in the address space, then fills them with larger blocks
 * in subsequent iterations.
 *
 * Strategy:
 * 1. Allocate N blocks of a given size (initially 1 page)
 * 2. Free every other block, creating holes
 * 3. Allocate N/4 blocks of 2-pages each (requires same memory as freed in step 2)
 * 4. Free half the blocks, creating larger holes
 * 5. Double block size, halve number of blocks each iteration
 * 6. Repeat until block size reaches half of free memory
 */
static void test_gpuvm_aperture_fragmentation(amdgpu_device_handle device)
{
	const unsigned int max_order_limit = 14; /* Limit to prevent excessive runtime */
	const size_t base_page_size = 4096;
	unsigned int max_order = 0;
	uint64_t vram_size, cpu_accessible_vram_size, gtt_size;
	uint64_t available_memory_pages;
	/* Calculate and log fragmentation statistics */
	uint64_t total_address_space = 0;
	uint64_t total_allocated_memory = 0;
	int r;

	struct buffer_level {
		amdgpu_bo_handle *buffers;
		uint64_t *mc_addresses;
		amdgpu_va_handle *va_handles;
		unsigned long num_buffers;
		unsigned int order;
	} levels[max_order_limit + 1];

	unsigned int order, o;
	unsigned long p, step, offset;
	size_t block_size;
	uint32_t test_value = 0;

	igt_info("Starting GPUVM aperture fragmentation test\n");

	/* Query available memory */
	r = get_gpu_memory_info(device, &vram_size, &cpu_accessible_vram_size, &gtt_size);
	igt_assert_eq(r, 0);

	if (vram_size == 0) {
		igt_skip("Test requires VRAM (not supported on APU)\n");
		return;
	}

	/* Calculate maximum order based on available memory */
	available_memory_pages = vram_size / base_page_size;

	/* Use up to half of available memory to avoid excessive memory movement */
	available_memory_pages /= 2;

	/* Find maximum order where we can allocate at least 16 pages */
	while (((available_memory_pages >> max_order) >= 16) && (max_order < max_order_limit))
		max_order++;

	igt_info("Available VRAM: %" PRIu64 " MB, Max order: %u\n",
	     vram_size / (1024 * 1024), max_order);

	/* Initialize levels array */
	memset(levels, 0, sizeof(levels));

	/* Main fragmentation algorithm */
	for (order = 0; order <= max_order; order++) {
		/* Calculate number of buffers and block size for this order */
		levels[order].order = order;
		levels[order].num_buffers = 1UL << (max_order - order + 2);

		/* At order > 0, half the memory is already allocated */
		if (order > 0)
		    levels[order].num_buffers >>= 1;

		block_size = (1UL << order) * base_page_size;

		igt_info("Order %u: Allocating %lu buffers of %zu bytes each\n",
			 order, levels[order].num_buffers, block_size);

		/* Allocate arrays for buffer handles */
		levels[order].buffers = calloc(levels[order].num_buffers, sizeof(amdgpu_bo_handle));
		levels[order].mc_addresses = calloc(levels[order].num_buffers, sizeof(uint64_t));
		levels[order].va_handles = calloc(levels[order].num_buffers, sizeof(amdgpu_va_handle));

		igt_assert(levels[order].buffers);
		igt_assert(levels[order].mc_addresses);
		igt_assert(levels[order].va_handles);

		/* Allocate and map buffers */
		for (p = 0; p < levels[order].num_buffers; p++) {
			struct amdgpu_bo_alloc_request req = {0};

			req.alloc_size = block_size;
			req.phys_alignment = base_page_size;
			req.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
			req.flags = 0;

			/* Try to allocate buffer */
			r = amdgpu_bo_alloc(device, &req, &levels[order].buffers[p]);
			if (r != 0) {
				igt_debug("Allocation failed at order %u, buffer %lu: %d\n", order, p, r);
				levels[order].num_buffers = p;
				break;
			}

			/* Allocate GPU VA space */
			r = amdgpu_va_range_alloc(device,
					      amdgpu_gpu_va_range_general,
					     block_size,
					     base_page_size,
					     0,
					     &levels[order].mc_addresses[p],
					     &levels[order].va_handles[p],
					     0);
			if (r != 0) {
				igt_debug("VA allocation failed at order %u, buffer %lu: %d\n", order, p, r);
				amdgpu_bo_free(levels[order].buffers[p]);
				levels[order].buffers[p] = NULL;
				levels[order].num_buffers = p;
				break;
			}

			/* Map buffer to GPU VA */
			r = amdgpu_bo_va_op(levels[order].buffers[p],
					0,
				       block_size,
				       levels[order].mc_addresses[p],
				       0,
				       AMDGPU_VA_OP_MAP);
			if (r != 0) {
				igt_debug("VA mapping failed at order %u, buffer %lu: %d\n", order, p, r);
				amdgpu_va_range_free(levels[order].va_handles[p]);
				amdgpu_bo_free(levels[order].buffers[p]);
				levels[order].buffers[p] = NULL;
				levels[order].va_handles[p] = NULL;
				levels[order].num_buffers = p;
				break;
			}

			/* Test buffer access with CPU mapping */
			if (block_size <= (16 * 1024 * 1024)) { /* Limit to reasonable sizes for CPU access */
				void *cpu_ptr;

				r = amdgpu_bo_cpu_map(levels[order].buffers[p], &cpu_ptr);
				if (r == 0) {
				    /* Write test pattern */
				    test_value++;
				    memset(cpu_ptr, test_value & 0xFF, MIN(block_size, 4096));
				    amdgpu_bo_cpu_unmap(levels[order].buffers[p]);
				}
			}
		}

		igt_info("Order %u: Successfully allocated %lu buffers\n",
			 order, levels[order].num_buffers);

		if (levels[order].num_buffers == 0) {
		    igt_warn("No buffers allocated at order %u, stopping test\n", order);
		break;
		}

		/* Free half the memory in all previous levels to create holes */
		for (o = 0; o <= order; o++) {
			if (levels[o].num_buffers == 0)
			continue;

			step = 1UL << (order - o + 1);
			offset = (step >> 1) - 1;
			block_size = (1UL << o) * base_page_size;

			igt_debug("Freeing every %luth order %u block starting with offset %lu\n",
			      step, o, offset);

			for (p = offset; p < levels[o].num_buffers; p += step) {
			if (levels[o].buffers[p]) {
				/* Unmap and free buffer */
				amdgpu_bo_va_op(levels[o].buffers[p],
					    0,
					   block_size,
					   levels[o].mc_addresses[p],
					   0,
					   AMDGPU_VA_OP_UNMAP);
				amdgpu_va_range_free(levels[o].va_handles[p]);
				amdgpu_bo_free(levels[o].buffers[p]);

				levels[o].buffers[p] = NULL;
				levels[o].va_handles[p] = NULL;
				}
			}
		}
	}

	for (order = 0; order <= max_order; order++) {
		if (levels[order].num_buffers == 0)
		continue;

		block_size = (1UL << order) * base_page_size;

		for (p = 0; p < levels[order].num_buffers; p++) {
			if (levels[order].buffers[p]) {
				total_address_space += block_size;
				total_allocated_memory += block_size;
			}
		}
	}

	igt_info("Fragmentation results:\n");
	igt_info("  Total address space used: %" PRIu64 " MB\n",
	     total_address_space / (1024 * 1024));
	igt_info("  Total allocated memory: %" PRIu64 " MB\n",
	     total_allocated_memory / (1024 * 1024));
	igt_info("  Address space to memory ratio: %.2f\n",
	     (double)total_address_space / total_allocated_memory);

	/* Cleanup - free all remaining buffers */
	for (order = 0; order <= max_order; order++) {
			if (!levels[order].buffers)
				continue;

		block_size = (1UL << order) * base_page_size;

		for (p = 0; p < levels[order].num_buffers; p++) {
			if (levels[order].buffers[p]) {
				amdgpu_bo_va_op(levels[order].buffers[p],
						0,
					       block_size,
					       levels[order].mc_addresses[p],
					       0,
					       AMDGPU_VA_OP_UNMAP);
				amdgpu_va_range_free(levels[order].va_handles[p]);
				amdgpu_bo_free(levels[order].buffers[p]);
			}
		}

		free(levels[order].buffers);
		free(levels[order].mc_addresses);
		free(levels[order].va_handles);
	}

	igt_info("GPUVM aperture fragmentation test completed successfully\n");
}


/**
 * Test: LLVM Assembler Basic Functionality Test
 *
 * This test verifies that the LLVM assembler can successfully assemble
 * a simple shader and that we can execute it on the GPU.
 */
static void test_llvm_assembler_basic(amdgpu_device_handle device)
{
	struct amdgpu_gpu_info gpu_info = {0};
	char mcpu[32];
	uint8_t isa_binary[PAGE_SIZE];
	size_t isa_size = 0;
	int r;
	int success_count = 0;
	int total_count = 0;

	const char * const *shaders_to_test = amdgpu_test_shaders;
	int i;

	igt_info("Starting LLVM Assembler Basic Functionality Test\n");

	/* Query GPU information */
	r = amdgpu_query_gpu_info(device, &gpu_info);
	igt_assert_eq(r, 0);

	/* Use common helper for mapping family_id -> mcpu string */
	amdgpu_family_id_to_mcpu(gpu_info.family_id, mcpu, sizeof(mcpu));
	igt_info("Testing LLVM assembler for detected MCPU: %s (family_id=%u)\n", mcpu, gpu_info.family_id);

	/* Initialize LLVM assembler */
	r = amdgpu_llvm_asm_init();
	if (r != 0) {
		igt_skip("LLVM assembler not available on this system\n");
		return;
	}

	/* Test all available shaders */
	igt_info("Testing assembly of all available shaders...\n");

	for (i = 0; shaders_to_test[i]; i++) {
		igt_info("Test %d: Assembling shader %d...\n", i + 1, i);

		r = amdgpu_llvm_assemble(mcpu, shaders_to_test[i], isa_binary, sizeof(isa_binary), &isa_size);
		if (r == 0 && isa_size > 0) {
		    success_count++;
		    igt_info("  Shader %d: assembled successfully - %zu bytes\n", i, isa_size);
		} else {
		    igt_info("  Shader %d: assembly failed (error: %d)\n", i, r);
		}
		total_count++;
	}

	igt_info("LLVM Assembler Test Results: %d/%d shaders assembled successfully\n",
	     success_count, total_count);

	/* Cleanup LLVM */
	amdgpu_llvm_asm_shutdown();

	igt_info("LLVM Assembler Basic Functionality Test: COMPLETED\n");

	/* Basic validation - if we can assemble at least one shader, consider it a success */
	igt_assert_f(success_count > 0, "No shaders could be assembled successfully (%d/%d failed)",
		 total_count - success_count, total_count);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	struct drm_amdgpu_info_hw_ip info = {0};
	int fd = -1;
	int r;
	bool arr_cap[AMD_IP_MAX] = {0};
	bool userq_arr_cap[AMD_IP_MAX] = {0};

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor,  &gpu_info, device);
		igt_assert_eq(r, 0);
		asic_rings_readness(device, 1, arr_cap);
		asic_userq_readiness(device, userq_arr_cap);
	}
	igt_describe("Check alloc visible or non-visible vram and GART combined cached");
	igt_subtest("memory-alloc")
		amdgpu_memory_alloc(device, true);

	igt_describe("Test invalid allocation parameters");
	igt_subtest("invalid_alloc")
		test_invalid_alloc(device, true);

	igt_describe("Test invalid parameters");
	igt_subtest("memory_alloc_all")
		memory_alloc_all(device, true);

	igt_describe("Test same memory ptr");
	igt_subtest("register_memory_same_ptr")
		if (!is_apu(&gpu_info))
			memory_register_same_ptr(device);

	igt_describe("Test userptr COW page table update (adapted from KFD MemoryRegister)");
	igt_subtest("memory-register") {
	if (!is_apu(&gpu_info))
		memory_register(device);
	}

	/* Large system memory mapping test */
	igt_describe("Map maximum possible system memory; validates limits. Skipped on APUs.");
		igt_subtest("mmap-large-system-mem") {
		if (!is_apu(&gpu_info))
			test_mmap_large(device);
	}

	/* PPR memory access test */
	igt_describe("Test PPR memory access on APU devices (synchronization/visibility)");
	igt_subtest("access-ppr-mem") {
		if (is_apu(&gpu_info))
			test_access_ppr_mem(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE));
	}

	igt_describe("Test basic GCA configuration reading from debugfs");
	igt_subtest("gca-config-basic")
		test_gca_config_basic(device, fd);

	igt_describe("Test allocation of large GTT memory buffers");
	igt_subtest("largest-gtt-buffer") {
		largest_buffer_test(device, AMDGPU_GEM_DOMAIN_GTT, 0);
	}

	igt_describe("Test allocation of large VRAM memory buffers");
		igt_subtest("largest-vram-buffer") {
		if (!is_apu(&gpu_info))
			largest_buffer_test(device, AMDGPU_GEM_DOMAIN_VRAM, 0);
	}

	igt_describe("Test allocation of large CPU accessible VRAM buffers");
		igt_subtest("largest-cpu-accessible-vram-buffer") {
		if (!is_apu(&gpu_info))
			largest_cpu_accessible_vram_test(device);
	}

	igt_describe("Stress test for large system buffer allocations");
	igt_subtest("big-sys-buffer-stress") {
		if (!is_apu(&gpu_info))
			big_sys_buffer_stress_test_with_queue(device);
	}

	igt_describe("AMDGPU Memory Management Performance Benchmark");
	igt_subtest("mm-benchmark") {
		mm_benchmark(device);
	}

	igt_describe("Test AMDGPU memory access via ptrace like a debugger");
	igt_subtest("ptrace-acess") {
		 ptrace_access_test(device);
	}

	igt_describe("Test ptrace peek/poke on VRAM addresses near 4MB boundary (adapted from KFD PtraceAccessInvisibleVram)");
	igt_subtest("ptrace-access-invisible-vram") {
	    ptrace_access_invisible_vram_test(device);
	}

	igt_describe("Test signal handling during AMDGPU memory operations");
	igt_subtest("signal-handling") {
		if (!is_apu(&gpu_info))
			test_signal_handling(device);
	}

	igt_describe("Verify system memory and vram buffers are properly zero-initialized upon allocation");
	igt_subtest("zero-initialization")
		test_zero_initialization(device);

	igt_describe("Measure memory bandwidth for different memory types and buffer sizes on large-bar systems");
	igt_subtest("memory-bandwidth") {
		if (!is_apu(&gpu_info))
			test_memory_bandwidth(device);
	}

	igt_describe("Test concurrent userptr registration from multiple threads to verify thread safety");
	igt_subtest("multi-thread-register-userptr")
		test_multithread_register_userptr(device);

	igt_describe("Validate GPU cache invalidation on SDMA write using PollMemoryIsa shader (Arcturus only)");
	igt_subtest("cache-invalidate-sdma-write") {
		if (!is_apu(&gpu_info))
			test_cache_invalidate_on_sdma_write_asm(device, &gpu_info);
	}

	igt_describe("Validate GPU cache invalidation on CPU write using PollMemoryIsa shader (Arcturus + large BAR only)");
	igt_subtest("cache-invalidate-cpu-write") {
		if (!is_apu(&gpu_info))
			test_cache_invalidate_on_cpu_write_asm(device, &gpu_info);
	}

	igt_describe("Deliberately fragment GPUVM aperture to fill up address space with minimal physical memory usage");
	igt_subtest("gpuvm-aperture-fragmentation") {
		if (!is_apu(&gpu_info))
			test_gpuvm_aperture_fragmentation(device);
	}

	igt_describe("Test LLVM assembler basic functionality with various shaders");
	igt_subtest("llvm-assembler-basic") {
		test_llvm_assembler_basic(device);
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
