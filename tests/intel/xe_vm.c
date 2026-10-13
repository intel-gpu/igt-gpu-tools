// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check if VMA functionality is working
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: VMA
 */

#include "igt.h"
#include "intel_pat.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>

#define USER_FENCE_VALUE 0xdeadbeefdeadbeefull
#define OVERCOMMIT_BO_SIZE SZ_64M

enum overcommit_stage {
	EXPECT_NONE,
	EXPECT_CREATE,
	EXPECT_BIND,
	EXPECT_EXEC,
};

static uint32_t
addr_low(uint64_t addr)
{
	return addr;
}

static uint32_t
addr_high(int fd, uint64_t addr)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint32_t leading_bits = 64 - va_bits;

	igt_assert_eq(addr >> va_bits, 0);
	return (int64_t)(addr << leading_bits) >> (32 + leading_bits);
}

static uint32_t
hash_addr(uint64_t addr)
{
	return (addr * 7229) ^ ((addr >> 32) * 5741);
}

static void
write_dwords(int fd, uint32_t vm, int n_dwords, uint64_t *addrs)
{
	uint32_t batch_size, batch_bo, *batch_map, exec_queue;
	uint64_t batch_addr = 0x1a0000;
	int i, b = 0;

	batch_size = (n_dwords * 4 + 1) * sizeof(uint32_t);
	batch_size = xe_bb_size(fd, batch_size);
	batch_bo = xe_bo_create(fd, vm, batch_size,
				vram_if_possible(fd, 0),
				DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	batch_map = xe_bo_map(fd, batch_bo, batch_size);

	for (i = 0; i < n_dwords; i++) {
		/* None of the addresses can land in our batch */
		igt_assert(addrs[i] + sizeof(uint32_t) <= batch_addr ||
			   batch_addr + batch_size <= addrs[i]);

		batch_map[b++] = MI_STORE_DWORD_IMM_GEN4;
		batch_map[b++] = addr_low(addrs[i]);
		batch_map[b++] = addr_high(fd, addrs[i]);
		batch_map[b++] = hash_addr(addrs[i]);

	}
	batch_map[b++] = MI_BATCH_BUFFER_END;
	igt_assert_lte(&batch_map[b] - batch_map, batch_size);
	munmap(batch_map, batch_size);

	xe_vm_bind_sync(fd, vm, batch_bo, 0, batch_addr, batch_size);
	exec_queue = xe_exec_queue_create_class(fd, vm, DRM_XE_ENGINE_CLASS_COPY);
	xe_exec_wait(fd, exec_queue, batch_addr);
	xe_vm_unbind_sync(fd, vm, 0, batch_addr, batch_size);

	gem_close(fd, batch_bo);
	xe_exec_queue_destroy(fd, exec_queue);
}

/**
 * SUBTEST: scratch
 * Functionality: scratch page
 * Description: Test scratch page creation and write
 * Test category: functionality test
 */

static void
test_scratch(int fd)
{
	uint32_t vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, 0);
	uint64_t addrs[] = {
		0x000000000000ull,
		0x7ffdb86402d8ull,
		0x7ffffffffffcull,
		0x800000000000ull,
		0x3ffdb86402d8ull,
		0xfffffffffffcull,
	};

	write_dwords(fd, vm, ARRAY_SIZE(addrs), addrs);

	xe_vm_destroy(fd, vm);
}

static void
__test_bind_one_bo(int fd, uint32_t vm, int n_addrs, uint64_t *addrs)
{
	uint32_t bo, bo_size = xe_get_default_alignment(fd);
	uint32_t *vms;
	void *map;
	int i;

	if (!vm) {
		vms = malloc(sizeof(*vms) * n_addrs);
		igt_assert(vms);
	}
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	map = xe_bo_map(fd, bo, bo_size);
	memset(map, 0, bo_size);

	for (i = 0; i < n_addrs; i++) {
		uint64_t bind_addr = addrs[i] & ~(uint64_t)(bo_size - 1);

		if (!vm)
			vms[i] = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE,
					      0);
		igt_debug("Binding addr %"PRIx64"\n", addrs[i]);
		xe_vm_bind_sync(fd, vm ? vm : vms[i], bo, 0,
				bind_addr, bo_size);
	}

	if (vm)
		write_dwords(fd, vm, n_addrs, addrs);
	else
		for (i = 0; i < n_addrs; i++)
			write_dwords(fd, vms[i], 1, addrs + i);

	for (i = 0; i < n_addrs; i++) {
		uint32_t *dw = map + (addrs[i] & (bo_size - 1));
		uint64_t bind_addr = addrs[i] & ~(uint64_t)(bo_size - 1);

		igt_debug("Testing addr %"PRIx64"\n", addrs[i]);
		igt_assert_eq(*dw, hash_addr(addrs[i]));

		xe_vm_unbind_sync(fd, vm ? vm : vms[i], 0,
				  bind_addr, bo_size);

		/* clear dw, to ensure same execbuf after unbind fails to write */
		*dw = 0;
	}

	if (vm)
		write_dwords(fd, vm, n_addrs, addrs);
	else
		for (i = 0; i < n_addrs; i++)
			write_dwords(fd, vms[i], 1, addrs + i);

	for (i = 0; i < n_addrs; i++) {
		uint32_t *dw = map + (addrs[i] & (bo_size - 1));

		igt_debug("Testing unbound addr %"PRIx64"\n", addrs[i]);
		igt_assert_eq(*dw, 0);
	}

	munmap(map, bo_size);

	gem_close(fd, bo);
	if (vm) {
		xe_vm_destroy(fd, vm);
	} else {
		for (i = 0; i < n_addrs; i++)
			xe_vm_destroy(fd, vms[i]);
		free(vms);
	}
}

uint64_t addrs_48b[] = {
	0x000000000000ull,
	0x0000b86402d4ull,
	0x0001b86402d8ull,
	0x7ffdb86402dcull,
	0x7fffffffffecull,
	0x800000000004ull,
	0x3ffdb86402e8ull,
	0xfffffffffffcull,
};

uint64_t addrs_57b[] = {
	0x000000000000ull,
	0x0000b86402d4ull,
	0x0001b86402d8ull,
	0x7ffdb86402dcull,
	0x7fffffffffecull,
	0x800000000004ull,
	0x3ffdb86402e8ull,
	0xfffffffffffcull,
	0x100000000000008ull,
	0xfffffdb86402e0ull,
	0x1fffffffffffff4ull,
};

/**
 * SUBTEST: bind-once
 * Functionality: bind BO
 * Description: bind once on one BO
 * Test category: functionality test
 */

static void
test_bind_once(int fd)
{
	uint64_t addr = 0x7ffdb86402d8ull;

	__test_bind_one_bo(fd,
			   xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, 0),
			   1, &addr);
}

/**
 * SUBTEST: bind-one-bo-many-times
 * Functionality: bind BO
 * Description: bind many times on one BO
 * Test category: functionality test
 */

static void
test_bind_one_bo_many_times(int fd)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint64_t *addrs = (va_bits == 57) ? addrs_57b : addrs_48b;
	uint64_t addrs_size = (va_bits == 57) ? ARRAY_SIZE(addrs_57b) :
						ARRAY_SIZE(addrs_48b);

	__test_bind_one_bo(fd,
			   xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, 0),
			   addrs_size, addrs);
}

/**
 * SUBTEST: bind-one-bo-many-times-many-vm
 * Functionality: bind BO
 * Description: Test bind many times and many VM on one BO
 * Test category: functionality test
 */

static void
test_bind_one_bo_many_times_many_vm(int fd)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint64_t *addrs = (va_bits == 57) ? addrs_57b : addrs_48b;
	uint64_t addrs_size = (va_bits == 57) ? ARRAY_SIZE(addrs_57b) :
						ARRAY_SIZE(addrs_48b);

	__test_bind_one_bo(fd, 0, addrs_size, addrs);
}

/**
 * SUBTEST: partial-unbinds
 * Functionality: unbind
 * Description: Test partial unbinds
 * Test category: functionality test
 */

static void test_partial_unbinds(int fd)
{
	uint32_t vm = xe_vm_create(fd, 0, 0);
	size_t bo_size = 3 * xe_get_default_alignment(fd);
	uint32_t bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0), 0);
	uint64_t unbind_size = bo_size / 3;
	uint64_t addr = 0x1a0000;

	struct drm_xe_sync sync = {
	    .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
	    .handle = syncobj_create(fd, 0),
	};

	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync.handle, 1);
	xe_vm_unbind_async(fd, vm, 0, 0, addr + unbind_size, unbind_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync.handle, 1);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, unbind_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync.handle, 1);
	xe_vm_unbind_async(fd, vm, 0, 0, addr + 2 * unbind_size, unbind_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync.handle);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: unbind-all-%d-vmas
 * Functionality: unbind
 * Description: Test unbind all with %arg[1] VMAs
 * Test category: functionality test
 *
 * arg[1].values: 2, 8
 */

static void unbind_all(int fd, int n_vmas)
{
	uint32_t bo, bo_size = xe_get_default_alignment(fd);
	uint64_t addr = 0x1a0000;
	uint32_t vm;
	int i;
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0), 0);

	for (i = 0; i < n_vmas; ++i)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr + i * bo_size,
				 bo_size, NULL, 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_unbind_all_async(fd, vm, 0, bo, sync, 1);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync[0].handle);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define	MAP_ADDRESS	0x00007fadeadbe000

/**
 * SUBTEST: userptr-invalid
 * Functionality: userptr
 * Description:
 *	Verifies that mapping an invalid userptr returns -EFAULT,
 *	and that it is correctly handled.
 * Test category: negative test
 */
static void userptr_invalid(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	uint32_t vm;
	void *data;
	int ret;

	data = mmap((void *)MAP_ADDRESS, size, PROT_READ |
		    PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
	igt_assert(data != MAP_FAILED);

	vm = xe_vm_create(fd, 0, 0);
	munmap(data, size);
	ret = __xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
			   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
			   DEFAULT_PAT_INDEX, 0);
	igt_assert(ret == -EFAULT);

	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: compact-64k-pages
 * Functionality: bind
 * Description:
 *	Take corner cases related to compact and 64k pages
 * Test category: functionality test
 */
static void compact_64k_pages(int fd, struct drm_xe_engine_class_instance *eci)
{
	size_t page_size = xe_get_default_alignment(fd);
	uint64_t addr0 = 0x10000000ull, addr1;
	uint32_t vm;
	uint32_t bo0, bo1;
	uint32_t exec_queue;
	void *ptr0, *ptr1;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data = NULL;
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t batch_offset;
	uint64_t batch_addr;
	uint64_t sdi_offset;
	uint64_t sdi_addr;
	int b = 0;

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	bo0 = xe_bo_create(fd, vm, SZ_8M,
			   vram_if_possible(fd, eci->gt_id),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	ptr0 = xe_bo_map(fd, bo0, SZ_8M);

	bo1 = xe_bo_create(fd, vm, SZ_8M / 2,
			   vram_if_possible(fd, eci->gt_id),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	ptr1 = xe_bo_map(fd, bo1, SZ_8M / 2);

	sync[0].handle = syncobj_create(fd, 0);
	if (page_size == SZ_4K) {
		/* Setup mapping to split a 64k PTE in cache */
		xe_vm_bind_async(fd, vm, 0, bo0, 0, addr0, SZ_64K, 0, 0);

		addr1 = addr0 + (SZ_64K / 2);
		xe_vm_bind_async(fd, vm, 0, bo1, 0, addr1, SZ_64K / 2,
				 sync, 1);
	} else if (page_size == SZ_64K) {
		addr0 += page_size;

		/* Setup mapping to split compact 64k pages */
		xe_vm_bind_async(fd, vm, 0, bo0, 0, addr0, SZ_8M, 0, 0);

		addr1 = addr0 + (SZ_8M / 4);
		xe_vm_bind_async(fd, vm, 0, bo1, 0, addr1, SZ_8M / 2,
				 sync, 1);
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	/* Verify 1st and 2nd mappings working */
	batch_offset = (char *)&data[0].batch - (char *)data;
	batch_addr = addr0 + batch_offset;
	sdi_offset = (char *)&data[0].data - (char *)data;
	sdi_addr = addr0 + sdi_offset;
	data = ptr0;

	data[0].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	data[0].batch[b++] = sdi_addr;
	data[0].batch[b++] = sdi_addr >> 32;
	data[0].batch[b++] = 0xc0ffee;

	sdi_addr = addr1 + sdi_offset;
	data[0].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	data[0].batch[b++] = sdi_addr;
	data[0].batch[b++] = sdi_addr >> 32;
	data[0].batch[b++] = 0xc0ffee;

	data[0].batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(data[0].batch));

	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].handle = syncobj_create(fd, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data[0].data, 0xc0ffee);
	data = ptr1;
	igt_assert_eq(data[0].data, 0xc0ffee);

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	syncobj_reset(fd, &sync[0].handle, 1);
	xe_vm_unbind_all_async(fd, vm, 0, bo0, 0, 0);
	xe_vm_unbind_all_async(fd, vm, 0, bo1, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	xe_exec_queue_destroy(fd, exec_queue);
	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	munmap(ptr0, SZ_8M);
	munmap(ptr1, SZ_8M / 2);
	gem_close(fd, bo0);
	gem_close(fd, bo1);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: shared-%s-page
 * Description: Test shared arg[1] page
 * Test category: functionality test
 *
 * Functionality: %arg[1] page
 * arg[1].values: pte, pde, pde2, pde3
 */


struct shared_pte_page_data {
	uint32_t batch[16];
	uint64_t pad;
	uint32_t data;
};

#define MAX_N_EXEC_QUEUES 4

static void
shared_pte_page(int fd, struct drm_xe_engine_class_instance *eci, int n_bo,
		uint64_t addr_stride)
{
	uint32_t vm;
	uint64_t addr = 0x1000 * 512;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_EXEC_QUEUES + 1];
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	size_t bo_size;
	uint32_t *bo;
	struct shared_pte_page_data **data;
	int n_exec_queues = n_bo, n_execs = n_bo;
	int i, b;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	bo = malloc(sizeof(*bo) * n_bo);
	igt_assert(bo);

	data = malloc(sizeof(*data) * n_bo);
	igt_assert(data);

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(struct shared_pte_page_data);
	bo_size = xe_bb_size(fd, bo_size);

	if (addr_stride <= bo_size)
		addr_stride = addr_stride + bo_size;

	for (i = 0; i < n_bo; ++i) {
		bo[i] = xe_bo_create(fd, vm, bo_size,
				     vram_if_possible(fd, eci->gt_id),
				     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data[i] = xe_bo_map(fd, bo[i], bo_size);
	}

	memset(sync_all, 0, sizeof(sync_all));
	for (i = 0; i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
		sync_all[i].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};

	sync[0].handle = syncobj_create(fd, 0);
	for (i = 0; i < n_bo; ++i)
		xe_vm_bind_async(fd, vm, 0, bo[i], 0, addr + i * addr_stride,
				 bo_size, sync, i == n_bo - 1 ? 1 : 0);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + i * addr_stride + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + i * addr_stride + sdi_offset;
		int e = i % n_exec_queues;

		b = 0;
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_bo; ++i) {
		if (i % 2)
			continue;

		sync_all[n_execs].flags = DRM_XE_SYNC_FLAG_SIGNAL;
		sync_all[n_execs].handle = sync[0].handle;
		xe_vm_unbind_async(fd, vm, 0, 0, addr + i * addr_stride,
				   bo_size, sync_all, n_execs + 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0,
					NULL));
	}

	for (i = 0; i < n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i]->data, 0xc0ffee);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + i * addr_stride + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + i * addr_stride + sdi_offset;
		int e = i % n_exec_queues;

		if (!(i % 2))
			continue;

		b = 0;
		memset(data[i], 0, sizeof(struct shared_pte_page_data));
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_bo; ++i) {
		if (!(i % 2))
			continue;

		sync_all[n_execs].flags = DRM_XE_SYNC_FLAG_SIGNAL;
		sync_all[n_execs].handle = sync[0].handle;
		xe_vm_unbind_async(fd, vm, 0, 0, addr + i * addr_stride,
				   bo_size, sync_all, n_execs + 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0,
					NULL));
	}

	for (i = 0; i < n_execs; i++) {
		if (!(i % 2))
			continue;
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i]->data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	for (i = 0; i < n_bo; ++i) {
		munmap(data[i], bo_size);
		gem_close(fd, bo[i]);
	}
	free(data);
	xe_vm_destroy(fd, vm);
}


/**
 * SUBTEST: bind-execqueues-independent
 * Functionality: bind exec_queues
 * Description: Test independent bind exec_queues
 * Test category: functionality test
 *
 * SUBTEST: bind-execqueues-conflict
 * Functionality: bind exec_queues
 * Description: Test conflict bind exec_queues
 * Test category: functionality test
 */

#define CONFLICT	(0x1 << 0)

static void
test_bind_execqueues_independent(int fd, struct drm_xe_engine_class_instance *eci,
			      unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
#define N_EXEC_QUEUES	2
	uint32_t exec_queues[N_EXEC_QUEUES];
	uint32_t bind_exec_queues[N_EXEC_QUEUES];
	uint32_t syncobjs[N_EXEC_QUEUES + 1];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = true };
	int i, b;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data) * N_EXEC_QUEUES;
	bo_size = xe_bb_size(fd, bo_size);
	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < N_EXEC_QUEUES; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		bind_exec_queues[i] = xe_bind_exec_queue_create(fd, vm, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	}
	syncobjs[N_EXEC_QUEUES] = syncobj_create(fd, 0);

	/* Initial bind, needed for spinner */
	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, bind_exec_queues[0], bo, 0, addr, bo_size,
			 sync, 1);

	for (i = 0; i < N_EXEC_QUEUES; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		int e = i;

		if (i == 0) {
			/* Cork 1st exec_queue with a spinner */
			spin_opts.addr = addr + spin_offset;
			xe_spin_init(&data[i].spin, &spin_opts);
			exec.exec_queue_id = exec_queues[e];
			exec.address = spin_opts.addr;
			sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].handle = syncobjs[e];
			xe_exec(fd, &exec);
			xe_spin_wait_started(&data[i].spin);

			/* Do bind to 1st exec_queue blocked on cork */
			addr += (flags & CONFLICT) ? (0x1 << 21) : bo_size;
			sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].handle = syncobjs[e];
			xe_vm_bind_async(fd, vm, bind_exec_queues[e], bo, 0, addr,
					 bo_size, sync + 1, 1);
			addr += bo_size;
		} else {
			/* Do bind to 2nd exec_queue which blocks write below */
			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			xe_vm_bind_async(fd, vm, bind_exec_queues[e], bo, 0, addr,
					 bo_size, sync, 1);
		}

		/*
		 * Write to either exec_queue, 1st blocked on spinner + bind, 2nd
		 * just blocked on bind. The 2nd should make independent
		 * progress.
		 */
		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[!i ? N_EXEC_QUEUES : e];

		exec.num_syncs = 2;
		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);
	}

	if (!(flags & CONFLICT)) {
		/* Verify initial bind, bind + write to 2nd exec_queue done */
		igt_assert(syncobj_wait(fd, &syncobjs[1], 1, INT64_MAX, 0,
					NULL));
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0,
					NULL));
		igt_assert_eq(data[1].data, 0xc0ffee);
	} else {
		/* Let jobs runs for a bit */
		usleep(100000);
		/* bind + write to 2nd exec_queue waiting */
		igt_assert(!syncobj_wait(fd, &syncobjs[1], 1, 1, 0, NULL));
		igt_assert(!syncobj_wait(fd, &sync[0].handle, 1, 0, 0, NULL));
	}

	/* Verify bind + write to 1st exec_queue still inflight */
	igt_assert(!syncobj_wait(fd, &syncobjs[0], 1, 1, 0, NULL));
	igt_assert(!syncobj_wait(fd, &syncobjs[N_EXEC_QUEUES], 1, 1, 0, NULL));

	/* Verify bind + write to 1st exec_queue done after ending spinner */
	xe_spin_end(&data[0].spin);
	igt_assert(syncobj_wait(fd, &syncobjs[0], 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &syncobjs[N_EXEC_QUEUES], 1, INT64_MAX, 0,
				NULL));
	igt_assert_eq(data[0].data, 0xc0ffee);

	if (flags & CONFLICT) {
		/* Verify bind + write to 2nd exec_queue done */
		igt_assert(syncobj_wait(fd, &syncobjs[1], 1, INT64_MAX, 0,
					NULL));
		igt_assert_eq(data[1].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
	sync[0].handle = syncobj_create(fd, 0);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_all_async(fd, vm, 0, bo, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < N_EXEC_QUEUES; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
		xe_exec_queue_destroy(fd, bind_exec_queues[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void xe_vm_bind_array_err(int fd, uint32_t vm, uint32_t exec_queue,
				 struct drm_xe_vm_bind_op *bind_ops,
				 uint32_t num_bind, struct drm_xe_sync *sync,
				 uint32_t num_syncs, int err)
{
	struct drm_xe_vm_bind bind = {
		.vm_id = vm,
		.num_binds = num_bind,
		.vector_of_binds = (uintptr_t)bind_ops,
		.num_syncs = num_syncs,
		.syncs = (uintptr_t)sync,
		.exec_queue_id = exec_queue,
	};

	igt_assert(num_bind > 1);
	do_ioctl_err(fd, DRM_IOCTL_XE_VM_BIND, &bind, err);
}

#define BIND_ARRAY_BIND_EXEC_QUEUE_FLAG	(0x1 << 0)
#define BIND_ARRAY_ENOBUFS_FLAG		(0x1 << 1)

/**
 * SUBTEST: bind-array-twice
 * Functionality: bind exec_queues
 * Description: Test bind array twice
 * Test category: functionality test
 *
 * SUBTEST: bind-array-many
 * Functionality: bind exec_queues
 * Description: Test bind array many times
 * Test category: functionality test
 *
 * SUBTEST: bind-array-enobufs
 * Functionality: bind exec_queues
 * Description: Test bind array that is deliberately oversized to intentionally trigger an -ENOBUFs error
 * Test category: functionality test
 *
 * SUBTEST: bind-array-exec_queue-twice
 * Functionality: bind exec_queues
 * Description: Test bind array exec_queue twice
 * Test category: functionality test
 *
 * SUBTEST: bind-array-exec_queue-many
 * Functionality: bind exec_queues
 * Description: Test bind array exec_queue many times
 * Test category: functionality test
 */
static void
test_bind_array(int fd, struct drm_xe_engine_class_instance *eci, int n_execs,
		uint64_t addr, size_t bo_size, unsigned int flags)
{
	uint32_t vm;
	uint64_t base_addr = addr;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queue, bind_exec_queue = 0;
	struct drm_xe_vm_bind_op *bind_ops;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	bind_ops = calloc(n_execs, sizeof(*bind_ops));
	igt_assert(bind_ops);

	vm = xe_vm_create(fd, 0, 0);
	bo_size = bo_size ?: sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	if (flags & BIND_ARRAY_BIND_EXEC_QUEUE_FLAG)
		bind_exec_queue = xe_bind_exec_queue_create(fd, vm, 0);
	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	for (i = 0; i < n_execs; ++i) {
		bind_ops[i].obj = bo;
		bind_ops[i].range = bo_size;
		bind_ops[i].addr = addr;
		bind_ops[i].op = DRM_XE_VM_BIND_OP_MAP;
		bind_ops[i].pat_index = intel_get_pat_idx_wb(fd);

		addr += bo_size;
	}

	sync[0].handle = syncobj_create(fd, 0);
	if (flags & BIND_ARRAY_ENOBUFS_FLAG) {
		struct xe_cork *cork;
		uint32_t vm_cork;

		vm_cork = xe_vm_create(fd, 0, 0);
		cork = xe_cork_create_opts(fd, eci, vm_cork, 1, 1);
		xe_cork_sync_start(fd, cork);

		sync[1].handle = cork->sync[1].handle;
		sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;

		xe_vm_bind_array_err(fd, vm, bind_exec_queue, bind_ops,
				     n_execs, sync, 2, ENOBUFS);
		/* destroy queue before sampling again */
		xe_cork_sync_end(fd, cork);
		xe_cork_destroy(fd, cork);
		xe_vm_destroy(fd, vm_cork);

		n_execs = n_execs / 4;
	}

	xe_vm_bind_array(fd, vm, bind_exec_queue, bind_ops, n_execs, sync, 1);

	addr = base_addr;
	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		if (i == n_execs - 1) {
			sync[1].handle = syncobj_create(fd, 0);
			exec.num_syncs = 2;
		} else {
			exec.num_syncs = 1;
		}

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += bo_size;
	}

	for (i = 0; i < n_execs; ++i) {
		bind_ops[i].obj = 0;
		bind_ops[i].op = DRM_XE_VM_BIND_OP_UNMAP;
		bind_ops[i].flags = 0;
	}

	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_bind_array(fd, vm, bind_exec_queue, bind_ops, n_execs, sync, 2);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_exec_queue_destroy(fd, exec_queue);
	if (bind_exec_queue)
		xe_exec_queue_destroy(fd, bind_exec_queue);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
	free(bind_ops);
}

/**
 * SUBTEST: bind-array-conflict
 * Functionality: bind exec_queues and page table updates
 * Description: Test bind array with conflicting address
 * Test category: functionality test
 *
 * SUBTEST: bind-no-array-conflict
 * Functionality: bind and page table updates
 * Description: Test binding with conflicting address
 * Test category: functionality test
 *
 * SUBTEST: bind-array-conflict-error-inject
 * Functionality: bind exec_queues and page table updates error paths
 * Description: Test bind array with conflicting address plus error injection
 * Test category: functionality test
 */
static void
test_bind_array_conflict(int fd, struct drm_xe_engine_class_instance *eci,
			 bool no_array, bool error_inject)
{
	uint32_t vm;
	uint64_t addr = 0x1a00000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queue;
#define BIND_ARRAY_CONFLICT_NUM_BINDS	4
	struct drm_xe_vm_bind_op bind_ops[BIND_ARRAY_CONFLICT_NUM_BINDS] = { };
#define ONE_MB	0x100000
	size_t bo_size = 8 * ONE_MB;
	uint32_t bo = 0, bo2 = 0;
	void *map, *map2 = NULL;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data = NULL;
	const struct binds {
		uint64_t size;
		uint64_t offset;
		uint32_t op;
	} bind_args[] = {
		{ ONE_MB, 0, DRM_XE_VM_BIND_OP_MAP },
		{ 2 * ONE_MB, ONE_MB, DRM_XE_VM_BIND_OP_MAP },
		{ 3 * ONE_MB, 3 * ONE_MB, DRM_XE_VM_BIND_OP_MAP },
		{ 4 * ONE_MB, ONE_MB, DRM_XE_VM_BIND_OP_UNMAP },
	};
	const struct execs {
		uint64_t offset;
	} exec_args[] = {
		{ 0 },
		{ ONE_MB / 2 },
		{ ONE_MB / 4 },
		{ 5 * ONE_MB },
	};
	int i, b, n_execs = 4;

	vm = xe_vm_create(fd, 0, 0);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	map = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	/* Map some memory that will be over written */
	if (error_inject) {
		bo2 = xe_bo_create(fd, vm, bo_size,
				   vram_if_possible(fd, eci->gt_id),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		map2 = xe_bo_map(fd, bo2, bo_size);

		i = 0;
		sync[0].handle = syncobj_create(fd, 0);
		xe_vm_bind_async(fd, vm, 0, bo2, bind_args[i].offset,
				 addr + bind_args[i].offset, bind_args[i].size,
				 sync, 1);
		{
			uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
			uint64_t batch_addr = addr + exec_args[i].offset + batch_offset;
			uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
			uint64_t sdi_addr = addr + exec_args[i].offset + sdi_offset;
			data = map2 + exec_args[i].offset;

			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			sync[1].handle = syncobj_create(fd, 0);
			exec.num_syncs = 2;

			exec.exec_queue_id = exec_queue;
			exec.address = batch_addr;
			xe_exec(fd, &exec);
		}

		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
		igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

		syncobj_reset(fd, &sync[0].handle, 1);
		syncobj_destroy(fd, sync[1].handle);

		data = map2 + exec_args[i].offset;
		igt_assert_eq(data[i].data, 0xc0ffee);
	}

	if (no_array) {
		sync[0].handle = syncobj_create(fd, 0);
		for (i = 0; i < BIND_ARRAY_CONFLICT_NUM_BINDS; ++i) {
			if (bind_args[i].op == DRM_XE_VM_BIND_OP_MAP)
				xe_vm_bind_async(fd, vm, 0, bo,
						 bind_args[i].offset,
						 addr + bind_args[i].offset,
						 bind_args[i].size,
						 sync, 1);
			else
				xe_vm_unbind_async(fd, vm, 0,
						   bind_args[i].offset,
						   addr + bind_args[i].offset,
						   bind_args[i].size,
						   sync, 1);
		}
	} else {
		for (i = 0; i < BIND_ARRAY_CONFLICT_NUM_BINDS; ++i) {
			bind_ops[i].obj = bind_args[i].op == DRM_XE_VM_BIND_OP_MAP ?
				bo : 0;
			bind_ops[i].obj_offset = bind_args[i].offset;
			bind_ops[i].range = bind_args[i].size;
			bind_ops[i].addr = addr + bind_args[i].offset;
			bind_ops[i].op = bind_args[i].op;
			bind_ops[i].flags = 0;
			bind_ops[i].prefetch_mem_region_instance = 0;
			bind_ops[i].pat_index = intel_get_pat_idx_wb(fd);
			bind_ops[i].reserved[0] = 0;
			bind_ops[i].reserved[1] = 0;
		}

		if (error_inject) {
			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			bind_ops[BIND_ARRAY_CONFLICT_NUM_BINDS - 1].flags |=
				0x1 << 31;
			xe_vm_bind_array_err(fd, vm, 0, bind_ops,
					     BIND_ARRAY_CONFLICT_NUM_BINDS,
					     sync, 1, ENOSPC);
			bind_ops[BIND_ARRAY_CONFLICT_NUM_BINDS - 1].flags &=
				~(0x1 << 31);

			/* Verify existing mappings still works */
			i = 1;
			{
				uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
				uint64_t batch_addr = addr + exec_args[i].offset + batch_offset;
				uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
				uint64_t sdi_addr = addr + exec_args[i].offset + sdi_offset;
				data = map2 + exec_args[i].offset;

				b = 0;
				data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
				data[i].batch[b++] = sdi_addr;
				data[i].batch[b++] = sdi_addr >> 32;
				data[i].batch[b++] = 0xc0ffee;
				data[i].batch[b++] = MI_BATCH_BUFFER_END;
				igt_assert(b <= ARRAY_SIZE(data[i].batch));

				sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
				sync[1].handle = syncobj_create(fd, 0);
				exec.num_syncs = 2;

				exec.exec_queue_id = exec_queue;
				exec.address = batch_addr;
				xe_exec(fd, &exec);
			}

			igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
			igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

			syncobj_destroy(fd, sync[0].handle);
			syncobj_destroy(fd, sync[1].handle);

			data = map2 + exec_args[i].offset;
			igt_assert_eq(data[i].data, 0xc0ffee);
			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		}
		sync[0].handle = syncobj_create(fd, 0);
		xe_vm_bind_array(fd, vm, 0, bind_ops,
				 BIND_ARRAY_CONFLICT_NUM_BINDS, sync, 1);
	}

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + exec_args[i].offset + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + exec_args[i].offset + sdi_offset;
		data = map + exec_args[i].offset;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		if (i == n_execs - 1) {
			sync[1].handle = syncobj_create(fd, 0);
			exec.num_syncs = 2;
		} else {
			exec.num_syncs = 1;
		}

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);
	}

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++) {
		data = map + exec_args[i].offset;
		igt_assert_eq(data[i].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_exec_queue_destroy(fd, exec_queue);

	munmap(map, bo_size);
	if (map2)
		munmap(map2, bo_size);
	gem_close(fd, bo);
	if (bo2)
		gem_close(fd, bo2);
	xe_vm_destroy(fd, vm);
}


#define LARGE_BIND_FLAG_MISALIGNED	(0x1 << 0)
#define LARGE_BIND_FLAG_SPLIT		(0x1 << 1)
#define LARGE_BIND_FLAG_USERPTR		(0x1 << 2)

/**
 * SUBTEST: %s-%ld
 * Functionality: bind
 * Description: Test %arg[1] with %arg[2] bind size
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @large-binds: large-binds
 * @large-split-binds: large-split-binds
 * @large-misaligned-binds: large-misaligned-binds
 * @large-split-misaligned-binds: large-split-misaligned-binds
 *
 * arg[2].values: 2097152, 4194304, 8388608, 16777216, 33554432
 * arg[2].values: 67108864, 134217728, 268435456, 536870912, 1073741824
 * arg[2].values: 2147483648
 */

/**
 * SUBTEST: %s-%ld
 * Functionality: userptr bind
 * Description: Test %arg[1] with %arg[2] bind size
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @large-userptr-binds: large-userptr-binds
 * @large-userptr-split-binds: large-userptr-split-binds
 * @large-userptr-misaligned-binds: large-userptr-misaligned-binds
 * @large-userptr-split-misaligned-binds: large-userptr-split-misaligned-binds
 *
 * arg[2].values: 2097152, 4194304, 8388608, 16777216, 33554432
 * arg[2].values: 67108864, 134217728, 268435456, 536870912, 1073741824
 * arg[2].values: 2147483648
 */

/**
 *
 * SUBTEST: %s-%ld
 * Functionality: mixed bind
 * Description: Test %arg[1] with %arg[2] bind size
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @mixed-binds: mixed-binds
 * @mixed-misaligned-binds: mixed-misaligned-binds
 *
 * arg[2].values: 3145728, 1611661312
 */

/**
 *
 * SUBTEST: %s-%ld
 * Functionality: mixed bind
 * Description: Test %arg[1] with %arg[2] bind size
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @mixed-userptr-binds: mixed-userptr-binds
 * @mixed-userptr-misaligned-binds: mixed-userptr-misaligned-binds
 * @mixed-userptr-binds: mixed-userptr-binds
 *
 * arg[2].values: 3145728, 1611661312
 */

static void
test_large_binds(int fd, struct drm_xe_engine_class_instance *eci,
		 int n_exec_queues, int n_execs, size_t bo_size,
		 unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	size_t bo_size_prefetch, padding;
	uint64_t addr = 0x1ull << 30, base_addr = 0x1ull << 30;
	uint32_t vm;
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	uint32_t bo = 0;
	void *map;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	if (flags & LARGE_BIND_FLAG_MISALIGNED) {
		addr -= xe_get_default_alignment(fd);
		base_addr -= xe_get_default_alignment(fd);
	}

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);
	vm = xe_vm_create(fd, 0, 0);

	bo_size_prefetch = xe_bb_size(fd, bo_size);

	if (flags & LARGE_BIND_FLAG_USERPTR) {
		map = aligned_alloc(xe_get_default_alignment(fd), bo_size_prefetch);
		igt_assert(map);
	} else {
		igt_skip_on(xe_visible_vram_size(fd, 0) && bo_size_prefetch >
			    xe_visible_vram_size(fd, 0));

		bo = xe_bo_create(fd, vm, bo_size_prefetch,
				  vram_if_possible(fd, eci->gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		map = xe_bo_map(fd, bo, bo_size);
	}
	padding = bo_size_prefetch - bo_size;

	for (i = 0; i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	if (flags & LARGE_BIND_FLAG_USERPTR) {
		if (flags & LARGE_BIND_FLAG_SPLIT) {
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map),
						 addr, bo_size / 2, NULL, 0);
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map) + bo_size / 2,
						 addr + bo_size / 2, bo_size / 2 + padding,
						 sync, 1);
		} else {
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map),
						 addr, bo_size + padding, sync, 1);
		}
	} else {
		if (flags & LARGE_BIND_FLAG_SPLIT) {
			xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size / 2, NULL, 0);
			xe_vm_bind_async(fd, vm, 0, bo, bo_size / 2, addr + bo_size / 2,
					 bo_size / 2 + padding, sync, 1);
		} else {
			xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size + padding, sync, 1);
		}
	}

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_exec_queues;

		data = map + (addr - base_addr);
		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		if (i != e)
			syncobj_reset(fd, &sync[1].handle, 1);

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		if (i + 1 != n_execs)
			addr += bo_size / n_execs;
		else
			addr = base_addr + bo_size - 0x1000;
	}

	for (i = 0; i < n_exec_queues; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	if (flags & LARGE_BIND_FLAG_SPLIT) {
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr,
				   bo_size / 2, NULL, 0);
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr + bo_size / 2,
				   bo_size / 2 + padding, sync, 1);
	} else {
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr, bo_size + padding,
				   sync, 1);
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	addr = base_addr;
	for (i = 0; i < n_execs; i++) {
		data = map + (addr - base_addr);
		igt_assert_eq(data[i].data, 0xc0ffee);

		if (i + 1 != n_execs)
			addr += bo_size / n_execs;
		else
			addr = base_addr + bo_size - 0x1000;
	}

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	if (bo) {
		munmap(map, bo_size);
		gem_close(fd, bo);
	} else {
		free(map);
	}
	xe_vm_destroy(fd, vm);
}

struct thread_data {
	pthread_t thread;
	pthread_barrier_t *barrier;
	int fd;
	uint32_t vm;
	uint64_t addr;
	struct drm_xe_engine_class_instance *eci;
	void *map;
	int *exit;
};

static void *hammer_thread(void *tdata)
{
	struct thread_data *t = tdata;
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data = t->map;
	uint32_t exec_queue = xe_exec_queue_create(t->fd, t->vm, t->eci, 0);
	int b;
	int i = 0;

	sync[0].handle = syncobj_create(t->fd, 0);
	pthread_barrier_wait(t->barrier);

	while (!*t->exit) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = t->addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = t->addr + sdi_offset;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		if (i % 32) {
			exec.num_syncs = 0;
			xe_exec(t->fd, &exec);
		} else {
			exec.num_syncs = 1;
			xe_exec(t->fd, &exec);
			igt_assert(syncobj_wait(t->fd, &sync[0].handle, 1,
						INT64_MAX, 0, NULL));
			syncobj_reset(t->fd, &sync[0].handle, 1);
		}
		++i;
	}

	syncobj_destroy(t->fd, sync[0].handle);
	xe_exec_queue_destroy(t->fd, exec_queue);

	return NULL;
}

#define MAP_FLAG_USERPTR		(0x1 << 0)
#define MAP_FLAG_INVALIDATE		(0x1 << 1)
#define MAP_FLAG_HAMMER_FIRST_PAGE	(0x1 << 2)
#define MAP_FLAG_LARGE_PAGE		(0x1 << 3)
#define MAP_FLAG_LARGE_PAGE_NO_SPLIT	(0x1 << 4)

/**
 * SUBTEST: munmap-style-unbind-%s
 * Functionality: unbind
 * Description: Test munmap style unbind with %arg[1]
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @one-partial:			one partial
 * @end:				end
 * @front:				front
 * @userptr-one-partial:		userptr one partial
 * @userptr-end:			userptr end
 * @userptr-front:			userptr front
 * @userptr-inval-end:			userptr inval end
 * @userptr-inval-front:		userptr inval front
 */

/**
 * SUBTEST: munmap-style-unbind-%s
 * Functionality: unbind
 * Description: Test munmap style unbind with %arg[1]
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @all:				all
 * @either-side-partial:		either side partial
 * @either-side-partial-hammer:		either side partial hammer
 * @either-side-full:			either side full
 * @many-all:				many all
 * @many-either-side-partial:		many either side partial
 * @many-either-side-partial-hammer:	many either side partial hammer
 * @many-either-side-full:		many either side full
 * @many-end:				many end
 * @many-front:				many front
 * @userptr-all:			userptr all
 * @userptr-either-side-partial:	userptr either side partial
 * @userptr-either-side-full:		userptr either side full
 * @userptr-many-all:			userptr many all
 * @userptr-many-either-side-full:	userptr many either side full
 * @userptr-many-end:			userptr many end
 * @userptr-many-front:			userptr many front
 * @userptr-inval-either-side-full:	userptr inval either side full
 * @userptr-inval-many-all:		userptr inval many all
 * @userptr-inval-many-either-side-partial:
 *					userptr inval many either side partial
 * @userptr-inval-many-either-side-full:
 *					userptr inval many either side full
 * @userptr-inval-many-end:		userptr inval many end
 * @userptr-inval-many-front:		userptr inval many front
 * @either-side-partial-large-page-hammer:
 *					either side partial large page hammer
 * @either-side-partial-split-page-hammer:
 *					either side partial split page hammer
 */

static void
test_munmap_style_unbind(int fd, struct drm_xe_engine_class_instance *eci,
			 int bo_n_pages, int n_binds,
			 int unbind_n_page_offset, int unbind_n_pages,
			 unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t addr = 0x1a00000, base_addr = 0x1a00000;
	uint32_t vm;
	uint32_t exec_queue;
	size_t bo_size;
	uint32_t bo = 0;
	uint64_t bind_size;
	uint64_t page_size = xe_get_default_alignment(fd);
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	void *map;
	int i, b;
	int invalidate = 0;
	struct thread_data t;
	pthread_barrier_t barrier;
	int exit = 0;
	int n_page_per_2mb = 0x200000 / xe_get_default_alignment(fd);

	/* Ensure prefetch will not fetch an unmapped page */
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE)
		igt_assert(unbind_n_page_offset * 0x1000 >
			   xe_cs_prefetch_size(fd));

	if (flags & MAP_FLAG_LARGE_PAGE) {
		bo_n_pages *= n_page_per_2mb;
		unbind_n_pages *= n_page_per_2mb;
		if (flags & MAP_FLAG_LARGE_PAGE_NO_SPLIT)
			unbind_n_page_offset *= n_page_per_2mb;
	}

	vm = xe_vm_create(fd, 0, 0);
	bo_size = xe_bb_size(fd, page_size * bo_n_pages);

	if (flags & MAP_FLAG_USERPTR) {
		map = mmap(from_user_pointer(addr), bo_size, PROT_READ |
			    PROT_WRITE, MAP_SHARED | MAP_FIXED |
			    MAP_ANONYMOUS, -1, 0);
		igt_assert(map != MAP_FAILED);
	} else {
		bo = xe_bo_create(fd, vm, bo_size,
				  vram_if_possible(fd, eci->gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		map = xe_bo_map(fd, bo, bo_size);
	}
	memset(map, 0, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	sync[0].handle = syncobj_create(fd, 0);
	sync[1].handle = syncobj_create(fd, 0);

	/* Do initial binds */
	bind_size = (page_size * bo_n_pages) / n_binds;
	for (i = 0; i < n_binds; ++i) {
		if (flags & MAP_FLAG_USERPTR)
			xe_vm_bind_userptr_async(fd, vm, 0, addr, addr,
						 bind_size, sync, 1);
		else
			xe_vm_bind_async(fd, vm, 0, bo, i * bind_size,
					 addr, bind_size, sync, 1);
		addr += bind_size;
	}
	addr = base_addr;

	/*
	 * Kick a thread to write the first page continuously to ensure we can't
	 * cause a fault if a rebind occurs during munmap style VM unbind
	 * (partial VMAs unbound).
	 */
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		t.fd = fd;
		t.vm = vm;
		t.addr = addr + page_size / 2;
		t.eci = eci;
		t.exit = &exit;
		t.map = map + page_size / 2;
		t.barrier = &barrier;
		pthread_barrier_init(&barrier, NULL, 2);
		pthread_create(&t.thread, 0, hammer_thread, &t);
		pthread_barrier_wait(&barrier);
	}

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		data = map + i * page_size;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		if (i)
			syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	/* Unbind some of the pages */
	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0,
			   addr + unbind_n_page_offset * page_size,
			   unbind_n_pages * page_size, sync, 2);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		data = map + i * page_size;
		igt_assert_eq(data->data, 0xc0ffee);
	}
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		memset(map, 0, page_size / 2);
		memset(map + page_size, 0, bo_size - page_size);
	} else {
		memset(map, 0, bo_size);
	}

try_again_after_invalidate:
	/* Verify we can use every page still bound */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;

		data = map + i * page_size;
		addr += page_size;

		if (i < unbind_n_page_offset ||
		    i + 1 > unbind_n_page_offset + unbind_n_pages) {
			b = 0;
			data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data->batch[b++] = sdi_addr;
			data->batch[b++] = sdi_addr >> 32;
			data->batch[b++] = 0xc0ffee;
			data->batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data->batch));

			sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
			syncobj_reset(fd, &sync[1].handle, 1);
			sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;

			exec.exec_queue_id = exec_queue;
			exec.address = batch_addr;
			xe_exec(fd, &exec);
		}
	}
	addr = base_addr;

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages still bound written */
	for (i = 0; i < n_binds; ++i) {
		if (i < unbind_n_page_offset ||
		    i + 1 > unbind_n_page_offset + unbind_n_pages) {
			data = map + i * page_size;
			igt_assert_eq(data->data, 0xc0ffee);
		}
	}
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		memset(map, 0, page_size / 2);
		memset(map + page_size, 0, bo_size - page_size);
	} else {
		memset(map, 0, bo_size);
	}

	/*
	 * The munmap style VM unbind can create new VMAs, make sure those are
	 * in the bookkeeping for another rebind after a userptr invalidate.
	 */
	if (flags & MAP_FLAG_INVALIDATE && !invalidate++) {
		map = mmap(from_user_pointer(addr), bo_size, PROT_READ |
			    PROT_WRITE, MAP_SHARED | MAP_FIXED |
			    MAP_ANONYMOUS, -1, 0);
		igt_assert(map != MAP_FAILED);
		goto try_again_after_invalidate;
	}

	/* Confirm unbound region can be rebound */
	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	if (flags & MAP_FLAG_USERPTR)
		xe_vm_bind_userptr_async(fd, vm, 0,
					 addr + unbind_n_page_offset * page_size,
					 addr + unbind_n_page_offset * page_size,
					 unbind_n_pages * page_size, sync, 1);
	else
		xe_vm_bind_async(fd, vm, 0, bo,
				 unbind_n_page_offset * page_size,
				 addr + unbind_n_page_offset * page_size,
				 unbind_n_pages * page_size, sync, 1);

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		data = map + i * page_size;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		data = map + i * page_size;
		igt_assert_eq(data->data, 0xc0ffee);
	}

	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		exit = 1;
		pthread_join(t.thread, NULL);
		pthread_barrier_destroy(&barrier);
	}

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_exec_queue_destroy(fd, exec_queue);
	munmap(map, bo_size);
	if (bo)
		gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: mmap-style-bind-%s
 * Functionality: bind
 * Description: Test mmap style unbind with %arg[1]
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @all:				all
 * @one-partial:			one partial
 * @either-side-partial:		either side partial
 * @either-side-full:			either side full
 * @either-side-partial-hammer:		either side partial hammer
 * @end:				end
 * @front:				front
 * @many-all:				many all
 * @many-either-side-partial:		many either side partial
 * @many-either-side-partial-hammer:	many either side partial hammer
 * @userptr-all:			userptr all
 * @userptr-one-partial:		userptr one partial
 * @userptr-either-side-partial:	userptr either side partial
 * @userptr-either-side-full:		userptr either side full
 * @either-side-partial-large-page-hammer:
 *					either side partial large page hammer
 * @either-side-partial-split-page-hammer:
 *					either side partial split page hammer
 */

static void
test_mmap_style_bind(int fd, struct drm_xe_engine_class_instance *eci,
		     int bo_n_pages, int n_binds, int unbind_n_page_offset,
		     int unbind_n_pages, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t addr = 0x1a00000, base_addr = 0x1a00000;
	uint32_t vm;
	uint32_t exec_queue;
	size_t bo_size;
	uint32_t bo0 = 0, bo1 = 0;
	uint64_t bind_size;
	uint64_t page_size = xe_get_default_alignment(fd);
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	void *map0, *map1;
	int i, b;
	struct thread_data t;
	pthread_barrier_t barrier;
	int exit = 0;
	int n_page_per_2mb = 0x200000 / xe_get_default_alignment(fd);

	if (flags & MAP_FLAG_LARGE_PAGE) {
		bo_n_pages *= n_page_per_2mb;
		unbind_n_pages *= n_page_per_2mb;
		if (flags & MAP_FLAG_LARGE_PAGE_NO_SPLIT)
			unbind_n_page_offset *= n_page_per_2mb;
	}

	vm = xe_vm_create(fd, 0, 0);
	bo_size = xe_bb_size(fd, page_size * bo_n_pages);

	if (flags & MAP_FLAG_USERPTR) {
		map0 = mmap(from_user_pointer(addr), bo_size, PROT_READ |
			    PROT_WRITE, MAP_SHARED | MAP_FIXED |
			    MAP_ANONYMOUS, -1, 0);
		map1 = mmap(from_user_pointer(addr + bo_size),
			    bo_size, PROT_READ | PROT_WRITE, MAP_SHARED |
			    MAP_FIXED | MAP_ANONYMOUS, -1, 0);
		igt_assert(map0 != MAP_FAILED);
		igt_assert(map1 != MAP_FAILED);
	} else {
		bo0 = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, eci->gt_id),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		map0 = xe_bo_map(fd, bo0, bo_size);
		bo1 = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, eci->gt_id),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		map1 = xe_bo_map(fd, bo1, bo_size);
	}
	memset(map0, 0, bo_size);
	memset(map1, 0, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	sync[0].handle = syncobj_create(fd, 0);
	sync[1].handle = syncobj_create(fd, 0);

	/* Do initial binds */
	bind_size = (page_size * bo_n_pages) / n_binds;
	for (i = 0; i < n_binds; ++i) {
		if (flags & MAP_FLAG_USERPTR)
			xe_vm_bind_userptr_async(fd, vm, 0, addr, addr,
						 bind_size, sync, 1);
		else
			xe_vm_bind_async(fd, vm, 0, bo0, i * bind_size,
					 addr, bind_size, sync, 1);
		addr += bind_size;
	}
	addr = base_addr;

	/*
	 * Kick a thread to write the first page continously to ensure we can't
	 * cause a fault if a rebind occurs during munmap style VM unbind
	 * (partial VMAs unbound).
	 */
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		t.fd = fd;
		t.vm = vm;
#define PAGE_SIZE	4096
		t.addr = addr + PAGE_SIZE / 2;
		t.eci = eci;
		t.exit = &exit;
		t.map = map0 + PAGE_SIZE / 2;
		t.barrier = &barrier;
		pthread_barrier_init(&barrier, NULL, 2);
		pthread_create(&t.thread, 0, hammer_thread, &t);
		pthread_barrier_wait(&barrier);
	}

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		data = map0 + i * page_size;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		if (i)
			syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	/* Bind some of the pages to different BO / userptr */
	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	if (flags & MAP_FLAG_USERPTR)
		xe_vm_bind_userptr_async(fd, vm, 0, addr + bo_size +
					 unbind_n_page_offset * page_size,
					 addr + unbind_n_page_offset * page_size,
					 unbind_n_pages * page_size, sync, 2);
	else
		xe_vm_bind_async(fd, vm, 0, bo1,
				 unbind_n_page_offset * page_size,
				 addr + unbind_n_page_offset * page_size,
				 unbind_n_pages * page_size, sync, 2);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		data = map0 + i * page_size;
		igt_assert_eq(data->data, 0xc0ffee);
	}
	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		memset(map0, 0, PAGE_SIZE / 2);
		memset(map0 + PAGE_SIZE, 0, bo_size - PAGE_SIZE);
	} else {
		memset(map0, 0, bo_size);
		memset(map1, 0, bo_size);
	}

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;

		data = map0 + i * page_size;
		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		data = map1 + i * page_size;
		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		if (i)
			syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;

		exec.exec_queue_id = exec_queue;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		uint32_t result = 0;

		data = map0 + i * page_size;
		result |= data->data;

		data = map1 + i * page_size;
		result |= data->data;

		igt_assert_eq(result, 0xc0ffee);
	}

	if (flags & MAP_FLAG_HAMMER_FIRST_PAGE) {
		exit = 1;
		pthread_join(t.thread, NULL);
		pthread_barrier_destroy(&barrier);
	}

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_exec_queue_destroy(fd, exec_queue);
	munmap(map0, bo_size);
	munmap(map1, bo_size);
	if (bo0)
		gem_close(fd, bo0);
	if (bo1)
		gem_close(fd, bo1);
	xe_vm_destroy(fd, vm);
}

static bool pxp_interface_supported(int fd)
{
	return xe_query_pxp_status(fd) != -EINVAL;
}

static void __bind_flag_valid(int fd, uint32_t bo, struct drm_xe_vm_bind bind,
			      struct drm_xe_vm_bind_op *bind_ops, int num_binds)
{
	struct drm_xe_sync *sync = from_user_pointer(bind.syncs);
	unsigned int valid_flags[] = {
		0,
		DRM_XE_VM_BIND_FLAG_READONLY,
		DRM_XE_VM_BIND_FLAG_IMMEDIATE,
		DRM_XE_VM_BIND_FLAG_NULL,
		DRM_XE_VM_BIND_FLAG_DUMPABLE,
		DRM_XE_VM_BIND_FLAG_CHECK_PXP,
	};

	for (int i = 0; i < ARRAY_SIZE(valid_flags); i++) {
		if (!pxp_interface_supported(fd) && valid_flags[i] == DRM_XE_VM_BIND_FLAG_CHECK_PXP)
			continue;

		for (int j = 0; j < num_binds; j++) {
			bind_ops[j].flags = valid_flags[i];
			bind_ops[j].obj = valid_flags[i] == DRM_XE_VM_BIND_FLAG_NULL ? 0 : bo;
		}

		if (num_binds == 1)
			bind.bind = bind_ops[0];

		igt_ioctl(fd, DRM_IOCTL_XE_VM_BIND, &bind);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
		syncobj_reset(fd, &sync[0].handle, 1);
	}
}

/**
 * SUBTEST: bind-flag-invalid
 * Functionality: bind
 * Description: Ensure invalid bind flags are rejected.
 * Test category: negative test
 *
 * SUBTEST: bind-array-flag-invalid
 * Functionality: bind
 * Description: Ensure invalid bind flags are rejected when submitting an array of binds.
 * Test category: negative test
 */
static void test_bind_flag_invalid(int fd, int num_binds)
{
	struct drm_xe_vm_bind_op *bind_ops;
	struct drm_xe_vm_bind bind;
	uint32_t vm;

	uint32_t bo, bo_size = xe_get_default_alignment(fd);
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};

	igt_assert(num_binds > 0);

	vm = xe_vm_create(fd, 0, 0);
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0), 0);
	sync[0].handle = syncobj_create(fd, 0);

	bind_ops = calloc(num_binds, sizeof(*bind_ops));
	for (int i = 0; i < num_binds; i++) {
		bind_ops[i].addr = addr + i * bo_size;
		bind_ops[i].range = bo_size;
		bind_ops[i].obj = bo;
		bind_ops[i].op = DRM_XE_VM_BIND_OP_MAP;
		bind_ops[i].pat_index = intel_get_pat_idx_wb(fd);
	}

	memset(&bind, 0, sizeof(bind));
	if (num_binds > 1)
		bind.vector_of_binds = to_user_pointer(bind_ops);
	bind.num_binds = num_binds;
	bind.syncs = to_user_pointer(sync);
	bind.num_syncs = 1;
	bind.vm_id = vm;

	/* Using valid flags should work */
	__bind_flag_valid(fd, bo, bind, bind_ops, num_binds);

	/* Using invalid flags should not work */
	for (int i = 0; i < num_binds; i++) {
		bind_ops[i].flags = BIT(30);
		bind_ops[i].obj = bo;
	}

	if (num_binds == 1)
		bind.bind = bind_ops[0];

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_BIND, &bind, EINVAL);

	/* Using valid flags should still work */
	__bind_flag_valid(fd, bo, bind, bind_ops, num_binds);

	syncobj_destroy(fd, sync[0].handle);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: invalid-flag-%s
 * Functionality: ioctl_input_validation
 * Description:  function %arg[1] used in vm create IOCTL to make it fail
 *
 * arg[1]:
 * @xe_vm_create_fault:  xe_vm_create_fault
 * @xe_vm_create_scratch_fault: xe_vm_create_scrach_fault
 * @xe_vm_create_scratch_fault_lr:    xe_vm_create_scrach_fault_lr
 */

static void invalid_flag(int fd, __u32 flags)
{
	struct drm_xe_vm_create create = {
		.flags = flags,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_CREATE, &create, EINVAL);
}

/**
 * SUBTEST: invalid-extensions
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid extensions returns expected error code
 *
 * SUBTEST: vm-create-invalid-reserved
 * Functionality: ioctl_input_validation
 * Description: Send query with invalid reserved value for vm_create ioctl
 */

static void invalid_extensions(int fd)
{
	struct drm_xe_vm_create create = {
		.extensions = -1,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_CREATE, &create, EINVAL);
}

static void vm_create_invalid_reserved(int fd)
{
	struct drm_xe_vm_create create = {
		.reserved[0] = 0xffff,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_CREATE, &create, EINVAL);

	create.reserved[0] = 0;
	create.reserved[1] = 0xffff;
	do_ioctl_err(fd, DRM_IOCTL_XE_VM_CREATE, &create, EINVAL);
}

/**
 * SUBTEST: vm-destroy-invalid-reserved
 * Functionality: ioctl_input_validation
 * Description: Send query with invalid reserved value for vm_destroy ioctl
 *
 * SUBTEST: invalid-pad
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid pad returns expected error code
 *
 * SUBTEST: invalid-vm-id
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid vm_id returns expected error code
 */

static void vm_destroy_invalid_reserved(int fd)
{
	struct drm_xe_vm_destroy destroy = {
		.reserved[0] = 0xffff,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_DESTROY, &destroy, EINVAL);

	destroy.reserved[0] = 0;
	destroy.reserved[1] = 0xffff;
	do_ioctl_err(fd, DRM_IOCTL_XE_VM_DESTROY, &destroy, EINVAL);
}

static void invalid_pad(int fd)
{
	struct drm_xe_vm_destroy destroy = {
		.pad = 1,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_DESTROY, &destroy, EINVAL);
}

static void invalid_vm_id(int fd)
{
	struct drm_xe_vm_destroy destroy = {
		.vm_id = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_DESTROY, &destroy, ENOENT);
}

static enum overcommit_stage create_data_bos(int fd, uint32_t vm, uint32_t *bos, int num_bos,
					     uint64_t data_addr, uint32_t bo_flags, int gt_id,
					     int *num_bound_out)
{
	uint32_t placement = vram_memory(fd, gt_id);

	*num_bound_out = 0;

	for (int i = 0; i < num_bos; i++) {
		int bind_err;
		int create_ret = 0;

		/* Create BO using the case's create function */
		create_ret = __xe_bo_create(fd, vm, OVERCOMMIT_BO_SIZE, placement,
					    bo_flags, NULL, &bos[i]);

		if (create_ret) {
			if (errno == ENOMEM || errno == ENOSPC) {
				igt_debug("BO create failed at %d/%d with error %d (%s)\n",
					  i, num_bos, errno, strerror(errno));
				return EXPECT_CREATE;
			}
			igt_assert_f(0, "Unexpected BO create error %d (%s)\n", errno,
				     strerror(errno));
		}

		bind_err = __xe_vm_bind_lr_sync(fd, vm, bos[i], 0, data_addr +
						((uint64_t)i * OVERCOMMIT_BO_SIZE),
						OVERCOMMIT_BO_SIZE, 0);
		if (bind_err) {
			if (errno == ENOMEM || errno == ENOSPC) {
				igt_debug("BO bind failed at %d/%d - error %d (%s), %d BOs bound\n",
					  i, num_bos, errno, strerror(errno), i);
				return EXPECT_BIND;
			}
			igt_assert_f(0, "Unexpected BO bind error %d (%s)\n", errno,
				     strerror(errno));
		}

		*num_bound_out = i + 1;
		igt_debug("Created and bound BO %d/%d at 0x%llx\n",
			  i + 1, num_bos,
			  (unsigned long long)(data_addr + ((uint64_t)i * OVERCOMMIT_BO_SIZE)));
	}
	return EXPECT_NONE;
}

static void verify_bo(int fd, uint32_t *bos, int num_bos, uint64_t stride)
{
	for (int i = 0; i < num_bos; i++) {
		uint32_t *verify_data;
		int errors = 0;

		verify_data = xe_bo_map(fd, bos[i], OVERCOMMIT_BO_SIZE);
		igt_assert(verify_data);

		for (int off = 0; off < OVERCOMMIT_BO_SIZE; off += stride) {
			uint32_t expected = 0xBB;
			uint32_t actual = *(uint32_t *)((char *)verify_data + off);

			if (actual != expected) {
				if (errors < 5)
					igt_debug("Mismatch at BO %d offset 0x%llx",
						  i, (unsigned long long)off);
				errors++;
			}
		}

		munmap(verify_data, OVERCOMMIT_BO_SIZE);
		igt_assert_f(errors == 0, "Data verification failed for BO %d with %d errors\n",
			     i, errors);
	}
}

static void bind_userptr_sync(int fd, uint32_t vm, uint32_t bind_exec_queue, void *userptr,
			      uint64_t addr, uint64_t size, struct drm_xe_sync *sync,
			      uint64_t *sync_mem)
{
	*sync_mem = 0;

	sync->addr = to_user_pointer(sync_mem);
	xe_vm_bind_userptr_async(fd, vm, bind_exec_queue, to_user_pointer(userptr), addr, size,
				 sync, 1);
	xe_wait_ufence(fd, sync_mem, USER_FENCE_VALUE, bind_exec_queue, 20 * NSEC_PER_SEC);
}

/**
 * SUBTEST: overcommit-fault-%s
 * Description: Test VM overcommit behavior in fault mode with %arg[1] configuration
 * Functionality: overcommit
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @vram-lr:VRAM with LR and fault mode, expects exec to pass
 * @vram-lr-no-overcommit:VRAM with LR, fault and NO_VM_OVERCOMMIT; exec succeeds via migration
 */

/**
 * SUBTEST: overcommit-nonfault-%s
 * Description: Test VM overcommit behavior in nonfault mode with %arg[1] configuration
 * Functionality: overcommit
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @vram-lr-defer:VRAM with LR and defer backing, expects bind rejection
 * @vram-lr-external-nodefer:VRAM with LR and external BO without defer, expects bind fail
 * @vram-no-lr:VRAM without LR mode, expects exec to fail
 */
struct vm_overcommit_case {
	const char *name;
	uint32_t vm_flags;
	uint32_t bo_flags;
	uint64_t data_addr;
	int overcommit_mult;
	enum overcommit_stage expected_stage;
};

static const struct vm_overcommit_case overcommit_cases[] = {
	/* DEFER_BACKING */
	{
		.name = "vram-lr-defer",
		.vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE,
		.bo_flags = DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING |
			    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
		.data_addr = 0x1a0000,
		.overcommit_mult = 2,
		.expected_stage = EXPECT_BIND,
	},
	/* External BO without defer backing */
	{
		.name = "vram-lr-external-nodefer",
		.vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE,
		.bo_flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
		.data_addr = 0x1a0000,
		.overcommit_mult = 2,
		.expected_stage = EXPECT_BIND,
	},
	/* LR + FAULT - should not fail on exec */
	{
		.name = "vram-lr",
		.vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE |
			    DRM_XE_VM_CREATE_FLAG_FAULT_MODE,
		.bo_flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
		.data_addr = 0x300000000,
		.overcommit_mult = 2,
		.expected_stage = EXPECT_NONE,
	},
	/* !LR - overcommit should fail on exec */
	{
		.name = "vram-no-lr",
		.vm_flags = 0,
		.bo_flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
		.data_addr = 0x300000000,
		.overcommit_mult = 2,
		.expected_stage = EXPECT_EXEC,
	},
	/* LR + FAULT + NO_VM_OVERCOMMIT */
	{
		.name = "vram-lr-no-overcommit",
		.vm_flags = DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT | DRM_XE_VM_CREATE_FLAG_LR_MODE |
			    DRM_XE_VM_CREATE_FLAG_FAULT_MODE,
		.bo_flags = DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING |
			    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
		.data_addr = 0x300000000,
		.overcommit_mult = 2,
		/*
		 * FAULT_MODE handles VRAM pressure via migration even with
		 * NO_VM_OVERCOMMIT; DEFER_BACKING defers physical allocation
		 * to fault time so bind-time rejection does not occur.
		 */
		.expected_stage = EXPECT_NONE,
	},
	{ }
};

static void
test_vm_overcommit(int fd, struct drm_xe_engine_class_instance *eci,
		   const struct vm_overcommit_case *c, int num_bos)
{
	uint32_t vm = 0, *bos, batch_bo = 0, exec_queue = 0, bind_exec_queue = 0;
	uint64_t sync_addr = 0x1000000000, batch_addr = 0x200000000;
	int i, num_bound = 0, bind_err, create_ret, wait_ret;
	enum overcommit_stage actual_stage = EXPECT_NONE;
	uint64_t off, data_addr;
	int64_t timeout = 20 * NSEC_PER_SEC;
	uint64_t stride = SZ_1M;
	size_t sync_size;
	struct drm_xe_sync bind_sync[1] = {
		{
			.type = DRM_XE_SYNC_TYPE_USER_FENCE,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL,
			.timeline_value = USER_FENCE_VALUE
		},
	};
	struct drm_xe_sync exec_sync[1] = {
		{
			.type = DRM_XE_SYNC_TYPE_USER_FENCE,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL,
			.timeline_value = USER_FENCE_VALUE,
		},
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(exec_sync),
	};
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
		uint64_t vm_sync;
	} *batch_data = NULL;
	uint64_t *user_fence_sync = NULL;

	data_addr = c->data_addr;

	bos = calloc(num_bos, sizeof(*bos));
	igt_assert(bos);

	/* Create VM with appropriate flags */
	vm = xe_vm_create(fd, c->vm_flags, 0);
	igt_assert(vm);

	bind_exec_queue = xe_bind_exec_queue_create(fd, vm, 0);
	sync_size = xe_bb_size(fd, sizeof(uint64_t) * num_bos);
	user_fence_sync = mmap(NULL, sync_size, PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	igt_assert(user_fence_sync != MAP_FAILED);
	memset(user_fence_sync, 0, sync_size);
	exec_sync->addr = to_user_pointer(&user_fence_sync[0]);

	/* Create and bind data BOs */
	actual_stage = create_data_bos(fd, vm, bos, num_bos, data_addr,
				       c->bo_flags, eci->gt_id, &num_bound);
	igt_debug("Overcommit BO creation: case=%s actual_stage=%d num_bound=%d/%d\n",
		  c->name, actual_stage, num_bound, num_bos);
	/*
	 * On EXPECT_CREATE nothing was bound so bail out entirely.
	 * On EXPECT_BIND with no BOs bound there is nothing to execute either.
	 * On EXPECT_BIND with some BOs bound, continue executing so that the
	 * already-bound BOs can still be executed, verifying they are usable
	 * after a partial bind failure.
	 */
	if (actual_stage == EXPECT_CREATE || (actual_stage == EXPECT_BIND && num_bound == 0))
		goto check_and_cleanup;

	/*
	 * Create batch buffer first in SRAM as focus is to
	 * check overcommit in VRAM
	 */
	create_ret = __xe_bo_create(fd, vm, 0x1000, system_memory(fd), 0, NULL, &batch_bo);
	igt_assert_f(create_ret == 0, "Unexpected batch BO create error %d (%s)\n",
		     create_ret, strerror(errno));

	igt_debug("Mapping the created BO\n");
	batch_data = xe_bo_map(fd, batch_bo, 0x1000);
	igt_assert(batch_data);
	memset(batch_data, 0, 0x1000);

	bind_userptr_sync(fd, vm, bind_exec_queue, user_fence_sync, sync_addr, sync_size, bind_sync,
			  &batch_data->vm_sync);
	exec_sync->addr = sync_addr;
	batch_data->vm_sync = 0;
	bind_err = __xe_vm_bind_lr_sync(fd, vm, batch_bo, 0, batch_addr, 0x1000, 0);
	if (bind_err) {
		if (errno == ENOMEM || errno == ENOSPC) {
			actual_stage = EXPECT_BIND;
			goto check_and_cleanup;
		} else { /* Assert on any unexpected bind error */
			igt_assert_f(0, "Unexpected bind error %d (%s)\n", bind_err,
				     strerror(errno));
		}
	}

	igt_debug("VM binds done - batch_bo at 0x%llx\n", (unsigned long long)batch_addr);
	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	/* Use GPU to write to each successfully bound BO */
	for (i = 0; i < num_bound; i++) {
		igt_debug("Writing to BO %d/%d via GPU\n", i + 1, num_bos);
		timeout = 20 * NSEC_PER_SEC;

		for (off = 0; off < OVERCOMMIT_BO_SIZE; off += stride) {
			uint64_t target_addr = data_addr + ((uint64_t)i * OVERCOMMIT_BO_SIZE) + off;
			int b_idx = 0;

			batch_data->batch[b_idx++] = MI_STORE_DWORD_IMM_GEN4;
			batch_data->batch[b_idx++] = target_addr & 0xFFFFFFFF;
			batch_data->batch[b_idx++] = (target_addr >> 32) & 0xFFFFFFFF;
			batch_data->batch[b_idx++] = 0xBB;
			batch_data->batch[b_idx++] = MI_BATCH_BUFFER_END;

			/* Submit batch */
			exec.exec_queue_id = exec_queue;
			exec.address = batch_addr;

			if (igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec)) {
				if (errno == ENOMEM || errno == ENOSPC) {
					igt_debug("Expected fault/error: %d (%s)\n",
						  errno, strerror(errno));
					actual_stage = EXPECT_EXEC;
					goto check_and_cleanup;
				}
				igt_assert_f(0, "Unexpected exec error: %d\n", errno);
			}
			wait_ret = __xe_wait_ufence(fd, &user_fence_sync[0],
						    USER_FENCE_VALUE, exec_queue, &timeout);
			/*
			 * EIO means the exec queue was banned due to VRAM
			 * exhaustion in non-fault mode after partial bind.
			 */
			if (wait_ret == -EIO) {
				igt_assert_f(c->expected_stage == EXPECT_BIND ||
					     c->expected_stage == EXPECT_EXEC,
					     "Unexpected queue reset\n");
				actual_stage = EXPECT_EXEC;
				goto check_and_cleanup;
			}
			igt_assert_eq(wait_ret, 0);
			user_fence_sync[0] = 0;
		}
		igt_debug("Accessed BO %d/%d via GPU\n", i + 1, num_bos);
	}
	igt_debug("All batches submitted - waiting for GPU completion\n");

	/* Verify GPU writes for bound BOs */
	if (actual_stage == EXPECT_NONE || (actual_stage == EXPECT_BIND && num_bound > 0))
		verify_bo(fd, bos, num_bound, stride);

check_and_cleanup:
	igt_assert_f(actual_stage == c->expected_stage, "Expected overcommit at stage %d, got %d\n",
		     c->expected_stage, actual_stage);
	/* Cleanup */
	if (exec_queue)
		xe_exec_queue_destroy(fd, exec_queue);
	if (bind_exec_queue)
		xe_exec_queue_destroy(fd, bind_exec_queue);
	if (batch_data)
		munmap(batch_data, 0x1000);
	if (batch_bo)
		gem_close(fd, batch_bo);

	if (user_fence_sync)
		munmap(user_fence_sync, sync_size);

	if (bos) {
		for (i = 0; i < num_bos; i++) {
			if (bos[i])
				gem_close(fd, bos[i]);
		}
		free(bos);
	}
	if (vm > 0)
		xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: out-of-memory
 * Description: Test if vm_bind ioctl results in oom
 * when creating and vm_binding buffer objects on an LR vm beyond available visible vram size.
 * Functionality: oom
 * Test category: functionality test
 */
static void test_oom(int fd)
{
#define BO_SIZE xe_bb_size(fd, SZ_512M)
#define MAX_BUFS ((int)(xe_visible_vram_size(fd, 0) / BO_SIZE))
	uint64_t addr = 0x1a0000;
	uint64_t vm_sync;
	uint32_t bo[MAX_BUFS + 1];
	uint32_t *data[MAX_BUFS + 1];
	uint32_t vm;
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	size_t bo_size = BO_SIZE;
	int total_bufs = MAX_BUFS;
	int bind_vm = 0;
	bool oom = false;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	for (int iter = 0; iter <= total_bufs; iter++) {
		int err = 0;

		bo[iter] = xe_bo_create(fd, 0, bo_size,
					vram_if_possible(fd, 0),
					DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING |
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

		sync[0].addr = to_user_pointer(&vm_sync);
		err = __xe_vm_bind(fd, vm, 0, bo[iter], 0,
				   addr + bo_size * iter, bo_size,
				   DRM_XE_VM_BIND_OP_MAP, 0, sync,
				   1, 0, DEFAULT_PAT_INDEX, 0);

		if (err) {
			if (err == -ENOMEM || err == -ENOSPC) {
				oom = true;
				break;
			}
			igt_assert_f(!err, "Unexpected error %d for vm bind\n",
				     err);
		} else {
			bind_vm = bind_vm + 1;
		}

		xe_wait_ufence(fd, &vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
		vm_sync = 0;
		data[iter] = xe_bo_map(fd, bo[iter], bo_size);
		memset(data[iter], 0, bo_size);
	}

	igt_assert_f(oom, "OOM scenario is not working as expected\n");

	if (bind_vm < total_bufs)
		igt_warn("VRAM was smaller than estimated,"
			 "may be due to leaked VRAM memory\n");

	for (int iter = 0; iter < bind_vm; iter++) {
		sync[0].addr = to_user_pointer(&vm_sync);
		xe_vm_unbind_async(fd, vm, 0, 0, addr + bo_size * iter, bo_size,
				   sync, 1);
		xe_wait_ufence(fd, &vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
		munmap(data[iter], bo_size);
		gem_close(fd, bo[iter]);
	}
}

/**
 * SUBTEST: vm-get-property-invalid-reserved
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid reserved returns expected error code
 *
 * SUBTEST: vm-get-property-invalid-extensions
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid extensions returns expected error code
 *
 * SUBTEST: vm-get-property-invalid-pad
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid pad returns expected error code
 *
 * SUBTEST: vm-get-property-invalid-vm-id
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid vm_id returns expected error code
 *
 * SUBTEST: vm-get-property-invalid-size
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid size return expected error code
 *
 * SUBTEST: vm-get-property-invalid-property
 * Functionality: ioctl_input_validation
 * Description: Check query with invalid property returns expected error code
 *
 * SUBTEST: vm-get-property-exercise
 * Functionality: drm_xe_vm_get_property
 * Description: Check query correctly reports pagefaults on vm
 */
static void get_property_invalid_reserved(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.reserved[0] = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, EINVAL);
}

static void get_property_invalid_extensions(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.extensions = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, EINVAL);
}

static void get_property_invalid_pad(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.pad = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, EINVAL);
}

static void get_property_invalid_vm_id(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.vm_id = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, ENOENT);
}

static void get_property_invalid_size(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.vm_id = vm,
		.property = DRM_XE_VM_GET_PROPERTY_FAULTS,
		.size = -1,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, EINVAL);
}

static void get_property_invalid_property(int fd, uint32_t vm)
{
	struct drm_xe_vm_get_property query = {
		.vm_id = vm,
		.property = 0xdeadbeef,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_VM_GET_PROPERTY, &query, EINVAL);
}

static void
gen_pf(int fd, uint32_t vm, struct drm_xe_engine_class_instance *eci)
{
	int n_exec_queues = 2;
	int n_execs = 2;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[2];
	uint32_t syncobjs[2];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = false };
	int i, b;

	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = !i ? addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_exec_queues;

		if (!i) {
			spin_opts.addr = base_addr + spin_offset;
			xe_spin_init(&data[i].spin, &spin_opts);
			exec_addr = spin_opts.addr;
		} else {
			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			exec_addr = batch_addr;
		}

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = exec_addr;
		if (e != i)
			syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_exec_queues && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
}

static void print_pf(struct xe_vm_fault *fault)
{
	igt_debug("FAULT:\n");
	igt_debug("address = 0x%08x%08x\n",
		  upper_32_bits(fault->address),
		  lower_32_bits(fault->address));
	igt_debug("address precision = %u\n", fault->address_precision);
	igt_debug("access type = %u\n", fault->access_type);
	igt_debug("fault type = %u\n", fault->fault_type);
	igt_debug("fault level = %u\n", fault->fault_level);
	igt_debug("\n");
}

static void get_property_exercise(int fd, uint32_t vm)
{
	struct drm_xe_engine_class_instance *hwe;
	struct xe_vm_fault *faults, f0, f;
	struct drm_xe_vm_get_property query = {
		.vm_id = vm,
		.property = DRM_XE_VM_GET_PROPERTY_FAULTS
	};
	int i, fault_count;

	xe_vm_get_property(fd, vm, &query);

	igt_assert_eq(query.size, 0);

	xe_for_each_engine(fd, hwe)
		gen_pf(fd, vm, hwe);

	xe_vm_get_property(fd, vm, &query);
	igt_assert_lt(0, query.size);

	faults = malloc(query.size);
	igt_assert(faults);

	query.data = to_user_pointer(faults);
	xe_vm_get_property(fd, vm, &query);

	fault_count = query.size / sizeof(struct xe_vm_fault);
	f0 = faults[0];
	for (i = 0; i < fault_count; i++) {
		f = faults[i];
		print_pf(&f);
		igt_assert_eq(f.address, f0.address);
		igt_assert_eq(f.access_type, f0.access_type);
		igt_assert_eq(f.fault_type, f0.fault_type);
		igt_assert_eq(f.reserved[0] | f.reserved[1] |
			      f.reserved[2] | f.reserved[3], 0);
	}
	free(faults);
}

static void test_get_property(int fd, void (*func)(int fd, uint32_t vm))
{
	uint32_t vm;

	vm = xe_vm_create(fd, 0, 0);
	func(fd, vm);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe, *hwe_non_copy = NULL;
	uint64_t bind_size;
	int fd;
	const struct section {
		const char *name;
		int bo_n_pages;
		int n_binds;
		int unbind_n_page_offset;
		int unbind_n_pages;
		unsigned int flags;
	} munmap_sections[] = {
		{ "all", 4, 2, 0, 4, 0 },
		{ "one-partial", 4, 1, 1, 2, 0 },
		{ "either-side-partial", 4, 2, 1, 2, 0 },
		{ "either-side-partial-hammer", 6, 2, 2, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE },
		{ "either-side-partial-split-page-hammer", 6, 2, 2, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE |
			MAP_FLAG_LARGE_PAGE },
		{ "either-side-partial-large-page-hammer", 6, 2, 2, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE |
			MAP_FLAG_LARGE_PAGE |
			MAP_FLAG_LARGE_PAGE_NO_SPLIT },
		{ "either-side-full", 4, 4, 1, 2, 0 },
		{ "end", 4, 2, 0, 3, 0 },
		{ "front", 4, 2, 1, 3, 0 },
		{ "many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8, 0 },
		{ "many-either-side-partial", 4 * 8, 2 * 8, 1, 4 * 8 - 2, 0 },
		{ "many-either-side-partial-hammer", 4 * 8, 2 * 8, 2, 4 * 8 - 4,
			MAP_FLAG_HAMMER_FIRST_PAGE },
		{ "many-either-side-full", 4 * 8, 4 * 8, 1 * 8, 2 * 8, 0 },
		{ "many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2, 0 },
		{ "many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2, 0 },
		{ "userptr-all", 4, 2, 0, 4, MAP_FLAG_USERPTR },
		{ "userptr-one-partial", 4, 1, 1, 2, MAP_FLAG_USERPTR },
		{ "userptr-either-side-partial", 4, 2, 1, 2,
			MAP_FLAG_USERPTR },
		{ "userptr-either-side-full", 4, 4, 1, 2,
			MAP_FLAG_USERPTR },
		{ "userptr-end", 4, 2, 0, 3, MAP_FLAG_USERPTR },
		{ "userptr-front", 4, 2, 1, 3, MAP_FLAG_USERPTR },
		{ "userptr-many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8,
			MAP_FLAG_USERPTR },
		{ "userptr-many-either-side-full", 4 * 8, 4 * 8, 1 * 8, 2 * 8,
			MAP_FLAG_USERPTR },
		{ "userptr-many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2,
			MAP_FLAG_USERPTR },
		{ "userptr-many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2,
			MAP_FLAG_USERPTR },
		{ "userptr-inval-either-side-full", 4, 4, 1, 2,
			MAP_FLAG_USERPTR | MAP_FLAG_INVALIDATE },
		{ "userptr-inval-end", 4, 2, 0, 3, MAP_FLAG_USERPTR |
			MAP_FLAG_INVALIDATE },
		{ "userptr-inval-front", 4, 2, 1, 3, MAP_FLAG_USERPTR |
			MAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8,
			MAP_FLAG_USERPTR | MAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-either-side-partial", 4 * 8, 2 * 8, 1,
			4 * 8 - 2, MAP_FLAG_USERPTR |
				MAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-either-side-full", 4 * 8, 4 * 8, 1 * 8,
			2 * 8, MAP_FLAG_USERPTR | MAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2,
			MAP_FLAG_USERPTR | MAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2,
			MAP_FLAG_USERPTR | MAP_FLAG_INVALIDATE },
		{ NULL },
	};
	const struct section mmap_sections[] = {
		{ "all", 4, 2, 0, 4, 0 },
		{ "one-partial", 4, 1, 1, 2, 0 },
		{ "either-side-partial", 4, 2, 1, 2, 0 },
		{ "either-side-full", 4, 4, 1, 2, 0 },
		{ "either-side-partial-hammer", 4, 2, 1, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE },
		{ "either-side-partial-split-page-hammer", 4, 2, 1, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE |
			MAP_FLAG_LARGE_PAGE },
		{ "either-side-partial-large-page-hammer", 4, 2, 1, 2,
			MAP_FLAG_HAMMER_FIRST_PAGE |
			MAP_FLAG_LARGE_PAGE |
			MAP_FLAG_LARGE_PAGE_NO_SPLIT },
		{ "end", 4, 2, 0, 3, 0 },
		{ "front", 4, 2, 1, 3, 0 },
		{ "many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8, 0 },
		{ "many-either-side-partial", 4 * 8, 2 * 8, 1, 4 * 8 - 2, 0 },
		{ "many-either-side-partial-hammer", 4 * 8, 2 * 8, 1, 4 * 8 - 2,
			MAP_FLAG_HAMMER_FIRST_PAGE },
		{ "userptr-all", 4, 2, 0, 4, MAP_FLAG_USERPTR },
		{ "userptr-one-partial", 4, 1, 1, 2, MAP_FLAG_USERPTR },
		{ "userptr-either-side-partial", 4, 2, 1, 2, MAP_FLAG_USERPTR },
		{ "userptr-either-side-full", 4, 4, 1, 2, MAP_FLAG_USERPTR },
		{ NULL },
	};

        const struct vm_create_section {
                const char *name;
                __u32 flags;
        } xe_vm_create_invalid_flags[] = {
                { "xe_vm_create_fault", DRM_XE_VM_CREATE_FLAG_FAULT_MODE },
                { "xe_vm_create_scratch_fault",
                        DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE |
                        DRM_XE_VM_CREATE_FLAG_FAULT_MODE },
                { "xe_vm_create_scratch_fault_lr",
                        ~(DRM_XE_VM_CREATE_FLAG_LR_MODE |
                        DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE |
                        DRM_XE_VM_CREATE_FLAG_FAULT_MODE) },
                { }
        };

	const struct vm_get_property {
		const char *name;
		void (*test)(int fd, uint32_t vm);
	} xe_vm_get_property_tests[] = {
		{ "invalid-reserved", get_property_invalid_reserved },
		{ "invalid-extensions", get_property_invalid_extensions },
		{ "invalid-pad", get_property_invalid_pad },
		{ "invalid-vm-id", get_property_invalid_vm_id },
		{ "invalid-size", get_property_invalid_size },
		{ "invalid-property", get_property_invalid_property },
		{ "exercise", get_property_exercise },
		{ }
	};

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);

		xe_for_each_engine(fd, hwe)
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COPY) {
				hwe_non_copy = hwe;
				break;
			}
	}

	igt_subtest("bind-once")
		test_bind_once(fd);

	igt_subtest("bind-one-bo-many-times")
		test_bind_one_bo_many_times(fd);

	igt_subtest("bind-one-bo-many-times-many-vm")
		test_bind_one_bo_many_times_many_vm(fd);

	igt_subtest("scratch")
		test_scratch(fd);

	igt_subtest("partial-unbinds")
		test_partial_unbinds(fd);

	igt_subtest("unbind-all-2-vmas")
		unbind_all(fd, 2);

	igt_subtest("unbind-all-8-vmas")
		unbind_all(fd, 8);

	igt_subtest("userptr-invalid")
		userptr_invalid(fd);

	igt_subtest("bind-flag-invalid")
		test_bind_flag_invalid(fd, 1);

	igt_subtest("compact-64k-pages")
		xe_for_each_engine(fd, hwe) {
			compact_64k_pages(fd, hwe);
			break;
		}

	igt_subtest("shared-pte-page")
		xe_for_each_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4,
					xe_get_default_alignment(fd));

	igt_subtest("shared-pde-page")
		xe_for_each_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512);

	igt_subtest("shared-pde2-page")
		xe_for_each_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512 * 512);

	igt_subtest("shared-pde3-page")
		xe_for_each_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512 * 512 * 512);

	igt_subtest("bind-execqueues-independent")
		xe_for_each_engine(fd, hwe)
			test_bind_execqueues_independent(fd, hwe, 0);

	igt_subtest("bind-execqueues-conflict")
		xe_for_each_engine(fd, hwe)
			test_bind_execqueues_independent(fd, hwe, CONFLICT);

	igt_subtest("bind-array-twice")
		xe_for_each_engine(fd, hwe)
			test_bind_array(fd, hwe, 2, 0x1a0000, 0, 0);

	igt_subtest("bind-array-many")
		xe_for_each_engine(fd, hwe)
			test_bind_array(fd, hwe, 16, 0x1a0000, 0, 0);

	igt_subtest("bind-array-enobufs")
		xe_for_each_engine(fd, hwe)
			test_bind_array(fd, hwe, xe_has_vram(fd) ? 1024 : 512,
					0x1a0000, SZ_2M,
					BIND_ARRAY_ENOBUFS_FLAG);

	igt_subtest("bind-array-exec_queue-twice")
		xe_for_each_engine(fd, hwe)
			test_bind_array(fd, hwe, 2, 0x1a0000, 0,
					BIND_ARRAY_BIND_EXEC_QUEUE_FLAG);

	igt_subtest("bind-array-exec_queue-many")
		xe_for_each_engine(fd, hwe)
			test_bind_array(fd, hwe, 16, 0x1a0000, 0,
					BIND_ARRAY_BIND_EXEC_QUEUE_FLAG);

	igt_subtest("bind-array-conflict")
		xe_for_each_engine(fd, hwe)
			test_bind_array_conflict(fd, hwe, false, false);

	igt_subtest("bind-no-array-conflict")
		xe_for_each_engine(fd, hwe)
			test_bind_array_conflict(fd, hwe, true, false);

	igt_subtest("bind-array-conflict-error-inject")
		xe_for_each_engine(fd, hwe)
			test_bind_array_conflict(fd, hwe, false, true);

	igt_subtest("bind-array-flag-invalid")
		test_bind_flag_invalid(fd, 16);

	for (bind_size = 0x1ull << 21; bind_size <= 0x1ull << 31;
	     bind_size = bind_size << 1) {
		igt_subtest_f("large-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size, 0);
				break;
			}
		igt_subtest_f("large-split-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT);
				break;
			}
		igt_subtest_f("large-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_MISALIGNED);
				break;
			}
		igt_subtest_f("large-split-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_MISALIGNED);
				break;
			}
		igt_subtest_f("large-userptr-binds-%lld", (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-split-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_MISALIGNED |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-split-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_MISALIGNED |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
	}

	bind_size = (0x1ull << 21) + (0x1ull << 20);
	igt_subtest_f("mixed-binds-%lld", (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size, 0);
			break;
		}

	igt_subtest_f("mixed-misaligned-binds-%lld", (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-binds-%lld", (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size, 0);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-misaligned-binds-%lld", (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED);
			break;
		}

	bind_size = (0x1ull << 21) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-binds-%lld", (long long) bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	igt_subtest_f("mixed-userptr-misaligned-binds-%lld",
		      (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED |
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-binds-%lld", (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-misaligned-binds-%lld",
		      (long long)bind_size)
		xe_for_each_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED |
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	for (const struct section *s = munmap_sections; s->name; s++) {
		igt_subtest_f("munmap-style-unbind-%s", s->name) {
			igt_require_f(hwe_non_copy,
				      "Requires non-copy engine to run\n");

			test_munmap_style_unbind(fd, hwe_non_copy,
						 s->bo_n_pages,
						 s->n_binds,
						 s->unbind_n_page_offset,
						 s->unbind_n_pages,
						 s->flags);
		}
	}

	for (const struct section *s = mmap_sections; s->name; s++) {
		igt_subtest_f("mmap-style-bind-%s", s->name) {
			igt_require_f(hwe_non_copy,
				      "Requires non-copy engine to run\n");

			test_mmap_style_bind(fd, hwe_non_copy,
					     s->bo_n_pages,
					     s->n_binds,
					     s->unbind_n_page_offset,
					     s->unbind_n_pages,
					     s->flags);
		}
	}

	for (const struct vm_create_section *s = xe_vm_create_invalid_flags; s->name; s++) {
		igt_subtest_f("invalid-flag-%s", s->name)
			invalid_flag(fd, s->flags);
	}

	igt_subtest("invalid-extensions")
		invalid_extensions(fd);

	igt_subtest("vm-create-invalid-reserved")
		vm_create_invalid_reserved(fd);

	igt_subtest("vm-destroy-invalid-reserved")
		vm_destroy_invalid_reserved(fd);

	igt_subtest("invalid-pad")
		invalid_pad(fd);

	igt_subtest("invalid-vm-id")
		invalid_vm_id(fd);

	igt_subtest("out-of-memory") {
		igt_require(xe_has_vram(fd));
		igt_assert(xe_visible_vram_size(fd, 0));
		test_oom(fd);
	}

	for (const struct vm_get_property *f = xe_vm_get_property_tests; f->name; f++) {
		igt_subtest_f("vm-get-property-%s", f->name)
			test_get_property(fd, f->test);
	}

	for (int i = 0; overcommit_cases[i].name; i++) {
		const struct vm_overcommit_case *c = &overcommit_cases[i];
		const char *mode = (c->vm_flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE) ?
					"fault" : "nonfault";
		igt_subtest_f("overcommit-%s-%s", mode, c->name) {
			uint64_t vram_size, vram_mb, avail_ram_mb, ram_mb, required_mb;
			uint64_t overcommit_size, total_bo_size;
			int num_bos;

			vram_size = xe_visible_vram_size(fd, 0);
			igt_require(vram_size);
			vram_mb = vram_size >> 20;
			avail_ram_mb = igt_get_avail_ram_mb();
			ram_mb = igt_get_total_ram_mb();

			overcommit_size = ALIGN(vram_size * c->overcommit_mult, 4096);
			num_bos = overcommit_size / OVERCOMMIT_BO_SIZE + 1;
			total_bo_size = (uint64_t)num_bos * OVERCOMMIT_BO_SIZE;

			igt_assert_lt_u64(vram_size, total_bo_size);

			igt_debug("Overcommit %s-%s: mult=%d ram=%" PRIu64 "MiB\n",
				  mode, c->name, c->overcommit_mult, ram_mb);
			igt_debug("Overcommit avail_ram=%" PRIu64 "MiB vram=%" PRIu64 "MiB\n",
				  avail_ram_mb, vram_mb);
			igt_debug("Overcommit size=%" PRIu64 "MiB num_bos=%d\n",
				  overcommit_size >> 20, num_bos);
			igt_debug("Overcommit bo_size=%dMiB total=%" PRIu64 "MiB\n",
				  OVERCOMMIT_BO_SIZE >> 20, total_bo_size >> 20);

			required_mb = ALIGN(DIV_ROUND_UP(total_bo_size - vram_size,
							 SZ_1M), 64) + 64;
			igt_require_f(required_mb <= avail_ram_mb * 50 / 100,
				      "Need %" PRIu64 "MiB, > 50%% of avail RAM %" PRIu64 "MiB\n",
				      required_mb, avail_ram_mb);
			test_vm_overcommit(fd, hwe_non_copy, c, num_bos);
		}
	}

	igt_fixture()
		drm_close_driver(fd);
}
