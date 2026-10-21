// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: kms chamelium color pipeline
 * Category: Display
 * Description: Test to validate DRM plane color pipeline using Chamelium frame capture instead of pipe CRC
 * Driver requirement: i915, xe
 * Mega feature: Color Management
 */

#include "kms_chamelium_helper.h"
#include "kms_color_helper.h"
#include "kms_colorop_helper.h"

#define MAX_COLOROPS	5

/**
 * SUBTEST: plane-%s
 * Description: Test plane color pipeline with colorops: %arg[1].
 *
 * arg[1]:
 *
 * @lut1d:			1D LUT
 * @lut1d-pre-ctm3x4:		1D LUT PRE CTM 3x4
 * @lut1d-post-ctm3x4:		1D LUT POST CTM 3x4
 * @ctm3x4:			3X4 CTM
 * @lut1d-ctm3x4:		1D LUT --> 3X4 CTM
 * @ctm3x4-lut1d:		3X4 CTM --> 1D LUT
 * @lut1d-lut1d:		1D LUT --> 1D LUT
 * @lut1d-ctm3x4-lut1d:		1D LUT --> 3X4 CTM --> 1D LUT
 * @lut3d-green-only:		3D LUT
 */

/**
 * SUBTEST: plane-fixed-matrix-%s
 * Description: Test FIXED_MATRIX colorop for color space conversion: %arg[1].
 *
 * arg[1]:
 *
 * @yuv-rgb-bt601:		YUV BT.601 full range to RGB BT.601
 * @yuv-rgb-bt709:		YUV BT.709 full range to RGB BT.709
 * @yuv-rgb-bt2020:		YUV BT.2020 full range to RGB BT.2020
 * @yuv-rgb-bt601-lim:		YUV BT.601 limited range to RGB BT.601
 * @yuv-rgb-bt709-lim:		YUV BT.709 limited range to RGB BT.709
 * @yuv-rgb-bt2020-lim:		YUV BT.2020 limited range to RGB BT.2020
 * @yuv-rgb-bt601-lut1d:	YUV BT.601 full range to RGB BT.601, then 1D LUT
 * @yuv-rgb-bt709-lut1d:	YUV BT.709 full range to RGB BT.709, then 1D LUT
 * @yuv-rgb-bt2020-lut1d:	YUV BT.2020 full range to RGB BT.2020, then 1D LUT
 * @yuv-rgb-bt601-lim-lut1d:	YUV BT.601 limited range to RGB BT.601, then 1D LUT
 * @yuv-rgb-bt709-lim-lut1d:	YUV BT.709 limited range to RGB BT.709, then 1D LUT
 * @yuv-rgb-bt2020-lim-lut1d:	YUV BT.2020 limited range to RGB BT.2020, then 1D LUT
 */

IGT_TEST_DESCRIPTION("Test to validate DRM plane color pipeline using Chamelium frame capture instead of pipe CRC");

static void test_cleanup(data_t *data)
{
	igt_output_set_crtc(data->output, NULL);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static int test_setup(data_t *data, igt_crtc_t *crtc)
{
	int i;

	igt_require(crtc);
	igt_require(crtc->n_planes > 0);

	igt_output_set_crtc(data->output, crtc);

	data->mode = igt_output_get_mode(data->output);
	igt_require(data->mode);

	/* Disable pipe color props. */
	disable_ctm(crtc);
	disable_degamma(crtc);
	disable_gamma(crtc);

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	/*
	 * Prefer HDMI over DP to avoid DP FSM issues
	 * during chamelium capture.
	 */
	for (i = 0; i < data->port_count; i++) {
		if ((data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
		     data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
		    !strcmp(data->output->name,
			    chamelium_port_get_name(data->ports[i])))
			return i;
	}

	/* Fall back to any matching port */
	for (i = 0; i < data->port_count; i++) {
		if (!strcmp(data->output->name,
			    chamelium_port_get_name(data->ports[i])))
			return i;
	}

	return -1;
}

static bool ctm_colorop_only(kms_colorop_t *colorops[])
{
	int i;

	if (!colorops[0])
		return false;

	for (i = 0; colorops[i]; i++) {
		if (colorops[i]->type != KMS_COLOROP_CTM_3X4)
			return false;
	}

	return true;
}

static void _test_plane_colorops(data_t *data,
				 igt_plane_t *plane,
				 const color_t *fb_colors,
				 const color_t *exp_colors,
				 kms_colorop_t *colorops[],
				 struct chamelium_port *port,
					     uint32_t input_format)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode = data->mode;
	igt_colorop_t *color_pipeline;
	struct igt_fb fb, fbref;
	struct chamelium_frame_dump *frame;
	bool is_yuv = igt_format_is_yuv(input_format);
	bool ret;

	color_pipeline = get_color_pipeline(display, plane, colorops);
	igt_skip_on(!color_pipeline);

	/* Create HW framebuffer */
	if (is_yuv) {
		enum igt_color_encoding encoding;
		enum igt_color_range range;

		igt_assert(colorops[0]);
		igt_assert_eq(colorops[0]->type, KMS_COLOROP_FIXED_MATRIX);
		encoding = colorops[0]->fixed_matrix_info.encoding;
		range = colorops[0]->fixed_matrix_info.range;

		igt_assert(igt_create_fb_with_bo_size(data->drm_fd,
						      mode->hdisplay,
						      mode->vdisplay,
						      input_format,
						      DRM_FORMAT_MOD_LINEAR,
						      encoding, range,
						      &fb, 0, 0));
	} else {
		igt_assert(igt_create_fb(data->drm_fd,
					 mode->hdisplay,
					 mode->vdisplay,
					 input_format,
					 DRM_FORMAT_MOD_LINEAR,
					 &fb));
	}

	/* Paint test fb */
	if (ctm_colorop_only(colorops) || is_yuv)
		paint_rectangles(data, mode, fb_colors, &fb);
	else
		paint_gradient_rectangles(data, mode, fb_colors, &fb);

	/* Create reference framebuffer */
	if (is_yuv) {
		igt_fb_convert(&fbref, &fb, DRM_FORMAT_XRGB8888,
			       DRM_FORMAT_MOD_LINEAR);
	} else {
		igt_assert(igt_create_fb(data->drm_fd,
					 mode->hdisplay,
					 mode->vdisplay,
					 DRM_FORMAT_XRGB8888,
					 DRM_FORMAT_MOD_LINEAR,
					 &fbref));
		paint_rectangles(data, mode, exp_colors, &fbref);
	}

	/* ---- Hardware path ---- */
	set_color_pipeline(display, plane, colorops, color_pipeline);

	igt_plane_set_fb(plane, &fb);
	igt_display_commit_atomic(&data->display, 0, NULL);

	chamelium_port_wait_video_input_stable(data->chamelium, port, 5);
	chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);

	frame = chamelium_read_captured_frame(data->chamelium, 0);

	ret = chamelium_frame_match_or_dump(data->chamelium,
					    port,
					    frame,
					    &fbref,
					    CHAMELIUM_CHECK_ANALOG);

	chamelium_destroy_frame_dump(frame);

	/* Cleanup before assert so failed plane doesn't affect next plane */
	igt_plane_set_color_pipeline(plane, NULL);
	reset_colorops(colorops);
	igt_plane_set_fb(plane, NULL);
	igt_display_commit_atomic(&data->display, 0, NULL);
	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &fbref);

	igt_assert(ret);
}

static void
test_plane_colorops(data_t *data, igt_crtc_t *crtc,
		    const color_t *fb_colors,
		    const color_t *exp_colors,
		    kms_colorop_t *colorops[],
		    int port_idx)
{
	int n_planes = crtc->n_planes;
	igt_output_t *output = data->output;
	igt_plane_t *plane;

	for (int plane_id = 0; plane_id < n_planes; plane_id++) {
		plane = igt_output_get_plane(output, plane_id);

		if (!igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE))
			continue;

		igt_dynamic_f("pipe-%s-plane-%u", igt_crtc_name(crtc), plane_id)
			_test_plane_colorops(data, plane, fb_colors,
					     exp_colors, colorops,
					     data->ports[port_idx],
					     DRM_FORMAT_XRGB8888);
	}
}

static void
run_tests_for_plane(data_t *data)
{
	igt_crtc_t *crtc;
	igt_output_t *output = NULL;
	int port_idx = 0;
	static const color_t colors_rgb[] = {
	        { 1.0, 0.0, 0.0 },
	        { 0.0, 1.0, 0.0 },
	        { 0.0, 0.0, 1.0 },
	};
	static const color_t colors_red_to_blue[] = {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 },
	};
	static const color_t colors_red_and_green[] = {
		{ 1.0, 1.0, 0.0 },
		{ 1.0, 1.0, 0.0 },
		{ 1.0, 1.0, 0.0 }
	};
	static const color_t colors_only_green[] = {
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 1.0, 0.0 }
	};
	const igt_matrix_3x4_t ctm_red_to_blue = { {
		0.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		1.0, 0.0, 1.0, 0.0,
	} };
	const igt_matrix_3x4_t ctm_linear = { {
		1.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
	} };
	kms_colorop_t lut1d_linear = {
		.type = KMS_COLOROP_CUSTOM_LUT1D,
		.name = "Pre/Post CSC GAMMA (linear LUT)",
		.lut1d = &igt_1dlut_linear,
		.transform = &igt_color_linear,
	};
	kms_colorop_t lut1d_max = {
		.type = KMS_COLOROP_CUSTOM_LUT1D,
		.lut1d = &igt_1dlut_max,
		.name = "Pre/Post CSC GAMMA (max LUT)",
		.transform = &igt_color_max,
	};
	kms_colorop_t lut3d = {
		.type = KMS_COLOROP_LUT3D,
		.lut3d = &igt_3dlut_17_green_only,
		.lut3d_info = {
			.size = 17,
			.interpolation = DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL,
		},
		.name = "3dlut passing only green channel (RGB order)",
		.transform = NULL,
	};
	kms_colorop_t ctm_3x4 = {
		.type = KMS_COLOROP_CTM_3X4,
		.name = "CTM 3X4 (red to blue)",
		.matrix_3x4 = &ctm_red_to_blue,
	};
	kms_colorop_t ctm_3x4_linear = {
		.type = KMS_COLOROP_CTM_3X4,
		.name = "CTM 3X4 (linear)",
		.matrix_3x4 = &ctm_linear,
	};

	struct {
		const char *name;
		const color_t *fb_colors;
		const color_t *exp_colors;
		kms_colorop_t *colorops[MAX_COLOROPS];
	} plane_colorops_tests[] = {
		{ .name = "lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_max, NULL },
		},
		{ .name = "lut1d-pre-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_max, &ctm_3x4_linear, NULL },
		},
		{ .name = "lut1d-post-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &ctm_3x4_linear, &lut1d_max, NULL },
		},
		{ .name = "ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &ctm_3x4, NULL },
		},
		{ .name = "lut1d-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &lut1d_max, &ctm_3x4, NULL },
		},
		{ .name = "ctm3x4-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &ctm_3x4, &lut1d_max, NULL },
		},
		{ .name = "lut1d-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_linear, &lut1d_max, NULL },
		},
		{ .name = "lut1d-ctm3x4-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &lut1d_linear, &ctm_3x4, &lut1d_max, NULL },
		},
		{ .name = "lut3d-green-only",
		  .fb_colors = colors_red_and_green,
		  .exp_colors = colors_only_green,
		  .colorops = { &lut3d, NULL },
		},
	};

	for (int i = 0; i < ARRAY_SIZE(plane_colorops_tests); i++) {
		igt_describe_f("Test plane color pipeline with colorops: %s", plane_colorops_tests[i].name);
		igt_subtest_with_dynamic_f("plane-%s", plane_colorops_tests[i].name) {
			for_each_crtc_with_single_output(&data->display, crtc,
							 output) {
				data->output = output;

				if (!crtc_output_combo_valid(data, crtc))
					continue;

				port_idx = test_setup(data, crtc);
				if (port_idx < 0) {
					test_cleanup(data);
					continue;
				}

				test_plane_colorops(data, crtc,
						    plane_colorops_tests[i].fb_colors,
						    plane_colorops_tests[i].exp_colors,
						    plane_colorops_tests[i].colorops,
						    port_idx);

				test_cleanup(data);
			}
		}
	}
}

static void
test_plane_fixed_matrix_colorops(data_t *data, igt_crtc_t *crtc,
				 const color_t *fb_colors,
				 kms_colorop_t *colorops[],
				 int port_idx)
{
	int n_planes = crtc->n_planes;
	igt_output_t *output = data->output;
	igt_plane_t *plane;

	for (int plane_id = 0; plane_id < n_planes; plane_id++) {
		plane = igt_output_get_plane(output, plane_id);

		if (!igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE))
			continue;

		igt_dynamic_f("pipe-%s-plane-%u", igt_crtc_name(crtc), plane_id)
			_test_plane_colorops(data, plane, fb_colors,
					     NULL, colorops,
					     data->ports[port_idx],
					     DRM_FORMAT_YUYV);
	}
}

static void
run_tests_for_fixed_matrix(data_t *data)
{
	igt_crtc_t *crtc;
	igt_output_t *output = NULL;
	int port_idx = 0;
	static const color_t colors_rgb[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 },
	};
	kms_colorop_t lut1d_linear = {
		.type = KMS_COLOROP_CUSTOM_LUT1D,
		.name = "1D LUT (linear)",
		.lut1d = &igt_1dlut_linear,
		.transform = &igt_color_linear,
	};
	kms_colorop_t fixed_matrix_yuv601_rgb601 = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV601 to RGB601",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR601_FULL_RGB,
			.encoding = IGT_COLOR_YCBCR_BT601,
			.range = IGT_COLOR_YCBCR_FULL_RANGE,
		},
	};
	kms_colorop_t fixed_matrix_yuv709_rgb709 = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV709 to RGB709",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR709_FULL_RGB,
			.encoding = IGT_COLOR_YCBCR_BT709,
			.range = IGT_COLOR_YCBCR_FULL_RANGE,
		},
	};
	kms_colorop_t fixed_matrix_yuv2020_rgb2020 = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV2020 to RGB2020",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_FULL_RGB,
			.encoding = IGT_COLOR_YCBCR_BT2020,
			.range = IGT_COLOR_YCBCR_FULL_RANGE,
		},
	};
	kms_colorop_t fixed_matrix_yuv601_rgb601_lim = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV601 limited to RGB601",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR601_LIMITED_RGB,
			.encoding = IGT_COLOR_YCBCR_BT601,
			.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
		},
	};
	kms_colorop_t fixed_matrix_yuv709_rgb709_lim = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV709 limited to RGB709",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR709_LIMITED_RGB,
			.encoding = IGT_COLOR_YCBCR_BT709,
			.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
		},
	};
	kms_colorop_t fixed_matrix_yuv2020_rgb2020_lim = {
		.type = KMS_COLOROP_FIXED_MATRIX,
		.name = "FIXED_MATRIX YUV2020 limited to RGB2020",
		.fixed_matrix_info = {
						.fixed_matrix_type = KMS_COLOROP_FIXED_MATRIX_YCBCR2020_NC_LIMITED_RGB,
			.encoding = IGT_COLOR_YCBCR_BT2020,
			.range = IGT_COLOR_YCBCR_LIMITED_RANGE,
		},
	};

	struct {
		const char *name;
		const color_t *fb_colors;
		kms_colorop_t *colorops[MAX_COLOROPS];
	} fixed_matrix_tests[] = {
		{ .name = "yuv-rgb-bt601",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv601_rgb601, NULL },
		},
		{ .name = "yuv-rgb-bt709",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv709_rgb709, NULL },
		},
		{ .name = "yuv-rgb-bt2020",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv2020_rgb2020, NULL },
		},
		{ .name = "yuv-rgb-bt601-lim",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv601_rgb601_lim, NULL },
		},
		{ .name = "yuv-rgb-bt709-lim",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv709_rgb709_lim, NULL },
		},
		{ .name = "yuv-rgb-bt2020-lim",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv2020_rgb2020_lim, NULL },
		},
		{ .name = "yuv-rgb-bt601-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv601_rgb601, &lut1d_linear, NULL },
		},
		{ .name = "yuv-rgb-bt709-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv709_rgb709, &lut1d_linear, NULL },
		},
		{ .name = "yuv-rgb-bt2020-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv2020_rgb2020, &lut1d_linear, NULL },
		},
		{ .name = "yuv-rgb-bt601-lim-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv601_rgb601_lim, &lut1d_linear, NULL },
		},
		{ .name = "yuv-rgb-bt709-lim-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv709_rgb709_lim, &lut1d_linear, NULL },
		},
		{ .name = "yuv-rgb-bt2020-lim-lut1d",
		  .fb_colors = colors_rgb,
		  .colorops = { &fixed_matrix_yuv2020_rgb2020_lim, &lut1d_linear, NULL },
		},
	};

	for (int i = 0; i < ARRAY_SIZE(fixed_matrix_tests); i++) {
		igt_describe_f("Test FIXED_MATRIX pipeline: plane-fixed-matrix-%s",
			       fixed_matrix_tests[i].name);
		igt_subtest_with_dynamic_f("plane-fixed-matrix-%s",
					   fixed_matrix_tests[i].name) {
			for_each_crtc_with_single_output(&data->display, crtc,
							 output) {
				data->output = output;

				if (!crtc_output_combo_valid(data, crtc))
					continue;

				port_idx = test_setup(data, crtc);
				if (port_idx < 0) {
					test_cleanup(data);
					continue;
				}

				test_plane_fixed_matrix_colorops(data, crtc,
								 fixed_matrix_tests[i].fb_colors,
								 fixed_matrix_tests[i].colorops,
								 port_idx);

				test_cleanup(data);
			}
		}
	}
}

int igt_main()
{
	int i;
	int has_plane_color_pipeline = 0;
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		drmSetClientCap(data.drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

		if (drmSetClientCap(data.drm_fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0)
			has_plane_color_pipeline = 1;

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		data.display.has_plane_color_pipeline = has_plane_color_pipeline;
		igt_require(data.display.is_atomic);
		igt_require(has_plane_color_pipeline);

		igt_chamelium_allow_fsm_handling = false;

		/* Chamelium init */
		data.chamelium = chamelium_init(data.drm_fd, &data.display);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		if (!data.port_count)
			igt_skip("No ports connected\n");

		/*
		 * The behavior differs based on the availability of port mappings:
		 * - When using port mappings (chamelium_read_port_mappings),
		 *   ports are not plugged
		 * - During autodiscovery, all ports are plugged at the end.
		 *
		 * This quick workaround (unplug, plug, and re-probe the connectors)
		 * prevents any ports from being unintentionally skipped in test_setup.
		 */
		for (i = 0; i < data.port_count; i++) {
			chamelium_unplug(data.chamelium, data.ports[i]);
			chamelium_plug(data.chamelium, data.ports[i]);
			chamelium_wait_for_conn_status_change(&data.display,
							      data.chamelium,
							      data.ports[i],
							      DRM_MODE_CONNECTED);
			igt_assert_f(chamelium_reprobe_connector(&data.display,
								 data.chamelium,
								 data.ports[i]) == DRM_MODE_CONNECTED,
								 "Output not connected\n");
		}
	}

	igt_subtest_group()
		run_tests_for_plane(&data);

	igt_subtest_group()
		run_tests_for_fixed_matrix(&data);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
