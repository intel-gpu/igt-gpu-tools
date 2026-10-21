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

#include "config.h"

#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_LINUX_KD_H
#include <linux/kd.h>
#elif HAVE_SYS_KD_H
#include <sys/kd.h>
#endif

#include <libudev.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#include "linux_scaffold.h"
#include <i915_drm.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_kms.h"
#include "igt_aux.h"
#include "igt_edid.h"
#include "intel_chipset.h"
#include "igt_debugfs.h"
#include "igt_device.h"
#include "igt_pipe_crc.h"
#include "igt_rc.h"
#include "igt_sysfs.h"
#include "sw_sync.h"
#ifdef HAVE_CHAMELIUM
#include "igt_chamelium.h"
#endif
#ifdef HAVE_UNIGRAF
#include "unigraf/unigraf.h"
#endif

/**
 * SECTION:igt_kms
 * @short_description: Kernel modesetting support library
 * @title: KMS
 * @include: igt.h
 *
 * This library provides support to enumerate and set modeset configurations.
 *
 * There are two parts in this library: First the low level helper function
 * which directly build on top of raw ioctls or the interfaces provided by
 * libdrm. Those functions all have a kmstest_ prefix.
 *
 * The second part is a high-level library to manage modeset configurations
 * which abstracts away some of the low-level details like the difference
 * between legacy and universal plane support for setting cursors or in the
 * future the difference between legacy and atomic commit. These high-level
 * functions have all igt_ prefixes. This part is still very much work in
 * progress and so also lacks a bit documentation for the individual functions.
 *
 * Note that this library's header pulls in the [i-g-t framebuffer](igt-gpu-tools-i-g-t-framebuffer.html)
 * library as a dependency.
 */

#define WAIT_FOR_CONNECTOR_LOOP_SLEEP_US 10000

/* list of connectors that need resetting on exit */
#define MAX_CONNECTORS 32
#define MAX_EDID 2
#define DISPLAY_TILE_BLOCK 0x12
#define MAX_NUM_COLOROPS 256
/**
 * IGT_KMS_CONNECTOR_NAME_SIZE - Size used when a connector name is needed
 */
#define IGT_KMS_CONNECTOR_NAME_SIZE 50

struct igt_connector_attr {
	uint32_t connector_type;
	uint32_t connector_type_id;
	int idx;
	int dir;
	igt_connector_attr_set set;
	const char *attr, *value, *reset_value;
};

static struct igt_connector_attr connector_attrs[MAX_CONNECTORS];

/**
 * igt_kms_get_base_edid:
 *
 * Get the base edid block, which includes the following modes:
 *
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * Returns: A basic edid block
 */
const struct edid *igt_kms_get_base_edid(void)
{
	static struct edid edid;
	drmModeModeInfo mode = {};

	mode.clock = 148500;
	mode.hdisplay = 1920;
	mode.hsync_start = 2008;
	mode.hsync_end = 2052;
	mode.htotal = 2200;
	mode.vdisplay = 1080;
	mode.vsync_start = 1084;
	mode.vsync_end = 1089;
	mode.vtotal = 1125;
	mode.vrefresh = 60;

	edid_init_with_mode(&edid, &mode);
	edid_update_checksum(&edid);

	return &edid;
}

/**
 * igt_kms_get_full_edid:
 *
 * Get the full edid block, which includes the following modes:
 *
 *  - 2288x1287 144Hz
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * Returns: A full edid block
 */
const struct edid *igt_kms_get_full_edid(void)
{
	static struct edid edid;
	drmModeModeInfo mode = {};

	/* 2288x1287 @ 60Hz with proper timing values */
	mode.clock = 194250;
	mode.hdisplay = 2288;
	mode.hsync_start = 2336;
	mode.hsync_end = 2368;
	mode.htotal = 2448;
	mode.vdisplay = 1287;
	mode.vsync_start = 1290;
	mode.vsync_end = 1295;
	mode.vtotal = 1324;
	mode.vrefresh = 60;
	edid_init_with_mode(&edid, &mode);

	std_timing_set(&edid.standard_timings[0], 256, 60, STD_TIMING_16_10);
	std_timing_set(&edid.standard_timings[1], 510, 69, STD_TIMING_4_3);
	std_timing_set(&edid.standard_timings[2], 764, 78, STD_TIMING_5_4);
	std_timing_set(&edid.standard_timings[3], 1018, 87, STD_TIMING_16_9);
	std_timing_set(&edid.standard_timings[4], 1526, 96, STD_TIMING_16_10);
	std_timing_set(&edid.standard_timings[5], 1780, 105, STD_TIMING_4_3);
	std_timing_set(&edid.standard_timings[6], 2034, 114, STD_TIMING_5_4);
	std_timing_set(&edid.standard_timings[7], 2288, 123, STD_TIMING_16_9);

	edid_update_checksum(&edid);
	return &edid;
}

/**
 * igt_kms_get_base_tile_edid:
 *
 * Get the base tile edid block, which includes the following modes:
 *
 *  - 1920x2160 60Hz
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * Returns: A basic tile edid block
 */
const struct edid *igt_kms_get_base_tile_edid(void)
{
	static struct edid edid;
	drmModeModeInfo mode = {};

	mode.clock = 277250;
	mode.hdisplay = 1920;
	mode.hsync_start = 1968;
	mode.hsync_end = 2000;
	mode.htotal = 2080;
	mode.vdisplay = 2160;
	mode.vsync_start = 2163;
	mode.vsync_end = 2173;
	mode.vtotal = 2222;
	mode.vrefresh = 60;
	edid_init_with_mode(&edid, &mode);
	edid_update_checksum(&edid);
	return &edid;
}

/**
 * igt_kms_get_alt_edid:
 *
 * Get an alternate edid block, which includes the following modes:
 *
 *  - 1400x1050 60Hz
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * Returns: An alternate edid block
 */
const struct edid *igt_kms_get_alt_edid(void)
{
	static struct edid edid;
	drmModeModeInfo mode = {};

	mode.clock = 101000;
	mode.hdisplay = 1400;
	mode.hsync_start = 1448;
	mode.hsync_end = 1480;
	mode.htotal = 1560;
	mode.vdisplay = 1050;
	mode.vsync_start = 1053;
	mode.vsync_end = 1057;
	mode.vtotal = 1080;
	mode.vrefresh = 60;

	edid_init_with_mode(&edid, &mode);
	edid_update_checksum(&edid);

	return &edid;
}

/**
 * igt_kms_frame_time_from_vrefresh:
 * @vrefresh: vertical refresh rate in 1/s units.
 *
 * Returns the frame time in nanoseconds for the given vrefresh rate.
 */
uint64_t igt_kms_frame_time_from_vrefresh(uint32_t vrefresh)
{
	return vrefresh ? (NSEC_PER_SEC / vrefresh) : 0;
}

#define AUDIO_EDID_SIZE (2 * EDID_BLOCK_SIZE)

static const struct edid *
generate_audio_edid(unsigned char raw_edid[static AUDIO_EDID_SIZE],
		    bool with_vsdb, struct cea_sad *sad,
		    struct cea_speaker_alloc *speaker_alloc)
{
	struct edid *edid;
	struct edid_ext *edid_ext;
	struct edid_cea *edid_cea;
	char *cea_data;
	struct edid_cea_data_block *block;
	const struct cea_vsdb *vsdb;
	size_t cea_data_size, vsdb_size;

	/* Create a new EDID from the base IGT EDID, and add an
	 * extension that advertises audio support. */
	edid = (struct edid *) raw_edid;
	memcpy(edid, igt_kms_get_base_edid(), sizeof(struct edid));
	edid->extensions_len = 1;
	edid_ext = &edid->extensions[0];
	edid_cea = &edid_ext->data.cea;
	cea_data = edid_cea->data;
	cea_data_size = 0;

	/* Short Audio Descriptor block */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_sad(block, sad, 1);

	/* A Vendor Specific Data block is needed for HDMI audio */
	if (with_vsdb) {
		block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
		vsdb = cea_vsdb_get_hdmi_default(&vsdb_size);
		cea_data_size += edid_cea_data_block_set_vsdb(block, vsdb,
							      vsdb_size);
	}

	/* Speaker Allocation Data block */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_speaker_alloc(block,
							       speaker_alloc);

	assert(cea_data_size <= sizeof(edid_cea->data));

	edid_ext_set_cea(edid_ext, cea_data_size, 0, EDID_CEA_BASIC_AUDIO);

	edid_update_checksum(edid);

	return edid;
}

/**
 * igt_kms_get_hdmi_audio_edid:
 *
 * Get a basic edid block, which includes the HDMI Audio
 *
 * Returns: A basic HDMI Audio edid block
 */
const struct edid *igt_kms_get_hdmi_audio_edid(void)
{
	int channels;
	uint8_t sampling_rates, sample_sizes;
	static unsigned char raw_edid[AUDIO_EDID_SIZE] = {0};
	struct cea_sad sad = {0};
	struct cea_speaker_alloc speaker_alloc = {0};

	/* Initialize the Short Audio Descriptor for PCM */
	channels = 2;
	sampling_rates = CEA_SAD_SAMPLING_RATE_32KHZ |
			 CEA_SAD_SAMPLING_RATE_44KHZ |
			 CEA_SAD_SAMPLING_RATE_48KHZ;
	sample_sizes = CEA_SAD_SAMPLE_SIZE_16 |
		       CEA_SAD_SAMPLE_SIZE_20 |
		       CEA_SAD_SAMPLE_SIZE_24;
	cea_sad_init_pcm(&sad, channels, sampling_rates, sample_sizes);

	/* Initialize the Speaker Allocation Data */
	speaker_alloc.speakers = CEA_SPEAKER_FRONT_LEFT_RIGHT_CENTER;

	return generate_audio_edid(raw_edid, true, &sad, &speaker_alloc);
}

/**
 * igt_kms_get_dp_audio_edid:
 *
 * Get a basic edid block, which includes the DP Audio
 *
 * Returns: A basic DP Audio edid block
 */
const struct edid *igt_kms_get_dp_audio_edid(void)
{
	int channels;
	uint8_t sampling_rates, sample_sizes;
	static unsigned char raw_edid[AUDIO_EDID_SIZE] = {0};
	struct cea_sad sad = {0};
	struct cea_speaker_alloc speaker_alloc = {0};

	/* Initialize the Short Audio Descriptor for PCM */
	channels = 2;
	sampling_rates = CEA_SAD_SAMPLING_RATE_32KHZ |
			 CEA_SAD_SAMPLING_RATE_44KHZ |
			 CEA_SAD_SAMPLING_RATE_48KHZ;
	sample_sizes = CEA_SAD_SAMPLE_SIZE_16 |
		       CEA_SAD_SAMPLE_SIZE_20 |
		       CEA_SAD_SAMPLE_SIZE_24;
	cea_sad_init_pcm(&sad, channels, sampling_rates, sample_sizes);

	/* Initialize the Speaker Allocation Data */
	speaker_alloc.speakers = CEA_SPEAKER_FRONT_LEFT_RIGHT_CENTER;

	return generate_audio_edid(raw_edid, false, &sad, &speaker_alloc);
}

/**
 * igt_kms_get_tiled_edid:
 * @htile: Target H-tile
 * @vtile: Target V-tile
 *
 * Get a basic edid block, which includes tiled display
 *
 * Returns: A basic tiled display edid block
 */
struct edid **igt_kms_get_tiled_edid(uint8_t htile, uint8_t vtile)
{
	uint8_t top[2];
	int edids, i;
	static  char raw_edid[MAX_EDID][256] = { };
	static struct edid *edid[MAX_EDID];

	top[0] = 0x00;
	top[1] = 0x00;
	top[0] = top[0] | (htile<<4);
	vtile = vtile & 15;
	top[0] = top[0] | vtile;
	top[1] = top[1] | ((htile << 2) & 192);
	top[1] = top[1] | (vtile & 48);

	edids = (htile+1) * (vtile+1);

	for (i = 0; i < edids; i++)
		edid[i] = (struct edid *) raw_edid[i];

	for (i = 0; i < edids; i++) {

		struct edid_ext *edid_ext;
		struct edid_tile *edid_tile;

	/* Create a new EDID from the base IGT EDID, and add an
	 * extension that advertises tile support.
	 */
		memcpy(edid[i],
		igt_kms_get_base_tile_edid(), sizeof(struct edid));
		edid[i]->extensions_len = 1;
		edid_ext = &edid[i]->extensions[0];
		edid_tile = &edid_ext->data.tile;
	/* Set 0x70 to 1st byte of extension,
	 * so it is identified as display block
	 */
		edid_ext_set_displayid(edid_ext);
	/* To identify it as a tiled display block extension */
		edid_tile->header[0] = DISPLAY_TILE_BLOCK;
		edid_tile->header[1] = 0x79;
		edid_tile->header[2] = 0x00;
		edid_tile->header[3] = 0x00;
		edid_tile->header[4] = 0x12;
		edid_tile->header[5] = 0x00;
		edid_tile->header[6] = 0x16;
	/* Tile Capabilities */
		edid_tile->tile_cap = SCALE_TO_FIT;
	/* Set number of htile and vtile */
		edid_tile->topo[0] = top[0];
		if (i == 0)
			edid_tile->topo[1] = 0x10;
		else if (i == 1)
			edid_tile->topo[1] = 0x00;
		edid_tile->topo[2] = top[1];
	/* Set tile resolution */
		edid_tile->tile_size[0] = 0x7f;
		edid_tile->tile_size[1] = 0x07;
		edid_tile->tile_size[2] = 0x6f;
		edid_tile->tile_size[3] = 0x08;
	/* Dimension of Bezels */
		edid_tile->tile_pixel_bezel[0] = 0;
		edid_tile->tile_pixel_bezel[1] = 0;
		edid_tile->tile_pixel_bezel[2] = 0;
		edid_tile->tile_pixel_bezel[3] = 0;
		edid_tile->tile_pixel_bezel[4] = 0;
	/* Manufacturer Information */
		edid_tile->topology_id[0] = 0x44;
		edid_tile->topology_id[1] = 0x45;
		edid_tile->topology_id[2] = 0x4c;
		edid_tile->topology_id[3] = 0x43;
		edid_tile->topology_id[4] = 0x48;
		edid_tile->topology_id[5] = 0x02;
		edid_tile->topology_id[6] = 0x00;
		edid_tile->topology_id[7] = 0x00;
		edid_tile->topology_id[8] = 0x00;
	}
	return edid;
}

static const uint8_t edid_4k_svds[] = {
	32 | CEA_SVD_NATIVE, /* 1080p @ 24Hz (native) */
	5,                   /* 1080i @ 60Hz */
	20,                  /* 1080i @ 50Hz */
	4,                   /* 720p @ 60Hz */
	19,                  /* 720p @ 50Hz */
};

/**
 * igt_kms_get_4k_edid:
 *
 * Get a basic edid block, which includes 4K resolution
 *
 * Returns: A basic edid block with 4K resolution
 */
const struct edid *igt_kms_get_4k_edid(void)
{
	static unsigned char raw_edid[256] = {0};
	struct edid *edid;
	struct edid_ext *edid_ext;
	struct edid_cea *edid_cea;
	char *cea_data;
	struct edid_cea_data_block *block;
	/* We'll add 6 extension fields to the HDMI VSDB. */
	char raw_hdmi[HDMI_VSDB_MIN_SIZE + 6] = {0};
	struct hdmi_vsdb *hdmi;
	size_t cea_data_size = 0;

	/* Create a new EDID from the base IGT EDID, and add an
	 * extension that advertises 4K support. */
	edid = (struct edid *) raw_edid;
	memcpy(edid, igt_kms_get_base_edid(), sizeof(struct edid));
	edid->extensions_len = 1;
	edid_ext = &edid->extensions[0];
	edid_cea = &edid_ext->data.cea;
	cea_data = edid_cea->data;

	/* Short Video Descriptor */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_svd(block, edid_4k_svds,
						     sizeof(edid_4k_svds));

	/* Vendor-Specific Data Block */
	hdmi = (struct hdmi_vsdb *) raw_hdmi;
	hdmi->src_phy_addr[0] = 0x10;
	hdmi->src_phy_addr[1] = 0x00;
	/* 6 extension fields */
	hdmi->flags1 = 0;
	hdmi->max_tdms_clock = 0;
	hdmi->flags2 = HDMI_VSDB_VIDEO_PRESENT;
	hdmi->data[0] = 0x00; /* HDMI video flags */
	hdmi->data[1] = 1 << 5; /* 1 VIC entry, 0 3D entries */
	hdmi->data[2] = 0x01; /* 2160p, specified as short descriptor */

	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_hdmi_vsdb(block, hdmi,
							   sizeof(raw_hdmi));

	assert(cea_data_size <= sizeof(edid_cea->data));

	edid_ext_set_cea(edid_ext, cea_data_size, 0, 0);

	edid_update_checksum(edid);

	return edid;
}

/**
 * igt_kms_get_3d_edid:
 *
 * Get a basic edid block, which includes 3D mode
 *
 * Returns: A basic edid block with 3D mode
 */
const struct edid *igt_kms_get_3d_edid(void)
{
	static unsigned char raw_edid[256] = {0};
	struct edid *edid;
	struct edid_ext *edid_ext;
	struct edid_cea *edid_cea;
	char *cea_data;
	struct edid_cea_data_block *block;
	/* We'll add 5 extension fields to the HDMI VSDB. */
	char raw_hdmi[HDMI_VSDB_MIN_SIZE + 5] = {0};
	struct hdmi_vsdb *hdmi;
	size_t cea_data_size = 0;

	/* Create a new EDID from the base IGT EDID, and add an
	 * extension that advertises 3D support. */
	edid = (struct edid *) raw_edid;
	memcpy(edid, igt_kms_get_base_edid(), sizeof(struct edid));
	edid->extensions_len = 1;
	edid_ext = &edid->extensions[0];
	edid_cea = &edid_ext->data.cea;
	cea_data = edid_cea->data;

	/* Short Video Descriptor */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_svd(block, edid_4k_svds,
						     sizeof(edid_4k_svds));

	/* Vendor-Specific Data Block */
	hdmi = (struct hdmi_vsdb *) raw_hdmi;
	hdmi->src_phy_addr[0] = 0x10;
	hdmi->src_phy_addr[1] = 0x00;
	/* 5 extension fields */
	hdmi->flags1 = 0;
	hdmi->max_tdms_clock = 0;
	hdmi->flags2 = HDMI_VSDB_VIDEO_PRESENT;
	hdmi->data[0] = HDMI_VSDB_VIDEO_3D_PRESENT; /* HDMI video flags */
	hdmi->data[1] = 0; /* 0 VIC entries, 0 3D entries */

	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_hdmi_vsdb(block, hdmi,
							   sizeof(raw_hdmi));

	assert(cea_data_size <= sizeof(edid_cea->data));

	edid_ext_set_cea(edid_ext, cea_data_size, 0, 0);

	edid_update_checksum(edid);

	return edid;
}

/* Set of Video Identification Codes advertised in the EDID */
static const uint8_t edid_ar_svds[] = {
	16, /* 1080p @ 60Hz, 16:9 */
};

/**
 * igt_kms_get_aspect_ratio_edid:
 *
 * Gets the base edid block, which includes the following modes
 * and different aspect ratio
 *
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * Returns: A basic edid block with aspect ratio block
 */
const struct edid *igt_kms_get_aspect_ratio_edid(void)
{
	static unsigned char raw_edid[2 * EDID_BLOCK_SIZE] = {0};
	struct edid *edid;
	struct edid_ext *edid_ext;
	struct edid_cea *edid_cea;
	char *cea_data;
	struct edid_cea_data_block *block;
	size_t cea_data_size = 0, vsdb_size;
	const struct cea_vsdb *vsdb;

	edid = (struct edid *) raw_edid;
	memcpy(edid, igt_kms_get_base_edid(), sizeof(struct edid));
	edid->extensions_len = 1;
	edid_ext = &edid->extensions[0];
	edid_cea = &edid_ext->data.cea;
	cea_data = edid_cea->data;

	/* The HDMI VSDB advertises support for InfoFrames */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	vsdb = cea_vsdb_get_hdmi_default(&vsdb_size);
	cea_data_size += edid_cea_data_block_set_vsdb(block, vsdb,
						      vsdb_size);

	/* Short Video Descriptor */
	block = (struct edid_cea_data_block *) &cea_data[cea_data_size];
	cea_data_size += edid_cea_data_block_set_svd(block, edid_ar_svds,
						     sizeof(edid_ar_svds));

	assert(cea_data_size <= sizeof(edid_cea->data));

	edid_ext_set_cea(edid_ext, cea_data_size, 0, 0);

	edid_update_checksum(edid);

	return edid;
}

/**
 * igt_kms_get_custom_edid:
 *
 * @edid: enum to specify which edid block is required
 * returns pointer to requested edid block
 *
 * Returns: Required edid
 */
const struct edid *igt_kms_get_custom_edid(enum igt_custom_edid_type edid)
{
	switch (edid) {
	case IGT_CUSTOM_EDID_BASE:
		return igt_kms_get_base_edid();
	case IGT_CUSTOM_EDID_FULL:
		return igt_kms_get_full_edid();
	case IGT_CUSTOM_EDID_ALT:
		return igt_kms_get_alt_edid();
	case IGT_CUSTOM_EDID_HDMI_AUDIO:
		return igt_kms_get_hdmi_audio_edid();
	case IGT_CUSTOM_EDID_DP_AUDIO:
		return igt_kms_get_dp_audio_edid();
	case IGT_CUSTOM_EDID_ASPECT_RATIO:
		return igt_kms_get_aspect_ratio_edid();
	}
	assert(0); /* unreachable */
}

const char * const igt_plane_prop_names[IGT_NUM_PLANE_PROPS] = {
	[IGT_PLANE_SRC_X] = "SRC_X",
	[IGT_PLANE_SRC_Y] = "SRC_Y",
	[IGT_PLANE_SRC_W] = "SRC_W",
	[IGT_PLANE_SRC_H] = "SRC_H",
	[IGT_PLANE_CRTC_X] = "CRTC_X",
	[IGT_PLANE_CRTC_Y] = "CRTC_Y",
	[IGT_PLANE_CRTC_W] = "CRTC_W",
	[IGT_PLANE_CRTC_H] = "CRTC_H",
	[IGT_PLANE_HOTSPOT_X] = "HOTSPOT_X",
	[IGT_PLANE_HOTSPOT_Y] = "HOTSPOT_Y",
	[IGT_PLANE_FB_ID] = "FB_ID",
	[IGT_PLANE_CRTC_ID] = "CRTC_ID",
	[IGT_PLANE_IN_FENCE_FD] = "IN_FENCE_FD",
	[IGT_PLANE_TYPE] = "type",
	[IGT_PLANE_ROTATION] = "rotation",
	[IGT_PLANE_IN_FORMATS] = "IN_FORMATS",
	[IGT_PLANE_COLOR_ENCODING] = "COLOR_ENCODING",
	[IGT_PLANE_COLOR_RANGE] = "COLOR_RANGE",
	[IGT_PLANE_PIXEL_BLEND_MODE] = "pixel blend mode",
	[IGT_PLANE_ALPHA] = "alpha",
	[IGT_PLANE_ZPOS] = "zpos",
	[IGT_PLANE_FB_DAMAGE_CLIPS] = "FB_DAMAGE_CLIPS",
	[IGT_PLANE_SCALING_FILTER] = "SCALING_FILTER",
	[IGT_PLANE_SIZE_HINTS] = "SIZE_HINTS",
	[IGT_PLANE_IN_FORMATS_ASYNC] = "IN_FORMATS_ASYNC",
	[IGT_PLANE_COLOR_PIPELINE] = "COLOR_PIPELINE",
};

const char * const igt_colorop_prop_names[IGT_NUM_COLOROP_PROPS] = {
	[IGT_COLOROP_TYPE] = "TYPE",
	[IGT_COLOROP_BYPASS] = "BYPASS",
	[IGT_COLOROP_CURVE_1D_TYPE] = "CURVE_1D_TYPE",
	[IGT_COLOROP_SIZE] = "SIZE",
	[IGT_COLOROP_DATA] = "DATA",
	[IGT_COLOROP_MULTIPLIER] = "MULTIPLIER",
	[IGT_COLOROP_LUT3D_INTERPOLATION] = "LUT3D_INTERPOLATION",
	[IGT_COLOROP_FIXED_MATRIX_TYPE] = "FIXED_MATRIX_TYPE",
	[IGT_COLOROP_NEXT] = "NEXT",
};

const char * const igt_crtc_prop_names[IGT_NUM_CRTC_PROPS] = {
	[IGT_CRTC_CTM] = "CTM",
	[IGT_CRTC_GAMMA_LUT] = "GAMMA_LUT",
	[IGT_CRTC_GAMMA_LUT_SIZE] = "GAMMA_LUT_SIZE",
	[IGT_CRTC_DEGAMMA_LUT] = "DEGAMMA_LUT",
	[IGT_CRTC_DEGAMMA_LUT_SIZE] = "DEGAMMA_LUT_SIZE",
	[IGT_CRTC_MODE_ID] = "MODE_ID",
	[IGT_CRTC_ACTIVE] = "ACTIVE",
	[IGT_CRTC_OUT_FENCE_PTR] = "OUT_FENCE_PTR",
	[IGT_CRTC_VRR_ENABLED] = "VRR_ENABLED",
	[IGT_CRTC_SCALING_FILTER] = "SCALING_FILTER",
	[IGT_CRTC_SHARPNESS_STRENGTH] = "SHARPNESS_STRENGTH",
	[IGT_CRTC_BACKGROUND_COLOR] = "BACKGROUND_COLOR",
};

const char * const igt_connector_prop_names[IGT_NUM_CONNECTOR_PROPS] = {
	[IGT_CONNECTOR_SCALING_MODE] = "scaling mode",
	[IGT_CONNECTOR_CRTC_ID] = "CRTC_ID",
	[IGT_CONNECTOR_DPMS] = "DPMS",
	[IGT_CONNECTOR_BROADCAST_RGB] = "Broadcast RGB",
	[IGT_CONNECTOR_CONTENT_PROTECTION] = "Content Protection",
	[IGT_CONNECTOR_VRR_CAPABLE] = "vrr_capable",
	[IGT_CONNECTOR_HDCP_CONTENT_TYPE] = "HDCP Content Type",
	[IGT_CONNECTOR_LINK_STATUS] = "link-status",
	[IGT_CONNECTOR_MAX_BPC] = "max bpc",
	[IGT_CONNECTOR_HDR_OUTPUT_METADATA] = "HDR_OUTPUT_METADATA",
	[IGT_CONNECTOR_WRITEBACK_PIXEL_FORMATS] = "WRITEBACK_PIXEL_FORMATS",
	[IGT_CONNECTOR_WRITEBACK_FB_ID] = "WRITEBACK_FB_ID",
	[IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR] = "WRITEBACK_OUT_FENCE_PTR",
	[IGT_CONNECTOR_DITHERING_MODE] = "dithering mode",
};

const char * const igt_rotation_names[] = {
	[0] = "rotate-0",
	[1] = "rotate-90",
	[2] = "rotate-180",
	[3] = "rotate-270",
	[4] = "reflect-x",
	[5] = "reflect-y",
};

static unsigned int
igt_plane_rotations(igt_display_t *display, igt_plane_t *plane,
		    drmModePropertyPtr prop)
{
	unsigned int rotations = 0;

	igt_assert_eq(prop->flags & DRM_MODE_PROP_LEGACY_TYPE,
		      DRM_MODE_PROP_BITMASK);
	igt_assert_eq(prop->count_values, prop->count_enums);

	for (int i = 0; i < ARRAY_SIZE(igt_rotation_names); i++) {
		for (int j = 0; j < prop->count_enums; j++) {
			if (strcmp(igt_rotation_names[i], prop->enums[j].name))
				continue;

			/* various places assume the uabi uses specific bit values */
			igt_assert_eq(prop->values[j], i);

			rotations |= 1 << i;
		}
	}
	igt_assert_neq(rotations, 0);

	return rotations;
}

/*
 * Retrieve all the properies specified in props_name and store them into
 * colorop->props.
 */
static void
igt_fill_colorop_props(igt_display_t *display, igt_colorop_t *colorop,
		       int num_props, const char * const prop_names[])
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, colorop->id, DRM_MODE_OBJECT_COLOROP);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_props; j++) {
			if (strcmp(prop->name, prop_names[j]) != 0)
				continue;

			colorop->props[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
}

static void igt_fill_colorop(igt_display_t *display, igt_plane_t *plane,
			     igt_colorop_t *colorop, uint32_t id,
			     char *name)
{
	colorop->id = id;
	colorop->plane = plane;

	if (name)
		snprintf(colorop->name, sizeof(colorop->name), "%s", name);

	igt_fill_colorop_props(display, colorop, IGT_NUM_COLOROP_PROPS, igt_colorop_prop_names);
}

static void
igt_fill_plane_color_pipelines(igt_display_t *display, igt_plane_t *plane,
			       drmModePropertyPtr prop)
{
	int i;
	uint32_t colorop_id;

	plane->num_color_pipelines = 0;

	for (i = 0; i < prop->count_enums; i++) {
		if (prop->enums[i].value) {
			igt_colorop_t *colorop;

			igt_assert(display->n_colorops < MAX_NUM_COLOROPS);
			colorop = &display->colorops[display->n_colorops++];

			igt_fill_colorop(display, plane, colorop, prop->enums[i].value, prop->enums[i].name);
			plane->color_pipelines[plane->num_color_pipelines++] = colorop;

			/* get all NEXT colorops */
			colorop_id = igt_colorop_get_prop(display, colorop,
							  IGT_COLOROP_NEXT);
			while (colorop_id) {
				igt_colorop_t *next;

				igt_assert(display->n_colorops < MAX_NUM_COLOROPS);
				next = &display->colorops[display->n_colorops++];
				igt_fill_colorop(display, plane, next, colorop_id, NULL);

				colorop->next = next;
				colorop = next;

				colorop_id = igt_colorop_get_prop(display, colorop,
								  IGT_COLOROP_NEXT);
			}
		}
	}

	igt_assert(plane->num_color_pipelines < IGT_NUM_PLANE_COLOR_PIPELINES);
}

/*
 * Retrieve all the properties specified in props_name and store them into
 * plane->props.
 */
static void
igt_fill_plane_props(igt_display_t *display, igt_plane_t *plane,
		     int num_props, const char * const prop_names[])
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, plane->drm_plane->plane_id, DRM_MODE_OBJECT_PLANE);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_props; j++) {
			if (strcmp(prop->name, prop_names[j]) != 0)
				continue;

			plane->props[j] = props->props[i];
			break;
		}

		if (strcmp(prop->name, "rotation") == 0)
			plane->rotations = igt_plane_rotations(display, plane, prop);

		if (strcmp(prop->name, "COLOR_PIPELINE") == 0)
			igt_fill_plane_color_pipelines(display, plane, prop);

		drmModeFreeProperty(prop);
	}

	if (!plane->rotations)
		plane->rotations = IGT_ROTATION_0;

	drmModeFreeObjectProperties(props);
}

/*
 * Retrieve all the properties specified in props_name and store them into
 * config->atomic_props_crtc and config->atomic_props_connector.
 */
static void
igt_atomic_fill_connector_props(igt_display_t *display, igt_output_t *output,
			int num_connector_props, const char * const conn_prop_names[])
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, output->config.connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_connector_props; j++) {
			if (strcmp(prop->name, conn_prop_names[j]) != 0)
				continue;

			output->props[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

static void
igt_fill_crtc_props(igt_crtc_t *crtc,
		    int num_crtc_props, const char * const crtc_prop_names[])
{
	igt_display_t *display = crtc->display;
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, crtc->crtc_id,
					   DRM_MODE_OBJECT_CRTC);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_crtc_props; j++) {
			if (strcmp(prop->name, crtc_prop_names[j]) != 0)
				continue;

			crtc->props[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

static igt_plane_t *igt_get_assigned_primary(igt_output_t *output,
					     igt_crtc_t *crtc)
{
	int drm_fd = output->display->drm_fd;
	drmModeModeInfo *mode;
	struct igt_fb fb;
	igt_plane_t *plane = NULL;
	uint32_t crtc_id;
	int i;

	mode = igt_output_get_mode(output);

	igt_create_color_fb(drm_fd, mode->hdisplay, mode->vdisplay,
						DRM_FORMAT_XRGB8888,
						DRM_FORMAT_MOD_LINEAR,
						1.0, 1.0, 1.0, &fb);

	crtc_id = crtc->crtc_id;

	/*
	 * Do a legacy SETCRTC to start things off, so that we know that
	 * the kernel will pick the correct primary plane and attach it
	 * to the CRTC. This lets us handle the case that there are
	 * multiple primary planes (one per CRTC), but which can *also*
	 * be attached to other CRTCs
	 */
	igt_assert(drmModeSetCrtc(output->display->drm_fd, crtc_id, fb.fb_id,
							  0, 0, &output->id, 1, mode) == 0);

	for(i = 0; i < crtc->n_planes; i++) {
		if (crtc->planes[i].type != DRM_PLANE_TYPE_PRIMARY)
			continue;

		if (igt_plane_get_prop(&crtc->planes[i], IGT_PLANE_CRTC_ID) != crtc_id)
			continue;

		plane = &crtc->planes[i];
		break;
	}

	/* Removing the FB will also shut down the display for us: */
	igt_remove_fb(drm_fd, &fb);
	igt_assert_f(plane, "Valid assigned primary plane for CRTC_ID %d not found.\n", crtc_id);

	return plane;
}

/**
 * kmstest_pipe_name:
 * @pipe: display pipe
 *
 * Returns: A string representing @pipe, e.g. "A".
 */
const char *kmstest_pipe_name(enum hardware_pipe pipe)
{
	static const char str[] = "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0N\0O\0P";

	_Static_assert(sizeof(str) == IGT_MAX_PIPES * 2,
		       "Missing pipe name");

	if (pipe == PIPE_NONE)
		return "None";

	if (pipe >= IGT_MAX_PIPES)
		return "invalid";

	return str + (pipe * 2);
}

/**
 * kmstest_plane_type_name:
 * @plane_type: display plane type
 *
 * Returns: A string representing @plane_type, e.g. "overlay".
 */
const char *kmstest_plane_type_name(int plane_type)
{
	static const char * const names[] = {
		[DRM_PLANE_TYPE_OVERLAY] = "overlay",
		[DRM_PLANE_TYPE_PRIMARY] = "primary",
		[DRM_PLANE_TYPE_CURSOR] = "cursor",
	};

	igt_assert(plane_type < ARRAY_SIZE(names) && names[plane_type]);

	return names[plane_type];
}

struct type_name {
	int type;
	const char *name;
};

static const char *find_type_name(const struct type_name *names, int type)
{
	for (; names->name; names++) {
		if (names->type == type)
			return names->name;
	}

	return "(invalid)";
}

static const struct type_name encoder_type_names[] = {
	{ DRM_MODE_ENCODER_NONE, "none" },
	{ DRM_MODE_ENCODER_DAC, "DAC" },
	{ DRM_MODE_ENCODER_TMDS, "TMDS" },
	{ DRM_MODE_ENCODER_LVDS, "LVDS" },
	{ DRM_MODE_ENCODER_TVDAC, "TVDAC" },
	{ DRM_MODE_ENCODER_VIRTUAL, "Virtual" },
	{ DRM_MODE_ENCODER_DSI, "DSI" },
	{ DRM_MODE_ENCODER_DPMST, "DP MST" },
	{}
};

/**
 * kmstest_encoder_type_str:
 * @type: DRM_MODE_ENCODER_* enumeration value
 *
 * Returns: A string representing the drm encoder @type.
 */
const char *kmstest_encoder_type_str(int type)
{
	return find_type_name(encoder_type_names, type);
}

static const struct type_name connector_status_names[] = {
	{ DRM_MODE_CONNECTED, "connected" },
	{ DRM_MODE_DISCONNECTED, "disconnected" },
	{ DRM_MODE_UNKNOWNCONNECTION, "unknown" },
	{}
};

/**
 * kmstest_connector_status_str:
 * @status: DRM_MODE_* connector status value
 *
 * Returns: A string representing the drm connector status @status.
 */
const char *kmstest_connector_status_str(int status)
{
	return find_type_name(connector_status_names, status);
}

enum scaling_filter {
	SCALING_FILTER_DEFAULT,
	SCALING_FILTER_NEAREST_NEIGHBOR,
};

static const struct type_name scaling_filter_names[] = {
	{ SCALING_FILTER_DEFAULT, "Default" },
	{ SCALING_FILTER_NEAREST_NEIGHBOR, "Nearest Neighbor" },
	{}
};

/**
 * kmstest_scaling_filter_str:
 * @filter: SCALING_FILTER_* filter value
 *
 * Returns: A string representing the scaling filter @filter.
 */
const char *kmstest_scaling_filter_str(int filter)
{
	return find_type_name(scaling_filter_names, filter);
}

static const struct type_name scaling_modes_names[] = {
	{ DRM_MODE_SCALE_FULLSCREEN, "fullscreen" },
	{ DRM_MODE_SCALE_CENTER, "center" },
	{ DRM_MODE_SCALE_ASPECT, "aspect" },
	{ DRM_MODE_SCALE_NONE, "none" },
	{}
};

/**
 * kmstest_scaling_mode_str:
 * @mode: SCALING_MODE_* mode value
 *
 * Returns: A string representing the scaling mode @mode.
 */
const char *kmstest_scaling_mode_str(int mode)
{
	return find_type_name(scaling_modes_names, mode);
}

static const struct type_name dsc_output_format_names[] = {
	{ DSC_FORMAT_RGB, "RGB" },
	{ DSC_FORMAT_YCBCR420, "YCBCR420" },
	{ DSC_FORMAT_YCBCR444, "YCBCR444" },
	{}
};

/**
 * kmstest_dsc_output_format_str:
 * @output_format: DSC_FORMAT_* output format value
 *
 * Returns: A string representing the output format @output format.
 */
const char *kmstest_dsc_output_format_str(int output_format)
{
	return find_type_name(dsc_output_format_names, output_format);
}

static const struct type_name connector_type_names[] = {
	{ DRM_MODE_CONNECTOR_Unknown, "Unknown" },
	{ DRM_MODE_CONNECTOR_VGA, "VGA" },
	{ DRM_MODE_CONNECTOR_DVII, "DVI-I" },
	{ DRM_MODE_CONNECTOR_DVID, "DVI-D" },
	{ DRM_MODE_CONNECTOR_DVIA, "DVI-A" },
	{ DRM_MODE_CONNECTOR_Composite, "Composite" },
	{ DRM_MODE_CONNECTOR_SVIDEO, "SVIDEO" },
	{ DRM_MODE_CONNECTOR_LVDS, "LVDS" },
	{ DRM_MODE_CONNECTOR_Component, "Component" },
	{ DRM_MODE_CONNECTOR_9PinDIN, "DIN" },
	{ DRM_MODE_CONNECTOR_DisplayPort, "DP" },
	{ DRM_MODE_CONNECTOR_HDMIA, "HDMI-A" },
	{ DRM_MODE_CONNECTOR_HDMIB, "HDMI-B" },
	{ DRM_MODE_CONNECTOR_TV, "TV" },
	{ DRM_MODE_CONNECTOR_eDP, "eDP" },
	{ DRM_MODE_CONNECTOR_VIRTUAL, "Virtual" },
	{ DRM_MODE_CONNECTOR_DSI, "DSI" },
	{ DRM_MODE_CONNECTOR_DPI, "DPI" },
	{ DRM_MODE_CONNECTOR_WRITEBACK, "Writeback" },
	{}
};

/**
 * kmstest_connector_type_str:
 * @type: DRM_MODE_CONNECTOR_* enumeration value
 *
 * Returns: A string representing the drm connector @type.
 */
const char *kmstest_connector_type_str(int type)
{
	return find_type_name(connector_type_names, type);
}

static const char *mode_stereo_name(const drmModeModeInfo *mode)
{
	switch (mode->flags & DRM_MODE_FLAG_3D_MASK) {
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
		return "FP";
	case DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE:
		return "FA";
	case DRM_MODE_FLAG_3D_LINE_ALTERNATIVE:
		return "LA";
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL:
		return "SBSF";
	case DRM_MODE_FLAG_3D_L_DEPTH:
		return "LD";
	case DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH:
		return "LDGFX";
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		return "TB";
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		return "SBSH";
	default:
		return NULL;
	}
}

static const char *mode_picture_aspect_name(const drmModeModeInfo *mode)
{
	switch (mode->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_NONE:
		return NULL;
	case DRM_MODE_FLAG_PIC_AR_4_3:
		return "4:3";
	case DRM_MODE_FLAG_PIC_AR_16_9:
		return "16:9";
	case DRM_MODE_FLAG_PIC_AR_64_27:
		return "64:27";
	case DRM_MODE_FLAG_PIC_AR_256_135:
		return "256:135";
	default:
		return "invalid";
	}
}

/**
 * kmstest_dump_mode:
 * @mode: libdrm mode structure
 *
 * Prints @mode to stdout in a human-readable form.
 */
void kmstest_dump_mode(drmModeModeInfo *mode)
{
	const char *stereo = mode_stereo_name(mode);
	const char *aspect = mode_picture_aspect_name(mode);

	igt_info("  %s: %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x %s%s%s%s%s%s\n",
		 mode->name, mode->vrefresh, mode->clock,
		 mode->hdisplay, mode->hsync_start,
		 mode->hsync_end, mode->htotal,
		 mode->vdisplay, mode->vsync_start,
		 mode->vsync_end, mode->vtotal,
		 mode->type, mode->flags,
		 stereo ? " (3D:" : "",
		 stereo ? stereo : "", stereo ? ")" : "",
		 aspect ? " (PAR:" : "",
		 aspect ? aspect : "", aspect ? ")" : "");
}

/*
 * The hardware pipe may be different from the CRTC index. Figure out the CRTC
 * index to pipe mapping from the debugfs.
 */
enum hardware_pipe __intel_get_pipe_from_crtc_index(int fd, int crtc_index)
{
	enum hardware_pipe hardware_pipe;
	char buf[2];
	int debugfs_fd, res = 0;
	char pipe_char;

	debugfs_fd = igt_debugfs_crtc_dir(fd, crtc_index);

	if (debugfs_fd >= 0) {
		res = igt_debugfs_simple_read(debugfs_fd, "i915_pipe", buf, sizeof(buf));
		close(debugfs_fd);
	}

	igt_assert_f(res > 0, "Failed to read the debugfs i915_pipe.\n");

	igt_assert_eq(sscanf(buf, "%c", &pipe_char), 1);

	hardware_pipe = pipe_char - 'A';

	igt_assert_f(hardware_pipe >= 0 && hardware_pipe < IGT_MAX_PIPES,
		     "i915_pipe %c out of range\n", pipe_char);

	return hardware_pipe;
}

/**
 * kmstest_get_crtc_index_from_id:
 * @fd: DRM fd
 * @crtc_id: DRM CRTC id
 *
 * Returns: The crtc index for the given DRM CRTC ID @crtc_id. Returns 0 if the
 * index could not be determined.
 */
int kmstest_get_crtc_index_from_id(int fd, int crtc_id)
{
	drmModeRes *res;
	drmModeCrtc *drm_crtc;
	int i, cur_id;

	res = drmModeGetResources(fd);
	igt_assert(res);

	for (i = 0; i < res->count_crtcs; i++) {
		drm_crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		igt_assert(drm_crtc);
		cur_id = drm_crtc->crtc_id;
		drmModeFreeCrtc(drm_crtc);
		if (cur_id == crtc_id)
			break;
	}

	igt_assert(i < res->count_crtcs);

	drmModeFreeResources(res);

	return i;
}

/**
 * kmstest_find_crtc_for_connector:
 * @fd: DRM fd
 * @res: libdrm resources pointer
 * @connector: libdrm connector pointer
 * @crtc_blacklist_idx_mask: a mask of CRTC indexes that we can't return
 *
 * Returns: The CRTC ID for a CRTC that fits the connector, otherwise it asserts
 * false and never returns. The blacklist mask can be used in case you have
 * CRTCs that are already in use by other connectors.
 */
uint32_t kmstest_find_crtc_for_connector(int fd, drmModeRes *res,
					 drmModeConnector *connector,
					 uint32_t crtc_blacklist_idx_mask)
{
	drmModeEncoder *e;
	uint32_t possible_crtcs;
	int i, j;

	for (i = 0; i < connector->count_encoders; i++) {
		e = drmModeGetEncoder(fd, connector->encoders[i]);
		possible_crtcs = e->possible_crtcs & ~crtc_blacklist_idx_mask;
		drmModeFreeEncoder(e);

		for (j = 0; possible_crtcs >> j; j++)
			if (possible_crtcs & (1 << j))
				return res->crtcs[j];
	}

	igt_assert(false);
}

/**
 * kmstest_dumb_create:
 * @fd: open drm file descriptor
 * @width: width of the buffer in pixels
 * @height: height of the buffer in pixels
 * @bpp: bytes per pixel of the buffer
 * @stride: Pointer which receives the dumb bo's stride, can be NULL.
 * @size: Pointer which receives the dumb bo's size, can be NULL.
 *
 * This wraps the CREATE_DUMB ioctl, which allocates a new dumb buffer object
 * for the specified dimensions.
 *
 * Returns: The file-private handle of the created buffer object
 */
uint32_t kmstest_dumb_create(int fd, int width, int height, int bpp,
			     unsigned *stride, uint64_t *size)
{
	struct drm_mode_create_dumb create;

	memset(&create, 0, sizeof(create));
	create.width = width;
	create.height = height;
	create.bpp = bpp;

	create.handle = 0;
	do_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	igt_assert(create.handle);
	igt_assert(create.size >= (uint64_t) width * height * bpp / 8);

	if (stride)
		*stride = create.pitch;

	if (size)
		*size = create.size;

	return create.handle;
}

/**
 * kmstest_dumb_map_buffer:
 * @fd: Opened drm file descriptor
 * @handle: Offset in the file referred to by fd
 * @size: Length of the mapping, must be greater than 0
 * @prot: Describes the memory protection of the mapping
 *
 * Returns: A pointer representing the start of the virtual mapping
 * Caller of this function should munmap the pointer returned, after its usage.
 */
void *kmstest_dumb_map_buffer(int fd, uint32_t handle, uint64_t size,
			      unsigned prot)
{
	struct drm_mode_map_dumb arg = {};
	void *ptr;

	arg.handle = handle;

	do_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);

	ptr = mmap(NULL, size, prot, MAP_SHARED, fd, arg.offset);
	igt_assert(ptr != MAP_FAILED);

	return ptr;
}

static int __kmstest_dumb_destroy(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb arg = { handle };
	int err = 0;

	if (drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg))
		err = -errno;

	errno = 0;
	return err;
}

/**
 * kmstest_dumb_destroy:
 * @fd: Opened drm file descriptor
 * @handle: Offset in the file referred to by fd
 */
void kmstest_dumb_destroy(int fd, uint32_t handle)
{
	igt_assert_eq(__kmstest_dumb_destroy(fd, handle), 0);
}

/*
 * Returns: The previous mode, or KD_GRAPHICS if no /dev/tty0 was
 * found and nothing was done.
 */
static signed long set_vt_mode(unsigned long mode)
{
	int fd;
	unsigned long prev_mode;
	static const char TTY0[] = "/dev/tty0";

	if (access(TTY0, F_OK)) {
		/* errno message should be "No such file". Do not
		   hardcode but ask strerror() in the very unlikely
		   case something else happened. */
		igt_debug("VT: %s: %s, cannot change its mode\n",
			  TTY0, strerror(errno));
		return KD_GRAPHICS;
	}

	fd = open(TTY0, O_RDONLY);
	if (fd < 0)
		return -errno;

	prev_mode = 0;
	if (drmIoctl(fd, KDGETMODE, &prev_mode))
		goto err;
	if (drmIoctl(fd, KDSETMODE, (void *)mode))
		goto err;

	close(fd);

	return prev_mode;
err:
	close(fd);

	return -errno;
}

static unsigned long orig_vt_mode = -1UL;

/**
 * kmstest_restore_vt_mode:
 *
 * Restore the VT mode in use before #kmstest_set_vt_graphics_mode was called.
 */
void kmstest_restore_vt_mode(void)
{
	long ret;

	if (orig_vt_mode != -1UL) {
		ret = set_vt_mode(orig_vt_mode);

		igt_assert(ret >= 0);
		igt_debug("VT: original mode 0x%lx restored\n", orig_vt_mode);
		orig_vt_mode = -1UL;
	}
}

/**
 * kmstest_set_vt_graphics_mode:
 *
 * Sets the controlling VT (if available) into graphics/raw mode and installs
 * an igt exit handler to set the VT back to text mode on exit. Use
 * #kmstest_restore_vt_mode to restore the previous VT mode manually.
 *
 * All kms tests must call this function to make sure that the fbcon doesn't
 * interfere by e.g. blanking the screen.
 */
void kmstest_set_vt_graphics_mode(void)
{
	long ret;

	igt_install_exit_handler((igt_exit_handler_t) kmstest_restore_vt_mode);

	ret = set_vt_mode(KD_GRAPHICS);

	igt_assert(ret >= 0);
	orig_vt_mode = ret;

	igt_debug("VT: graphics mode set (mode was 0x%lx)\n", ret);
}

/**
 * kmstest_set_vt_text_mode:
 *
 * Sets the controlling VT (if available) into text mode.
 * Unlikely kmstest_set_vt_graphics_mode() it do not install an igt exit
 * handler to set the VT back to the previous mode.
 */
void kmstest_set_vt_text_mode(void)
{
	igt_assert(set_vt_mode(KD_TEXT) >= 0);
}

/**
 * reset_connectors_at_exit:
 *
 * Exit handler used to remove any forced state from the connectors.
 */
void reset_connectors_at_exit(int sig)
{
	igt_reset_connectors();
}

static char *kmstest_connector_dirname(int idx,
				       uint32_t connector_type,
				       uint32_t connector_type_id,
				       char *name, int namelen)
{
	snprintf(name, namelen, "card%d-%s-%d", idx,
		 kmstest_connector_type_str(connector_type),
		 connector_type_id);

	return name;
}

/**
 * igt_connector_sysfs_open:
 * @drm_fd: drm file descriptor
 * @connector: drm connector
 *
 * Returns: The connector sysfs fd, or -1 on failure.
 */
int igt_connector_sysfs_open(int drm_fd,
			     drmModeConnector *connector)
{
	char name[80];
	int dir, conn_dir;

	dir = igt_sysfs_open(drm_fd);
	if (dir < 0)
		return dir;

	kmstest_connector_dirname(igt_device_get_card_index(drm_fd),
				  connector->connector_type,
				  connector->connector_type_id,
				  name, sizeof(name));

	conn_dir = openat(dir, name, O_RDONLY);

	close(dir);

	return conn_dir;
}

static struct igt_connector_attr *connector_attr_find(int idx, drmModeConnector *connector,
						      igt_connector_attr_set set,
						      const char *attr)
{
	igt_assert(connector->connector_type != 0);

	for (int i = 0; i < ARRAY_SIZE(connector_attrs); i++) {
		struct igt_connector_attr *c = &connector_attrs[i];

		if (c->idx == idx &&
		    c->connector_type == connector->connector_type &&
		    c->connector_type_id == connector->connector_type_id &&
		    c->set == set && !strcmp(c->attr, attr))
			return c;
	}

	return NULL;
}

static struct igt_connector_attr *connector_attr_find_free(void)
{
	for (int i = 0; i < ARRAY_SIZE(connector_attrs); i++) {
		struct igt_connector_attr *c = &connector_attrs[i];

		if (!c->attr)
			return c;
	}

	return NULL;
}

static struct igt_connector_attr *connector_attr_alloc(int idx, drmModeConnector *connector,
						       int dir, igt_connector_attr_set set,
						       const char *attr, const char *reset_value)
{
	struct igt_connector_attr *c = connector_attr_find_free();

	c->idx = idx;
	c->connector_type = connector->connector_type;
	c->connector_type_id = connector->connector_type_id;

	c->dir = dir;
	c->set = set;
	c->attr = attr;
	c->reset_value = reset_value;

	return c;
}

static void connector_attr_free(struct igt_connector_attr *c)
{
	memset(c, 0, sizeof(*c));
}

/**
 * connector_attr_set:
 * @idx: card index
 * @connector: libdrm connector
 * @dir: directory file descriptor
 * @set: function to set the attribute
 * @attr: attribute name
 * @value: attribute value
 * @reset_value: value to reset the attribute to
 * @force_reset: whether to force reset the attribute
 *
 * Configure a new attribute for @connector on card @idx that can be set
 * using @set function.
 * connector_attr_set register this attribute to reset it to the @reset_value
 * during igt_reset_connector.
 *
 * Returns: True on success, false on failure.
 */
bool connector_attr_set(int idx, drmModeConnector *connector,
			int dir, igt_connector_attr_set set,
			const char *attr, const char *value,
			const char *reset_value,
			bool force_reset)
{
	struct igt_connector_attr *c;

	c = connector_attr_find(idx, connector, set, attr);
	if (!c)
		c = connector_attr_alloc(idx, connector, dir, set,
					 attr, reset_value);

	c->value = value;

	if (!c->set(c->dir, c->attr, c->value)) {
		connector_attr_free(c);
		return false;
	}

	if (!force_reset && !strcmp(c->value, c->reset_value))
		connector_attr_free(c);

	return true;
}

static bool connector_attr_set_sysfs(int drm_fd,
				     drmModeConnector *connector,
				     const char *attr, const char *value,
				     const char *reset_value,
				     bool force_reset)
{
	char name[80];
	int idx, dir;

	idx = igt_device_get_card_index(drm_fd);
	if (idx < 0 || idx > 63)
		return false;

	kmstest_connector_dirname(idx, connector->connector_type,
				  connector->connector_type_id,
				  name, sizeof(name));

	dir = igt_connector_sysfs_open(drm_fd, connector);
	if (dir < 0)
		return false;

	if (!connector_attr_set(idx, connector, dir,
				igt_sysfs_set, attr, value, reset_value,
				force_reset))
		return false;

	igt_debug("Connector %s/%s is now %s\n", name, attr, value);

	return true;
}

/**
 * dump_connector_attrs: Display on debug log all the connector attributes
 */
void dump_connector_attrs(void)
{
	char name[80];

	igt_debug("Current connector attrs:\n");

	for (int i = 0; i < ARRAY_SIZE(connector_attrs); i++) {
		struct igt_connector_attr *c = &connector_attrs[i];

		if (!c->attr)
			continue;

		kmstest_connector_dirname(c->idx, c->connector_type,
					  c->connector_type_id,
					  name, sizeof(name));
		igt_debug("\t%s/%s: %s\n", name, c->attr, c->value);
	}
}

static bool force_connector(int drm_fd,
			    drmModeConnector *connector,
			    const char *value)
{
	return connector_attr_set_sysfs(drm_fd, connector,
					"status", value, "detect",
					false);
}

/**
 * kmstest_force_connector:
 * @fd: drm file descriptor
 * @connector: connector
 * @state: state to force on @connector
 *
 * Force the specified state on the specified connector.
 *
 * Returns: True on success
 */
bool kmstest_force_connector(int drm_fd, drmModeConnector *connector,
			     enum kmstest_force_connector_state state)
{
	const char *value;
	drmModeConnector *temp;

	/*
	 * Forcing DP connectors doesn't currently work, so
	 * fail early to allow the test to skip if required.
	 */
	if (is_intel_device(drm_fd) &&
	    connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		return false;

	switch (state) {
	case FORCE_CONNECTOR_ON:
		value = "on";
		break;
	case FORCE_CONNECTOR_DIGITAL:
		value = "on-digital";
		break;
	case FORCE_CONNECTOR_OFF:
		value = "off";
		break;

	default:
	case FORCE_CONNECTOR_UNSPECIFIED:
		value = "detect";
		break;
	}

	if (!force_connector(drm_fd, connector, value))
		return false;

	dump_connector_attrs();

	igt_install_exit_handler(reset_connectors_at_exit);

	/* To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here. */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);
	drmModeFreeConnector(temp);

	return true;
}

static bool force_connector_joiner(int drm_fd,
				      drmModeConnector *connector,
				      const char *value)
{
	return connector_attr_set_debugfs(drm_fd, connector,
					  "i915_joiner_force_enable",
					  value, "0", false);
}

/**
 * kmstest_force_connector_joiner:
 * @fd: drm file descriptor
 * @connector: connector
 *
 * Enable force joiner state on the specified connector
 * and install exit handler for resetting
 *
 * Returns: True on success
 */
bool kmstest_force_connector_joiner(int drm_fd, drmModeConnector *connector, int joined_pipes)
{
	const char *value;
	drmModeConnector *temp;

	switch (joined_pipes) {
	case JOINED_PIPES_DEFAULT:
		value = "0";
		break;
	case JOINED_PIPES_NONE:
		value = "1";
		break;
	case JOINED_PIPES_BIG_JOINER:
		value = "2";
		break;
	case JOINED_PIPES_ULTRA_JOINER:
		value = "4";
		break;
	default:
		igt_assert(0);
	}

	if (!is_intel_device(drm_fd))
		return false;

	if (!force_connector_joiner(drm_fd, connector, value))
		return false;

	dump_connector_attrs();
	igt_install_exit_handler(reset_connectors_at_exit);

	/*
	 * To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here.
	 */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);
	drmModeFreeConnector(temp);

	return true;
}

/**
 * kmstest_force_edid:
 * @drm_fd: drm file descriptor
 * @connector: connector to set @edid on
 * @edid: An EDID data block
 *
 * Set the EDID data on @connector to @edid. See also #igt_kms_get_base_edid.
 *
 * If @edid is NULL, the forced EDID will be removed.
 */
void kmstest_force_edid(int drm_fd, drmModeConnector *connector,
			const struct edid *edid)
{
	char *path;
	int debugfs_fd, ret;
	drmModeConnector *temp;

	igt_assert_neq(asprintf(&path, "%s-%d/edid_override", kmstest_connector_type_str(connector->connector_type), connector->connector_type_id),
		       -1);
	debugfs_fd = igt_debugfs_open(drm_fd, path, O_WRONLY | O_TRUNC);
	free(path);

	igt_require(debugfs_fd != -1);

	if (edid == NULL)
		ret = write(debugfs_fd, "reset", 5);
	else
		ret = write(debugfs_fd, edid,
			    edid_get_size(edid));
	close(debugfs_fd);

	/* To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here. */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);
	drmModeFreeConnector(temp);

	igt_assert(ret != -1);
}

/**
 * sort_drm_modes_by_clk_dsc:
 * @a: first element
 * @b: second element
 *
 * Comparator function for sorting DRM modes in descending order by clock.
 *
 * Returns: True if first element's clock is less than second element's clock,
 * else False.
 */
int sort_drm_modes_by_clk_dsc(const void *a, const void *b)
{
	const drmModeModeInfo *mode1 = a, *mode2 = b;

	return (mode1->clock < mode2->clock) - (mode2->clock < mode1->clock);
}

/**
 * sort_drm_modes_by_clk_asc:
 * @a: first element
 * @b: second element
 *
 * Comparator function for sorting DRM modes in ascending order by clock.
 *
 * Returns: True if first element's clock is greater than second element's clock,
 * else False.
 */
int sort_drm_modes_by_clk_asc(const void *a, const void *b)
{
	const drmModeModeInfo *mode1 = a, *mode2 = b;

	return (mode1->clock > mode2->clock) - (mode2->clock > mode1->clock);
}

/**
 * sort_drm_modes_by_res_dsc:
 * @a: first element
 * @b: second element
 *
 * Comparator function for sorting DRM modes in descending order by resolution.
 *
 * Returns: True if first element's resolution is less than second element's
 * resolution, else False.
 */
int sort_drm_modes_by_res_dsc(const void *a, const void *b)
{
	const drmModeModeInfo *mode1 = a, *mode2 = b;

	return (mode1->hdisplay < mode2->hdisplay) - (mode2->hdisplay < mode1->hdisplay);
}

/**
 * sort_drm_modes_by_res_asc:
 * @a: first element
 * @b: second element
 *
 * Comparator function for sorting DRM modes in ascending order by resolution.
 *
 * Returns: True if first element's resolution is greater than second element's
 * resolution, else False.
 */
int sort_drm_modes_by_res_asc(const void *a, const void *b)
{
	const drmModeModeInfo *mode1 = a, *mode2 = b;

	return (mode1->hdisplay > mode2->hdisplay) - (mode2->hdisplay > mode1->hdisplay);
}

/**
 * igt_sort_connector_modes:
 * @connector: libdrm connector
 * @comparator: comparison function to compare two elements
 *
 * Sorts connector modes based on the @comparator.
 */
void igt_sort_connector_modes(drmModeConnector *connector,
			      int (*comparator)(const void *, const void*))
{
	qsort(connector->modes,
	      connector->count_modes,
	      sizeof(drmModeModeInfo),
	      comparator);
}

/**
 * kmstest_get_connector_default_mode:
 * @drm_fd: DRM fd
 * @connector: libdrm connector
 * @mode: libdrm mode
 *
 * Retrieves the default mode for @connector and stores it in @mode.
 *
 * Returns: True on success, false on failure
 */
bool kmstest_get_connector_default_mode(int drm_fd, drmModeConnector *connector,
					drmModeModeInfo *mode)
{
	char *env;
	int i;

	if (!connector->count_modes) {
		igt_warn("no modes for connector %d\n",
			 connector->connector_id);
		return false;
	}

	env = getenv("IGT_KMS_RESOLUTION");
	if (env) {
		/*
		 * Only (0 or 1) and (lowest or highest) are allowed.
		 *
		 * 0/lowest: Choose connector mode with lowest possible resolution.
		 * 1/highest: Choose connector mode with highest possible resolution.
		 */
		if (!strcmp(env, "highest") || !strcmp(env, "1"))
			igt_sort_connector_modes(connector, sort_drm_modes_by_res_dsc);
		else if (!strcmp(env, "lowest") || !strcmp(env, "0"))
			igt_sort_connector_modes(connector, sort_drm_modes_by_res_asc);
		else
			goto default_mode;

		*mode = connector->modes[0];
		return true;
	}

default_mode:
	for (i = 0; i < connector->count_modes; i++) {
		if (i == 0 ||
		    connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			*mode = connector->modes[i];
			if (mode->type & DRM_MODE_TYPE_PREFERRED)
				break;
		}
	}

	return true;
}

static void
_kmstest_connector_config_crtc_mask(int drm_fd,
				    drmModeConnector *connector,
				    struct kmstest_connector_config *config)
{
	int i;

	config->valid_crtc_index_mask = 0;

	/* Now get a compatible encoder */
	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm_fd,
							    connector->encoders[i]);

		if (!encoder) {
			igt_warn("could not get encoder %d: %s\n",
				 connector->encoders[i],
				 strerror(errno));

			continue;
		}

		config->valid_crtc_index_mask |= encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);
	}
}

static drmModeEncoder *
_kmstest_connector_config_find_encoder(int drm_fd, drmModeConnector *connector, int crtc_index)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm_fd, connector->encoders[i]);

		if (!encoder) {
			igt_warn("could not get encoder %d: %s\n",
				 connector->encoders[i],
				 strerror(errno));

			continue;
		}

		if (encoder->possible_crtcs & (1 << crtc_index))
			return encoder;

		drmModeFreeEncoder(encoder);
	}

	igt_assert(false);
	return NULL;
}

/**
 * _kmstest_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 * @probe: whether to fully re-probe mode list or not
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config.
 *
 * Returns: True if suitable configuration found for a given connector & CRTC,
 * else False.
 */
static bool _kmstest_connector_config(int drm_fd, uint32_t connector_id,
				      unsigned long crtc_idx_mask,
				      struct kmstest_connector_config *config,
				      bool probe)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModePropertyBlobPtr path_blob;

	config->crtc_index = -1;

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		igt_warn("drmModeGetResources failed");
		goto err1;
	}

	/* First, find the connector & mode */
	if (probe)
		connector = drmModeGetConnector(drm_fd, connector_id);
	else
		connector = drmModeGetConnectorCurrent(drm_fd, connector_id);

	if (!connector)
		goto err2;

	if (connector->connector_id != connector_id) {
		igt_warn("connector id doesn't match (%d != %d)\n",
			 connector->connector_id, connector_id);
		goto err3;
	}

	/* Set connector path for MST connectors. */
	path_blob = kmstest_get_path_blob(drm_fd, connector_id);
	if (path_blob) {
		config->connector_path = strdup(path_blob->data);
		drmModeFreePropertyBlob(path_blob);
	}

	/*
	 * Find given CRTC if crtc_id != 0 or else the first CRTC not in use.
	 * In both cases find the first compatible encoder and skip the CRTC
	 * if there is non such.
	 */
	_kmstest_connector_config_crtc_mask(drm_fd, connector, config);

	if (!connector->count_modes)
		memset(&config->default_mode, 0, sizeof(config->default_mode));
	else if (!kmstest_get_connector_default_mode(drm_fd, connector,
						     &config->default_mode))
		goto err3;

	config->connector = connector;

	crtc_idx_mask &= config->valid_crtc_index_mask;
	if (!crtc_idx_mask)
		/* Keep config->connector */
		goto err2;

	config->crtc_index = ffs(crtc_idx_mask) - 1;

	config->encoder = _kmstest_connector_config_find_encoder(drm_fd, connector, config->crtc_index);
	config->crtc = drmModeGetCrtc(drm_fd, resources->crtcs[config->crtc_index]);

	if (connector->connection != DRM_MODE_CONNECTED)
		goto err2;

	if (!connector->count_modes) {
		if (probe)
			igt_warn("connector %d/%s-%d has no modes\n", connector_id,
				kmstest_connector_type_str(connector->connector_type),
				connector->connector_type_id);
		goto err2;
	}

	drmModeFreeResources(resources);
	return true;
err3:
	drmModeFreeConnector(connector);
err2:
	drmModeFreeResources(resources);
err1:
	return false;
}

/**
 * kmstest_get_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config.
 *
 * Returns: True if suitable configuration found for a given connector & CRTC,
 * else False.
 */
bool kmstest_get_connector_config(int drm_fd, uint32_t connector_id,
				  unsigned long crtc_idx_mask,
				  struct kmstest_connector_config *config)
{
	return _kmstest_connector_config(drm_fd, connector_id, crtc_idx_mask,
					 config, 0);
}

/**
 * kmstest_get_path_blob:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 *
 * Finds a property with the name "PATH" on the connector object.
 *
 * Returns: Pointer to the connector's PATH property if found else NULL.
 */
drmModePropertyBlobPtr kmstest_get_path_blob(int drm_fd, uint32_t connector_id)
{
	uint64_t path_blob_id = 0;
	drmModePropertyBlobPtr path_blob = NULL;

	if (!kmstest_get_property(drm_fd, connector_id,
				  DRM_MODE_OBJECT_CONNECTOR, "PATH", NULL,
				  &path_blob_id, NULL)) {
		return NULL;
	}

	path_blob = drmModeGetPropertyBlob(drm_fd, path_blob_id);
	igt_assert(path_blob);
	return path_blob;
}

/**
 * kmstest_probe_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config, fully probing the connector in the
 * process.
 *
 * Returns: True if suitable configuration found for a given connector & CRTC,
 * else False.
 */
bool kmstest_probe_connector_config(int drm_fd, uint32_t connector_id,
				    unsigned long crtc_idx_mask,
				    struct kmstest_connector_config *config)
{
	return _kmstest_connector_config(drm_fd, connector_id, crtc_idx_mask,
					 config, 1);
}

/**
 * kmstest_free_connector_config:
 * @config: connector configuration structure
 *
 * Free any resources in @config allocated in kmstest_get_connector_config().
 */
void kmstest_free_connector_config(struct kmstest_connector_config *config)
{
	drmModeFreeCrtc(config->crtc);
	config->crtc = NULL;

	drmModeFreeEncoder(config->encoder);
	config->encoder = NULL;

	drmModeFreeConnector(config->connector);
	config->connector = NULL;
}

/**
 * kmstest_set_connector_dpms:
 * @fd: DRM fd
 * @connector: libdrm connector
 * @mode: DRM DPMS value
 *
 * This function sets the DPMS setting of @connector to @mode.
 */
void kmstest_set_connector_dpms(int fd, drmModeConnector *connector, int mode)
{
	int i, dpms = 0;
	bool found_it = false;

	for (i = 0; i < connector->count_props; i++) {
		struct drm_mode_get_property prop = {
			.prop_id = connector->props[i],
		};

		if (drmIoctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop))
			continue;

		if (strcmp(prop.name, "DPMS"))
			continue;

		dpms = prop.prop_id;
		found_it = true;
		break;
	}
	igt_assert_f(found_it, "DPMS property not found on %d\n",
		     connector->connector_id);

	igt_assert(drmModeConnectorSetProperty(fd, connector->connector_id,
					       dpms, mode) == 0);
}

/**
 * kmstest_get_property:
 * @drm_fd: drm file descriptor
 * @object_id: object whose properties we're going to get
 * @object_type: type of obj_id (DRM_MODE_OBJECT_*)
 * @name: name of the property we're going to get
 * @prop_id: if not NULL, returns the property id
 * @value: if not NULL, returns the property value
 * @prop: if not NULL, returns the property, and the caller will have to free
 *        it manually.
 *
 * Finds a property with the given name on the given object.
 *
 * Returns: True in case we found something.
 */
bool
kmstest_get_property(int drm_fd, uint32_t object_id, uint32_t object_type,
		     const char *name, uint32_t *prop_id /* out */,
		     uint64_t *value /* out */,
		     drmModePropertyPtr *prop /* out */)
{
	drmModeObjectPropertiesPtr proplist;
	drmModePropertyPtr _prop;
	bool found = false;
	int i;

	proplist = drmModeObjectGetProperties(drm_fd, object_id, object_type);
	if (!proplist)
		return false;

	for (i = 0; i < proplist->count_props; i++) {
		_prop = drmModeGetProperty(drm_fd, proplist->props[i]);
		if (!_prop)
			continue;

		if (strcmp(_prop->name, name) == 0) {
			found = true;
			if (prop_id)
				*prop_id = proplist->props[i];
			if (value)
				*value = proplist->prop_values[i];
			if (prop)
				*prop = _prop;
			else
				drmModeFreeProperty(_prop);

			break;
		}
		drmModeFreeProperty(_prop);
	}

	drmModeFreeObjectProperties(proplist);
	return found;
}

/**
 * kmstest_unset_all_crtcs:
 * @drm_fd: the DRM fd
 * @resources: libdrm resources pointer
 *
 * Disables all the screens.
 */
void kmstest_unset_all_crtcs(int drm_fd, drmModeResPtr resources)
{
	int i, rc;

	for (i = 0; i < resources->count_crtcs; i++) {
		rc = drmModeSetCrtc(drm_fd, resources->crtcs[i], 0, 0, 0, NULL,
				    0, NULL);
		igt_assert(rc == 0);
	}
}

/**
 * kmstest_get_crtc_idx:
 * @res: the libdrm resources
 * @crtc_id: the CRTC id
 *
 * Get the CRTC index based on its ID. This is useful since a few places of
 * libdrm deal with CRTC masks.
 *
 * Returns: CRTC index for a given @crtc_id
 */
int kmstest_get_crtc_idx(drmModeRes *res, uint32_t crtc_id)
{
	int i;

	for (i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == crtc_id)
			return i;

	igt_assert(false);
}

/**
 * kmstest_get_vblank:
 * @fd: Opened drm file descriptor
 * @crtc_index: CRTC_index
 * @flags: Flags passed to drm_ioctl_wait_vblank
 *
 * Blocks or request a signal when a specified vblank event occurs
 *
 * Returns 0 on failure or the reply vblank sequence number otherwise
 */
unsigned int kmstest_get_vblank(int fd, int crtc_index, unsigned int flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | kmstest_get_vbl_flag(crtc_index) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

/**
 * igt_crtc_get_vblank:
 * @crtc: CRTC
 * @flags: Flags passed to drm_ioctl_wait_vblank
 *
 * Blocks or request a signal when a specified vblank event occurs
 *
 * Returns 0 on failure or the reply vblank sequence number otherwise
 */
unsigned int igt_crtc_get_vblank(igt_crtc_t *crtc, unsigned int flags)
{
	return kmstest_get_vblank(crtc->display->drm_fd, crtc->crtc_index,
				  flags);
}

/**
 * kmstest_wait_for_pageflip_timeout:
 * @fd: Opened drm file descriptor
 * @timeout_us: timeout used for waiting
 *
 * Blocks until pageflip is completed
 */
void kmstest_wait_for_pageflip_timeout(int fd, uint64_t timeout_us)
{
	drmEventContext evctx = { .version = 2 };
	struct timeval timeout = { .tv_sec = 0, .tv_usec = timeout_us };
	fd_set fds;
	int ret;

	/* Wait for pageflip completion, then consume event on fd */
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	do {
		errno = 0;
		ret = select(fd + 1, &fds, NULL, NULL, &timeout);
	} while (ret < 0 && errno == EINTR);

	igt_fail_on_f(ret == 0,
		     "Exceeded timeout (%" PRIu64 " us) while waiting for a pageflip\n",
		     timeout_us);

	igt_assert_f(ret == 1,
		     "Waiting for pageflip failed with %d from select(drmfd)\n",
		     ret);

	igt_assert(drmHandleEvent(fd, &evctx) == 0);
}

/**
 * kmstest_wait_for_pageflip:
 * @fd: Opened drm file descriptor
 *
 * Blocks until pageflip is completed using a 50 ms timeout.
 */
void kmstest_wait_for_pageflip(int fd)
{
	kmstest_wait_for_pageflip_timeout(fd, 50000);
}

/**
 * kms_has_vblank:
 * @fd: DRM fd
 *
 * Get the VBlank errno after an attempt to call drmWaitVBlank(). This
 * function is useful for checking if a driver has support or not for VBlank.
 *
 * Returns: True if target driver has VBlank support, otherwise return false.
 */
bool kms_has_vblank(int fd)
{
	drmVBlank dummy_vbl;

	memset(&dummy_vbl, 0, sizeof(drmVBlank));
	dummy_vbl.request.type = DRM_VBLANK_RELATIVE;

	errno = 0;
	drmWaitVBlank(fd, &dummy_vbl);
	return (errno != EOPNOTSUPP);
}

/*
 * A small modeset API
 */
#define LOG_SPACES		"    "
#define LOG_N_SPACES		(sizeof(LOG_SPACES) - 1)

#define LOG_INDENT(d, section)				\
	do {						\
		igt_display_log(d, "%s {\n", section);	\
		igt_display_log_shift(d, 1);		\
	} while (0)
#define LOG_UNINDENT(d)					\
	do {						\
		igt_display_log_shift(d, -1);		\
		igt_display_log(d, "}\n");		\
	} while (0)
#define LOG(d, fmt, ...)	igt_display_log(d, fmt, ## __VA_ARGS__)

static void  __attribute__((format(printf, 2, 3)))
igt_display_log(igt_display_t *display, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	igt_debug("display: ");
	for (i = 0; i < display->log_shift; i++)
		igt_debug("%s", LOG_SPACES);
	igt_vlog(IGT_LOG_DOMAIN, IGT_LOG_DEBUG, fmt, args);
	va_end(args);
}

static void igt_display_log_shift(igt_display_t *display, int shift)
{
	display->log_shift += shift;
	igt_assert(display->log_shift >= 0);
}

/**
 * igt_output_refresh:
 * @output: Target output
 *
 * This function sets the given @output to a valid default pipe
 */
void igt_output_refresh(igt_output_t *output)
{
	igt_display_t *display = output->display;
	unsigned long crtc_idx_mask = 0;

	if (output->pending_crtc)
		crtc_idx_mask = 1 << output->pending_crtc->crtc_index;

	kmstest_free_connector_config(&output->config);

	_kmstest_connector_config(display->drm_fd, output->id, crtc_idx_mask,
				  &output->config, output->force_reprobe);
	output->force_reprobe = false;

	if (!output->name && output->config.connector) {
		drmModeConnector *c = output->config.connector;

		igt_assert_neq(asprintf(&output->name, "%s-%d", kmstest_connector_type_str(c->connector_type), c->connector_type_id),
			       -1);
	}

	if (output->config.connector)
		igt_atomic_fill_connector_props(display, output,
			IGT_NUM_CONNECTOR_PROPS, igt_connector_prop_names);

	LOG(display, "%s: Selecting CRTC %s\n", output->name,
	    igt_crtc_name(output->pending_crtc));
}

static int
igt_plane_set_property(igt_plane_t *plane, uint32_t prop_id, uint64_t value)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	return drmModeObjectSetProperty(display->drm_fd, plane->drm_plane->plane_id,
				 DRM_MODE_OBJECT_PLANE, prop_id, value);
}

/*
 * Walk a plane's property list to determine its type.  If we don't
 * find a type property, then the kernel doesn't support universal
 * planes and we know the plane is an overlay/sprite.
 */
static int get_drm_plane_type(int drm_fd, uint32_t plane_id)
{
	uint64_t value;
	bool has_prop;

	has_prop = kmstest_get_property(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
					"type", NULL, &value, NULL);
	if (has_prop)
		return (int)value;

	return DRM_PLANE_TYPE_OVERLAY;
}

static void igt_plane_reset(igt_plane_t *plane)
{
	igt_display_t *display = plane->crtc->display;

	/* Reset src coordinates. */
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_X, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_Y, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_W, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_H, 0);

	/* Reset crtc coordinates. */
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_X, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_Y, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_W, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_H, 0);

	/* Reset binding to fb and crtc. */
	igt_plane_set_prop_value(plane, IGT_PLANE_FB_ID, 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_ID, 0);

	if (!display->has_plane_color_pipeline) {
		if (igt_plane_has_prop(plane, IGT_PLANE_COLOR_ENCODING))
			igt_plane_set_prop_enum(plane, IGT_PLANE_COLOR_ENCODING,
				igt_color_encoding_to_str(IGT_COLOR_YCBCR_BT601));

		if (igt_plane_has_prop(plane, IGT_PLANE_COLOR_RANGE))
			igt_plane_set_prop_enum(plane, IGT_PLANE_COLOR_RANGE,
				igt_color_range_to_str(IGT_COLOR_YCBCR_LIMITED_RANGE));
	}

	/* Use default rotation */
	if (igt_plane_has_prop(plane, IGT_PLANE_ROTATION))
		igt_plane_set_prop_value(plane, IGT_PLANE_ROTATION, IGT_ROTATION_0);

	if (igt_plane_has_prop(plane, IGT_PLANE_PIXEL_BLEND_MODE))
		igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "Pre-multiplied");

	if (igt_plane_has_prop(plane, IGT_PLANE_ALPHA))
		igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0xffff);

	if (igt_plane_has_prop(plane, IGT_PLANE_FB_DAMAGE_CLIPS))
		igt_plane_set_prop_value(plane, IGT_PLANE_FB_DAMAGE_CLIPS, 0);

	if (igt_plane_has_prop(plane, IGT_PLANE_SCALING_FILTER))
		igt_plane_set_prop_enum(plane, IGT_PLANE_SCALING_FILTER, "Default");

	if (igt_plane_has_prop(plane, IGT_PLANE_HOTSPOT_X))
		igt_plane_set_prop_value(plane, IGT_PLANE_HOTSPOT_X, 0);
	if (igt_plane_has_prop(plane, IGT_PLANE_HOTSPOT_Y))
		igt_plane_set_prop_value(plane, IGT_PLANE_HOTSPOT_Y, 0);

	if (igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE))
		igt_plane_set_color_pipeline(plane, NULL);

	igt_plane_clear_prop_changed(plane, IGT_PLANE_IN_FENCE_FD);
	plane->values[IGT_PLANE_IN_FENCE_FD] = ~0ULL;
	plane->gem_handle = 0;
}

static void igt_crtc_reset(igt_crtc_t *crtc)
{
	igt_crtc_set_prop_value(crtc, IGT_CRTC_MODE_ID, 0);
	igt_crtc_set_prop_value(crtc, IGT_CRTC_ACTIVE, 0);
	igt_crtc_clear_prop_changed(crtc, IGT_CRTC_OUT_FENCE_PTR);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_CTM))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_CTM, 0);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_GAMMA_LUT))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_GAMMA_LUT, 0);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_DEGAMMA_LUT))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_DEGAMMA_LUT, 0);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_SCALING_FILTER))
		igt_crtc_set_prop_enum(crtc, IGT_CRTC_SCALING_FILTER,
				       "Default");

	if (igt_crtc_has_prop(crtc, IGT_CRTC_VRR_ENABLED))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_VRR_ENABLED, 0);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_SHARPNESS_STRENGTH))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_SHARPNESS_STRENGTH, 0);

	if (igt_crtc_has_prop(crtc, IGT_CRTC_BACKGROUND_COLOR))
		igt_crtc_set_prop_value(crtc, IGT_CRTC_BACKGROUND_COLOR,
					DRM_ARGB64_PREP(0xffff, 0, 0, 0));

	crtc->out_fence_fd = -1;
}

static void igt_output_reset(igt_output_t *output)
{
	output->pending_crtc = NULL;
	output->use_override_mode = false;
	memset(&output->override_mode, 0, sizeof(output->override_mode));

	igt_output_set_prop_value(output, IGT_CONNECTOR_CRTC_ID, 0);

	if (igt_output_has_prop(output, IGT_CONNECTOR_BROADCAST_RGB))
		igt_output_set_prop_value(output, IGT_CONNECTOR_BROADCAST_RGB,
					  BROADCAST_RGB_FULL);

	if (igt_output_has_prop(output, IGT_CONNECTOR_CONTENT_PROTECTION))
		igt_output_set_prop_enum(output, IGT_CONNECTOR_CONTENT_PROTECTION,
					 "Undesired");

	if (igt_output_has_prop(output, IGT_CONNECTOR_HDR_OUTPUT_METADATA))
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_HDR_OUTPUT_METADATA, 0);

	if (igt_output_has_prop(output, IGT_CONNECTOR_WRITEBACK_FB_ID))
		igt_output_set_prop_value(output, IGT_CONNECTOR_WRITEBACK_FB_ID, 0);
	if (igt_output_has_prop(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR))
		igt_output_clear_prop_changed(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR);
	output->writeback_out_fence_fd = -1;
	if (igt_output_has_prop(output, IGT_CONNECTOR_DITHERING_MODE))
		igt_output_set_prop_enum(output, IGT_CONNECTOR_DITHERING_MODE,
					 "off");
}

/**
 * igt_display_reset:
 * @display: a pointer to an #igt_display_t structure
 *
 * Reset basic pipes, connectors and planes on @display back to default values.
 * In particular, the following properties will be reset:
 *
 * For outputs:
 * - %IGT_CONNECTOR_CRTC_ID
 * - %IGT_CONNECTOR_BROADCAST_RGB (if applicable)
 *   %IGT_CONNECTOR_CONTENT_PROTECTION (if applicable)
 *   %IGT_CONNECTOR_HDR_OUTPUT_METADATA (if applicable)
 * - %IGT_CONNECTOR_DITHERING_MODE (if applicable)
 * - igt_output_override_mode() to default.
 *
 * For pipes:
 * - %IGT_CRTC_MODE_ID (leaked)
 * - %IGT_CRTC_ACTIVE
 * - %IGT_CRTC_OUT_FENCE_PTR
 *
 * For planes:
 * - %IGT_PLANE_SRC_*
 * - %IGT_PLANE_CRTC_*
 * - %IGT_PLANE_FB_ID
 * - %IGT_PLANE_CRTC_ID
 * - %IGT_PLANE_ROTATION
 * - %IGT_PLANE_IN_FENCE_FD
 */
void igt_display_reset(igt_display_t *display)
{
	igt_crtc_t *crtc;
	int i;

	/*
	 * Allow resetting rotation on all planes, which is normally
	 * prohibited on the primary and cursor plane for legacy commits.
	 */
	display->first_commit = true;

	for_each_crtc(display, crtc) {
		igt_plane_t *plane;

		for_each_plane_on_crtc(crtc,
				       plane)
			igt_plane_reset(plane);

		igt_crtc_reset(crtc);
	}

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		igt_output_reset(output);
	}
}

static void igt_fill_plane_format_mod(igt_display_t *display, igt_plane_t *plane);
static void igt_fill_display_format_mod(igt_display_t *display);

static bool igt_crtc_has_valid_output(igt_crtc_t *crtc)
{
	igt_display_t *display = crtc->display;
	igt_output_t *output;

	for_each_valid_output_on_crtc(display, crtc, output)
		return true;

	return false;
}

/**
 * igt_handle_spurious_hpd:
 * @display: a pointer to igt_display_t structure
 *
 * Handle environment variable "IGT_KMS_IGNORE_HPD" to manage the spurious
 * HPD cases in CI systems where such spurious HPDs are generated by the
 * panels without any specific reasons and cause CI execution failures.
 *
 * This will set the i915_ignore_long_hpd debugfs entry to 1 as a cue for
 * the driver to start ignoring the HPDs.
 *
 * Also, this will set the active connectors' force status to "on"
 * so that dp/hdmi_detect routines don't get called frequently.
 *
 * Force status is kept on after this until it is manually reset.
 */
static void igt_handle_spurious_hpd(igt_display_t *display)
{
	igt_output_t *output;

	/* Proceed with spurious HPD handling only if the env var is set */
	if (!getenv("IGT_KMS_IGNORE_HPD"))
		return;

	/* Set the ignore HPD for the driver */
	if (!igt_ignore_long_hpd(display->drm_fd, true)) {
		igt_info("Unable set the ignore HPD debugfs entry \n");
		return;
	}

	for_each_connected_output(display, output) {
		drmModeConnector *conn = output->config.connector;

		if (!force_connector(display->drm_fd, conn, "on")) {
			igt_info("Unable to force state on %s-%d\n",
				 kmstest_connector_type_str(conn->connector_type),
				 conn->connector_type_id);
			continue;
		}

		igt_info("Force connector ON for %s-%d\n",
			 kmstest_connector_type_str(conn->connector_type),
			 conn->connector_type_id);
	}

	dump_connector_attrs();
}

/**
 * igt_display_reset_outputs:
 * @display: a pointer to an initialized #igt_display_t structure
 *
 * Initialize @display outputs with their connectors and pipes.
 * This function clears any previously allocated outputs.
 */
void igt_display_reset_outputs(igt_display_t *display)
{
	igt_crtc_t *crtc;
	int i;
	drmModeRes *resources;

	/* Clear any existing outputs*/
	if (display->n_outputs) {
		for (i = 0; i < display->n_outputs; i++) {
			struct kmstest_connector_config *config =
				&display->outputs[i].config;
			drmModeFreeConnector(config->connector);
			drmModeFreeEncoder(config->encoder);
			drmModeFreeCrtc(config->crtc);
			free(config->connector_path);
		}
		free(display->outputs);
		display->n_outputs = 0;
	}

	resources = drmModeGetResources(display->drm_fd);
	if (!resources)
		return;

	display->n_outputs = resources->count_connectors;
	display->outputs = calloc(display->n_outputs, sizeof(igt_output_t));
	igt_assert_f(display->outputs,
		     "Failed to allocate memory for %d outputs\n",
		     display->n_outputs);

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];
		drmModeConnector *connector;

		/*
		 * We don't assign each output a pipe unless
		 * a CRTC is set with igt_output_set_crtc().
		 */
		output->pending_crtc = NULL;
		output->id = resources->connectors[i];
		output->display = display;

		igt_output_refresh(output);

		connector = output->config.connector;
		if (connector &&
		    (!connector->count_modes ||
		     connector->connection == DRM_MODE_UNKNOWNCONNECTION)) {
			output->force_reprobe = true;
			igt_output_refresh(output);
		}
	}

	/* Set reasonable default values for every object in the
	 * display. */
	igt_display_reset(display);

	for_each_crtc(display, crtc) {
		igt_output_t *output;

		if (!igt_crtc_has_valid_output(crtc))
			continue;

		output = igt_get_single_output_for_crtc(crtc);

		if (crtc->num_primary_planes > 1) {
			igt_plane_t *old_primary = &crtc->planes[0];
			igt_plane_t *new_primary =
				igt_get_assigned_primary(output, crtc);

			/*
			 * If the driver-assigned primary plane isn't at
			 * index 0, swap it with the plane that's currently
			 * at index 0 and update the indexes accordingly.
			 *
			 * This way, the primary plane is always at index 0.
			 */
			if (new_primary->index != 0) {
				igt_assert(old_primary != new_primary);

				igt_assert_eq(old_primary->index, 0);
				igt_assert_neq(new_primary->index, 0);

				igt_swap(*old_primary, *new_primary);
				igt_swap(old_primary->index, new_primary->index);
				igt_swap(old_primary, new_primary);

				igt_assert_neq(old_primary->index, 0);
				igt_assert_eq(new_primary->index, 0);
			} else {
				igt_assert(old_primary == new_primary);

				igt_assert_eq(old_primary->index, 0);
			}
		}
	}

	drmModeFreeResources(resources);
}

static int plane_type_index(igt_crtc_t *crtc, int type)
{
	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		return 0;
	case DRM_PLANE_TYPE_CURSOR:
		return crtc->n_planes - 1;
	default:
		return -1;
	}
}

static void igt_crtc_plane_init(igt_display_t *display,
				igt_crtc_t *crtc,
				drmModeRes *resources,
				igt_plane_t *global_plane)
{
	drmModePlane *drm_plane = global_plane->drm_plane;
	int type = global_plane->type;
	igt_plane_t *plane;
	int index;

	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		crtc->num_primary_planes++;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		display->has_cursor_plane = true;
		break;
	default:
		break;
	}

	index = plane_type_index(crtc, type);

	if (index < 0 || crtc->planes[index].index >= 0) {
		for (index = 1; index < crtc->n_planes; index++) {
			if (crtc->planes[index].index < 0)
				break;
		}
	}

	igt_assert_lt(index, crtc->n_planes);

	plane = &crtc->planes[index];

	igt_assert_lt(plane->index, 0);

	plane->index = index;
	plane->type = type;
	plane->crtc = crtc;
	plane->drm_plane = drm_plane;
	plane->values[IGT_PLANE_IN_FENCE_FD] = ~0ULL;
	plane->ref = global_plane;

	/*
	 * HACK: point the global plane to the first pipe that
	 * it can go on.
	 */
	if (!global_plane->ref)
		igt_plane_set_crtc(plane, crtc);

	igt_fill_plane_props(display, plane, IGT_NUM_PLANE_PROPS, igt_plane_prop_names);

	igt_fill_plane_format_mod(display, plane);
}

static void igt_crtc_init(igt_display_t *display, drmModeRes *resources, igt_crtc_t *crtc)
{
	int crtc_mask;
	int j;

	crtc->display = display;
	crtc->planes = NULL;
	crtc->num_primary_planes = 0;

	igt_fill_crtc_props(crtc, IGT_NUM_CRTC_PROPS, igt_crtc_prop_names);

	crtc_mask = 1 << crtc->crtc_index;

	/* count number of valid planes */
	for (j = 0; j < display->n_planes; j++) {
		drmModePlane *drm_plane = display->planes[j].drm_plane;
		igt_assert(drm_plane);

		if (drm_plane->possible_crtcs & crtc_mask)
			crtc->n_planes++;
	}

	igt_assert_lt(0, crtc->n_planes);
	crtc->planes = calloc(crtc->n_planes, sizeof(igt_plane_t));
	igt_assert_f(crtc->planes,
		     "Failed to allocate memory for %d planes\n",
		     crtc->n_planes);

	for (j = 0; j < crtc->n_planes; j++)
		crtc->planes[j].index = -1;

	/* add the planes that can be used with that pipe */
	for (j = 0; j < display->n_planes; j++) {
		igt_plane_t *global_plane = &display->planes[j];
		drmModePlane *drm_plane = global_plane->drm_plane;

		if (!(drm_plane->possible_crtcs & crtc_mask))
			continue;

		igt_crtc_plane_init(display, crtc, resources, global_plane);
	}

	for (j = 0; j < crtc->n_planes; j++)
		igt_assert_lte(0, crtc->planes[j].index);
}

static int crtc_pipe_compare(const void *_a, const void *_b)
{
	const igt_crtc_t *a = _a, *b = _b;

	return a->hardware_pipe - b->hardware_pipe;
}

/**
 * igt_display_require:
 * @display: a pointer to an #igt_display_t structure
 * @drm_fd: a drm file descriptor
 *
 * Initialize @display and allocate the various resources required. Use
 * #igt_display_fini to release the resources when they are no longer
 * required.
 *
 * This function automatically skips if the kernel driver doesn't
 * support any CRTC or outputs.
 */
void igt_display_require(igt_display_t *display, int drm_fd)
{
	drmModeRes *resources;
	drmModePlaneRes *plane_resources;
	igt_crtc_t *crtc;
	int i, crtc_index;
	bool is_intel_dev;

#ifdef HAVE_UNIGRAF
	unigraf_open_device(drm_fd);
#endif

	memset(display, 0, sizeof(igt_display_t));

	LOG_INDENT(display, "init");

	display->drm_fd = drm_fd;
	is_intel_dev = is_intel_device(drm_fd);

	igt_require(igt_has_drm_cap(drm_fd, DRM_CAP_VBLANK_HIGH_CRTC));

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
		display->is_atomic = 1;

	resources = drmModeGetResources(display->drm_fd);
	if (!resources)
		goto out;

#ifdef HAVE_CHAMELIUM
	{
		struct chamelium *chamelium;

		chamelium = chamelium_init_rpc_only();
		if (chamelium) {
			igt_abort_on_f(!chamelium_wait_reachable(chamelium, 20),
				       "cannot reach the configured chamelium!\n");
			igt_abort_on_f(!chamelium_plug_all(chamelium),
				       "failed to plug all the chamelium ports!\n");
			igt_abort_on_f(!chamelium_wait_all_configured_ports_connected(chamelium, drm_fd),
				       "not all configured chamelium ports are connected!\n");
			chamelium_deinit_rpc_only(chamelium);
		}
	}
#endif

	igt_require_f(resources->count_crtcs <= IGT_MAX_PIPES,
		     "count_crtcs exceeds IGT_MAX_PIPES, resources->count_crtcs=%d, IGT_MAX_PIPES=%d\n",
		     resources->count_crtcs, IGT_MAX_PIPES);

	display->n_crtcs = resources->count_crtcs;
	display->crtcs = calloc(igt_display_n_crtcs(display), sizeof(igt_crtc_t));
	igt_assert_f(display->crtcs,
		     "Failed to allocate memory for %d CRTCs\n",
		     igt_display_n_crtcs(display));

	for (crtc_index = 0; crtc_index < resources->count_crtcs; crtc_index++) {
		crtc = &display->crtcs[crtc_index];

		crtc->crtc_id = resources->crtcs[crtc_index];
		crtc->crtc_index = crtc_index;
		crtc->hardware_pipe = is_intel_dev ? __intel_get_pipe_from_crtc_index(drm_fd, crtc_index) : crtc_index;
	}

	/* For Intel, sort the CRTCs in pipe order */
	if (is_intel_dev)
		qsort(display->crtcs, igt_display_n_crtcs(display),
		      sizeof(display->crtcs[0]), crtc_pipe_compare);

	drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	if (drmSetClientCap(drm_fd, LOCAL_DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1) == 0)
		display->has_virt_cursor_plane = 1;

	plane_resources = drmModeGetPlaneResources(display->drm_fd);
	igt_assert(plane_resources);

	display->n_planes = plane_resources->count_planes;
	display->planes = calloc(display->n_planes, sizeof(igt_plane_t));
	igt_assert_f(display->planes, "Failed to allocate memory for %d planes\n", display->n_planes);

	for (i = 0; i < plane_resources->count_planes; ++i) {
		igt_plane_t *plane = &display->planes[i];
		uint32_t id = plane_resources->planes[i];

		plane->drm_plane = drmModeGetPlane(display->drm_fd, id);
		igt_assert(plane->drm_plane);

		plane->type = get_drm_plane_type(display->drm_fd, id);
	}

	drmModeFreePlaneResources(plane_resources);

	/* init colorops */
	display->colorops = calloc(MAX_NUM_COLOROPS, sizeof(igt_colorop_t));
	display->n_colorops = 0;

	for_each_crtc(display, crtc)
		igt_crtc_init(display, resources, crtc);

	drmModeFreeResources(resources);

	igt_fill_display_format_mod(display);

	igt_display_reset_outputs(display);

out:
	LOG_UNINDENT(display);

	if (igt_display_n_crtcs(display) && display->n_outputs) {
		igt_enable_connectors(drm_fd);

		igt_handle_spurious_hpd(display);
	}
	else {
		igt_skip("No KMS driver or no outputs, pipes: %d, outputs: %d\n",
			 igt_display_n_crtcs(display), display->n_outputs);
	}
}

/**
 * igt_display_n_crtcs:
 * @display: A pointer to an #igt_display_t structure
 *
 * Returns: Total number of CRTCs for the given @display
 */
int igt_display_n_crtcs(igt_display_t *display)
{
	return display->n_crtcs;
}

/**
 * igt_display_require_output:
 * @display: A pointer to an #igt_display_t structure
 *
 * Checks whether there's a valid @pipe/@output combination for the given @display
 * Skips test if a valid combination of @pipe and @output is not found
 */
void igt_display_require_output(igt_display_t *display)
{
	igt_crtc_t *crtc;
	igt_output_t *output;

	for_each_crtc_with_valid_output(display, crtc, output)
		return;

	igt_skip("No valid crtc/connector combinations found.\n");
}

/**
 * igt_display_require_output_on_crtc:
 * @crtc: CRTC
 *
 * Checks whether there's a valid @crtc/@output combination for the given @crtc
 */
void igt_display_require_output_on_crtc(igt_crtc_t *crtc)
{
	if (!igt_crtc_has_valid_output(crtc))
		igt_skip("No valid connector found on CRTC %s\n",
			 igt_crtc_name(crtc));
}

/**
 * igt_output_from_connector:
 * @display: a pointer to an #igt_display_t structure
 * @connector: a pointer to a drmModeConnector
 *
 * Finds the output corresponding to the given connector
 *
 * Returns: A #igt_output_t structure configured to use the connector, or NULL
 * if none was found
 */
igt_output_t *igt_output_from_connector(igt_display_t *display,
					drmModeConnector *connector)
{
	int i;
	igt_output_t *found = NULL;
	igt_assert(display);
	igt_assert(connector);

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];
		bool is_mst = !!output->config.connector_path;

		if (is_mst) {
			drmModePropertyBlobPtr path_blob =
				kmstest_get_path_blob(display->drm_fd,
						      connector->connector_id);
			if (path_blob) {
				bool is_same_connector =
					strcmp(output->config.connector_path,
					       path_blob->data) == 0;
				drmModeFreePropertyBlob(path_blob);
				if (is_same_connector) {
					output->id = connector->connector_id;
					found = output;
					break;
				}
			}

		} else {
			if (output->config.connector &&
			    output->config.connector->connector_id ==
				    connector->connector_id) {
				found = output;
				break;
			}
		}
	}

	return found;
}

/**
 * igt_std_1024_mode_get:
 * @vrefresh: Required refresh rate for 1024 mode
 *
 * This function will create a standard drm mode with a given @vrefresh
 *
 * Returns: Standard 1024@vrefresh mode.
 */
drmModeModeInfo *igt_std_1024_mode_get(int vrefresh)
{
	const drmModeModeInfo std_1024_mode = {
		.clock = 65000 * vrefresh / 60,
		.hdisplay = 1024,
		.hsync_start = 1048,
		.hsync_end = 1184,
		.htotal = 1344,
		.hskew = 0,
		.vdisplay = 768,
		.vsync_start = 771,
		.vsync_end = 777,
		.vtotal = 806,
		.vscan = 0,
		.vrefresh = vrefresh,
		.flags = 0xA,
		.type = 0x40,
		.name = "Custom 1024x768",
	};

	return igt_memdup(&std_1024_mode, sizeof(std_1024_mode));
}

/**
 * igt_modeset_disable_all_outputs:
 * @diplay: igt display structure
 *
 * Modeset to disable all output
 *
 * We need to do a modeset disabling all output to get the next
 * HPD event on TypeC port
 */
void igt_modeset_disable_all_outputs(igt_display_t *display)
{
	int i;

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		igt_output_set_crtc(output, NULL);
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

}

static void igt_crtc_fini(igt_crtc_t *crtc)
{
	free(crtc->planes);
	crtc->planes = NULL;

	if (crtc->out_fence_fd != -1)
		close(crtc->out_fence_fd);
}

static void igt_output_fini(igt_output_t *output)
{
	kmstest_free_connector_config(&output->config);
	free(output->name);
	output->name = NULL;

	if (output->writeback_out_fence_fd != -1) {
		close(output->writeback_out_fence_fd);
		output->writeback_out_fence_fd = -1;
	}
}

/**
 * igt_display_fini:
 * @display: a pointer to an #igt_display_t structure
 *
 * Release any resources associated with @display. This does not free @display
 * itself.
 */
void igt_display_fini(igt_display_t *display)
{
	igt_crtc_t *crtc;
	int i;

	for (i = 0; i < display->n_planes; ++i) {
		igt_plane_t *plane = &display->planes[i];

		if (plane->drm_plane) {
			drmModeFreePlane(plane->drm_plane);
			plane->drm_plane = NULL;
		}
	}

	for_each_crtc(display, crtc)
		igt_crtc_fini(crtc);

	for (i = 0; i < display->n_outputs; i++)
		igt_output_fini(&display->outputs[i]);

	free(display->outputs);
	display->outputs = NULL;
	free(display->crtcs);
	display->crtcs = NULL;
	free(display->planes);
	display->planes = NULL;
	free(display->colorops);
	display->colorops = NULL;
}

static void igt_display_refresh(igt_display_t *display)
{
	igt_output_t *output;
	unsigned int crtc_index_in_use_mask = 0;
	int i;

       /* Check that two outputs aren't trying to use the same pipe */
	for (i = 0; i < display->n_outputs; i++) {
		output = &display->outputs[i];

		if (output->pending_crtc) {
			unsigned int crtc_index_mask = 1 << output->pending_crtc->crtc_index;

			if (crtc_index_in_use_mask & crtc_index_mask)
				goto report_dup;

			crtc_index_in_use_mask |= crtc_index_mask;
		}

		if (output->force_reprobe)
			igt_output_refresh(output);
	}

	return;

report_dup:
	for (; i > 0; i--) {
		igt_output_t *b = &display->outputs[i - 1];

		if (!b->pending_crtc)
			continue;

		igt_assert_f(output->pending_crtc != b->pending_crtc,
			     "%s and %s are both trying to use CRTC %s\n",
			     igt_output_name(output), igt_output_name(b),
			     igt_crtc_name(output->pending_crtc));
	}
}

/*
 * Return the pending CRTC (i.e. the CRTC that should drive this output after
 * the commit(), or NULL if the user hasn't specified a CRTC to use.
 */
igt_crtc_t *igt_output_get_driving_crtc(igt_output_t *output)
{
	return output->pending_crtc;
}

static igt_plane_t *igt_crtc_get_plane(igt_crtc_t *crtc, int plane_idx)
{
	igt_require_f(plane_idx >= 0 && plane_idx < crtc->n_planes,
		      "Valid pipe->planes plane_idx not found, plane_idx=%d n_planes=%d",
		      plane_idx, crtc->n_planes);

	return &crtc->planes[plane_idx];
}

/**
 * igt_crtc_get_plane_type:
 * @crtc: Target CRTC
 * @plane_type: Cursor, primary or an overlay plane
 *
 * Finds a valid plane type for the given @crtc otherwise
 * it skips the test if the right combination of @crtc/@plane_type is not found
 *
 * Returns: A #igt_plane_t structure that matches the requested plane type
 */
igt_plane_t *igt_crtc_get_plane_type(igt_crtc_t *crtc, int plane_type)
{
	int plane_idx = plane_type_index(crtc, plane_type);

	if (plane_idx < 0) {
		for (plane_idx = 0; plane_idx < crtc->n_planes; plane_idx++) {
			if (crtc->planes[plane_idx].type == plane_type)
				break;
		}
	}

	igt_require_f(plane_idx >= 0 && plane_idx < crtc->n_planes,
		      "Valid pipe->planes idx not found. plane_idx=%d plane_type=%d n_planes=%d\n",
		      plane_idx, plane_type, crtc->n_planes);

	igt_require_f(crtc->planes[plane_idx].type == plane_type,
		      "Unexpected plane type, found plane_idx=%d, "
		      "type=%d != requested plane_type=%d (%s)\n", plane_idx,
		      crtc->planes[plane_idx].type,
		      plane_type,
		      plane_type == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY" : "CURSOR");

	return &crtc->planes[plane_idx];
}

/**
 * igt_crtc_count_plane_type:
 * @crtc: Target CRTC
 * @plane_type: Cursor, primary or an overlay plane
 *
 * Counts the number of planes of type @plane_type for the provided @crtc.
 *
 * Returns: The number of planes that match the requested plane type
 */
int igt_crtc_count_plane_type(igt_crtc_t *crtc, int plane_type)
{
	int i, count = 0;

	for(i = 0; i < crtc->n_planes; i++)
		if (crtc->planes[i].type == plane_type)
			count++;

	return count;
}

/**
 * igt_crtc_get_plane_type_index:
 * @crtc: Target CRTC
 * @plane_type: Cursor, primary or an overlay plane
 * @index: the index of the plane among planes of the same type
 *
 * Get the @index th plane of type @plane_type for the provided @crtc.
 *
 * Returns: The @index th plane that matches the requested plane type
 */
igt_plane_t *igt_crtc_get_plane_type_index(igt_crtc_t *crtc, int plane_type,
					   int index)
{
	int i, type_index = 0;

	for(i = 0; i < crtc->n_planes; i++) {
		if (crtc->planes[i].type != plane_type)
			continue;

		if (type_index == index)
			return &crtc->planes[i];

		type_index++;
	}

	return NULL;
}

/**
 * output_is_internal_panel:
 * @output: Target output
 *
 * Returns: True if the given @output type is internal else False.
 */
bool output_is_internal_panel(igt_output_t *output)
{
	switch (output->config.connector->connector_type) {
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_DSI:
	case DRM_MODE_CONNECTOR_DPI:
		return true;
	default:
		return false;
	}
}

static int output_crtc_pipe_compare(const void *_a, const void *_b)
{
	const igt_output_crtc_t *a = _a, *b = _b;

	/* pipe order for valid output/crtc combos */
	if (a->crtc && b->crtc)
		return a->crtc->hardware_pipe - b->crtc->hardware_pipe;

	/* valid combos before empty elements */
	return !b->crtc - !a->crtc;
}

igt_output_crtc_t *__igt_output_crtc_populate(igt_display_t *display, igt_output_crtc_t *chosen_outputs)
{
	unsigned int full_crtc_index_mask = 0, assigned_crtc_index_mask = 0;
	igt_output_t *output;
	igt_crtc_t *crtc;
	int i;

	memset(chosen_outputs, 0,
	       sizeof(*chosen_outputs) * igt_display_n_crtcs(display));

	for_each_crtc(display, crtc)
		full_crtc_index_mask |= 1 << crtc->crtc_index;

	/*
	 * Try to assign all outputs to the first available CRTC for
	 * it, start with the outputs restricted to 1 pipe, then increase
	 * number of pipes until we assign connectors to all pipes.
	 */
	for (i = 0; i <= igt_display_n_crtcs(display); i++) {
		for_each_connected_output(display, output) {
			uint32_t crtc_index_mask = output->config.valid_crtc_index_mask & full_crtc_index_mask;
			bool found = false;

			if (output_is_internal_panel(output)) {
				/*
				 * Internal panel should be assigned to pipe A
				 * if possible, so make sure they're enumerated
				 * first.
				 */

				if (i)
					continue;
			} else if (__builtin_popcount(crtc_index_mask) != i) {
				continue;
			}

			for_each_crtc(display, crtc) {
				bool crtc_assigned = assigned_crtc_index_mask & (1 << crtc->crtc_index);

				if (crtc_assigned || !(crtc_index_mask & (1 << crtc->crtc_index)))
					continue;

				if (!found) {
					/* We found an unassigned CRTC, use it! */
					found = true;
					assigned_crtc_index_mask |= 1 << crtc->crtc_index;
					chosen_outputs[crtc->crtc_index].output = output;
					chosen_outputs[crtc->crtc_index].crtc = crtc;
				} else if (!chosen_outputs[crtc->crtc_index].output ||
					   output_is_internal_panel(chosen_outputs[crtc->crtc_index].output)) {
					/*
					 * Overwrite internal panel if not
					 * assigned, external outputs are faster
					 * to do modesets
					 */
					chosen_outputs[crtc->crtc_index].output = output;
					chosen_outputs[crtc->crtc_index].crtc = crtc;
				}
			}

			if (!found)
				igt_warn("Output %s could not be assigned to a pipe\n",
					 igt_output_name(output));
		}
	}

	/* For Intel, sort the output/crtc combos in pipe order */
	if (is_intel_device(display->drm_fd))
		qsort(chosen_outputs, igt_display_n_crtcs(display),
		      sizeof(*chosen_outputs), output_crtc_pipe_compare);

	return chosen_outputs;
}

/**
 * igt_get_single_output_for_crtc:
 * @crtc: The CRTC for which an #igt_output_t must be returned.
 *
 * Get a compatible output for a CRTC.
 *
 * Returns: A compatible output for a given CRTC, or NULL.
 */
igt_output_t *igt_get_single_output_for_crtc(igt_crtc_t *crtc)
{
	igt_output_t *output;
	igt_crtc_t *iter;

	for_each_crtc_with_single_output(crtc->display, iter, output) {
		if (iter == crtc)
			return output;
	}

	return NULL;
}

static igt_output_t *igt_crtc_get_output(igt_crtc_t *crtc)
{
	igt_display_t *display = crtc->display;
	int i;

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (output->pending_crtc == crtc)
			return output;
	}

	return NULL;
}

static uint32_t igt_plane_get_fb_id(igt_plane_t *plane)
{
	return plane->values[IGT_PLANE_FB_ID];
}

#define CHECK_RETURN(r, fail) {	\
	if (r && !fail)		\
		return r;	\
	igt_assert_eq(r, 0);	\
}

static bool
igt_atomic_ignore_plane_prop(igt_crtc_t *crtc, uint32_t prop)
{
	igt_display_t *display = crtc->display;

	switch (prop) {
	case IGT_PLANE_COLOR_ENCODING:
	case IGT_PLANE_COLOR_RANGE:
		return display->has_plane_color_pipeline;
	case IGT_PLANE_COLOR_PIPELINE:
		return !display->has_plane_color_pipeline;
	default:
		return false;
	}
}

/*
 * Add position and fb changes of a plane to the atomic property set
 */
static void
igt_atomic_prepare_plane_commit(igt_plane_t *plane, igt_crtc_t *crtc,
				drmModeAtomicReq *req)
{
	igt_display_t *display = crtc->display;
	int i;

	igt_assert(plane->drm_plane);

	LOG(display,
	    "populating plane data: %s.%d, fb %u\n",
	    igt_crtc_name(crtc),
	    plane->index,
	    igt_plane_get_fb_id(plane));

	for (i = 0; i < IGT_NUM_PLANE_PROPS; i++) {
		if (igt_atomic_ignore_plane_prop(crtc, i)) {
			igt_debug("plane %s.%d: Ignoring property \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
				   igt_crtc_name(crtc),
				   plane->index, igt_plane_prop_names[i],
				   plane->values[i], plane->values[i]);
			continue;
		}

		if (!igt_plane_is_prop_changed(plane, i))
			continue;

		/* it's an error to try an unsupported feature */
		igt_assert(plane->props[i]);

		igt_debug("plane %s.%d: Setting property \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
			igt_crtc_name(crtc), plane->index,
			igt_plane_prop_names[i],
			plane->values[i], plane->values[i]);

		igt_assert_lt(0, drmModeAtomicAddProperty(req, plane->drm_plane->plane_id,
						  plane->props[i],
						  plane->values[i]));
	}
}

/*
 * Add colorop properties
 */
static void
igt_atomic_prepare_colorop_commit(igt_colorop_t *color_pipeline, igt_crtc_t *crtc,
				  drmModeAtomicReq *req)
{
	igt_display_t *display = crtc->display;
	igt_colorop_t *colorop;
	int i;

	for_each_colorop_in_pipeline(color_pipeline, colorop) {
		LOG(display,
		    "populating colorop data: %s.%d\n",
		    igt_crtc_name(crtc),
		    colorop->id);

		for (i = 0; i < IGT_NUM_COLOROP_PROPS; i++) {
			if (!igt_colorop_is_prop_changed(colorop, i))
				continue;

			/* it's an error to try an unsupported feature */
			igt_assert(colorop->props[i]);

			igt_debug("colorop %s.%d: Setting property \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
				igt_crtc_name(crtc), colorop->id,
				igt_colorop_prop_names[i],
				colorop->values[i], colorop->values[i]);

			igt_assert_lt(0, drmModeAtomicAddProperty(req, colorop->id,
							colorop->props[i],
							colorop->values[i]));
		}
	}
}

/*
 * Properties that can be changed through legacy SetProperty:
 * - Obviously not the XYWH SRC/CRTC coordinates.
 * - Not CRTC_ID or FENCE_ID, done through SetPlane.
 * - Can't set IN_FENCE_FD, that would be silly.
 *
 * Theoretically the above can all be set through the legacy path
 * with the atomic cap set, but that's not how our legacy plane
 * commit behaves, so blacklist it by default.
 */
#define LEGACY_PLANE_COMMIT_MASK \
	(((1ULL << IGT_NUM_PLANE_PROPS) - 1) & \
	 ~(IGT_PLANE_COORD_CHANGED_MASK | \
	   (1ULL << IGT_PLANE_FB_ID) | \
	   (1ULL << IGT_PLANE_CRTC_ID) | \
	   (1ULL << IGT_PLANE_IN_FENCE_FD)))

/*
 * Commit position and fb changes to a DRM plane via the SetPlane ioctl; if the
 * DRM call to program the plane fails, we'll either fail immediately (for
 * tests that expect the commit to succeed) or return the failure code (for
 * tests that expect a specific error code).
 */
static int igt_drm_plane_commit(igt_plane_t *plane,
				igt_crtc_t *crtc,
				bool fail_on_error)
{
	igt_display_t *display = crtc->display;
	uint32_t fb_id, crtc_id;
	int ret, i;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	int32_t crtc_x;
	int32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint64_t changed_mask;
	bool setplane =
		igt_plane_is_prop_changed(plane, IGT_PLANE_FB_ID) ||
		plane->changed & IGT_PLANE_COORD_CHANGED_MASK;

	igt_assert(plane->drm_plane);

	fb_id = igt_plane_get_fb_id(plane);
	crtc_id = crtc->crtc_id;

	if (setplane && fb_id == 0) {
		LOG(display,
		    "SetPlane pipe %s, plane %d, disabling\n",
		    igt_crtc_name(crtc),
		    plane->index);

		ret = drmModeSetPlane(display->drm_fd,
				      plane->drm_plane->plane_id,
				      crtc_id,
				      fb_id,
				      0,    /* flags */
				      0, 0, /* crtc_x, crtc_y */
				      0, 0, /* crtc_w, crtc_h */
				      IGT_FIXED(0,0), /* src_x */
				      IGT_FIXED(0,0), /* src_y */
				      IGT_FIXED(0,0), /* src_w */
				      IGT_FIXED(0,0) /* src_h */);

		CHECK_RETURN(ret, fail_on_error);
	} else if (setplane) {
		src_x = plane->values[IGT_PLANE_SRC_X];
		src_y = plane->values[IGT_PLANE_SRC_Y];
		src_w = plane->values[IGT_PLANE_SRC_W];
		src_h = plane->values[IGT_PLANE_SRC_H];
		crtc_x = plane->values[IGT_PLANE_CRTC_X];
		crtc_y = plane->values[IGT_PLANE_CRTC_Y];
		crtc_w = plane->values[IGT_PLANE_CRTC_W];
		crtc_h = plane->values[IGT_PLANE_CRTC_H];

		LOG(display,
		    "SetPlane %s.%d, fb %u, src = (%d, %d) "
			"%ux%u dst = (%u, %u) %ux%u\n",
		    igt_crtc_name(crtc),
		    plane->index,
		    fb_id,
		    src_x >> 16, src_y >> 16, src_w >> 16, src_h >> 16,
		    crtc_x, crtc_y, crtc_w, crtc_h);

		ret = drmModeSetPlane(display->drm_fd,
				      plane->drm_plane->plane_id,
				      crtc_id,
				      fb_id,
				      0,    /* flags */
				      crtc_x, crtc_y,
				      crtc_w, crtc_h,
				      src_x, src_y,
				      src_w, src_h);

		CHECK_RETURN(ret, fail_on_error);
	}

	changed_mask = plane->changed & LEGACY_PLANE_COMMIT_MASK;

	for (i = 0; i < IGT_NUM_PLANE_PROPS; i++) {
		if (!(changed_mask & (1 << i)))
			continue;

		LOG(display, "SetProp plane %s.%d \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
			igt_crtc_name(crtc), plane->index,
			igt_plane_prop_names[i],
			plane->values[i], plane->values[i]);

		igt_assert(plane->props[i]);

		ret = igt_plane_set_property(plane,
					     plane->props[i],
					     plane->values[i]);

		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

/*
 * Commit position and fb changes to a cursor via legacy ioctl's.  If commit
 * fails, we'll either fail immediately (for tests that expect the commit to
 * succeed) or return the failure code (for tests that expect a specific error
 * code).
 */
static int igt_cursor_commit_legacy(igt_plane_t *cursor,
				    igt_crtc_t *crtc,
				    bool fail_on_error)
{
	igt_display_t *display = crtc->display;
	uint32_t crtc_id = crtc->crtc_id;
	int ret;

	if (igt_plane_is_prop_changed(cursor, IGT_PLANE_FB_ID) ||
	    igt_plane_is_prop_changed(cursor, IGT_PLANE_CRTC_W) ||
	    igt_plane_is_prop_changed(cursor, IGT_PLANE_CRTC_H)) {
		if (cursor->gem_handle)
			LOG(display,
			    "SetCursor pipe %s, fb %u %dx%d\n",
			    igt_crtc_name(crtc),
			    cursor->gem_handle,
			    (unsigned)cursor->values[IGT_PLANE_CRTC_W],
			    (unsigned)cursor->values[IGT_PLANE_CRTC_H]);
		else
			LOG(display,
			    "SetCursor pipe %s, disabling\n",
			    igt_crtc_name(crtc));

		ret = drmModeSetCursor(display->drm_fd, crtc_id,
				       cursor->gem_handle,
				       cursor->values[IGT_PLANE_CRTC_W],
				       cursor->values[IGT_PLANE_CRTC_H]);
		CHECK_RETURN(ret, fail_on_error);
	}

	if (igt_plane_is_prop_changed(cursor, IGT_PLANE_CRTC_X) ||
	    igt_plane_is_prop_changed(cursor, IGT_PLANE_CRTC_Y)) {
		int x = cursor->values[IGT_PLANE_CRTC_X];
		int y = cursor->values[IGT_PLANE_CRTC_Y];

		LOG(display,
		    "MoveCursor pipe %s, (%d, %d)\n",
		    igt_crtc_name(crtc),
		    x, y);

		ret = drmModeMoveCursor(display->drm_fd, crtc_id, x, y);
		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

/*
 * Commit position and fb changes to a primary plane via the legacy interface
 * (setmode).
 */
static int igt_primary_plane_commit_legacy(igt_plane_t *primary,
					   igt_crtc_t *crtc,
					   bool fail_on_error)
{
	igt_display_t *display = primary->crtc->display;
	igt_output_t *output = igt_crtc_get_output(crtc);
	drmModeModeInfo *mode;
	uint32_t fb_id, crtc_id;
	int ret;

	/* Primary planes can't be windowed when using a legacy commit */
	igt_assert((primary->values[IGT_PLANE_CRTC_X] == 0 && primary->values[IGT_PLANE_CRTC_Y] == 0));

	/* nor rotated */
	if (!crtc->display->first_commit)
		igt_assert(!igt_plane_is_prop_changed(primary, IGT_PLANE_ROTATION));

	if (!igt_plane_is_prop_changed(primary, IGT_PLANE_FB_ID) &&
	    !(primary->changed & IGT_PLANE_COORD_CHANGED_MASK) &&
	    !igt_crtc_is_prop_changed(primary->crtc, IGT_CRTC_MODE_ID))
		return 0;

	crtc_id = crtc->crtc_id;
	fb_id = output ? igt_plane_get_fb_id(primary) : 0;
	if (fb_id)
		mode = igt_output_get_mode(output);
	else
		mode = NULL;

	if (fb_id) {
		uint32_t src_x = primary->values[IGT_PLANE_SRC_X] >> 16;
		uint32_t src_y = primary->values[IGT_PLANE_SRC_Y] >> 16;

		LOG(display,
		    "%s: SetCrtc pipe %s, fb %u, src (%d, %d), "
		    "mode %dx%d\n",
		    igt_output_name(output),
		    igt_crtc_name(crtc),
		    fb_id,
		    src_x, src_y,
		    mode->hdisplay, mode->vdisplay);

		ret = drmModeSetCrtc(display->drm_fd,
				     crtc_id,
				     fb_id,
				     src_x, src_y,
				     &output->id,
				     1,
				     mode);
	} else {
		LOG(display,
		    "SetCrtc pipe %s, disabling\n",
		    igt_crtc_name(crtc));

		ret = drmModeSetCrtc(display->drm_fd,
				     crtc_id,
				     fb_id,
				     0, 0, /* x, y */
				     NULL, /* connectors */
				     0,    /* n_connectors */
				     NULL  /* mode */);
	}

	CHECK_RETURN(ret, fail_on_error);

	return 0;
}

static int igt_plane_fixup_rotation(igt_plane_t *plane,
				    igt_crtc_t *crtc)
{
	int ret;

	if (!igt_plane_has_prop(plane, IGT_PLANE_ROTATION))
		return 0;

	LOG(crtc->display, "Fixing up initial rotation pipe %s, plane %d\n",
	    igt_crtc_name(crtc), plane->index);

	/* First try the easy case, can we change rotation without problems? */
	ret = igt_plane_set_property(plane, plane->props[IGT_PLANE_ROTATION],
				     plane->values[IGT_PLANE_ROTATION]);
	if (!ret)
		return 0;

	/* Disable the plane, while we tinker with rotation */
	ret = drmModeSetPlane(crtc->display->drm_fd,
			      plane->drm_plane->plane_id,
			      crtc->crtc_id, 0, /* fb_id */
			      0, /* flags */
			      0, 0, 0, 0, /* crtc_x, crtc_y, crtc_w, crtc_h */
			      IGT_FIXED(0,0), IGT_FIXED(0,0), /* src_x, src_y */
			      IGT_FIXED(0,0), IGT_FIXED(0,0)); /* src_w, src_h */

	if (ret && plane->type != DRM_PLANE_TYPE_PRIMARY)
		return ret;

	/* For primary plane, fall back to disabling the crtc. */
	if (ret) {
		ret = drmModeSetCrtc(crtc->display->drm_fd,
				     crtc->crtc_id, 0, 0, 0, NULL, 0, NULL);

		if (ret)
			return ret;
	}

	/* and finally, set rotation property. */
	return igt_plane_set_property(plane, plane->props[IGT_PLANE_ROTATION],
				      plane->values[IGT_PLANE_ROTATION]);
}

/*
 * Commit position and fb changes to a plane.  The value of @s will determine
 * which API is used to do the programming.
 */
static int igt_plane_commit(igt_plane_t *plane,
			    igt_crtc_t *crtc,
			    enum igt_commit_style s,
			    bool fail_on_error)
{
	igt_plane_t *plane_primary = igt_crtc_get_plane_type(crtc,
							     DRM_PLANE_TYPE_PRIMARY);

	if (crtc->display->first_commit || (s == COMMIT_UNIVERSAL &&
					    igt_plane_is_prop_changed(plane, IGT_PLANE_ROTATION))) {
		int ret;

		ret = igt_plane_fixup_rotation(plane, crtc);
		CHECK_RETURN(ret, fail_on_error);
	}

	if (plane->type == DRM_PLANE_TYPE_CURSOR && s == COMMIT_LEGACY) {
		return igt_cursor_commit_legacy(plane, crtc, fail_on_error);
	} else if (plane == plane_primary && s == COMMIT_LEGACY) {
		return igt_primary_plane_commit_legacy(plane, crtc,
						       fail_on_error);
	} else {
		return igt_drm_plane_commit(plane, crtc, fail_on_error);
	}
}

static bool is_atomic_prop(enum igt_atomic_crtc_properties prop)
{
       if (prop == IGT_CRTC_MODE_ID ||
	   prop == IGT_CRTC_ACTIVE ||
	   prop == IGT_CRTC_OUT_FENCE_PTR)
		return true;

	return false;
}

/*
 * Commit all plane changes to an output.  Note that if @s is COMMIT_LEGACY,
 * enabling/disabling the primary plane will also enable/disable the CRTC.
 *
 * If @fail_on_error is true, any failure to commit plane state will lead
 * to subtest failure in the specific function where the failure occurs.
 * Otherwise, the first error code encountered will be returned and no
 * further programming will take place, which may result in some changes
 * taking effect and others not taking effect.
 */
static int igt_crtc_commit(igt_crtc_t *crtc,
			   enum igt_commit_style s,
			   bool fail_on_error)
{
	int i;
	int ret;

	for (i = 0; i < IGT_NUM_CRTC_PROPS; i++)
		if (igt_crtc_is_prop_changed(crtc, i) &&
		    !is_atomic_prop(i)) {
			igt_assert(crtc->props[i]);

			ret = drmModeObjectSetProperty(crtc->display->drm_fd,
						       crtc->crtc_id,
						       DRM_MODE_OBJECT_CRTC,
						       crtc->props[i],
						       crtc->values[i]);

			CHECK_RETURN(ret, fail_on_error);
		}

	for (i = 0; i < crtc->n_planes; i++) {
		igt_plane_t *plane = &crtc->planes[i];

		/* skip planes that are handled by another pipe */
		if (plane->ref->crtc != crtc)
			continue;

		ret = igt_plane_commit(plane, crtc, s, fail_on_error);
		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

static int igt_output_commit(igt_output_t *output,
			     enum igt_commit_style s,
			     bool fail_on_error)
{
	int i, ret;

	for (i = 0; i < IGT_NUM_CONNECTOR_PROPS; i++) {
		if (!igt_output_is_prop_changed(output, i))
			continue;

		/* CRTC_ID is set by calling drmModeSetCrtc in the legacy path. */
		if (i == IGT_CONNECTOR_CRTC_ID)
			continue;

		igt_assert(output->props[i]);

		if (s == COMMIT_LEGACY)
			ret = drmModeConnectorSetProperty(output->display->drm_fd, output->id,
							  output->props[i], output->values[i]);
		else
			ret = drmModeObjectSetProperty(output->display->drm_fd, output->id,
						       DRM_MODE_OBJECT_CONNECTOR,
						       output->props[i], output->values[i]);

		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

static uint64_t igt_mode_object_get_prop(igt_display_t *display,
					 uint32_t object_type,
					 uint32_t object_id,
					 uint32_t prop)
{
	drmModeObjectPropertiesPtr proplist;
	bool found = false;
	int i;
	uint64_t ret;

	proplist = drmModeObjectGetProperties(display->drm_fd, object_id, object_type);
	for (i = 0; i < proplist->count_props; i++) {
		if (proplist->props[i] != prop)
			continue;

		found = true;
		break;
	}

	igt_assert(found);

	ret = proplist->prop_values[i];

	drmModeFreeObjectProperties(proplist);
	return ret;
}

/**
 * igt_plane_get_prop:
 * @plane: Target plane.
 * @prop: Property to check.
 *
 * Return current value on a plane for a given property.
 *
 * Returns: The value the property is set to, if this
 * is a blob, the blob id is returned. This can be passed
 * to drmModeGetPropertyBlob() to get the contents of the blob.
 */
uint64_t igt_plane_get_prop(igt_plane_t *plane, enum igt_atomic_plane_properties prop)
{
	igt_assert(igt_plane_has_prop(plane, prop));

	return igt_mode_object_get_prop(plane->crtc->display,
					DRM_MODE_OBJECT_PLANE,
					plane->drm_plane->plane_id, plane->props[prop]);
}

/**
 * igt_colorop_get_prop:
 * @colorop: Target colorop.
 * @prop: Property to check.
 *
 * Return current value on a colorop for a given property.
 *
 * Returns: The value the property is set to, if this
 * is a blob, the blob id is returned. This can be passed
 * to drmModeGetPropertyBlob() to get the contents of the blob.
 */
uint64_t igt_colorop_get_prop(igt_display_t *display, igt_colorop_t *colorop, enum igt_atomic_colorop_properties prop)
{
	igt_assert(igt_colorop_has_prop(colorop, prop));

	return igt_mode_object_get_prop(display, DRM_MODE_OBJECT_COLOROP,
					colorop->id, colorop->props[prop]);
}

static bool igt_mode_object_get_prop_enum_value(int drm_fd, uint32_t id, const char *str, uint64_t *val)
{
	drmModePropertyPtr prop = drmModeGetProperty(drm_fd, id);
	int i;

	igt_assert(id);
	igt_assert(prop);

	for (i = 0; i < prop->count_enums; i++)
		if (!strcmp(str, prop->enums[i].name)) {
			*val = prop->enums[i].value;
			drmModeFreeProperty(prop);
			return true;
		}

	return false;
}

/**
 * igt_plane_try_prop_enum:
 * @plane: Target plane.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * Returns: False if the given @plane doesn't have the enum @prop or
 * failed to set the enum property @val else True.
 */
bool igt_plane_try_prop_enum(igt_plane_t *plane,
			     enum igt_atomic_plane_properties prop,
			     const char *val)
{
	igt_display_t *display = plane->crtc->display;
	uint64_t uval;

	igt_assert(plane->props[prop]);

	if (!igt_mode_object_get_prop_enum_value(display->drm_fd,
						 plane->props[prop], val, &uval))
		return false;

	igt_plane_set_prop_value(plane, prop, uval);
	return true;
}

/**
 * igt_plane_set_prop_enum:
 * @plane: Target plane.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * This function tries to set given enum property @prop value @val to
 * the given @plane, and terminate the execution if its failed.
 */
void igt_plane_set_prop_enum(igt_plane_t *plane,
			     enum igt_atomic_plane_properties prop,
			     const char *val)
{
	igt_assert(igt_plane_try_prop_enum(plane, prop, val));
}

/**
 * igt_plane_check_prop_is_mutable:
 * @plane: Target plane.
 * @prop: Property to check.
 *
 * Check if a plane supports a given property and if this property is mutable.
 *
 * Returns true if the plane has the mutable property. False if the property is
 * not support or it's immutable.
 */
bool igt_plane_check_prop_is_mutable(igt_plane_t *plane,
				     enum igt_atomic_plane_properties igt_prop)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool has_prop;

	has_prop = kmstest_get_property(plane->crtc->display->drm_fd,
					plane->drm_plane->plane_id,
					DRM_MODE_OBJECT_PLANE,
				        igt_plane_prop_names[igt_prop], NULL,
					&value, &prop);
	if (!has_prop)
		return false;

	return !(prop->flags & DRM_MODE_PROP_IMMUTABLE);
}

/**
 * igt_plane_is_valid_colorop:
 * @plane: Target plane.
 * @colorop: Colorop to check.
 *
 * Returns: True if the given @colorop is a valid color pipeline on
 * the given @plane
 */
bool igt_plane_is_valid_colorop(igt_plane_t *plane, igt_colorop_t *colorop)
{
	igt_colorop_t *color_pipeline;

	for_each_color_pipeline(plane, color_pipeline) {
		if (color_pipeline == colorop)
			return true;
	}

	return false;
}

/**
 * igt_plane_set_color_pipeline:
 * @plane: Target plane.
 * @colorop: Colorop to set as color pipeline, or NULL for "Bypass".
 *
 * This function sets the given @colorop as color pipeline on @plane, or fails
 * the test if it's an invalid color pipeline for the plane. Passing NULL sets
 * the plane color pipeline to "Bypass" but keeps the previously assigned
 * pipeline, so that pending colorop changes are still submitted with the
 * commit that deactivates it, which the kernel accepts because the colorop is
 * in the plane's old state.
 */
void igt_plane_set_color_pipeline(igt_plane_t *plane, igt_colorop_t *colorop)
{
	igt_assert(!colorop || igt_plane_is_valid_colorop(plane, colorop));

	if (colorop)
		plane->assigned_color_pipeline = colorop;

	igt_plane_set_prop_enum(plane,
				IGT_PLANE_COLOR_PIPELINE,
				colorop ? colorop->name : "Bypass");
}

/**
 * igt_plane_replace_prop_blob:
 * @plane: plane to set property on.
 * @prop: property for which the blob will be replaced.
 * @ptr: Pointer to contents for the property.
 * @length: Length of contents.
 *
 * This function will destroy the old property blob for the given property,
 * and will create a new property blob with the values passed to this function.
 *
 * The new property blob will be committed when you call igt_display_commit(),
 * igt_display_commit2() or igt_display_commit_atomic().
 */
void
igt_plane_replace_prop_blob(igt_plane_t *plane, enum igt_atomic_plane_properties prop, const void *ptr, size_t length)
{
	igt_display_t *display = plane->crtc->display;
	uint64_t *blob = &plane->values[prop];
	uint32_t blob_id = 0;

	if (*blob != 0)
		igt_assert(drmModeDestroyPropertyBlob(display->drm_fd,
						      *blob) == 0);

	if (length > 0)
		igt_assert(drmModeCreatePropertyBlob(display->drm_fd,
						     ptr, length, &blob_id) == 0);

	*blob = blob_id;
	igt_plane_set_prop_changed(plane, prop);
}

/**
 * igt_colorop_replace_prop_blob:
 * @plane: colorop to set property on.
 * @prop: property for which the blob will be replaced.
 * @ptr: Pointer to contents for the property.
 * @length: Length of contents.
 *
 * This function will destroy the old property blob for the given property,
 * and will create a new property blob with the values passed to this function.
 *
 * The new property blob will be committed when you call igt_display_commit(),
 * igt_display_commit2() or igt_display_commit_atomic().
 */
void
igt_colorop_replace_prop_blob(igt_colorop_t *colorop, enum igt_atomic_colorop_properties prop, const void *ptr, size_t length)
{
	igt_display_t *display = colorop->plane->crtc->display;
	uint64_t *blob = &colorop->values[prop];
	uint32_t blob_id = 0;

	if (*blob != 0)
		igt_assert(drmModeDestroyPropertyBlob(display->drm_fd,
						      *blob) == 0);

	if (length > 0)
		igt_assert(drmModeCreatePropertyBlob(display->drm_fd,
						     ptr, length, &blob_id) == 0);

	*blob = blob_id;
	igt_colorop_set_prop_changed(colorop, prop);
}

/**
 * igt_colorop_has_prop_enum_value:
 * @colorop: Target colorop.
 * @prop: Enum property to check.
 * @val: Enum value (as string) to look up.
 *
 * Read-only counterpart to igt_colorop_try_prop_enum(): returns whether
 * @colorop's @prop accepts the enum value @val, without setting it or
 * marking @prop as changed in IGT's cache.
 *
 * Returns: true if @val is a valid value of the enum @prop on @colorop.
 */
bool igt_colorop_has_prop_enum_value(igt_colorop_t *colorop,
				     enum igt_atomic_colorop_properties prop,
				     const char *val)
{
	igt_display_t *display = colorop->plane->crtc->display;
	uint64_t uval;

	igt_assert(colorop->props[prop]);

	return igt_mode_object_get_prop_enum_value(display->drm_fd,
						   colorop->props[prop],
						   val, &uval);
}

/**
 * igt_colorop_try_prop_enum:
 * @colorop: Target colorop.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * Returns: False if the given @colorop doesn't have the enum @prop or
 * failed to set the enum property @val else True.
 */
bool igt_colorop_try_prop_enum(igt_colorop_t *colorop,
			       enum igt_atomic_colorop_properties prop,
			       const char *val)
{
	igt_display_t *display = colorop->plane->crtc->display;
	uint64_t uval;

	igt_assert(colorop->props[prop]);

	if (!igt_mode_object_get_prop_enum_value(display->drm_fd,
						 colorop->props[prop], val, &uval))
		return false;

	igt_colorop_set_prop_value(colorop, prop, uval);
	return true;
}

/**
 * igt_colorop_set_prop_enum:
 * @plane: Target plane.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * This function tries to set given enum property @prop value @val to
 * the given @colorop, and terminate the execution if its failed.
 */
void igt_colorop_set_prop_enum(igt_colorop_t *colorop,
			       enum igt_atomic_colorop_properties prop,
			       const char *val)
{
	bool result = false;

	result = igt_colorop_try_prop_enum(colorop, prop, val);
	igt_assert(result);
}

/**
 * igt_output_get_prop:
 * @output: Target output.
 * @prop: Property to return.
 *
 * Return current value on an output for a given property.
 *
 * Returns: The value the property is set to, if this
 * is a blob, the blob id is returned. This can be passed
 * to drmModeGetPropertyBlob() to get the contents of the blob.
 */
uint64_t igt_output_get_prop(igt_output_t *output, enum igt_atomic_connector_properties prop)
{
	igt_assert(igt_output_has_prop(output, prop));

	return igt_mode_object_get_prop(output->display, DRM_MODE_OBJECT_CONNECTOR,
					output->id, output->props[prop]);
}

/**
 * igt_output_try_prop_enum:
 * @output: Target output.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * Returns: False if the given @output doesn't have the enum @prop or
 * failed to set the enum property @val else True.
 */
bool igt_output_try_prop_enum(igt_output_t *output,
			      enum igt_atomic_connector_properties prop,
			      const char *val)
{
	igt_display_t *display = output->display;
	uint64_t uval;

	igt_assert(output->props[prop]);

	if (!igt_mode_object_get_prop_enum_value(display->drm_fd,
						 output->props[prop], val, &uval))
		return false;

	igt_output_set_prop_value(output, prop, uval);
	return true;
}

/**
 * igt_output_set_prop_enum:
 * @output: Target output.
 * @prop: Property to check.
 * @val: Value to set.
 *
 * This function tries to set given enum property @prop value @val to
 * the given @output, and terminate the execution if its failed.
 */
void igt_output_set_prop_enum(igt_output_t *output,
			      enum igt_atomic_connector_properties prop,
			      const char *val)
{
	igt_assert(igt_output_try_prop_enum(output, prop, val));
}

/**
 * igt_output_replace_prop_blob:
 * @output: output to set property on.
 * @prop: property for which the blob will be replaced.
 * @ptr: Pointer to contents for the property.
 * @length: Length of contents.
 *
 * This function will destroy the old property blob for the given property,
 * and will create a new property blob with the values passed to this function.
 *
 * The new property blob will be committed when you call igt_display_commit(),
 * igt_display_commit2() or igt_display_commit_atomic().
 */
void
igt_output_replace_prop_blob(igt_output_t *output, enum igt_atomic_connector_properties prop, const void *ptr, size_t length)
{
	igt_display_t *display = output->display;
	uint64_t *blob = &output->values[prop];
	uint32_t blob_id = 0;

	if (*blob != 0)
		igt_assert(drmModeDestroyPropertyBlob(display->drm_fd,
						      *blob) == 0);

	if (length > 0)
		igt_assert(drmModeCreatePropertyBlob(display->drm_fd,
						     ptr, length, &blob_id) == 0);

	*blob = blob_id;
	igt_output_set_prop_changed(output, prop);
}

/**
 * igt_crtc_get_prop:
 * @crtc: Target CRTC.
 * @prop: Property to return.
 *
 * Return current value on a CRTC for a given property.
 *
 * Returns: The value the property is set to, if this
 * is a blob, the blob id is returned. This can be passed
 * to drmModeGetPropertyBlob() to get the contents of the blob.
 */
uint64_t igt_crtc_get_prop(igt_crtc_t *crtc,
			   enum igt_atomic_crtc_properties prop)
{
	igt_assert(igt_crtc_has_prop(crtc, prop));

	return igt_mode_object_get_prop(crtc->display, DRM_MODE_OBJECT_CRTC,
					crtc->crtc_id, crtc->props[prop]);
}

/**
 * igt_crtc_try_prop_enum:
 * @crtc: Target CRTC
 * @prop: Property to check.
 * @val: Value to set.
 *
 * Returns: False if the given @crtc doesn't have the enum @prop or
 * failed to set the enum property @val else True.
 */
bool igt_crtc_try_prop_enum(igt_crtc_t *crtc,
				enum igt_atomic_crtc_properties prop,
				const char *val)
{
	igt_display_t *display = crtc->display;
	uint64_t uval;

	igt_assert(crtc->props[prop]);

	if (!igt_mode_object_get_prop_enum_value(display->drm_fd,
						 crtc->props[prop], val, &uval))
		return false;

	igt_crtc_set_prop_value(crtc, prop, uval);
	return true;
}

/**
 * igt_crtc_set_prop_enum:
 * @crtc: Target CRTC
 * @prop: Property to check.
 * @val: Value to set.
 *
 * This function tries to set given enum property @prop value @val to
 * the given @crtc, and terminate the execution if its failed.
 */
void igt_crtc_set_prop_enum(igt_crtc_t *crtc,
				enum igt_atomic_crtc_properties prop,
				const char *val)
{
	igt_assert(igt_crtc_try_prop_enum(crtc, prop, val));
}

/**
 * igt_crtc_replace_prop_blob:
 * @crtc: CRTC to set property on
 * @prop: property for which the blob will be replaced.
 * @ptr: Pointer to contents for the property.
 * @length: Length of contents.
 *
 * This function will destroy the old property blob for the given property,
 * and will create a new property blob with the values passed to this function.
 *
 * The new property blob will be committed when you call igt_display_commit(),
 * igt_display_commit2() or igt_display_commit_atomic().
 *
 * Please use igt_output_override_mode() if you want to set #IGT_CRTC_MODE_ID,
 * it works better with legacy commit.
 */
void
igt_crtc_replace_prop_blob(igt_crtc_t *crtc,
			   enum igt_atomic_crtc_properties prop,
			   const void *ptr, size_t length)
{
	igt_display_t *display = crtc->display;
	uint64_t *blob = &crtc->values[prop];
	uint32_t blob_id = 0;

	if (*blob != 0)
		igt_assert(drmModeDestroyPropertyBlob(display->drm_fd,
						      *blob) == 0);

	if (length > 0)
		igt_assert(drmModeCreatePropertyBlob(display->drm_fd,
						     ptr, length, &blob_id) == 0);

	*blob = blob_id;
	igt_crtc_set_prop_changed(crtc, prop);
}

/*
 * Add crtc property changes to the atomic property set
 */
static void igt_atomic_prepare_crtc_commit(igt_crtc_t *crtc,
					   drmModeAtomicReq *req)
{
	int i;

	for (i = 0; i < IGT_NUM_CRTC_PROPS; i++) {
		if (!igt_crtc_is_prop_changed(crtc, i))
			continue;

		igt_debug("Pipe %s: Setting property \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
			igt_crtc_name(crtc), igt_crtc_prop_names[i],
			crtc->values[i], crtc->values[i]);

		igt_assert_lt(0,
			      drmModeAtomicAddProperty(req, crtc->crtc_id, crtc->props[i], crtc->values[i]));
	}

	if (crtc->out_fence_fd != -1) {
		close(crtc->out_fence_fd);
		crtc->out_fence_fd = -1;
	}
}

/*
 * Add connector property changes to the atomic property set
 */
static void igt_atomic_prepare_connector_commit(igt_output_t *output, drmModeAtomicReq *req)
{

	int i;

	for (i = 0; i < IGT_NUM_CONNECTOR_PROPS; i++) {
		if (!igt_output_is_prop_changed(output, i))
			continue;

		/* it's an error to try an unsupported feature */
		igt_assert(output->props[i]);

		igt_debug("%s: Setting property \"%s\" to 0x%"PRIx64"/%"PRIi64"\n",
			  igt_output_name(output), igt_connector_prop_names[i],
			  output->values[i], output->values[i]);

		igt_assert_lt(0, drmModeAtomicAddProperty(req,
					  output->config.connector->connector_id,
					  output->props[i],
					  output->values[i]));
	}
}

/*
 * Commit all the changes of all the planes,crtcs, connectors
 * atomically using drmModeAtomicCommit()
 */
static int igt_atomic_commit(igt_display_t *display, uint32_t flags, void *user_data)
{

	int ret = 0, i;
	igt_crtc_t *crtc;
	drmModeAtomicReq *req;
	igt_output_t *output;

	if (display->is_atomic != 1)
		return -1;
	req = drmModeAtomicAlloc();

	for_each_crtc(display, crtc) {
		igt_plane_t *plane;

		/*
		 * Add CRTC Properties to the property set
		 */
		if (crtc->changed)
			igt_atomic_prepare_crtc_commit(crtc, req);

		for_each_plane_on_crtc(crtc,
				       plane) {
			/* skip planes that are handled by another pipe */
			if (plane->ref->crtc != crtc)
				continue;

			if (plane->changed)
				igt_atomic_prepare_plane_commit(plane, crtc,
								req);

			if (plane->assigned_color_pipeline)
				igt_atomic_prepare_colorop_commit(plane->assigned_color_pipeline,
								  crtc, req);
		}

	}

	for (i = 0; i < display->n_outputs; i++) {
		output = &display->outputs[i];

		if (!output->config.connector || !output->changed)
			continue;

		LOG(display, "%s: preparing atomic, CRTC: %s\n",
		    igt_output_name(output),
		    kmstest_pipe_name(output->config.crtc_index));

		igt_atomic_prepare_connector_commit(output, req);
	}

	ret = drmModeAtomicCommit(display->drm_fd, req, flags, user_data);

	drmModeAtomicFree(req);
	return ret;

}

static void
display_commit_changed(igt_display_t *display, enum igt_commit_style s)
{
	int i;
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		igt_plane_t *plane;

		if (s == COMMIT_ATOMIC) {
			if (igt_crtc_is_prop_changed(crtc, IGT_CRTC_OUT_FENCE_PTR))
				igt_assert(crtc->out_fence_fd >= 0);

			crtc->values[IGT_CRTC_OUT_FENCE_PTR] = 0;
			crtc->changed = 0;
		} else {
			for (i = 0; i < IGT_NUM_CRTC_PROPS; i++)
				if (!is_atomic_prop(i))
					igt_crtc_clear_prop_changed(crtc, i);

			if (s != COMMIT_UNIVERSAL) {
				igt_crtc_clear_prop_changed(crtc,
							    IGT_CRTC_MODE_ID);
				igt_crtc_clear_prop_changed(crtc,
							    IGT_CRTC_ACTIVE);
			}
		}

		for_each_plane_on_crtc(crtc,
				       plane) {
			if (s == COMMIT_ATOMIC) {
				int fd;
				igt_colorop_t *colorop;

				plane->changed = 0;

				/* clear changed flags on every colorop in the assigned chain
				 * so already-committed property values aren't re-emitted on
				 * the next commit.
				 */
				for_each_colorop_in_pipeline(plane->assigned_color_pipeline,
							     colorop)
					colorop->changed = 0;

				fd = plane->values[IGT_PLANE_IN_FENCE_FD];
				if (fd != -1)
					close(fd);

				/* reset fence_fd to prevent it from being set for the next commit */
				plane->values[IGT_PLANE_IN_FENCE_FD] = -1;
			} else {
				plane->changed &= ~IGT_PLANE_COORD_CHANGED_MASK;

				igt_plane_clear_prop_changed(plane, IGT_PLANE_CRTC_ID);
				igt_plane_clear_prop_changed(plane, IGT_PLANE_FB_ID);

				if (s != COMMIT_LEGACY ||
				    !(plane->type == DRM_PLANE_TYPE_PRIMARY ||
				      plane->type == DRM_PLANE_TYPE_CURSOR))
					plane->changed &= ~LEGACY_PLANE_COMMIT_MASK;

				if (display->first_commit)
					igt_plane_clear_prop_changed(plane, IGT_PLANE_ROTATION);
			}
		}
	}

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (s != COMMIT_UNIVERSAL)
			output->changed = 0;
		else
			/* no modeset in universal commit, no change to crtc. */
			output->changed &= 1 << IGT_CONNECTOR_CRTC_ID;

		if (s == COMMIT_ATOMIC) {
			if (igt_output_is_prop_changed(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR))
				igt_assert(output->writeback_out_fence_fd >= 0);

			output->values[IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR] = 0;
			output->values[IGT_CONNECTOR_WRITEBACK_FB_ID] = 0;
			igt_output_clear_prop_changed(output, IGT_CONNECTOR_WRITEBACK_FB_ID);
			igt_output_clear_prop_changed(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR);
		}
	}

	if (display->first_commit) {
		igt_reset_fifo_underrun_reporting(display->drm_fd);

		igt_display_drop_events(display);

		display->first_commit = false;
	}
}

/*
 * Commit all plane changes across all outputs of the display.
 *
 * If @fail_on_error is true, any failure to commit plane state will lead
 * to subtest failure in the specific function where the failure occurs.
 * Otherwise, the first error code encountered will be returned and no
 * further programming will take place, which may result in some changes
 * taking effect and others not taking effect.
 */
static int do_display_commit(igt_display_t *display,
			     enum igt_commit_style s,
			     bool fail_on_error)
{
	int i, ret = 0;
	igt_crtc_t *crtc;
	LOG_INDENT(display, "commit");

	/* someone managed to bypass igt_display_require, catch them */
	assert(igt_display_n_crtcs(display) && display->n_outputs);

	igt_display_refresh(display);

	if (s == COMMIT_ATOMIC) {
		ret = igt_atomic_commit(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	} else {
		for_each_crtc(display, crtc) {
			ret = igt_crtc_commit(crtc, s, fail_on_error);
			if (ret)
				break;
		}

		for (i = 0; !ret && i < display->n_outputs; i++)
			ret = igt_output_commit(&display->outputs[i], s, fail_on_error);
	}

	LOG_UNINDENT(display);
	CHECK_RETURN(ret, fail_on_error);

	display_commit_changed(display, s);

	igt_debug_wait_for_keypress("modeset");

	return 0;
}

/**
 * igt_display_try_commit_atomic:
 * @display: #igt_display_t to commit.
 * @flags: Flags passed to drmModeAtomicCommit.
 * @user_data: User defined pointer passed to drmModeAtomicCommit.
 *
 * This function is similar to #igt_display_try_commit2, but is
 * used when you want to pass different flags to the actual commit.
 *
 * Useful flags can be DRM_MODE_ATOMIC_ALLOW_MODESET,
 * DRM_MODE_ATOMIC_NONBLOCK, DRM_MODE_PAGE_FLIP_EVENT,
 * or DRM_MODE_ATOMIC_TEST_ONLY.
 *
 * @user_data is returned in the event if you pass
 * DRM_MODE_PAGE_FLIP_EVENT to @flags.
 *
 * This function will return an error if commit fails, instead of
 * aborting the test.
 */
int igt_display_try_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data)
{
	int ret;

	/* someone managed to bypass igt_display_require, catch them */
	assert(igt_display_n_crtcs(display) && display->n_outputs);

	LOG_INDENT(display, "commit");

	igt_display_refresh(display);

	ret = igt_atomic_commit(display, flags, user_data);

	LOG_UNINDENT(display);

	if (ret || (flags & DRM_MODE_ATOMIC_TEST_ONLY))
		return ret;

	if (display->first_commit)
		igt_fail_on_f(flags & (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK),
			      "First commit has to drop all stale events\n");

	display_commit_changed(display, COMMIT_ATOMIC);

	igt_debug_wait_for_keypress("modeset");

	return 0;
}

/**
 * igt_display_commit_atomic:
 * @display: #igt_display_t to commit.
 * @flags: Flags passed to drmModeAtomicCommit.
 * @user_data: User defined pointer passed to drmModeAtomicCommit.
 *
 * This function is similar to #igt_display_commit2, but is
 * used when you want to pass different flags to the actual commit.
 *
 * Useful flags can be DRM_MODE_ATOMIC_ALLOW_MODESET,
 * DRM_MODE_ATOMIC_NONBLOCK, DRM_MODE_PAGE_FLIP_EVENT,
 * or DRM_MODE_ATOMIC_TEST_ONLY.
 *
 * @user_data is returned in the event if you pass
 * DRM_MODE_PAGE_FLIP_EVENT to @flags.
 *
 * This function will abort the test if commit fails.
 */
void igt_display_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data)
{
	int ret = igt_display_try_commit_atomic(display, flags, user_data);

	igt_assert_eq(ret, 0);
}

/**
 * igt_display_commit2:
 * @display: DRM device handle
 * @s: Commit style
 *
 * Commits framebuffer and positioning changes to all planes of each display
 * pipe, using a specific API to perform the programming.  This function should
 * be used to exercise a specific driver programming API; igt_display_commit
 * should be used instead if the API used is unimportant to the test being run.
 *
 * This function should only be used to commit changes that are expected to
 * succeed, since any failure during the commit process will cause the IGT
 * subtest to fail.  To commit changes that are expected to fail, use
 * @igt_display_try_commit2 instead.
 *
 * Returns: 0 upon success.  This function will never return upon failure
 * since igt_fail() at lower levels will longjmp out of it.
 */
int igt_display_commit2(igt_display_t *display,
		       enum igt_commit_style s)
{
	do_display_commit(display, s, true);

	return 0;
}

/**
 * igt_display_try_commit2:
 * @display: DRM device handle
 * @s: Commit style
 *
 * Attempts to commit framebuffer and positioning changes to all planes of each
 * display pipe.  This function should be used to commit changes that are
 * expected to fail, so that the error code can be checked for correctness.
 * For changes that are expected to succeed, use @igt_display_commit instead.
 *
 * Note that in non-atomic commit styles, no display programming will be
 * performed after the first failure is encountered, so only some of the
 * operations requested by a test may have been completed.  Tests that catch
 * errors returned by this function should take care to restore the display to
 * a sane state after a failure is detected.
 *
 * Returns: 0 upon success, otherwise the error code of the first error
 * encountered.
 */
int igt_display_try_commit2(igt_display_t *display, enum igt_commit_style s)
{
	return do_display_commit(display, s, false);
}

/**
 * igt_display_commit:
 * @display: DRM device handle
 *
 * Commits framebuffer and positioning changes to all planes of each display
 * pipe.
 *
 * Returns: 0 upon success.  This function will never return upon failure
 * since igt_fail() at lower levels will longjmp out of it.
 */
int igt_display_commit(igt_display_t *display)
{
	return igt_display_commit2(display, COMMIT_LEGACY);
}

/**
 * igt_display_drop_events:
 * @display: DRM device handle
 *
 * Nonblockingly reads all current events and drops them, for highest
 * reliability, call igt_display_commit2() first to flush all outstanding
 * events.
 *
 * This will be called on the first commit after igt_display_reset() too,
 * to make sure any stale events are flushed.
 *
 * Returns: Number of dropped events.
 */
int igt_display_drop_events(igt_display_t *display)
{
	int ret = 0;

	/* Clear all events from drm fd. */
	struct pollfd pfd = {
		.fd = display->drm_fd,
		.events = POLLIN
	};

	while (poll(&pfd, 1, 0) > 0) {
		struct drm_event *ev;
		char buf[4096];
		ssize_t retval;

		retval = read(display->drm_fd, &buf, sizeof(buf));
		igt_assert_lt(0, retval);

		for (int i = 0; i < retval; i += ev->length) {
			ev = (struct drm_event *)&buf[i];

			igt_info("Dropping event type %u length %u\n", ev->type, ev->length);
			igt_assert(ev->length + i <= sizeof(buf));
			ret++;
		}
	}

	return ret;
}

/**
 * igt_output_name:
 * @output: Target output
 *
 * Returns: String representing a connector's name, e.g. "DP-1".
 */
const char *igt_output_name(const igt_output_t *output)
{
	return output->name;
}

/**
 * kmstest_mode_is_valid:
 * @mode: pointer to drmModeModeInfo
 *
 * Returns: True if the mode is non-NULL and has valid
 * width, height and refresh rate.
 */
bool kmstest_mode_is_valid(const drmModeModeInfo *mode)
{
	return mode &&
	       mode->hdisplay > 0 &&
	       mode->vdisplay > 0 &&
	       mode->vrefresh > 0;
}

/**
 * igt_output_get_mode:
 * @output: Target output
 *
 * Get the current mode of the given connector
 *
 * Returns: A #drmModeModeInfo struct representing the current mode
 */
drmModeModeInfo *igt_output_get_mode(igt_output_t *output)
{
	if (output->use_override_mode)
		return &output->override_mode;
	else
		return &output->config.default_mode;
}

/**
 * igt_output_get_highres_mode:
 * @output: Target output
 *
 * Returns: A #drmModeModeInfo struct representing the highest mode, NULL otherwise.
 */
drmModeModeInfo *igt_output_get_highres_mode(igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *highest_mode = NULL;

	igt_sort_connector_modes(connector, sort_drm_modes_by_res_dsc);
	highest_mode = &connector->modes[0];

	return highest_mode;
}

/**
 * igt_output_get_lowres_mode:
 * @output: Target output
 *
 * Returns: A #drmModeModeInfo struct representing the lowest mode, NULL otherwise.
 */
drmModeModeInfo *igt_output_get_lowres_mode(igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *lowest_mode = NULL;

	igt_sort_connector_modes(connector, sort_drm_modes_by_res_asc);
	lowest_mode = &connector->modes[0];

	return lowest_mode;
}

/**
 * igt_output_override_mode:
 * @output: Output of which the mode will be overridden
 * @mode: New mode, or NULL to disable override.
 *
 * Overrides the output's mode with @mode, so that it is used instead of the
 * mode obtained with get connectors. Note that the mode is used without
 * checking if the output supports it, so this might lead to unexpected results.
 */
void igt_output_override_mode(igt_output_t *output, const drmModeModeInfo *mode)
{
	igt_crtc_t *crtc = igt_output_get_driving_crtc(output);

	if (mode)
		output->override_mode = *mode;

	output->use_override_mode = !!mode;

	if (crtc) {
		if (output->display->is_atomic)
			igt_crtc_replace_prop_blob(crtc, IGT_CRTC_MODE_ID,
						   igt_output_get_mode(output),
						   sizeof(*mode));
		else
			igt_crtc_set_prop_changed(crtc, IGT_CRTC_MODE_ID);
	}
}

/**
 * igt_output_preferred_vrefresh:
 * @output: Output whose preferred vrefresh is queried
 *
 * Returns: The vertical refresh rate of @output's preferred
 * mode. If the output reports no modes return 60Hz as
 * a fallback.
 */
int igt_output_preferred_vrefresh(igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;

	if (connector->count_modes)
		return connector->modes[0].vrefresh;
	else
		return 60;
}

igt_pipe_crc_t *igt_crtc_crc_new(igt_crtc_t *crtc, const char *source)
{
	return igt_pipe_crc_new(crtc->display->drm_fd, crtc->crtc_index, source);
}

igt_pipe_crc_t *igt_crtc_crc_new_nonblock(igt_crtc_t *crtc, const char *source)
{
	return igt_pipe_crc_new_nonblock(crtc->display->drm_fd, crtc->crtc_index, source);
}

int igt_crtc_debugfs_dir(igt_crtc_t *crtc)
{
	return igt_debugfs_crtc_dir(crtc->display->drm_fd, crtc->crtc_index);
}

const char *igt_crtc_name(igt_crtc_t *crtc)
{
	if (crtc == NULL)
		return "None";

	return kmstest_pipe_name(crtc->hardware_pipe);
}

/**
 * igt_output_set_crtc:
 * @output: Target output for which the pipe is being set to
 * @crtc: CRTC to set to
 *
 * This function sets a @crtc to a specific @output connector by
 * setting the CRTC_ID property of the @crtc.
 */
void igt_output_set_crtc(igt_output_t *output, igt_crtc_t *crtc)
{
	igt_display_t *display = output->display;
	igt_crtc_t *old_crtc = NULL;

	igt_assert(output->name);

	if (output->pending_crtc)
		old_crtc = igt_output_get_driving_crtc(output);

	LOG(display, "%s: set_crtc(%s)\n", igt_output_name(output),
	    igt_crtc_name(crtc));
	output->pending_crtc = crtc;

	if (old_crtc) {
		igt_output_t *old_output;

		old_output = igt_crtc_get_output(old_crtc);
		if (!old_output) {
			if (display->is_atomic)
				igt_crtc_replace_prop_blob(old_crtc,
							   IGT_CRTC_MODE_ID,
							   NULL, 0);
			else
				igt_crtc_set_prop_changed(old_crtc,
							  IGT_CRTC_MODE_ID);

			igt_crtc_set_prop_value(old_crtc, IGT_CRTC_ACTIVE, 0);
		}
	}

	igt_output_set_prop_value(output, IGT_CONNECTOR_CRTC_ID,
				  crtc ? crtc->crtc_id : 0);

	igt_output_refresh(output);

	if (crtc) {
		if (display->is_atomic)
			igt_crtc_replace_prop_blob(crtc, IGT_CRTC_MODE_ID,
						   igt_output_get_mode(output),
						   sizeof(drmModeModeInfo));
		else
			igt_crtc_set_prop_changed(crtc, IGT_CRTC_MODE_ID);

		igt_crtc_set_prop_value(crtc, IGT_CRTC_ACTIVE, 1);
	}
}

static
bool __override_all_active_output_modes_to_fit_bw(igt_display_t *display,
						  igt_output_t *outputs[IGT_MAX_PIPES],
						  const int n_outputs,
						  int base)
{
	igt_output_t *output = NULL;
	drmModeModeInfo *mode;

	if (base >= n_outputs)
		return false;

	output = outputs[base];

	for_each_connector_mode(output, mode) {
		int ret;

		igt_output_override_mode(output, mode);

		if (__override_all_active_output_modes_to_fit_bw(display, outputs, n_outputs, base + 1))
			return true;

		if (display->is_atomic)
			ret = igt_display_try_commit_atomic(display,
					DRM_MODE_ATOMIC_TEST_ONLY |
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL);
		else
			ret = igt_display_try_commit2(display, COMMIT_LEGACY);

		if (!ret)
			return true;
		else if (ret != -ENOSPC && ret != -EINVAL)
			return false;
	}

	return false;
}

/**
 * igt_override_all_active_output_modes_to_fit_bw:
 * @display: a pointer to an #igt_display_t structure
 *
 * Override the mode on all active outputs (i.e. pending_crtc != NULL)
 * on basis of bandwidth.
 *
 * Returns: True if a valid connector mode combo found, else false
 */
bool igt_override_all_active_output_modes_to_fit_bw(igt_display_t *display)
{
	int i, n_outputs = 0;
	igt_output_t *outputs[IGT_MAX_PIPES];

	for (i = 0 ; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (!output->pending_crtc)
			continue;

		/* Sort the modes in descending order by clock freq. */
		igt_sort_connector_modes(output->config.connector,
					 sort_drm_modes_by_clk_dsc);

		outputs[n_outputs++] = output;
	}
	igt_require_f(n_outputs, "No active outputs found.\n");

	return __override_all_active_output_modes_to_fit_bw(display, outputs, n_outputs, 0);
}

/*
 * igt_fit_modes_in_bw :
 * @display: a pointer to an #igt_display_t structure
 *
 * Attempts to commit the current display configuration using
 * atomic or legacy commit style based on the platform support.
 *
 * If the commit fails, attempts to override all active output
 * modes to try to fit within the available bandwidth.
 *
 * Returns: true if a valid mode combination is found or the commit succeeds,
 * false otherwise.
 */
bool igt_fit_modes_in_bw(igt_display_t *display)
{
	int ret;

	if (display->is_atomic)
		ret = igt_display_try_commit_atomic(display,
						    DRM_MODE_ATOMIC_TEST_ONLY |
						    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	else
		ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	if (ret != 0) {
		bool found;

		found = igt_override_all_active_output_modes_to_fit_bw(display);
		if (!found) {
			igt_debug("No valid mode combo found for modeset\n");
			return false;
		}
	}

	return true;
}

/**
 * igt_crtc_refresh:
 * @crtc: CRTC to refresh
 * @force: Should be set to true if mode_blob is no longer considered
 * to be valid, for example after doing an atomic commit during fork or closing display fd.
 *
 * Requests the CRTC to be part of the state on next update.
 * This is useful when state may have been out of sync after
 * a fork, or we just want to be sure the pipe is included
 * in the next commit.
 */
void igt_crtc_refresh(igt_crtc_t *crtc, bool force)
{
	igt_display_t *display = crtc->display;

	if (force && display->is_atomic) {
		igt_output_t *output = igt_crtc_get_output(crtc);

		crtc->values[IGT_CRTC_MODE_ID] = 0;
		if (output)
			igt_crtc_replace_prop_blob(crtc, IGT_CRTC_MODE_ID,
						   igt_output_get_mode(output),
						   sizeof(drmModeModeInfo));
	} else
		igt_crtc_set_prop_changed(crtc, IGT_CRTC_MODE_ID);
}

/**
 * igt_output_get_plane:
 * @output: Target output
 * @plane_idx: Plane index
 *
 * Finds a driving pipe for the given @output otherwise and gets the valid
 * plane associated with that pipe for the given @plane_idx. This function
 * will terminate the execution if driving pipe is not for a given @output.
 *
 * Returns: A #igt_plane_t structure that matches the requested plane index
 */
igt_plane_t *igt_output_get_plane(igt_output_t *output, int plane_idx)
{
	igt_crtc_t *crtc;

	crtc = igt_output_get_driving_crtc(output);
	igt_assert(crtc);

	return igt_crtc_get_plane(crtc, plane_idx);
}

/**
 * igt_output_get_plane_type:
 * @output: Target output
 * @plane_type: Cursor, primary or an overlay plane
 *
 * Finds a valid plane type for the given @output otherwise
 * the test is skipped if the right combination of @output/@plane_type is not found
 *
 * Returns: A #igt_plane_t structure that matches the requested plane type
 */
igt_plane_t *igt_output_get_plane_type(igt_output_t *output, int plane_type)
{
	igt_crtc_t *crtc;

	crtc = igt_output_get_driving_crtc(output);
	igt_assert(crtc);

	return igt_crtc_get_plane_type(crtc, plane_type);
}

/**
 * igt_output_count_plane_type:
 * @output: Target output
 * @plane_type: Cursor, primary or an overlay plane
 *
 * Counts the number of planes of type @plane_type for the provided @output.
 *
 * Returns: The number of planes that match the requested plane type
 */
int igt_output_count_plane_type(igt_output_t *output, int plane_type)
{
	igt_crtc_t *crtc = igt_output_get_driving_crtc(output);
	igt_assert(crtc);

	return igt_crtc_count_plane_type(crtc, plane_type);
}

/**
 * igt_output_get_plane_type_index:
 * @output: Target output
 * @plane_type: Cursor, primary or an overlay plane
 * @index: the index of the plane among planes of the same type
 *
 * Get the @index th plane of type @plane_type for the provided @output.
 *
 * Returns: The @index th plane that matches the requested plane type
 */
igt_plane_t *igt_output_get_plane_type_index(igt_output_t *output,
					     int plane_type, int index)
{
	igt_crtc_t *crtc = igt_output_get_driving_crtc(output);
	igt_assert(crtc);

	return igt_crtc_get_plane_type_index(crtc, plane_type, index);
}

/**
 * igt_plane_set_fb:
 * @plane: Plane
 * @fb: Framebuffer pointer
 *
 * Pairs a given @framebuffer to a @plane
 *
 * This function also sets a default size and position for the framebuffer
 * to avoid crashes on applications that ignore to set these.
 */
void igt_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: plane_set_fb(%d)\n",
	    igt_crtc_name(crtc),
	    plane->index, fb ? fb->fb_id : 0);

	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_ID,
				 fb ? crtc->crtc_id : 0);
	igt_plane_set_prop_value(plane, IGT_PLANE_FB_ID, fb ? fb->fb_id : 0);

	if (plane->type == DRM_PLANE_TYPE_CURSOR && fb)
		plane->gem_handle = fb->gem_handle;
	else
		plane->gem_handle = 0;

	/* hack to keep tests working that don't call igt_plane_set_size() */
	if (fb) {
		/* set default plane size as fb size */
		igt_plane_set_size(plane, fb->width, fb->height);

		/* set default src pos/size as fb size */
		igt_fb_set_position(fb, plane, 0, 0);
		igt_fb_set_size(fb, plane, fb->width, fb->height);

		if (!display->has_plane_color_pipeline) {
			if (igt_plane_has_prop(plane, IGT_PLANE_COLOR_ENCODING))
				igt_plane_set_prop_enum(plane, IGT_PLANE_COLOR_ENCODING,
					igt_color_encoding_to_str(fb->color_encoding));

			if (igt_plane_has_prop(plane, IGT_PLANE_COLOR_RANGE))
				igt_plane_set_prop_enum(plane, IGT_PLANE_COLOR_RANGE,
					igt_color_range_to_str(fb->color_range));
		}

		/* Hack to prioritize the plane on the pipe that last set fb */
		igt_plane_set_crtc(plane, crtc);
	} else {
		igt_plane_set_size(plane, 0, 0);

		/* set default src pos/size as fb size */
		igt_fb_set_position(fb, plane, 0, 0);
		igt_fb_set_size(fb, plane, 0, 0);
	}
}

/**
 * igt_plane_set_fence_fd:
 * @plane: plane
 * @fence_fd: fence fd, disable fence_fd by setting it to -1
 *
 * This function sets a fence fd to enable a commit to wait for some event to
 * occur before completing.
 */
void igt_plane_set_fence_fd(igt_plane_t *plane, int fence_fd)
{
	int64_t fd;

	fd = plane->values[IGT_PLANE_IN_FENCE_FD];
	if (fd != -1)
		close(fd);

	if (fence_fd != -1) {
		fd = dup(fence_fd);
		igt_fail_on(fd == -1);
	} else
		fd = -1;

	igt_plane_set_prop_value(plane, IGT_PLANE_IN_FENCE_FD, fd);
}

/**
 * igt_plane_set_crtc:
 * @plane: Target plane pointer
 * @crtc: The CRTC to assign the plane to
 */
void igt_plane_set_crtc(igt_plane_t *plane, igt_crtc_t *crtc)
{
	/*
	 * HACK: Point the global plane back to the local plane.
	 * This is used to help apply the correct atomic state while
	 * we're moving away from the single pipe per plane model.
	 */
	plane->ref->ref = plane;
	plane->ref->crtc = crtc;
}

/**
 * igt_plane_set_position:
 * @plane: Plane pointer for which position is to be set
 * @x: X coordinate
 * @y: Y coordinate
 *
 * This function sets a new (x,y) position for the given plane.
 * New position will be committed at plane commit time via drmModeSetPlane().
 */
void igt_plane_set_position(igt_plane_t *plane, int x, int y)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: plane_set_position(%d,%d)\n",
	    igt_crtc_name(crtc), plane->index, x, y);

	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_X, x);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_Y, y);
}

/**
 * igt_plane_set_size:
 * @plane: plane pointer for which size to be set
 * @w: width
 * @h: height
 *
 * This function sets width and height for requested plane.
 * New size will be committed at plane commit time via
 * drmModeSetPlane().
 */
void igt_plane_set_size(igt_plane_t *plane, int w, int h)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: plane_set_size (%dx%d)\n",
	    igt_crtc_name(crtc), plane->index, w, h);

	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_W, w);
	igt_plane_set_prop_value(plane, IGT_PLANE_CRTC_H, h);
}

/**
 * igt_fb_set_position:
 * @fb: framebuffer pointer
 * @plane: plane
 * @x: X position
 * @y: Y position
 *
 * This function sets position for requested framebuffer as src to plane.
 * New position will be committed at plane commit time via drmModeSetPlane().
 */
void igt_fb_set_position(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t x, uint32_t y)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: fb_set_position(%d,%d)\n",
	    igt_crtc_name(crtc), plane->index, x, y);

	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_X, IGT_FIXED(x, 0));
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_Y, IGT_FIXED(y, 0));
}

/**
 * igt_fb_set_size:
 * @fb: framebuffer pointer
 * @plane: plane
 * @w: width
 * @h: height
 *
 * This function sets fetch rect size from requested framebuffer as src
 * to plane. New size will be committed at plane commit time via
 * drmModeSetPlane().
 */
void igt_fb_set_size(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t w, uint32_t h)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: fb_set_size(%dx%d)\n",
	    igt_crtc_name(crtc), plane->index, w, h);

	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_W, IGT_FIXED(w, 0));
	igt_plane_set_prop_value(plane, IGT_PLANE_SRC_H, IGT_FIXED(h, 0));
}

/**
 * igt_plane_rotation_name:
 * @rotation: Plane rotation value (0, 90, 180, 270)
 *
 * Returns: Plane rotation value as a string
 */
const char *igt_plane_rotation_name(igt_rotation_t rotation)
{
	switch (rotation & IGT_ROTATION_MASK) {
	case IGT_ROTATION_0:
		return "0";
	case IGT_ROTATION_90:
		return "90";
	case IGT_ROTATION_180:
		return "180";
	case IGT_ROTATION_270:
		return "270";
	default:
		igt_assert(0);
	}
}

/**
 * igt_plane_set_rotation:
 * @plane: Plane pointer for which rotation is to be set
 * @rotation: Plane rotation value (0, 90, 180, 270)
 *
 * This function sets a new rotation for the requested @plane.
 * New @rotation will be committed at plane commit time via
 * drmModeSetPlane().
 */
void igt_plane_set_rotation(igt_plane_t *plane, igt_rotation_t rotation)
{
	igt_crtc_t *crtc = plane->crtc;
	igt_display_t *display = crtc->display;

	LOG(display, "%s.%d: plane_set_rotation(%s°)\n",
	    igt_crtc_name(crtc),
	    plane->index, igt_plane_rotation_name(rotation));

	igt_plane_set_prop_value(plane, IGT_PLANE_ROTATION, rotation);
}

/**
 * igt_crtc_request_out_fence:
 * @crtc: CRTC which out fence will be requested for
 *
 * Marks this pipe for requesting an out fence at the next atomic commit
 * will contain the fd number of the out fence created by KMS.
 */
void igt_crtc_request_out_fence(igt_crtc_t *crtc)
{
	igt_crtc_set_prop_value(crtc, IGT_CRTC_OUT_FENCE_PTR,
				(ptrdiff_t)&crtc->out_fence_fd);
}

/**
 * igt_output_set_writeback_fb:
 * @output: Target output
 * @fb: Target framebuffer
 *
 * This function sets the given @fb to be used as the target framebuffer for the
 * writeback engine at the next atomic commit. It will also request a writeback
 * out fence that will contain the fd number of the out fence created by KMS if
 * the given @fb is valid.
 */
void igt_output_set_writeback_fb(igt_output_t *output, struct igt_fb *fb)
{
	igt_display_t *display = output->display;

	LOG(display, "%s: output_set_writeback_fb(%d)\n", output->name, fb ? fb->fb_id : 0);

	igt_output_set_prop_value(output, IGT_CONNECTOR_WRITEBACK_FB_ID, fb ? fb->fb_id : 0);
	/* only request a writeback out fence if the framebuffer is valid */
	if (fb)
		igt_output_set_prop_value(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR,
					  (ptrdiff_t)&output->writeback_out_fence_fd);
}

static int __igt_vblank_wait(int drm_fd, int crtc_index, int count,
			     uint64_t *ts_ns, unsigned int *seq)
{
	drmVBlank wait_vbl;
	uint32_t pipe_id_flag;
	int ret;

	memset(&wait_vbl, 0, sizeof(wait_vbl));
	pipe_id_flag = kmstest_get_vbl_flag(crtc_index);

	wait_vbl.request.type = DRM_VBLANK_RELATIVE | pipe_id_flag;
	wait_vbl.request.sequence = count;

	ret = drmWaitVBlank(drm_fd, &wait_vbl);
	if (ret)
		return ret;

	if (ts_ns)
		*ts_ns = wait_vbl.reply.tval_sec * NSEC_PER_SEC +
			 wait_vbl.reply.tval_usec * NSEC_PER_USEC;
	if (seq)
		*seq = wait_vbl.reply.sequence;

	return 0;
}

/**
 * igt_wait_for_vblank_count:
 * @drm_fd: A drm file descriptor
 * @crtc: the CRTC
 * @count: Number of vblanks to wait on
 *
 * Waits for a given number of vertical blank intervals
 */
void igt_wait_for_vblank_count(igt_crtc_t *crtc, int count)
{
	igt_assert(__igt_vblank_wait(crtc->display->drm_fd, crtc->crtc_index,
				     count, NULL, NULL) == 0);
}

/**
 * igt_wait_for_vblank_ts_seq:
 * @crtc: the CRTC
 * @ts_ns: returns the vblank timestamp in nanoseconds, may be NULL
 * @seq: returns the vblank sequence number, may be NULL
 *
 * Waits for the next vertical blank interval on @crtc and hands back the
 * timestamp and the sequence number reported by the kernel for that event.
 */
void igt_wait_for_vblank_ts_seq(igt_crtc_t *crtc, uint64_t *ts_ns, unsigned int *seq)
{
	igt_assert(__igt_vblank_wait(crtc->display->drm_fd, crtc->crtc_index,
				     1, ts_ns, seq) == 0);
}

/**
 * igt_wait_for_vblank:
 * @drm_fd: A drm file descriptor
 * @crtc: the CRTC
 *
 * See #igt_wait_for_vblank_count for more details
 *
 * Waits for 1 vertical blank intervals
 */
void igt_wait_for_vblank(igt_crtc_t *crtc)
{
	igt_assert(__igt_vblank_wait(crtc->display->drm_fd, crtc->crtc_index,
				     1, NULL, NULL) == 0);
}

/**
 * igt_enable_connectors:
 * @drm_fd: A drm file descriptor
 *
 * Force connectors to be enabled where this is known to work well. Use
 * #igt_reset_connectors to revert the changes.
 *
 * An exit handler is installed to ensure connectors are reset when the test
 * exits.
 */
void igt_enable_connectors(int drm_fd)
{
#define MAX_TRIES	10
#define SLEEP_DURATION	50000
	drmModeRes *res;
	int tries;

	res = drmModeGetResources(drm_fd);
	if (!res)
		return;

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c;

		/*
		 * The kernel returns the count of connectors before
		 * they're all fully set up, so we can have a race
		 * condition where we try to get the connector when
		 * it's not fully set up yet.  To avoid failing here
		 * in these cases, retry a few times.
		 */
		for (tries = 0; tries < MAX_TRIES; tries++) {
			/* Do a probe. This may be the first action after booting */
			c = drmModeGetConnector(drm_fd, res->connectors[i]);
			if (c)
				break;

			igt_debug("Could not read connector %u: %m (try %d of %d)\n",
				  res->connectors[i], tries + 1, MAX_TRIES);

			usleep(SLEEP_DURATION);
		}

		if (tries == MAX_TRIES) {
			igt_warn("Could not read connector %u after %d tries, skipping\n",
				 res->connectors[i], MAX_TRIES);
			continue;
		}

		/* don't attempt to force connectors that are already connected
		 */
		if (c->connection == DRM_MODE_CONNECTED)
			continue;

		/* just enable VGA for now */
		if (c->connector_type == DRM_MODE_CONNECTOR_VGA) {
			if (!kmstest_force_connector(drm_fd, c, FORCE_CONNECTOR_ON))
				igt_info("Unable to force state on %s-%d\n",
					 kmstest_connector_type_str(c->connector_type),
					 c->connector_type_id);
		}

		drmModeFreeConnector(c);
	}
}

/**
 * igt_reset_connectors:
 *
 * Remove any forced state from the connectors.
 */
void igt_reset_connectors(void)
{
	/* reset the connectors stored in connector_attrs, avoiding any
	 * functions that are not safe to call in signal handlers */
	for (int i = 0; i < ARRAY_SIZE(connector_attrs); i++) {
		struct igt_connector_attr *c = &connector_attrs[i];

		if (!c->attr)
			continue;

		c->set(c->dir, c->attr, c->reset_value);
	}
}

/**
 * igt_watch_uevents:
 *
 * Begin monitoring udev for sysfs uevents.
 *
 * Returns: A udev monitor for detecting uevents on
 */
struct udev_monitor *igt_watch_uevents(void)
{
	struct udev *udev;
	struct udev_monitor *mon;
	int ret, flags, fd;

	udev = udev_new();
	igt_assert(udev != NULL);

	mon = udev_monitor_new_from_netlink(udev, "udev");
	igt_assert(mon != NULL);

	ret = udev_monitor_filter_add_match_subsystem_devtype(mon,
							      "drm",
							      "drm_minor");
	igt_assert_eq(ret, 0);
	ret = udev_monitor_filter_update(mon);
	igt_assert_eq(ret, 0);
	ret = udev_monitor_enable_receiving(mon);
	igt_assert_eq(ret, 0);

	/* Set the fd for udev as non blocking */
	fd = udev_monitor_get_fd(mon);
	flags = fcntl(fd, F_GETFL, 0);
	igt_assert(flags);

	flags |= O_NONBLOCK;
	igt_assert_neq(fcntl(fd, F_SETFL, flags), -1);

	return mon;
}

static
bool event_detected(struct udev_monitor *mon, int timeout_secs,
		    const char **property, int *expected_val, int num_props)
{
	struct udev_device *dev;
	const char *prop_val;
	struct pollfd fd = {
		.fd = udev_monitor_get_fd(mon),
		.events = POLLIN
	};
	bool event_received = false;
	int i;

	/* Go through all of the events pending on the udev monitor.
	 * Match the given set of properties and their values to
	 * the expected values.
	 */
	while (!event_received && poll(&fd, 1, timeout_secs * 1000)) {
		dev = udev_monitor_receive_device(mon);
		for (i = 0; i < num_props; i++) {
			prop_val = udev_device_get_property_value(dev,
								  property[i]);
			if (!prop_val || atoi(prop_val) != expected_val[i])
				break;
		}
		if (i == num_props)
			event_received = true;

		udev_device_unref(dev);
	}

	return event_received;
}

/**
 * igt_connector_event_detected:
 * @mon: A udev monitor initialized with #igt_watch_uevents
 * @conn_id: Connector id of the Connector for which the property change is
 * expected.
 * @prop_id: Property id for which the change is expected.
 * @timeout_secs: How long to wait for a connector event to occur.
 *
 * Detect if a connector event is received for a given connector and property.
 *
 * Returns: True if the connector event was received, false if we timed out
 */
bool igt_connector_event_detected(struct udev_monitor *mon, uint32_t conn_id,
				  uint32_t prop_id, int timeout_secs)
{
	const char *props[3] = {"HOTPLUG", "CONNECTOR", "PROPERTY"};
	int expected_val[3] = {1, conn_id, prop_id};

	return event_detected(mon, timeout_secs, props, expected_val,
			      ARRAY_SIZE(props));
}

/**
 * igt_hotplug_detected:
 * @mon: A udev monitor initialized with #igt_watch_uevents
 * @timeout_secs: How long to wait for a hotplug event to occur.
 *
 * Detect if a hotplug event was received since we last checked the monitor.
 *
 * Returns: True if a sysfs hotplug event was received, false if we timed out
 */
bool igt_hotplug_detected(struct udev_monitor *mon, int timeout_secs)
{
	const char *props[1] = {"HOTPLUG"};
	int expected_val = 1;

	return event_detected(mon, timeout_secs, props, &expected_val,
			      ARRAY_SIZE(props));
}

/**
 * igt_lease_change_detected:
 * @mon: A udev monitor initialized with #igt_watch_uevents
 * @timeout_secs: How long to wait for a lease change event to occur.
 *
 * Detect if a lease change event was received since we last checked the monitor.
 *
 * Returns: True if a sysfs lease change event was received, false if we timed out
 */
bool igt_lease_change_detected(struct udev_monitor *mon, int timeout_secs)
{
	const char *props[1] = {"LEASE"};
	int expected_val = 1;

	return event_detected(mon, timeout_secs, props, &expected_val,
			      ARRAY_SIZE(props));
}

/**
 * igt_flush_uevents:
 * @mon: A udev monitor initialized with #igt_watch_uevents
 *
 * Get rid of any pending uevents
 */
void igt_flush_uevents(struct udev_monitor *mon)
{
	struct udev_device *dev;

	while ((dev = udev_monitor_receive_device(mon)))
		udev_device_unref(dev);
}

/**
 * igt_cleanup_uevents:
 * @mon: A udev monitor initialized with #igt_watch_uevents
 *
 * Cleanup the resources allocated by #igt_watch_uevents
 */
void igt_cleanup_uevents(struct udev_monitor *mon)
{
	struct udev *udev = udev_monitor_get_udev(mon);

	udev_monitor_unref(mon);
	mon = NULL;
	udev_unref(udev);
}

#define IGT_KMS_HELPER_PARAMS "/sys/module/drm_kms_helper/parameters"

static struct {
	bool saved;
	bool orig_poll;
} __igt_kms_poll;

static void __igt_kms_poll_exit_handler(int sig)
{
	igt_kms_poll_restore();
}

/**
 * igt_kms_poll_save:
 *
 * Saves the current drm_kms_helper.poll value and installs an exit handler to
 * restore it. Safe to call more than once, only the first value is kept.
 *
 * Returns: false if the module parameter is not available, in which case
 * nothing was saved.
 */
bool igt_kms_poll_save(void)
{
	int dir;

	if (__igt_kms_poll.saved)
		return true;

	dir = open(IGT_KMS_HELPER_PARAMS, O_RDONLY);
	if (dir < 0)
		return false;

	if (!__igt_sysfs_get_boolean(dir, "poll", &__igt_kms_poll.orig_poll)) {
		close(dir);
		return false;
	}
	close(dir);

	__igt_kms_poll.saved = true;
	igt_install_exit_handler(__igt_kms_poll_exit_handler);

	igt_debug("Saved drm_kms_helper.poll = %s\n",
		  __igt_kms_poll.orig_poll ? "Y" : "N");

	return true;
}

/**
 * igt_kms_poll_set:
 * @enable: whether to allow connector polling
 *
 * Writes the drm_kms_helper.poll module parameter. Callers are expected to have
 * called igt_kms_poll_save() first so the original value is restored on exit.
 *
 * This only changes the parameter, it does not itself start or stop the poll
 * worker. Disabling takes effect the next time the worker runs, at most one
 * poll period later, and the worker then stops. Enabling does not restart a
 * worker that has stopped: the kernel only does that from
 * drm_kms_helper_poll_enable(), i.e. on driver load and, for most drivers,
 * system resume. Callers that rely on polling actually running after enabling
 * it must check for it.
 */
void igt_kms_poll_set(bool enable)
{
	int dir;

	dir = open(IGT_KMS_HELPER_PARAMS, O_RDONLY);
	igt_assert_f(dir >= 0, "Can't open %s\n", IGT_KMS_HELPER_PARAMS);

	igt_sysfs_set_boolean(dir, "poll", enable);
	close(dir);

	igt_debug("drm_kms_helper.poll = %s\n", enable ? "Y" : "N");
}

/**
 * igt_kms_poll_restore:
 *
 * Restores drm_kms_helper.poll to the value saved by igt_kms_poll_save(). Does
 * nothing if it was never saved, so it is safe to use as an exit handler that
 * may run before any save happened.
 *
 * As with igt_kms_poll_set(), restoring the parameter to enabled does not
 * restart a poll worker that stopped while it was disabled.
 */
void igt_kms_poll_restore(void)
{
	int dir;

	if (!__igt_kms_poll.saved)
		return;

	dir = open(IGT_KMS_HELPER_PARAMS, O_RDONLY);
	if (dir >= 0) {
		__igt_sysfs_set_boolean(dir, "poll", __igt_kms_poll.orig_poll);
		close(dir);
	}

	__igt_kms_poll.saved = false;
}

/**
 * kmstest_get_vbl_flag:
 * @crtc_index: CRTC index to convert into DRM_IOCTL_WAIT_VBLANK parameter
 *
 * Convert a CRTC index (of a CRTC in drmModeRes.crtcs) into flag representation
 * expected by DRM_IOCTL_WAIT_VBLANK.
 *
 * See #igt_wait_for_vblank_count for details.
 */
uint32_t kmstest_get_vbl_flag(int crtc_index)
{
	uint32_t flag;

	flag = crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT;

	igt_assert_eq(flag & ~DRM_VBLANK_HIGH_CRTC_MASK, 0);

	return flag;
}

/**
 * igt_crtc_get_vbl_flag:
 * @crtc: CRTC
 *
 * Convert a CRTC into flag representation
 * expected by DRM_IOCTL_WAIT_VBLANK.
 *
 * See #igt_wait_for_vblank_count for details.
 */
uint32_t igt_crtc_get_vbl_flag(igt_crtc_t *crtc)
{
	return kmstest_get_vbl_flag(crtc->crtc_index);
}

static inline const uint32_t *
formats_ptr(const struct drm_format_modifier_blob *blob)
{
	return (const uint32_t *)((const char *)blob + blob->formats_offset);
}

static inline const struct drm_format_modifier *
modifiers_ptr(const struct drm_format_modifier_blob *blob)
{
	return (const struct drm_format_modifier *)((const char *)blob + blob->modifiers_offset);
}

static int igt_count_plane_format_mod(const struct drm_format_modifier_blob *blob_data)
{
	const struct drm_format_modifier *modifiers;
	int count = 0;

	modifiers = modifiers_ptr(blob_data);

	for (int i = 0; i < blob_data->count_modifiers; i++)
		count += igt_hweight(modifiers[i].formats);

	return count;
}

static void format_mods_alloc(struct igt_format_mods *format_mods, int count)
{
	format_mods->count = count;

	if (count == 0)
		return;

	format_mods->formats = calloc(count, sizeof(format_mods->formats[0]));
	igt_assert(format_mods->formats);

	format_mods->modifiers = calloc(count, sizeof(format_mods->modifiers[0]));
	igt_assert(format_mods->modifiers);
}

static void igt_parse_format_mod_blob(const struct drm_format_modifier_blob *blob_data,
				      struct igt_format_mods *format_mods)
{
	const struct drm_format_modifier *m = modifiers_ptr(blob_data);
	const uint32_t *f = formats_ptr(blob_data);
	int idx = 0;

	format_mods_alloc(format_mods, igt_count_plane_format_mod(blob_data));

	for (int i = 0; i < blob_data->count_modifiers; i++) {
		for (int j = 0; j < 64; j++) {
			if (!(m[i].formats & (1ULL << j)))
				continue;

			format_mods->formats[idx] = f[m[i].offset + j];
			format_mods->modifiers[idx] = m[i].modifier;
			idx++;
			igt_assert_lte(idx, format_mods->count);
		}
	}

	igt_assert_eq(idx, format_mods->count);
}

static void fill_plane_default_format_mods(igt_plane_t *plane,
					   struct igt_format_mods *format_mods)
{
	drmModePlanePtr p = plane->drm_plane;

	format_mods_alloc(format_mods, p->count_formats);

	/*
	 * We don't know which modifiers are
	 * supported, so we'll assume linear only.
	 */
	for (int i = 0; i < plane->format_mods.count; i++) {
		format_mods->formats[i] = p->formats[i];
		format_mods->modifiers[i] = DRM_FORMAT_MOD_LINEAR;
	}
}

static void igt_fill_plane_format_mod(igt_display_t *display, igt_plane_t *plane)
{
	const struct drm_format_modifier_blob *blob_data;
	drmModePropertyBlobPtr blob;
	uint64_t blob_id;

	if (!igt_plane_has_prop(plane, IGT_PLANE_IN_FORMATS)) {
		/* IN_FORMATS_ASYNC should never be present without IN_FORMATS */
		igt_assert(!igt_plane_has_prop(plane, IGT_PLANE_IN_FORMATS_ASYNC));

		fill_plane_default_format_mods(plane, &plane->format_mods);

		if (igt_has_drm_cap(display->drm_fd, DRM_CAP_ASYNC_PAGE_FLIP))
			fill_plane_default_format_mods(plane, &plane->format_mods_async);

		return;
	}

	blob_id = igt_plane_get_prop(plane, IGT_PLANE_IN_FORMATS);
	blob = drmModeGetPropertyBlob(display->drm_fd, blob_id);
	if (!blob)
		return;

	blob_data = (const struct drm_format_modifier_blob *)blob->data;
	igt_parse_format_mod_blob(blob_data, &plane->format_mods);

	if (!igt_plane_has_prop(plane, IGT_PLANE_IN_FORMATS_ASYNC) &&
	    igt_has_drm_cap(display->drm_fd, DRM_CAP_ASYNC_PAGE_FLIP)) {
		igt_warn("Driver has IN_FORMATS but no IN_FORMATS_ASYNC while supporting async flips\n");

		/* assume everything supports async flips */
		igt_parse_format_mod_blob(blob_data, &plane->format_mods_async);
	}

	drmModeFreePropertyBlob(blob);

	if (igt_plane_has_prop(plane, IGT_PLANE_IN_FORMATS_ASYNC)) {
		blob_id = igt_plane_get_prop(plane, IGT_PLANE_IN_FORMATS_ASYNC);
		blob = drmModeGetPropertyBlob(display->drm_fd, blob_id);
		if (!blob)
			return;

		blob_data = (const struct drm_format_modifier_blob *)blob->data;
		igt_parse_format_mod_blob(blob_data, &plane->format_mods_async);
		drmModeFreePropertyBlob(blob);
	}
}

/**
 * igt_format_mods_has_format_and_modifier:
 * @format_mods: a pointer to an #igt_format_mods structure
 * @format: Pixel format
 * @modifier: Modifier
 *
 * Returns: True if @format_mods includes the given @format/@modifier tuple, else false
 */
bool igt_format_mods_has_format_and_modifier(const struct igt_format_mods *format_mods,
					     uint32_t format, uint64_t modifier)
{
	uint64_t _modifier;
	uint32_t _format;

	for_each_format_and_modifier(format_mods, _format, _modifier) {
		if (_format == format && _modifier == modifier)
			return true;
	}

	return false;
}

/**
 * igt_format_mods_has_format:
 * @format_mods: a pointer to an #igt_format_mods structure
 * @format: Pixel format
 *
 * Returns: True if @format_mods includes the given @format, else false
 */
bool igt_format_mods_has_format(const struct igt_format_mods *format_mods,
				uint32_t format)
{
	uint64_t _modifier;
	uint32_t _format;

	for_each_format_and_modifier(format_mods, _format, _modifier) {
		if (_format == format)
			return true;
		(void)_modifier;
	}

	return false;
}

/**
 * igt_format_mods_has_modifier:
 * @format_mods: a pointer to an #igt_format_mods structure
 * @modifier: Modifier
 *
 * Returns: True if @format_mods includes the given @modifier, else false
 */
bool igt_format_mods_has_modifier(const struct igt_format_mods *format_mods,
				  uint64_t modifier)
{
	uint64_t _modifier;
	uint32_t _format;

	for_each_format_and_modifier(format_mods, _format, _modifier) {
		if (_modifier == modifier)
			return true;
		(void)_format;
	}

	return false;
}

/**
 * igt_plane_has_format_mod:
 * @plane: Target plane
 * @format: Target format
 * @modifier: Target modifier
 *
 * Returns: True if @plane supports the given @format and @modifier, else false
 */
bool igt_plane_has_format_mod(igt_plane_t *plane, uint32_t format,
			      uint64_t modifier)
{
	return igt_format_mods_has_format_and_modifier(&plane->format_mods,
						       format, modifier);
}

static void
format_mods_append(struct igt_format_mods *format_mods,
		   uint32_t format, uint64_t modifier)
{
	int i;

	if (igt_format_mods_has_format_and_modifier(format_mods,
						    format, modifier))
		return;

	i = format_mods->count;

	format_mods->formats[i] = format;
	format_mods->modifiers[i] = modifier;

	format_mods->count++;
}

static void
format_mods_union(struct igt_format_mods *format_mods_dst,
		  const struct igt_format_mods *format_mods_src)
{
	for (int i = 0; i < format_mods_src->count; i++)
		format_mods_append(format_mods_dst,
				   format_mods_src->formats[i],
				   format_mods_src->modifiers[i]);
}

static int display_format_mods_count(igt_display_t *display,
				     const struct igt_format_mods *(*plane_format_mods)(igt_plane_t *plane))
{
	igt_crtc_t *crtc;
	int count = 0;

	for_each_crtc(display, crtc) {
		igt_plane_t *plane;

		for_each_plane_on_crtc(crtc, plane)
			count += plane_format_mods(plane)->count;
	}

	return count;
}

static void display_format_mods_union(igt_display_t *display,
				      struct igt_format_mods *format_mods,
				      const struct igt_format_mods *(*plane_format_mods)(igt_plane_t *plane))
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		igt_plane_t *plane;

		for_each_plane_on_crtc(crtc, plane)
			format_mods_union(format_mods, plane_format_mods(plane));
	}
}

static void display_format_mods_fill(igt_display_t *display,
				     struct igt_format_mods *format_mods,
				     const struct igt_format_mods *(*plane_format_mods)(igt_plane_t *plane))
{
	int count = display_format_mods_count(display, plane_format_mods);

	if (!count)
		return;

	format_mods_alloc(format_mods, count);

	/*
	 * Initial count was an upper bound and may include
	 * duplicates. The final count, without duplicates,
	 * is determined while filling the arrays.
	 */
	format_mods->count = 0;

	display_format_mods_union(display, format_mods, plane_format_mods);

	igt_assert_lte(format_mods->count, count);
}

static const struct igt_format_mods *plane_format_mods(igt_plane_t *plane)
{
	return &plane->format_mods;
}

static const struct igt_format_mods *plane_format_mods_async(igt_plane_t *plane)
{
	return &plane->format_mods_async;
}

static void igt_fill_display_format_mod(igt_display_t *display)
{
	display_format_mods_fill(display, &display->format_mods, plane_format_mods);
	display_format_mods_fill(display, &display->format_mods_async, plane_format_mods_async);
}

/**
 * igt_display_has_format_mod:
 * @display: a pointer to an #igt_display_t structure
 * @format: Target format
 * @modifier: Target modifier
 *
 * Returns: True if @display supports the given @format and @modifier, else false
 */
bool igt_display_has_format_mod(igt_display_t *display, uint32_t format,
				uint64_t modifier)
{
	return igt_format_mods_has_format_and_modifier(&display->format_mods,
						       format, modifier);
}

/**
 * igt_parse_connector_tile_blob:
 * @blob: pointer to the connector's tile properties
 * @tile: pointer to tile structure that is populated by the function
 *
 * Parses the connector tile blob to extract the tile information.
 * The blob information is exposed from drm/drm_connector.c in the kernel.
 * The format of the tile property is defined in the kernel as char tile[256]
 * that consists of 8 integers that are ':' separated.
 */
void igt_parse_connector_tile_blob(drmModePropertyBlobPtr blob,
		igt_tile_info_t *tile)
{
	char *blob_data = blob->data;

	igt_assert(blob);

	tile->tile_group_id = atoi(strtok(blob_data, ":"));
	tile->tile_is_single_monitor = atoi(strtok(NULL, ":"));
	tile->num_h_tile = atoi(strtok(NULL, ":"));
	tile->num_v_tile = atoi(strtok(NULL, ":"));
	tile->tile_h_loc = atoi(strtok(NULL, ":"));
	tile->tile_v_loc = atoi(strtok(NULL, ":"));
	tile->tile_h_size = atoi(strtok(NULL, ":"));
	tile->tile_v_size = atoi(strtok(NULL, ":"));
}

/**
 * igt_reduce_format:
 * @format: drm fourcc
 *
 * Reduce @format to a base format. The aim is to allow grouping
 * sufficiently similar formats into classes. Formats with identical
 * component sizes, overall pixel size, chroma subsampling, etc. are
 * considered part of the same class, no matter in which order the
 * components appear. We arbitrarily choose one of the formats in
 * the class as the base format. Note that the base format itself
 * may not be supported by whatever device is being tested even if
 * some of the other formats in the class are supported.
 *
 * Returns: The base format for @format
 */
uint32_t igt_reduce_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_RGB332:
	case DRM_FORMAT_BGR233:
		return DRM_FORMAT_RGB332;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_BGRA5551:
		return DRM_FORMAT_XRGB1555;
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
		return DRM_FORMAT_RGB565;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
		return DRM_FORMAT_XRGB8888;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_BGRA1010102:
		return DRM_FORMAT_XRGB2101010;
	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		return DRM_FORMAT_XRGB16161616F;
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_VYUY:
		return DRM_FORMAT_YUYV;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		return DRM_FORMAT_NV12;
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
		return DRM_FORMAT_NV16;
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
		return DRM_FORMAT_NV24;
	case DRM_FORMAT_P010:
	case DRM_FORMAT_P012:
	case DRM_FORMAT_P016:
		return DRM_FORMAT_P010;
	case DRM_FORMAT_Y210:
	case DRM_FORMAT_Y212:
	case DRM_FORMAT_Y216:
		return DRM_FORMAT_Y210;
	case DRM_FORMAT_XYUV8888:
	case DRM_FORMAT_AYUV:
		return DRM_FORMAT_XYUV8888;
	case DRM_FORMAT_XVYU2101010:
	case DRM_FORMAT_Y410:
		return DRM_FORMAT_XVYU2101010;
	case DRM_FORMAT_XVYU12_16161616:
	case DRM_FORMAT_XVYU16161616:
	case DRM_FORMAT_Y412:
	case DRM_FORMAT_Y416:
		return DRM_FORMAT_XVYU12_16161616;
	default:
		return format;
	}
}

/**
 * igt_dump_connectors_fd:
 * @drmfd: handle to open drm device.
 *
 * Iterates through list of connectors and
 * dumps their list of modes.
 */
void igt_dump_connectors_fd(int drmfd)
{
	int i, j;

	drmModeRes *mode_resources = drmModeGetResources(drmfd);

	if (!mode_resources) {
		igt_warn("drmModeGetResources failed: %s\n", strerror(errno));
		return;
	}

	igt_info("Connectors:\n");
	igt_info("id\tencoder\tstatus\t\ttype\tsize (mm)\tmodes\n");
	for (i = 0; i < mode_resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnectorCurrent(drmfd,
				mode_resources->connectors[i]);
		if (!connector) {
			igt_warn("Could not get connector %i: %s\n",
				 mode_resources->connectors[i],
				 strerror(errno));
			continue;
		}

		igt_info("%d\t%d\t%s\t%s\t%dx%d\t\t%d\n",
			 connector->connector_id,
			 connector->encoder_id,
			 kmstest_connector_status_str(connector->connection),
			 kmstest_connector_type_str(connector->connector_type),
			 connector->mmWidth,
			 connector->mmHeight,
			 connector->count_modes);

		if (!connector->count_modes)
			continue;

		igt_info("  Modes:\n");
		igt_info("  name refresh (Hz) hdisp hss hse htot vdisp ""vss vse vtot flags type clock\n");
		for (j = 0; j < connector->count_modes; j++) {
			igt_info("[%d]", j);
			kmstest_dump_mode(&connector->modes[j]);
		}

		drmModeFreeConnector(connector);
	}
	igt_info("\n");

	drmModeFreeResources(mode_resources);
}

/**
 * igt_dump_crtcs_fd:
 * @drmfd: handle to open drm device.
 *
 * Iterates through the list of crtcs and
 * dumps out the mode and basic information
 * for each of them.
 */
void igt_dump_crtcs_fd(int drmfd)
{
	int i;
	drmModeRes *mode_resources;

	mode_resources = drmModeGetResources(drmfd);
	if (!mode_resources) {
		igt_warn("drmModeGetResources failed: %s\n", strerror(errno));
		return;
	}

	igt_info("CRTCs:\n");
	igt_info("id\tfb\tpos\tsize\n");
	for (i = 0; i < mode_resources->count_crtcs; i++) {
		drmModeCrtc *crtc;

		crtc = drmModeGetCrtc(drmfd, mode_resources->crtcs[i]);
		if (!crtc) {
			igt_warn("Could not get crtc %i: %s\n",
					mode_resources->crtcs[i],
					strerror(errno));
			continue;
		}
		igt_info("%d\t%d\t(%d,%d)\t(%dx%d)\n",
			 crtc->crtc_id,
			 crtc->buffer_id,
			 crtc->x,
			 crtc->y,
			 crtc->width,
			 crtc->height);

		kmstest_dump_mode(&crtc->mode);

		drmModeFreeCrtc(crtc);
	}
	igt_info("\n");

	drmModeFreeResources(mode_resources);
}

/**
 * igt_get_i915_edp_lobf_status
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Return: True if its enabled.
 */
bool igt_get_i915_edp_lobf_status(int drmfd, char *connector_name)
{
	char buf[24];
	int fd, res;

	fd = igt_debugfs_connector_dir(drmfd, connector_name, O_RDONLY);
	igt_assert(fd >= 0);

	res = igt_debugfs_simple_read(fd, "i915_edp_lobf_info", buf, sizeof(buf));
	igt_require(res > 0);

	close(fd);

	return strstr(buf, "LOBF status: enabled");
}

/**
 * igt_is_aux_less_alpm_enabled
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Check if Aux-less ALPM (Advanced Link Power Management) mode is enabled by reading
 * the i915_edp_lobf_info debugfs.
 *
 * Return: True if aux-less ALPM is enabled.
 */
bool igt_is_aux_less_alpm_enabled(int drmfd, char *connector_name)
{
	char buf[256];
	int fd, res;

	fd = igt_debugfs_connector_dir(drmfd, connector_name, O_RDONLY);
	if (fd < 0)
		return false;

	res = igt_debugfs_simple_read(fd, "i915_edp_lobf_info", buf, sizeof(buf));
	close(fd);

	if (res <= 0) {
		igt_info("Failed to read i915_edp_lobf_info for %s\n", connector_name);
		return false;
	}

	buf[res < sizeof(buf) ? res : sizeof(buf) - 1] = '\0';

	return strstr(buf, "Aux-less alpm status: enabled");
}

/**
 * igt_get_output_max_bpc:
 * @output: Output to query.
 * @maximum: Pointer to store the maximum supported bpc value.
 *
 * Read and parse the connector's debugfs output_bpc entry to obtain the
 * maximum supported bits-per-component value.
 *
 * Returns: true on success, false on failure.
 */
bool igt_get_output_max_bpc(igt_output_t *output, unsigned int *maximum)
{
	igt_display_t *display = output->display;
	int drmfd = display->drm_fd;
	char buf[24];
	char *start_loc;
	int fd, res;

	if (!maximum)
		return false;

	*maximum = 0;

	fd = igt_debugfs_connector_dir(drmfd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	res = igt_debugfs_simple_read(fd, "output_bpc", buf, sizeof(buf));
	close(fd);
	if (res <= 0)
		return false;

	buf[res < sizeof(buf) ? res : sizeof(buf) - 1] = '\0';

	start_loc = strstr(buf, "Maximum: ");
	if (!start_loc)
		return false;

	if (sscanf(start_loc, "Maximum: %u", maximum) != 1)
		return false;

	return true;
}

/**
 * igt_get_pipe_current_bpc:
 * @drmfd: A drm file descriptor
 * @pipe: Display pipe
 *
 * Returns: The current bpc from the crtc debugfs.
 */
unsigned int igt_get_crtc_current_bpc(igt_crtc_t *crtc)
{
	igt_display_t *display = crtc->display;
	int drmfd = display->drm_fd;
	char buf[24];
	char debugfs_name[24];
	char *start_loc;
	int fd, res;
	unsigned int current;

	fd = igt_crtc_debugfs_dir(crtc);
	igt_assert(fd >= 0);

	if (is_intel_device(drmfd))
		strcpy(debugfs_name, "i915_current_bpc");
	else if (is_amdgpu_device(drmfd))
		strcpy(debugfs_name, "amdgpu_current_bpc");
	else if (is_mtk_device(drmfd))
		strcpy(debugfs_name, "mtk_current_bpc");

	res = igt_debugfs_simple_read(fd, debugfs_name, buf, sizeof(buf));
	igt_require(res > 0);

	close(fd);

	igt_assert(start_loc = strstr(buf, "Current: "));
	igt_assert_eq(sscanf(start_loc, "Current: %u", &current), 1);

	return current;
}

static unsigned int get_current_bpc(igt_crtc_t *crtc, igt_output_t *output,
				    unsigned int bpc)
{
	unsigned int maximum;
	unsigned int current = igt_get_crtc_current_bpc(crtc);

	igt_require_f(igt_get_output_max_bpc(output, &maximum),
		      "Failed to read max bpc for %s\n", igt_output_name(output));

	igt_require_f(maximum >= bpc,
		      "Monitor doesn't support %u bpc, max is %u\n", bpc,
		      maximum);

	return current;
}

/**
 * igt_assert_output_bpc_equal:
 * @drmfd: A drm file descriptor
 * @pipe: Display pipe
 * @output_name: Name of the libdrm connector we're going to use
 * @bpc: BPC to compare with max & current bpc
 *
 * Assert if crtc's current bpc is not matched with the requested one.
 */
void igt_assert_output_bpc_equal(igt_crtc_t *crtc, igt_output_t *output,
				 unsigned int bpc)
{
	unsigned int current = get_current_bpc(crtc,
					       output, bpc);

	igt_assert_eq(current, bpc);
}

/**
 * igt_check_output_bpc_equal:
 * @drmfd: A drm file descriptor
 * @pipe: Display pipe
 * @output_name: Name of the libdrm connector we're going to use
 * @bpc: BPC to compare with max & current bpc
 *
 * This is similar to igt_assert_output_bpc_equal, instead of assert
 * it'll return True if crtc has the correct requested bpc, else False.
 *
 * Returns: True if crtc's current bpc is matched with the requested bpc,
 * else False.
 */
bool igt_check_output_bpc_equal(igt_crtc_t *crtc, igt_output_t *output,
				unsigned int bpc)
{
	unsigned int current = get_current_bpc(crtc,
					       output, bpc);

	return (current == bpc);
}

/**
 * igt_max_bpc_constraint:
 * @display: a pointer to an #igt_display_t structure
 * @pipe: Display pipe
 * @output: Target output
 * @bpc: BPC to compare with max & current bpc
 *
 * The "max bpc" property only ensures that the bpc will not go beyond
 * the value set through this property. It does not guarantee that the
 * same bpc will be used for the given mode.
 *
 * So, if we really want a particular bpc set, try reducing the resolution
 * till we get the bpc that we set in max bpc property.
 *
 * Returns: True if suitable mode found to use requested bpc, else False.
 */
bool igt_max_bpc_constraint(igt_display_t *display, igt_crtc_t *crtc,
			    igt_output_t *output, int bpc)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *mode;

	igt_sort_connector_modes(connector, sort_drm_modes_by_clk_dsc);

	for_each_connector_mode(output, mode) {
		igt_output_override_mode(output, mode);

		if (is_intel_device(display->drm_fd) &&
		    !igt_check_bigjoiner_support(display))
			continue;

		if (igt_display_try_commit2(display,
					    display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY))
			continue;

		if (!igt_check_output_bpc_equal(crtc, output, bpc))
			continue;

		return true;
	}

	igt_output_override_mode(output, NULL);
	return false;
}

static int read_and_parse_cdclk_debugfs(int fd, const char *check_str)
{
	char buf[4096];
	char *s;
	int dir, res, clk = 0;
	drmModeRes *resources;

	if (!is_intel_device(fd))
		return 0;

	/* If there is no display, then no point to check further. */
	resources = drmModeGetResources(fd);
	if (!resources)
		return 0;

	drmModeFreeResources(resources);

	dir = igt_debugfs_dir(fd);
	igt_require(dir != -1);

	/*
	 * Display specific clock frequency info is moved to i915_cdclk_info,
	 * On older kernels if this debugfs is not found, fallback to read from
	 * i915_frequency_info.
	 */
	res = igt_debugfs_simple_read(dir, "i915_cdclk_info",
				      buf, sizeof(buf));
	if (res <= 0)
		res = igt_debugfs_simple_read(dir, "i915_frequency_info",
					      buf, sizeof(buf));
	close(dir);

	igt_require(res > 0);

	igt_assert(s = strstr(buf, check_str));
	s += strlen(check_str);
	igt_assert_eq(sscanf(s, "%d kHz", &clk), 1);

	return clk;
}

/**
 * igt_get_max_dotclock:
 * @fd: A drm file descriptor
 *
 * Get the Max pixel clock frequency from intel specific debugfs
 * "i915_frequency_info"/"i915_cdclk_info".
 *
 * Returns: Max pixel clock frequency, otherwise 0.
 */
int igt_get_max_dotclock(int fd)
{
	int max_dotclock = read_and_parse_cdclk_debugfs(fd, "Max pixel clock frequency:");

	/* 100 Mhz to 5 GHz seem like reasonable values to expect */
	if (max_dotclock > 0) {
		igt_assert_lt(max_dotclock, 5000000);
		igt_assert_lt(100000, max_dotclock);
	}

	return max_dotclock > 0 ? max_dotclock : 0;
}

/**
 * igt_get_max_cdclk:
 * @fd: A drm file descriptor
 *
 * Get the max CD clock frequency from intel specific debugfs
 * "i915_frequency_info"/"i915_cdclk_info".
 *
 * Returns: Max CD clk frequency, otherwise 0.
 */
int igt_get_max_cdclk(int fd)
{
	return read_and_parse_cdclk_debugfs(fd, "Max CD clock frequency:");
}

/**
 * igt_get_current_cdclk:
 * @fd: A drm file descriptor
 *
 * Get the current CD clock frequency from intel specific debugfs
 * "i915_frequency_info"/"i915_cdclk_info".
 *
 * Returns: Current CD clock frequency, otherwise 0.
 */
int igt_get_current_cdclk(int fd)
{
	return read_and_parse_cdclk_debugfs(fd, "Current CD clock frequency:");
}

/**
 * intel_get_max_pipe_hdisplay:
 * @drm_fd: drm file descriptor
 *
 * Get the maximum hdisplay (width) supported by a single pipe on the
 * platform. This is a public function for querying platform capabilities.
 *
 * Returns: The maximum hdisplay supported per pipe.
 */
int intel_get_max_pipe_hdisplay(int drm_fd)
{
	int dev_id = intel_get_drm_devid(drm_fd);

	return (intel_display_ver(dev_id) >= 30) ? HDISPLAY_6K_PER_PIPE :
						   HDISPLAY_5K_PER_PIPE;
}

struct joiner_mode_exception {
	uint16_t hdisplay, vdisplay;
	uint32_t clock;
};

static bool match_joiner_exception(drmModeModeInfo *mode,
				   const struct joiner_mode_exception *list,
				   int count)
{
	for (int i = 0; i < count; i++) {
		if (mode->hdisplay == list[i].hdisplay &&
		    mode->vdisplay == list[i].vdisplay &&
		    mode->clock == list[i].clock)
			return true;
	}

	return false;
}

/*
 * TODO: IGT cannot compute the DSC bubble overhead the driver adds to the
 * effective pixel rate, since it cannot estimate the DSC parameters -
 * whether DSC is used and how many slices. On NVL this lets modes like
 * 6144x3456@60 pass igt_bigjoiner_possible()'s clock/hdisplay checks,
 * yet the kernel still enables bigjoiner for them.
 *
 * Add mode_needs_joiner_exception() as a stopgap.
 */
static const struct joiner_mode_exception nvl_joiner_exception_modes[] = {
	{ 6144, 3456, 1413390 }, /* 6144x3456@60Hz */
	{ 3840, 2160, 1543000 }, /* 3840x2160@165Hz */
	{ 3840, 2160, 1398400 }, /* 3840x2160@160Hz */
	{ 3840, 2160, 2291120 }, /* 3840x2160@240 */
};

/*
 * mode_needs_joiner_exception - check if a mode requires joiner via explicit exception
 * @drm_fd: drm file descriptor
 * @mode: libdrm mode
 *
 * On some platforms, a higher max_dotclock means certain modes won't trigger
 * the standard clock or hdisplay checks even though the kernel enables joiner
 * for them. Each platform has its own exception list.
 *
 * Returns: True if the mode is a known joiner exception, else False.
 */
static bool mode_needs_joiner_exception(int drm_fd, drmModeModeInfo *mode)
{
	unsigned int disp_ver = intel_display_ver(intel_get_drm_devid(drm_fd));

	if (disp_ver == 35)
		return match_joiner_exception(mode, nvl_joiner_exception_modes,
					      ARRAY_SIZE(nvl_joiner_exception_modes));

	return false;
}

/**
 * igt_bigjoiner_possible:
 * @drm_fd: drm file descriptor
 * @mode: libdrm mode
 * @max_dotclock: Max pixel clock frequency
 *
 * Bigjoiner is required when the requested mode exceeds single-pipe
 * platform limits, i.e. hdisplay is above the platform threshold or
 * clock is above @max_dotclock.
 *
 * Returns: True if mode requires Bigjoiner, else False.
 */
bool igt_bigjoiner_possible(int drm_fd, drmModeModeInfo *mode, int max_dotclock)
{
	/**
	 * TODO: remove mode_needs_joiner_exception() once IGT can estimate
	 * DSC parameters accurately
	 */
	return (mode_needs_joiner_exception(drm_fd, mode) ||
		mode->hdisplay > intel_get_max_pipe_hdisplay(drm_fd) ||
		mode->clock > max_dotclock);
}

/**
 * bigjoiner_mode_found:
 * @drm_fd: drm file descriptor
 * @connector: libdrm connector
 * @max_dot_clock: max dot clock frequency
 * @mode: libdrm mode to be filled
 *
 * Finds the first connector mode that requires bigjoiner support based on
 * igt_bigjoiner_possible(), while excluding modes that require ultrajoiner.
 *
 * Returns: True if a bigjoiner mode is found.
 */
bool bigjoiner_mode_found(int drm_fd, drmModeConnector *connector,
			  int max_dotclock, drmModeModeInfo *mode)
{
	bool found = false;

	for (int i=0; i< connector->count_modes; i++) {
		if (igt_bigjoiner_possible(drm_fd, &connector->modes[i], max_dotclock) &&
		    !igt_ultrajoiner_possible(drm_fd, &connector->modes[i], max_dotclock)) {
			*mode = connector->modes[i];
			found = true;
			break;
		}
	}
	return found;
}

/**
 * intel_boundary_non_joiner_mode_found:
 * @drm_fd: drm file descriptor
 * @connector: libdrm connector
 * @max_dot_clock: max dot clock frequency
 * @mode: libdrm mode to be filled
 *
 * Check if connector supports a mode at the platform's maximum single-pipe
 * hdisplay boundary.
 *
 * Returns: True if a valid non-joiner mode at the boundary is found,
 * false otherwise.
 */
bool intel_boundary_non_joiner_mode_found(int drm_fd, drmModeConnector *connector,
					  int max_dotclock, drmModeModeInfo *mode)
{
	int max_hdisplay = intel_get_max_pipe_hdisplay(drm_fd);

	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->hdisplay == max_hdisplay &&
		    current_mode->clock < max_dotclock &&
		    !mode_needs_joiner_exception(drm_fd, current_mode)) {
			*mode = *current_mode;
			return true;
		}
	}

	return false;
}

/* TODO: Move these lib functions to the joiner-specific library file
 *	 once those patches are merged.
 */

/**
 * igt_is_joiner_enabled_for_pipe:
 * @drmfd: A drm file descriptor
 * @pipe: display pipe
 *
 * Returns: True if joiner is enabled, false otherwise.
 */
bool igt_is_joiner_enabled_for_pipe(int drmfd, enum hardware_pipe pipe)
{
	char buf[16384], master_str[64], slave_str[64];
	int dir, res;
	unsigned  int pipe_mask = (1 << 0) | (1 << 1);

	dir = igt_debugfs_dir(drmfd);
	igt_assert(dir >= 0);

	res = igt_debugfs_simple_read(dir, "i915_display_info",
					    buf, sizeof(buf));
	close(dir);
	igt_assert(res >= 0);
	pipe_mask <<= pipe;

	snprintf(master_str, sizeof(master_str),
		 "Linked to 0x%x pipes as a master", pipe_mask);
	snprintf(slave_str, sizeof(slave_str),
		 "Linked to 0x%x pipes as a slave", pipe_mask);

	return (strstr(buf, master_str) && strstr(buf, slave_str));
}

/**
 * igt_ultrajoiner_possible:
 * @mode: libdrm mode
 * @max_dotclock: Max pixel clock frequency
 *
 * Ultrajoiner will come into the picture, when the requested
 * mode resolution > 10K or mode clock > 2 * max_dotclock.
 *
 * Returns: True if mode requires Ultrajoiner, else False.
 */
bool igt_ultrajoiner_possible(int drm_fd, drmModeModeInfo *mode, int max_dotclock)
{
	return (mode->hdisplay > 2 * intel_get_max_pipe_hdisplay(drm_fd) ||
		mode->clock > 2 * max_dotclock);
}

/**
 * Ultrajoiner_mode_found:
 * @drm_fd: drm file descriptor
 * @connector: libdrm connector
 * @max_dot_clock: max dot clock frequency
 * @mode: libdrm mode to be filled
 *
 * Ultrajoiner will come in to the picture when the
 * resolution > 10K or clock > 2 * max-dot-clock.
 *
 * Returns: True if ultra joiner found in connector modes
 */
bool ultrajoiner_mode_found(int drm_fd, drmModeConnector *connector,
			  int max_dotclock, drmModeModeInfo *mode)
{
	bool found = false;

	for (int i = 0; i < connector->count_modes; i++) {
		if (igt_ultrajoiner_possible(drm_fd, &connector->modes[i], max_dotclock)) {
			*mode = connector->modes[i];
			found = true;
			break;
		}
	}

	return found;
}

/**
 * igt_get_non_joiner_mode:
 * @drm_fd: drm file descriptor
 * @output: pointer to the output structure
 *
 * Finds the display mode from the output that does not require
 * Big Joiner or Ultra Joiner.
 *
 * Returns: Pointer to non-joiner mode, or NULL if not found.
 */
drmModeModeInfo *igt_get_non_joiner_mode(int drm_fd, igt_output_t *output)
{
	drmModeConnector *connector;
	int max_dotclock;

	connector = output->config.connector;
	max_dotclock = igt_get_max_dotclock(drm_fd);

	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		/* Check if mode requires joiner */
		if (!igt_bigjoiner_possible(drm_fd, current_mode, max_dotclock) &&
		    !igt_ultrajoiner_possible(drm_fd, current_mode, max_dotclock)) {
			return current_mode;
		}
	}

	return NULL;
}

/**
 * is_joiner_mode:
 * @drm_fd: drm file descriptor
 * @output: pointer to the output structure
 *
 * Checks if the current configuration requires Big Joiner or Ultra Joiner mode
 * based on the maximum dot clock and connector settings.
 *
 * Returns: True if joiner mode is required, otherwise False.
 */
bool is_joiner_mode(int drm_fd, igt_output_t *output)
{
	bool is_joiner = false;
	bool is_ultra_joiner = false;
	int max_dotclock;
	drmModeModeInfo mode;

        if (!is_intel_device(drm_fd))
                return false;

	max_dotclock = igt_get_max_dotclock(drm_fd);
	is_joiner = bigjoiner_mode_found(drm_fd,
					 output->config.connector,
					 max_dotclock, &mode);
	is_ultra_joiner = ultrajoiner_mode_found(drm_fd,
						 output->config.connector,
						 max_dotclock, &mode);

	if (is_joiner || is_ultra_joiner)
		return true;

	return false;
}

/**
 * igt_has_force_joiner_debugfs
 * @drmfd: A drm file descriptor
 * @conn_name: Name of the connector
 *
 * Checks if the force big joiner debugfs is available
 * for a specific connector.
 *
 * Returns:
 *  true if the debugfs is available, false otherwise.
 */
bool igt_has_force_joiner_debugfs(int drmfd, char *conn_name)
{
	char buf[512];
	int debugfs_fd, ret;

	/*
	 * bigjoiner is supported on display<= 12 with DSC only
	 * and only on Pipe A for Display 11
	 * For simplicity avoid Display 11 and 12, check for >= 13
	 */
	if (intel_display_ver(intel_get_drm_devid(drmfd)) < 13)
		return false;

	igt_assert_f(conn_name, "Connector name cannot be NULL\n");
	debugfs_fd = igt_debugfs_connector_dir(drmfd, conn_name, O_RDONLY);
	if (debugfs_fd < 0)
		return false;

	ret = igt_debugfs_simple_read(debugfs_fd, "i915_joiner_force_enable", buf, sizeof(buf));
	close(debugfs_fd);

	return ret >= 0;
}

/**
 * igt_check_force_joiner_status
 * @drmfd: file descriptor of the DRM device.
 * @connector_name: connector to check.
 *
 * Checks if the force joiner is enabled.
 *
 * Returns: True if the force joiner is enabled, False otherwise.
 */
bool igt_check_force_joiner_status(int drmfd, char *connector_name)
{
	char buf[512];
	int debugfs_fd, ret;

	if (!connector_name)
		return false;

	debugfs_fd = igt_debugfs_connector_dir(drmfd, connector_name, O_RDONLY);
	if (debugfs_fd < 0) {
		igt_debug("Could not open debugfs for connector: %s\n", connector_name);
		return false;
	}

	ret = igt_debugfs_simple_read(debugfs_fd, "i915_joiner_force_enable", buf, sizeof(buf));
	close(debugfs_fd);

	if (ret < 0) {
		igt_debug("Could not read i915_joiner_force_enable for connector: %s\n", connector_name);
		return false;
	}

	return (atoi(buf) > 1);
}

/**
 * igt_check_bigjoiner_support:
 * @display: a pointer to an #igt_display_t structure
 *
 * Get all active pipes from connected outputs (i.e. pending_crtc != NULL)
 * and check those pipes supports the selected mode(s).
 *
 * Example:
 *  * Pipe-D can't support mode > 5K
 *  * To use 8K mode on a pipe then consecutive pipe must be free.
 *
 * Returns: True if a valid crtc/connector mode combo found, else false
 */
bool igt_check_bigjoiner_support(igt_display_t *display)
{
	uint8_t i, pipes_in_use = 0;
	igt_crtc_t *crtc;
	igt_output_t *output;
	enum hardware_pipe last_pipe = PIPE_NONE;
	struct {
		enum hardware_pipe idx;
		drmModeModeInfo *mode;
		igt_output_t *output;
		bool force_joiner;
	} pipes[IGT_MAX_PIPES];
	int max_dotclock;

	/* Get last pipe */
	for_each_crtc(display, crtc) {
		if (crtc->hardware_pipe > last_pipe)
			last_pipe = crtc->hardware_pipe;
	}

	/*
	 * Get list of CRTCs in use those were set by igt_output_set_crtc()
	 * just before calling this function.
	 */
	for_each_connected_output(display, output) {
		if (!output->pending_crtc)
			continue;

		pipes[pipes_in_use].idx = output->pending_crtc->hardware_pipe;
		pipes[pipes_in_use].mode = igt_output_get_mode(output);
		pipes[pipes_in_use].output = output;
		pipes[pipes_in_use].force_joiner = igt_check_force_joiner_status(display->drm_fd, output->name);
		pipes_in_use++;
	}

	if (!pipes_in_use) {
		igt_info("We must set at least one output to pipe.\n");
		return true;
	}

	max_dotclock = igt_get_max_dotclock(display->drm_fd);

	/*
	 * if force joiner is enabled (or) the mode requires bigjoiner,
	 * then ignore
	 *  - if the consecutive pipe is not available
	 *  - last crtc in single/multi-connector config
	 *  - consecutive crtcs in multi-connector config
	 *
	 * in multi-connector config ignore if
	 *  - previous crtc (force joiner or mode requires bigjoiner) and
	 *  - current & previous crtcs are consecutive
	 */
	for (i = 0; i < pipes_in_use; i++) {
		if (pipes[i].force_joiner ||
		    igt_bigjoiner_possible(display->drm_fd, pipes[i].mode, max_dotclock)) {
			igt_info("pipe-%s-%s: (Max dot-clock: %d KHz), force joiner: %s\n",
				 kmstest_pipe_name(pipes[i].idx),
				 igt_output_name(pipes[i].output),
				 max_dotclock, pipes[i].force_joiner ? "Yes" : "No");
			kmstest_dump_mode(pipes[i].mode);

			if (pipes[i].idx >= last_pipe) {
				igt_info("pipe-%s: Last pipe couldn't be used as a Bigjoiner Primary.\n",
					 kmstest_pipe_name(pipes[i].idx));
				return false;
			}

			for (int j = 0; j < pipes_in_use; j++) {
				if (pipes[j].idx == pipes[i].idx + 1) {
					igt_info("pipe-%s: Next pipe is already assigned to another output.\n",
						 kmstest_pipe_name(pipes[j].idx));
					return false;
				}
			}

			if (!igt_crtc_for_pipe(display, pipes[i].idx + 1)) {
				igt_info("Consecutive pipe-%s: Fused-off, couldn't be used as a Bigjoiner Secondary.\n",
					 kmstest_pipe_name(pipes[i].idx + 1));
				return false;
			}

			if ((i < (pipes_in_use - 1)) &&
			    (abs(pipes[i + 1].idx - pipes[i].idx) <= 1)) {
				igt_info("Consecutive pipe-%s: Not free to use it as a Bigjoiner Secondary.\n",
					 kmstest_pipe_name(pipes[i + 1].idx));
				return false;
			}
		}

		if ((i > 0) && (pipes[i - 1].force_joiner ||
				igt_bigjoiner_possible(display->drm_fd, pipes[i - 1].mode, max_dotclock))) {
			igt_info("pipe-%s-%s: (Max dot-clock: %d KHz), force joiner: %s\n",
				 kmstest_pipe_name(pipes[i - 1].idx),
				 igt_output_name(pipes[i - 1].output),
				 max_dotclock, pipes[i - 1].force_joiner ? "Yes" : "No");
			kmstest_dump_mode(pipes[i - 1].mode);

			if (!igt_crtc_for_pipe(display, pipes[i - 1].idx + 1)) {
				igt_info("Consecutive pipe-%s: Fused-off, couldn't be used as a Bigjoiner Secondary.\n",
					 kmstest_pipe_name(pipes[i - 1].idx + 1));
				return false;
			}

			if (abs(pipes[i].idx - pipes[i - 1].idx) <= 1) {
				igt_info("Consecutive pipe-%s: Not free to use it as a Bigjoiner Secondary.\n",
					 kmstest_pipe_name(pipes[i].idx));
				return false;
			}
		}
	}

	return true;
}

/**
 * igt_parse_mode_string:
 * @mode_string: modeline string
 * @mode: a pointer to a drm mode structure
 *
 * Parse mode string and populate mode
 *
 * Format: clock(MHz),hdisp,hsync-start,hsync-end,htotal,vdisp,vsync-start,
 * vsync-end,vtotal
 *
 * Returns: True if the correct number of arguments are entered, else false.
 */
bool igt_parse_mode_string(const char *mode_string, drmModeModeInfo *mode)
{
	float force_clock;

	if (sscanf(mode_string, "%f,%hu,%hu,%hu,%hu,%hu,%hu,%hu,%hu",
	   &force_clock, &mode->hdisplay, &mode->hsync_start, &mode->hsync_end, &mode->htotal,
	   &mode->vdisplay, &mode->vsync_start, &mode->vsync_end, &mode->vtotal) != 9)
		return false;

	mode->clock = force_clock * 1000;

	return true;
}

/**
 * intel_pipe_output_combo_valid:
 * @display: a pointer to an #igt_display_t structure
 *
 * Every individual test must use igt_output_set_crtc() before calling this
 * helper, so that this function will get all active pipes from connected
 * outputs (i.e. pending_crtc != NULL) and check the selected combo is
 * valid or not.
 *
 * This helper is supposed to be a superset of all constraints of pipe/output
 * combo.
 *
 * Example:
 *  * Pipe-D can't support mode > 5K
 *  * To use 8K mode on a pipe then consecutive pipe must be free.
 *  * MSO is supported only on PIPE_A/PIPE_B.
 *
 * Returns: True if a valid pipe/output mode combo found, else false
 */
bool intel_pipe_output_combo_valid(igt_display_t *display)
{
	int combo = 0;
	igt_output_t *output;

	for_each_connected_output(display, output) {
		if (!output->pending_crtc)
			continue;

		if (!igt_crtc_connector_valid(output->pending_crtc, output)) {
			igt_info("Output %s is disconnected (or) pipe-%s & %s cannot be used together\n",
				 igt_output_name(output),
				 igt_crtc_name(output->pending_crtc),
				 igt_output_name(output));
			return false;
		}

		combo++;
	}

	if (!combo) {
		igt_info("At least one pipe/output combo needed.\n");
		return false;
	}

	if (!is_intel_device(display->drm_fd))
		return true;

	/*
	 * Check the given pipe/output combo is valid for Bigjoiner.
	 *
	 * TODO: Update this helper to support other features like MSO.
	 */
	return igt_check_bigjoiner_support(display);
}

/**
 * igt_check_output_is_dp_mst:
 * @output: Target output
 *
 * Returns: True if output is dp-mst, else false.
 */
bool igt_check_output_is_dp_mst(igt_output_t *output)
{
	return !!output->config.connector_path;
}

static int parse_path_connector(char *connector_path)
{
	int connector_id;
	char *encoder;
	char *connector_path_copy = strdup(connector_path);

	encoder = strtok(connector_path_copy, ":");
	igt_assert_f(!strcmp(encoder, "mst"), "PATH connector property expected to have 'mst'\n");
	connector_id = atoi(strtok(NULL, "-"));
	free(connector_path_copy);

	return connector_id;
}

/**
 * igt_get_dp_mst_connector_id:
 * @output: Target output
 *
 * Returns: Connector id if output is dp-mst, else -EINVAL.
 */
int igt_get_dp_mst_connector_id(igt_output_t *output)
{
	int connector_id;

	if (!igt_check_output_is_dp_mst(output))
		return -EINVAL;

	connector_id = parse_path_connector(output->config.connector_path);

	return connector_id;
}

/**
 * igt_crtc_num_scalers:
 * @crtc: the CRTC
 *
 * Returns: Number of scalers supported on the CRTC.
 */
int igt_crtc_num_scalers(igt_crtc_t *crtc)
{
	igt_display_t *display = crtc->display;
	char buf[8120];
	char *start_loc1, *start_loc2;
	int dir, res;
	int num_scalers = 0;
	int drm_fd = display->drm_fd;
	char dest[20] = ":pipe ";

	strcat(dest, igt_crtc_name(crtc));

	if (is_intel_device(drm_fd) &&
	    intel_display_ver(intel_get_drm_devid(drm_fd)) >= 9) {

		dir = igt_debugfs_dir(drm_fd);
		igt_assert(dir >= 0);

		res = igt_debugfs_simple_read(dir, "i915_display_info", buf, sizeof(buf));
		close(dir);
		igt_require(res > 0);

		start_loc1 = strstr(buf, dest);

		if ((start_loc1 = strstr(buf, dest))) {
			igt_assert(start_loc2 = strstr(start_loc1, "num_scalers="));
			igt_assert_eq(sscanf(start_loc2, "num_scalers=%d", &num_scalers), 1);
		}
	} else if (is_msm_device(drm_fd)) {
		igt_plane_t *plane;

		/*
		 * msm devices have dma pipes (no csc, no scaling), rgb
		 * pipes (no csc, has scaling), and vid pipes (has csc,
		 * has scaling), but not all devices have rgb pipes.
		 * We can use the # of pipes that support YUV formats
		 * as a rough approximation of the # of scalars.. it may
		 * undercount on some hw, but it will not overcount
		 */
		for_each_plane_on_crtc(crtc, plane) {
			uint32_t format;

			for_each_plane_format(plane, format) {
				if (igt_format_is_yuv(format)) {
					num_scalers++;
					break;
				}
			}
		}
	}

	return num_scalers;
}

/**
 * igt_backlight_read:
 * @result:	Pointer to store the result
 * @fname:	Name of the file to read
 * @context:	Pointer to the context structure
 */
int igt_backlight_read(int *result, const char *fname, igt_backlight_context_t *context)
{
	int fd;
	char full[PATH_MAX];
	char dst[64];
	int r, e;

	igt_assert(snprintf(full, PATH_MAX, "%s/%s/%s",
			    context->backlight_dir_path,
			    context->path,
			    fname) < PATH_MAX);

	fd = open(full, O_RDONLY);
	if (fd == -1)
		return -errno;

	r = read(fd, dst, sizeof(dst));
	e = errno;
	close(fd);

	if (r < 0)
		return -e;

	errno = 0;
	*result = strtol(dst, NULL, 10);
	return errno;
}

/**
 * igt_backlight_write:
 * @value:		Value to write
 * @fname:		Name of the file to write
 * @context:	Pointer to the context structure
 */
int igt_backlight_write(int value, const char *fname, igt_backlight_context_t *context)
{
	int fd;
	char full[PATH_MAX];
	char src[64];
	int len;

	igt_assert(snprintf(full, PATH_MAX, "%s/%s/%s",
			    context->backlight_dir_path,
			    context->path,
			    fname) < PATH_MAX);

	fd = open(full, O_WRONLY);
	if (fd == -1)
		return -errno;

	len = snprintf(src, sizeof(src), "%i", value);
	len = write(fd, src, len);
	close(fd);

	if (len < 0)
		return len;

	return 0;
}

/**
 * igt_get_writeback_formats_blob:
 * @output: Target output
 *
 * get supported formats from the writeback connector
 *
 * Returns: pointer to the writeback formats blob or NULL if not available
 */
drmModePropertyBlobRes *igt_get_writeback_formats_blob(igt_output_t *output)
{
	drmModePropertyBlobRes *blob = NULL;
	uint64_t blob_id;
	int ret;

	ret = kmstest_get_property(output->display->drm_fd,
				   output->config.connector->connector_id,
				   DRM_MODE_OBJECT_CONNECTOR,
				   igt_connector_prop_names[IGT_CONNECTOR_WRITEBACK_PIXEL_FORMATS],
				   NULL, &blob_id, NULL);
	if (ret)
		blob = drmModeGetPropertyBlob(output->display->drm_fd, blob_id);

	return blob;
}

/**
 * igt_get_writeback_fb_id - Get writeback framebuffer ID
 * @display: display structure
 *
 * Returns the framebuffer ID for writeback capture, or 0 if none configured.
 */
uint64_t igt_get_writeback_fb_id(igt_output_t *output)
{
	return igt_output_get_prop(output, IGT_CONNECTOR_WRITEBACK_FB_ID);
}

/**
 * igt_detach_crtc - Detach the crtc from the output
 * @display: display structure
 * @output: output structure
 *
 * Detach the crtc from the output.
 */
void igt_detach_crtc(igt_display_t *display, igt_output_t *output)
{
	if (igt_get_writeback_fb_id(output) == 0)
		return;

	igt_output_set_crtc(output, NULL);
	igt_display_commit2(display, COMMIT_ATOMIC);
}

/**
 * igt_get_and_wait_out_fence - Get and wait for the out fence
 * @output: output structure
 *
 * wait for fence from the writeback.
 */
void igt_get_and_wait_out_fence(igt_output_t *output)
{
	int ret;

	igt_assert(output->writeback_out_fence_fd >= 0);

	ret = sync_fence_wait(output->writeback_out_fence_fd, 1000);
	igt_assert_f(ret == 0, "sync_fence_wait failed: %s\n", strerror(-ret));
	close(output->writeback_out_fence_fd);
	output->writeback_out_fence_fd = -1;
}

/**
 * igt_get_connected_output_count:
 * @display: pointer to igt_display_t
 *
 * Get the number of actively connected outputs.
 *
 * Returns: the count of connected outputs.
 */
uint32_t igt_get_connected_output_count(igt_display_t *display)
{
	uint32_t conn_outputs = 0;

	for (int i = 0; i < display->n_outputs; i++) {
		if (igt_output_is_connected(&display->outputs[i]))
			conn_outputs++;
	}
	return conn_outputs;
}

/**
 * Checks if the lobf debugfs
 * is available for a specific output.
 *
 * @drmfd: file descriptor of the DRM device.
 * @output: output to check.
 * Returns:
 *  true if the debugfs is available, false otherwise.
 */
bool igt_has_lobf_debugfs(int drmfd, igt_output_t *output)
{
        char buf[512];
        int res;

        res = igt_debugfs_read_connector_file(drmfd, output->name,
                                              "i915_edp_lobf_info",
                                              buf, sizeof(buf));
        return res == 0;
}

igt_crtc_t *igt_crtc_for_crtc_id(igt_display_t *display, uint32_t crtc_id)
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		if (crtc->crtc_id == crtc_id)
			return crtc;
	}

	return NULL;
}

igt_crtc_t *igt_crtc_for_crtc_index(igt_display_t *display, int crtc_index)
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		if (crtc->crtc_index == crtc_index)
			return crtc;
	}

	return NULL;
}

igt_crtc_t *igt_crtc_for_pipe(igt_display_t *display, enum hardware_pipe pipe)
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		if (crtc->hardware_pipe == pipe)
			return crtc;
	}

	return NULL;
}

/*
 * igt_first_crtc:
 * @display: pointer to igt_display_t
 *
 * Returns: The first CRTC on the device
 */
igt_crtc_t *igt_first_crtc(igt_display_t *display)
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc)
		return crtc;

	igt_skip("No CRTCs on device\n");

	return NULL;
}

/**
 * igt_first_crtc_with_single_output:
 * @display: a pointer to an #igt_display_t structure
 * @ret_output: Returned output
 *
 * Returns: The first CRTC with a connected output (the output
 * is returned via @ret_output)
 */
igt_crtc_t *igt_first_crtc_with_single_output(igt_display_t *display, igt_output_t **ret_output)
{
	igt_output_t *output;
	igt_crtc_t *crtc;

	for_each_crtc_with_single_output(display, crtc, output) {
		*ret_output = output;
		return crtc;
	}

	igt_skip("No valid CRTC/output combinations\n");

	*ret_output = NULL;

	return NULL;
}

/*
 * igt_next_crtc:
 * @display: pointer to igt_display_t
 * @crtc: pointer to igt_crtc_t, or NULL for first
 *
 * Returns: The next CRTC on the device
 */
igt_crtc_t *igt_next_crtc(igt_display_t *display, igt_crtc_t *crtc)
{
	igt_crtc_t *next;

	for_each_crtc(display, next) {
		if (!crtc || next->hardware_pipe > crtc->hardware_pipe)
			return next;
	}

	return NULL;
}

/*
 * igt_random_crtc:
 * @display: pointer to igt_display_t
 *
 * Returns: A random CRTC on the device
 */
igt_crtc_t *igt_random_crtc(igt_display_t *display)
{
	int n = igt_display_n_crtcs(display);

	igt_skip_on_f(!n, "No CRTCs on device\n");

	return igt_crtc_for_crtc_index(display, rand() % n);
}

static drmModeConnectorPtr igt_wait_for_connector(int drm_fd, unsigned int connector_id,
						  double timeout)
{
	drmModeConnectorPtr connector = NULL;
	struct timespec start, end;

	connector = drmModeGetConnector(drm_fd, connector_id);
	/*
	 * This time is required as sometimes some id in the connector list are not totally
	 * ready or can disappear
	 */
	igt_assert_eq(igt_gettime(&start), 0);
	end = start;

	while (!connector &&
	       igt_time_elapsed(&start, &end) <= timeout) {
		drmModeFreeConnector(connector);
		connector = drmModeGetConnector(drm_fd, connector_id);
		usleep(WAIT_FOR_CONNECTOR_LOOP_SLEEP_US);
		igt_assert_eq(igt_gettime(&end), 0);
	}

	return connector;
}

/**
 * igt_wait_for_connector_status:
 * @drm_fd: drm file descriptor
 * @connector_id: connector to monitor
 * @timeout: maximum duration to wait, in second. Use -1.0 to set the timeout
 *           to igt_default_detect_timeout().
 * @drm_mode: mode to wait for, see enum drmModeConnection
 *
 * Wait for at most @timeout that the connector @connector_id  status
 * become @drm_mode
 * Returns: true when the status is reached, false if there is a timeout
 */
bool igt_wait_for_connector_status(int drm_fd, unsigned int connector_id, double timeout,
				   int drm_mode)
{
	drmModeConnector *connector;
	struct timespec start, end;

	if (timeout < 0.0)
		timeout = igt_default_display_detect_timeout();

	igt_assert_eq(igt_gettime(&start), 0);
	end = start;

	while (igt_time_elapsed(&start, &end) <= timeout) {
		connector = drmModeGetConnector(drm_fd, connector_id);
		if (connector && connector->connection == drm_mode) {
			drmModeFreeConnector(connector);
			return true;
		}
		drmModeFreeConnector(connector);
		usleep(WAIT_FOR_CONNECTOR_LOOP_SLEEP_US);
		igt_assert_eq(igt_gettime(&end), 0);
	}

	igt_debug("Timeout waiting for connection status %d on connector %d\n", drm_mode,
		  connector_id);

	return false;
}

/**
 * igt_get_connected_connectors:
 *
 * @drm_fd: DRM file descriptor
 * @connector_ids: Out pointer for the array of connector ids connected. The function
 * will allocate and store the pointer in this out pointer. The caller is
 * responsible to free the inner pointer using free(*connector_ids).
 *
 * This will probe all the connectors and return the list of all
 * connected connectors.
 *
 * Asserts if:
 * - drm_fd is invalid
 * - drmModeGetResources fails
 * - reallocarray fails
 * Returns: The number of connectors in @connector_ids
 */
int igt_get_connected_connectors(int drm_fd, uint32_t **connector_ids)
{
	uint32_t *tmp_alloc;
	int connected_count = 0;
	double timeout = igt_default_display_detect_timeout();
	drmModeResPtr resources;

	igt_assert_fd(drm_fd);
	*connector_ids = NULL;

	resources = drmModeGetResources(drm_fd);
	igt_assert_f(resources, "drmModeGetResources failed");
	for (int j = 0; j < resources->count_connectors; j++) {
		int status = DRM_MODE_UNKNOWNCONNECTION;

		drmModeConnectorPtr connector = igt_wait_for_connector(drm_fd,
								       resources->connectors[j],
								       timeout);

		if (connector)
			status = connector->connection;
		drmModeFreeConnector(connector);

		if (status == DRM_MODE_CONNECTED) {
			tmp_alloc = reallocarray(*connector_ids,
						 connected_count + 1,
						 sizeof(**connector_ids));
			if (!tmp_alloc) {
				free(*connector_ids);
				drmModeFreeResources(resources);
				igt_assert_f(false, "Failed to allocate connector_ids list");
			}

			*connector_ids = tmp_alloc;
			(*connector_ids)[connected_count] = resources->connectors[j];
			connected_count++;
		}
	}
	drmModeFreeResources(resources);

	return connected_count;
}

/**
 * igt_get_connector_from_name:
 * @drm_fd: DRM file descriptor
 * @port_name: Port name to search
 *
 * Returns: The connector if found, NULL otherwise. The pointer must
 * be freed with drmModeFreeConnector()
 */
drmModeConnectorPtr igt_get_connector_from_name(int drm_fd, const char *port_name)
{
	double timeout = igt_default_display_detect_timeout();
	drmModeResPtr res = drmModeGetResources(drm_fd);
	int i;

	if (!res)
		return NULL;

	for (i = 0; i < res->count_connectors; i++) {
		char name[IGT_KMS_CONNECTOR_NAME_SIZE];
		int len;

		drmModeConnectorPtr connector = igt_wait_for_connector(drm_fd, res->connectors[i],
								       timeout);

		if (connector) {
			/* We have to generate the connector name on our own */
			len = snprintf(name, sizeof(name), "%s-%u",
				       kmstest_connector_type_str(connector->connector_type),
				       connector->connector_type_id);
			name[len] = 0;

			if (strcmp(port_name, name) == 0) {
				drmModeFreeResources(res);

				return connector;
			}

			drmModeFreeConnector(connector);
		}
	}

	drmModeFreeResources(res);

	return NULL;
}

/**
 * igt_get_connector_id_from_name:
 * @drm_fd: DRM file descriptor
 * @port_name: Port name to find in the connector
 *
 * Returns: The connector id if the port is found, 0 if the port is not found
 */
uint32_t igt_get_connector_id_from_name(int drm_fd, const char *port_name)
{
	drmModeConnectorPtr connector = igt_get_connector_from_name(drm_fd, port_name);

	if (connector) {
		uint32_t connector_id = connector->connector_id;

		drmModeFreeConnector(connector);

		return connector_id;
	}

	return 0;
}

/**
 * igt_get_connector_id_from_mst_path:
 * @drm_fd: DRM file descriptor
 * @mst_path: MST path to find in the connector
 *
 * Returns: 0 when no connector is found with the correct mst path
 */
uint32_t igt_get_connector_id_from_mst_path(int drm_fd, const void *mst_path)
{
	drmModeResPtr res = drmModeGetResources(drm_fd);
	int i;

	if (!res)
		return 0;

	for (i = 0; i < res->count_connectors; i++) {
		uint32_t connector_id = res->connectors[i];

		drmModePropertyBlobPtr path_blob = kmstest_get_path_blob(drm_fd, connector_id);

		if (path_blob) {
			if (memcmp(path_blob->data, mst_path, path_blob->length) == 0) {
				drmModeFreePropertyBlob(path_blob);
				drmModeFreeResources(res);
				return connector_id;
			}

			drmModeFreePropertyBlob(path_blob);
		}
	}

	drmModeFreeResources(res);

	return 0;
}

/**
 * array_contains() - Search an element in the array
 *
 * @array: Pointer to the array to search into
 * @array_len: Length of the array
 * @value: Value to search in the array
 *
 * Returns true if @array contains @value
 */
static bool array_contains(const uint32_t *array, int array_len, uint32_t value)
{
	igt_assert(array_len == 0 || array);

	for (int i = 0; i < array_len; i++)
		if (array[i] == value)
			return true;

	return false;
}

/**
 * get_array_diff() - Compute and return the set difference between two arrays
 *
 * @array_a: Pointer to the first array to compare
 * @array_a_len: Length of the first array
 * @array_b: Pointer to the second array to compare
 * @array_b_len: Length of the second array
 * @diff: Out pointer returning a array of items in @array_a but not
 *        in @array_b. Can be NULL to only count the elements.
 *
 * Returns the number of element which are in @array_a but not in @array_b.
 */
int
get_array_diff(const uint32_t *array_a, int array_a_len, const uint32_t *array_b, int array_b_len,
	       uint32_t **diff)
{
	int diff_len = 0;

	igt_assert(array_a_len == 0 || array_a);
	igt_assert(array_b_len == 0 || array_b);

	if (diff)
		*diff = NULL;

	for (int i = 0; i < array_a_len; i++) {
		if (!array_contains(array_b, array_b_len, array_a[i])) {
			if (diff) {
				*diff = reallocarray(*diff, diff_len + 1, sizeof(**diff));
				igt_assert(*diff);
				(*diff)[diff_len] = array_a[i];
			}

			diff_len++;
		}
	}

	return diff_len;
}

/**
 * kms_wait_for_new_connectors() - Wait for new connector to appear
 *
 * @newly_connected: Out pointer returning the array of connectors currently
 *                   found connected. If @newly_connected is not null, the
 *                   content will be freed and replaced by a new allocation.
 * @already_connected: Input pointer to array of connectors previously
 *                     connected
 * @already_connected_count: Length of @already_connected
 * @drm_fd: DRM file descriptor
 *
 * Assert if:
 * - @drm_fd is invalid
 * - igt_get_time fails
 * Returns: the number of connectors not in @already_connected and currently
 * found connected.
 */
int kms_wait_for_new_connectors(uint32_t **newly_connected,
				const uint32_t *already_connected,
				int already_connected_count, int drm_fd)
{
	int newly_connected_count;
	struct timespec start, end;

	igt_assert(newly_connected);
	igt_assert_fd(drm_fd);

	igt_assert_eq(igt_gettime(&start), 0);
	do {
		if (*newly_connected)
			free(*newly_connected);
		newly_connected_count = igt_get_connected_connectors(drm_fd, newly_connected);
		igt_assert_eq(igt_gettime(&end), 0);
	} while (!get_array_diff(*newly_connected, newly_connected_count,
			       already_connected, already_connected_count,
			       NULL) &&
		 igt_time_elapsed(&start, &end) <= igt_default_display_detect_timeout());

	return newly_connected_count;
}

/**
 * igt_get_crtc_index_from_connector_id() - Get a pipe from the connector id
 * @drm_fd: drm file descriptor to which the connector belongs
 * @connector_id: connector to inspect
 *
 * Assert if:
 * - CRTC_ID property is not found
 * - CRTC_ID property don't have the DRM_MODE_PROP_OBJECT flag
 * Returns: 0 if the connector is not connected to any pipe
 */
int igt_get_crtc_index_from_connector_id(int drm_fd, int connector_id)
{
	drmModeObjectPropertiesPtr proplist;
	drmModePropertyPtr crtc_id_prop;
	drmModePropertyPtr prop;
	int crtc_index;
	uint32_t crtc_id;
	int i;

	proplist = drmModeObjectGetProperties(drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
	crtc_id_prop = NULL;

	for (i = 0; i < proplist->count_props; i++) {
		prop = drmModeGetProperty(drm_fd, proplist->props[i]);

		if (strcmp(prop->name, "CRTC_ID") == 0) {
			crtc_id_prop = prop;
			break;
		}
		drmModeFreeProperty(prop);
	}

	igt_assert(crtc_id_prop);
	igt_assert((crtc_id_prop->flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_OBJECT);
	crtc_id = proplist->prop_values[i];
	drmModeFreeProperty(crtc_id_prop);
	drmModeFreeObjectProperties(proplist);
	if (crtc_id != 0) {
		crtc_index = kmstest_get_crtc_index_from_id(drm_fd, crtc_id);
		return crtc_index;
	}
	return 0;
}

/**
 * igt_get_crtc_for_output:
 * @display: display to fetch the CRTC
 * @output: output to use
 *
 * Get a valid CRTC for a specific output. The return value is the first valid CRTC for a
 * specific output.
 */
igt_crtc_t *igt_get_crtc_for_output(igt_display_t *display,
				    igt_output_t *output)
{
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc)
		if (igt_crtc_connector_valid(crtc, output))
			return crtc;

	return NULL;
}
