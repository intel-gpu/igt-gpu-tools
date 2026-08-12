/*
 * Copyright © 2018 Intel Corporation
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
 */

#include "gpgpu_shader.h"
#include "gpu_cmds.h"
#include "intel_mocs.h"
#include "xe/xe_util.h"

static uint32_t
xehp_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst);

uint32_t
gen7_fill_curbe_buffer_data(struct intel_bb *ibb, uint8_t color)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	curbe_buffer = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	*curbe_buffer = color;
	intel_bb_ptr_add(ibb, 32);

	return offset;
}

uint32_t
gen11_fill_curbe_buffer_data(struct intel_bb *ibb)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	curbe_buffer = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	*curbe_buffer++ = 0;
	*curbe_buffer = 1;
	intel_bb_ptr_add(ibb, 64);

	return offset;
}

static uint32_t
gen7_fill_kernel(struct intel_bb *ibb,
		const uint32_t kernel[][4],
		size_t size)
{
	return intel_bb_copy_data(ibb, kernel, size, 64);
}

static uint32_t
gen7_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen7_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4, buf->addr.offset);
	igt_assert(address >> 32 == 0);

	ss->ss1.base_addr = address;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen8_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen8_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y || buf->tiling == I915_TILING_4)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, buf->addr.offset);

	ss->ss8.base_addr = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen9_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct gen9_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */

	ss->ss1.mocs_index = buf->mocs_index;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y || buf->tiling == I915_TILING_4)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, buf->addr.offset);

	ss->ss8.base_addr = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return offset;
}

static uint32_t
gen11_fill_surface_state(struct intel_bb *ibb,
			 const struct intel_buf *buf,
			 uint32_t surface_type,
			 uint32_t format,
			 uint32_t vertical_alignment,
			 uint32_t horizontal_alignment,
			 int is_dst)
{
	struct gen9_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = surface_type;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = vertical_alignment; /* align 4 */
	ss->ss0.horizontal_alignment = horizontal_alignment; /* align 4 */

	ss->ss1.mocs_index = buf->mocs_index;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y || buf->tiling == I915_TILING_4)
		ss->ss0.tiled_mode = 3;
	else
		ss->ss0.tiled_mode = 0;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, buf->addr.offset);

	ss->ss8.base_addr = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	if (is_dst) {
		ss->ss1.mocs_index = I915_MOCS_PTE;
		ss->ss2.height = 1;
		ss->ss2.width  = 95;
		ss->ss3.pitch  = 0;
		ss->ss7.shader_chanel_select_r = 4;
		ss->ss7.shader_chanel_select_g = 5;
		ss->ss7.shader_chanel_select_b = 6;
		ss->ss7.shader_chanel_select_a = 7;
	}
	else {
		ss->ss1.qpitch = 4040;
		ss->ss1.base_mip_level = 31;
		ss->ss2.height = 9216;
		ss->ss2.width  = 1019;
		ss->ss3.pitch  = 64;
		ss->ss5.mip_count = 2;
	}

	return offset;
}

static uint32_t
fill_binding_table(struct intel_bb *ibb, struct intel_buf *buf)
{
	uint32_t binding_table_offset;
	uint32_t *binding_table;
	uint32_t devid = intel_get_drm_devid(ibb->fd);

	intel_bb_ptr_align(ibb, 64);
	binding_table_offset = intel_bb_offset(ibb);
	binding_table = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	if (intel_graphics_ver(devid) >= IP_VER(20, 0)) {
		/*
		 * Up until now, SURFACEFORMAT_R8_UNROM was used regardless of the 'bpp' value.
		 * For bpp 32 this results in a surface that is 4x narrower than expected. However
		 * it worked, because the 'Media Block Read/Write' message assumes the surface width
		 * is always in units of dwords.
		 *
		 * Since Xe2 the Media Block Write message got replaced with 'Typed 2D Block
		 * Load/Store Message' which correctly interprets the surface format.
		 */
		if (buf->bpp == 32)
			binding_table[0] = xehp_fill_surface_state(ibb, buf,
								      SURFACEFORMAT_R8G8B8A8_UNORM,
								      1);
		else if (buf->bpp == 8)
			binding_table[0] = xehp_fill_surface_state(ibb, buf,
								      SURFACEFORMAT_R8_UNORM,
								      1);
		else
			igt_assert_f(false,
				     "Surface state for bpp = %u not implemented",
				     buf->bpp);
	} else if (intel_graphics_ver(devid) >= IP_VER(12, 50)) {
		binding_table[0] = xehp_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);
	} else if (intel_graphics_ver(devid) >= IP_VER(9, 0)) {
		binding_table[0] = gen9_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);
	} else if (intel_graphics_ver(devid) >= IP_VER(8, 0)) {
		binding_table[0] = gen8_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);
	} else {
		binding_table[0] = gen7_fill_surface_state(ibb, buf,
							   SURFACEFORMAT_R8_UNORM, 1);
	}

	return binding_table_offset;
}

static uint32_t
gen11_fill_binding_table(struct intel_bb *ibb,
			 const struct intel_buf *src,
			 const struct intel_buf *dst)
{
	uint32_t binding_table_offset;
	uint32_t *binding_table;

	intel_bb_ptr_align(ibb, 64);
	binding_table_offset = intel_bb_offset(ibb);
	binding_table = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	binding_table[0] = gen11_fill_surface_state(ibb, src,
						    SURFACE_1D,
						    SURFACEFORMAT_R32G32B32A32_FLOAT,
						    0, 0, 0);
	binding_table[1] = gen11_fill_surface_state(ibb, dst,
						    SURFACE_BUFFER,
						    SURFACEFORMAT_RAW,
						    1, 1, 1);

	return binding_table_offset;
}

uint32_t
gen7_fill_interface_descriptor(struct intel_bb *ibb,
			       struct intel_buf *buf,
			       const uint32_t kernel[][4],
			       size_t size)
{
	struct gen7_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = fill_binding_table(ibb, buf);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc1.single_program_flow = 1;
	idd->desc1.floating_point_mode = GEN7_FLOATING_POINT_IEEE_754;

	idd->desc2.sampler_count = 0;      /* 0 samplers used */
	idd->desc2.sampler_state_pointer = 0;

	idd->desc3.binding_table_entry_count = 0;
	idd->desc3.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc4.constant_urb_entry_read_offset = 0;
	idd->desc4.constant_urb_entry_read_length = 1; /* grf 1 */

	intel_bb_ptr_add(ibb, sizeof(*idd));

	return offset;
}

uint32_t
gen8_fill_interface_descriptor(struct intel_bb *ibb,
			       struct intel_buf *buf,
			       const uint32_t kernel[][4],
			       size_t size)
{
	struct gen8_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = fill_binding_table(ibb, buf);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.constant_urb_entry_read_offset = 0;
	idd->desc5.constant_urb_entry_read_length = 1; /* grf 1 */

	idd->desc6.num_threads_in_tg = 1;

	intel_bb_ptr_add(ibb, sizeof(*idd));

	return offset;
}

uint32_t
gen11_fill_interface_descriptor(struct intel_bb *ibb,
				struct intel_buf *src, struct intel_buf *dst,
				const uint32_t kernel[][4],
				size_t size)
{
	struct gen8_interface_descriptor_data *idd;
	uint32_t offset;
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = gen11_fill_binding_table(ibb, src, dst);
	kernel_offset = gen7_fill_kernel(ibb, kernel, size);

	intel_bb_ptr_align(ibb, 64);
	idd = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.constant_urb_entry_read_offset = 0;
	idd->desc5.constant_urb_entry_read_length = 1; /* grf 1 */

	idd->desc6.num_threads_in_tg = 1;

	intel_bb_ptr_add(ibb, sizeof(*idd));

	return offset;
}

void
gen7_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_STATE_BASE_ADDRESS | (10 - 2));

	/* general */
	intel_bb_out(ibb, 0);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* general/dynamic/indirect/instruction access Bound */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
}

void
gen8_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | (16 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);


	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* instruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);
}

void
gen9_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | (19 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, 0x0);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* intruction buffer size, must set modify enable bit, otherwise it may
	 * result in GPU hang
	 */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);

	/* Bindless surface state base address */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0xfffff000);
}

void
gen7_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
		    uint32_t urb_entries, uint32_t urb_size,
		    uint32_t curbe_size, uint32_t mode)
{
	intel_bb_out(ibb, GEN7_MEDIA_VFE_STATE | (8 - 2));

	/* scratch buffer */
	intel_bb_out(ibb, 0);

	/* number of threads & urb entries */
	intel_bb_out(ibb, threads << 16 |
		     urb_entries << 8 |
		     mode << 2); /* GPGPU vs media mode */

	intel_bb_out(ibb, 0);

	/* urb entry size & curbe size */
	intel_bb_out(ibb, urb_size << 16 |	/* in 256 bits unit */
		     curbe_size);		/* in 256 bits unit */

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
__gen8_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
		      uint32_t urb_entries, uint32_t urb_size,
		      uint32_t curbe_size, bool legacy_mode)
{
	intel_bb_out(ibb, GEN7_MEDIA_VFE_STATE | (9 - 2));

	/* scratch buffer */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* number of threads & urb entries & eu fusion */
	intel_bb_out(ibb, threads << 16 | urb_entries << 8 | legacy_mode << 6);

	intel_bb_out(ibb, 0);

	/* urb entry size & curbe size */
	intel_bb_out(ibb, urb_size << 16 | curbe_size);

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

/**
 * gen8_emit_vfe_state:
 * @ibb: batchbuffer
 * @threads: maximum number of threads
 * @urb_entries: number of URB entries
 * @urb_size: URB entry allocation size
 * @curbe_size: CURBE allocation size
 *
 * Emits instruction MEDIA_VFE_STATE for Gen8+ which sets Video Front End (VFE)
 * state.
 */
void gen8_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
			 uint32_t urb_entries, uint32_t urb_size,
			 uint32_t curbe_size)
{
	__gen8_emit_vfe_state(ibb, threads, urb_entries, urb_size, curbe_size,
			      false);
}

void
gen7_emit_curbe_load(struct intel_bb *ibb, uint32_t curbe_buffer)
{
	intel_bb_out(ibb, GEN7_MEDIA_CURBE_LOAD | (4 - 2));
	intel_bb_out(ibb, 0);
	/* curbe total data length */
	intel_bb_out(ibb, 64);
	/* curbe data start address, is relative to the dynamics base address */
	intel_bb_out(ibb, curbe_buffer);
}

void
gen7_emit_interface_descriptor_load(struct intel_bb *ibb,
				    uint32_t interface_descriptor)
{
	intel_bb_out(ibb, GEN7_MEDIA_INTERFACE_DESCRIPTOR_LOAD | (4 - 2));
	intel_bb_out(ibb, 0);
	/* interface descriptor data length */
	if (ibb->gen == 7)
		intel_bb_out(ibb, sizeof(struct gen7_interface_descriptor_data));
	else
		intel_bb_out(ibb, sizeof(struct gen8_interface_descriptor_data));
	/* interface descriptor address, is relative to the dynamics base
	 * address
	 */
	intel_bb_out(ibb, interface_descriptor);
}

void
gen7_emit_gpgpu_walk(struct intel_bb *ibb,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height)
{
	uint32_t x_dim, y_dim, tmp, right_mask;

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (x + width + 15) / 16;
	y_dim = y + height;

	tmp = (x + width) & 15;
	if (tmp == 0)
		right_mask = (1 << 16) - 1;
	else
		right_mask = (1 << tmp) - 1;

	intel_bb_out(ibb, GEN7_GPGPU_WALKER | 9);

	/* interface descriptor offset */
	intel_bb_out(ibb, 0);

	/* SIMD size, thread w/h/d */
	intel_bb_out(ibb, 1 << 30 | /* SIMD16 */
		  0 << 16 | /* depth:1 */
		  0 << 8 | /* height:1 */
		  0); /* width:1 */

	/* thread group X */
	intel_bb_out(ibb, x / 16);
	intel_bb_out(ibb, x_dim);

	/* thread group Y */
	intel_bb_out(ibb, y);
	intel_bb_out(ibb, y_dim);

	/* thread group Z */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1);

	/* right mask */
	intel_bb_out(ibb, right_mask);

	/* bottom mask, height 1, always 0xffffffff */
	intel_bb_out(ibb, 0xffffffff);
}

void
gen8_emit_gpgpu_walk(struct intel_bb *ibb,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height)
{
	uint32_t x_dim, y_dim, tmp, right_mask;

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (x + width + 15) / 16;
	y_dim = y + height;

	tmp = (x + width) & 15;
	if (tmp == 0)
		right_mask = (1 << 16) - 1;
	else
		right_mask = (1 << tmp) - 1;

	intel_bb_out(ibb, GEN7_GPGPU_WALKER | 13);

	intel_bb_out(ibb, 0); /* kernel offset */
	intel_bb_out(ibb, 0); /* indirect data length */
	intel_bb_out(ibb, 0); /* indirect data offset */

	/* SIMD size, thread w/h/d */
	intel_bb_out(ibb, 1 << 30 | /* SIMD16 */
		     0 << 16 | /* depth:1 */
		     0 << 8 | /* height:1 */
		     0); /* width:1 */

	/* thread group X */
	intel_bb_out(ibb, x / 16);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, x_dim);

	/* thread group Y */
	intel_bb_out(ibb, y);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, y_dim);

	/* thread group Z */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1);

	/* right mask */
	intel_bb_out(ibb, right_mask);

	/* bottom mask, height 1, always 0xffffffff */
	intel_bb_out(ibb, 0xffffffff);
}

void
gen8_emit_media_state_flush(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_MEDIA_STATE_FLUSH | (2 - 2));
	intel_bb_out(ibb, 0);
}

void
gen_emit_media_object(struct intel_bb *ibb,
		      unsigned int xoffset, unsigned int yoffset)
{
	intel_bb_out(ibb, GEN7_MEDIA_OBJECT | (8 - 2));

	/* interface descriptor offset */
	intel_bb_out(ibb, 0);

	/* without indirect data */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* scoreboard */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* inline data (xoffset, yoffset) */
	intel_bb_out(ibb, xoffset);
	intel_bb_out(ibb, yoffset);
	if (intel_gen(ibb->devid) >= 8 && !IS_CHERRYVIEW(ibb->devid))
		gen8_emit_media_state_flush(ibb);
}

void
gen7_emit_media_objects(struct intel_bb *ibb,
			unsigned int x, unsigned int y,
			unsigned int width, unsigned int height)
{
	int i, j;

	for (i = 0; i < width / 16; i++)
		for (j = 0; j < height / 16; j++)
			gen_emit_media_object(ibb, x + i * 16, y + j * 16);
}

/**
 * xelp_emit_vfe_state:
 * @ibb: pointer to intel_bb
 * @threads: maximum number of threads
 * @urb_entries: number of URB entries
 * @urb_size: URB entry allocation size
 * @curbe_size: CURBE allocation size
 * @legacy_mode: if set, threads are dispatched individually (legacy mode),
 *     otherwise they are dispatched in sets(fused EU mode)
 *
 * Emits instruction MEDIA_VFE_STATE for XeLP which sets Video Front End (VFE)
 * state.
 */
void xelp_emit_vfe_state(struct intel_bb *ibb, uint32_t threads,
			 uint32_t urb_entries, uint32_t urb_size,
			 uint32_t curbe_size, bool legacy_mode)
{
	__gen8_emit_vfe_state(ibb, threads, urb_entries, urb_size,
			      curbe_size, legacy_mode);
}

static int __lower_bound(int val, const int *arr, int size)
{
	for (int i = 0; i < size; ++i) {
		if (arr[i] >= val)
			return i;
	}
	return -1;
}

#define NOPAREN(...) __VA_ARGS__
#define lookup_lower_bound(_key, _keys, _vals) ({				\
	static const int keys[] = { NOPAREN _keys }, vals[] = { NOPAREN _vals };\
	int size = ARRAY_SIZE(keys), i;						\
										\
	igt_assert_eq(size, ARRAY_SIZE(vals));					\
	i = __lower_bound(_key, keys, size);					\
	igt_assert_lte(0, i);							\
	vals[i];								\
})

/* bspec: 56593 */
static void idd_set_grfs_per_thread(struct xehp_interface_descriptor_data *idd,
				    const struct gpgpu_shader *shdr)
{
	if (shdr->gfx_ver < 3000)
		return;

	idd->desc2.registers_per_thread = lookup_lower_bound(shdr->grfs_per_thread,
		(32, 64, 96, 128, 160, 192, 256),
		( 0,  1,  2,   3,   4,   5,   7));
}

void
xehp_fill_interface_descriptor(struct intel_bb *ibb,
			       struct intel_buf *dst,
			       const struct gpgpu_shader *shdr,
			       struct xehp_interface_descriptor_data *idd)
{
	uint32_t binding_table_offset, kernel_offset;

	binding_table_offset = fill_binding_table(ibb, dst);
	kernel_offset = gen7_fill_kernel(ibb, shdr->instr, 4 * shdr->size);

	memset(idd, 0, sizeof(*idd));
	idd->desc0.kernel_start_pointer = (kernel_offset >> 6);

	idd->desc2.single_program_flow = 1;
	idd->desc2.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;
	if (shdr->grfs_per_thread)
		idd_set_grfs_per_thread(idd, shdr);

	idd->desc3.sampler_count = 0;      /* 0 samplers used */
	idd->desc3.sampler_state_pointer = 0;

	idd->desc4.binding_table_entry_count = 0;
	idd->desc4.binding_table_pointer = (binding_table_offset >> 5);

	idd->desc5.num_threads_in_tg = 1;
}

/* bspec: 77779 */
static void idd2_set_grfs_per_thread(struct xe3p_interface_descriptor_data *idd,
				     const struct gpgpu_shader *shdr)
{
	idd->dw02.registers_per_thread = lookup_lower_bound(shdr->grfs_per_thread,
		(16, 32, 48, 64, 80, 96, 128, 160, 256),
		( 0,  1,  2,  3,  4,  5,   7,   9,  15));
}

/* bspec: 75546 */
static void idd2_set_slm_size(struct xe3p_interface_descriptor_data *idd,
		     const struct gpgpu_shader *shdr)
{
	int v = lookup_lower_bound(shdr->slm_size,
		(0, 1, 2,  3, 4,  5,  6,  7, 8,  9, 10, 11, 12, 13, 14, 15, 16, 24, 32, 48, 64, 96, 128, 192, 256, 320, 384),
		(0, 1, 2, 16, 3, 17, 18, 19, 4, 20, 21, 22, 23, 24, 25, 26,  5,  8,  6,  9,  7, 10,  11,  12,  13,  14,  15));
	idd->dw05.shared_local_memory_size = v;
}

void
xe3p_fill_interface_descriptor(struct intel_bb *ibb,
			       const struct gpgpu_shader *shdr,
			       struct xe3p_interface_descriptor_data *idd)
{
	uint64_t kernel_offset;

	kernel_offset = gen7_fill_kernel(ibb, shdr->instr, 4 * shdr->size);
	kernel_offset += ibb->batch_offset;
	kernel_offset = xe_canonical_va(ibb->fd, kernel_offset);

	memset(idd, 0, sizeof(*idd));

	/* 64-bit canonical format setting is needed. */
	idd->dw00.kernel_start_pointer = (((uint32_t)kernel_offset) >> 6);
	idd->dw01.kernel_start_pointer_high = kernel_offset >> 32;

	/* Single program flow has no SIMD-specific branching in SIMD exec in EU threads */
	idd->dw02.single_program_flow = 1;
	idd->dw02.floating_point_mode = GEN8_FLOATING_POINT_IEEE_754;

	if (shdr->grfs_per_thread)
		idd2_set_grfs_per_thread(idd, shdr);

	/*
	* For testing purposes, use only one thread per thread group.
	* This makes it possible to identify threads by thread group id.
	*/
	idd->dw05.number_of_threads_in_gpgpu_thread_group = 1;

	if (shdr->slm_size)
		idd2_set_slm_size(idd, shdr);
}

static uint32_t
xehp_fill_surface_state(struct intel_bb *ibb,
			struct intel_buf *buf,
			uint32_t format,
			int is_dst)
{
	struct xehp_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	uint64_t address;

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	intel_bb_ptr_align(ibb, 64);
	offset = intel_bb_offset(ibb);
	ss = intel_bb_ptr(ibb);
	intel_bb_ptr_add(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	ss->ss0.surface_format = format;
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */

	ss->ss1.mocs_index = buf->mocs_index;

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y || buf->tiling == I915_TILING_4)
		ss->ss0.tiled_mode = 3;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					offset + 4 * 8, 0x0);

	ss->ss8.base_addr_lo = (uint32_t) address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_channel_select_r = 4;
	ss->ss7.shader_channel_select_g = 5;
	ss->ss7.shader_channel_select_b = 6;
	ss->ss7.shader_channel_select_a = 7;

	return offset;
}

void
xehp_emit_cfe_state(struct intel_bb *ibb, uint32_t threads)
{
	bool dfeud = CFE_CAN_DISABLE_FUSED_EU_DISPATCH(ibb->devid);

	intel_bb_out(ibb, XEHP_CFE_STATE | (6 - 2));

	/* scratch buffer */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

#define _LEGACY_MODE (1 << 6)
	/* number of threads & urb entries */
	intel_bb_out(ibb, (max_t(threads, threads, 128) - 1) << 16 | (dfeud ? _LEGACY_MODE : 0));

	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static uint32_t calculate_state_compute_mode_exceptions(struct gpgpu_shader *shdr)
{
	static const uint32_t STATE_COMPUTE_MODE_ENABLE_FE_FEH = BIT(15);
	static const uint32_t STATE_COMPUTE_MODE_ENABLE_BREAKPOINTS = BIT(14);
	static const uint32_t STATE_COMPUTE_MODE_ENABLE_MEMORY_EXCEPTION = BIT(13);
	static const uint32_t STATE_COMPUTE_MODE_ENABLE_PAGE_FAULT_EXCEPTION = BIT(9);
	static const uint32_t STATE_COMPUTE_MODE_ENABLE_OOB = BIT(7);
	uint32_t exceptions = 0xffff0000;

	if (shdr->exception_config & SHADER_EXCEPTION_BREAKPOINT)
		exceptions |= STATE_COMPUTE_MODE_ENABLE_BREAKPOINTS;
	if (shdr->exception_config & SHADER_EXCEPTION_FE_FEH)
		exceptions |= STATE_COMPUTE_MODE_ENABLE_FE_FEH;
	if (shdr->exception_config & SHADER_EXCEPTION_PAGEFAULT)
		exceptions |= (STATE_COMPUTE_MODE_ENABLE_MEMORY_EXCEPTION |
			       STATE_COMPUTE_MODE_ENABLE_PAGE_FAULT_EXCEPTION);
	if (shdr->exception_config & SHADER_MEMORY_EXCEPTION)
		exceptions |= STATE_COMPUTE_MODE_ENABLE_MEMORY_EXCEPTION;
	if (shdr->exception_config & SHADER_EXCEPTION_OOB)
		exceptions |= STATE_COMPUTE_MODE_ENABLE_OOB;

	return exceptions;
}

void xehp_emit_state_compute_mode(struct intel_bb *ibb, struct gpgpu_shader *shdr)
{
	static const uint32_t STATE_COMPUTE_MODE_DW1_ENABLE_VRT = REG_BIT(10);
	uint32_t dword_length = shdr->gfx_ver >= 2000;
	uint32_t dw1 = 0xffff0000;

	intel_bb_out(ibb, XEHP_STATE_COMPUTE_MODE | dword_length);
	if (shdr->grfs_per_thread) {
		if (shdr->gfx_ver >= 3000)
			dw1 |= STATE_COMPUTE_MODE_DW1_ENABLE_VRT;
		else
			igt_assert_lte(shdr->grfs_per_thread, 128);
	}
	intel_bb_out(ibb, dw1);

	if (dword_length)
		intel_bb_out(ibb, calculate_state_compute_mode_exceptions(shdr));
}

void
xehp_emit_state_binding_table_pool_alloc(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_3DSTATE_BINDING_TABLE_POOL_ALLOC | 2);
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0, 0, 0x0);
	intel_bb_out(ibb, 1 << 12);
}

void
xehp_emit_state_base_address(struct intel_bb *ibb)
{
	uint32_t tmp;

	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | 0x14);            //dw0

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);                   //dw1-dw2
	intel_bb_out(ibb, 0);

	/* stateless data port */
	tmp = intel_graphics_ver(ibb->devid) >= IP_VER(20, 0) ? 0 : BASE_ADDRESS_MODIFY;
	intel_bb_out(ibb, 0 | tmp);                  //dw3

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_SAMPLER, //dw4-dw5
			    0, BASE_ADDRESS_MODIFY, 0x0);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,                          //dw6-dw7
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			    0, BASE_ADDRESS_MODIFY, 0x0);

	/* indirect */
	intel_bb_out(ibb, 0);                                       //dw8-dw9
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION,            //dw10-dw11
			    0, BASE_ADDRESS_MODIFY, 0x0);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);                          //dw12
	/* dynamic state buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);           //dw13
	/* indirect object buffer size */
	if (intel_graphics_ver(ibb->devid) >= IP_VER(20, 0))	    //dw14
		intel_bb_out(ibb, 0);
	else
		intel_bb_out(ibb, 0xfffff000 | 1);
	/* instruction buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);           //dw15

	/* Bindless surface state base address */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);                 //dw16
	intel_bb_out(ibb, 0);                                       //dw17
	intel_bb_out(ibb, 0xfffff000);                              //dw18

	/* Bindless sampler state base address */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);                 //dw19
	intel_bb_out(ibb, 0);                                       //dw20
	intel_bb_out(ibb, 0);                                       //dw21
}

void
xe3p_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_STATE_BASE_ADDRESS | 0x14);            //dw0

	/* general state */
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);                   //dw1-dw2
	intel_bb_out(ibb, 0);

	/*
	 * For full 64b Mode, set BASEADDR_DIS.
	 * In Full 64b Mode, all heaps are managed by SW.
	 * STATE_BASE_ADDRESS base addresses are ignored by HW
	 * stateless data port moc not set, so EU threads have to access
	 * only uncached without moc when load/store
	 */
	intel_bb_out(ibb, BASEADDR_DIS);                                   //dw3

	/* surface state */
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);                   //dw4-dw5
	intel_bb_out(ibb, 0);

	/* dynamic state */
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);                   //dw6-dw7
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, 0);                                         //dw8-dw9
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION,              //dw10-dw11
			    0, BASE_ADDRESS_MODIFY, 0x0);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);                            //dw12

	/* dynamic state buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);             //dw13

	intel_bb_out(ibb, 0);                          	              //dw14

	/* intruction buffer size */
	intel_bb_out(ibb, ALIGN(ibb->size, 1 << 12) | 1);             //dw15

	/* Bindless surface state base address */
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);                   //dw16-17
	intel_bb_out(ibb, 0);

	/* Bindless surface state size */
	/* number of surface state entries in the Bindless Surface State buffer */
	intel_bb_out(ibb, 0xfffff000);                                //dw18

	/* Bindless sampler state */
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);                   //dw19-20
	intel_bb_out(ibb, 0);
	/*  Bindless sampler state size */
	intel_bb_out(ibb, 0);                                         //dw21
}

void
xehp_emit_compute_walk(struct intel_bb *ibb,
		       unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height,
		       struct xehp_interface_descriptor_data *pidd,
		       uint8_t color)
{
	uint32_t x_dim, y_dim, mask, dword_length;

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (x + width + 15) / 16;
	y_dim = y + height;

	mask = (x + width) & 15;
	if (mask == 0)
		mask = (1 << 16) - 1;
	else
		mask = (1 << mask) - 1;

	dword_length = intel_graphics_ver(ibb->devid) >= IP_VER(20, 0) ? 0x26 : 0x25;
	intel_bb_out(ibb, XEHP_COMPUTE_WALKER | dword_length);

	intel_bb_out(ibb, 0); /* debug object */		//dw1
	intel_bb_out(ibb, 0); /* indirect data length */	//dw2
	intel_bb_out(ibb, 0); /* indirect data offset */	//dw3

	/* SIMD size */
	/* SIMD16 | enable inline | Message SIMD16 */
	intel_bb_out(ibb, 1 << 30 | 1 << 25 | 1 << 17);		//dw4

	/* Execution mask */
	intel_bb_out(ibb, mask);				//dw5

	/* x/y/z max */
	intel_bb_out(ibb, (x_dim << 20) | (y_dim << 10) | 1);	//dw6

	/* x dim */
	intel_bb_out(ibb, x_dim);				//dw7

	/* y dim */
	intel_bb_out(ibb, y_dim);				//dw8

	/* z dim */
	intel_bb_out(ibb, 1);					//dw9

	/* group id x/y/z */
	intel_bb_out(ibb, x / 16);				//dw10
	intel_bb_out(ibb, y);					//dw11
	intel_bb_out(ibb, 0);					//dw12

	/* partition id / partition size */
	intel_bb_out(ibb, 0);					//dw13
	intel_bb_out(ibb, 0);					//dw14

	/* preempt x/y/z */
	intel_bb_out(ibb, 0);					//dw15
	intel_bb_out(ibb, 0);					//dw16
	intel_bb_out(ibb, 0);					//dw17

	if (intel_graphics_ver(ibb->devid) >= IP_VER(20, 0))	//Xe2:dw18
		intel_bb_out(ibb, 0);
	/* Interface descriptor data */
	for (int i = 0; i < 8; i++) {			       //dw18-25 (Xe2:dw19-26)
		intel_bb_out(ibb, ((uint32_t *) pidd)[i]);
	}

	/* Postsync data */
	intel_bb_out(ibb, 0);					//dw26
	intel_bb_out(ibb, 0);					//dw27
	intel_bb_out(ibb, 0);					//dw28
	intel_bb_out(ibb, 0);					//dw29
	intel_bb_out(ibb, 0);					//dw30

	/* Inline data */
	intel_bb_out(ibb, (uint32_t) color);			//dw31
	for (int i = 0; i < 7; i++) {			        //dw32-38
		intel_bb_out(ibb, 0x0);
	}
}

static void
__xe3p_emit_compute_walk2(struct intel_bb *ibb,
			  unsigned int x, unsigned int y,
			  unsigned int width, unsigned int height,
			  struct xe3p_interface_descriptor_data *pidd,
			  uint32_t max_threads,
			  struct xe3p_cw2_interrupt_data *intdata,
			  struct xe3p_cw2_gpgpu_fill_data *filldata)
{
	/*
	 * Max Threads represent range: [1, 2^16-1],
	 * Max Threads limit range: [64, number of subslices * number of EUs per SubSlice * number of threads per EU]
	 */
	const uint32_t MAX_THREADS = (1 << 16) - 1;
	uint32_t x_dim, y_dim, mask, max;

	if (filldata) {
		if (width + x > filldata->buf_width)
			width = filldata->buf_width - x;

		if (height + y > filldata->buf_height)
			height = filldata->buf_height - y;
	}

	/*
	 * Simply do SIMD16 based dispatch, so every thread uses
	 * SIMD16 channels.
	 *
	 * Define our own thread group size, e.g 16x1 for every group, then
	 * will have 1 thread each group in SIMD16 dispatch. So thread
	 * width/height/depth are all 1.
	 *
	 * Then thread group X = width / 16 (aligned to 16)
	 * thread group Y = height;
	 */
	x_dim = (x + width + 15) / 16;
	y_dim = height + y * (filldata ? 0 : 1);

	mask = (x + width) & 15;
	if (mask == 0)
		mask = (1 << 16) - 1;
	else
		mask = (1 << mask) - 1;

	intel_bb_out(ibb, XE3P_COMPUTE_WALKER2 | 0x3e);			//dw0, 0x32 => dw length: 62

	intel_bb_out(ibb, 0); /* debug object id */			//dw0
	intel_bb_out(ibb, 0);						//dw1

	/* Maximum Number of Threads */
	max = min_t(max_threads, max_t(max_threads, max_threads, 64), MAX_THREADS);
	intel_bb_out(ibb, max << 16);					//dw2

	/* SIMD size, size: SIMT16 | enable inline Parameter | Message SIMT16 */
	intel_bb_out(ibb, 1 << 30 | 1 << 25 | 1 << 17);			//dw3

	/* Execution mask: masking the use of some SIMD lanes by the last thread in a thread group */
	intel_bb_out(ibb, mask);					//dw4

	/*
	 * LWS =(Local_X_Max+1)*(Local_Y_Max+1)*(Local_Z_Max+1).
	 */
	intel_bb_out(ibb, (x_dim << 20) | (y_dim << 10) | 1);		//dw5

	/* Thread Group ID X Dimension */
	intel_bb_out(ibb, x_dim);					//dw6

	/* Thread Group ID Y Dimension */
	intel_bb_out(ibb, y_dim);					//dw7

	/* Thread Group ID Z Dimension */
	intel_bb_out(ibb, 1);						//dw8

	/* Thread Group ID Starting X, Y, Z */
	if (filldata) {
		intel_bb_out(ibb, 0);					//dw9
		intel_bb_out(ibb, 0);					//dw10
		intel_bb_out(ibb, 0);					//dw11
	} else {
		intel_bb_out(ibb, x / 16);				//dw9
		intel_bb_out(ibb, y);					//dw10
		intel_bb_out(ibb, 0);					//dw11
	}

	/* partition type / id / size */
	intel_bb_out(ibb, 0);						//dw12-13
	intel_bb_out(ibb, 0);

	/* Preempt X / Y / Z */
	intel_bb_out(ibb, 0);						//dw14
	intel_bb_out(ibb, 0);						//dw15
	intel_bb_out(ibb, 0);						//dw16

	/* APQID, PostSync ID, Over dispatch TG count, Walker ID for preemption restore */
	intel_bb_out(ibb, 0);						//dw17

	/* Interface descriptor data */
	for (int i = 0; i < 8; i++) {					//dw18-25
		intel_bb_out(ibb, ((uint32_t *) pidd)[i]);
	}

	/* Post Sync command payload 0 */
	if (intdata != NULL) {
		intel_bb_out(ibb, intdata->post_sync_op);
		intel_bb_out(ibb, intdata->post_sync_addr);
		intel_bb_out(ibb, intdata->post_sync_addr >> 32);
		intel_bb_out(ibb, intdata->post_sync_val);
		intel_bb_out(ibb, intdata->post_sync_val >> 32);
	} else {
		for (int i = 0; i < 5; i++) {					//dw26-30
			intel_bb_out(ibb, 0);
		}
	}

	if (filldata) {
		/* Inline data */
		intel_bb_out(ibb, filldata->color);			//dw31
		intel_bb_out(ibb, filldata->buf_width);			//dw32
		intel_bb_out(ibb, filldata->buf_height);		//dw33
		intel_bb_out(ibb, width);				//dw34
		intel_bb_out(ibb, height);				//dw35
		intel_bb_out(ibb, x);					//dw36
		intel_bb_out(ibb, y);					//dw37
		intel_bb_out(ibb, filldata->buf_addr);			//dw38
		intel_bb_out(ibb, filldata->buf_addr >> 32);		//dw39
		for (int i = 0; i < 7; i++)				//dw40-46
			intel_bb_out(ibb, 0x0);

	} else {
		/* Inline data */
		/* DW31 and DW32 of Inline data will be copied into R0.14 and R0.15. */
		/* The rest of DW33 through DW46 will be copied to the following GRFs. */
		intel_bb_out(ibb, x_dim);				//dw31
		for (int i = 0; i < 15; i++)				//dw32-46
			intel_bb_out(ibb, 0);
	}

	/* Post Sync command payload 1 */
	for (int i = 0; i < 5; i++) {					//dw47-51
		intel_bb_out(ibb, 0);
	}

	/* Post Sync command payload 2 */
	for (int i = 0; i < 5; i++) {					//dw52-56
		intel_bb_out(ibb, 0);
	}

	/* Post Sync command payload 3 */
	for (int i = 0; i < 5; i++) {					//dw57-61
		intel_bb_out(ibb, 0);
	}

	/* Preempt CS Interrupt Vector: Saved by HW on a TG preemption */
	intel_bb_out(ibb, 0);						//dw62
}

void
xe3p_emit_compute_walk2(struct intel_bb *ibb,
			unsigned int x, unsigned int y,
			unsigned int width, unsigned int height,
			struct xe3p_interface_descriptor_data *pidd,
			uint32_t max_threads,
			struct xe3p_cw2_interrupt_data *intdata)
{
	__xe3p_emit_compute_walk2(ibb, x, y, width, height,
				  pidd, max_threads, intdata, NULL);
}

void
xe3p_emit_fill_compute_walk2(struct intel_bb *ibb,
			     struct compute_walker2_inline_data *idata,
			     struct xe3p_interface_descriptor_data *pidd)
{
	struct xe3p_cw2_gpgpu_fill_data filldata = {
		.buf_width = idata->fill.width,
		.buf_height = idata->fill.height,
		.buf_addr = idata->fill.addr,
		.color = idata->fill.color,
	};

	__xe3p_emit_compute_walk2(ibb,
				  idata->fill.rect_x, idata->fill.rect_y,
				  idata->fill.rect_width, idata->fill.rect_height,
				  pidd, 64, NULL, &filldata);
}
