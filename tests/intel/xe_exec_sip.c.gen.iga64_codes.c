// SPDX-License-Identifier: MIT
// Generated using Intel Graphics Assembler 1.1.0-int

#include "gpgpu_shader.h"

#define MD5_SUM_IGA64_ASMS 6a7e389099486480abcfab7fbf536427

struct iga64_template const iga64_code_xe_exec_sip_illegal[] = {
	{ .gfx_ver = 1250, .size = 8, .code = (const uint32_t []) {
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x80000901, 0x00010000, 0x00000000, 0x00000000,
	}},
	{ .gfx_ver = 0, .size = 8, .code = (const uint32_t []) {
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x80000101, 0x00010000, 0x00000000, 0x00000000,
	}}
};

struct iga64_template const iga64_code_oob_exception[] = {
	{ .gfx_ver = 1250, .size = 12, .code = (const uint32_t []) {
		0x80000061, 0x10014110, 0x00000000, 0xfffefffe,
		0x80000961, 0x00014118, 0x00000000, 0x00000000,
		0x80000901, 0x00010000, 0x00000000, 0x00000000,
	}},
	{ .gfx_ver = 0, .size = 12, .code = (const uint32_t []) {
		0x80000061, 0x10014110, 0x00000000, 0xfffefffe,
		0x80000161, 0x00014118, 0x00000000, 0x00000000,
		0x80000101, 0x00010000, 0x00000000, 0x00000000,
	}}
};
