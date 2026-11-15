// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <fcntl.h>
#include <glob.h>

#include "amd_memory.h"
#include "amd_ip_blocks.h"
#include "amd_PM4.h"
#include "amd_sdma.h"
#include <amdgpu.h>


#include <amdgpu_drm.h>
#include "amdgpu_asic_addr.h"
#include "amd_gfx_v8_0.h"
#include "ioctl_wrappers.h"

static void amdgpu_ib_fill_nop_range(uint32_t *ib, uint32_t start_dw, uint32_t end_dw)
{
	uint32_t remaining = end_dw - start_dw;

	while (remaining >= 2) {
		uint32_t packet_dw = remaining > 0x4001 ? 0x4001 : remaining;
		uint32_t payload_dw = packet_dw - 1;

		ib[start_dw++] = PACKET3(PACKET3_NOP, packet_dw - 2);
		while (payload_dw > 0) {
			ib[start_dw++] = 0;
			payload_dw--;
		}
		remaining = end_dw - start_dw;
	}
	if (remaining)
		ib[start_dw] = CP_PACKET2;
}

static void amdgpu_ib_fill_nop(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords)
{
	(void)base;
	amdgpu_ib_fill_nop_range(ib, 0, ib_dwords);
}

static void amdgpu_ib_fill_chain(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
				uint64_t next_va, uint32_t next_size_dw)
{
	uint32_t last;
	(void)base;
	igt_assert(ib_dwords >= 4);
	last = ib_dwords - 4;
	amdgpu_ib_fill_nop_range(ib, 0, last);
	ib[last] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
	ib[last + 1] = lower_32_bits(next_va);
	ib[last + 2] = upper_32_bits(next_va);
	ib[last + 3] = (next_size_dw & INDIRECT_BUFFER_SIZE_MASK) |
		       INDIRECT_BUFFER_VALID | INDIRECT_BUFFER_CHAIN;
}

static void amdgpu_ib_fill_writedata(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
				     uint64_t dst_va, uint32_t dst_size)
{
	uint32_t off = 0;
	uint32_t dst_off = 0;
	const uint32_t packet_dwords = 8;

	(void)base;
	while (off + packet_dwords <= ib_dwords) {
		uint64_t addr = dst_va + (dst_off % dst_size);

		ib[off] = PACKET3(PACKET3_WRITE_DATA, 6);
		ib[off + 1] = WRITE_DATA_DST_SEL(5);
		ib[off + 2] = lower_32_bits(addr & ~0x3ULL);
		ib[off + 3] = upper_32_bits(addr);
		ib[off + 4] = 0xDEADBEEF;
		ib[off + 5] = 0xCAFEBABE;
		ib[off + 6] = 0x12345678;
		ib[off + 7] = 0xABCD0000 | (off & 0xFFFF);
		off += packet_dwords;
		dst_off += 16;
	}
	amdgpu_ib_fill_nop_range(ib, off, ib_dwords);
}

static void amdgpu_ib_fill_writedata_chain(struct amdgpu_cmd_base *base, uint32_t *ib,
					   uint32_t ib_dwords, uint64_t dst_va,
					   uint32_t dst_size, uint64_t next_va,
					   uint32_t next_size_dw)
{
	uint32_t chain_dw;

	igt_assert(ib_dwords >= 12);
	chain_dw = ib_dwords - 4;
	amdgpu_ib_fill_writedata(base, ib, chain_dw, dst_va, dst_size);
	ib[chain_dw] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
	ib[chain_dw + 1] = lower_32_bits(next_va);
	ib[chain_dw + 2] = upper_32_bits(next_va);
	ib[chain_dw + 3] = (next_size_dw & INDIRECT_BUFFER_SIZE_MASK) |
			   INDIRECT_BUFFER_VALID | INDIRECT_BUFFER_CHAIN;
}

static void amdgpu_ib_fill_wait(struct amdgpu_cmd_base *base, uint32_t *ib, uint32_t ib_dwords,
				uint64_t signal_va, uint64_t next_ib_va, uint32_t next_ib_size_dw)
{
	uint32_t chain_dw;

	(void)base;
	igt_assert(ib_dwords >= 11);
	chain_dw = ib_dwords - 4;
	ib[0] = PACKET3(PACKET3_WAIT_REG_MEM, 5);
	ib[1] = WAIT_REG_MEM_FUNCTION(3) | WAIT_REG_MEM_MEM_SPACE(1) |
		WAIT_REG_MEM_ENGINE(0);
	ib[2] = lower_32_bits(signal_va);
	ib[3] = upper_32_bits(signal_va);
	ib[4] = 1;
	ib[5] = 0xFFFFFFFF;
	ib[6] = 0x00000004;
	amdgpu_ib_fill_nop_range(ib, 7, chain_dw);
	ib[chain_dw] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
	ib[chain_dw + 1] = lower_32_bits(next_ib_va);
	ib[chain_dw + 2] = upper_32_bits(next_ib_va);
	ib[chain_dw + 3] = (next_ib_size_dw & INDIRECT_BUFFER_SIZE_MASK) |
			   INDIRECT_BUFFER_VALID | INDIRECT_BUFFER_CHAIN;
}

/*
 * SDMA functions:
 * - write_linear
 * - const_fill
 * - copy_linear
 */
static int
sdma_ring_write_linear(const struct amdgpu_ip_funcs *func,
		       const struct amdgpu_ring_context *ring_context,
		       uint32_t *pm4_dw)
{
	uint32_t i, j;
	uint32_t fill = ring_context->write_data_valid ?
			ring_context->write_data : func->deadbeaf;

	i = 0;
	j = 0;

	/* Guard: write_length must be DWORD-aligned (bytes) */
	igt_assert_f(ring_context->write_length % 4 == 0,
		     "SDMA write_linear: write_length %lu not DWORD-aligned\n",
		     (unsigned long)ring_context->write_length);
	if (func->family_id == AMDGPU_FAMILY_SI)
		ring_context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_WRITE, 0, 0, 0,
					 ring_context->write_length / 4);
	else
		ring_context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_WRITE,
					 SDMA_WRITE_SUB_OPCODE_LINEAR,
					 ring_context->secure ? SDMA_ATOMIC_TMZ(1) : 0);

	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	if (func->family_id >= AMDGPU_FAMILY_AI)
		ring_context->pm4[i++] = ring_context->write_length / 4 - 1;
	else
		ring_context->pm4[i++] = ring_context->write_length / 4;

	if (ring_context->hw_ip_info.hw_ip_version_major >= 8)
		ring_context->pm4[i++] = 0;

	while (j++ < ring_context->write_length / 4)
		ring_context->pm4[i++] = fill;

	*pm4_dw = i;

	/* Guard: ensure PM4 packet fits in allocated buffer */
	if (ring_context->pm4_size > 0)
		igt_assert_f(*pm4_dw <= ring_context->pm4_size,
			     "SDMA write_linear: pm4_dw %u exceeds buffer %u\n",
			     *pm4_dw, ring_context->pm4_size);

	return 0;
}

static int
sdma_ring_bad_write_linear(const struct amdgpu_ip_funcs *func,
		       const struct amdgpu_ring_context *ring_context,
		       uint32_t *pm4_dw, unsigned int cmd_error)
{
	uint32_t i, j, stream_length;
	uint32_t opcode;

	i = 0;
	j = 0;

	if (cmd_error == CMD_STREAM_EXEC_INVALID_PACKET_LENGTH)
		stream_length = ring_context->write_length / 4 / 16;
	else
		stream_length = ring_context->write_length / 4;

	if (cmd_error == CMD_STREAM_EXEC_INVALID_OPCODE)
		opcode = 0xf2;
	else
		opcode = SDMA_OPCODE_WRITE;

	if (func->family_id == AMDGPU_FAMILY_SI)
		ring_context->pm4[i++] = SDMA_PACKET_SI(opcode, 0, 0, 0,
					 ring_context->write_length / 4);
	else
		ring_context->pm4[i++] = SDMA_PACKET(opcode,
					 SDMA_WRITE_SUB_OPCODE_LINEAR,
					 ring_context->secure ? SDMA_ATOMIC_TMZ(1) : 0);
	if (cmd_error == CMD_STREAM_TRANS_BAD_MEM_ADDRESS) {
		ring_context->pm4[i++] = lower_32_bits(0xdeadbee0);
		ring_context->pm4[i++] = upper_32_bits(0xdeadbee0);
	} else if (cmd_error == CMD_STREAM_TRANS_BAD_REG_ADDRESS) {
		ring_context->pm4[i++] = lower_32_bits(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
		ring_context->pm4[i++] = upper_32_bits(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
	} else {
		ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	}
	if (func->family_id >= AMDGPU_FAMILY_AI)
		ring_context->pm4[i++] = ring_context->write_length / 4 - 1;
	else
		ring_context->pm4[i++] = ring_context->write_length / 4;

	if (ring_context->hw_ip_info.hw_ip_version_major >= 8)
		ring_context->pm4[i++] = 0;

	while (j++ < stream_length)
		ring_context->pm4[i++] = func->deadbeaf;
	*pm4_dw = i;

	return 0;
}

static int
sdma_ring_atomic(const struct amdgpu_ip_funcs *func,
		       const struct amdgpu_ring_context *ring_context,
		       uint32_t *pm4_dw)
{
	uint32_t i = 0;

	memset(ring_context->pm4, 0, ring_context->pm4_size * sizeof(uint32_t));

		/* atomic opcode for 32b w/ RTN and ATOMIC_SWAPCMP_RTN
		 * loop, 1-loop_until_compare_satisfied.
		 * single_pass_atomic, 0-lru
		 */
	ring_context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_ATOMIC,
				       0,
				       SDMA_ATOMIC_LOOP(1) |
				       (ring_context->secure ? SDMA_ATOMIC_TMZ(1) : SDMA_ATOMIC_TMZ(0)) |
				       SDMA_ATOMIC_OPCODE(TC_OP_ATOMIC_CMPSWAP_RTN_32));
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 0x12345678;
	ring_context->pm4[i++] = 0x0;
	ring_context->pm4[i++] = func->deadbeaf;
	ring_context->pm4[i++] = 0x0;
	ring_context->pm4[i++] = 0x100;
	*pm4_dw = i;

	return 0;

}

static int
sdma_ring_const_fill(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *context,
		     uint32_t *pm4_dw)
{
	uint32_t i;
	uint32_t fill = context->write_data_valid ?
			context->write_data : func->deadbeaf;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_CONSTANT_FILL_SI,
						   0, 0, 0, context->write_length / 4);
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = fill;
		context->pm4[i++] = upper_32_bits(context->bo_mc) >> 16;
	} else {
		context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_CONSTANT_FILL, 0,
						SDMA_CONSTANT_FILL_EXTRA_SIZE(2));
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = upper_32_bits(context->bo_mc);
		context->pm4[i++] = fill;

		if (func->family_id >= AMDGPU_FAMILY_AI)
			context->pm4[i++] = context->write_length - 1;
		else
			context->pm4[i++] = context->write_length;
	}

	if (context->hw_ip_info.hw_ip_version_major >= 8)
		context->pm4[i++] = 0;

	*pm4_dw = i;

	return 0;
}

static int
sdma_ring_copy_linear(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *context,
		      uint32_t *pm4_dw)
{
	uint32_t i;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_COPY_SI,
					  0, 0, 0,
					  context->write_length);
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = upper_32_bits(context->bo_mc);
		context->pm4[i++] = lower_32_bits(context->bo_mc2);
		context->pm4[i++] = upper_32_bits(context->bo_mc2);
	} else {
		context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_COPY,
				       SDMA_COPY_SUB_OPCODE_LINEAR,
					context->secure ? 0x4 : 0);
		if (func->family_id >= AMDGPU_FAMILY_AI) {
			/* For mi100, the maximum copy range supported by sdma is 4MB */
			if (func->family_id == AMDGPU_FAMILY_AI && func->chip_external_rev == 0x33
				&& context->write_length > 0x3fffff) {
				context->pm4[i++] = 0x3fffff;
				igt_warn("sdma copy count exceeds the maximum limit of 4MB\n");
			} else {
				context->pm4[i++] = context->write_length - 1;
			}
		} else {
			context->pm4[i++] = context->write_length;
		}

		if (context->hw_ip_info.hw_ip_version_major >= 8)
			context->pm4[i++] = 0;

		context->pm4[i++] = 0;
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = upper_32_bits(context->bo_mc);
		context->pm4[i++] = lower_32_bits(context->bo_mc2);
		context->pm4[i++] = upper_32_bits(context->bo_mc2);
	}

	*pm4_dw = i;

	return 0;
}

/*
 * GFX and COMPUTE functions:
 * - write_linear
 * - const_fill
 * - copy_linear
 */


static int
gfx_ring_write_linear(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *ring_context,
		      uint32_t *pm4_dw)
{
	uint32_t i, j;
	uint32_t fill = ring_context->write_data_valid ?
			ring_context->write_data : func->deadbeaf;

	i = 0;
	j = 0;

	/* Guard: write_length must be DWORD-aligned (bytes) */
	igt_assert_f(ring_context->write_length % 4 == 0,
		     "GFX write_linear: write_length %lu not DWORD-aligned\n",
		     (unsigned long)ring_context->write_length);
	if (ring_context->pm4_size > 0)
		igt_assert_f(ring_context->write_length / 4 + 4 <= ring_context->pm4_size,
			     "GFX write_linear: %lu DWORDs + 4 header > pm4 buffer %u\n",
			     (unsigned long)(ring_context->write_length / 4),
			     ring_context->pm4_size);

	ring_context->pm4[i++] = PACKET3(PACKET3_WRITE_DATA, 2 + ring_context->write_length / 4);
	ring_context->pm4[i++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	while (j++ < ring_context->write_length / 4)
		ring_context->pm4[i++] = fill;

	*pm4_dw = i;
	if (ring_context->pm4_size > 0)
		igt_assert_f(*pm4_dw <= ring_context->pm4_size,
			     "GFX write_linear: pm4_dw %u exceeds buffer %u\n",
			     *pm4_dw, ring_context->pm4_size);
	return 0;
}

static int
gfx_ring_bad_write_linear(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *ring_context,
		      uint32_t *pm4_dw, unsigned int cmd_error)
{
	uint32_t i, j, stream_length;

	i = 0;
	j = 0;

	 /* Invalid opcode are different for different asics,
	  * But the range applies to all asics.
	  * 0xcb-0xcf, 0xd2-0xef, 0xf1-0xfb
	  */
	if (cmd_error == CMD_STREAM_EXEC_INVALID_PACKET_LENGTH)
		stream_length = ring_context->write_length / 16;
	else if (cmd_error == CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE)
		stream_length = ring_context->write_length * 2;
	else
		stream_length = ring_context->write_length;

	if (cmd_error == CMD_STREAM_EXEC_INVALID_OPCODE)
		ring_context->pm4[i++] = PACKET3(0xf2, 2 +  ring_context->write_length);
	else if (cmd_error == CMD_STREAM_EXEC_INVALID_PACKET_LENGTH)
		ring_context->pm4[i++] = PACKET3(PACKET3_WRITE_DATA, (ring_context->write_length - 2));
	else
		ring_context->pm4[i++] = PACKET3(PACKET3_WRITE_DATA, 2 +  ring_context->write_length);

	if (cmd_error == CMD_STREAM_TRANS_BAD_REG_ADDRESS) {
		ring_context->pm4[i++] =  WRITE_DATA_DST_SEL(0);
		ring_context->pm4[i++] = lower_32_bits(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
		ring_context->pm4[i++] = upper_32_bits(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
	} else if (cmd_error == CMD_STREAM_TRANS_BAD_MEM_ADDRESS) {
		ring_context->pm4[i++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
		ring_context->pm4[i++] = lower_32_bits(0xdeadbee0);
		ring_context->pm4[i++] = upper_32_bits(0xdeadbee0);
	} else if (cmd_error == CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC) {
		ring_context->pm4[i++] = WRITE_DATA_DST_SEL(1);
		ring_context->pm4[i++] = lower_32_bits(0xdeadbee0);
		ring_context->pm4[i++] = upper_32_bits(0xdeadbee0);
	} else {
		ring_context->pm4[i++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
		ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	}

	while (j++ < stream_length)
		ring_context->pm4[i++] = func->deadbeaf;
	*pm4_dw = i;
	return i;
}

static int
gfx_ring_atomic(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *ring_context,
		      uint32_t *pm4_dw)
{
	uint32_t i = 0;

	memset(ring_context->pm4, 0, ring_context->pm4_size * sizeof(uint32_t));
		ring_context->pm4[i++] = PACKET3(PACKET3_ATOMIC_MEM, 7);

	/* atomic opcode for 32b w/ RTN and ATOMIC_SWAPCMP_RTN
	 * command, 1-loop_until_compare_satisfied.
	 * single_pass_atomic, 0-lru
	 * engine_sel, 0-micro_engine
	 */
	ring_context->pm4[i++] = (TC_OP_ATOMIC_CMPSWAP_RTN_32 |
				ATOMIC_MEM_COMMAND(1) |
				ATOMIC_MEM_CACHEPOLICAY(0) |
				ATOMIC_MEM_ENGINESEL(0));
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 0x12345678;
	ring_context->pm4[i++] = 0x0;
	ring_context->pm4[i++] = 0xdeadbeaf;
	ring_context->pm4[i++] = 0x0;
	ring_context->pm4[i++] = 0x100;

	*pm4_dw = i;
	return 0;
}

static int
gfx_ring_const_fill(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *ring_context,
		     uint32_t *pm4_dw)
{
	uint32_t i;
	uint32_t fill = ring_context->write_data_valid ?
			ring_context->write_data : func->deadbeaf;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		ring_context->pm4[i++] = PACKET3(PACKET3_DMA_DATA_SI, 4);
		ring_context->pm4[i++] = fill;
		ring_context->pm4[i++] = PACKET3_DMA_DATA_SI_ENGINE(0) |
					 PACKET3_DMA_DATA_SI_DST_SEL(0) |
					 PACKET3_DMA_DATA_SI_SRC_SEL(2) |
					 PACKET3_DMA_DATA_SI_CP_SYNC;
		ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = ring_context->write_length;
	} else {
		ring_context->pm4[i++] = PACKET3(PACKET3_DMA_DATA, 5);
		ring_context->pm4[i++] = PACKET3_DMA_DATA_ENGINE(0) |
					 PACKET3_DMA_DATA_DST_SEL(0) |
					 PACKET3_DMA_DATA_SRC_SEL(2) |
					 PACKET3_DMA_DATA_CP_SYNC;
		ring_context->pm4[i++] = fill;
		ring_context->pm4[i++] = 0;
		ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
		ring_context->pm4[i++] = ring_context->write_length;
	}
	*pm4_dw = i;

	return 0;
}

static int
gfx_ring_copy_linear(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *context,
		     uint32_t *pm4_dw)
{
	uint32_t i;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = PACKET3(PACKET3_DMA_DATA_SI, 4);
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = PACKET3_DMA_DATA_SI_ENGINE(0) |
			   PACKET3_DMA_DATA_SI_DST_SEL(0) |
			   PACKET3_DMA_DATA_SI_SRC_SEL(0) |
			   PACKET3_DMA_DATA_SI_CP_SYNC |
			   upper_32_bits(context->bo_mc);
		context->pm4[i++] = lower_32_bits(context->bo_mc2);
		context->pm4[i++] = upper_32_bits(context->bo_mc2);
		context->pm4[i++] = context->write_length;
	} else {
		context->pm4[i++] = PACKET3(PACKET3_DMA_DATA, 5);
		context->pm4[i++] = PACKET3_DMA_DATA_ENGINE(0) |
			   PACKET3_DMA_DATA_DST_SEL(0) |
			   PACKET3_DMA_DATA_SRC_SEL(0) |
			   PACKET3_DMA_DATA_CP_SYNC;
		context->pm4[i++] = lower_32_bits(context->bo_mc);
		context->pm4[i++] = upper_32_bits(context->bo_mc);
		context->pm4[i++] = lower_32_bits(context->bo_mc2);
		context->pm4[i++] = upper_32_bits(context->bo_mc2);
		context->pm4[i++] = context->write_length;
	}

	*pm4_dw = i;

	return 0;
}

static int
gfx_ring_wait_reg_mem(const struct amdgpu_ip_funcs *func,
			const struct amdgpu_ring_context *ring_context,
			uint32_t *pm4_dw)
{
	uint32_t i;

	i = *pm4_dw;
	ring_context->pm4[i++] = PACKET3(PACKET3_WAIT_REG_MEM, 5);
	ring_context->pm4[i++] = (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
							WAIT_REG_MEM_FUNCTION(3) | /* == */
							WAIT_REG_MEM_ENGINE(0));  /* me */
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = func->deadbeaf; /* reference value */
	ring_context->pm4[i++] = 0xffffffff; /* and mask */
	ring_context->pm4[i++] = 0x00000004; /* poll interval */
	*pm4_dw = i;

	return 0;
}

static int
sdma_ring_wait_reg_mem(const struct amdgpu_ip_funcs *func,
			const struct amdgpu_ring_context *ring_context,
			uint32_t *pm4_dw)
{
	int r;

	r = gfx_ring_wait_reg_mem(func, ring_context, pm4_dw);
	return r;
}

/* we may cobine these two functions later */
static int
x_compare(const struct amdgpu_ip_funcs *func,
	  const struct amdgpu_ring_context *ring_context, int div)
{
	int i = 0, ret = 0;

	int num_compare = ring_context->write_length/div;

	while (i < num_compare) {
		if (ring_context->bo_cpu[i++] != func->deadbeaf) {
			ret = -1;
			break;
		}
	}
	return ret;
}

static int
x_compare_pattern(const struct amdgpu_ip_funcs *func,
	  const struct amdgpu_ring_context *ring_context, int div)
{
	int i = 0, ret = 0;

	int num_compare = ring_context->write_length/div;

	while (i < num_compare) {
		if (ring_context->bo2_cpu[i++] != func->pattern) {
			ret = -1;
			break;
		}
	}
	return ret;
}

#ifdef AMDGPU_USERQ_ENABLED
static void amdgpu_alloc_doorbell(amdgpu_device_handle device_handle,
				  struct amdgpu_userq_bo *doorbell_bo,
				  unsigned int size, unsigned int domain)
{
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;
	int r;

	req.alloc_size = ALIGN(size, PAGE_SIZE);
	req.preferred_heap = domain;
	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, 0);

	doorbell_bo->handle = buf_handle;
	doorbell_bo->size = req.alloc_size;

	r = amdgpu_bo_cpu_map(doorbell_bo->handle,
			      (void **)&doorbell_bo->ptr);
	igt_assert_eq(r, 0);
}

int
amdgpu_bo_alloc_and_map_uq(amdgpu_device_handle device_handle, unsigned int size,
			   unsigned int alignment, unsigned int heap, uint64_t alloc_flags,
			   uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			   uint64_t *mc_address, amdgpu_va_handle *va_handle,
			   uint32_t timeline_syncobj_handle, uint64_t point)
{
	struct amdgpu_bo_alloc_request request = {};
	amdgpu_bo_handle buf_handle;
	uint64_t vmc_addr;
	int r;

	request.alloc_size = size;
	request.phys_alignment = alignment;
	request.preferred_heap = heap;
	request.flags = alloc_flags;

	r = amdgpu_bo_alloc(device_handle, &request, &buf_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, &vmc_addr,
				  va_handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op_raw2(device_handle, buf_handle, 0,
				 ALIGN(size, getpagesize()), vmc_addr,
				 AMDGPU_VM_PAGE_READABLE |
				 AMDGPU_VM_PAGE_WRITEABLE |
				 AMDGPU_VM_PAGE_EXECUTABLE |
				 mapping_flags,
				 AMDGPU_VA_OP_MAP,
				 timeline_syncobj_handle,
				 point, 0, 0);
	if (r)
		goto error_va_map;

	if (cpu) {
		r = amdgpu_bo_cpu_map(buf_handle, cpu);
		if (r)
			goto error_cpu_map;
	}

	*bo = buf_handle;
	*mc_address = vmc_addr;

	return 0;

error_cpu_map:
	amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);
error_va_map:
	amdgpu_va_range_free(*va_handle);
error_va_alloc:
	amdgpu_bo_free(buf_handle);
	return r;
}

static void amdgpu_bo_unmap_and_free_uq(amdgpu_device_handle device_handle,
					amdgpu_bo_handle bo, amdgpu_va_handle va_handle,
					uint64_t mc_addr, uint64_t size,
					uint32_t timeline_syncobj_handle,
					uint64_t point, uint64_t syncobj_handles_array,
					uint32_t num_syncobj_handles)
{
	amdgpu_bo_cpu_unmap(bo);
	amdgpu_bo_va_op_raw2(device_handle, bo, 0, size, mc_addr, 0, AMDGPU_VA_OP_UNMAP,
				  timeline_syncobj_handle, point,
				  syncobj_handles_array, num_syncobj_handles);
	amdgpu_va_range_free(va_handle);
	amdgpu_bo_free(bo);
}

int amdgpu_timeline_syncobj_wait(amdgpu_device_handle device_handle,
				 uint32_t timeline_syncobj_handle, uint64_t point)
{
	uint32_t flags = DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED;
	int r;

	r = amdgpu_cs_syncobj_query2(device_handle, &timeline_syncobj_handle,
				     &point, 1, flags);
	if (r)
		return r;

	r = amdgpu_cs_syncobj_timeline_wait(device_handle, &timeline_syncobj_handle,
					    &point, 1, INT64_MAX,
					    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
					    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
					    NULL);
	if (r)
		igt_warn("Timeline timed out\n");
	return r;
}

static
int wait_for_packet_consumption(struct amdgpu_ring_context *ring_context)
{
	uint64_t count = 0;

	while (*ring_context->rptr_cpu == *ring_context->wptr_cpu) {
		if (count > 2000) {
			igt_warn("Timeout waiting for bad packet consumption\n");
			return -ETIMEDOUT;
		}
		count++;
		usleep(1000);
	}
	return 0;
}

static
int create_sync_signal(amdgpu_device_handle device,
			struct amdgpu_ring_context *ring_context,
			uint64_t timeout)
{
	uint32_t syncarray[1];
	struct drm_amdgpu_userq_signal signal_data;
	int r;

	syncarray[0] = ring_context->timeline_syncobj_handle;
	signal_data.queue_id = ring_context->queue_id;
	signal_data.syncobj_handles = (uintptr_t)syncarray;
	signal_data.num_syncobj_handles = 1;
	signal_data.bo_read_handles = 0;
	signal_data.bo_write_handles = 0;
	signal_data.num_bo_read_handles = 0;
	signal_data.num_bo_write_handles = 0;

	r = amdgpu_userq_signal(device, &signal_data);
	if (r)
		return r;

	return amdgpu_cs_syncobj_wait(device, &ring_context->timeline_syncobj_handle,
				  1, timeout, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
}

static int
user_queue_submit(amdgpu_device_handle device, struct amdgpu_ring_context *ring_context,
			      unsigned int ip_type, uint64_t mc_address)
{
	int r;
	uint32_t control = ring_context->pm4_dw;
	uint64_t timeout;
	unsigned int nop_count;
	struct timespec ts;
	uint64_t current_ns;
	unsigned int i, j;
	uint64_t va, value;
	unsigned num_fences_in_iter;
	struct amdgpu_userq_params *userq_params;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	current_ns = (uint64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
	timeout = current_ns + (60 * NSEC_PER_SEC);

	if (ip_type == AMD_IP_DMA) {
		amdgpu_sdma_pkt_begin();
		/* For SDMA, we need to align the IB to 8 DW boundary */
		nop_count = (2 - lower_32_bits(*ring_context->wptr_cpu)) & 7;
		for (i = 0; i < nop_count; i++)
			amdgpu_pkt_add_dw(SDMA_PKT_HEADER_OP(SDMA_NOP));
		amdgpu_pkt_add_dw(SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT));
		amdgpu_pkt_add_dw(lower_32_bits(mc_address) & 0xffffffe0); // 32-byte aligned
		amdgpu_pkt_add_dw(upper_32_bits(mc_address));
		amdgpu_pkt_add_dw(control); // IB length in DWORDS
		amdgpu_pkt_add_dw(lower_32_bits(ring_context->csa.mc_addr)); // CSA MC address low
		amdgpu_pkt_add_dw(upper_32_bits(ring_context->csa.mc_addr)); // CSA MC address high
		amdgpu_pkt_add_dw(SDMA_PACKET(SDMA_OP_PROTECTED_FENCE, SDMA_SUB_OP_PROTECTED_FENCE, 0));
#if DETECT_CC_GCC && (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
		asm volatile ("mfence" : : : "memory");
#endif
		/* Below call update the wptr address so will wait till all writes are completed */
		amdgpu_sdma_pkt_end();
	} else {
		amdgpu_pkt_begin();

		if (ring_context->userq_params) {
			userq_params = ring_context->userq_params;

			if (userq_params->job_start_write_data_va_addr) {
				amdgpu_pkt_add_dw(PACKET3(PACKET3_WRITE_DATA, 4));
				amdgpu_pkt_add_dw(WRITE_DATA_DST_SEL(5) | WR_CONFIRM | WRITE_DATA_CACHE_POLICY(3));
				va = userq_params->job_start_write_data_va_addr;
				value = userq_params->job_start_write_data_val;
				amdgpu_pkt_add_dw(lower_32_bits(va));
				amdgpu_pkt_add_dw(upper_32_bits(va));
				amdgpu_pkt_add_dw(lower_32_bits(value));
				amdgpu_pkt_add_dw(upper_32_bits(value));
			}

			if (ring_context->userq_params->num_fences) {
				for (i = 0; i < userq_params->num_fences; i = i + ring_context->max_num_fences_fwm) {
					num_fences_in_iter = (i + ring_context->max_num_fences_fwm > userq_params->num_fences) ?
							     userq_params->num_fences - i : ring_context->max_num_fences_fwm;
					amdgpu_pkt_add_dw(PACKET3(PACKET3_FENCE_WAIT_MULTI, num_fences_in_iter * 4));
					amdgpu_pkt_add_dw(FWM_ENGINE_SEL(1) | FWM_POLL_INTERVAL(4));
					for (j = 0; j < num_fences_in_iter; j++) {
						va = userq_params->fence_info[i + j].va;
						value = userq_params->fence_info[i + j].value;
						amdgpu_pkt_add_dw(lower_32_bits(va));
						amdgpu_pkt_add_dw(upper_32_bits(va));
						amdgpu_pkt_add_dw(lower_32_bits(value));
						amdgpu_pkt_add_dw(upper_32_bits(value));
					}
				}
			}
		}

		/* Prepare the Indirect IB to submit the IB to user queue */
		amdgpu_pkt_add_dw(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
		amdgpu_pkt_add_dw(lower_32_bits(mc_address));
		amdgpu_pkt_add_dw(upper_32_bits(mc_address));

		if (ip_type == AMD_IP_GFX)
			amdgpu_pkt_add_dw(control | S_3F3_INHERIT_VMID_MQD_GFX(1));
		else
			amdgpu_pkt_add_dw(control | S_3F3_VALID_COMPUTE(1)
						| S_3F3_INHERIT_VMID_MQD_COMPUTE(1));

		amdgpu_pkt_add_dw(PACKET3(PACKET3_PROTECTED_FENCE_SIGNAL, 0));

		/* empty dword is needed for fence signal pm4 */
		amdgpu_pkt_add_dw(0);
#if DETECT_CC_GCC && (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
	asm volatile ("mfence" : : : "memory");
#endif
		/* Below call update the wptr address so will wait till all writes are completed */
		amdgpu_pkt_end();
	}

#if DETECT_CC_GCC && (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
	asm volatile ("mfence" : : : "memory");
#endif
	/* Ring at the engine's per-page UMQ sub-aperture (see amd_ip_blocks.h). */
	switch (ip_type) {
	case AMD_IP_DMA:
		ring_context->doorbell_cpu[SDMA_DOORBELL_INDEX] = *ring_context->wptr_cpu;
		break;
	case AMD_IP_GFX:
		ring_context->doorbell_cpu[GFX_DOORBELL_INDEX] = *ring_context->wptr_cpu;
		break;
	default:
		ring_context->doorbell_cpu[DOORBELL_INDEX] = *ring_context->wptr_cpu;
		break;
	}

	switch (ring_context->submit_mode) {
	case UQ_SUBMIT_NO_SYNC:
		/* Error injection: wait for packet consumption without sync */
		r = wait_for_packet_consumption(ring_context);
		break;
	case UQ_SUBMIT_NORMAL:
	default:
		/* Standard submission with full synchronization */
		r = create_sync_signal(device, ring_context, timeout);
		break;
	}
	return r;
}

static void
user_queue_destroy(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt,
			       unsigned int type)
{
	int r;

	if (type > AMD_IP_DMA) {
		igt_info("Invalid IP not supported for UMQ Submission\n");
		return;
	}

	/* Free the Usermode Queue */
	r = amdgpu_free_userqueue(device_handle, ctxt->queue_id);
	igt_assert_eq(r, 0);

	switch (type) {
	case AMD_IP_GFX:
		amdgpu_bo_unmap_and_free_uq(device_handle, ctxt->csa.handle,
					    ctxt->csa.va_handle,
					    ctxt->csa.mc_addr, ctxt->info.gfx.csa_size,
					    ctxt->timeline_syncobj_handle, ++ctxt->point,
					    0, 0);

		amdgpu_bo_unmap_and_free_uq(device_handle, ctxt->shadow.handle,
					    ctxt->shadow.va_handle,
					    ctxt->shadow.mc_addr, ctxt->info.gfx.shadow_size,
					    ctxt->timeline_syncobj_handle, ++ctxt->point,
					    0, 0);

		r = amdgpu_timeline_syncobj_wait(device_handle, ctxt->timeline_syncobj_handle,
						 ctxt->point);
		igt_assert_eq(r, 0);
		break;

	case AMD_IP_COMPUTE:
		amdgpu_bo_unmap_and_free_uq(device_handle, ctxt->eop.handle,
					    ctxt->eop.va_handle,
					    ctxt->eop.mc_addr, 256,
					    ctxt->timeline_syncobj_handle, ++ctxt->point,
					    0, 0);

		r = amdgpu_timeline_syncobj_wait(device_handle, ctxt->timeline_syncobj_handle,
						 ctxt->point);
		igt_assert_eq(r, 0);
		break;

	case AMD_IP_DMA:
		amdgpu_bo_unmap_and_free_uq(device_handle, ctxt->csa.handle,
					    ctxt->csa.va_handle,
					    ctxt->csa.mc_addr, ctxt->info.gfx.csa_size,
					    ctxt->timeline_syncobj_handle, ++ctxt->point,
					    0, 0);

		r = amdgpu_timeline_syncobj_wait(device_handle, ctxt->timeline_syncobj_handle,
						 ctxt->point);
		igt_assert_eq(r, 0);
		break;

	default:
		igt_info("IP invalid for cleanup\n");
	}

	r = amdgpu_cs_destroy_syncobj(device_handle, ctxt->timeline_syncobj_handle);
	igt_assert_eq(r, 0);

	/* Clean up doorbell*/
	r = amdgpu_bo_cpu_unmap(ctxt->doorbell.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(ctxt->doorbell.handle);
	igt_assert_eq(r, 0);

	/* Clean up rptr wptr queue */
	amdgpu_bo_unmap_and_free(ctxt->rptr.handle, ctxt->rptr.va_handle,
				 ctxt->rptr.mc_addr, 8);

	amdgpu_bo_unmap_and_free(ctxt->wptr.handle, ctxt->wptr.va_handle,
				 ctxt->wptr.mc_addr, 8);

	amdgpu_bo_unmap_and_free(ctxt->queue.handle, ctxt->queue.va_handle,
				 ctxt->queue.mc_addr, USERMODE_QUEUE_SIZE);
}

static void
user_queue_create(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt,
			      unsigned int type)
{
	int r;
	uint64_t gtt_flags = 0, queue_flags = 0;
	struct drm_amdgpu_userq_mqd_gfx11 gfx_mqd;
	struct drm_amdgpu_userq_mqd_sdma_gfx11 sdma_mqd;
	struct drm_amdgpu_userq_mqd_compute_gfx11 compute_mqd;
	void *mqd;

	if (type > AMD_IP_DMA) {
		igt_info("Invalid IP not supported for UMQ Submission\n");
		return;
	}

	if (ctxt->secure) {
		gtt_flags |= AMDGPU_GEM_CREATE_ENCRYPTED;
		queue_flags |= AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE;
	}

	if (ctxt->priority)
		queue_flags |= ctxt->priority & AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_MASK;

	r = amdgpu_query_uq_fw_area_info(device_handle, AMD_IP_GFX, 0, &ctxt->info);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_create_syncobj2(device_handle, 0, &ctxt->timeline_syncobj_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map_uq(device_handle, USERMODE_QUEUE_SIZE,
				       ALIGNMENT,
				       AMDGPU_GEM_DOMAIN_GTT,
				       gtt_flags,
				       AMDGPU_VM_MTYPE_UC,
				       &ctxt->queue.handle, &ctxt->queue.ptr,
				       &ctxt->queue.mc_addr, &ctxt->queue.va_handle,
				       ctxt->timeline_syncobj_handle, ++ctxt->point);
	igt_assert_eq(r, 0);

	/*
	 * The wptr normally sits at offset 0 of its own 8-byte BO. When
	 * wptr_offset is set, allocate a full page and place the wptr at that
	 * (8-byte aligned, < PAGE_SIZE) offset instead, so the kernel's wptr
	 * read path has to account for a wptr that is not at the start of the
	 * BO. Offset 0 is later poisoned so a kernel that wrongly reads BO
	 * offset 0 gets a bogus wptr.
	 */
	igt_assert_f(ctxt->wptr_offset % sizeof(uint64_t) == 0 &&
		     ctxt->wptr_offset + sizeof(uint64_t) <= PAGE_SIZE,
		     "wptr_offset %lu must be 8-byte aligned and fit in a page\n",
		     (unsigned long)ctxt->wptr_offset);
	r = amdgpu_bo_alloc_and_map_uq(device_handle,
				       ctxt->wptr_offset ? PAGE_SIZE : 8,
				       ALIGNMENT,
				       AMDGPU_GEM_DOMAIN_GTT,
				       gtt_flags,
				       AMDGPU_VM_MTYPE_UC,
				       &ctxt->wptr.handle, &ctxt->wptr.ptr,
				       &ctxt->wptr.mc_addr, &ctxt->wptr.va_handle,
				       ctxt->timeline_syncobj_handle, ++ctxt->point);
	igt_assert_eq(r, 0);
	if (ctxt->wptr_offset) {
		/*
		 * The wptr BO is not zero-initialized. Clear the real wptr slot
		 * so the queue starts from a well-defined wptr of 0, then poison
		 * BO offset 0 so a buggy (offset-ignoring) read is fatal.
		 */
		*(volatile uint64_t *)((uint8_t *)ctxt->wptr.ptr +
				       ctxt->wptr_offset) = 0;
		*(volatile uint64_t *)ctxt->wptr.ptr = 0xdead0000dead0000ULL;
	}

	r = amdgpu_bo_alloc_and_map_uq(device_handle, 8,
				       ALIGNMENT,
				       AMDGPU_GEM_DOMAIN_GTT,
				       gtt_flags,
				       AMDGPU_VM_MTYPE_UC,
				       &ctxt->rptr.handle, &ctxt->rptr.ptr,
				       &ctxt->rptr.mc_addr, &ctxt->rptr.va_handle,
				       ctxt->timeline_syncobj_handle, ++ctxt->point);
	igt_assert_eq(r, 0);

	switch (type) {
	case AMD_IP_GFX:
		r = amdgpu_bo_alloc_and_map_uq(device_handle, ctxt->info.gfx.shadow_size,
					       ctxt->info.gfx.shadow_alignment,
					       AMDGPU_GEM_DOMAIN_GTT,
					       gtt_flags,
					       AMDGPU_VM_MTYPE_UC,
					       &ctxt->shadow.handle, NULL,
					       &ctxt->shadow.mc_addr, &ctxt->shadow.va_handle,
					       ctxt->timeline_syncobj_handle, ++ctxt->point);
		igt_assert_eq(r, 0);

		r = amdgpu_bo_alloc_and_map_uq(device_handle, ctxt->info.gfx.csa_size,
					       ctxt->info.gfx.csa_alignment,
					       AMDGPU_GEM_DOMAIN_GTT,
					       gtt_flags,
					       AMDGPU_VM_MTYPE_UC,
					       &ctxt->csa.handle, NULL,
					       &ctxt->csa.mc_addr, &ctxt->csa.va_handle,
					       ctxt->timeline_syncobj_handle, ++ctxt->point);
		igt_assert_eq(r, 0);

		gfx_mqd.shadow_va = ctxt->shadow.mc_addr;
		gfx_mqd.csa_va = ctxt->csa.mc_addr;
		mqd = &gfx_mqd;
		break;

	case AMD_IP_COMPUTE:
		r = amdgpu_bo_alloc_and_map_uq(device_handle, 256,
					       ALIGNMENT,
					       AMDGPU_GEM_DOMAIN_GTT,
					       gtt_flags,
					       AMDGPU_VM_MTYPE_UC,
					       &ctxt->eop.handle, NULL,
					       &ctxt->eop.mc_addr, &ctxt->eop.va_handle,
					       ctxt->timeline_syncobj_handle, ++ctxt->point);
		igt_assert_eq(r, 0);
		compute_mqd.eop_va = ctxt->eop.mc_addr;
		mqd = &compute_mqd;
		break;

	case AMD_IP_DMA:
		r = amdgpu_bo_alloc_and_map_uq(device_handle, ctxt->info.gfx.csa_size,
					       ctxt->info.gfx.csa_alignment,
					       AMDGPU_GEM_DOMAIN_GTT,
					       gtt_flags,
					       AMDGPU_VM_MTYPE_UC,
					       &ctxt->csa.handle, NULL,
					       &ctxt->csa.mc_addr, &ctxt->csa.va_handle,
					       ctxt->timeline_syncobj_handle, ++ctxt->point);
		igt_assert_eq(r, 0);
		sdma_mqd.csa_va = ctxt->csa.mc_addr;
		mqd = &sdma_mqd;
		break;

	default:
		igt_info("Unsupported IP for UMQ submission\n");
		return;

	}

	r = amdgpu_timeline_syncobj_wait(device_handle, ctxt->timeline_syncobj_handle,
					 ctxt->point);
	igt_assert_eq(r, 0);

	amdgpu_alloc_doorbell(device_handle, &ctxt->doorbell, PAGE_SIZE,
			      AMDGPU_GEM_DOMAIN_DOORBELL);

	ctxt->doorbell_cpu = (uint64_t *)ctxt->doorbell.ptr;
	/*
	 * wptr_offset shifts the wptr within its BO. The BO struct fields stay
	 * pristine (destroy unmaps by base addr); only the queue-facing wptr
	 * address and cpu pointer are offset.
	 */
	ctxt->wptr_cpu = (uint64_t *)((uint8_t *)ctxt->wptr.ptr + ctxt->wptr_offset);
	ctxt->rptr_cpu = (uint64_t *)ctxt->rptr.ptr;

	ctxt->queue_cpu = (uint32_t *)ctxt->queue.ptr;
	memset(ctxt->queue_cpu, 0, USERMODE_QUEUE_SIZE);

	/* get db bo handle */
	amdgpu_bo_export(ctxt->doorbell.handle, amdgpu_bo_handle_type_kms, &ctxt->db_handle);

	/* Create the Usermode Queue */
	switch (type) {
	case AMD_IP_GFX:
		r = amdgpu_create_userqueue(device_handle, AMDGPU_HW_IP_GFX,
					    ctxt->db_handle, GFX_DOORBELL_INDEX,
					    ctxt->queue.mc_addr, USERMODE_QUEUE_SIZE,
					    ctxt->wptr.mc_addr + ctxt->wptr_offset, ctxt->rptr.mc_addr,
					    mqd, queue_flags, &ctxt->queue_id);
		igt_assert_eq(r, 0);
		break;

	case AMD_IP_COMPUTE:
		r = amdgpu_create_userqueue(device_handle, AMDGPU_HW_IP_COMPUTE,
					    ctxt->db_handle, DOORBELL_INDEX,
					    ctxt->queue.mc_addr, USERMODE_QUEUE_SIZE,
					    ctxt->wptr.mc_addr + ctxt->wptr_offset, ctxt->rptr.mc_addr,
					    mqd, queue_flags, &ctxt->queue_id);
		igt_assert_eq(r, 0);
		break;

	case AMD_IP_DMA:
		/* Per-page BIF decode verified: standard userqueue path with
		 * doorbell_offset placed in any 4K page's SDMA sub-aperture. */
		r = amdgpu_create_userqueue(device_handle, AMDGPU_HW_IP_DMA,
					    ctxt->db_handle, SDMA_DOORBELL_INDEX,
					    ctxt->queue.mc_addr, USERMODE_QUEUE_SIZE,
					    ctxt->wptr.mc_addr + ctxt->wptr_offset, ctxt->rptr.mc_addr,
					    mqd, queue_flags, &ctxt->queue_id);
		igt_assert_eq(r, 0);
		break;

	default:
		igt_info("Unsupported IP, failed to create user queue\n");
		return;

	}
}
#else
int
amdgpu_bo_alloc_and_map_uq(amdgpu_device_handle device_handle, unsigned int size,
			   unsigned int alignment, unsigned int heap, uint64_t alloc_flags,
			   uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			   uint64_t *mc_address, amdgpu_va_handle *va_handle,
			   uint32_t timeline_syncobj_handle, uint64_t point)
{
	return 0;
}

int
amdgpu_timeline_syncobj_wait(amdgpu_device_handle device_handle,
	uint32_t timeline_syncobj_handle, uint64_t point)
{
	return 0;
}

static int
user_queue_submit(amdgpu_device_handle device, struct amdgpu_ring_context *ring_context,
	unsigned int ip_type, uint64_t mc_address)
{
	return 0;
}

static void
user_queue_destroy(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt,
	unsigned int type)
{
}

static void
user_queue_create(amdgpu_device_handle device_handle, struct amdgpu_ring_context *ctxt,
	unsigned int type)
{
}
#endif

static struct amdgpu_ip_funcs gfx_v8_x_ip_funcs = {
	.family_id = FAMILY_VI,
	.align_mask = 0xff,
	.nop = 0x80000000,
	.deadbeaf = 0xdeadbeaf,
	.pattern = 0xaaaaaaaa,
	.write_linear = gfx_ring_write_linear,
	.bad_write_linear = gfx_ring_bad_write_linear,
	.write_linear_atomic = gfx_ring_atomic,
	.const_fill = gfx_ring_const_fill,
	.copy_linear = gfx_ring_copy_linear,
	.compare = x_compare,
	.compare_pattern = x_compare_pattern,
	.get_reg_offset = gfx_v8_0_get_reg_offset,
	.wait_reg_mem = gfx_ring_wait_reg_mem,
	.userq_create = user_queue_create,
	.userq_submit = user_queue_submit,
	.userq_destroy = user_queue_destroy,
};

static struct amdgpu_ip_funcs sdma_v3_x_ip_funcs = {
	.family_id = FAMILY_VI,
	.align_mask = 0xff,
	.nop = 0x80000000,
	.deadbeaf = 0xdeadbeaf,
	.pattern = 0xaaaaaaaa,
	.write_linear = sdma_ring_write_linear,
	.bad_write_linear = sdma_ring_bad_write_linear,
	.write_linear_atomic = sdma_ring_atomic,
	.const_fill = sdma_ring_const_fill,
	.copy_linear = sdma_ring_copy_linear,
	.compare = x_compare,
	.compare_pattern = x_compare_pattern,
	.get_reg_offset = gfx_v8_0_get_reg_offset,
	.wait_reg_mem = sdma_ring_wait_reg_mem,
	.userq_create = user_queue_create,
	.userq_submit = user_queue_submit,
	.userq_destroy = user_queue_destroy,
};

struct amdgpu_ip_block_version gfx_v8_x_ip_block = {
	.type = AMD_IP_GFX,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_v8_x_ip_funcs
};

struct amdgpu_ip_block_version compute_v8_x_ip_block = {
	.type = AMD_IP_COMPUTE,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_v8_x_ip_funcs
};

struct amdgpu_ip_block_version sdma_v3_x_ip_block = {
	.type = AMD_IP_DMA,
	.major = 3,
	.minor = 0,
	.rev = 0,
	.funcs = &sdma_v3_x_ip_funcs
};

/* we may improve later */
struct amdgpu_ip_blocks_device amdgpu_ips;
const struct chip_info  *g_pChip;
struct chip_info g_chip;

static int
amdgpu_device_ip_block_add(struct amdgpu_ip_block_version *ip_block_version)
{
	if (amdgpu_ips.num_ip_blocks >= AMD_IP_MAX)
		return -1;

	amdgpu_ips.ip_blocks[amdgpu_ips.num_ip_blocks++] = ip_block_version;

	return 0;
}

static int
amdgpu_device_ip_block_ex_setup(struct amdgpu_ip_block_version *ip_block_version)
{
	if (amdgpu_ips.num_ip_blocks >= AMD_IP_MAX)
		return -1;

	if (ip_block_version->funcs &&
		(!ip_block_version->funcs->gfx_program_compute ||
		 !ip_block_version->funcs->gfx_dispatch_direct ||
		 !ip_block_version->funcs->gfx_write_confirm)) {
		amd_ip_blocks_ex_init(ip_block_version->funcs);
	}
	return 0;
}

const struct amdgpu_ip_block_version *
get_ip_block(amdgpu_device_handle device, enum amd_ip_block_type type)
{
	int i;

	if (g_chip.dev != device)
		return NULL;

	for (i = 0; i <  amdgpu_ips.num_ip_blocks; i++)
		if (amdgpu_ips.ip_blocks[i]->type == type)
			return amdgpu_ips.ip_blocks[i];
	return NULL;
}

static int
cmd_allocate_buf(struct amdgpu_cmd_base  *base, uint32_t size_dw)
{
	if (size_dw > base->max_dw) {
		if (base->buf) {
			free(base->buf);
			base->buf = NULL;
			base->max_dw = 0;
			base->cdw = 0;
		}
		base->buf = calloc(4, size_dw);
		if (!base->buf)
			return -1;
		base->max_dw = size_dw;
		base->cdw = 0;
	}
	return 0;
}

static int
cmd_attach_buf(struct amdgpu_cmd_base  *base, void *ptr, uint32_t size_bytes)
{
	if (base->buf && base->is_assigned_buf)
		return -1;

	if (base->buf) {
		free(base->buf);
		base->buf = NULL;
		base->max_dw = 0;
		base->cdw = 0;
	}
	assert(ptr != NULL);
	base->buf = (uint32_t *)ptr;
	base->max_dw = size_bytes>>2;
	base->cdw = 0;
	base->is_assigned_buf = true; /* allocated externally , no free */
	return 0;
}

static void
cmd_emit(struct amdgpu_cmd_base  *base, uint32_t value)
{
	assert(base->cdw <  base->max_dw);
	base->buf[base->cdw++] = value;
}

static void
cmd_emit_aligned(struct amdgpu_cmd_base *base, uint32_t mask, uint32_t cmd)
{
	while (base->cdw & mask)
		base->emit(base, cmd);
}
static void
cmd_emit_buf(struct amdgpu_cmd_base  *base, const void *ptr, uint32_t offset_bytes, uint32_t size_bytes)
{
	uint32_t total_offset_dw = (offset_bytes + size_bytes) >> 2;
	uint32_t offset_dw = offset_bytes >> 2;
	/*TODO read the requirements to fix */
	assert(size_bytes % 4 == 0); /* no gaps */
	assert(offset_bytes % 4 == 0);
	assert(base->cdw + total_offset_dw <  base->max_dw);
	memcpy(base->buf + base->cdw + offset_dw, ptr, size_bytes);
	base->cdw += total_offset_dw;
}

static void
cmd_emit_repeat(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t number_of_times)
{
	while (number_of_times > 0) {
		assert(base->cdw <  base->max_dw);
		base->buf[base->cdw++] = value;
		number_of_times--;
	}
}

static void
cmd_emit_at_offset(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t offset_dwords)
{
	assert(base->cdw + offset_dwords <  base->max_dw);
	base->buf[base->cdw + offset_dwords] = value;
}

struct amdgpu_cmd_base *
get_cmd_base(void)
{
	struct amdgpu_cmd_base *base = calloc(1, sizeof(*base));

	base->cdw = 0;
	base->max_dw = 0;
	base->buf = NULL;
	base->is_assigned_buf = false;

	base->allocate_buf = cmd_allocate_buf;
	base->attach_buf = cmd_attach_buf;
	base->emit = cmd_emit;
	base->emit_aligned = cmd_emit_aligned;
	base->emit_repeat = cmd_emit_repeat;
	base->emit_at_offset = cmd_emit_at_offset;
	base->emit_buf = cmd_emit_buf;
	base->ib_fill_nop = amdgpu_ib_fill_nop;
	base->ib_fill_chain = amdgpu_ib_fill_chain;
	base->ib_fill_writedata = amdgpu_ib_fill_writedata;
	base->ib_fill_writedata_chain = amdgpu_ib_fill_writedata_chain;
	base->ib_fill_wait = amdgpu_ib_fill_wait;

	return base;
}

void
free_cmd_base(struct amdgpu_cmd_base *base)
{
	if (base) {
		if (base->buf && base->is_assigned_buf == false)
			free(base->buf);
		free(base);
	}

}

/*
 * GFX: 8.x
 * COMPUTE: 8.x
 * SDMA 3.x
 *
 * GFX9:
 * COMPUTE: 9.x
 * SDMA 4.x
 *
 * GFX10.1:
 * COMPUTE: 10.1
 * SDMA 5.0
 *
 * GFX10.3:
 * COMPUTE: 10.3
 * SDMA 5.2
 *
 * copy function from mesa
 *  should be called once per test
 */
int setup_amdgpu_ip_blocks(uint32_t major, uint32_t minor, struct amdgpu_gpu_info *amdinfo,
			   amdgpu_device_handle device)
{
#define identify_chip2(asic, chipname)	\
	do {\
		if (ASICREV_IS(amdinfo->chip_external_rev, asic)) {\
			info->family = CHIP_##chipname;	\
			info->name = #chipname;	\
		} \
	} while (0)

#define identify_chip(chipname) identify_chip2(chipname, chipname)

	const struct chip_class_arr {
		const char *name;
		enum chip_class class;
	} chip_class_arr[] = {
		{"CLASS_UNKNOWN",	CLASS_UNKNOWN},
		{"R300",				R300},
		{"R400",				R400},
		{"R500",				R500},
		{"R600",				R600},
		{"R700",				R700},
		{"EVERGREEN",			EVERGREEN},
		{"CAYMAN",				CAYMAN},
		{"GFX6",				GFX6},
		{"GFX7",				GFX7},
		{"GFX8",				GFX8},
		{"GFX9",				GFX9},
		{"GFX10",				GFX10},
		{"GFX10_3",				GFX10_3},
		{"GFX11",				GFX11},
		{"GFX12",				GFX12},
		{"GFX13",				GFX13},
		{},
	};
	struct chip_info *info = &g_chip;
	int i;

	g_pChip = &g_chip;

	switch (amdinfo->family_id) {
	case AMDGPU_FAMILY_SI:
		identify_chip(TAHITI);
		identify_chip(PITCAIRN);
		identify_chip2(CAPEVERDE, VERDE);
		identify_chip(OLAND);
		identify_chip(HAINAN);
		break;
	case FAMILY_CI:
		identify_chip(BONAIRE);//tested
		identify_chip(HAWAII);
		break;
	case FAMILY_KV:
		identify_chip2(SPECTRE, KAVERI);
		identify_chip2(SPOOKY, KAVERI);
		identify_chip2(KALINDI, KABINI);
		identify_chip2(GODAVARI, KABINI);
		break;
	case FAMILY_VI:
		identify_chip(ICELAND);
		identify_chip(TONGA);
		identify_chip(FIJI);
		identify_chip(POLARIS10);
		identify_chip(POLARIS11);//tested
		identify_chip(POLARIS12);
		identify_chip(VEGAM);
		break;
	case FAMILY_CZ:
		identify_chip(CARRIZO);
		identify_chip(STONEY);
		break;
	case FAMILY_AI:
		identify_chip(VEGA10);
		identify_chip(VEGA12);
		identify_chip(VEGA20);
		identify_chip(ARCTURUS);
		identify_chip(ALDEBARAN);
		break;
	case FAMILY_RV:
		identify_chip(RAVEN);
		identify_chip(RAVEN2);
		identify_chip(RENOIR);
		break;
	case FAMILY_NV:
		identify_chip(NAVI10); //tested
		identify_chip(NAVI12);
		identify_chip(NAVI14);
		identify_chip(SIENNA_CICHLID);
		identify_chip(NAVY_FLOUNDER);
		identify_chip(DIMGREY_CAVEFISH);
		identify_chip(BEIGE_GOBY);
		break;
	case FAMILY_VGH:
		identify_chip(VANGOGH);
		break;
	case FAMILY_YC:
		identify_chip(YELLOW_CARP);
		break;
	case FAMILY_GFX1036:
		identify_chip(GFX1036);
		break;
	case FAMILY_GFX1037:
		identify_chip(GFX1037);
		break;
	case FAMILY_GFX1100:
		identify_chip(GFX1100);
		identify_chip(GFX1101);
		identify_chip(GFX1102);
		break;
	case FAMILY_GFX1103:
		identify_chip(GFX1103_R1);
		identify_chip(GFX1103_R2);
		break;
	case FAMILY_GFX1150:
		identify_chip(GFX1150);
		identify_chip(GFX1151);
		identify_chip(GFX1152);
		identify_chip(GFX1153);
		break;
	case FAMILY_GFX1154:
		identify_chip(GFX1154);
		break;
	case FAMILY_GFX1200:
		identify_chip(GFX1200);
		break;
	case FAMILY_GFX1300:
		identify_chip(GFX1300);
		break;
	}
	if (!info->name) {
		igt_info("amdgpu: unknown (family_id, chip_external_rev): (%u, %u)\n",
			 amdinfo->family_id, amdinfo->chip_external_rev);
		return -1;
	}
	igt_info("amdgpu: %s (family_id, chip_external_rev): (%u, %u)\n",
				info->name, amdinfo->family_id, amdinfo->chip_external_rev);

	if (info->family >= CHIP_GFX1300) {
		info->chip_class = GFX13;
	} else if (info->family >= CHIP_GFX1200) {
		info->chip_class = GFX12;
	} else if (info->family >= CHIP_GFX1100) {
		info->chip_class = GFX11;
	} else if (info->family >= CHIP_SIENNA_CICHLID) {
		info->chip_class = GFX10_3;
	} else if (info->family >= CHIP_NAVI10) {
		info->chip_class = GFX10;
	} else if (info->family >= CHIP_VEGA10) {
		info->chip_class = GFX9;
	} else if (info->family >= CHIP_TONGA) {
		info->chip_class = GFX8;
	} else if (info->family >= CHIP_BONAIRE) {
		info->chip_class = GFX7;
	} else if (info->family >= CHIP_TAHITI) {
		info->chip_class = GFX6;
	} else {
		igt_info("amdgpu: Unknown family.\n");
		return -1;
	}
	igt_assert_eq(chip_class_arr[info->chip_class].class, info->chip_class);
	igt_info("amdgpu: chip_class %s\n", chip_class_arr[info->chip_class].name);

	switch (info->chip_class) {
	case GFX6:
		break;
	case GFX7: /* tested */
	case GFX8: /* tested */
	case GFX9: /* tested */
	case GFX10:/* tested */
	case GFX10_3: /* tested */
	case GFX11: /* tested */
	case GFX12: /* tested */
	case GFX13: /* tested */
		/* extra precaution if re-factor again */
		igt_assert_eq(gfx_v8_x_ip_block.major, 8);
		igt_assert_eq(compute_v8_x_ip_block.major, 8);
		igt_assert_eq(sdma_v3_x_ip_block.major, 3);

		/* Add three default IP blocks: GFX, Compute, and SDMA.
		 *
		 * These represent the base hardware engines available on the ASIC.
		 * - gfx_v8_x_ip_block: graphics + compute pipeline control
		 * - compute_v8_x_ip_block: dedicated compute support (user queues, shaders)
		 * - sdma_v3_x_ip_block: asynchronous DMA engine for buffer transfers
		 */
		amdgpu_device_ip_block_add(&gfx_v8_x_ip_block);
		amdgpu_device_ip_block_add(&compute_v8_x_ip_block);
		amdgpu_device_ip_block_add(&sdma_v3_x_ip_block);

		/* Assign the actual hardware identifiers to all registered IP blocks.
		 *
		 * These fields are taken from amdinfo (queried from the kernel):
		 * - family_id: ASIC family (gfx9/gfx10/gfx11/gfx12, etc.)
		 * - chip_external_rev: revision exposed to firmware
		 * - chip_rev: internal revision used by HW-specific hooks
		 */
		for (i = 0; i < amdgpu_ips.num_ip_blocks; i++) {
			amdgpu_ips.ip_blocks[i]->funcs->family_id = amdinfo->family_id;
			amdgpu_ips.ip_blocks[i]->funcs->chip_external_rev = amdinfo->chip_external_rev;
			amdgpu_ips.ip_blocks[i]->funcs->chip_rev = amdinfo->chip_rev;
		}

		/* Configure ASIC-specific function hooks.
		 *
		 * Depending on family_id, each IP block installs the correct
		 * callback implementations for:
		 * - PM4 packet encoding (e.g., gfx10/gfx11/gfx12 differences)
		 * - Register offsets (PGM_LO/PGM_RSRC, etc.)
		 * - Compute and dispatch programming
		 *
		 * This ensures that test code emits commands in the correct
		 * hardware-specific format for the active GPU.
		 */
		for (i = 0; i < amdgpu_ips.num_ip_blocks; i++)
			amdgpu_device_ip_block_ex_setup(amdgpu_ips.ip_blocks[i]);

		/* Sanity-check the initialized IP blocks to ensure the family_id matches
		 * the ASIC info from the kernel. If these assertions fail, it usually
		 * means that:
		 * - IP block registration order changed, or
		 * - amdinfo data mismatches the active hooks, or
		 * - future refactoring broke expected initialization paths.
		 */
		igt_assert_eq(gfx_v8_x_ip_block.funcs->family_id, amdinfo->family_id);
		igt_assert_eq(sdma_v3_x_ip_block.funcs->family_id, amdinfo->family_id);



		break;
	default:
		igt_info("amdgpu: GFX11 or old.\n");
		return -1;
	}
	info->dev = device;

	return 0;
}

int
amdgpu_open_devices(bool open_render_node, int  max_cards_supported, int drm_amdgpu_fds[])
{
	drmDevicePtr devices[MAX_CARDS_SUPPORTED];
	int i;
	int drm_node;
	int amd_index = 0;
	int drm_count;
	int fd;
	drmVersionPtr version;

	for (i = 0; i < max_cards_supported && i < MAX_CARDS_SUPPORTED; i++)
		drm_amdgpu_fds[i] = -1;

	drm_count = drmGetDevices2(0, devices, MAX_CARDS_SUPPORTED);

	if (drm_count < 0) {
		igt_debug("drmGetDevices2() returned an error %d\n", drm_count);
		return 0;
	}

	for (i = 0; i < drm_count; i++) {
		/* If this is not PCI device, skip*/
		if (devices[i]->bustype != DRM_BUS_PCI)
			continue;

		/* If this is not AMD GPU vender ID, skip*/
		if (devices[i]->deviceinfo.pci->vendor_id != 0x1002)
			continue;

		if (open_render_node)
			drm_node = DRM_NODE_RENDER;
		else
			drm_node = DRM_NODE_PRIMARY;

		fd = -1;
		if (devices[i]->available_nodes & 1 << drm_node)
			fd = open(
				devices[i]->nodes[drm_node],
				O_RDWR | O_CLOEXEC);

		/* This node is not available. */
		if (fd < 0)
			continue;

		version = drmGetVersion(fd);
		if (!version) {
			igt_debug("Warning: Cannot get version for %s\n",
				devices[i]->nodes[drm_node]);
			close(fd);
			continue;
		}

		if (strcmp(version->name, "amdgpu")) {
			/* This is not AMDGPU driver, skip.*/
			drmFreeVersion(version);
			close(fd);
			continue;
		}

		drmFreeVersion(version);

		drm_amdgpu_fds[amd_index] = fd;
		amd_index++;
	}

	drmFreeDevices(devices, drm_count);
	return amd_index;
}

/**
 * is_rings_available:
 * @device handle: handle to driver internal information
 * @mask: number of rings we are interested in checking the availability and readiness
 * @type the type of IP, for example GFX, COMPUTE, etc.
 *
 * Check whether the given ring number is ready to accept jobs
 * hw_ip_info.available_rings represents a bit vector of available rings are
 * ready to work.
 */
static bool
is_rings_available(amdgpu_device_handle device_handle, uint32_t mask,
		enum amd_ip_block_type type)
{
	struct drm_amdgpu_info_hw_ip hw_ip_info;

	memset(&hw_ip_info, 0, sizeof(hw_ip_info));
	/*
	 * Ignore the check of the return value of amdgpu_query_hw_ip_info(), as
	 * it could fail if certain IP instance types are not present in the
	 * ASIC
	 */
	amdgpu_query_hw_ip_info(device_handle, type, 0, &hw_ip_info);

	return  hw_ip_info.available_rings & mask;
}

void asic_userq_readiness(amdgpu_device_handle device_handle, bool arr[AMD_IP_MAX])
{
	int r, i;
	enum amd_ip_block_type ip;
	struct drm_amdgpu_info_device dev_info = {0};

	r = amdgpu_query_info(device_handle, AMDGPU_INFO_DEV_INFO,
			      sizeof(dev_info), &dev_info);
	igt_assert_eq(r, 0);

	if (!dev_info.userq_ip_mask)
		return;

	for (i = 0, ip = AMD_IP_GFX; ip < AMD_IP_MAX; ip++)
		arr[i++] = dev_info.userq_ip_mask & (1 << ip);
}

/**
 * asic_rings_readness:
 * @device handle: handle to driver internal information
 * @mask: number of rings we are interested in checking the availability and readiness
 * @arr array all possible IP ring readiness for the given mask
 *
 * Enumerate all possible IPs by checking their readiness for the given mask.
 */

void
asic_rings_readness(amdgpu_device_handle device_handle, uint32_t mask,
				bool arr[AMD_IP_MAX])
{
	enum amd_ip_block_type ip;
	int i;

	for (i = 0, ip = AMD_IP_GFX; ip < AMD_IP_MAX; ip++)
		arr[i++] = is_rings_available(device_handle, mask, ip);
}

/**
 * is_reset_enable:
 * @ip_type: such as gfx, compute and dma.
 * @reset_type: includes full adapter reset, soft reset, queue reset, and pipeline reset
 *
 * Check if reset supports certain reset types.
 */

bool
is_reset_enable(enum amd_ip_block_type ip_type, uint32_t reset_type, const struct pci_addr *pci)
{
	const struct reset_arr {
		const char *name;
		unsigned int reset;
	} reset_arr[] = {
		{"full",	AMDGPU_RESET_TYPE_FULL		},
		{"soft",	AMDGPU_RESET_TYPE_SOFT_RESET},
		{"queue",	AMDGPU_RESET_TYPE_PER_QUEUE },
		{"pipe",	AMDGPU_RESET_TYPE_PER_PIPE },
		{NULL, 0}
	};

	bool enable = false;
	char cmd[256];
	FILE *fp;
	char buffer[128];
	char buffer2[128];

	char *token, *buf;
	char reset_mask[32];
	uint32_t mask = 0;
	int i;

	if (ip_type == AMD_IP_GFX)
		snprintf(reset_mask, sizeof(reset_mask) - 1, "gfx_reset_mask");
	else if (ip_type == AMD_IP_COMPUTE)
		snprintf(reset_mask, sizeof(reset_mask) - 1, "compute_reset_mask");
	else if (ip_type == AMD_IP_VCN_UNIFIED)
		snprintf(reset_mask, sizeof(reset_mask) - 1, "vcn_reset_mask");
	else if (ip_type == AMD_IP_VCN_JPEG)
		snprintf(reset_mask, sizeof(reset_mask) - 1, "jpeg_reset_mask");
	else
		snprintf(reset_mask, sizeof(reset_mask) - 1, "sdma_reset_mask");

	snprintf(cmd, sizeof(cmd) - 1, "sudo cat /sys/bus/pci/devices/%04x:%02x:%02x.%01x/%s",
			pci->domain, pci->bus, pci->device, pci->function, reset_mask);

	fp = popen(cmd, "r");
	if (fp == NULL) {
		igt_kmsg("***FAILURE popen %s LINE %d FILE %s\n", cmd, __LINE__, __FILE__);
		return false;
	}

	buf = fgets(buffer, sizeof(buffer)-1, fp);

	if (buf != NULL) {
		strcpy(buffer2, buffer);
		token = strtok(buf, " \n");
		while (token != NULL) {
			for (i = 0; reset_arr[i].name != NULL; i++) {
				if (reset_type == reset_arr[i].reset &&
					strcmp(token, reset_arr[i].name) == 0) {
					mask |= reset_arr[i].reset;
					break;
				}
			}
			token = strtok(NULL, " \n");
		}
	} else {
		igt_kmsg("***FAILURE fgets %s LINE %d FILE %s\n",
				buffer, __LINE__, __FILE__);
	}
	if (mask & reset_type)
		enable = true;
	else
		igt_kmsg("***FAILURE mask found 0x%x(%s) requested 0x%x operation %s is not supported LINE %d FILE %s\n",
				mask, buffer2, reset_type, ip_type == AMD_IP_GFX ? "GFX" : ip_type == AMD_IP_COMPUTE ? "COMPUTE" : "SDMA",
						__LINE__, __FILE__);

	pclose(fp);

	return enable;
}

/**
 * get_pci_addr_from_fd - Extracts the PCI device address from a file descriptor.
 * @fd: The file descriptor to extract the address from.
 * @pci: Pointer to a pci_addr struct to store the extracted address.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int get_pci_addr_from_fd(int fd, struct pci_addr *pci)
{
	char path[80];
	struct stat st;
	char link[20], pci_path[256];
	char *buf;
	int len, sysfs;
	int ret;

	// Check if the file descriptor is a character device and can be accessed
	if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode))
		return -1;

	snprintf(path, sizeof(path), "/sys/dev/char/%d:%d",
			major(st.st_rdev), minor(st.st_rdev));

	// Check if the sysfs path exists
	if (access(path, F_OK) < 0)
		return -1;

	// Open the sysfs directory
	sysfs = open(path, O_RDONLY);
	if (sysfs < 0)
		return -1;

	// Read the "device" link from the sysfs directory
	snprintf(link, sizeof(link), "device");
	len = readlinkat(sysfs, link, pci_path, sizeof(pci_path) - 1);
	if (len == -1) {
		close(sysfs);
		return -ENOENT;
	}
	close(sysfs);
 // Null-terminate the extracted path
	pci_path[len] = '\0';

	// Find the last occurrence of '/' in the extracted path
	buf = strrchr(pci_path, '/');
	if (!buf)
		return -ENOENT;

	// Extract the PCI device address from the path using sscanf
	ret = sscanf(buf, "/%4x:%2x:%2x.%2x", &pci->domain, &pci->bus,
			&pci->device, &pci->function);

	if (ret != 4) {
		igt_info("error %s Unable to extract PCI device address from '%s\n",
				__func__, buf);
		return -ENOENT;
	}

	return 0;
}

/**
 * Find corresponding dri_id from PCI address
 * @param pci Pointer to PCI address structure
 * @return Found dri_id, -1 if failed
 */
int find_dri_id_by_pci(const struct pci_addr *pci)
{
	char pci_str[32];
	char path[256];
	char buffer[128];
	FILE *dri_fp;
	int dri_id;

	if (!pci)
		return -1;

	snprintf(pci_str, sizeof(pci_str), "%04x:%02x:%02x.%01x",
	pci->domain, pci->bus, pci->device, pci->function);

	/* Search for corresponding dri_id (typically not very large, limit to 1024 here) */
	for (dri_id = 0; dri_id < 1024; dri_id++) {
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/uevent", dri_id);
		dri_fp = fopen(path, "r");
		if (!dri_fp)
		continue;

		/* Read uevent file to find matching PCI address */
		while (fgets(buffer, sizeof(buffer), dri_fp)) {
			if (strstr(buffer, "PCI_SLOT_NAME=") && strstr(buffer, pci_str)) {
			fclose(dri_fp);
			return dri_id;
			}
		}
		fclose(dri_fp);
	}

	return -1;
}

/*
 * Function to check if page queue files exist for a given IP block type and PCI address
 */
bool is_support_page_queue(enum amd_ip_block_type ip_type, const struct pci_addr *pci)
{
	glob_t glob_result;
	int ret;
	char search_pattern[1024];

	/* If the IP type is not SDMA, return false */
	if (ip_type != AMD_IP_DMA)
		return false;

	/* Construct the search pattern for the page queue files */
	snprintf(search_pattern, sizeof(search_pattern) - 1, "/sys/kernel/debug/dri/%04x:%02x:%02x.%01x/amdgpu_ring_page*",
		pci->domain, pci->bus, pci->device, pci->function);

	/* Use glob to find files matching the pattern */
	ret = glob(search_pattern, GLOB_NOSORT, NULL, &glob_result);
	/* Free the memory allocated by glob */
	globfree(&glob_result);

	/* Return true if files matching the pattern were found, otherwise return false */
	return (ret == 0 && glob_result.gl_pathc > 0);
}

int get_dri_index_from_device(amdgpu_device_handle device, int fd)
{
	/* For AMDGPU, the DRI index is typically available through the render node */
	/* We can use the device fd to determine the appropriate debugfs path */
	char path[64];
	char target[1024];
	ssize_t len;
	int dri_index = 0;

	/* Try to read the symlink from /proc/self/fd */
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

	len = readlink(path, target, sizeof(target) - 1);
	if (len != -1) {
		target[len] = '\0';
		/* Extract DRI index from path like /dev/dri/renderD128 */
		if (sscanf(target, "/dev/dri/renderD%d", &dri_index) == 1) {
			return dri_index;
		}
		/* Try card path as well */
		if (sscanf(target, "/dev/dri/card%d", &dri_index) == 1) {
			return dri_index;
		}
	}

	return 0;
}

bool is_apu(const struct amdgpu_gpu_info *info)
{
	return !!(info && (info->ids_flags & AMDGPU_IDS_FLAGS_FUSION));
}

/**
 * Get IP block name string
 */
const char *cmd_get_ip_name(enum amd_ip_block_type ip_type)
{
	switch (ip_type) {
	case AMD_IP_GFX:
		return "GFX";
	case AMD_IP_COMPUTE:
		return "Compute";
	case AMD_IP_DMA:
		return "DMA";
	case AMD_IP_UVD:
		return "UVD";
	case AMD_IP_VCE:
		return "VCE";
	case AMD_IP_UVD_ENC:
		return "UVD_ENC";
	case AMD_IP_VCN_DEC:
		return "VCN_DEC";
	case AMD_IP_VCN_ENC:
		return "VCN_ENC";
	case AMD_IP_VCN_JPEG:
		return "VCN_JPEG";
	case AMD_IP_VPE:
		return "VPE";
	default:
		return "Unknown";
	}
}

bool is_spx_mode(const struct pci_addr *pci)
{
	char path[256];
	char buffer[16] = {0};
	FILE *fp;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/current_compute_partition",
		 pci->domain, pci->bus, pci->device, pci->function);

	/* If file doesn't exist, assume SPX or non-partitioned capable */
	if (access(path, R_OK) != 0)
		return true;

	fp = fopen(path, "r");
	if (!fp)
		return true;

	if (fgets(buffer, sizeof(buffer), fp)) {
		if (strstr(buffer, "SPX")) {
			fclose(fp);
			return true;
		}
	}
	fclose(fp);
	return false;
}

long amdgpu_get_ip_schedule_mask(const struct pci_addr *pci, enum amd_ip_block_type ip_type, char *sysfs_path)
{
	char sysfs_local[256];
	char *sysfs = sysfs_path ? sysfs_path : sysfs_local;
	char cmd[512];
	char buffer[128];
	FILE *fp;
	long sched_mask = 0;
	int dri_id;
	const char *mask_name = NULL;

	switch (ip_type) {
	case AMD_IP_GFX:
		mask_name = "amdgpu_gfx_sched_mask";
		break;
	case AMD_IP_COMPUTE:
		mask_name = "amdgpu_compute_sched_mask";
		break;
	case AMD_IP_DMA:
		mask_name = "amdgpu_sdma_sched_mask";
		break;
	case AMD_IP_VCN_UNIFIED:
		mask_name = "amdgpu_vcn_sched_mask";
		break;
	case AMD_IP_VCN_JPEG:
		mask_name = "amdgpu_jpeg_sched_mask";
		break;
	default:
		return 0;
	}

	snprintf(sysfs, 256, "/sys/kernel/debug/dri/%04x:%02x:%02x.%01x/%s",
		 pci->domain, pci->bus, pci->device, pci->function, mask_name);

	if (access(sysfs, F_OK) != 0) {
		dri_id = find_dri_id_by_pci(pci);
		if (dri_id < 0)
			dri_id = 0;

		snprintf(sysfs, 256, "/sys/kernel/debug/dri/%d/%s", dri_id, mask_name);
	}

	if (access(sysfs, R_OK) != 0)
		return 1;

	snprintf(cmd, sizeof(cmd) - 1, "sudo cat %s", sysfs);

	fp = popen(cmd, "r");
	if (fp) {
		if (fgets(buffer, sizeof(buffer), fp))
			sched_mask = strtol(buffer, NULL, 16);
		pclose(fp);
	}

	return sched_mask;
}
