// SPDX-License-Identifier: MIT
/*
 * Remote cache invalidation and map/unmap tests for AMD multi-GPU
 *
 * Contains two tests:
 * 1. remote-cache-invalidate: Tests cache coherency across GPUs
 * 2. map-unmap-to-nodes: Tests memory mapping stability under stress
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
#include "lib/amdgpu/compute_utils/amd_dispatch_helpers.h"
#include "amdgpu/shaders/amd_shader_store.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"
#include "igt.h"
#include "igt_multigpu.h"

#include <libdrm/amdgpu.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#define POLL_PATTERN 0x5678
#define DW_LOCATION 100
#define SRC_LOCATION 0
#define MAX_ITERATIONS (1 << 10)  /* Reduced for debugging */

/* Forward declarations */
static void remote_cache_invalidate_test(amdgpu_device_handle *devices, int device_count);
static void map_unmap_to_nodes_test(amdgpu_device_handle *devices, int device_count);

/* Assemble PollMemoryIsa into a shader BO */
static int build_poll_shader(amdgpu_device_handle dev,
			     const struct amdgpu_gpu_info *info,
			     amdgpu_bo_handle *bo_shader,
			     uint64_t *mc_shader,
			     amdgpu_va_handle *va_shader,
			     void **cpu_shader,
			     uint32_t *out_version)
{
	uint8_t isa_bin[4096];
	size_t isa_sz = 0;
	char mcpu[32];
	int r;
	uint32_t version;
	struct drm_amdgpu_info_hw_ip hw_ip_info;

	amdgpu_family_id_to_mcpu(info->family_id, mcpu, sizeof(mcpu));
	r = amdgpu_query_hw_ip_info(dev, AMDGPU_HW_IP_COMPUTE, 0, &hw_ip_info);
	igt_assert_eq(r, 0);
	version = hw_ip_info.hw_ip_version_major;

	r = amdgpu_llvm_asm_init();
	if (r)
		return r;
	r = amdgpu_llvm_assemble(mcpu, PollMemoryIsa, isa_bin, sizeof(isa_bin), &isa_sz);
	amdgpu_llvm_asm_shutdown();
	if (r || !isa_sz)
		return -EINVAL;

	r = amdgpu_bo_alloc_and_map(dev, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				bo_shader, cpu_shader, mc_shader, va_shader);
	if (r)
		return r;
	memset(*cpu_shader, 0, 4096);
	memcpy(*cpu_shader, isa_bin, isa_sz);
	*out_version = version;
	return 0;
}

/* Submit polling dispatch: polls src addr until POLL_PATTERN then writes pattern to dst */
static int submit_poll_dispatch(amdgpu_device_handle dev,
				uint32_t version,
				uint64_t mc_shader,
				uint64_t src_va,
				uint64_t dst_va,
				amdgpu_bo_handle bo_shader,
				amdgpu_bo_handle bo_target,
				amdgpu_context_handle *out_ctx,
				uint64_t *out_seq)
{
	int r;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	amdgpu_bo_handle bo_cmd = NULL;
	amdgpu_va_handle va_cmd = 0;
	uint64_t mc_cmd = 0;
	uint32_t *ptr_cmd = NULL;
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_request ib_req = {0};
	amdgpu_context_handle ctx;
	amdgpu_bo_handle bos[3];

	r = amdgpu_cs_ctx_create(dev, &ctx);
	if (r)
		return r;

	r = amdgpu_bo_alloc_and_map(dev, 4096, 4096,
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

	/* SGPRs: src_va, dst_va */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240); /* first user data SGPR */
	base_cmd->emit(base_cmd, src_va & 0xffffffff);
	base_cmd->emit(base_cmd, src_va >> 32);
	base_cmd->emit(base_cmd, dst_va & 0xffffffff);
	base_cmd->emit(base_cmd, dst_va >> 32);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10); /* grid dims (arbitrary small) */
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_target;
	r = amdgpu_bo_list_create(dev, 3, bos, NULL, &bo_list);
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

	r = amdgpu_cs_submit(ctx, 0, &ib_req, 1);
	if (r) {
		amdgpu_bo_list_destroy(bo_list);
		amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
		amdgpu_cs_ctx_free(ctx);
		return r;
	}

	*out_ctx = ctx;
	*out_seq = ib_req.seq_no;

	/* Keep command buffer and list until fence signaled */
	amdgpu_bo_list_destroy(bo_list); /* list can be destroyed after submit */
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
	free_cmd_base(base_cmd);
	return 0;
}

/* Submit polling dispatch with two buffers (for map/unmap test) */
static int submit_poll_dispatch_two_buffers(amdgpu_device_handle dev,
					    uint32_t version,
					   uint64_t mc_shader,
					   uint64_t src_va,
					   uint64_t dst_va,
					   amdgpu_bo_handle bo_shader,
					   amdgpu_bo_handle bo_src,
					   amdgpu_bo_handle bo_dst,
					   amdgpu_context_handle *out_ctx,
					   uint64_t *out_seq)
{
	int r;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	amdgpu_bo_handle bo_cmd = NULL;
	amdgpu_va_handle va_cmd = 0;
	uint64_t mc_cmd = 0;
	uint32_t *ptr_cmd = NULL;
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_request ib_req = {0};
	amdgpu_context_handle ctx;
	amdgpu_bo_handle bos[4]; /* cmd, shader, src, dst */

	r = amdgpu_cs_ctx_create(dev, &ctx);
	if (r)
		return r;

	r = amdgpu_bo_alloc_and_map(dev, 4096, 4096,
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

	/* SGPRs: src_va, dst_va */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240); /* first user data SGPR */
	base_cmd->emit(base_cmd, src_va & 0xffffffff);
	base_cmd->emit(base_cmd, src_va >> 32);
	base_cmd->emit(base_cmd, dst_va & 0xffffffff);
	base_cmd->emit(base_cmd, dst_va >> 32);

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10); /* grid dims (arbitrary small) */
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_src;
	bos[3] = bo_dst;
	r = amdgpu_bo_list_create(dev, 4, bos, NULL, &bo_list);
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

	r = amdgpu_cs_submit(ctx, 0, &ib_req, 1);
	if (r) {
		amdgpu_bo_list_destroy(bo_list);
		amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
		amdgpu_cs_ctx_free(ctx);
		return r;
	}

	*out_ctx = ctx;
	*out_seq = ib_req.seq_no;

	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
	free_cmd_base(base_cmd);
	return 0;
}

static void remote_cache_invalidate_test(amdgpu_device_handle *devices, int device_count)
{
	int r;
	struct amdgpu_gpu_info infoA = {0}, infoB = {0};
	amdgpu_bo_handle boA = NULL; /* target buffer on GPU A */
	amdgpu_va_handle vaA = 0;
	uint64_t mcA = 0;
	void *cpuA = NULL;
	amdgpu_bo_handle boShaderA = NULL;
	amdgpu_va_handle vaShaderA = 0;
	uint64_t mcShaderA = 0;
	void *cpuShaderA = NULL;
	uint32_t versionA = 0;
	amdgpu_context_handle ctxA;
	uint64_t seqA = 0;
	struct amdgpu_cs_fence fenceA = {0};
	uint32_t expired = 0;
	/* Declarations (C90) for later use */
	uint64_t src_va = 0, dst_va = 0;
	uint32_t prime_handle = 0; /* exported shared handle (dma-buf fd) */
	int prime_fd = -1;         /* signed fd for close() */
	struct amdgpu_bo_import_result import_res; /* import result */
	void *cpuB = NULL;
	volatile uint32_t *remote_buf = NULL;
	uint32_t *bufA = NULL;

	igt_require_f(device_count >= 2, "Need >=2 AMD GPUs for remote test\n");

	/* Query GPU info for gating */
	r = amdgpu_query_gpu_info(devices[0], &infoA); igt_require(r == 0);
	r = amdgpu_query_gpu_info(devices[1], &infoB); igt_require(r == 0);

	/* Family gate: require AI (Arcturus-like) on both for closer semantics */
	if (!FAMILY_IS_AI(infoA.family_id) || !FAMILY_IS_AI(infoB.family_id)) {
		igt_skip("Skipping: requires two AI/Arcturus family GPUs\n");
		return;
	}

	/* Allocate host-accessible VRAM on GPU A */
	r = amdgpu_bo_alloc_and_map_sync(devices[0],
					 PAGE_SIZE, 4096,
				     AMDGPU_GEM_DOMAIN_VRAM,
				     AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				     AMDGPU_VM_MTYPE_RW,
				     &boA, &cpuA, &mcA, &vaA,
				     0, 0, true);
	igt_assert_eq(r, 0);
	memset(cpuA, 0, PAGE_SIZE);

	/* Build poll shader */
	r = build_poll_shader(devices[0], &infoA, &boShaderA, &mcShaderA, &vaShaderA, &cpuShaderA, &versionA);
	igt_assert_eq(r, 0);

	/* Dispatch polling compute on GPU A */
	src_va = mcA + SRC_LOCATION * sizeof(uint32_t);
	dst_va = mcA + DW_LOCATION * sizeof(uint32_t);
	r = submit_poll_dispatch(devices[0], versionA, mcShaderA, src_va, dst_va, boShaderA, boA, &ctxA, &seqA);
	igt_assert_eq(r, 0);

	/* Export boA as dma-buf and import into GPU B */
	/* Export BO as dma-buf fd (older libdrm signature: (bo, type, &shared_handle)) */
	r = amdgpu_bo_export(boA, amdgpu_bo_handle_type_dma_buf_fd, &prime_handle);
	igt_assert_eq(r, 0);
	prime_fd = (int)prime_handle;
	igt_assert(prime_fd >= 0);

	memset(&import_res, 0, sizeof(import_res));
	r = amdgpu_bo_import(devices[1], amdgpu_bo_handle_type_dma_buf_fd, prime_fd, &import_res);
	igt_assert_eq(r, 0);

	/* Map on GPU B (remote) and write trigger pattern */
	r = amdgpu_bo_cpu_map(import_res.buf_handle, &cpuB);
	igt_assert_eq(r, 0);
	remote_buf = (volatile uint32_t *)cpuB;
	remote_buf[SRC_LOCATION] = POLL_PATTERN; /* remote write */
	amdgpu_bo_cpu_unmap(import_res.buf_handle);

	/* Wait for compute shader fence on GPU A */
	fenceA.context = ctxA;
	fenceA.ip_type = AMDGPU_HW_IP_COMPUTE;
	fenceA.ip_instance = 0;
	fenceA.ring = 0;
	fenceA.fence = seqA;
	r = amdgpu_cs_query_fence_status(&fenceA, AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_f(expired, "Compute fence not signaled\n");

	/* Validate result */
	bufA = (uint32_t *)cpuA;
	igt_assert_f(bufA[DW_LOCATION] == POLL_PATTERN,
		     "Remote cache invalidate failed: expected 0x%x got 0x%x\n",
	 POLL_PATTERN, bufA[DW_LOCATION]);
	igt_info("remote-cache-invalidate: PASSED (dst[%d]=0x%x)\n", DW_LOCATION, bufA[DW_LOCATION]);

	/* Cleanup */
	close(prime_fd);
	amdgpu_bo_free(import_res.buf_handle);

	amdgpu_cs_ctx_free(ctxA);
	amdgpu_bo_unmap_and_free(boShaderA, vaShaderA, mcShaderA, 4096);
	amdgpu_bo_unmap_and_free(boA, vaA, mcA, PAGE_SIZE);
}

static void map_unmap_to_nodes_test(amdgpu_device_handle *devices, int device_count)
{
	int r;
	struct amdgpu_gpu_info infoA = {0}, infoB = {0};

	/* GPU A resources (default node) */
	amdgpu_bo_handle boSrcA = NULL, boDstA = NULL, boShaderA = NULL;
	amdgpu_va_handle vaSrcA = 0, vaDstA = 0, vaShaderA = 0;
	uint64_t mcSrcA = 0, mcDstA = 0, mcShaderA = 0;
	void *cpuSrcA = NULL, *cpuDstA = NULL, *cpuShaderA = NULL;
	uint32_t versionA = 0;
	amdgpu_context_handle ctxA = NULL;
	uint64_t seqA = 0;
	struct amdgpu_cs_fence fenceA = {0};
	uint32_t expired = 0;

	/* GPU B resources (non-default node) */
	uint32_t prime_handle_src = 0, prime_handle_dst = 0;
	int prime_fd_src = -1, prime_fd_dst = -1;
	struct amdgpu_bo_import_result import_src, import_dst;
	void *cpuSrcB = NULL, *cpuDstB = NULL;

	uint64_t src_va = 0, dst_va = 0;
	volatile uint32_t *src_val = NULL, *dst_val = NULL;
	unsigned int i;
	bool wait_result;

	igt_require_f(device_count >= 2, "Need >=2 AMD GPUs for map/unmap test\n");

	/* Query GPU info */
	r = amdgpu_query_gpu_info(devices[0], &infoA);
	igt_require(r == 0);
	r = amdgpu_query_gpu_info(devices[1], &infoB);
	igt_require(r == 0);

	/* Require GFX9+ on default node (GPU A) - corrected condition */
	if (infoA.family_id < FAMILY_AI) {
		igt_skip("Skipping: requires GFX9+ AI family GPU as default node\n");
		return;
	}

	igt_info("Starting map-unmap test: GPU A (default) will run shader, GPU B will map/unmap\n");
	igt_info("GPU A family: 0x%x, GPU B family: 0x%x\n", infoA.family_id, infoB.family_id);

	/* Allocate source and destination buffers on GPU A */
	r = amdgpu_bo_alloc_and_map_sync(devices[0], PAGE_SIZE, 4096,
					 AMDGPU_GEM_DOMAIN_VRAM,
				    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				    AMDGPU_VM_MTYPE_RW,
				    &boSrcA, &cpuSrcA, &mcSrcA, &vaSrcA,
				    0, 0, true);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map_sync(devices[0], PAGE_SIZE, 4096,
					 AMDGPU_GEM_DOMAIN_VRAM,
				    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				    AMDGPU_VM_MTYPE_RW,
				    &boDstA, &cpuDstA, &mcDstA, &vaDstA,
				    0, 0, true);
	igt_assert_eq(r, 0);

	/* Initialize buffers to 0 */
	memset(cpuSrcA, 0, PAGE_SIZE);
	memset(cpuDstA, 0, PAGE_SIZE);

	igt_info("Buffers allocated and initialized to 0\n");
	igt_info("Source buffer MC address: 0x%lx, Destination buffer MC address: 0x%lx\n", mcSrcA, mcDstA);

	/* Build poll shader on GPU A */
	r = build_poll_shader(devices[0], &infoA, &boShaderA, &mcShaderA,
			      &vaShaderA, &cpuShaderA, &versionA);
	igt_assert_eq(r, 0);
	igt_info("Poll shader built successfully, version: %u, MC address: 0x%lx\n", versionA, mcShaderA);

	/* Submit polling compute on GPU A */
	src_va = mcSrcA;
	dst_va = mcDstA;
	igt_info("Submitting compute shader: src_va=0x%lx, dst_va=0x%lx\n", src_va, dst_va);

	r = submit_poll_dispatch_two_buffers(devices[0], versionA, mcShaderA, src_va, dst_va,
					     boShaderA, boSrcA, boDstA, &ctxA, &seqA);
	igt_assert_eq(r, 0);
	igt_info("Compute shader submitted on GPU A, sequence: %lu\n", seqA);

	/* Immediately check if shader is running by checking fence status */
	fenceA.context = ctxA;
	fenceA.ip_type = AMDGPU_HW_IP_COMPUTE;
	fenceA.ip_instance = 0;
	fenceA.ring = 0;
	fenceA.fence = seqA;

	r = amdgpu_cs_query_fence_status(&fenceA, 0, 0, &expired);
	igt_info("Initial fence status: query_result=%d, expired=%d\n", r, expired);

	/* Check buffer values immediately after submission */
	dst_val = (volatile uint32_t *)cpuDstA;
	src_val = (volatile uint32_t *)cpuSrcA;
	igt_info("Buffer values after submission: src=0x%x, dst=0x%x\n", *src_val, *dst_val);

	/* Let the shader run for a short time before starting cross-GPU operations */
	usleep(100000); /* 100ms */

	r = amdgpu_cs_query_fence_status(&fenceA, 0, 0, &expired);
	igt_info("Fence status after 100ms: query_result=%d, expired=%d\n", r, expired);
	igt_info("Buffer values after 100ms: src=0x%x, dst=0x%x\n", *src_val, *dst_val);

	/* If fence is already expired, the shader completed immediately - this indicates a problem */
	if (expired) {
		igt_info("WARNING: Compute fence expired immediately - shader may not have run correctly\n");
		igt_info("This could indicate: 1) Shader compilation issue, 2) Invalid addresses, 3) Shader too simple\n");
	}

	/* Export buffers from GPU A and import to GPU B */
	r = amdgpu_bo_export(boSrcA, amdgpu_bo_handle_type_dma_buf_fd, &prime_handle_src);
	igt_assert_eq(r, 0);
	prime_fd_src = (int)prime_handle_src;

	r = amdgpu_bo_export(boDstA, amdgpu_bo_handle_type_dma_buf_fd, &prime_handle_dst);
	igt_assert_eq(r, 0);
	prime_fd_dst = (int)prime_handle_dst;

	memset(&import_src, 0, sizeof(import_src));
	r = amdgpu_bo_import(devices[1], amdgpu_bo_handle_type_dma_buf_fd,
			     prime_fd_src, &import_src);
	igt_assert_eq(r, 0);

	memset(&import_dst, 0, sizeof(import_dst));
	r = amdgpu_bo_import(devices[1], amdgpu_bo_handle_type_dma_buf_fd,
			     prime_fd_dst, &import_dst);
	igt_assert_eq(r, 0);

	igt_info("Buffers exported from GPU A and imported to GPU B\n");

	/* The key difference: In KFD test, they use hsaKmtMapMemoryToGPUNodes to
	* repeatedly map/unmap between GPU nodes. In IGT, we simulate this by
	* repeatedly importing/exporting the buffers between GPUs.
	*/
	igt_info("Starting cross-GPU mapping iterations...\n");

	/* Repeatedly import/export between GPUs while shader is running */
	for (i = 0; i < MAX_ITERATIONS; i++) {
		/* Free the imported buffers on GPU B */
		amdgpu_bo_free(import_src.buf_handle);
		amdgpu_bo_free(import_dst.buf_handle);

		/* Re-import the buffers to GPU B */
		memset(&import_src, 0, sizeof(import_src));
		r = amdgpu_bo_import(devices[1], amdgpu_bo_handle_type_dma_buf_fd,
				     prime_fd_src, &import_src);
		igt_assert_eq(r, 0);

		memset(&import_dst, 0, sizeof(import_dst));
		r = amdgpu_bo_import(devices[1], amdgpu_bo_handle_type_dma_buf_fd,
				     prime_fd_dst, &import_dst);
		igt_assert_eq(r, 0);

		/* Map and immediately unmap on GPU B to simulate the mapping behavior */
		r = amdgpu_bo_cpu_map(import_src.buf_handle, &cpuSrcB);
		igt_assert_eq(r, 0);
		amdgpu_bo_cpu_unmap(import_src.buf_handle);

		r = amdgpu_bo_cpu_map(import_dst.buf_handle, &cpuDstB);
		igt_assert_eq(r, 0);
		amdgpu_bo_cpu_unmap(import_dst.buf_handle);

		/* Every 32 iterations, check if shader is still running */
		if ((i & 0x1F) == 0) {
			    /* Check fence status */
			    r = amdgpu_cs_query_fence_status(&fenceA, 0, 0, &expired);

			    /* Quick check - if dst buffer changed, shader might have completed */
			    dst_val = (volatile uint32_t *)cpuDstA;
			if (*dst_val == POLL_PATTERN) {
				igt_info("Shader completed early at iteration %u\n", i);
				break;
			}
			    igt_info("Iteration %u: fence_expired=%d, dst buffer=0x%x\n", i, expired, *dst_val);

			    /* If fence expired but dst buffer is still 0, shader didn't work */
			if (expired && *dst_val == 0) {
				igt_info("WARNING: Fence expired but shader didn't produce expected result at iteration %u\n", i);
			}
		}
	}

	igt_info("Cross-GPU mapping iterations completed, signaling shader to quit\n");

	/* Signal shader to quit by writing pattern to source buffer */
	src_val = (volatile uint32_t *)cpuSrcA;
	*src_val = POLL_PATTERN;
	igt_info("Wrote 0x%x to source buffer\n", POLL_PATTERN);

	/* Wait for shader to complete (write to dst buffer) using existing wait_on_value */
	igt_info("Waiting for shader to complete...\n");
	wait_result = wait_on_value(cpuDstA, POLL_PATTERN);

	if (!wait_result) {
		igt_info("Shader did not complete. This might indicate a GPU VM fault.\n");
		/* Check current buffer values */
		dst_val = (volatile uint32_t *)cpuDstA;
		src_val = (volatile uint32_t *)cpuSrcA;
		igt_info("Final buffer values: src=0x%x, dst=0x%x\n", *src_val, *dst_val);

		/* Check fence status */
		fenceA.context = ctxA;
		fenceA.ip_type = AMDGPU_HW_IP_COMPUTE;
		fenceA.ip_instance = 0;
		fenceA.ring = 0;
		fenceA.fence = seqA;
		r = amdgpu_cs_query_fence_status(&fenceA, 0, 0, &expired);
		igt_info("Final fence status: query_result=%d, expired=%d\n", r, expired);

		if (expired && *dst_val == 0) {
			igt_info("CONCLUSION: Fence completed but shader produced no result - shader execution failed\n");
			igt_info("Possible causes:\n");
			igt_info("1. Shader compilation failed\n");
			igt_info("2. Invalid memory addresses passed to shader\n");
			igt_info("3. Shader code has bugs\n");
			igt_info("4. GPU hardware issue\n");
		}
	}

	igt_assert_f(wait_result, "Timeout waiting for shader completion - possible GPU VM fault\n");

	/* Wait for compute fence */
	fenceA.context = ctxA;
	fenceA.ip_type = AMDGPU_HW_IP_COMPUTE;
	fenceA.ip_instance = 0;
	fenceA.ring = 0;
	fenceA.fence = seqA;
	r = amdgpu_cs_query_fence_status(&fenceA, AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_f(expired, "Compute fence not signaled\n");

	/* Validate result */
	dst_val = (volatile uint32_t *)cpuDstA;
	igt_assert_f(*dst_val == POLL_PATTERN,
		     "Map/unmap test failed: expected 0x%x got 0x%x\n",
		 POLL_PATTERN, *dst_val);

	igt_info("map-unmap-to-nodes: PASSED (iterations=%u, dst=0x%x)\n",
		 i, *dst_val);

	/* Cleanup */
	amdgpu_bo_free(import_src.buf_handle);
	amdgpu_bo_free(import_dst.buf_handle);

	close(prime_fd_src);
	close(prime_fd_dst);

	amdgpu_cs_ctx_free(ctxA);
	amdgpu_bo_unmap_and_free(boShaderA, vaShaderA, mcShaderA, 4096);
	amdgpu_bo_unmap_and_free(boSrcA, vaSrcA, mcSrcA, PAGE_SIZE);
	amdgpu_bo_unmap_and_free(boDstA, vaDstA, mcDstA, PAGE_SIZE);
}

int igt_main() {
	static int fds[8];
	static int fd_count;
	static amdgpu_device_handle devices[8];
	uint32_t majors[8] = {0}, minors[8] = {0};
	int i;
	struct pci_addr pci_addr;
	int ret;

	igt_fixture() {
		memset(fds, -1, sizeof(fds));

		/* Use amdgpu_open_devices from IGT lib to open AMD GPU devices */
		fd_count = amdgpu_open_devices(true, 8, fds);
		igt_require_f(fd_count >= 2, "Need >=2 AMD GPUs for remote test\n");

		for (i = 0; i < fd_count; i++) {
			igt_require(amdgpu_device_initialize(fds[i], &majors[i], &minors[i], &devices[i]) == 0);

			/* Get and print PCI bus info for each device */
			ret = get_pci_addr_from_fd(fds[i], &pci_addr);
			if (ret == 0) {
			igt_info("Initialized GPU %d: version %d.%d, PCI %04x:%02x:%02x.%x\n",
				 i, majors[i], minors[i],
				 pci_addr.domain, pci_addr.bus, pci_addr.device, pci_addr.function);
			} else {
				igt_info("Initialized GPU %d: version %d.%d, PCI info unavailable\n",
					 i, majors[i], minors[i]);
			}
		}
	}

	igt_subtest("remote-cache-invalidate") {
		remote_cache_invalidate_test(devices, fd_count);
	}

	igt_subtest("map-unmap-to-nodes") {
		map_unmap_to_nodes_test(devices, fd_count);
	}

	igt_fixture() {
		for (i = 0; i < fd_count; i++) {
			if (devices[i]) {
			amdgpu_device_deinitialize(devices[i]);
			}
			if (fds[i] >= 0) {
			close(fds[i]);
			}
		}
	}
}
