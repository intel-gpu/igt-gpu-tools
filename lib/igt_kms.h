/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright © 2013 Intel Corporation
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
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 */

#ifndef __IGT_KMS_H__
#define __IGT_KMS_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include <xf86drmMode.h>

#include "igt_core.h"
#include "igt_fb.h"
#include "ioctl_wrappers.h"

/* Low-level helpers with kmstest_ prefix */

/**
 * Clients which do set cursor hotspot and treat the cursor plane
 * like a mouse cursor should set this property.
 */
#define LOCAL_DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT	6

/**
 * hardware_pipe:
 *
 * GPU vendor-specific hardware pipe, which may be different from the CRTC
 * index. Only code and tests that specifically need to know the pipe should use
 * this, and conditional to the device in question. Defaults to matching the
 * CRTC index, but do not rely on it.
 *
 * @PIPE_NONE: Invalid pipe, used for disconnecting a output from a pipe.
 * @PIPE_A: First crtc.
 * @PIPE_B: Second crtc.
 * @PIPE_C: Third crtc.
 * @PIPE_D: Fourth crtc.
 * @PIPE_E: Fifth crtc.
 * @PIPE_F: Sixth crtc.
 * @PIPE_G: Seventh crtc.
 * @PIPE_H: Eighth crtc.
 * @PIPE_I: Ninth crtc.
 * @PIPE_J: Tenth crtc.
 * @PIPE_K: Eleventh crtc.
 * @PIPE_L: Twelfth crtc.
 * @PIPE_M: Thirteenth crtc.
 * @PIPE_N: Fourteenth crtc.
 * @PIPE_O: Fifteenth crtc.
 * @PIPE_P: Sixteenth crtc.
 * @IGT_MAX_PIPES: Max number of pipes allowed.
 */
enum hardware_pipe {
        PIPE_NONE = -1,
        PIPE_A = 0,
        PIPE_B,
        PIPE_C,
        PIPE_D,
        PIPE_E,
        PIPE_F,
        PIPE_G,
        PIPE_H,
        PIPE_I,
        PIPE_J,
        PIPE_K,
        PIPE_L,
        PIPE_M,
        PIPE_N,
        PIPE_O,
        PIPE_P,
        IGT_MAX_PIPES
};
const char *kmstest_pipe_name(enum hardware_pipe pipe);
const char *kmstest_plane_type_name(int plane_type);

enum port {
        PORT_A = 0,
        PORT_B,
        PORT_C,
        PORT_D,
        PORT_E,
        I915_MAX_PORTS
};

/**
 * igt_custom_edid_type:
 *
 * Enum used for the helper function igt_custom_edid_type
 * @IGT_CUSTOM_EDID_BASE: Returns base edid
 * @IGT_CUSTOM_EDID_FULL: Returns edid with full list of standard timings.
 * @IGT_CUSTOM_EDID_ALT: Returns alternate edid
 * @IGT_CUSTOM_EDID_HDMI_AUDIO: Returns edid with HDMI audio block
 * @IGT_CUSTOM_EDID_DP_AUDIO: Returns edid with DP audio block
 * @IGT_CUSTOM_EDID_ASPECT_RATIO: Returns base edid with apect ratio data block
 */
enum igt_custom_edid_type {
	IGT_CUSTOM_EDID_BASE,
	IGT_CUSTOM_EDID_FULL,
	IGT_CUSTOM_EDID_ALT,
	IGT_CUSTOM_EDID_HDMI_AUDIO,
	IGT_CUSTOM_EDID_DP_AUDIO,
	IGT_CUSTOM_EDID_ASPECT_RATIO,
};
#define IGT_CUSTOM_EDID_COUNT 6

/**
 * kmstest_port_name:
 * @port: display plane
 *
 * Returns: String representing @port, e.g. "A".
 */
#define kmstest_port_name(port) ((port) + 'A')

enum dsc_output_format {
	DSC_FORMAT_RGB = 0,
	DSC_FORMAT_YCBCR420,
	DSC_FORMAT_YCBCR444,
};

const char *kmstest_encoder_type_str(int type);
const char *kmstest_connector_status_str(int status);
const char *kmstest_connector_type_str(int type);
const char *kmstest_scaling_filter_str(int filter);
const char *kmstest_scaling_mode_str(int mode);
const char *kmstest_dsc_output_format_str(int output_format);

void kmstest_dump_mode(drmModeModeInfo *mode);
#define HDISPLAY_6K_PER_PIPE 6144
#define HDISPLAY_5K_PER_PIPE 5120

enum hardware_pipe __intel_get_pipe_from_crtc_index(int fd, int crtc_index);
int kmstest_get_crtc_index_from_id(int fd, int crtc_id);
void kmstest_set_vt_graphics_mode(void);
void kmstest_restore_vt_mode(void);
void kmstest_set_vt_text_mode(void);
void reset_connectors_at_exit(int sig);

enum igt_atomic_crtc_properties {
       IGT_CRTC_CTM = 0,
       IGT_CRTC_GAMMA_LUT,
       IGT_CRTC_GAMMA_LUT_SIZE,
       IGT_CRTC_DEGAMMA_LUT,
       IGT_CRTC_DEGAMMA_LUT_SIZE,
       IGT_CRTC_MODE_ID,
       IGT_CRTC_ACTIVE,
       IGT_CRTC_OUT_FENCE_PTR,
       IGT_CRTC_VRR_ENABLED,
       IGT_CRTC_SCALING_FILTER,
       IGT_CRTC_SHARPNESS_STRENGTH,
       IGT_CRTC_BACKGROUND_COLOR,
       IGT_NUM_CRTC_PROPS
};

/**
 * igt_crtc_prop_names
 *
 * igt_crtc_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_crtc_properties enum.
 */
extern const char * const igt_crtc_prop_names[];

enum igt_atomic_connector_properties {
       IGT_CONNECTOR_SCALING_MODE = 0,
       IGT_CONNECTOR_CRTC_ID,
       IGT_CONNECTOR_DPMS,
       IGT_CONNECTOR_BROADCAST_RGB,
       IGT_CONNECTOR_CONTENT_PROTECTION,
       IGT_CONNECTOR_VRR_CAPABLE,
       IGT_CONNECTOR_HDCP_CONTENT_TYPE,
       IGT_CONNECTOR_LINK_STATUS,
       IGT_CONNECTOR_MAX_BPC,
       IGT_CONNECTOR_HDR_OUTPUT_METADATA,
       IGT_CONNECTOR_WRITEBACK_PIXEL_FORMATS,
       IGT_CONNECTOR_WRITEBACK_FB_ID,
       IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR,
       IGT_CONNECTOR_DITHERING_MODE,
       IGT_NUM_CONNECTOR_PROPS
};

/**
 * igt_connector_prop_names
 *
 * igt_connector_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_connector_properties enum.
 */
extern const char * const igt_connector_prop_names[];

struct kmstest_connector_config {
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeModeInfo default_mode;

	int crtc_index;
	unsigned int valid_crtc_index_mask;
	char *connector_path;
};

/**
 * kmstest_force_connector_state:
 * @FORCE_CONNECTOR_UNSPECIFIED: Unspecified
 * @FORCE_CONNECTOR_ON: On
 * @FORCE_CONNECTOR_DIGITAL: Digital
 * @FORCE_CONNECTOR_OFF: Off
 */
enum kmstest_force_connector_state {
	FORCE_CONNECTOR_UNSPECIFIED,
	FORCE_CONNECTOR_ON,
	FORCE_CONNECTOR_DIGITAL,
	FORCE_CONNECTOR_OFF
};

/**
 * intel_broadcast_rgb_mode:
 * @BROADCAST_RGB_AUTO: Choose the color range to use automatically
 * @BROADCAST_RGB_FULL: Force the connector to use full color range
 * @BROADCAST_RGB_16_235: Force the connector to use a limited 16:235 color
 * range
 */
enum intel_broadcast_rgb_mode {
	BROADCAST_RGB_AUTO = 0,
	BROADCAST_RGB_FULL,
	BROADCAST_RGB_16_235
};

struct edid;

/**
 * joined_pipes:
 * @JOINED_PIPES_DEFAULT: Default setting with no force joiner
 * @JOINED_PIPES_NONE: Force to exactly one pipe
 * @JOINED_PIPES_BIG_JOINER: Join two pipes big joiner
 * @JOINED_PIPES_ULTRA_JOINER: Join four pipes for ultra joiner
 */
enum joined_pipes {
	JOINED_PIPES_DEFAULT,
	JOINED_PIPES_NONE,
	JOINED_PIPES_BIG_JOINER,
	JOINED_PIPES_ULTRA_JOINER = 4
};

uint64_t igt_kms_frame_time_from_vrefresh(uint32_t vrefresh);

void dump_connector_attrs(void);
bool kmstest_force_connector(int fd, drmModeConnector *connector,
			     enum kmstest_force_connector_state state);
bool kmstest_force_connector_joiner(int drm_fd, drmModeConnector *connector, int joined_pipes);
void kmstest_force_edid(int drm_fd, drmModeConnector *connector,
			const struct edid *edid);

bool kmstest_get_connector_default_mode(int drm_fd, drmModeConnector *connector,
					drmModeModeInfo *mode);
bool kmstest_get_connector_config(int drm_fd, uint32_t connector_id,
				  unsigned long crtc_idx_mask,
				  struct kmstest_connector_config *config);
drmModePropertyBlobPtr kmstest_get_path_blob(int drm_fd, uint32_t connector_id);
bool kmstest_probe_connector_config(int drm_fd, uint32_t connector_id,
				    unsigned long crtc_idx_mask,
				    struct kmstest_connector_config *config);
void kmstest_free_connector_config(struct kmstest_connector_config *config);

void kmstest_set_connector_dpms(int fd, drmModeConnector *connector, int mode);
bool kmstest_get_property(int drm_fd, uint32_t object_id, uint32_t object_type,
			  const char *name, uint32_t *prop_id, uint64_t *value,
			  drmModePropertyPtr *prop);
void kmstest_unset_all_crtcs(int drm_fd, drmModeResPtr resources);
int kmstest_get_crtc_idx(drmModeRes *res, uint32_t crtc_id);
uint32_t kmstest_find_crtc_for_connector(int fd, drmModeRes *res,
					 drmModeConnector *connector,
					 uint32_t crtc_blacklist_idx_mask);

uint32_t kmstest_dumb_create(int fd, int width, int height, int bpp,
			     unsigned *stride, uint64_t *size);

void *kmstest_dumb_map_buffer(int fd, uint32_t handle, uint64_t size,
			      unsigned prot);
void kmstest_dumb_destroy(int fd, uint32_t handle);
void kmstest_wait_for_pageflip(int fd);
void kmstest_wait_for_pageflip_timeout(int fd, uint64_t timeout_us);
unsigned int kmstest_get_vblank(int fd, int crtc_index, unsigned int flags);

bool kms_has_vblank(int fd);

/*
 * A small modeset API
 */

/* High-level kms api with igt_ prefix */

/**
 * igt_commit_style:
 * @COMMIT_LEGACY: Changes will be committed using the legacy API.
 * @COMMIT_UNIVERSAL: Changes will be committed with the universal plane API, no modesets are allowed.
 * @COMMIT_ATOMIC: Changes will be committed using the atomic API.
 */
enum igt_commit_style {
	COMMIT_LEGACY = 0,
	COMMIT_UNIVERSAL,
	COMMIT_ATOMIC,
};

enum igt_atomic_plane_properties {
       IGT_PLANE_SRC_X = 0,
       IGT_PLANE_SRC_Y,
       IGT_PLANE_SRC_W,
       IGT_PLANE_SRC_H,

       IGT_PLANE_CRTC_X,
       IGT_PLANE_CRTC_Y,
       IGT_PLANE_CRTC_W,
       IGT_PLANE_CRTC_H,

/* Append new properties after IGT_PLANE_COORD_CHANGED_MASK */
#define IGT_PLANE_COORD_CHANGED_MASK 0xff

       IGT_PLANE_FB_ID,
       IGT_PLANE_CRTC_ID,
       IGT_PLANE_IN_FENCE_FD,
       IGT_PLANE_TYPE,
       IGT_PLANE_ROTATION,
       IGT_PLANE_IN_FORMATS,
       IGT_PLANE_COLOR_ENCODING,
       IGT_PLANE_COLOR_RANGE,
       IGT_PLANE_PIXEL_BLEND_MODE,
       IGT_PLANE_ALPHA,
       IGT_PLANE_ZPOS,
       IGT_PLANE_FB_DAMAGE_CLIPS,
       IGT_PLANE_SCALING_FILTER,
       IGT_PLANE_HOTSPOT_X,
       IGT_PLANE_HOTSPOT_Y,
       IGT_PLANE_SIZE_HINTS,
       IGT_PLANE_IN_FORMATS_ASYNC,
       IGT_PLANE_COLOR_PIPELINE,
       IGT_NUM_PLANE_PROPS,
};

enum igt_atomic_colorop_properties {
	IGT_COLOROP_TYPE,
	IGT_COLOROP_BYPASS,
	IGT_COLOROP_CURVE_1D_TYPE,
	IGT_COLOROP_SIZE,
	IGT_COLOROP_DATA,
	IGT_COLOROP_MULTIPLIER,
	IGT_COLOROP_LUT3D_INTERPOLATION,
	IGT_COLOROP_FIXED_MATRIX_TYPE,
	IGT_COLOROP_NEXT,
	IGT_NUM_COLOROP_PROPS
};

/**
 * igt_plane_prop_names
 *
 * igt_plane_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_plane_properties enum.
 */
extern const char * const igt_plane_prop_names[];

/**
 * igt_colorop_prop_names
 *
 * igt_colorop_prop_names contains a list of colorop property names,
 * as indexed by the igt_atomic_colorop_properties enum.
 */
extern const char * const igt_colorop_prop_names[];

typedef struct _igt_display igt_display_t;
typedef struct _igt_crtc igt_crtc_t;
typedef uint32_t igt_fixed_t;			/* 16.16 fixed point */

#define IGT_NUM_PLANE_COLOR_PIPELINES 4

typedef enum {
	/* this maps to the kernel API */
	IGT_ROTATION_0   = 1 << 0,
	IGT_ROTATION_90  = 1 << 1,
	IGT_ROTATION_180 = 1 << 2,
	IGT_ROTATION_270 = 1 << 3,
	IGT_REFLECT_X    = 1 << 4,
	IGT_REFLECT_Y    = 1 << 5,
} igt_rotation_t;

#define IGT_ROTATION_MASK \
	(IGT_ROTATION_0 | IGT_ROTATION_90 | IGT_ROTATION_180 | IGT_ROTATION_270)

/**
 * igt_rotation_90_or_270:
 * @rotation: Target rotation
 *
 * Returns: True if the given @rotation contains 90 or 270 degrees,
 * else False.
 */
static inline bool igt_rotation_90_or_270(igt_rotation_t rotation)
{
	return rotation & (IGT_ROTATION_90 | IGT_ROTATION_270);
}

typedef struct _igt_plane igt_plane_t;

typedef struct {
	uint32_t id;
	igt_plane_t *plane;

	char name[DRM_PROP_NAME_LEN];

	uint64_t changed;
	uint32_t props[IGT_NUM_COLOROP_PROPS];
	uint64_t values[IGT_NUM_COLOROP_PROPS];

} igt_colorop_t;

struct igt_format_mods {
	uint64_t *modifiers;
	uint32_t *formats;
	int count;
};

typedef struct _igt_plane {
	/*< private >*/
	igt_crtc_t *crtc;
	igt_plane_t *ref;
	int index;
	/* capabilities */
	int type;

	/*
	 * drm_plane can be NULL for primary and cursor planes (when not
	 * using the atomic modeset API)
	 */
	drmModePlane *drm_plane;

	/* gem handle for fb */
	uint32_t gem_handle;

	struct {
		uint64_t values[IGT_NUM_COLOR_ENCODINGS];
	} color_encoding;
	struct {
		uint64_t values[IGT_NUM_COLOR_RANGES];
	} color_range;

	igt_rotation_t rotations;

	uint64_t changed;
	uint32_t props[IGT_NUM_PLANE_PROPS];
	uint64_t values[IGT_NUM_PLANE_PROPS];

	struct igt_format_mods format_mods;
	struct igt_format_mods format_mods_async;

	igt_colorop_t *color_pipelines[IGT_NUM_PLANE_COLOR_PIPELINES];
	int num_color_pipelines;

	igt_colorop_t *assigned_color_pipeline;
} igt_plane_t;

/*
 * This struct represents a hardware pipe
 */
struct _igt_crtc {
	igt_display_t *display;
	/* ID of a hardware pipe. This may be vendor specific. Use crtc_index instead. */
	enum hardware_pipe hardware_pipe;

	int n_planes;
	int num_primary_planes;
	igt_plane_t *planes;

	uint64_t changed;
	uint32_t props[IGT_NUM_CRTC_PROPS];
	uint64_t values[IGT_NUM_CRTC_PROPS];

	/* ID of KMS CRTC object */
	uint32_t crtc_id;
	/* CRTC index in drmModeRes.crtcs */
	uint32_t crtc_index;

	int32_t out_fence_fd;
};

typedef struct {
	/*< private >*/
	igt_display_t *display;
	uint32_t id;					/* KMS id */
	struct kmstest_connector_config config;
	char *name;
	bool force_reprobe;
	igt_crtc_t *pending_crtc;
	bool use_override_mode;
	drmModeModeInfo override_mode;

	int32_t writeback_out_fence_fd;

	/* bitmask of changed properties */
	uint64_t changed;

	uint32_t props[IGT_NUM_CONNECTOR_PROPS];
	uint64_t values[IGT_NUM_CONNECTOR_PROPS];
} igt_output_t;

typedef struct {
	igt_output_t *output;
	igt_crtc_t *crtc;
} igt_output_crtc_t;

struct _igt_display {
	int drm_fd;
	int log_shift;
	int n_crtcs;
	int n_planes;
	int n_colorops;
	int n_outputs;
	igt_output_t *outputs;
	igt_plane_t *planes;
	igt_colorop_t *colorops;
	igt_crtc_t *crtcs;
	bool has_cursor_plane;
	bool is_atomic;
	bool has_virt_cursor_plane;
	bool has_plane_color_pipeline;
	bool first_commit;

	struct igt_format_mods format_mods;
	struct igt_format_mods format_mods_async;
};

typedef struct {
	int tile_group_id;
	bool tile_is_single_monitor;
	uint8_t num_h_tile, num_v_tile;
	uint8_t tile_h_loc, tile_v_loc;
	uint16_t tile_h_size, tile_v_size;
} igt_tile_info_t;

/* Backlight context*/
typedef struct {
	int max;
	int old;
	int color_flag;
	struct igt_fb green;
	struct igt_fb red;
	igt_output_t *output;
	char path[PATH_MAX];
	char backlight_dir_path[PATH_MAX];
} igt_backlight_context_t;

void igt_display_reset_outputs(igt_display_t *display);
void igt_display_require(igt_display_t *display, int drm_fd);
void igt_display_fini(igt_display_t *display);
void igt_display_reset(igt_display_t *display);
int  igt_display_commit2(igt_display_t *display, enum igt_commit_style s);
int  igt_display_commit(igt_display_t *display);
int  igt_display_try_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data);
void igt_display_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data);
int  igt_display_try_commit2(igt_display_t *display, enum igt_commit_style s);
int  igt_display_drop_events(igt_display_t *display);
void igt_display_require_output(igt_display_t *display);
void igt_display_require_output_on_crtc(igt_crtc_t *crtc);
int igt_display_n_crtcs(igt_display_t *display);

const char *igt_crtc_name(igt_crtc_t *crtc);

igt_crtc_t *igt_crtc_for_crtc_id(igt_display_t *display, uint32_t crtc_id);
igt_crtc_t *igt_crtc_for_crtc_index(igt_display_t *display, int crtc_index);
igt_crtc_t *igt_crtc_for_pipe(igt_display_t *display, enum hardware_pipe pipe);
igt_crtc_t *igt_first_crtc(igt_display_t *display);
igt_crtc_t *igt_first_crtc_with_single_output(igt_display_t *display, igt_output_t **ret_output);
igt_crtc_t *igt_next_crtc(igt_display_t *display, igt_crtc_t *crtc);
igt_crtc_t *igt_random_crtc(igt_display_t *display);

uint32_t igt_crtc_get_vbl_flag(igt_crtc_t *crtc);
unsigned int igt_crtc_get_vblank(igt_crtc_t *crtc, unsigned int flags);

typedef struct _igt_pipe_crc igt_pipe_crc_t;
igt_pipe_crc_t *igt_crtc_crc_new(igt_crtc_t *crtc, const char *source);
igt_pipe_crc_t *igt_crtc_crc_new_nonblock(igt_crtc_t *crtc, const char *source);

int igt_crtc_debugfs_dir(igt_crtc_t *crtc);

igt_crtc_t *igt_output_get_driving_crtc(igt_output_t *output);
const char *igt_output_name(const igt_output_t *output);
bool kmstest_mode_is_valid(const drmModeModeInfo *mode);
drmModeModeInfo *igt_output_get_mode(igt_output_t *output);
drmModeModeInfo *igt_output_get_highres_mode(igt_output_t *output);
drmModeModeInfo *igt_output_get_lowres_mode(igt_output_t *output);
void igt_output_override_mode(igt_output_t *output, const drmModeModeInfo *mode);
int igt_output_preferred_vrefresh(igt_output_t *output);
void igt_output_set_crtc(igt_output_t *output, igt_crtc_t *crtc);
igt_plane_t *igt_output_get_plane(igt_output_t *output, int plane_idx);
igt_plane_t *igt_output_get_plane_type(igt_output_t *output, int plane_type);
int igt_output_count_plane_type(igt_output_t *output, int plane_type);
igt_plane_t *igt_output_get_plane_type_index(igt_output_t *output,
					     int plane_type, int index);
igt_output_t *igt_output_from_connector(igt_display_t *display,
    drmModeConnector *connector);
void igt_output_refresh(igt_output_t *output);
drmModeModeInfo *igt_std_1024_mode_get(int vrefresh);
void igt_output_set_writeback_fb(igt_output_t *output, struct igt_fb *fb);
void igt_modeset_disable_all_outputs(igt_display_t *display);

igt_plane_t *igt_crtc_get_plane_type(igt_crtc_t *crtc, int plane_type);
int igt_crtc_count_plane_type(igt_crtc_t *crtc, int plane_type);
igt_plane_t *igt_crtc_get_plane_type_index(igt_crtc_t *crtc, int plane_type,
					   int index);
bool output_is_internal_panel(igt_output_t *output);
igt_output_t *igt_get_single_output_for_crtc(igt_crtc_t *crtc);

void igt_crtc_request_out_fence(igt_crtc_t *crtc);

void igt_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb);
void igt_plane_set_fence_fd(igt_plane_t *plane, int fence_fd);
void igt_plane_set_crtc(igt_plane_t *plane, igt_crtc_t *crtc);
void igt_plane_set_position(igt_plane_t *plane, int x, int y);
void igt_plane_set_size(igt_plane_t *plane, int w, int h);
void igt_plane_set_rotation(igt_plane_t *plane, igt_rotation_t rotation);
void igt_fb_set_position(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t x, uint32_t y);
void igt_fb_set_size(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t w, uint32_t h);

/**
 * igt_plane_has_rotation:
 * @plane: Plane pointer for which rotation is to be queried
 * @rotation: Plane rotation value (0, 90, 180, 270)
 *
 * Check whether @plane potentially supports the given @rotation.
 * Note that @rotation may still rejected later due to other
 * constraints (eg. incompatible pixel format or modifier).
 */
static inline bool igt_plane_has_rotation(igt_plane_t *plane, igt_rotation_t rotation)
{
	return (plane->rotations & rotation) == rotation;
}
const char *igt_plane_rotation_name(igt_rotation_t rotation);

void igt_wait_for_vblank(igt_crtc_t *crtc);
void igt_wait_for_vblank_count(igt_crtc_t *crtc, int count);
void igt_wait_for_vblank_ts_seq(igt_crtc_t *crtc, uint64_t *ts_ns, unsigned int *seq);

/**
 * igt_output_is_connected:
 * @output: #igt_output_t to check.
 *
 * Returns: True if given @output's connection status is CONNECTED,
 * else False.
 */
static inline bool igt_output_is_connected(igt_output_t *output)
{
	/* Something went wrong during probe? */
	if (!output->config.connector ||
	    !output->config.connector->count_modes)
		return false;

	if (output->config.connector->connection == DRM_MODE_CONNECTED)
		return true;

	return false;
}

/**
 * igt_output_matches_connector_filter:
 * @output: #igt_output_t to check.
 *
 * Checks whether the given @output passes the global connector filter set via
 * the '--connector' command line option (or the 'IGT_CONNECTOR' environment
 * variable). The match is done with strstr(), so e.g. "HDMI" matches all HDMI
 * connectors.
 *
 * Returns: True if no filter is set or the @output's name contains the filter
 * string, otherwise False.
 */
static inline bool igt_output_matches_connector_filter(igt_output_t *output)
{
	if (!igt_connector_filter)
		return true;

	return output->name &&
	       strstr(output->name, igt_connector_filter);
}

/**
 * igt_crtc_connector_valid:
 * @crtc: CRTC to check.
 * @output: #igt_output_t to check.
 *
 * Checks whether the given CRTC and output can be used together.
 */
static inline bool igt_crtc_connector_valid(igt_crtc_t *crtc, igt_output_t *output)
{
	return igt_output_is_connected(output) &&
		igt_output_matches_connector_filter(output) &&
		output->config.valid_crtc_index_mask & (1 << crtc->crtc_index);
}

#define for_each_if(condition) if (!(condition)) {} else

/**
 * for_each_output:
 * @display: a pointer to an #igt_display_t structure
 * @output: The output to iterate.
 *
 * This for loop iterates over all outputs.
 */
#define for_each_output(display, output) \
	for ((output) = &(display)->outputs[0]; \
	     (output) < &(display)->outputs[(display)->n_outputs]; \
	     (output)++)

/**
 * for_each_connected_output:
 * @display: a pointer to an #igt_display_t structure
 * @output: The output to iterate.
 *
 * This for loop iterates over all connected outputs.
 */
#define for_each_connected_output(display, output)		\
	for_each_output((display), (output))	\
		for_each_if ((igt_output_is_connected((output))) && \
			     igt_output_matches_connector_filter((output)))

/**
 * for_each_disconnected_output:
 * @display: a pointer to an #igt_display_t structure
 * @output: The output to iterate.
 *
 * This for loop iterates over all disconnected outputs.
 */
#define for_each_disconnected_output(display, output)		\
	for_each_output((display), (output))	\
		for_each_if ((!(igt_output_is_connected((output)))))

/**
 * for_each_crtc:
 * @display: a pointer to an #igt_display_t structure
 * @crtc: The CRTC
 *
 * This for loop iterates over all CRTCs.
 *
 * Note that this cannot be used to enumerate per-CRTC subtest names since it
 * depends upon runtime probing of the actual kms driver that is being tested.
 * Use #for_each_pipe_static instead.
 */
#define for_each_crtc(display, crtc) \
	for ((crtc) = &(display)->crtcs[0]; \
	     (crtc) < &(display)->crtcs[(display)->n_crtcs]; \
	     (crtc)++)

/**
 * for_each_crtc_with_valid_output:
 * @display: a pointer to an #igt_display_t structure
 * @crtc: CRTC for which this @crtc / @output combination is valid.
 * @output: The output for which this @crtc / @output combination is valid.
 *
 * This for loop is called over all connected outputs. This function
 * will try every combination of @crtc and @output.
 *
 * If you only need to test a single output for each pipe, use
 * for_each_crtc_with_single_output(), if you only need an
 * output for a single CRTC, use igt_get_single_output_for_crtc().
 */
#define for_each_crtc_with_valid_output(display, crtc, output) \
	for ((output) = &(display)->outputs[0], (crtc) = &(display)->crtcs[0]; \
	     assert(igt_can_fail()), (crtc) < &(display)->crtcs[(display)->n_crtcs] && \
		     (output) < &(display)->outputs[(display)->n_outputs]; \
	     (output) = (output) + 1 < &(display)->outputs[(display)->n_outputs] ? \
		     (output) + 1 : ((crtc)++, &(display)->outputs[0])) \
		for_each_if (igt_crtc_connector_valid((crtc), (output)))

igt_output_crtc_t *__igt_output_crtc_populate(igt_display_t *display,
					      igt_output_crtc_t *chosen_outputs);

/**
 * for_each_crtc_with_single_output:
 * @display: a pointer to an #igt_display_t structure
 * @_crtc: The CRTC for which this @_crtc / @_output combination is valid.
 * @_output: The output for which this @_crtc / @_output combination is valid.
 *
 * This loop is called over all CRTCs, and will try to find a compatible output
 * for each CRTC. Unlike for_each_pipe_with_valid_output(), this function will
 * be called at most once for each pipe.
 */
#define for_each_crtc_with_single_output(display, _crtc, _output) \
	for (igt_output_crtc_t __outputs[igt_display_n_crtcs(display)], \
	     *__output = __igt_output_crtc_populate((display), __outputs); \
		 __output < &__outputs[igt_display_n_crtcs(display)]; __output++) \
		for_each_if (__output->output && __output->crtc && \
			     ((_output) = __output->output, (_crtc) = __output->crtc, 1))

/**
 * for_each_valid_output_on_crtc:
 * @display: a pointer to an #igt_display_t structure
 * @crtc: CRTC to enumerate valid outputs over
 * @output: The enumerated output.
 *
 * This for loop is called over all connected @output that can be used
 * on this @crtc . If there are no valid outputs for this CRTC, nothing
 * happens.
 */
#define for_each_valid_output_on_crtc(display, crtc, output) \
	for_each_connected_output((display), (output)) \
		for_each_if (igt_crtc_connector_valid((crtc), (output)))

/**
 * for_each_plane_on_crtc:
 * @crtc: CRTC to enumerate valid outputs over
 * @plane: The enumerated plane.
 *
 * This for loop iterates over all planes associated to the given @crtc.
 * If there are no valid planes for this CRTC, nothing happens.
 */
#define for_each_plane_on_crtc(crtc, plane) \
	for (int j__ = 0; assert(igt_can_fail()), \
		     (plane) = &(crtc)->planes[j__], \
		     j__ < (crtc)->n_planes; j__++)

/**
 * for_each_connector_mode:
 * @output: Output to enumerate available modes.
 * @mode: The enumerated mode.
 *
 * This for loop iterates over all modes associated to the given @output.
 * If there are no mode available for this output, nothing happens.
 */
#define for_each_connector_mode(output, mode) \
	for ((mode) = &(output)->config.connector->modes[0]; \
	     (mode) < &(output)->config.connector->modes[(output)->config.connector->count_modes]; \
	     (mode)++)

/**
 * for_each_plane_format:
 * @plane: a pointer to an #igt_plane_t structure
 * @format: format iterator
 *
 * This for loop iterates over all formats supported by @plane.
 */
#define for_each_plane_format(plane, format) \
	for (int __i = 0; __i < (plane)->drm_plane->count_formats; __i++) \
		for_each_if (((format) = (plane)->drm_plane->formats[__i], \
			      1))

/**
 * for_each_format_and_modifier:
 * @format_mods: a pointer to an #igt_format_mod structure
 * @format: format iterator
 * @modifier: modifier iterator
 *
 * This for loop iterates over all format+modifier combinations.
 */
#define for_each_format_and_modifier(format_mods, format, modifier) \
	for (int igt_unique(__i) = 0; igt_unique(__i) < (format_mods)->count; igt_unique(__i)++) \
		for_each_if ((assert_type((format), uint32_t), \
			      assert_type((modifier), uint64_t), \
			      (format) = (format_mods)->formats[igt_unique(__i)], \
			      (modifier) = (format_mods)->modifiers[igt_unique(__i)], \
			      1))

/**
 * for_each_format_with_modifier:
 * @format_mods: a pointer to an #igt_format_mod structure
 * @modifier: specified modifier
 * @format: format iterator
 *
 * This for loop iterates over all formats supporting @modifier.
 */
#define for_each_format_with_modifier(format_mods, format, modifier) \
	for (int igt_unique(__i) = 0; igt_unique(__i) < (format_mods)->count; igt_unique(__i)++) \
		for_each_if ((assert_type((format), uint32_t), \
			      (format) = (format_mods)->formats[igt_unique(__i)], \
			      (modifier) == (format_mods)->modifiers[igt_unique(__i)]))

/**
 * for_each_modifier_with_format:
 * @format_mods: a pointer to an #igt_format_mod structure
 * @format: specified format
 * @modifier: modifier iterator
 *
 * This for loop iterates over all modifiers supporting @format.
 */
#define for_each_modifier_with_format(format_mods, format, modifier) \
	for (int igt_unique(__i) = 0; igt_unique(__i) < (format_mods)->count; igt_unique(__i)++) \
		for_each_if ((assert_type((modifier), uint64_t), \
			      (modifier) = (format_mods)->modifiers[igt_unique(__i)], \
			      (format) == (format_mods)->formats[igt_unique(__i)]))

#define IGT_FIXED(i,f)	((i) << 16 | (f))

/**
 * igt_plane_has_prop:
 * @plane: Plane to check.
 * @prop: Property to check.
 *
 * Check whether plane supports a given property.
 *
 * Returns: True if the property is supported, otherwise false.
 */
static inline bool
igt_plane_has_prop(igt_plane_t *plane, enum igt_atomic_plane_properties prop)
{
	return plane->props[prop];
}

uint64_t igt_plane_get_prop(igt_plane_t *plane, enum igt_atomic_plane_properties prop);

/**
 * igt_plane_is_prop_changed:
 * @plane: Plane to check.
 * @prop: Property to check.
 *
 * Check whether a given @prop changed for the @plane.
 */
static inline bool igt_plane_is_prop_changed(igt_plane_t *plane,
					     enum igt_atomic_plane_properties prop)
{
	return plane->changed & (1 << prop);
}

/**
 * igt_plane_set_prop_changed:
 * @plane: Plane to check.
 * @prop: Property to check.
 *
 * Sets the given @prop for the @plane.
 */
static inline void igt_plane_set_prop_changed(igt_plane_t *plane,
					      enum igt_atomic_plane_properties prop)
{
	plane->changed |= 1 << prop;
}

/**
 * igt_plane_clear_prop_changed:
 * @plane: Plane to check.
 * @prop: Property to check.
 *
 * Clears the given @prop for the @plane.
 */
static inline void igt_plane_clear_prop_changed(igt_plane_t *plane,
						enum igt_atomic_plane_properties prop)
{
	plane->changed &= ~(1 << prop);
}

/**
 * igt_plane_set_prop_value:
 * @plane: Plane to check.
 * @prop: Property to check.
 * @value: Value to set.
 *
 * Sets the given @prop with the @value for the @plane.
 */
static inline void igt_plane_set_prop_value(igt_plane_t *plane,
					    enum igt_atomic_plane_properties prop,
					    uint64_t value)
{
	plane->values[prop] = value;
	igt_plane_set_prop_changed(plane, prop);
}

extern bool igt_plane_try_prop_enum(igt_plane_t *plane,
				    enum igt_atomic_plane_properties prop,
				    const char *val);

extern void igt_plane_set_prop_enum(igt_plane_t *plane,
				    enum igt_atomic_plane_properties prop,
				    const char *val);

extern void igt_plane_replace_prop_blob(igt_plane_t *plane,
					enum igt_atomic_plane_properties prop,
					const void *ptr, size_t length);

extern bool igt_plane_is_valid_colorop(igt_plane_t *plane, igt_colorop_t *colorop);

extern void igt_plane_set_color_pipeline(igt_plane_t *plane, igt_colorop_t *colorop);


/**
 * igt_colorop_has_prop:
 * @colorop: colorop to check.
 * @prop: Property to check.
 *
 * Check whether colorop supports a given property.
 *
 * Returns: True if the property is supported, otherwise false.
 */
static inline bool
igt_colorop_has_prop(igt_colorop_t *colorop, enum igt_atomic_colorop_properties prop)
{
	return colorop->props[prop];
}

uint64_t igt_colorop_get_prop(igt_display_t *display, igt_colorop_t *colorop, enum igt_atomic_colorop_properties prop);

#define igt_colorop_is_prop_changed(colorop, prop) \
	(!!((colorop)->changed & (1 << (prop))))

#define igt_colorop_set_prop_changed(colorop, prop) \
	((colorop)->changed |= 1 << (prop))

#define igt_colorop_clear_prop_changed(colorop, prop) \
	((colorop)->changed &= ~(1 << (prop)))

#define igt_colorop_set_prop_value(colorop, prop, value) \
	do { \
		colorop->values[prop] = value; \
		igt_colorop_set_prop_changed(colorop, prop); \
	} while (0)


extern bool igt_colorop_has_prop_enum_value(igt_colorop_t *colorop,
					    enum igt_atomic_colorop_properties prop,
					    const char *val);

extern bool igt_colorop_try_prop_enum(igt_colorop_t *colorop,
				      enum igt_atomic_colorop_properties prop,
				      const char *val);

extern void igt_colorop_set_prop_enum(igt_colorop_t *colorop,
				      enum igt_atomic_colorop_properties prop,
				      const char *val);

extern void igt_colorop_replace_prop_blob(igt_colorop_t *colorop,
					  enum igt_atomic_colorop_properties prop,
					  const void *ptr, size_t length);

extern bool igt_plane_check_prop_is_mutable(igt_plane_t *plane,
					    enum igt_atomic_plane_properties igt_prop);
/**
 * igt_output_has_prop:
 * @output: Output to check.
 * @prop: Property to check.
 *
 * Check whether output supports a given property.
 *
 * Returns: True if the property is supported, otherwise false.
 */
static inline bool
igt_output_has_prop(igt_output_t *output, enum igt_atomic_connector_properties prop)
{
	return output->props[prop];
}

uint64_t igt_output_get_prop(igt_output_t *output, enum igt_atomic_connector_properties prop);

/**
 * igt_output_is_prop_changed:
 * @output: Output to check.
 * @prop: Property to check.
 *
 * Check whether a given @prop changed for the @Output.
 */
static inline bool igt_output_is_prop_changed(igt_output_t *output,
					      enum igt_atomic_connector_properties prop)
{
	return output->changed & (1 << prop);
}

/**
 * igt_output_set_prop_changed:
 * @output: Output to check.
 * @prop: Property to check.
 *
 * Sets the given @prop for the @output.
 */
static inline void igt_output_set_prop_changed(igt_output_t *output,
					       enum igt_atomic_connector_properties prop)
{
	output->changed |= 1 << prop;
}

/**
 * igt_output_clear_prop_changed:
 * @output: Output to check.
 * @prop: Property to check.
 *
 * Clears the given @prop for the @output.
 */
static inline void igt_output_clear_prop_changed(igt_output_t *output,
						 enum igt_atomic_connector_properties prop)
{
	output->changed &= ~(1 << prop);
}

/**
 * igt_output_set_prop_value:
 * @output: Output to check.
 * @prop: Property to check.
 * @value: Value to set.
 *
 * Sets the given @prop with the @value for the @output.
 */
static inline void igt_output_set_prop_value(igt_output_t *output,
					     enum igt_atomic_connector_properties prop,
					     uint64_t value)
{
	output->values[prop] = value;
	igt_output_set_prop_changed(output, prop);
}

extern bool igt_output_try_prop_enum(igt_output_t *output,
				     enum igt_atomic_connector_properties prop,
				     const char *val);

extern void igt_output_set_prop_enum(igt_output_t *output,
				     enum igt_atomic_connector_properties prop,
				     const char *val);

extern void igt_output_replace_prop_blob(igt_output_t *output,
					 enum igt_atomic_connector_properties prop,
					 const void *ptr, size_t length);
/**
 * igt_crtc_has_prop:
 * @crtc: CRTC to check.
 * @prop: Property to check.
 *
 * Check whether CRTC supports a given property.
 *
 * Returns: True if the property is supported, otherwise false.
 */
static inline bool
igt_crtc_has_prop(igt_crtc_t *crtc, enum igt_atomic_crtc_properties prop)
{
	return crtc->props[prop];
}

uint64_t igt_crtc_get_prop(igt_crtc_t *crtc,
			   enum igt_atomic_crtc_properties prop);

/**
 * igt_crtc_is_prop_changed:
 * @crtc: CRTC to check.
 * @prop: Property to check.
 *
 * Check whether a given @prop changed for the @crtc.
 */
static inline bool igt_crtc_is_prop_changed(igt_crtc_t *crtc,
						enum igt_atomic_crtc_properties prop)
{
	return crtc->changed & (1 << prop);
}

/**
 * igt_crtc_set_prop_changed:
 * @crtc: CRTC to check.
 * @prop: Property to check.
 *
 * Sets the given @prop for the @crtc.
 */
static inline void igt_crtc_set_prop_changed(igt_crtc_t *crtc,
						 enum igt_atomic_crtc_properties prop)
{
	crtc->changed |= 1 << prop;
}

/**
 * igt_crtc_clear_prop_changed:
 * @crtc: CRTC to check.
 * @prop: Property to check.
 *
 * Clears the given @prop for the @crtc.
 */
static inline void igt_crtc_clear_prop_changed(igt_crtc_t *crtc,
						   enum igt_atomic_crtc_properties prop)
{
	crtc->changed &= ~(1 << prop);
}

/**
 * igt_crtc_set_prop_value:
 * @crtc: CRTC to check.
 * @prop: Property to check.
 * @value: Value to set.
 *
 * Sets the given @prop with the @value for the @crtc.
 */
static inline void igt_crtc_set_prop_value(igt_crtc_t *crtc,
					       enum igt_atomic_crtc_properties prop,
					       uint64_t value)
{
	crtc->values[prop] = value;
	igt_crtc_set_prop_changed(crtc, prop);
}

extern bool igt_crtc_try_prop_enum(igt_crtc_t *crtc,
				       enum igt_atomic_crtc_properties prop,
				       const char *val);

extern void igt_crtc_set_prop_enum(igt_crtc_t *crtc,
				       enum igt_atomic_crtc_properties prop,
				       const char *val);
extern void igt_crtc_replace_prop_blob(igt_crtc_t *crtc,
					   enum igt_atomic_crtc_properties prop,
					   const void *ptr, size_t length);
void igt_crtc_refresh(igt_crtc_t *crtc, bool force);

void igt_enable_connectors(int drm_fd);
void igt_reset_connectors(void);

uint32_t kmstest_get_vbl_flag(int crtc_index);

const struct edid *igt_kms_get_base_edid(void);
const struct edid *igt_kms_get_full_edid(void);
const struct edid *igt_kms_get_base_tile_edid(void);
const struct edid *igt_kms_get_alt_edid(void);
const struct edid *igt_kms_get_hdmi_audio_edid(void);
const struct edid *igt_kms_get_dp_audio_edid(void);
const struct edid *igt_kms_get_4k_edid(void);
const struct edid *igt_kms_get_3d_edid(void);
const struct edid *igt_kms_get_aspect_ratio_edid(void);
struct edid **igt_kms_get_tiled_edid(uint8_t htile, uint8_t vtile);
const struct edid *igt_kms_get_custom_edid(enum igt_custom_edid_type edid);
struct udev_monitor *igt_watch_uevents(void);
bool igt_hotplug_detected(struct udev_monitor *mon,
			  int timeout_secs);
bool igt_lease_change_detected(struct udev_monitor *mon,
			       int timeout_secs);
bool igt_connector_event_detected(struct udev_monitor *mon, uint32_t conn_id,
				  uint32_t prop_id, int timeout_msecs);
void igt_flush_uevents(struct udev_monitor *mon);
void igt_cleanup_uevents(struct udev_monitor *mon);
bool igt_kms_poll_save(void);
void igt_kms_poll_set(bool enable);
void igt_kms_poll_restore(void);

bool igt_format_mods_has_format_and_modifier(const struct igt_format_mods *format_mods,
					     uint32_t format, uint64_t modifier);
bool igt_format_mods_has_format(const struct igt_format_mods *format_mods,
				uint32_t format);
bool igt_format_mods_has_modifier(const struct igt_format_mods *format_mods,
				  uint64_t modifier);

bool igt_display_has_format_mod(igt_display_t *display, uint32_t format, uint64_t modifier);
bool igt_plane_has_format_mod(igt_plane_t *plane, uint32_t format, uint64_t modifier);

/**
 * igt_vblank_after_eq:
 * @a: First vblank sequence number.
 * @b: Second vblank sequence number.
 *
 * Compare vblank sequence numbers,
 * handling wraparound correctly.
 *
 * Returns: @a >= @b
 */
static inline bool igt_vblank_after_eq(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) >= 0;
}

/**
 * igt_vblank_before_eq:
 * @a: First vblank sequence number.
 * @b: Second vblank sequence number.
 *
 * Compare vblank sequence numbers,
 * handling wraparound correctly.
 *
 * Returns: @a <= @b
 */
static inline bool igt_vblank_before_eq(uint32_t a, uint32_t b)
{
	return igt_vblank_after_eq(b, a);
}

/**
 * igt_vblank_after:
 * @a: First vblank sequence number.
 * @b: Second vblank sequence number.
 *
 * Compare vblank sequence numbers,
 * handling wraparound correctly.
 *
 * Returns: @a > @b
 */
static inline bool igt_vblank_after(uint32_t a, uint32_t b)
{
	return (int32_t)(b - a) < 0;
}

/**
 * igt_vblank_before:
 * @a: First vblank sequence number.
 * @b: Second vblank sequence number.
 *
 * Compare vblank sequence numbers,
 * handling wraparound correctly.
 *
 * Returns: @a < @b
 */
static inline bool igt_vblank_before(uint32_t a, uint32_t b)
{
	return igt_vblank_after(b, a);
}

void igt_parse_connector_tile_blob(drmModePropertyBlobPtr blob,
		igt_tile_info_t *tile);

int igt_connector_sysfs_open(int drm_fd,
			     drmModeConnector *connector);
typedef bool (*igt_connector_attr_set)(int dir, const char *attr, const char *value);
bool connector_attr_set(int idx, drmModeConnector *connector,
			int dir, igt_connector_attr_set set,
			const char *attr, const char *value,
			const char *reset_value,
			bool force_reset);
uint32_t igt_reduce_format(uint32_t format);


void igt_dump_connectors_fd(int drmfd);
void igt_dump_crtcs_fd(int drmfd);
bool igt_override_all_active_output_modes_to_fit_bw(igt_display_t *display);
bool igt_fit_modes_in_bw(igt_display_t *display);
bool igt_has_lobf_debugfs(int drmfd, igt_output_t *output);
bool igt_get_i915_edp_lobf_status(int drmfd, char *connector_name);
bool igt_is_aux_less_alpm_enabled(int drmfd, char *connector_name);
bool igt_get_output_max_bpc(igt_output_t *output, unsigned int *maximum);
unsigned int igt_get_crtc_current_bpc(igt_crtc_t *crtc);
void igt_assert_output_bpc_equal(igt_crtc_t *crtc, igt_output_t *output,
				 unsigned int bpc);
bool igt_check_output_bpc_equal(igt_crtc_t *crtc, igt_output_t *output,
				unsigned int bpc);

int sort_drm_modes_by_clk_dsc(const void *a, const void *b);
int sort_drm_modes_by_clk_asc(const void *a, const void *b);
int sort_drm_modes_by_res_dsc(const void *a, const void *b);
int sort_drm_modes_by_res_asc(const void *a, const void *b);
void igt_sort_connector_modes(drmModeConnector *connector,
		int (*comparator)(const void *, const void*));

bool igt_max_bpc_constraint(igt_display_t *display, igt_crtc_t *crtc,
			    igt_output_t *output, int bpc);
int igt_get_max_dotclock(int fd);
int intel_get_max_pipe_hdisplay(int drm_fd);
int igt_get_max_cdclk(int fd);
int igt_get_current_cdclk(int fd);
bool igt_bigjoiner_possible(int drm_fd, drmModeModeInfo *mode, int max_dotclock);
bool bigjoiner_mode_found(int drm_fd, drmModeConnector *connector,
			  int max_dotclock, drmModeModeInfo *mode);
bool intel_boundary_non_joiner_mode_found(int drm_fd, drmModeConnector *connector,
					   int max_dotclock, drmModeModeInfo *mode);
bool igt_is_joiner_enabled_for_pipe(int drmfd, enum hardware_pipe pipe);
bool igt_ultrajoiner_possible(int drmfd, drmModeModeInfo *mode, int max_dotclock);
bool ultrajoiner_mode_found(int drm_fd, drmModeConnector *connector,
			  int max_dotclock, drmModeModeInfo *mode);
bool igt_has_force_joiner_debugfs(int drmfd, char *conn_name);
drmModeModeInfo *igt_get_non_joiner_mode(int drm_fd, igt_output_t *output);
bool is_joiner_mode(int drm_fd, igt_output_t *output);
bool igt_check_force_joiner_status(int drmfd, char *connector_name);
bool igt_check_bigjoiner_support(igt_display_t *display);
bool igt_parse_mode_string(const char *mode_string, drmModeModeInfo *mode);
bool intel_pipe_output_combo_valid(igt_display_t *display);
bool igt_check_output_is_dp_mst(igt_output_t *output);
int igt_get_dp_mst_connector_id(igt_output_t *output);
int igt_crtc_num_scalers(igt_crtc_t *crtc);
int igt_backlight_read(int *result, const char *fname, igt_backlight_context_t *context);
int igt_backlight_write(int value, const char *fname, igt_backlight_context_t *context);
uint32_t igt_get_connected_output_count(igt_display_t *display);

drmModePropertyBlobRes *igt_get_writeback_formats_blob(igt_output_t *output);

uint64_t igt_get_writeback_fb_id(igt_output_t *output);
void igt_detach_crtc(igt_display_t *display, igt_output_t *output);
void igt_get_and_wait_out_fence(igt_output_t *output);

igt_colorop_t *igt_find_colorop(igt_display_t *display, uint32_t id);

bool igt_wait_for_connector_status(int drm_fd, unsigned int connector_id, double timeout,
				   int drm_mode);
int igt_get_connected_connectors(int drm_fd, uint32_t **connector_ids);
drmModeConnectorPtr igt_get_connector_from_name(int drm_fd, const char *port_name);
uint32_t igt_get_connector_id_from_name(int drm_fd, const char *port_name);
uint32_t igt_get_connector_id_from_mst_path(int drm_fd, const void *mst_path);

int kms_wait_for_new_connectors(uint32_t **newly_connected,
				const uint32_t *already_connected,
				int already_connected_count, int drm_fd);
int
get_array_diff(const uint32_t *array_a, int array_a_len, const uint32_t *array_b, int array_b_len,
	       uint32_t **diff);

int igt_get_crtc_index_from_connector_id(int drm_fd, int connector_id);
igt_crtc_t *igt_get_crtc_for_output(igt_display_t *display,
				    igt_output_t *output);

#endif /* __IGT_KMS_H__ */
