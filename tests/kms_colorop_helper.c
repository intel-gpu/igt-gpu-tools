// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "kms_colorop_helper.h"
#include "igt_color_encoding.h"

const char * const kms_colorop_lut1d_tf_names[KMS_COLOROP_LUT1D_NUM_ENUMS] = {
	[KMS_COLOROP_LUT1D_SRGB_EOTF] = "sRGB EOTF",
	[KMS_COLOROP_LUT1D_SRGB_INV_EOTF] = "sRGB Inverse EOTF",
	[KMS_COLOROP_LUT1D_BT2020_INV_OETF] = "BT.2020 Inverse OETF",
	[KMS_COLOROP_LUT1D_BT2020_OETF] = "BT.2020 OETF",
	[KMS_COLOROP_LUT1D_PQ_EOTF] = "PQ EOTF",
	[KMS_COLOROP_LUT1D_PQ_INV_EOTF] = "PQ Inverse EOTF",
	[KMS_COLOROP_LUT1D_PQ_125_EOTF] = "PQ 125 EOTF",
	[KMS_COLOROP_LUT1D_PQ_125_INV_EOTF] = "PQ 125 Inverse EOTF",
	[KMS_COLOROP_LUT1D_GAMMA_2_2_OETF] = "Gamma 2.2",
	[KMS_COLOROP_LUT1D_GAMMA_2_2_INV_OETF] = "Gamma 2.2 Inverse",
};

const char * const kms_colorop_fixed_matrix_names[KMS_COLOROP_FIXED_MATRIX_NUM_ENUMS] = {
	[KMS_COLOROP_FIXED_MATRIX_YCBCR601_FULL_RGB] = "YCbCr 601 Full to RGB",
	[KMS_COLOROP_FIXED_MATRIX_YCBCR601_LIMITED_RGB] = "YCbCr 601 Limited to RGB",
	[KMS_COLOROP_FIXED_MATRIX_YCBCR709_FULL_RGB] = "YCbCr 709 Full to RGB",
	[KMS_COLOROP_FIXED_MATRIX_YCBCR709_LIMITED_RGB] = "YCbCr 709 Limited to RGB",
	[KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_FULL_RGB] = "YCbCr 2020 NC Full to RGB",
	[KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_LIMITED_RGB] = "YCbCr 2020 NC Limited to RGB",
};

kms_colorop_t kms_colorop_srgb_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_SRGB_EOTF
	},
	.name = "srgb_eotf",
	.transform = &igt_color_srgb_eotf
};

kms_colorop_t kms_colorop_srgb_eotf_2 = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_SRGB_EOTF
	},
	.name = "srgb_eotf",
	.transform = &igt_color_srgb_eotf
};

kms_colorop_t kms_colorop_srgb_inv_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_SRGB_INV_EOTF
	},
	.name = "srgb_inv_eotf",
	.transform = &igt_color_srgb_inv_eotf
};

kms_colorop_t kms_colorop_srgb_inv_eotf_lut = {
	.type = KMS_COLOROP_CUSTOM_LUT1D,
	.lut1d = &igt_1dlut_srgb_inv_eotf,
	.name = "srgb_inv_eotf_lut",
	.transform = &igt_color_srgb_inv_eotf
};

kms_colorop_t kms_colorop_srgb_eotf_lut = {
	.type = KMS_COLOROP_CUSTOM_LUT1D,
	.lut1d = &igt_1dlut_srgb_eotf,
	.name = "srgb_eotf_lut",
	.transform = &igt_color_srgb_eotf
};

kms_colorop_t kms_colorop_bt2020_inv_oetf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_BT2020_INV_OETF
	},
	.name = "bt2020_inv_oetf",
	.transform = &igt_color_bt2020_inv_oetf
};

kms_colorop_t kms_colorop_bt2020_oetf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_BT2020_OETF
	},
	.name = "bt2020_oetf",
	.transform = &igt_color_bt2020_oetf
};

kms_colorop_t kms_colorop_pq_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_PQ_EOTF
	},
	.name = "pq_eotf",
	.transform = &igt_color_pq_eotf
};

kms_colorop_t kms_colorop_pq_inv_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_PQ_INV_EOTF
	},
	.name = "pq_inv_eotf",
	.transform = &igt_color_pq_inv_eotf
};

kms_colorop_t kms_colorop_pq_125_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_PQ_125_EOTF
	},
	.name = "pq_125_eotf",
	.transform = &igt_color_pq_125_eotf
};

kms_colorop_t kms_colorop_pq_125_eotf_2 = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_PQ_125_EOTF
	},
	.name = "pq_125_eotf",
	.transform = &igt_color_pq_125_eotf
};

kms_colorop_t kms_colorop_pq_125_inv_eotf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_PQ_125_INV_EOTF
	},
	.name = "pq_125_inv_eotf",
	.transform = &igt_color_pq_125_inv_eotf
};

kms_colorop_t kms_colorop_gamma_22_oetf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_GAMMA_2_2_OETF
	},
	.name = "gamma_2_2",
	.transform = &igt_color_gamma_2_2
};

kms_colorop_t kms_colorop_gamma_22_inv_oetf = {
	.type = KMS_COLOROP_ENUMERATED_LUT1D,
	.enumerated_lut1d_info = {
		.tf = KMS_COLOROP_LUT1D_GAMMA_2_2_INV_OETF
	},
	.name = "gamma_2_2_inv",
	.transform = &igt_color_gamma_2_2_inv
};

kms_colorop_t kms_colorop_ctm_3x4_50_desat = {
	.type = KMS_COLOROP_CTM_3X4,
	.matrix_3x4 = &igt_matrix_3x4_50_desat,
	.name = "ctm_3x4_50_desat",
	.transform = &igt_color_ctm_3x4_50_desat
};

kms_colorop_t kms_colorop_ctm_3x4_overdrive = {
	.type = KMS_COLOROP_CTM_3X4,
	.matrix_3x4 = &igt_matrix_3x4_overdrive,
	.name = "ctm_3x4_overdrive",
	.transform = &igt_color_ctm_3x4_overdrive
};

kms_colorop_t kms_colorop_ctm_3x4_oversaturate = {
	.type = KMS_COLOROP_CTM_3X4,
	.matrix_3x4 = &igt_matrix_3x4_oversaturate,
	.name = "ctm_3x4_oversaturate",
	.transform = &igt_color_ctm_3x4_oversaturate
};

kms_colorop_t kms_colorop_ctm_3x4_bt709_enc = {
	.type = KMS_COLOROP_CTM_3X4,
	.matrix_3x4 = &igt_matrix_3x4_bt709_enc,
	.name = "ctm_3x4_bt709_enc",
	.transform = &igt_color_ctm_3x4_bt709_enc
};

kms_colorop_t kms_colorop_ctm_3x4_bt709_dec = {
	.type = KMS_COLOROP_CTM_3X4,
	.matrix_3x4 = &igt_matrix_3x4_bt709_dec,
	.name = "ctm_3x4_bt709_dec",
	.transform = &igt_color_ctm_3x4_bt709_dec
};

kms_colorop_t kms_colorop_multiply_125 = {
	.type =	KMS_COLOROP_MULTIPLIER,
	.multiplier = 125.0f,
	.name = "multiply_125",
	.transform = &igt_color_multiply_125
};

kms_colorop_t kms_colorop_multiply_inv_125 = {
	.type =	KMS_COLOROP_MULTIPLIER,
	.multiplier = 1/125.0f,
	.name = "multiply_inv_125",
	.transform = &igt_color_multiply_inv_125
};

kms_colorop_t kms_colorop_3dlut_17_12_rgb = {
	.type =	KMS_COLOROP_LUT3D,
	.lut3d = &igt_3dlut_17_rgb,
	.lut3d_info = {
		.size = 17,
		.interpolation = DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL,
	},
	.name = "3dlut with traversal order RGB",
	.transform = &igt_color_3dlut_17_12_rgb,
};

kms_colorop_t kms_colorop_bt709_limited_ycbcr_to_rgb = {
	.type = KMS_COLOROP_FIXED_MATRIX,
	.fixed_matrix_info = {
		.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR709_LIMITED_RGB,
		.encoding = IGT_COLOR_YCBCR_BT709,
		.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
	},
	.name = "YCbCr BT.709 Limited Range to RGB",
	.transform = NULL,
};

kms_colorop_t kms_colorop_bt709_full_ycbcr_to_rgb = {
	.type = KMS_COLOROP_FIXED_MATRIX,
	.fixed_matrix_info = {
		.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR709_FULL_RGB,
		.encoding = IGT_COLOR_YCBCR_BT709,
		.range = IGT_COLOR_YCBCR_FULL_RANGE,
	},
	.name = "YCbCr BT.709 Full Range to RGB",
	.transform = NULL,
};

kms_colorop_t kms_colorop_bt601_limited_ycbcr_to_rgb = {
	.type = KMS_COLOROP_FIXED_MATRIX,
	.fixed_matrix_info = {
		.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR601_LIMITED_RGB,
		.encoding = IGT_COLOR_YCBCR_BT601,
		.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
	},
	.name = "YCbCr BT.601 Limited Range to RGB",
	.transform = NULL,
};

kms_colorop_t kms_colorop_bt2020_limited_ycbcr_to_rgb = {
	.type = KMS_COLOROP_FIXED_MATRIX,
	.fixed_matrix_info = {
		.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_LIMITED_RGB,
		.encoding = IGT_COLOR_YCBCR_BT2020,
		.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
	},
	.name = "YCbCr BT.2020 Limited Range to RGB",
	.transform = NULL,
};

static bool can_use_colorop(igt_display_t *display, igt_colorop_t *colorop, kms_colorop_t *desired)
{
	switch (desired->type) {
	case KMS_COLOROP_ENUMERATED_LUT1D:
		if (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_1D_CURVE &&
		    igt_colorop_has_prop_enum_value(colorop,
						    IGT_COLOROP_CURVE_1D_TYPE,
						    kms_colorop_lut1d_tf_names[desired->enumerated_lut1d_info.tf]))
			return true;
		return false;
	case KMS_COLOROP_CTM_3X4:
		return (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_CTM_3X4);
	case KMS_COLOROP_CUSTOM_LUT1D:
		if (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_1D_LUT)
			return true;
		return false;
	case KMS_COLOROP_MULTIPLIER:
		return (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_MULTIPLIER);
	case KMS_COLOROP_LUT3D:
		return (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_3D_LUT);
	case KMS_COLOROP_FIXED_MATRIX:
		if (igt_colorop_get_prop(display, colorop, IGT_COLOROP_TYPE) == DRM_COLOROP_FIXED_MATRIX &&
		    igt_colorop_try_prop_enum(colorop, IGT_COLOROP_FIXED_MATRIX_TYPE,
					      kms_colorop_fixed_matrix_names[desired->fixed_matrix_info.fixed_matrix_type]))
			return true;
		return false;
	default:
		return false;
	}
}

/**
 * Iterate color pipeline that begins with colorop and try to map
 * colorops[] to it.
 */
static bool map_to_pipeline(igt_display_t *display,
			    igt_colorop_t *colorop,
			    kms_colorop_t *colorops[])
{
	igt_colorop_t *next = colorop;
	kms_colorop_t *current_op;
	int i = 0;
	int prop_val = 0;

	current_op = colorops[i];
	i++;
	igt_require(current_op);

	while (next) {
		if (can_use_colorop(display, next, current_op)) {
			current_op->colorop = next;
			current_op = colorops[i];
			i++;
			if (!current_op)
				break;
		}
		prop_val = igt_colorop_get_prop(display, next,
						IGT_COLOROP_NEXT);
		next = igt_find_colorop(display, prop_val);
	}

	if (current_op) {
		/* we failed to map the pipeline */

		/* clean up colorops[i].colorop mappings */
		for (i = 0, current_op = colorops[0]; current_op; current_op = colorops[i++])
			current_op->colorop = NULL;

		return false;
	}

	return true;
}

igt_colorop_t *get_color_pipeline(igt_display_t *display,
			          igt_plane_t *plane,
				  kms_colorop_t *colorops[])
{
	igt_colorop_t *colorop = NULL;
	int i;

	/* go through all color pipelines */
	for (i = 0; i < plane->num_color_pipelines; ++i) {
		if (map_to_pipeline(display, plane->color_pipelines[i], colorops)) {
			colorop = plane->color_pipelines[i];
			break;
		}
	}

	return colorop;
}

static void fill_custom_1dlut(igt_display_t *display, kms_colorop_t *colorop)
{
	uint64_t lut_size = igt_colorop_get_prop(display, colorop->colorop, IGT_COLOROP_SIZE);
	igt_pixel_t pixel;
	float index;
	int i;

	for (i = 0; i < lut_size; i++) {
		index = i / (float) lut_size;

		pixel.r = index;
		pixel.g = index;
		pixel.b = index;

		colorop->transform(&pixel);

		colorop->lut1d->lut[i].red = (double)pixel.r * UINT_MAX;
		colorop->lut1d->lut[i].green = (double)pixel.g * UINT_MAX;
		colorop->lut1d->lut[i].blue = (double)pixel.b * UINT_MAX;
	}
}

static void configure_3dlut(igt_display_t *display, kms_colorop_t *colorop, uint64_t size)
{
	uint64_t lut_size = 0;
	uint64_t i;
	igt_3dlut_norm_t *igt_3dlut;

	/* Convert 3DLUT floating points to u16 required by colorop API */
	lut_size = size * size * size;
	igt_3dlut = (igt_3dlut_norm_t *) malloc(sizeof(struct drm_color_lut32) * lut_size);
	for (i = 0; i < lut_size; i++) {
		const struct igt_color_lut_float *lut_f = &colorop->lut3d->lut[i];

		igt_3dlut->lut[i].red = round((double)lut_f->red * UINT_MAX);
		igt_3dlut->lut[i].green = round((double)lut_f->green * UINT_MAX);
		igt_3dlut->lut[i].blue = round((double)lut_f->blue * UINT_MAX);
	}

	igt_colorop_set_3dlut(display, colorop->colorop, igt_3dlut, lut_size * sizeof(struct drm_color_lut32));
	free(igt_3dlut);
}

static void set_colorop(igt_display_t *display, kms_colorop_t *colorop)
{
	enum drm_colorop_lut3d_interpolation_type interpolation;
	uint64_t lut_size = 0;
	uint64_t mult = 1;

	igt_assert(colorop->colorop);
	igt_colorop_set_prop_value(colorop->colorop, IGT_COLOROP_BYPASS, 0);

	switch (colorop->type) {
	case KMS_COLOROP_ENUMERATED_LUT1D:
		igt_colorop_set_prop_enum(colorop->colorop, IGT_COLOROP_CURVE_1D_TYPE, kms_colorop_lut1d_tf_names[colorop->enumerated_lut1d_info.tf]);
		break;
	case KMS_COLOROP_CTM_3X4:
		igt_colorop_set_ctm_3x4(display, colorop->colorop, colorop->matrix_3x4);
		break;
	case KMS_COLOROP_CUSTOM_LUT1D:
		fill_custom_1dlut(display, colorop);
		lut_size = igt_colorop_get_prop(display, colorop->colorop, IGT_COLOROP_SIZE);
		igt_colorop_set_custom_1dlut(display, colorop->colorop, colorop->lut1d, lut_size * sizeof(struct drm_color_lut32));
		break;
	case KMS_COLOROP_MULTIPLIER:
		mult = colorop->multiplier * (mult << 32);	/* convert double to fixed number */
		igt_colorop_set_prop_value(colorop->colorop, IGT_COLOROP_MULTIPLIER, mult);
		break;
	case KMS_COLOROP_LUT3D:
		lut_size = igt_colorop_get_prop(display, colorop->colorop, IGT_COLOROP_SIZE);
		interpolation = igt_colorop_get_prop(display, colorop->colorop, IGT_COLOROP_LUT3D_INTERPOLATION);

		/* Check driver's lut size, color depth and interpolation with kms_colorop */
		igt_skip_on(colorop->lut3d_info.size != lut_size);
		igt_skip_on(colorop->lut3d_info.interpolation != interpolation);

		configure_3dlut(display, colorop, lut_size);
		break;
	case KMS_COLOROP_FIXED_MATRIX:
		igt_colorop_set_prop_enum(colorop->colorop, IGT_COLOROP_FIXED_MATRIX_TYPE,
					  kms_colorop_fixed_matrix_names[colorop->fixed_matrix_info.fixed_matrix_type]);
		break;
	default:
		igt_fail(IGT_EXIT_FAILURE);
	}
}

void set_color_pipeline(igt_display_t *display,
			igt_plane_t *plane,
			kms_colorop_t *colorops[],
			igt_colorop_t *color_pipeline)
{
	igt_colorop_t *next;
	int prop_val = 0;
	int i;

	igt_plane_set_color_pipeline(plane, color_pipeline);

	for (i = 0; colorops[i]; i++)
		set_colorop(display, colorops[i]);

	/* set unused ops in pipeline to bypass */
	next = color_pipeline;
	i = 0;
	while (next) {
		if (!colorops[i] || colorops[i]->colorop != next)
			igt_colorop_set_prop_value(next, IGT_COLOROP_BYPASS, 1);
		else
			i++;

		prop_val = igt_colorop_get_prop(display, next,
						IGT_COLOROP_NEXT);
		next = igt_find_colorop(display, prop_val);
	}
}

void set_color_pipeline_bypass(igt_plane_t *plane)
{
	igt_plane_set_prop_enum(plane, IGT_PLANE_COLOR_PIPELINE, "Bypass");
}

static void reset_colorop(kms_colorop_t *colorop)
{
	igt_assert(colorop->colorop);
	igt_colorop_set_prop_value(colorop->colorop, IGT_COLOROP_BYPASS, 1);

	switch (colorop->type) {
	case KMS_COLOROP_CTM_3X4:
	case KMS_COLOROP_CUSTOM_LUT1D:
	case KMS_COLOROP_LUT3D:
		igt_colorop_replace_prop_blob(colorop->colorop, IGT_COLOROP_DATA, NULL, 0);
		break;
	case KMS_COLOROP_MULTIPLIER:
		igt_colorop_set_prop_value(colorop->colorop, IGT_COLOROP_MULTIPLIER, 1);
		break;
	case KMS_COLOROP_ENUMERATED_LUT1D:
	default:
		return;
	}
}

void reset_colorops(kms_colorop_t *colorops[])
{
	int i;

	for(i = 0; colorops[i]; i++)
		reset_colorop(colorops[i]);
}
