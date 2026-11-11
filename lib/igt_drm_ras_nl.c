// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <errno.h>
#include <stdint.h>

#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>

#include "igt_core.h"
#include "igt_drm_ras_nl.h"

void igt_drm_ras_nl_cleanup(struct igt_drm_ras_nl_context *ctx)
{
	if (!ctx || !ctx->sock)
		return;

	nl_close(ctx->sock);
	nl_socket_free(ctx->sock);
	ctx->sock = NULL;
	ctx->family_id = -1;

	igt_debug("Cleaned up netlink socket.\n");
}

int igt_drm_ras_nl_init(struct igt_drm_ras_nl_context *ctx)
{
	ctx->sock = nl_socket_alloc();
	if (!ctx->sock)
		return -ENOMEM;

	if (genl_connect(ctx->sock) < 0) {
		igt_warn("Failed to connect to Generic Netlink\n");
		igt_drm_ras_nl_cleanup(ctx);
		return -EIO;
	}

	ctx->family_id = genl_ctrl_resolve(ctx->sock, DRM_RAS_FAMILY_NAME);
	if (ctx->family_id < 0) {
		igt_warn("Failed to resolve Netlink family '%s': %s\n",
			 DRM_RAS_FAMILY_NAME, nl_geterror(ctx->family_id));
		igt_drm_ras_nl_cleanup(ctx);
		return -EIO;
	}

	igt_debug("Resolved Generic Netlink family '%s' with id %d.\n",
		  DRM_RAS_FAMILY_NAME, ctx->family_id);
	return 0;
}
