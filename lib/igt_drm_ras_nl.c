// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>

#include <netlink/errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>

#include "igt_core.h"
#include "igt_drm_ras_nl.h"

#define DRM_RAS_NL_OVERALL_TIMEOUT_MS 60000
#define DRM_RAS_NL_MSG_TIMEOUT_MS 10000

/* Normalize netlink error codes to standard errnos */
static int normalize_nl_error(int ret)
{
	if (ret >= 0)
		return ret;

	switch (-ret) {
	case NLE_INVAL:
		return -EINVAL;
	case NLE_NOMEM:
		return -ENOMEM;
	case NLE_NOATTR:
	case NLE_MISSING_ATTR:
		return -ENODATA;
	case NLE_OPNOTSUPP:
		return -EOPNOTSUPP;
	case NLE_EXIST:
		return -EEXIST;
	case NLE_NOADDR:
		return -EADDRNOTAVAIL;
	case NLE_PERM:
		return -EPERM;
	case NLE_NOACCESS:
		return -EACCES;
	case NLE_NODEV:
		return -ENODEV;
	case NLE_BUSY:
		return -EBUSY;
	case NLE_AGAIN:
		return -EAGAIN;
	default:
		igt_debug("Unhandled netlink error code: %d\n", -ret);
		return -EIO;
	}
}

static int ras_command_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX + 1];
	struct igt_drm_ras_nl_context *ctx = arg;
	struct genlmsghdr *gnlh;
	struct nlmsghdr *nlh;
	int ret;

	nlh = nlmsg_hdr(msg);
	gnlh = nlmsg_data(nlh);

	ret = genlmsg_parse(nlh, 0, attrs,
			    DRM_RAS_A_ERROR_COUNTER_ATTRS_MAX, NULL);
	if (ret < 0)
		return NL_SKIP;

	switch (gnlh->cmd) {
	case DRM_RAS_CMD_SET_ERROR_THRESHOLD:
		break;
	case DRM_RAS_CMD_GET_ERROR_COUNTER: {
		if (!attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE])
			return NL_SKIP;

		ctx->error_value = nla_get_u32(attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE]);
		break;
	}
	case DRM_RAS_CMD_GET_ERROR_THRESHOLD: {
		if (!attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_THRESHOLD])
			return NL_SKIP;

		ctx->error_threshold =
			nla_get_u32(attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_THRESHOLD]);
		break;
	}
	default:
		return NL_SKIP;
	}

	ctx->reply_received = true;
	if (ctx->ack_received)
		ctx->cmd_done = true;

	return NL_OK;
}

static int ras_error_cb(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
	struct igt_drm_ras_nl_context *ctx = arg;

	/* err->error is a raw kernel errno, not a libnl NLE_* code */
	ctx->last_nl_error = err->error;
	ctx->cmd_done = true;

	return NL_STOP;
}

static int ras_ack_cb(struct nl_msg *msg, void *arg)
{
	struct igt_drm_ras_nl_context *ctx = arg;

	ctx->ack_received = true;
	if (!ctx->response_required || ctx->reply_received)
		ctx->cmd_done = true;

	return NL_STOP;
}

static int ras_finish_cb(struct nl_msg *msg, void *arg)
{
	struct igt_drm_ras_nl_context *ctx = arg;

	ctx->cmd_done = true;

	return NL_STOP;
}

static int register_callbacks(struct nl_cb *cb, struct igt_drm_ras_nl_context *ctx)
{
	int ret;

	ret = nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, ras_command_cb, ctx);
	if (ret < 0) {
		igt_warn("Failed to set NL_CB_VALID callback (ret=%d)\n", ret);
		return ret;
	}

	ret = nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ras_ack_cb, ctx);
	if (ret < 0) {
		igt_warn("Failed to set NL_CB_ACK callback (ret=%d)\n", ret);
		return ret;
	}

	ret = nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, ras_finish_cb, ctx);
	if (ret < 0) {
		igt_warn("Failed to set NL_CB_FINISH callback (ret=%d)\n", ret);
		return ret;
	}

	ret = nl_cb_err(cb, NL_CB_CUSTOM, ras_error_cb, ctx);
	if (ret < 0) {
		igt_warn("Failed to set NL_CB_ERR callback (ret=%d)\n", ret);
		return ret;
	}

	return 0;
}

static int send_and_recv_nl_msg(struct igt_drm_ras_nl_context *ctx,
				struct nl_cb *cb,
				struct nl_msg *msg)
{
	struct pollfd pfd = {
		.fd = nl_socket_get_fd(ctx->sock),
		.events = POLLIN,
	};
	const uint64_t timeout_ns = (uint64_t)DRM_RAS_NL_OVERALL_TIMEOUT_MS * 1000000;
	const uint64_t msg_timeout_ns =
		(uint64_t)DRM_RAS_NL_MSG_TIMEOUT_MS * 1000000;
	struct timespec start, message_start;
	int ret, err_ret = 0;

	ret = nl_send_auto(ctx->sock, msg);
	nlmsg_free(msg);
	if (ret < 0) {
		err_ret = normalize_nl_error(ret);
		goto process_error;
	}

	ret = igt_gettime(&start);
	if (ret < 0) {
		err_ret = -errno;
		goto process_error;
	}

	message_start = start;

	/* NL Reply and ACK arrive as separate responses */
	while (!ctx->cmd_done) {
		uint64_t msg_elapsed_ns = igt_nsec_elapsed(&message_start);
		uint64_t elapsed_ns = igt_nsec_elapsed(&start);
		uint64_t remaining_ns;
		int timeout_ms;

		if (elapsed_ns >= timeout_ns || msg_elapsed_ns >= msg_timeout_ns) {
			err_ret = -ETIMEDOUT;
			goto process_error;
		}

		remaining_ns = timeout_ns - elapsed_ns;
		if (msg_timeout_ns - msg_elapsed_ns < remaining_ns)
			remaining_ns = msg_timeout_ns - msg_elapsed_ns;
		timeout_ms = (remaining_ns + 999999) / 1000000;
		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0 && errno == EINTR)
			continue;

		/* poll()/errno results are already standard errnos, not libnl codes */
		if (ret == 0) {
			err_ret = -ETIMEDOUT;
			goto process_error;
		}

		if (ret < 0) {
			err_ret = -errno;
			goto process_error;
		}

		if (!(pfd.revents & POLLIN)) {
			igt_debug("Received unexpected poll revents: %d\n", pfd.revents);
			err_ret = -EIO;
			goto process_error;
		}

		ret = nl_recvmsgs(ctx->sock, cb);
		if (ret < 0) {
			err_ret = normalize_nl_error(ret);
			goto process_error;
		}

		ret = igt_gettime(&message_start);
		if (ret < 0) {
			err_ret = -errno;
			goto process_error;
		}
	}

process_error:
	nl_cb_put(cb);
	return err_ret;
}

static int send_command(struct igt_drm_ras_nl_context *ctx, uint8_t cmd)
{
	struct nl_msg *msg;
	struct nl_cb *cb;
	void *msg_head;
	int ret;

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	ctx->last_nl_error = 0;
	ctx->cmd_done = false;
	ctx->reply_received = false;
	ctx->ack_received = false;
	ctx->response_required = false;

	msg_head = genlmsg_put(msg,
			       NL_AUTO_PORT,
			       NL_AUTO_SEQ,
			       ctx->family_id,
			       0,
			       NLM_F_REQUEST | NLM_F_ACK,
			       cmd,
			       DRM_RAS_FAMILY_VERSION);
	if (!msg_head) {
		nlmsg_free(msg);
		return -ENOMEM;
	}

	switch (cmd) {
	case DRM_RAS_CMD_GET_ERROR_THRESHOLD:
	case DRM_RAS_CMD_GET_ERROR_COUNTER:
		ctx->response_required = true;
	case DRM_RAS_CMD_SET_ERROR_THRESHOLD:
		ret = nla_put_u32(msg,
				  DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID,
				  ctx->node_id);
		if (ret < 0) {
			nlmsg_free(msg);
			return normalize_nl_error(ret);
		}

		ret = nla_put_u32(msg,
				  DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID,
				  ctx->error_id);
		if (ret < 0) {
			nlmsg_free(msg);
			return normalize_nl_error(ret);
		}

		if (cmd == DRM_RAS_CMD_SET_ERROR_THRESHOLD) {
			ret = nla_put_u32(msg,
					  DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_THRESHOLD,
					  ctx->error_threshold);
			if (ret < 0) {
				nlmsg_free(msg);
				return normalize_nl_error(ret);
			}
		}
		break;
	default:
		nlmsg_free(msg);
		return -EOPNOTSUPP;
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);
	if (!cb) {
		nlmsg_free(msg);
		return -ENOMEM;
	}

	ret = register_callbacks(cb, ctx);
	if (ret < 0) {
		nl_cb_put(cb);
		nlmsg_free(msg);
		return normalize_nl_error(ret);
	}

	return send_and_recv_nl_msg(ctx, cb, msg);
}

static void reset_drm_ras_nl_context(struct igt_drm_ras_nl_context *ctx)
{
	ctx->sock = NULL;
	ctx->node_id = UINT32_MAX;
	ctx->error_id = DRM_XE_RAS_ERR_COMP_MAX;
	ctx->error_value = 0;
	ctx->last_nl_error = 0;
	ctx->cmd_done = false;
	ctx->reply_received = false;
	ctx->ack_received = false;
	ctx->response_required = false;
	ctx->error_threshold = 0;
	ctx->family_id = -1;
}

void igt_drm_ras_nl_cleanup(struct igt_drm_ras_nl_context *ctx)
{
	if (!ctx || !ctx->sock)
		return;

	nl_close(ctx->sock);
	nl_socket_free(ctx->sock);

	reset_drm_ras_nl_context(ctx);

	igt_debug("Cleaned up netlink socket.\n");
}

int igt_drm_ras_nl_init(struct igt_drm_ras_nl_context *ctx)
{
	if (!ctx)
		return -EINVAL;

	reset_drm_ras_nl_context(ctx);

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

static int validate_inputs(struct igt_drm_ras_nl_context *ctx, uint8_t cmd)
{
	if (!ctx || !ctx->sock || ctx->family_id < 0) {
		igt_warn("Invalid DRM RAS netlink context\n");
		return -EINVAL;
	}

	if (ctx->node_id == UINT32_MAX ||
	    ctx->error_id == DRM_XE_RAS_ERR_COMP_MAX ||
	    ctx->error_id == 0) {
		igt_warn("Invalid node_id (%u) or error_id (%u) provided\n",
			 ctx->node_id, ctx->error_id);
		return -EINVAL;
	}

	if (cmd == DRM_RAS_CMD_SET_ERROR_THRESHOLD &&
	    (ctx->error_threshold < 1 || ctx->error_threshold > 16)) {
		igt_warn("Invalid error_threshold (%u) provided. "
			 "error_threshold should be >= 1 and <= 16.\n",
			 ctx->error_threshold);
		return -EINVAL;
	}

	return 0;
}

int igt_drm_ras_nl_get_error_counter(struct igt_drm_ras_nl_context *ctx)
{
	int ret;

	ret = validate_inputs(ctx, DRM_RAS_CMD_GET_ERROR_COUNTER);
	if (ret < 0)
		return ret;

	ctx->error_value = UINT32_MAX;

	ret = send_command(ctx, DRM_RAS_CMD_GET_ERROR_COUNTER);
	if (ret < 0)
		return ret;

	if (ctx->error_value == UINT32_MAX) {
		igt_warn("No valid error counter reply: node_id=%u error_id=%u kernel_error=%d\n",
			 ctx->node_id, ctx->error_id, ctx->last_nl_error);

		return ctx->last_nl_error ? ctx->last_nl_error : -ENODATA;
	}

	igt_debug("Retrieved error counter: node_id=%u error_id=%u value=%u\n",
		  ctx->node_id, ctx->error_id, ctx->error_value);

	return 0;
}

int igt_drm_ras_nl_get_error_threshold(struct igt_drm_ras_nl_context *ctx)
{
	int ret;

	ret = validate_inputs(ctx, DRM_RAS_CMD_GET_ERROR_THRESHOLD);
	if (ret < 0)
		return ret;

	ctx->error_threshold = UINT32_MAX;

	ret = send_command(ctx, DRM_RAS_CMD_GET_ERROR_THRESHOLD);
	if (ret < 0)
		return ret;

	if (ctx->error_threshold == UINT32_MAX) {
		igt_warn("No valid error threshold reply: node_id=%u error_id=%u kernel_error=%d\n",
			 ctx->node_id, ctx->error_id, ctx->last_nl_error);

		return ctx->last_nl_error ? ctx->last_nl_error : -ENODATA;
	}

	igt_debug("Retrieved error threshold: node_id=%u error_id=%u threshold=%u\n",
		  ctx->node_id, ctx->error_id, ctx->error_threshold);

	return 0;
}

int igt_drm_ras_nl_set_error_threshold(struct igt_drm_ras_nl_context *ctx)
{
	int ret;

	ret = validate_inputs(ctx, DRM_RAS_CMD_SET_ERROR_THRESHOLD);
	if (ret < 0)
		return ret;

	ret = send_command(ctx, DRM_RAS_CMD_SET_ERROR_THRESHOLD);
	if (ret < 0)
		return ret;

	igt_debug("Set error threshold: node_id=%u error_id=%u threshold=%u\n",
		  ctx->node_id, ctx->error_id, ctx->error_threshold);

	return 0;
}
