/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef IGT_DRM_RAS_NL_H
#define IGT_DRM_RAS_NL_H

#include <drm-uapi/drm_ras.h>
#include <drm-uapi/xe_drm.h>

/**
 * struct igt_drm_ras_nl_context - DRM RAS generic netlink context
 *
 * Holds the state for a DRM RAS generic netlink request/response exchange.
 */
struct igt_drm_ras_nl_context {
	/** @sock: Netlink socket used for the exchange */
	struct nl_sock *sock;

	/** @node_id: DRM RAS node ID to query the error counters */
	uint32_t node_id;

	/** @error_id: RAS error ID to query the error counter */
	uint32_t error_id;

	/** @error_value: Error counter value returned by the kernel */
	uint32_t error_value;

	/** @last_nl_error: Last netlink error reported by the kernel */
	int last_nl_error;

	/** @cmd_done: Set when the command exchange has completed */
	bool cmd_done;

	/** @reply_received: Set when the command reply has been received */
	bool reply_received;

	/** @ack_received: Set when the command ACK has been received */
	bool ack_received;

	/** @response_required: Set when a reply is expected before @cmd_done */
	bool response_required;

	/** @family_id: Generic netlink family ID */
	int family_id;

	/** @error_threshold: Error threshold value to query/set */
	uint32_t error_threshold;

	/** @event_node_id: RAS node ID reported by an error-report event */
	uint32_t event_node_id;

	/** @event_error_id: RAS error ID reported by an error-report event */
	uint32_t event_error_id;

	/** @event_error_value: Error counter value reported by an error-report event */
	uint32_t event_error_value;

	/** @event_received: Set when a matching error-report event has been received */
	bool event_received;

	/** @subscribed_grp_id: Multicast group ID currently subscribed to, or -1 if none */
	int subscribed_grp_id;
};

void igt_drm_ras_nl_cleanup(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_init(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_get_error_counter(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_get_error_threshold(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_set_error_threshold(struct igt_drm_ras_nl_context *ctx);
int igt_drm_ras_nl_subscribe_error_report(struct igt_drm_ras_nl_context *ctx,
					  const char *notify_group_name);
int igt_drm_ras_nl_wait_for_error_report_event(struct igt_drm_ras_nl_context *ctx, int timeout_ms);

#endif /* IGT_DRM_RAS_NL_H */
