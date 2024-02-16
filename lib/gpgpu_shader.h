/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef GPGPU_SHADER_H
#define GPGPU_SHADER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct intel_bb;
struct intel_buf;

enum gpgpu_shader_vrt_modes {
	VRT_32 = 0x0,
	VRT_64 = 0x1,
	VRT_96 = 0x2,
	VRT_128 = 0x3,
	VRT_160 = 0x4,
	VRT_192 = 0x5,
	VRT_256 = 0x7,
	VRT_DISABLED,
};

struct gpgpu_shader {
	uint32_t gfx_ver;
	uint32_t size;
	uint32_t max_size;
	union {
		uint32_t *code;
		uint32_t (*instr)[4];
	};
	struct igt_map *labels;
	uint32_t exceptions;
	bool illegal_opcode_exception_enable;
	uint32_t num_threads_in_tg;
	bool large_grf_mode;
	uint32_t simd_size;
	bool hw_local_id_generation;
	enum gpgpu_shader_vrt_modes vrt;
};

struct iga64_template {
	uint32_t gfx_ver;
	uint32_t size;
	const uint32_t *code;
};

#pragma GCC diagnostic ignored "-Wnested-externs"

uint32_t
__emit_iga64_code(struct gpgpu_shader *shdr, const struct iga64_template *tpls,
		  int argc, uint32_t *argv);

#define emit_iga64_code(__shdr, __name, __txt, __args...) \
({ \
	static const char t[] __attribute__ ((section(".iga64_assembly"), used)) =\
		"iga64_assembly_" #__name ":" __txt "\n"; \
	extern struct iga64_template const iga64_code_ ## __name[]; \
	u32 args[] = { __args }; \
	__emit_iga64_code(__shdr, iga64_code_ ## __name, ARRAY_SIZE(args), args); \
})

struct gpgpu_shader *gpgpu_shader_create(int fd);
void gpgpu_shader_destroy(struct gpgpu_shader *shdr);

void gpgpu_shader_dump(struct gpgpu_shader *shdr);

void gpgpu_shader_exec(struct intel_bb *ibb,
		       struct intel_buf *target,
		       unsigned int x_dim, unsigned int y_dim,
		       struct gpgpu_shader *shdr,
		       struct gpgpu_shader *sip,
		       uint64_t ring, bool explicit_engine);

static inline uint32_t gpgpu_shader_last_instr(struct gpgpu_shader *shdr)
{
	return shdr->size / 4 - 1;
}

void gpgpu_shader_set_vrt(struct gpgpu_shader *shdr, enum gpgpu_shader_vrt_modes vrt);

uint32_t gpgpu_shader__get_max_threads_in_tg(struct gpgpu_shader *shdr);

void fill_inline_data(uint32_t *inline_data, uint64_t target_offset,
		      struct intel_buf *target, uint32_t x_dim);
void gpgpu_shader__wait(struct gpgpu_shader *shdr);
void gpgpu_shader__breakpoint_on(struct gpgpu_shader *shdr, uint32_t cmd_no);
void gpgpu_shader__breakpoint(struct gpgpu_shader *shdr);
void gpgpu_shader__nop(struct gpgpu_shader *shdr);
void gpgpu_shader__eot(struct gpgpu_shader *shdr);
void gpgpu_shader__common_target_write(struct gpgpu_shader *shdr,
				       uint32_t y_offset, const uint32_t value[4]);
void gpgpu_shader__common_target_write_u32(struct gpgpu_shader *shdr,
				     uint32_t y_offset, uint32_t value);
void gpgpu_shader__clear_exception(struct gpgpu_shader *shdr, uint32_t value);
void gpgpu_shader__set_exception(struct gpgpu_shader *shdr, uint32_t value);
void gpgpu_shader__end_system_routine(struct gpgpu_shader *shdr,
				      bool breakpoint_suppress);
void gpgpu_shader__end_system_routine_step_if_eq(struct gpgpu_shader *shdr,
						 uint32_t dw_offset,
						 uint32_t value);
void gpgpu_shader__trigger_oob_exception(struct gpgpu_shader *shdr);
void gpgpu_shader__write_aip(struct gpgpu_shader *shdr, uint32_t y_offset);
void gpgpu_shader__increase_aip(struct gpgpu_shader *shdr, uint32_t value);
void gpgpu_shader__write_dword(struct gpgpu_shader *shdr, uint32_t value,
			       uint32_t y_offset);
void gpgpu_shader__write_on_exception(struct gpgpu_shader *shdr, uint32_t dw, uint32_t x_offset,
				      uint32_t y_offset, uint32_t mask, uint32_t value);
void gpgpu_shader__write_a64_d32(struct gpgpu_shader *shdr, uint64_t ppgtt_addr,
				 uint32_t value);
void gpgpu_shader__read_a64_d32(struct gpgpu_shader *shdr, uint64_t ppgtt_addr);
void gpgpu_shader__label(struct gpgpu_shader *shdr, int label_id);
void gpgpu_shader__jump(struct gpgpu_shader *shdr, int label_id);
void gpgpu_shader__jump_neq(struct gpgpu_shader *shdr, int label_id,
			    uint32_t dw_offset, uint32_t value);
void gpgpu_shader__loop_begin(struct gpgpu_shader *shdr, int label_id);
void gpgpu_shader__loop_end(struct gpgpu_shader *shdr, int label_id, uint32_t iter);
#endif /* GPGPU_SHADER_H */
