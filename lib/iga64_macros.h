/* SPDX-License-Identifier: MIT */
/* Copyright © 2024 Intel Corporation */

/* Header used during pre-process phase of iga64 assembly.
 * WARNING: changing this file causes rebuild of all shaders.
 * Do not touch without current version of iga64 compiler.
 */

#ifndef IGA64_MACROS_H
#define IGA64_MACROS_H

/* send instruction for DG2+ requires 0 length in case src1 is null, BSpec: 47443 */
#if GEN_VER <= 1250
#define src1_null null
#else
#define src1_null null:0
#endif

/* GPGPU_R0Payload fields, Bspec: 55396, 56587 */
#define R0_TGIDX r0.1<0;1,0>:ud
#define R0_TGIDY r0.6<0;1,0>:ud
#define R0_FFTID r0.5<0;1,0>:ud

/* Inline data from COMPUTE_WALKER*, Bspec: 47203, 73584
 * Filled by __xe*_gpgpu_execfunc.
 */
#define R1_TGT_ADDRESS r1.0<0;1,0>:uq
#define R1_TGT_WIDTH r1.2<0;1,0>:ud
#define R1_TGT_HEIGHT r1.3<0;1,0>:ud
#define R1_DIM_X r1.4<0;1,0>:ud

#define SET_SHARED_MEDIA_BLOCK_MSG_HDR(dst, y, width)	\
(W)	mov (8)		dst.0<1>:ud	0x0:ud		;\
(W)	mov (1)		dst.1<1>:ud	y		;\
(W)	mov (1)		dst.2<1>:ud	(width - 1):ud	;\
(W)	mov (1)		dst.4<1>:ud	R0_FFTID

#define SET_THREAD_MEDIA_BLOCK_MSG_HDR(dst, x, y, width)	\
(W)	mov (8)		dst.0<1>:ud	0x0:ud			;\
(W)	shl (1)		dst.0<1>:ud	R0_TGIDX	0x2:ud	;\
(W)	add (1)		dst.0<1>:ud	dst.0<0;1,0>:ud	x:ud	;\
(W)	add (1)		dst.1<1>:ud	R0_TGIDY	y	;\
(W)	mov (1)		dst.2<1>:ud	(width - 1):ud		;\
(W)	mov (1)		dst.4<1>:ud	R0_FFTID

#if GEN_VER < 3000
#define SET_SURFACE_DESC(dst)			\
(W)	mov (8)		dst.0<1>:ud	0x0:ud
#else
#define SET_SURFACE_DESC(dst)					\
(W)	mov (1)		dst.0<1>:uq	R1_TGT_ADDRESS		;\
(W)	add (1)		dst.2<1>:ud	R1_TGT_WIDTH	-1:d	;\
(W)	add (1)		dst.3<1>:ud	R1_TGT_HEIGHT	-1:d	;\
(W)	add (1)		dst.4<1>:ud	R1_TGT_WIDTH	-1:d
#endif

#define SET_SHARED_MEDIA_A2DBLOCK_PAYLOAD(dst, y, width)	\
	SET_SURFACE_DESC(dst)					;\
(W)	mov (1)		dst.5<1>:ud	0x0:ud			;\
(W)	mov (1)		dst.6<1>:ud	y			;\
(W)	mov (1)		dst.7<1>:ud	(width - 1):ud

#define SET_THREAD_MEDIA_A2DBLOCK_PAYLOAD(dst, x, y, width)	\
	SET_SURFACE_DESC(dst)					;\
(W)	shl (1)		dst.5<1>:ud	R0_TGIDX	0x2:ud	;\
(W)	add (1)		dst.5<1>:ud	dst.5<0;1,0>:ud	x:ud	;\
(W)	add (1)		dst.6<1>:ud	R0_TGIDY	y	;\
(W)	mov (1)		dst.7<1>:ud	(width - 1):ud		;\

#if GEN_VER < 2000
#define SET_SHARED_SPACE_ADDR(dst, y, width) SET_SHARED_MEDIA_BLOCK_MSG_HDR(dst, y, width)
#define SET_THREAD_SPACE_ADDR(dst, x, y, width) SET_THREAD_MEDIA_BLOCK_MSG_HDR(dst, x, y, width)
#define LOAD_SPACE_DW(dst, src) send.dc1 (1)	dst	src	src1_null 0x0	0x2190000
#define STORE_SPACE_DW(dst, src) send.dc1 (1)	null	dst	src1_null 0x0	0x40A8000
#else
#define SET_SHARED_SPACE_ADDR(dst, y, width) SET_SHARED_MEDIA_A2DBLOCK_PAYLOAD(dst, y, width)
#define SET_THREAD_SPACE_ADDR(dst, x, y, width) SET_THREAD_MEDIA_A2DBLOCK_PAYLOAD(dst, x, y, width)
#if GEN_VER < 3000
#define LOAD_SPACE_DW(dst, src) send.tgm (1)	dst	src	null:0	0x0	0x62100003
#define STORE_SPACE_DW(dst, src) send.tgm (1)	null	dst	null:0	0x0	0x64000007
#else
#define LOAD_SPACE_DW(dst, src) send.ugm (1)	dst	src	null:0	0x0	0x2120003
#define STORE_SPACE_DW(dst, src) send.ugm (1)	null	dst	src:1	0x0	0x2020007
#endif
#endif

#endif
