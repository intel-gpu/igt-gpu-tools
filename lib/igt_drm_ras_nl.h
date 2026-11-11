/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef IGT_DRM_RAS_NL_H
#define IGT_DRM_RAS_NL_H

#include <drm-uapi/drm_ras.h>

struct igt_drm_ras_nl_context {
	struct nl_sock *sock;
	int family_id;
};

void igt_drm_ras_nl_cleanup(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_init(struct igt_drm_ras_nl_context *ctx);

#endif /* IGT_DRM_RAS_NL_H */
