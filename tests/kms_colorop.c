// SPDX-License-Identifier: MIT
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include "sw_sync.h"
#include "kms_colorop_helper.h"

#ifdef ANDROID
#include "android/glib.h"
#else
#include <glib.h>
#endif

/**
 * TEST: kms colorop
 * Category: Display
 * Description: Test to validate the retrieving and setting of DRM colorops
 *
 * SUBTEST: check_plane_colorop_ids
 * Description: Verify that all igt_colorop_t IDs are unique across planes
 *
 * SUBTEST: plane-XR24-XR24-bypass
 * SUBTEST: plane-XR24-XR24-srgb_eotf
 * SUBTEST: plane-XR24-XR24-srgb_inv_eotf
 * SUBTEST: plane-XR24-XR24-srgb_eotf-srgb_inv_eotf
 * SUBTEST: plane-XR24-XR24-srgb_eotf-srgb_inv_eotf-srgb_eotf
 * SUBTEST: plane-XR24-XR24-srgb_inv_eotf_lut
 * SUBTEST: plane-XR24-XR24-srgb_inv_eotf_lut-srgb_eotf_lut
 * SUBTEST: plane-XR24-XR24-bt2020_inv_oetf
 * SUBTEST: plane-XR24-XR24-bt2020_oetf
 * SUBTEST: plane-XR24-XR24-bt2020_inv_oetf-bt2020_oetf
 * SUBTEST: plane-XR24-XR24-pq_eotf
 * SUBTEST: plane-XR24-XR24-pq_inv_eotf
 * SUBTEST: plane-XR24-XR24-pq_eotf-pq_inv_eotf
 * SUBTEST: plane-XR24-XR24-pq_125_eotf
 * SUBTEST: plane-XR24-XR24-pq_125_inv_eotf
 * SUBTEST: plane-XR24-XR24-pq_125_eotf-pq_125_inv_eotf
 * SUBTEST: plane-XR24-XR24-pq_125_eotf-pq_125_inv_eotf-pq_125_eotf
 * SUBTEST: plane-XR24-XR24-gamma_2_2
 * SUBTEST: plane-XR24-XR24-gamma_2_2-gamma_2_2_inv
 * SUBTEST: plane-XR24-XR24-gamma_2_2-gamma_2_2_inv-gamma_2_2
 * SUBTEST: plane-XR24-XR24-ctm_3x4_50_desat
 * SUBTEST: plane-XR24-XR24-ctm_3x4_overdrive
 * SUBTEST: plane-XR24-XR24-ctm_3x4_oversaturate
 * SUBTEST: plane-XR24-XR24-ctm_3x4_bt709_enc
 * SUBTEST: plane-XR24-XR24-ctm_3x4_bt709_dec
 * SUBTEST: plane-XR24-XR24-ctm_3x4_bt709_enc_dec
 * SUBTEST: plane-XR24-XR24-ctm_3x4_bt709_dec_enc
 * SUBTEST: plane-XR24-XR24-multiply_125
 * SUBTEST: plane-XR24-XR24-multiply_inv_125
 * SUBTEST: plane-XR24-XR24-3dlut_17_12_rgb
 * SUBTEST: plane-XR30-XR30-bypass
 * SUBTEST: plane-XR30-XR30-srgb_eotf
 * SUBTEST: plane-XR30-XR30-srgb_inv_eotf
 * SUBTEST: plane-XR30-XR30-srgb_eotf-srgb_inv_eotf
 * SUBTEST: plane-XR30-XR30-srgb_eotf-srgb_inv_eotf-srgb_eotf
 * SUBTEST: plane-XR30-XR30-srgb_inv_eotf_lut
 * SUBTEST: plane-XR30-XR30-srgb_inv_eotf_lut-srgb_eotf_lut
 * SUBTEST: plane-XR30-XR30-bt2020_inv_oetf
 * SUBTEST: plane-XR30-XR30-bt2020_oetf
 * SUBTEST: plane-XR30-XR30-bt2020_inv_oetf-bt2020_oetf
 * SUBTEST: plane-XR30-XR30-pq_eotf
 * SUBTEST: plane-XR30-XR30-pq_inv_eotf
 * SUBTEST: plane-XR30-XR30-pq_eotf-pq_inv_eotf
 * SUBTEST: plane-XR30-XR30-pq_125_eotf
 * SUBTEST: plane-XR30-XR30-pq_125_inv_eotf
 * SUBTEST: plane-XR30-XR30-pq_125_eotf-pq_125_inv_eotf
 * SUBTEST: plane-XR30-XR30-pq_125_eotf-pq_125_inv_eotf-pq_125_eotf
 * SUBTEST: plane-XR30-XR30-gamma_2_2
 * SUBTEST: plane-XR30-XR30-gamma_2_2-gamma_2_2_inv
 * SUBTEST: plane-XR30-XR30-gamma_2_2-gamma_2_2_inv-gamma_2_2
 * SUBTEST: plane-XR30-XR30-ctm_3x4_50_desat
 * SUBTEST: plane-XR30-XR30-ctm_3x4_overdrive
 * SUBTEST: plane-XR30-XR30-ctm_3x4_oversaturate
 * SUBTEST: plane-XR30-XR30-ctm_3x4_bt709_enc
 * SUBTEST: plane-XR30-XR30-ctm_3x4_bt709_dec
 * SUBTEST: plane-XR30-XR30-ctm_3x4_bt709_enc_dec
 * SUBTEST: plane-XR30-XR30-ctm_3x4_bt709_dec_enc
 * SUBTEST: plane-XR30-XR30-multiply_125
 * SUBTEST: plane-XR30-XR30-multiply_inv_125
 * SUBTEST: plane-XR30-XR30-3dlut_17_12_rgb
 * SUBTEST: plane-bypass-XR24-XR24-srgb_eotf
 * SUBTEST: plane-bypass-XR24-XR24-srgb_inv_eotf_lut
 * SUBTEST: plane-bypass-XR24-XR24-ctm_3x4_50_desat
 * SUBTEST: plane-bypass-XR24-XR24-3dlut_17_12_rgb
 * SUBTEST: plane-bypass-XR24-XR24-srgb_eotf-ctm_3x4_50_desat
 * SUBTEST: plane-bypass-XR30-XR30-srgb_eotf
 * SUBTEST: plane-bypass-XR30-XR30-srgb_inv_eotf_lut
 * SUBTEST: plane-bypass-XR30-XR30-ctm_3x4_50_desat
 * SUBTEST: plane-bypass-XR30-XR30-3dlut_17_12_rgb
 * SUBTEST: plane-bypass-XR30-XR30-srgb_eotf-ctm_3x4_50_desat
 * Description: Tests DRM colorop properties on RGB formats
 * Driver requirement: amdgpu
 * Functionality: kms_core
 * Mega feature: General Display Features
 * Test category: functionality test
 *
 * SUBTEST: plane-NV12-XR24-fm_bt709_limited
 * SUBTEST: plane-NV12-XR24-fm_bt709_full
 * SUBTEST: plane-NV12-XR24-fm_bt601_limited
 * SUBTEST: plane-NV12-XR24-fm_bt2020_limited
 * SUBTEST: plane-NV12-XR24-fm_bt709_limited-srgb_eotf
 * SUBTEST: plane-NV12-XR24-fm_bt601_limited-srgb_eotf
 * SUBTEST: plane-NV12-XR24-fm_bt709_limited-3dlut_17_12_rgb
 * SUBTEST: plane-NV12-XR24-fm_bt709_limited-ctm_3x4_50_desat
 * SUBTEST: plane-NV12-XR24-fm_bt709_limited-srgb_eotf-ctm_3x4_50_desat
 * SUBTEST: plane-P010-XR30-fm_bt709_limited
 * SUBTEST: plane-P010-XR30-fm_bt709_full
 * SUBTEST: plane-P010-XR30-fm_bt601_limited
 * SUBTEST: plane-P010-XR30-fm_bt2020_limited
 * SUBTEST: plane-P010-XR30-fm_bt709_limited-srgb_eotf
 * SUBTEST: plane-P010-XR30-fm_bt601_limited-srgb_eotf
 * SUBTEST: plane-P010-XR30-fm_bt709_limited-3dlut_17_12_rgb
 * SUBTEST: plane-P010-XR30-fm_bt709_limited-ctm_3x4_50_desat
 * SUBTEST: plane-P010-XR30-fm_bt709_limited-srgb_eotf-ctm_3x4_50_desat
 * Description: Tests DRM colorop properties on YUV formats
 * Driver requirement: amdgpu
 * Functionality: kms_core
 * Mega feature: General Display Features
 * Test category: functionality test
 *
 */

static bool check_writeback_config(igt_display_t *display, igt_output_t *output,
				    drmModeModeInfo override_mode, __u32 fourcc_in,
				    __u32 fourcc_out)
{
	igt_fb_t input_fb, output_fb;
	igt_plane_t *plane;
	uint32_t writeback_format = fourcc_out;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	int width, height, ret;
	drmModePropertyBlobRes *wb_formats_blob;
	int i;
	__u32 *format;
	bool found_format = false;

	igt_output_override_mode(output, &override_mode);

	width = override_mode.hdisplay;
	height = override_mode.vdisplay;

	plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_skip_on_f(!igt_plane_has_format_mod(plane, fourcc_in, DRM_FORMAT_MOD_LINEAR),
		      "plane doesn't support fourcc format %x\n", fourcc_in);

	ret = igt_create_fb(display->drm_fd, width, height,
			    fourcc_in, modifier, &input_fb);
	igt_assert(ret >= 0);

	/* check writeback formats */
	wb_formats_blob = igt_get_writeback_formats_blob(output);
	format = wb_formats_blob->data;

	for (i = 0; i < wb_formats_blob->length / 4; i++)
		if (fourcc_out == format[i])
			found_format = true;

	igt_skip_on_f(!found_format,
		      "writeback doesn't support fourcc format %x\n", fourcc_out);

	ret = igt_create_fb(display->drm_fd, width, height,
			    writeback_format, modifier, &output_fb);
	igt_assert(ret >= 0);

	igt_plane_set_fb(plane, &input_fb);
	igt_output_set_writeback_fb(output, &output_fb);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_plane_set_fb(plane, NULL);
	igt_remove_fb(display->drm_fd, &input_fb);
	igt_remove_fb(display->drm_fd, &output_fb);

	return !ret;
}

typedef struct {
	bool dump_check;
} data_t;

static data_t data;

static igt_output_t *kms_writeback_get_output(igt_display_t *display, __u32 fourcc_in, __u32 fourcc_out)
{
	igt_output_t *output;
	igt_crtc_t *crtc;

	drmModeModeInfo override_mode = {
		.clock = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.hskew = 0,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
		.vscan = 0,
		.vrefresh = 60,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		.name = {"640x480-60"},
	};

	for_each_output(display, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		for_each_crtc(display, crtc) {
			igt_output_set_crtc(output,
					    crtc);

			if (check_writeback_config(display, output, override_mode, fourcc_in, fourcc_out)) {
				igt_debug("Using connector %u:%s on pipe %s\n",
					  output->config.connector->connector_id,
					  output->name, igt_crtc_name(crtc));
				return output;
			}
		}

		igt_debug("We found %u:%s, but this test will not be able to use it.\n",
			  output->config.connector->connector_id, output->name);

		/* Restore any connectors we don't use, so we don't trip on them later */
		kmstest_force_connector(display->drm_fd, output->config.connector, FORCE_CONNECTOR_UNSPECIFIED);
	}

	return NULL;
}

static bool compare_with_bracket(igt_fb_t *in, igt_fb_t *out)
{
	/* Each driver is expected to have its own bracket, i.e., by trial and error */
	if (is_vkms_device(in->fd))
		return igt_cmp_fb_pixels(in, out, 3, 1);

	if (is_amdgpu_device(in->fd))
		return igt_cmp_fb_pixels(in, out, 13, 13);

	/*
	 * By default we'll look for a [0, 0] bracket. We can then
	 * define it for each driver that implements support for this
	 * test. That way we can understand the precision of each
	 * driver better.
	 */
	return igt_cmp_fb_pixels(in, out, 0, 0);
}

#define MAX_COLOROPS 5

static void apply_transforms(kms_colorop_t *colorops[], igt_fb_t *input_fb,
			     igt_fb_t *sw_transform_fb)
{
	int i;
	int yuv_encoding = -1;
	enum igt_color_range yuv_range = IGT_COLOR_YCBCR_LIMITED_RANGE;
	igt_pixel_transform transforms[MAX_COLOROPS];

	for (i = 0; colorops[i]; i++)
		transforms[i] = colorops[i]->transform;

	/* If first colorop is CSC FF, extract encoding/range for sw reference */
	if (colorops[0] && colorops[0]->type == KMS_COLOROP_FIXED_MATRIX) {
		yuv_encoding = colorops[0]->fixed_matrix_info.encoding;
		yuv_range = colorops[0]->fixed_matrix_info.range;
	}

	igt_color_transform_pixels(input_fb, sw_transform_fb, transforms, i,
				   yuv_encoding, yuv_range);
}

static void colorop_plane_test(igt_display_t *display,
			       igt_output_t *output,
			       igt_plane_t *plane,
			       igt_fb_t *input_fb,
			       igt_fb_t *output_fb,
			       __u32 fourcc_in,
			       __u32 fourcc_out,
			       kms_colorop_t *colorops[],
			       bool verify_bypass)
{
	igt_colorop_t *color_pipeline = NULL;
	igt_fb_t sw_transform_fb;
	int res;

	/* reset color pipeline*/

	igt_plane_set_color_pipeline(plane, NULL);

	/* Commit */
	igt_plane_set_fb(plane, input_fb);
	igt_output_set_writeback_fb(output, output_fb);

	igt_display_commit_atomic(output->display,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL);
	igt_get_and_wait_out_fence(output);

	/* create sw transform buffer with output format */
	res = igt_create_fb(display->drm_fd,
			    input_fb->width, input_fb->height,
			    output_fb->drm_format,
			    DRM_FORMAT_MOD_LINEAR,
			    &sw_transform_fb);
	igt_assert_lte(0, res);

	apply_transforms(colorops, input_fb, &sw_transform_fb);

	if (data.dump_check)
		igt_dump_fb(display, &sw_transform_fb, ".", "sw_transform");

	/* discover and set COLOR PIPELINE */

	if (!colorops[0]) {
		/* bypass test */
		igt_plane_set_color_pipeline(plane, NULL);
	} else {
		/* get COLOR_PIPELINE enum */
		color_pipeline = get_color_pipeline(display, plane, colorops);

		/* skip test if we can't find applicable pipeline */
		igt_skip_on(!color_pipeline);

		set_color_pipeline(display, plane, colorops, color_pipeline);
	}

	igt_output_set_writeback_fb(output, output_fb);

	/* commit COLOR_PIPELINE */
	igt_display_commit_atomic(display,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL);
	igt_get_and_wait_out_fence(output);

	if (data.dump_check)
		igt_dump_fb(display, output_fb, ".", "output");

	/* compare sw transformed and KMS transformed FBs */
	igt_assert(compare_with_bracket(&sw_transform_fb, output_fb));

	/* Test bypass transition if requested */
	if (verify_bypass) {
		/* reset color pipeline*/
		igt_plane_set_color_pipeline(plane, NULL);

		/* Commit */
		igt_plane_set_fb(plane, input_fb);
		igt_output_set_writeback_fb(output, output_fb);

		igt_display_commit_atomic(output->display,
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL);
		igt_get_and_wait_out_fence(output);

		if (data.dump_check)
			igt_dump_fb(display, output_fb, ".", "bypass_output");

		/* For RGB bypass, output should match input */
		igt_assert(compare_with_bracket(input_fb, output_fb));
	}
}

static void check_plane_colorop_ids(igt_display_t *display)
{
	igt_plane_t *plane;
	igt_colorop_t *colorop, *color_pipeline;
	igt_crtc_t *crtc;

	/* Use hash tables to track drm_planes and unique IDs */
	GHashTable *plane_set = g_hash_table_new(g_direct_hash, g_direct_equal);
	GHashTable *id_set = g_hash_table_new(g_direct_hash, g_direct_equal);

	for_each_crtc(display, crtc) {
		for_each_plane_on_crtc(crtc,
				       plane) {
			/* Skip when a drm_plane is already scanned */
			if (g_hash_table_contains(plane_set, GINT_TO_POINTER(plane->drm_plane->plane_id)))
				continue;

			g_hash_table_add(plane_set, GINT_TO_POINTER(plane->drm_plane->plane_id));

			for_each_color_pipeline(plane, color_pipeline) {
				for_each_colorop_in_pipeline(color_pipeline, colorop) {
					/* Check if the ID already exists in the set */
					if (g_hash_table_contains(id_set, GINT_TO_POINTER(colorop->id))) {
						igt_fail_on_f(true, "Duplicate colorop ID %u found on plane %d\n",
						colorop->id, plane->drm_plane->plane_id);
					}

					g_hash_table_add(id_set, GINT_TO_POINTER(colorop->id));
				}
			}
		}
	}

	g_hash_table_destroy(id_set);
	g_hash_table_destroy(plane_set);
	igt_info("All igt_colorop_t IDs are unique across planes\n");
}

static int opt_handler(int option, int option_index, void *_data)
{
	switch (option) {
	case 'd':
		data.dump_check = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}
	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	" --dump | -d Prints buffer to files.\n";

static const struct option long_options[] = {
	{ .name = "dump", .has_arg = false, .val = 'd', },
	{}
};

int igt_main_args("d", long_options, help_str, opt_handler, NULL)
{

	/* RGB tests - for RGB input formats only */
	struct {
		kms_colorop_t *colorops[MAX_COLOROPS];
		const char *name;
	} tests_rgb[] = {
		{ { NULL }, "bypass" },
		{ { &kms_colorop_srgb_eotf, NULL }, "srgb_eotf" },
		{ { &kms_colorop_srgb_inv_eotf, NULL }, "srgb_inv_eotf" },
		{ { &kms_colorop_srgb_eotf, &kms_colorop_srgb_inv_eotf, NULL }, "srgb_eotf-srgb_inv_eotf" },
		{ { &kms_colorop_srgb_eotf, &kms_colorop_srgb_inv_eotf, &kms_colorop_srgb_eotf_2, NULL }, "srgb_eotf-srgb_inv_eotf-srgb_eotf" },
		{ { &kms_colorop_srgb_inv_eotf_lut, NULL }, "srgb_inv_eotf_lut" },
		{ { &kms_colorop_srgb_inv_eotf_lut, &kms_colorop_srgb_eotf_lut, NULL }, "srgb_inv_eotf_lut-srgb_eotf_lut" },
		{ { &kms_colorop_bt2020_inv_oetf, NULL }, "bt2020_inv_oetf" },
		{ { &kms_colorop_bt2020_oetf, NULL }, "bt2020_oetf" },
		{ { &kms_colorop_bt2020_inv_oetf, &kms_colorop_bt2020_oetf, NULL }, "bt2020_inv_oetf-bt2020_oetf" },
		{ { &kms_colorop_pq_eotf, NULL }, "pq_eotf" },
		{ { &kms_colorop_pq_inv_eotf, NULL }, "pq_inv_eotf" },
		{ { &kms_colorop_pq_eotf, &kms_colorop_pq_inv_eotf, NULL }, "pq_eotf-pq_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, NULL }, "pq_125_eotf" },
		{ { &kms_colorop_pq_125_inv_eotf, NULL }, "pq_125_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, &kms_colorop_pq_125_inv_eotf, NULL }, "pq_125_eotf-pq_125_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, &kms_colorop_pq_125_inv_eotf, &kms_colorop_pq_125_eotf_2, NULL }, "pq_125_eotf-pq_125_inv_eotf-pq_125_eotf" },
		{ { &kms_colorop_gamma_22_oetf, NULL }, "gamma_2_2" },
		{ { &kms_colorop_gamma_22_oetf, &kms_colorop_gamma_22_inv_oetf, NULL }, "gamma_2_2-gamma_2_2_inv" },
		{ { &kms_colorop_gamma_22_oetf, &kms_colorop_gamma_22_inv_oetf, &kms_colorop_gamma_22_oetf, NULL }, "gamma_2_2-gamma_2_2_inv-gamma_2_2" },
		{ { &kms_colorop_ctm_3x4_50_desat, NULL }, "ctm_3x4_50_desat" },
		{ { &kms_colorop_ctm_3x4_overdrive, NULL }, "ctm_3x4_overdrive" },
		{ { &kms_colorop_ctm_3x4_oversaturate, NULL }, "ctm_3x4_oversaturate" },
		{ { &kms_colorop_ctm_3x4_bt709_enc, NULL }, "ctm_3x4_bt709_enc" },
		{ { &kms_colorop_ctm_3x4_bt709_dec, NULL }, "ctm_3x4_bt709_dec" },
		{ { &kms_colorop_ctm_3x4_bt709_enc, &kms_colorop_ctm_3x4_bt709_dec, NULL }, "ctm_3x4_bt709_enc_dec" },
		{ { &kms_colorop_ctm_3x4_bt709_dec, &kms_colorop_ctm_3x4_bt709_enc, NULL }, "ctm_3x4_bt709_dec_enc" },
		{ { &kms_colorop_multiply_125, NULL }, "multiply_125" },
		{ { &kms_colorop_multiply_inv_125, NULL }, "multiply_inv_125" },
		{ { &kms_colorop_3dlut_17_12_rgb, NULL }, "3dlut_17_12_rgb" },
	};

	/* YUV tests - CSC FF colorop with various encoding/range combinations and optional additional colorops */
	struct {
		kms_colorop_t *colorops[MAX_COLOROPS];
		const char *name;
	} tests_yuv[] = {
		/* CSC only tests */
		{ { &kms_colorop_bt709_limited_ycbcr_to_rgb, NULL }, "fm_bt709_limited" },
		{ { &kms_colorop_bt709_full_ycbcr_to_rgb, NULL }, "fm_bt709_full" },
		{ { &kms_colorop_bt601_limited_ycbcr_to_rgb, NULL }, "fm_bt601_limited" },
		{ { &kms_colorop_bt2020_limited_ycbcr_to_rgb, NULL }, "fm_bt2020_limited" },
		/* CSC + additional colorops */
		{ { &kms_colorop_bt709_limited_ycbcr_to_rgb, &kms_colorop_srgb_eotf, NULL }, "fm_bt709_limited-srgb_eotf" },
		{ { &kms_colorop_bt601_limited_ycbcr_to_rgb, &kms_colorop_srgb_eotf, NULL }, "fm_bt601_limited-srgb_eotf" },
		{ { &kms_colorop_bt709_limited_ycbcr_to_rgb, &kms_colorop_3dlut_17_12_rgb, NULL }, "fm_bt709_limited-3dlut_17_12_rgb" },
		{ { &kms_colorop_bt709_limited_ycbcr_to_rgb, &kms_colorop_ctm_3x4_50_desat, NULL }, "fm_bt709_limited-ctm_3x4_50_desat" },
		{ { &kms_colorop_bt709_limited_ycbcr_to_rgb, &kms_colorop_srgb_eotf, &kms_colorop_ctm_3x4_50_desat, NULL }, "fm_bt709_limited-srgb_eotf-ctm_3x4_50_desat" },
	};

	/* Bypass transition tests - test config -> bypass -> verify identity (RGB only) */
	struct {
		kms_colorop_t *colorops[MAX_COLOROPS];
		const char *name;
	} tests_bypass_transitions_rgb[] = {
		/* One per colorop type */
		{ { &kms_colorop_srgb_eotf, NULL }, "srgb_eotf" },
		{ { &kms_colorop_srgb_inv_eotf_lut, NULL }, "srgb_inv_eotf_lut" },
		{ { &kms_colorop_ctm_3x4_50_desat, NULL }, "ctm_3x4_50_desat" },
		{ { &kms_colorop_3dlut_17_12_rgb, NULL }, "3dlut_17_12_rgb" },
		/* Multi-stage */
		{ { &kms_colorop_srgb_eotf, &kms_colorop_ctm_3x4_50_desat, NULL }, "srgb_eotf-ctm_3x4_50_desat" },
	};

	struct {
		__u32 fourcc_in;
		__u32 fourcc_out;
		const char *name;
	} formats_rgb[] = {
		{ DRM_FORMAT_XRGB8888, DRM_FORMAT_XRGB8888, "XR24-XR24" },
		{ DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB2101010, "XR30-XR30" },
	};

	struct {
		__u32 fourcc_in;
		__u32 fourcc_out;
		const char *name;
	} formats_yuv[] = {
		{ DRM_FORMAT_NV12, DRM_FORMAT_XRGB8888, "NV12-XR24" },
		{ DRM_FORMAT_P010, DRM_FORMAT_XRGB2101010, "P010-XR30" },
	};

	igt_display_t display;
	int i, j, ret;
	int has_plane_color_pipeline = 0;

	igt_fixture() {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);

		if (drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
			display.is_atomic = 1;

		ret = drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);

		igt_require_f(!ret, "error setting DRM_CLIENT_CAP_WRITEBACK_CONNECTORS\n");

		if (drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0)
			has_plane_color_pipeline = 1;

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, display.drm_fd);
		display.has_plane_color_pipeline = has_plane_color_pipeline;

		igt_require(display.is_atomic);
	}

	igt_subtest_f("check_plane_colorop_ids") {
		check_plane_colorop_ids(&display);
	}

	/* RGB format tests */
	for (j = 0; j < ARRAY_SIZE(formats_rgb); j++) {
		igt_output_t *output;
		igt_plane_t *plane;
		igt_fb_t input_fb, output_fb;
		unsigned int fb_id;
		drmModeModeInfo mode;

		igt_subtest_group() {
			igt_fixture() {
				output = kms_writeback_get_output(&display,
								  formats_rgb[j].fourcc_in,
								  formats_rgb[j].fourcc_out);
				igt_require(output);

				if (output->use_override_mode)
					memcpy(&mode, &output->override_mode, sizeof(mode));
				else
					memcpy(&mode, &output->config.default_mode, sizeof(mode));

				/* create input fb */
				plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				igt_assert(plane);
				igt_require(igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE));

				fb_id = igt_create_color_pattern_fb(display.drm_fd,
								mode.hdisplay, mode.vdisplay,
								formats_rgb[j].fourcc_in, DRM_FORMAT_MOD_LINEAR,
								0.2, 0.2, 0.2, &input_fb);
				igt_assert(fb_id >= 0);
				igt_plane_set_fb(plane, &input_fb);

				if (data.dump_check)
					igt_dump_fb(&display, &input_fb, ".", "input");

				/* create output fb */
				fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
							formats_rgb[j].fourcc_out,
							igt_fb_mod_to_tiling(0),
							&output_fb);
				igt_require(fb_id > 0);
			}

			/* Run RGB tests */
			for (i = 0; i < ARRAY_SIZE(tests_rgb); i++) {
				igt_describe("Check color ops on a plane");
				igt_subtest_f("plane-%s-%s", formats_rgb[j].name, tests_rgb[i].name)
					colorop_plane_test(&display,
							output,
							plane,
							&input_fb,
							&output_fb,
							formats_rgb[j].fourcc_in,
							formats_rgb[j].fourcc_out,
							tests_rgb[i].colorops,
							false);
			}

			igt_fixture() {
				igt_detach_crtc(&display, output);
				igt_remove_fb(display.drm_fd, &input_fb);
				igt_remove_fb(display.drm_fd, &output_fb);

			}
		}
	}

	/* Bypass transition tests - RGB formats */
	for (j = 0; j < ARRAY_SIZE(formats_rgb); j++) {
		igt_output_t *output;
		igt_plane_t *plane;
		igt_fb_t input_fb, output_fb;
		unsigned int fb_id;
		drmModeModeInfo mode;

		igt_subtest_group() {
			igt_fixture() {
				output = kms_writeback_get_output(&display,
								  formats_rgb[j].fourcc_in,
								  formats_rgb[j].fourcc_out);
				igt_require(output);

				if (output->use_override_mode)
					memcpy(&mode, &output->override_mode, sizeof(mode));
				else
					memcpy(&mode, &output->config.default_mode, sizeof(mode));

				/* create input fb */
				plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				igt_assert(plane);
				igt_require(igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE));

				fb_id = igt_create_color_pattern_fb(display.drm_fd,
								mode.hdisplay, mode.vdisplay,
								formats_rgb[j].fourcc_in, DRM_FORMAT_MOD_LINEAR,
								0.2, 0.2, 0.2, &input_fb);
				igt_assert(fb_id >= 0);
				igt_plane_set_fb(plane, &input_fb);

				if (data.dump_check)
					igt_dump_fb(&display, &input_fb, ".", "input");

				/* create output fb */
				fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
							formats_rgb[j].fourcc_out,
							igt_fb_mod_to_tiling(0),
							&output_fb);
				igt_require(fb_id > 0);
			}

			/* Run bypass transition tests */
			for (i = 0; i < ARRAY_SIZE(tests_bypass_transitions_rgb); i++) {
				igt_describe("Test color pipeline to bypass transition");
				igt_subtest_f("plane-bypass-%s-%s", formats_rgb[j].name, tests_bypass_transitions_rgb[i].name)
					colorop_plane_test(&display,
							output,
							plane,
							&input_fb,
							&output_fb,
							formats_rgb[j].fourcc_in,
							formats_rgb[j].fourcc_out,
							tests_bypass_transitions_rgb[i].colorops,
							true);
			}

			igt_fixture() {
				igt_detach_crtc(&display, output);
				igt_remove_fb(display.drm_fd, &input_fb);
				igt_remove_fb(display.drm_fd, &output_fb);

			}
		}
	}

	/* YUV format tests */
	for (j = 0; j < ARRAY_SIZE(formats_yuv); j++) {
		igt_output_t *output;
		igt_plane_t *plane;
		igt_fb_t temp_fb, output_fb;
		unsigned int fb_id;
		drmModeModeInfo mode;

		igt_subtest_group() {
			igt_fixture() {
				output = kms_writeback_get_output(&display,
								  formats_yuv[j].fourcc_in,
								  formats_yuv[j].fourcc_out);
				igt_require(output);

				if (output->use_override_mode)
					memcpy(&mode, &output->override_mode, sizeof(mode));
				else
					memcpy(&mode, &output->config.default_mode, sizeof(mode));

				plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				igt_assert(plane);
				igt_require(igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE));

				/* Create temp fb to keep plane active between tests */
				fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
							formats_yuv[j].fourcc_in,
							igt_fb_mod_to_tiling(0),
							&temp_fb);
				igt_require(fb_id > 0);

				/* Create output fb shared across tests */
				fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
							formats_yuv[j].fourcc_out,
							igt_fb_mod_to_tiling(0),
							&output_fb);
				igt_require(fb_id > 0);
			}

			/* Run YUV tests - create input_fb per test */
			for (i = 0; i < ARRAY_SIZE(tests_yuv); i++) {
				igt_describe("Check YUV CSC colorop");
				igt_subtest_f("plane-%s-%s", formats_yuv[j].name, tests_yuv[i].name) {
					igt_fb_t input_fb;
					enum igt_color_encoding encoding;
					enum igt_color_range range;

					/* Extract encoding and range from first colorop (must be CSC FF) */
					igt_assert(tests_yuv[i].colorops[0]);
					igt_assert(tests_yuv[i].colorops[0]->type == KMS_COLOROP_FIXED_MATRIX);

					encoding = tests_yuv[i].colorops[0]->fixed_matrix_info.encoding;
					range = tests_yuv[i].colorops[0]->fixed_matrix_info.range;

					/* Create input fb with matching encoding/range */
					fb_id = igt_create_color_pattern_fb_yuv(display.drm_fd,
									mode.hdisplay, mode.vdisplay,
									formats_yuv[j].fourcc_in, DRM_FORMAT_MOD_LINEAR,
									encoding, range,
									0.2, 0.2, 0.2, &input_fb);
					igt_assert(fb_id >= 0);

					if (data.dump_check)
						igt_dump_fb(&display, &input_fb, ".", "input");

					colorop_plane_test(&display,
							output,
							plane,
							&input_fb,
							&output_fb,
							formats_yuv[j].fourcc_in,
							formats_yuv[j].fourcc_out,
							tests_yuv[i].colorops,
							false);

					/* Switch plane back to temp_fb to keep CRTC active */
					igt_plane_set_fb(plane, &temp_fb);
					igt_display_commit_atomic(&display,
								DRM_MODE_ATOMIC_ALLOW_MODESET,
								NULL);

					igt_remove_fb(display.drm_fd, &input_fb);
				}
			}

			igt_fixture() {
				igt_detach_crtc(&display, output);
				igt_remove_fb(display.drm_fd, &temp_fb);
				igt_remove_fb(display.drm_fd, &output_fb);
			}
		}
	}

	igt_fixture() {
		igt_display_fini(&display);
		drm_close_driver(display.drm_fd);
	}
}
