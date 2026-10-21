/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#ifndef __KMS_COLOROP_HELPER_H__
#define __KMS_COLOROP_HELPER_H__

#include "igt.h"
#include "igt_color.h"
#include "igt_color_lut.h"

typedef bool (*compare_fb_t)(igt_fb_t *in, igt_fb_t *out);

typedef int (*transform_fb)(igt_fb_t *in);

typedef int (*transform_pixel)(igt_pixel_t *pixel);

/* Test version definitions */
typedef enum kms_colorop_type {
	KMS_COLOROP_ENUMERATED_LUT1D,
	KMS_COLOROP_CUSTOM_LUT1D,
	KMS_COLOROP_CTM_3X4,
	KMS_COLOROP_MULTIPLIER,
	KMS_COLOROP_LUT3D,
	KMS_COLOROP_FIXED_MATRIX,
} kms_colorop_type_t;

typedef enum kms_colorop_lut1d_tf {
	KMS_COLOROP_LUT1D_SRGB_EOTF,
	KMS_COLOROP_LUT1D_SRGB_INV_EOTF,
	KMS_COLOROP_LUT1D_BT2020_INV_OETF,
	KMS_COLOROP_LUT1D_BT2020_OETF,
	KMS_COLOROP_LUT1D_PQ_EOTF,
	KMS_COLOROP_LUT1D_PQ_INV_EOTF,
	KMS_COLOROP_LUT1D_PQ_125_EOTF,
	KMS_COLOROP_LUT1D_PQ_125_INV_EOTF,
	KMS_COLOROP_LUT1D_GAMMA_2_2_OETF,
	KMS_COLOROP_LUT1D_GAMMA_2_2_INV_OETF,
	KMS_COLOROP_LUT1D_NUM_ENUMS
} kms_colorop_lut1d_tf_t;

extern const char * const kms_colorop_lut1d_tf_names[KMS_COLOROP_LUT1D_NUM_ENUMS];

typedef struct kms_colorop_enumerated_lut1d_info {
	kms_colorop_lut1d_tf_t tf;
} kms_colorop_enumerated_lut1d_info_t;

typedef struct kms_colorop_lut3d_info {
	uint32_t size;
	enum drm_colorop_lut3d_interpolation_type interpolation;
} kms_colorop_lut3d_info_t;

typedef enum kms_colorop_fixed_matrix {
	KMS_COLOROP_FIXED_MATRIX_YCBCR601_FULL_RGB,
	KMS_COLOROP_FIXED_MATRIX_YCBCR601_LIMITED_RGB,
	KMS_COLOROP_FIXED_MATRIX_YCBCR709_FULL_RGB,
	KMS_COLOROP_FIXED_MATRIX_YCBCR709_LIMITED_RGB,
	KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_FULL_RGB,
	KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_LIMITED_RGB,
	KMS_COLOROP_FIXED_MATRIX_NUM_ENUMS
} kms_colorop_fixed_matrix_t;

extern const char * const kms_colorop_fixed_matrix_names[KMS_COLOROP_FIXED_MATRIX_NUM_ENUMS];

typedef struct kms_colorop_fixed_matrix_info {
	kms_colorop_fixed_matrix_t fixed_matrix_type;
	enum igt_color_encoding encoding;
	enum igt_color_range range;
} kms_colorop_fixed_matrix_info_t;

typedef struct kms_colorop {
	kms_colorop_type_t type;
	kms_colorop_fixed_matrix_info_t fixed_matrix_info;

	union {
		kms_colorop_enumerated_lut1d_info_t enumerated_lut1d_info;
		igt_1dlut_t *lut1d;
		const igt_3dlut_t *lut3d;
		const igt_matrix_3x4_t *matrix_3x4;
		double multiplier;
	};

	kms_colorop_lut3d_info_t lut3d_info;

	const char *name;

	igt_pixel_transform transform;

	/* Mapped colorop */
	igt_colorop_t *colorop;

} kms_colorop_t;

extern kms_colorop_t kms_colorop_srgb_eotf;
extern kms_colorop_t kms_colorop_srgb_eotf_2;
extern kms_colorop_t kms_colorop_srgb_inv_eotf;
extern kms_colorop_t kms_colorop_srgb_inv_eotf_lut;
extern kms_colorop_t kms_colorop_srgb_eotf_lut;
extern kms_colorop_t kms_colorop_bt2020_inv_oetf;
extern kms_colorop_t kms_colorop_bt2020_oetf;
extern kms_colorop_t kms_colorop_pq_eotf;
extern kms_colorop_t kms_colorop_pq_inv_eotf;
extern kms_colorop_t kms_colorop_pq_125_eotf;
extern kms_colorop_t kms_colorop_pq_125_eotf_2;
extern kms_colorop_t kms_colorop_pq_125_inv_eotf;
extern kms_colorop_t kms_colorop_gamma_22_oetf;
extern kms_colorop_t kms_colorop_gamma_22_inv_oetf;
extern kms_colorop_t kms_colorop_ctm_3x4_50_desat;
extern kms_colorop_t kms_colorop_ctm_3x4_overdrive;
extern kms_colorop_t kms_colorop_ctm_3x4_oversaturate;
extern kms_colorop_t kms_colorop_ctm_3x4_bt709_enc;
extern kms_colorop_t kms_colorop_ctm_3x4_bt709_dec;
extern kms_colorop_t kms_colorop_multiply_125;
extern kms_colorop_t kms_colorop_multiply_inv_125;
extern kms_colorop_t kms_colorop_3dlut_17_12_rgb;
extern kms_colorop_t kms_colorop_bt709_limited_ycbcr_to_rgb;
extern kms_colorop_t kms_colorop_bt709_full_ycbcr_to_rgb;
extern kms_colorop_t kms_colorop_bt601_limited_ycbcr_to_rgb;
extern kms_colorop_t kms_colorop_bt2020_limited_ycbcr_to_rgb;

igt_colorop_t *get_color_pipeline(igt_display_t *display,
			          igt_plane_t *plane,
				  kms_colorop_t *colorops[]);
void set_color_pipeline(igt_display_t *display,
			igt_plane_t *plane,
			kms_colorop_t *colorops[],
			igt_colorop_t *color_pipeline);
void reset_colorops(kms_colorop_t *colorops[]);

#endif /* __KMS_COLOROP_HELPER_H__ */
