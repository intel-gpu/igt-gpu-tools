/* SPDX-License-Identifier: MIT */
/* Copyright © 2024 Intel Corporation */

/* Header used during pre-process phase of iga64 assembly.
 * WARNING: changing this file causes rebuild of all shaders.
 * Do not touch without current version of iga64 compiler.
 */

#ifndef IGA64_MACROS_H
#define IGA64_MACROS_H

/* send instruction for DG2+ requires 0 length in case src1 is null, BSpec: 47443 */
#if GFX_VER <= 1250
#define src1_null null
#else
#define src1_null null:0
#endif

/* GPGPU_R0Payload fields, Bspec: 55396, 56587 */
#  define TGID_X r0.1
#  define TGID_Y r0.6
#  define R0_TGIDX TGID_X<0;1,0>:ud
#  define R0_TGIDY TGID_Y<0;1,0>:ud
#  define R0_FFTID r0.5<0;1,0>:ud

/* Inline data from COMPUTE_WALKER*, Bspec: 47203
 * Filled by __xe*_gpgpu_execfunc.
 */
#  define TGT_ADDRESS r1.0
#  define TGT_WIDTH r1.2
#  define TGT_HEIGHT r1.3
#  define DIM_X r1.4
#  define R1_TGT_ADDRESS TGT_ADDRESS<0;1,0>:uq
#  define R1_TGT_WIDTH TGT_WIDTH<0;1,0>:ud
#  define R1_TGT_HEIGHT TGT_HEIGHT<0;1,0>:ud
#  define R1_DIM_X DIM_X<0;1,0>:ud

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

#define SET_SURFACE_DESC(dst)					\
(W)	mov (1)		dst.0<1>:uq	R1_TGT_ADDRESS		;\
(W)	add (1)		dst.2<1>:ud	R1_TGT_WIDTH	-1:d	;\
(W)	add (1)		dst.3<1>:ud	R1_TGT_HEIGHT	-1:d	;\
(W)	add (1)		dst.4<1>:ud	R1_TGT_WIDTH	-1:d

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

#define SET_SHARED_A64_ADDR(dst, y)				\
(W)	mul(1)		dst.0:ud TGT_WIDTH:ud y			;\
(W)	add(1)		dst.0:uq TGT_ADDRESS:uq dst.0:ud	;\

#define SET_THREAD_A64_ADDR(dst, x, y)				\
(W)	shl(1)		dst.0:ud TGID_X:ud 2:ud			;\
(W)	add(1)		dst.0:ud dst.0:ud x:ud			;\
(W)	add(1)		dst.1:ud TGID_Y:ud y			;\
(W)	mad(1)		dst.0:ud dst.0:ud dst.1:ud TGT_WIDTH:ud	;\
(W)	add(1)		dst.0:uq TGT_ADDRESS:uq dst.0:ud	;\

#if GFX_VER < 1260
#define SET_SHARED_SPACE_ADDR(dst, y, width) SET_SHARED_MEDIA_BLOCK_MSG_HDR(dst, y, width)
#define SET_THREAD_SPACE_ADDR(dst, x, y, width) SET_THREAD_MEDIA_BLOCK_MSG_HDR(dst, x, y, width)
#define LOAD_SPACE_DW(dst, src) send.dc1 (1)	dst	src	src1_null 0x0	0x2190000
#define STORE_SPACE_DW(dst, src) send.dc1 (1)	null	dst	src1_null 0x0	0x40A8000
#elif GFX_VER < 3500
#define SET_SHARED_SPACE_ADDR(dst, y, width) SET_SHARED_A64_ADDR(dst, y)
#define SET_THREAD_SPACE_ADDR(dst, x, y, width) SET_THREAD_A64_ADDR(dst, x, y)
#define LOAD_SPACE_DW(dst, src) send.ugm(1)	dst src null:0 0x0 0x02128580 // load.ugm.d32x1t.a64.uc.uc
#define STORE_SPACE_DW(dst, src) send.ugm(1)	null dst src:1 0x0 0x02028584 // store.ugm.d32x1t.a64.uc.uc
#else
#define SET_SHARED_SPACE_ADDR(dst, y, width) SET_SHARED_MEDIA_A2DBLOCK_PAYLOAD(dst, y, width)
#define SET_THREAD_SPACE_ADDR(dst, x, y, width) SET_THREAD_MEDIA_A2DBLOCK_PAYLOAD(dst, x, y, width)
#define LOAD_SPACE_DW(dst, src) sendg.ugm (1)	dst	src:1	null:0	0x28003
#define STORE_SPACE_DW(dst, src) sendg.ugm (1)	null	dst:1	src:1	0x28007
#endif

#endif
