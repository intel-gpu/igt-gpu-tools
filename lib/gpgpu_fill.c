/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *  Zhenyu Wang <zhenyuw@linux.intel.com>
 *  Dominik Zeromski <dominik.zeromski@intel.com>
 */

#include <i915_drm.h>
#include <ioctl_wrappers.h>
#include "intel_reg.h"
#include "drmtest.h"

#include "gpgpu_fill.h"
#include "gpgpu_shader.h"
#include "gpu_cmds.h"
#include "xe/xe_util.h"

/* lib/i915/shaders/gpgpu/gpgpu_fill.gxa */
static const uint32_t gen7_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20200231, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400c21, 0x00000004, 0x00000010 },
	{ 0x00000001, 0x20440021, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800021, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800021, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880061, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00021, 0x00000020, 0x00000000 },
	{ 0x05800031, 0x24001ca8, 0x00000080, 0x060a8000 },
	{ 0x00600001, 0x2e000021, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20001ca8, 0x00000e00, 0x82000010 },
};

static const uint32_t gen8_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x0e000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
};

static const uint32_t gen9_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x06000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

static const uint32_t gen11_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000009, 0x20400208, 0x06000004, 0x00000004 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x06000080, 0x040a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

static const uint32_t gen12_gpgpu_kernel[][4] = {
	{ 0x00020061, 0x01050000, 0x00000104, 0x00000000 },
	{ 0x00000069, 0x02058220, 0x02000024, 0x00000004 },
	{ 0x00000061, 0x02250220, 0x000000c4, 0x00000000 },
	{ 0x00030061, 0x04050220, 0x00460005, 0x00000000 },
	{ 0x00010261, 0x04050220, 0x00220205, 0x00000000 },
	{ 0x00000061, 0x04454220, 0x00000000, 0x0000000f },
	{ 0x00040661, 0x05050220, 0x00000104, 0x00000000 },
	{ 0x00049031, 0x00000000, 0xc0000414, 0x02a00000 },
	{ 0x00030061, 0x70050220, 0x00460005, 0x00000000 },
	{ 0x00040131, 0x00000004, 0x7020700c, 0x10000000 },
};

/*
 * This sets up the gpgpu pipeline,
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 */

#define PAGE_SIZE 4096
#define BATCH_STATE_SPLIT 2048
/* VFE STATE params */
#define THREADS 1
#define GEN7_GPGPU_URB_ENTRIES 0
#define GEN8_GPGPU_URB_ENTRIES 1
#define GPGPU_URB_SIZE 0
#define GPGPU_CURBE_SIZE 1
#define GEN7_VFE_STATE_GPGPU_MODE 1

void
gen7_gpgpu_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/* Fill curbe buffer data */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	interface_descriptor =
			gen7_fill_interface_descriptor(ibb, buf,
						       gen7_gpgpu_kernel,
						       sizeof(gen7_gpgpu_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen7_emit_state_base_address(ibb);
	gen7_emit_vfe_state(ibb, THREADS, GEN7_GPGPU_URB_ENTRIES,
			       GPGPU_URB_SIZE, GPGPU_CURBE_SIZE,
			       GEN7_VFE_STATE_GPGPU_MODE);
	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);
	gen7_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void
gen8_gpgpu_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
				gen8_gpgpu_kernel, sizeof(gen8_gpgpu_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen8_emit_state_base_address(ibb);
	gen8_emit_vfe_state(ibb, THREADS, GEN8_GPGPU_URB_ENTRIES,
			    GPGPU_URB_SIZE, GPGPU_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen8_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

static void
__gen9_gpgpu_fillfunc(int i915,
		      struct intel_buf *buf,
		      unsigned x, unsigned y,
		      unsigned width, unsigned height,
		      uint8_t color,
		      const uint32_t kernel[][4], size_t kernel_size)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	/* Fill curbe buffer data */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
							      kernel,
							      kernel_size);

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | GEN9_PIPELINE_SELECTION_MASK |
		     PIPELINE_SELECT_GPGPU);

	gen9_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, GEN8_GPGPU_URB_ENTRIES,
			    GPGPU_URB_SIZE, GPGPU_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen8_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_RENDER | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

static struct gpgpu_shader *__xehp_gpgpu_kernel(int fd, struct intel_buf *buf)
{
	struct gpgpu_shader *kernel = gpgpu_shader_create(fd);
	uint64_t offset;

	if (is_xe_device(fd))
		offset = xe_canonical_va(fd, buf->addr.offset);
	else
		offset = CANONICAL(buf->addr.offset);

	emit_iga64_code(kernel, gpgpu_fill, R"(
// fill up r1 with target colour
mov (4|M0)		r1.0<1>:ub	r1.0<0;1,0>:ub
// prepare block x offset (Thread Group Id X * 16)
shl (1|M0)		r2.0<1>:ud	r0.1<0;1,0>:ud	0x4:ud
// prepare block y offset (Thread Group Id Y)
mov (1|M0)		r2.1<1>:ud	r0.6<0;1,0>:ud
// zero message header payload
mov (8|M0)		r4.0<1>:ud	0x0:ud
// fill up message payload with target colour
mov (16|M0)		r5.0<1>:ud	r1.0<0;1,0>:ud
#if GFX_VER < 2000
// load block offsets into message header payload
mov (2|M0)		r4.0<1>:ud	r2.0<2;2,1>:ud
// load block width
mov (1|M0)		r4.2<1>:ud	0xF:ud
// load FFTID from R0 header
mov (1|M0)		r4.4<1>:ud	r0.5<0;1,0>:ud
// Media block write to bti[0] surface
// Message Descriptor
//	0x40A8000:
//	[28:25]		Mlen: 2
//	[24:20]		Rlen: 0
//	[19]		Header: 1 (included)
//	[18:14]		MessageType: 0xA (media block write)
//	[7:0]		BTI: 0
send.dc1 (16|M0)	null	r4	src1_null	0x0	0x40A8000
#elif GFX_VER < 3000
// load block offsets into message header payload
mov (2|M0)		r4.5<1>:ud	r2.0<2;2,1>:ud
// load block width
mov (1|M0)		 r4.14<1>:w	0xF:w
// Typed 2D block store to bti[0] surface
// Message Descriptor
//	0x6400007:
//	[30:29]		AddrType: 3 (BTI)
//	[28:25]		Mlen: 2
//	[24:20]		Rlen: 0
//	[19:17]		Caching: 0  (use state settings for both L1 and L3)
//	[5:0]		Opcode: 0x07  (store_block2d)
send.tgm (16|M0)	null	r4	null	0x0	0x64000007
#else
// load A64 base surface state offset into address payload
mov (1|M0)		r4.0<1>:ud	ARG(0):ud
mov (1|M0)		r4.1<1>:ud	ARG(1):ud
// load surface width
mov (1|M0)		r4.2<1>:ud	ARG(2):ud
// load surface height
mov (1|M0)		r4.3<1>:ud	ARG(3):ud
// load surface pitch
mov (1|M0)		r4.4<1>:ud	ARG(4):ud
// load block offsets
mov (2|M0)		r4.5<1>:ud	r2.0<2;2,1>:ud
// load block width
mov (1|M0)		r4.14<1>:w	0xF:w
// Untyped 2D block store
// Message Descriptor
//	bspec:63981
//	0x2020007:
//	[30:29]		AddrType: 0 (Flat)
//	[28:25]		Mlen: 1 address register written
//	[24:20]		Rlen: 0 registers read back
//	[19:17]		Caching: 1  (L1 uncached, L3 uncached)
//	[15]		DataOrder: 0  non-transposed
//	[11:9]		DataSize: 0  (8b per data element)
//	[5:0]		Opcode: 0x07  (store_block2d)
send.ugm (1|M0)		null	r4	r5:1	0x0	0x2020007
#endif
	)", offset & 0xffffffff, offset >> 32,
	intel_buf_width(buf) * buf->bpp/8 - 1, intel_buf_height(buf) - 1,
	buf->surface[0].stride - 1);

	gpgpu_shader__eot(kernel);
	return kernel;
}

static struct gpgpu_shader *__xe3p_gpgpu_kernel(int xe)
{
	struct gpgpu_shader *kernel = gpgpu_shader_create(xe);

	emit_iga64_code(kernel, xe3p_gpgpu_fill, R"(
#define RX		r0.1
#define RY		r0.6
#define COLOR		r1.0
#define SURFWIDTH	r1.1
#define SURFHEIGHT	r1.2
#define WIDTH		r1.3
#define HEIGHT		r1.4
#define XPOS		r1.5
#define YPOS		r1.6
#define ADDR_BO_LOW	r1.7
#define ADDR_BO_HIGH	r1.8
#define OFFSET		r3.0
#define XOFFSET		r3.1
#define YOFFSET		r3.2
#define XEND		r3.3
#define XCURRENT	r3.4
#define TMP		r3.7
#define ADDR_LO		r4.0
#if GFX_VER >= 3500
(W)	add (1)		XEND<1>:ud	XPOS:ud		WIDTH:ud
(W)	mov (1)		OFFSET<1>:ud	0x0:ud

(W)	shl (1)		XOFFSET<1>:ud	RX:ud		0x4:ud
(W)	add (1)		XOFFSET<1>:ud	XOFFSET:ud	XPOS:ud
(W)	mov (1)		XCURRENT<1>:ud	XOFFSET:ud

(W)	add (1)		TMP<1>:ud	RY:ud		YPOS:ud
(W)	mul (1)		YOFFSET<1>:ud	TMP:ud		SURFWIDTH:ud
(W)	add (1)		OFFSET<1>:ud	XOFFSET:ud	YOFFSET:ud

// Set base address with scalar register
(W)	add (1)		ADDR_LO<1>:ud	OFFSET:ud	ADDR_BO_LOW:ud
(W)	mov (1)		s0.0<1>:ud	ADDR_LO:ud
(W)	mov (1)		s0.1<1>:ud	ADDR_BO_HIGH:ud

// color
(W)	mov (4)		r20.0<1>:ub		COLOR:ub

// A64 offset
(W)	mov (8)		r30.0<1>:uq		0x0:uq

//dword: 0
(W)	cmp (1)		(lt)f0.0 null:ud        XCURRENT:ud	XEND:ud
(W&f0.0)sendg.ugm (1)	null	r30:1	r20:1	s0.0	0x29404
//dword: 1
(W)	add (1)		XCURRENT<1>:ud		XCURRENT:ud	4:ud
(W)	add (1)		ADDR_LO<1>:ud		ADDR_LO:ud	0x4:ud
(W)	mov (1)		s0.0<1>:ud		ADDR_LO:ud
(W)	cmp (1)		(lt)f0.0 null:ud        XCURRENT:ud	XEND:ud
(W&f0.0)sendg.ugm (1)	null	r30:1	r20:1	s0.0	0x29404
//dword: 2
(W)	add (1)		XCURRENT<1>:ud		XCURRENT:ud	4:ud
(W)	add (1)		ADDR_LO<1>:ud		ADDR_LO:ud	0x4:ud
(W)	mov (1)		s0.0<1>:ud		ADDR_LO:ud
(W)	cmp (1)		(lt)f0.0 null:ud        XCURRENT:ud	XEND:ud
(W&f0.0)sendg.ugm (1)	null	r30:1	r20:1	s0.0	0x29404
//dword: 3
(W)	add (1)		XCURRENT<1>:ud		XCURRENT:ud	4:ud
(W)	add (1)		ADDR_LO<1>:ud		ADDR_LO:ud	0x4:ud
(W)	mov (1)		s0.0<1>:ud		ADDR_LO:ud
(W)	cmp (1)		(lt)f0.0 null:ud        XCURRENT:ud	XEND:ud
(W&f0.0)sendg.ugm (1)	null	r30:1	r20:1	s0.0	0x29404
#endif
)");

	gpgpu_shader__eot(kernel);
	return kernel;
}

void xehp_gpgpu_fillfunc(int fd,
			 struct intel_buf *buf,
			 unsigned int x, unsigned int y,
			 unsigned int width, unsigned int height,
			 uint8_t color)
{
	struct intel_bb *ibb;
	struct gpgpu_shader *kernel;
	struct xehp_interface_descriptor_data idd;

	ibb = intel_bb_create(fd, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	kernel = __xehp_gpgpu_kernel(fd, buf);
	xehp_fill_interface_descriptor(ibb, buf, kernel->instr,
				       kernel->size * 4, &idd);
	gpgpu_shader_destroy(kernel);

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | GEN9_PIPELINE_SELECTION_MASK |
		  PIPELINE_SELECT_GPGPU);
	xehp_emit_state_base_address(ibb);
	xehp_emit_state_compute_mode(ibb, false, 0);
	xehp_emit_state_binding_table_pool_alloc(ibb);
	xehp_emit_cfe_state(ibb, THREADS);
	xehp_emit_compute_walk(ibb, x, y, width, height, &idd, color);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_RENDER | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void xe3p_gpgpu_fillfunc(int fd,
			 struct intel_buf *buf,
			 unsigned int x, unsigned int y,
			 unsigned int width, unsigned int height,
			 uint8_t color)
{
	struct intel_bb *ibb;
	struct gpgpu_shader *kernel;
	struct xe3p_interface_descriptor_data idd;

	ibb = intel_bb_create(fd, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	kernel = __xe3p_gpgpu_kernel(fd);
	xe3p_fill_interface_descriptor(ibb, buf, kernel->instr,
				       kernel->size * 4, &idd);
	gpgpu_shader_destroy(kernel);

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | GEN9_PIPELINE_SELECTION_MASK |
		  PIPELINE_SELECT_GPGPU);
	xe3p_emit_state_base_address(ibb);
	xehp_emit_state_compute_mode(ibb, false, 0);
	xe3p_emit_fill_compute_walk2(ibb, buf->width * buf->bpp / 8, buf->height,
				     xe_canonical_va(fd, buf->addr.offset),
				     x, y, width, height, &idd, color);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_RENDER | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}


void gen9_gpgpu_fillfunc(int i915,
			 struct intel_buf *buf,
			 unsigned x, unsigned y,
			 unsigned width, unsigned height,
			 uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen9_gpgpu_kernel,
			      sizeof(gen9_gpgpu_kernel));
}

void gen11_gpgpu_fillfunc(int i915,
			  struct intel_buf *buf,
			  unsigned x, unsigned y,
			  unsigned width, unsigned height,
			  uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen11_gpgpu_kernel,
			      sizeof(gen11_gpgpu_kernel));
}

void gen12_gpgpu_fillfunc(int i915,
			  struct intel_buf *buf,
			  unsigned x, unsigned y,
			  unsigned width, unsigned height,
			  uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen12_gpgpu_kernel,
			      sizeof(gen12_gpgpu_kernel));
}
