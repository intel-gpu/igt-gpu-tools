// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "igt.h"
#include "igt_sysfs.h"
#include "intel_pat.h"
#include "xe_eudebug.h"
#include "xe_ioctl.h"
#include "xe/xe_query.h"

struct debugger_fd_entry {
	int fd;
	struct igt_list_head link;
};

static IGT_LIST_HEAD(active_debugger_fds);
static pthread_mutex_t active_debugger_fds_lock = PTHREAD_MUTEX_INITIALIZER;

static void register_debugger_fd(int fd)
{
	struct debugger_fd_entry *entry;

	entry = calloc(1, sizeof(*entry));
	igt_assert(entry);
	entry->fd = fd;
	IGT_INIT_LIST_HEAD(&entry->link);

	pthread_mutex_lock(&active_debugger_fds_lock);
	igt_list_add(&entry->link, &active_debugger_fds);
	pthread_mutex_unlock(&active_debugger_fds_lock);
}

static void unregister_debugger_fd(int fd)
{
	struct debugger_fd_entry *entry, *tmp;

	pthread_mutex_lock(&active_debugger_fds_lock);
	igt_list_for_each_entry_safe(entry, tmp, &active_debugger_fds, link) {
		if (entry->fd == fd) {
			igt_list_del(&entry->link);
			free(entry);
			break;
		}
	}
	pthread_mutex_unlock(&active_debugger_fds_lock);
}

static void close_all_debugger_fds(void)
{
	struct debugger_fd_entry *entry, *tmp;

	pthread_mutex_lock(&active_debugger_fds_lock);
	igt_list_for_each_entry_safe(entry, tmp, &active_debugger_fds, link) {
		igt_debug("closing leftover debugger fd %d\n", entry->fd);
		close(entry->fd);
		igt_list_del(&entry->link);
		free(entry);
	}
	pthread_mutex_unlock(&active_debugger_fds_lock);
}

struct event_trigger {
	xe_eudebug_trigger_fn fn;
	int type;
	struct igt_list_head link;
};

struct seqno_list_entry {
	struct igt_list_head link;
	uint64_t seqno;
};

struct match_dto {
	struct drm_xe_eudebug_event *target;
	struct igt_list_head *seqno_list;
	uint64_t client_handle;
	uint32_t filter;

	/* store latest 'EVENT_VM_BIND' seqno */
	uint64_t *bind_seqno;
	/* latest vm_bind_op seqno matching bind_seqno */
	uint64_t *bind_op_seqno;
};

#define TOKEN_NONE  0
#define CLIENT_PID  1
#define CLIENT_RUN  2
#define CLIENT_FINI 3
#define CLIENT_STOP 4
#define CLIENT_STAGE 5
#define DEBUGGER_STAGE 6

static const char *token_to_str(uint64_t token)
{
	static const char * const s[] = {
		"none",
		"client pid",
		"client run",
		"client fini",
		"client stop",
		"client stage",
		"debugger stage",
	};

	igt_assert(token);

	if (token >= ARRAY_SIZE(s)) {
		igt_warn("token outside of bounds %ld\n", token);
		return "unknown";
	}

	return s[token];
}

static const char *type_to_str(unsigned int type)
{
	switch (type) {
	case DRM_XE_EUDEBUG_EVENT_NONE:
		return "none";
	case DRM_XE_EUDEBUG_EVENT_READ:
		return "read";
	case DRM_XE_EUDEBUG_EVENT_OPEN:
		return "client";
	case DRM_XE_EUDEBUG_EVENT_VM:
		return "vm";
	case DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE:
		return "exec_queue";
	case DRM_XE_EUDEBUG_EVENT_EU_ATTENTION:
		return "attention";
	case DRM_XE_EUDEBUG_EVENT_VM_BIND:
		return "vm_bind";
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP:
		return "vm_bind_op";
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE:
		return "vm_bind_ufence";
	case DRM_XE_EUDEBUG_EVENT_METADATA:
		return "metadata";
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA:
		return "vm_bind_op_metadata";
	case DRM_XE_EUDEBUG_EVENT_PAGEFAULT:
		return "pagefault";
	}

	return "UNKNOWN";
}

static const char *event_type_to_str(struct drm_xe_eudebug_event *e, char *buf)
{
	sprintf(buf, "%s(%d)", type_to_str(e->type), e->type);

	return buf;
}

static const char *flags_to_str(unsigned int flags)
{
	if (flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		if (flags & DRM_XE_EUDEBUG_EVENT_NEED_ACK)
			return "create|ack";
		else
			return "create";
	}
	if (flags & DRM_XE_EUDEBUG_EVENT_DESTROY)
		return "destroy";

	if (flags & DRM_XE_EUDEBUG_EVENT_STATE_CHANGE)
		return "state-change";

	igt_assert(!(flags & DRM_XE_EUDEBUG_EVENT_NEED_ACK));

	return "flags unknown";
}

static const char *event_members_to_str(struct drm_xe_eudebug_event *e, char *buf)
{
	switch (e->type) {
	case DRM_XE_EUDEBUG_EVENT_OPEN: {
		struct drm_xe_eudebug_event_client *ec = igt_container_of(e, ec, base);

		sprintf(buf, "handle=%llu", ec->client_handle);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM: {
		struct drm_xe_eudebug_event_vm *evm = igt_container_of(e, evm, base);

		sprintf(buf, "client_handle=%llu, handle=%llu",
			evm->client_handle, evm->vm_handle);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE: {
		struct drm_xe_eudebug_event_exec_queue *ee = igt_container_of(e, ee, base);

		sprintf(buf, "client_handle=%llu, vm_handle=%llu, "
			"exec_queue_handle=%llu, engine_class=%d, exec_queue_width=%d",
			ee->client_handle, ee->vm_handle,
			ee->exec_queue_handle, ee->engine_class, ee->width);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_EU_ATTENTION: {
		struct drm_xe_eudebug_event_eu_attention *ea = igt_container_of(e, ea, base);

		sprintf(buf, "client_handle=%llu, exec_queue_handle=%llu, "
			"lrc_handle=%llu, bitmask_size=%d",
			ea->client_handle, ea->exec_queue_handle,
			ea->lrc_handle, ea->bitmask_size);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND: {
		struct drm_xe_eudebug_event_vm_bind *evmb = igt_container_of(e, evmb, base);

		sprintf(buf, "client_handle=%llu, vm_handle=%llu, flags=0x%x, num_binds=%u",
			evmb->client_handle, evmb->vm_handle, evmb->flags, evmb->num_binds);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP: {
		struct drm_xe_eudebug_event_vm_bind_op *op = igt_container_of(e, op, base);

		sprintf(buf, "vm_bind_ref_seqno=%lld, addr=%016llx, range=%llu num_extensions=%llu",
			op->vm_bind_ref_seqno, op->addr, op->range, op->num_extensions);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE: {
		struct drm_xe_eudebug_event_vm_bind_ufence *f = igt_container_of(e, f, base);

		sprintf(buf, "vm_bind_ref_seqno=%lld", f->vm_bind_ref_seqno);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_METADATA: {
		struct drm_xe_eudebug_event_metadata *em = igt_container_of(e, em, base);

		sprintf(buf, "client_handle=%llu, metadata_handle=%llu, type=%llu, len=%llu",
			em->client_handle, em->metadata_handle, em->type, em->len);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA: {
		struct drm_xe_eudebug_event_vm_bind_op_metadata *op = igt_container_of(e, op, base);

		sprintf(buf, "vm_bind_op_ref_seqno=%lld, metadata_handle=%llu, metadata_cookie=%llu",
			op->vm_bind_op_ref_seqno, op->metadata_handle, op->metadata_cookie);
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_PAGEFAULT: {
		struct drm_xe_eudebug_event_pagefault *pf = igt_container_of(e, pf, base);

		sprintf(buf, "client_handle=%llu, exec_queue_handle=%llu, "
			"lrc_handle=%llu, bitmask_size=%d, pagefault_address=0x%llx",
			pf->client_handle, pf->exec_queue_handle, pf->lrc_handle,
			pf->bitmask_size, pf->pagefault_address);
		break;
	}
	default:
		strcpy(buf, "<...>");
	}

	return buf;
}

/**
 * xe_eudebug_event_to_str:
 * @e: pointer to event
 * @buf: target to write string representation of @e
 * @len: size of target buffer @buf
 *
 * Creates string representation for given event.
 *
 * Returns: the written input buffer pointed by @buf.
 */
const char *xe_eudebug_event_to_str(struct drm_xe_eudebug_event *e, char *buf, size_t len)
{
	char a[256];
	char b[256];

	igt_assert(e);
	igt_assert(buf);

	snprintf(buf, len, "(%llu) %15s:%s: %s",
		 e->seqno,
		 event_type_to_str(e, a),
		 flags_to_str(e->flags),
		 event_members_to_str(e, b));

	return buf;
}

static void catch_child_failure(void)
{
	pid_t pid;
	int status;

	pid = waitpid(-1, &status, WNOHANG);

	if (pid == 0 || pid == -1)
		return;

	if (!WIFEXITED(status))
		return;

	igt_assert_f(WEXITSTATUS(status) == 0, "Client failed!\n");
}

static int safe_pipe_read(int pipe[2], void *buf, int nbytes, int timeout_ms)
{
	int ret;
	int t = 0;
	struct pollfd r = {
		.fd = pipe[0],
		.events = POLLIN,
		.revents = 0
	};

	/* When child fails we may get stuck forever. Check whether
	 * the child process ended with an error.
	 */
	do {
		const int interval_ms = 1000;

		ret = poll(&r, 1, interval_ms);
		if (!ret) {
			catch_child_failure();
			t += interval_ms;
		} else if (ret == -1) {
			if (errno == EINTR) {
				ret = 0;
				continue;
			}
			return -errno;
		}

		if (ret == 1) {
			if (r.revents == POLLIN)
				return read(pipe[0], buf, nbytes);

			if (r.revents & ~POLLIN) {
				igt_debug("pipe read failed: %s%s (0x%x)\n",
					  r.revents & POLLHUP ? "pipe closed" : "",
					  r.revents & POLLERR ? "poll error" : "",
					  r.revents);
				return -EIO;
			}
		}
	} while (!ret && t < timeout_ms);

	return -ETIMEDOUT;
}

static void pipe_signal(int pipe[2], uint64_t token)
{
	/* Skip signaling if pipe was already closed */
	if (pipe[1] >= 0)
		igt_assert(write(pipe[1], &token, sizeof(token)) == sizeof(token));
}

static void pipe_close(int pipe[2])
{
	if (pipe[0] >= 0) {
		igt_assert_eq(close(pipe[0]), 0);
		pipe[0] = -1;
	}

	if (pipe[1] >= 0) {
		igt_assert_eq(close(pipe[1]), 0);
		pipe[1] = -1;
	}
}

#define DEAD_CLIENT 0xccccdead

static uint64_t __wait_token(int pipe[2], const uint64_t token, int timeout_ms)
{
	uint64_t in;
	int ret;

	ret = safe_pipe_read(pipe, &in, sizeof(in), timeout_ms);
	if (ret < 0) {
		igt_debug("safe_pipe_read failed with error: %d waiting for token '%s:(%" PRId64 ")'\n",
			  ret, token_to_str(token), token);
		return DEAD_CLIENT;
	} else if (ret == 0) {
		igt_debug("safe_pipe_read failed: EOF\n");
		return DEAD_CLIENT;
	}

	igt_assert_eq(in, token);

	ret = safe_pipe_read(pipe, &in, sizeof(in), timeout_ms);
	if (ret < 0) {
		igt_debug("safe_pipe_read failed with error: %d waiting for token '%s:(%" PRId64 ")'\n",
			  ret, token_to_str(token), token);
		return DEAD_CLIENT;
	} else if (ret == 0) {
		igt_debug("safe_pipe_read failed: EOF\n");
		return DEAD_CLIENT;
	}

	return in;
}

static uint64_t client_wait_token(struct xe_eudebug_client *c, const uint64_t token)
{
	uint64_t ret = 0;

	igt_debug("client: %d waiting for token '%s'\n", getpid(), token_to_str(token));

	ret = __wait_token(c->p_in, token, c->timeout_ms);
	if (ret == DEAD_CLIENT)
		igt_assert(c->allow_dead_client);

	return ret;
}

static uint64_t wait_from_client(struct xe_eudebug_client *c, const uint64_t token)
{
	uint64_t ret = 0;

	igt_debug("debugger: %d waiting for token '%s'\n", getpid(), token_to_str(token));

	ret = __wait_token(c->p_out, token, c->timeout_ms);
	if (ret == DEAD_CLIENT)
		igt_assert(c->allow_dead_client);

	return ret;
}

static void token_signal(int pipe[2], const uint64_t token, const uint64_t value)
{
	igt_debug("%d signalling token '%s' with value '%ld'\n",
		  getpid(), token_to_str(token), value);

	pipe_signal(pipe, token);
	pipe_signal(pipe, value);
}

static void client_signal(struct xe_eudebug_client *c,
			  const uint64_t token,
			  const uint64_t value)
{
	token_signal(c->p_out, token, value);
}

static void event_log_write_to_fd(struct xe_eudebug_event_log *l, int fd)
{
	igt_assert_eq(write(fd, &l->head, sizeof(l->head)),
		      sizeof(l->head));

	igt_assert_eq(write(fd, l->log, l->head), l->head);
}

static void read_all(int fd, void *buf, size_t nbytes)
{
	ssize_t remaining_size = nbytes;
	ssize_t current_size = 0;
	ssize_t read_size = 0;

	do {
		read_size = read(fd, buf + current_size, remaining_size);
		igt_assert_f(read_size >= 0, "read failed: %s\n", strerror(errno));

		current_size += read_size;
		remaining_size -= read_size;
	} while (remaining_size > 0 && read_size > 0);

	igt_assert_eq(current_size, nbytes);
}

static void event_log_read_from_fd(struct xe_eudebug_event_log *l, int fd)
{
	read_all(fd, &l->head, sizeof(l->head));
	igt_assert_lt(l->head, l->max_size);

	read_all(fd, l->log, l->head);
}

typedef int (*cmp_fn_t)(struct drm_xe_eudebug_event *, void *);

static struct drm_xe_eudebug_event *
event_cmp(struct xe_eudebug_event_log *l,
	  struct drm_xe_eudebug_event *current,
	  cmp_fn_t match,
	  void *data)
{
	struct drm_xe_eudebug_event *e = current;

	xe_eudebug_for_each_event(e, l) {
		if (match(e, data))
			return e;
	}

	return NULL;
}

static int match_type_and_flags(struct drm_xe_eudebug_event *a, void *data)
{
	struct drm_xe_eudebug_event *b = data;

	if (a->type == b->type &&
	    a->flags == b->flags)
		return 1;

	return 0;
}

static int match_fields(struct drm_xe_eudebug_event *a, void *data)
{
	struct drm_xe_eudebug_event *b = data;
	int ret = 0;

	ret = match_type_and_flags(a, data);
	if (!ret)
		return ret;

	ret = 0;

	switch (a->type) {
	case DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE: {
		struct drm_xe_eudebug_event_exec_queue *ae = igt_container_of(a, ae, base);
		struct drm_xe_eudebug_event_exec_queue *be = igt_container_of(b, be, base);

		if (ae->engine_class == be->engine_class && ae->width == be->width)
			ret = 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND: {
		struct drm_xe_eudebug_event_vm_bind *ea = igt_container_of(a, ea, base);
		struct drm_xe_eudebug_event_vm_bind *eb = igt_container_of(b, eb, base);

		if (ea->num_binds == eb->num_binds)
			ret = 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP: {
		struct drm_xe_eudebug_event_vm_bind_op *ea = igt_container_of(a, ea, base);
		struct drm_xe_eudebug_event_vm_bind_op *eb = igt_container_of(b, eb, base);

		if (ea->addr == eb->addr && ea->range == eb->range &&
		    ea->num_extensions == eb->num_extensions)
			ret = 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA: {
		struct drm_xe_eudebug_event_vm_bind_op_metadata *ea = igt_container_of(a, ea, base);
		struct drm_xe_eudebug_event_vm_bind_op_metadata *eb = igt_container_of(b, eb, base);

		if (ea->metadata_handle == eb->metadata_handle &&
		    ea->metadata_cookie == eb->metadata_cookie)
			ret = 1;
		break;
	}

	default:
		ret = 1;
		break;
	}

	return ret;
}

static int match_client_handle(struct drm_xe_eudebug_event *e, void *data)
{
	struct match_dto *md = data;
	uint64_t *bind_seqno = md->bind_seqno;
	uint64_t *bind_op_seqno = md->bind_op_seqno;
	uint64_t h = md->client_handle;

	if (XE_EUDEBUG_EVENT_IS_FILTERED(e->type, md->filter))
		return 0;

	switch (e->type) {
	case DRM_XE_EUDEBUG_EVENT_OPEN: {
		struct drm_xe_eudebug_event_client *client = igt_container_of(e, client, base);

		if (client->client_handle == h)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM: {
		struct drm_xe_eudebug_event_vm *vm = igt_container_of(e, vm, base);

		if (vm->client_handle == h)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE: {
	struct drm_xe_eudebug_event_exec_queue *ee = igt_container_of(e, ee, base);

		if (ee->client_handle == h)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND: {
		struct drm_xe_eudebug_event_vm_bind *evmb = igt_container_of(e, evmb, base);

		if (evmb->client_handle == h) {
			*bind_seqno = evmb->base.seqno;
			return 1;
		}
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP: {
		struct drm_xe_eudebug_event_vm_bind_op *eo = igt_container_of(e, eo, base);

		if (eo->vm_bind_ref_seqno == *bind_seqno) {
			*bind_op_seqno = eo->base.seqno;
			return 1;
		}
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE: {
		struct drm_xe_eudebug_event_vm_bind_ufence *ef = igt_container_of(e, ef, base);

		if (ef->vm_bind_ref_seqno == *bind_seqno)
			return 1;

		break;
	}
	case DRM_XE_EUDEBUG_EVENT_METADATA: {
		struct drm_xe_eudebug_event_metadata *em = igt_container_of(e, em, base);

		if (em->client_handle == h)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA: {
		struct drm_xe_eudebug_event_vm_bind_op_metadata *eo = igt_container_of(e, eo, base);

		if (eo->vm_bind_op_ref_seqno == *bind_op_seqno)
			return 1;
		break;
	}
	default:
		break;
	}

	return 0;
}

static int match_opposite_resource(struct drm_xe_eudebug_event *e, void *data)
{
	struct drm_xe_eudebug_event *d = data;
	int ret;

	d->flags ^= DRM_XE_EUDEBUG_EVENT_CREATE | DRM_XE_EUDEBUG_EVENT_DESTROY;
	d->flags &= ~(DRM_XE_EUDEBUG_EVENT_NEED_ACK);
	ret = match_type_and_flags(e, data);
	d->flags ^= DRM_XE_EUDEBUG_EVENT_CREATE | DRM_XE_EUDEBUG_EVENT_DESTROY;

	if (!ret)
		return 0;

	switch (e->type) {
	case DRM_XE_EUDEBUG_EVENT_OPEN: {
		struct drm_xe_eudebug_event_client *client = igt_container_of(e, client, base);
		struct drm_xe_eudebug_event_client *filter = data;

		if (client->client_handle == filter->client_handle)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM: {
		struct drm_xe_eudebug_event_vm *vm = igt_container_of(e, vm, base);
		struct drm_xe_eudebug_event_vm *filter = data;

		if (vm->vm_handle == filter->vm_handle)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE: {
		struct drm_xe_eudebug_event_exec_queue *ee = igt_container_of(e, ee, base);
		struct drm_xe_eudebug_event_exec_queue *filter = data;

		if (ee->exec_queue_handle == filter->exec_queue_handle)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND: {
		struct drm_xe_eudebug_event_vm_bind *evmb = igt_container_of(e, evmb, base);
		struct drm_xe_eudebug_event_vm_bind *filter = data;

		if (evmb->vm_handle == filter->vm_handle &&
		    evmb->num_binds == filter->num_binds)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP: {
		struct drm_xe_eudebug_event_vm_bind_op *avmb = igt_container_of(e, avmb, base);
		struct drm_xe_eudebug_event_vm_bind_op *filter = data;

		if (avmb->addr == filter->addr &&
		    avmb->range == filter->range)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_METADATA: {
		struct drm_xe_eudebug_event_metadata *em = igt_container_of(e, em, base);
		struct drm_xe_eudebug_event_metadata *filter = data;

		if (em->metadata_handle == filter->metadata_handle)
			return 1;
		break;
	}
	case DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA: {
		struct drm_xe_eudebug_event_vm_bind_op_metadata *avmb = igt_container_of(e, avmb,
											 base);
		struct drm_xe_eudebug_event_vm_bind_op_metadata *filter = data;

		if (avmb->metadata_handle == filter->metadata_handle &&
		    avmb->metadata_cookie == filter->metadata_cookie)
			return 1;
		break;
	}

	default:
		break;
	}
	return 0;
}

static int match_full(struct drm_xe_eudebug_event *e, void *data)
{
	struct seqno_list_entry *sl;

	struct match_dto *md = (void *)data;
	int ret = 0;

	ret = match_client_handle(e, md);
	if (!ret)
		return 0;

	ret = match_fields(e, md->target);
	if (!ret)
		return 0;

	igt_list_for_each_entry(sl, md->seqno_list, link) {
		if (sl->seqno == e->seqno)
			return 0;
	}

	return 1;
}

static struct drm_xe_eudebug_event *
event_type_match(struct xe_eudebug_event_log *l,
		 struct drm_xe_eudebug_event *target,
		 struct drm_xe_eudebug_event *current)
{
	return event_cmp(l, current, match_type_and_flags, target);
}

static struct drm_xe_eudebug_event *
client_match(struct xe_eudebug_event_log *l,
	     uint64_t client_handle,
	     struct drm_xe_eudebug_event *current,
	     uint32_t filter,
	     uint64_t *bind_seqno,
	     uint64_t *bind_op_seqno)
{
	struct match_dto md = {
		.client_handle = client_handle,
		.filter = filter,
		.bind_seqno = bind_seqno,
		.bind_op_seqno = bind_op_seqno,
	};

	return event_cmp(l, current, match_client_handle, &md);
}

static struct drm_xe_eudebug_event *
opposite_event_match(struct xe_eudebug_event_log *l,
		     struct drm_xe_eudebug_event *target,
		     struct drm_xe_eudebug_event *current)
{
	return event_cmp(l, current, match_opposite_resource, target);
}

static struct drm_xe_eudebug_event *
event_match(struct xe_eudebug_event_log *l,
	    struct drm_xe_eudebug_event *target,
	    uint64_t client_handle,
	    struct igt_list_head *seqno_list,
	    uint64_t *bind_seqno,
	    uint64_t *bind_op_seqno)
{
	struct match_dto md = {
		.target = target,
		.client_handle = client_handle,
		.seqno_list = seqno_list,
		.bind_seqno = bind_seqno,
		.bind_op_seqno = bind_op_seqno,
	};

	return event_cmp(l, NULL, match_full, &md);
}

static void compare_client(struct xe_eudebug_event_log *log1, struct drm_xe_eudebug_event *ev1,
			   struct xe_eudebug_event_log *log2, struct drm_xe_eudebug_event *ev2,
			   uint32_t filter)
{
	struct drm_xe_eudebug_event_client *ev1_client = igt_container_of(ev1, ev1_client, base);
	struct drm_xe_eudebug_event_client *ev2_client = igt_container_of(ev2, ev2_client, base);
	uint64_t cbs = 0, dbs = 0, cbso = 0, dbso = 0;

	struct igt_list_head matched_seqno_list;
	struct drm_xe_eudebug_event *evptr1, *evptr2;
	struct seqno_list_entry *entry, *tmp;

	igt_assert(ev1_client);
	igt_assert(ev2_client);

	igt_debug("client: %llu -> %llu\n", ev1_client->client_handle, ev2_client->client_handle);

	evptr1 = NULL;
	evptr2 = NULL;
	IGT_INIT_LIST_HEAD(&matched_seqno_list);

	do {
		evptr1 = client_match(log1, ev1_client->client_handle, evptr1, filter, &cbs, &cbso);
		if (!evptr1)
			break;

		evptr2 = event_match(log2, evptr1, ev2_client->client_handle, &matched_seqno_list,
				     &dbs, &dbso);

		igt_assert_f(evptr2, "%s (%llu): no matching event type %u found for client %llu\n",
			     log1->name,
			     evptr1->seqno,
			     evptr1->type,
			     ev1_client->client_handle);

		igt_debug("comparing %s %llu vs %s %llu\n",
			  log1->name, evptr1->seqno, log2->name, evptr2->seqno);

		/*
		 * Store the seqno of the event that was matched above,
		 * inside 'matched_seqno_list', to avoid it getting matched
		 * by subsequent 'event_match' calls.
		 */
		entry = malloc(sizeof(*entry));
		entry->seqno = evptr2->seqno;
		igt_list_add(&entry->link, &matched_seqno_list);
	} while (evptr1);

	igt_list_for_each_entry_safe(entry, tmp, &matched_seqno_list, link)
		free(entry);
}

/**
 * xe_eudebug_event_log_find_seqno:
 * @l: event log pointer
 * @seqno: seqno of event to be found
 *
 * Finds the event with given seqno in the event log.
 *
 * Returns: pointer to the event with given seqno within @l or NULL seqno is
 * not present.
 */
struct drm_xe_eudebug_event *
xe_eudebug_event_log_find_seqno(struct xe_eudebug_event_log *l, uint64_t seqno)
{
	struct drm_xe_eudebug_event *e = NULL, *found = NULL;

	igt_assert(l);
	igt_assert_neq(seqno, 0);
	/*
	 * Try to catch if seqno is corrupted and prevent too long tests,
	 * as our post processing of events is not optimized.
	 */
	igt_assert_lt(seqno, 10 * 1000 * 1000);

	xe_eudebug_for_each_event(e, l) {
		if (e->seqno == seqno) {
			if (found) {
				igt_warn("Found multiple events with the same seqno %" PRIu64 "\n",
					 seqno);
				xe_eudebug_event_log_print(l, false);
				igt_assert(!found);
			}
			found = e;
		}
	}

	return found;
}

static void event_log_sort(struct xe_eudebug_event_log *l)
{
	struct xe_eudebug_event_log *tmp;
	struct drm_xe_eudebug_event *e = NULL;
	uint64_t first_seqno = UINT64_MAX;
	uint64_t last_seqno = 0;
	uint64_t events = 0, added = 0;
	uint64_t i;

	xe_eudebug_for_each_event(e, l) {
		if (e->seqno > last_seqno)
			last_seqno = e->seqno;

		if (e->seqno < first_seqno)
			first_seqno = e->seqno;

		events++;
	}

	if (!events)
		return;

	tmp = xe_eudebug_event_log_create("tmp", l->max_size);

	for (i = first_seqno; i <= last_seqno; i++) {
		e = xe_eudebug_event_log_find_seqno(l, i);
		if (e) {
			xe_eudebug_event_log_write(tmp, e);
			added++;
		}
	}

	igt_assert_eq(events, added);
	igt_assert_eq(tmp->head, l->head);

	memcpy(l->log, tmp->log, tmp->head);

	xe_eudebug_event_log_destroy(tmp);
}

/**
 * xe_eudebug_connect:
 * @fd: Xe file descriptor
 * @pid: client PID
 * @flags: connection flags
 *
 * Opens the xe eu debugger connection to the process described by @pid
 *
 * Returns: 0 if the debugger was successfully attached, -errno otherwise.
 */
static int xe_eudebug_connect(int fd, pid_t pid, uint32_t flags)
{
	struct drm_xe_eudebug_connect param = {
		.pid = pid,
		.flags = flags,
	};
	int debugfd;

	debugfd = igt_ioctl(fd, DRM_IOCTL_XE_EUDEBUG_CONNECT, &param);

	if (debugfd < 0)
		return -errno;

	return debugfd;
}

/**
 * xe_eudebug_event_log_create:
 * @name: event log identifier
 * @max_size: maximum size of created log
 *
 * Function creates an Eu Debugger event log with size equal to @max_size.
 *
 * Returns: pointer to just created log
 */
#define MAX_EVENT_LOG_SIZE (32 * 1024 * 1024)
struct xe_eudebug_event_log *xe_eudebug_event_log_create(const char *name, unsigned int max_size)
{
	struct xe_eudebug_event_log *l;

	igt_assert(name);

	l = calloc(1, sizeof(*l));
	igt_assert(l);
	l->log = calloc(1, max_size);
	igt_assert(l->log);
	l->max_size = max_size;
	strncpy(l->name, name, sizeof(l->name) - 1);
	pthread_mutex_init(&l->lock, NULL);

	return l;
}

/**
 * xe_eudebug_event_log_destroy:
 * @l: event log pointer
 *
 * Frees given event log @l.
 */
void xe_eudebug_event_log_destroy(struct xe_eudebug_event_log *l)
{
	igt_assert(l);
	pthread_mutex_destroy(&l->lock);
	free(l->log);
	free(l);
}

/**
 * xe_eudebug_event_log_write:
 * @l: event log pointer
 * @e: event to be written to event log
 *
 * Writes event @e to the event log, thread-safe.
 */
void xe_eudebug_event_log_write(struct xe_eudebug_event_log *l, struct drm_xe_eudebug_event *e)
{
	igt_assert(l);
	igt_assert(e);
	igt_assert(e->seqno);
	/*
	 * Try to catch if seqno is corrupted and prevent too long tests,
	 * as our post processing of events is not optimized.
	 */
	igt_assert_lt(e->seqno, 10 * 1000 * 1000);

	pthread_mutex_lock(&l->lock);
	igt_assert_lt(l->head + e->len, l->max_size);
	memcpy(l->log + l->head, e, e->len);
	l->head += e->len;
	pthread_mutex_unlock(&l->lock);
}

/**
 * xe_eudebug_event_log_print:
 * @l: event log pointer
 * @debug: when true function uses igt_debug instead of igt_info.
 *
 * Prints given event log.
 */
void
xe_eudebug_event_log_print(struct xe_eudebug_event_log *l, bool debug)
{
	struct drm_xe_eudebug_event *e = NULL;
	int level = debug ? IGT_LOG_DEBUG : IGT_LOG_INFO;
	char str[XE_EUDEBUG_EVENT_STRING_MAX_LEN];

	igt_assert(l);

	igt_log(IGT_LOG_DOMAIN, level,
		"event log '%s' (%u bytes):\n", l->name, l->head);

	xe_eudebug_for_each_event(e, l) {
		xe_eudebug_event_to_str(e, str, XE_EUDEBUG_EVENT_STRING_MAX_LEN);
		igt_log(IGT_LOG_DOMAIN, level, "%s\n", str);
	}
}

/**
 * xe_eudebug_event_log_compare:
 * @a: event log pointer
 * @b: event log pointer
 * @filter: mask that represents events to be skipped during comparison, useful
 * for events like 'VM_BIND' since they can be asymmetric. Note that
 * 'DRM_XE_EUDEBUG_EVENT_OPEN' will always be matched.
 *
 * Compares and asserts event logs @a, @b if the event
 * sequence matches.
 */
void xe_eudebug_event_log_compare(struct xe_eudebug_event_log *log1,
				  struct xe_eudebug_event_log *log2,
				  uint32_t filter)
{
	struct drm_xe_eudebug_event *ev1 = NULL;
	struct drm_xe_eudebug_event *ev2 = NULL;

	igt_assert(log1);
	igt_assert(log2);

	xe_eudebug_for_each_event(ev1, log1) {
		if (ev1->type == DRM_XE_EUDEBUG_EVENT_OPEN &&
		    ev1->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
			ev2 = event_type_match(log2, ev1, ev2);

			compare_client(log1, ev1, log2, ev2, filter);
			compare_client(log2, ev2, log1, ev1, filter);
		}
	}
}

/**
 * xe_eudebug_event_log_match_opposite:
 * @l: event log pointer
 * @filter: mask that represents events to be skipped during comparison, useful
 * for events like 'VM_BIND' since they can be asymmetric
 *
 * Matches and asserts content of all opposite events (create vs destroy).
 */
void
xe_eudebug_event_log_match_opposite(struct xe_eudebug_event_log *l, uint32_t filter)
{
	struct drm_xe_eudebug_event *ev1 = NULL;
	struct drm_xe_eudebug_event *ev2 = NULL;

	igt_assert(l);

	xe_eudebug_for_each_event(ev1, l) {
		if (ev1->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
			uint8_t offset = sizeof(struct drm_xe_eudebug_event);
			int opposite_matching;

			if (XE_EUDEBUG_EVENT_IS_FILTERED(ev1->type, filter))
				continue;

			/* No opposite matching for binds */
			if ((ev1->type >= DRM_XE_EUDEBUG_EVENT_VM_BIND &&
			     ev1->type <= DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE) ||
			    ev1->type == DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA)
				continue;

			ev2 = opposite_event_match(l, ev1, ev1);

			igt_assert_f(ev2, "no opposite event of type %u found\n", ev1->type);

			igt_assert_eq(ev1->len, ev2->len);
			opposite_matching = memcmp((uint8_t *)ev2 + offset,
						   (uint8_t *)ev1 + offset,
						   ev2->len - offset) == 0;

			igt_assert_f(opposite_matching,
				     "%s: create|destroy event not maching (%llu) vs (%llu)\n",
				     l->name, ev2->seqno, ev1->seqno);
		}
	}
}

static void debugger_run_triggers(struct xe_eudebug_debugger *d,
				  struct drm_xe_eudebug_event *e)
{
	struct event_trigger *t;

	igt_list_for_each_entry(t, &d->triggers, link) {
		if (e->type == t->type)
			t->fn(d, e);
	}
}

static int
xe_eudebug_read_event(int fd, struct drm_xe_eudebug_event *event)
{
	int ret;

	event->type = DRM_XE_EUDEBUG_EVENT_READ;
	event->flags = 0;
	event->len = MAX_EVENT_SIZE;

	ret = igt_ioctl(fd, DRM_XE_EUDEBUG_IOCTL_READ_EVENT, event);
	if (ret < 0)
		return -errno;

	return ret;
}

static void debugger_signal_handler(int sig, siginfo_t *info, void *context)
{
	struct xe_eudebug_debugger *d = info->si_ptr;

	igt_assert(d);

	d->received_signal = true;

	if (sig == SIGINT)
		d->received_sigint = true;
}

static void *debugger_worker_loop(void *data)
{
	uint8_t buf[MAX_EVENT_SIZE];
	struct drm_xe_eudebug_event *e = (void *)buf;
	struct xe_eudebug_debugger *d = data;
	struct pollfd p = {
		.events = POLLIN,
		.revents = 0,
	};
	int timeout_ms = 100, ret;
	struct sigaction sa = { 0 };

	igt_assert(d->master_fd >= 0);

	igt_assert_eq(sigaction(SIGINT, NULL, &sa), 0);
	sa.sa_sigaction = debugger_signal_handler;
	sa.sa_flags |= SA_SIGINFO;
	igt_assert_eq(sigaction(SIGINT, &sa, NULL), 0);

	igt_assert_eq(sigaction(SIGTERM, NULL, &sa), 0);
	sa.sa_sigaction = debugger_signal_handler;
	sa.sa_flags |= SA_SIGINFO;
	igt_assert_eq(sigaction(SIGTERM, &sa, NULL), 0);

	do {
		p.fd = d->fd;
		ret = poll(&p, 1, timeout_ms);
		if (d->received_sigint) {
			d->handled_sigint = true;
			pthread_exit(NULL);
		}

		if (ret == -1) {
			if (d->received_signal) {
				d->received_signal = false;

				if (errno == EINTR)
					continue;
			}

			igt_info("poll failed with errno %d\n", errno);
			break;
		}

		if (ret == 1 && (p.revents & POLLIN)) {
			int err = xe_eudebug_read_event(d->fd, e);

			if (!err) {
				++d->event_count;

				xe_eudebug_event_log_write(d->log, e);
				debugger_run_triggers(d, e);
			} else {
				igt_info("xe_eudebug_read_event returned %d\n", ret);
			}
		}
	} while ((ret && READ_ONCE(d->worker_state) == DEBUGGER_WORKER_QUITTING) ||
		 READ_ONCE(d->worker_state) == DEBUGGER_WORKER_ACTIVE);

	d->worker_state = DEBUGGER_WORKER_INACTIVE;

	return NULL;
}

/**
 * xe_eudebug_debugger_available:
 * @fd: Xe file descriptor
 *
 * Returns: true it debugger connection is available, false otherwise.
 */
bool xe_eudebug_debugger_available(int fd)
{
	struct drm_xe_eudebug_connect param = { .pid = getpid() };
	int debugfd;

	debugfd = igt_ioctl(fd, DRM_IOCTL_XE_EUDEBUG_CONNECT, &param);
	if (debugfd >= 0)
		close(debugfd);

	return debugfd >= 0;
}

/**
 * xe_eudebug_debugger_create:
 * @master_fd: xe client used to open the debugger connection
 * @flags: flags stored in a debugger structure, can be used at will
 * of the caller, i.e. to be used inside triggers.
 * @data: test's private data, allocated with MAP_SHARED | MAP_ANONYMOUS,
 * can be shared between client and debugger. Can be NULL.
 *
 * Returns: newly created xe_eudebug_debugger structure with its
 * event log initialized. Note that to open the connection
 * you need call @xe_eudebug_debugger_attach.
 */
struct xe_eudebug_debugger *
xe_eudebug_debugger_create(int master_fd, uint64_t flags, void *data)
{
	struct xe_eudebug_debugger *d;

	d = calloc(1, sizeof(*d));
	igt_assert(d);

	d->flags = flags;
	IGT_INIT_LIST_HEAD(&d->triggers);
	d->log = xe_eudebug_event_log_create("debugger", MAX_EVENT_LOG_SIZE);
	d->fd = -1;
	d->master_fd = master_fd;
	d->ptr = data;
	d->received_sigint = false;
	d->handled_sigint = false;
	d->received_signal = false;

	return d;
}

static void debugger_destroy_triggers(struct xe_eudebug_debugger *d)
{
	struct event_trigger *t, *tmp;

	igt_list_for_each_entry_safe(t, tmp, &d->triggers, link)
		free(t);
}

/**
 * xe_eudebug_debugger_destroy:
 * @d: pointer to the debugger
 *
 * Frees xe_eudebug_debugger structure pointed by @d. If the debugger
 * connection was still opened it terminates it.
 */
void xe_eudebug_debugger_destroy(struct xe_eudebug_debugger *d)
{
	if (d->worker_state != DEBUGGER_WORKER_INACTIVE)
		xe_eudebug_debugger_stop_worker(d);

	if (d->target_pid)
		xe_eudebug_debugger_detach(d);

	xe_eudebug_event_log_destroy(d->log);
	debugger_destroy_triggers(d);
	free(d);
}

static int __xe_eudebug_debugger_attach(struct xe_eudebug_debugger *d, pid_t pid)
{
	int ret;

	igt_assert_eq(d->fd, -1);
	igt_assert_eq(d->target_pid, 0);

	ret = xe_eudebug_connect(d->master_fd, pid, 0);
	if (ret < 0)
		return ret;

	d->fd = ret;
	d->target_pid = pid;

	register_debugger_fd(d->fd);

	return 0;
}

/**
 * xe_eudebug_debugger_reattach:
 * @d: pointer to the debugger
 * @pid: pid of the process to attach to
 *
 * Re-establishes debugger connection to a process. To be used only for
 * reconnection scenarios where the debugger was previously attached using
 * xe_eudebug_debugger_attach() and detached with xe_eudebug_debugger_detach().
 *
 * Returns: 0 if the debugger was successfully reattached, -errno otherwise.
 */
int xe_eudebug_debugger_reattach(struct xe_eudebug_debugger *d, pid_t pid)
{
	return  __xe_eudebug_debugger_attach(d, pid);
}

/**
 * xe_eudebug_debugger_attach:
 * @d: pointer to the debugger
 * @c: pointer to the client
 *
 * Opens the xe eu debugger connection to the process described by @c (c->pid)
 *
 * Returns: 0 if the debugger was successfully attached, -errno otherwise.
 */
int xe_eudebug_debugger_attach(struct xe_eudebug_debugger *d,
			       struct xe_eudebug_client *c)
{
	int ret;

	ret = __xe_eudebug_debugger_attach(d, c->pid);
	if (ret < 0)
		return ret;

	d->p_client[0] = c->p_in[0];
	d->p_client[1] = c->p_in[1];

	return 0;
}

/**
 * xe_eudebug_debugger_detach:
 * @d: pointer to the debugger
 *
 * Closes previously opened xe eu debugger connection. Asserts if
 * the debugger has active session.
 */
void xe_eudebug_debugger_detach(struct xe_eudebug_debugger *d)
{
	igt_assert(d->target_pid);
	unregister_debugger_fd(d->fd);
	close(d->fd);
	d->target_pid = 0;
	d->fd = -1;
}

/**
 * xe_eudebug_debugger_add_trigger:
 * @d: pointer to the debugger
 * @type: the type of the event which activates the trigger
 * @fn: function to be called when event of @type was read by the debugger.
 *
 * Adds function @fn to the list of triggers activated when event of @type
 * has been read by worker.
 * Note: Triggers are activated by the worker.
 */
void xe_eudebug_debugger_add_trigger(struct xe_eudebug_debugger *d,
				     int type, xe_eudebug_trigger_fn fn)
{
	struct event_trigger *t;

	t = calloc(1, sizeof(*t));
	igt_assert(t);

	IGT_INIT_LIST_HEAD(&t->link);
	t->type = type;
	t->fn = fn;

	igt_list_add_tail(&t->link, &d->triggers);
	igt_debug("added trigger %p\n", t);
}

/**
 * xe_eudebug_debugger_remove_trigger:
 * @d: pointer to the debugger
 * @type: the type of the event which activates the trigger.
 * @fn: function to be removed when event of @type was read by the debugger.
 *
 * Removes function @fn from the list of triggers activated when event of
 * @type has been read by worker.
 */
void xe_eudebug_debugger_remove_trigger(struct xe_eudebug_debugger *d,
					int type, xe_eudebug_trigger_fn fn)
{
	struct event_trigger *t;
	bool found = false;

	igt_list_for_each_entry(t, &d->triggers, link) {
		if (type == t->type && fn == t->fn) {
			igt_list_del(&t->link);
			found = true;
			break;
		}
	}
	if (found) {
		igt_debug("removed trigger %p\n", t);
		free(t);
	} else {
		igt_debug("trigger of type %d was not removed as it's not in the list\n", type);
	}
}

/**
 * xe_eudebug_debugger_start_worker:
 * @d: pointer to the debugger
 *
 * Starts the debugger worker. Worker is resposible for reading all
 * incoming events from the debugger, put then into debugger log and
 * execute appropriate event triggers. Note that using the debuggers
 * event log while worker is running is not safe.
 */
void xe_eudebug_debugger_start_worker(struct xe_eudebug_debugger *d)
{
	int ret;

	d->worker_state = DEBUGGER_WORKER_ACTIVE;
	ret = pthread_create(&d->worker_thread, NULL, &debugger_worker_loop, d);

	igt_assert_f(ret == 0, "Debugger worker thread creation failed!");
}

/**
 * xe_eudebug_debugger_stop_worker:
 * @d: pointer to the debugger
 *
 * Stops the debugger worker. Event log is sorted by seqno after closure.
 */
void xe_eudebug_debugger_stop_worker(struct xe_eudebug_debugger *d)
{
	const int timeout_s = 3;
	struct timespec t = {};
	int ret;

	igt_assert_neq(d->worker_state, DEBUGGER_WORKER_INACTIVE);

	d->worker_state = DEBUGGER_WORKER_QUITTING; /* First time be polite. */
	igt_assert_eq(clock_gettime(CLOCK_REALTIME, &t), 0);
	t.tv_sec += timeout_s;

	ret = pthread_timedjoin_np(d->worker_thread, NULL, &t);

	if (ret == ETIMEDOUT) {
		d->worker_state = DEBUGGER_WORKER_INACTIVE;
		ret = pthread_join(d->worker_thread, NULL);
	}

	igt_assert_f(ret == 0 || ret != ESRCH,
		     "pthread join failed with error %d!\n", ret);

	event_log_sort(d->log);
}

/**
 * xe_eudebug_debugger_signal_stage:
 * @d: pointer to the debugger
 * @stage: stage to signal
 *
 * Signals to client, waiting in xe_eudebug_client_wait_stage(),
 * releasing it to proceed.
 */
void xe_eudebug_debugger_signal_stage(struct xe_eudebug_debugger *d, uint64_t stage)
{
	token_signal(d->p_client, CLIENT_STAGE, stage);
}

/**
 * xe_eudebug_debugger_wait_stage:
 * @s: pointer to xe_eudebug_debugger structure
 * @stage: stage to wait on
 *
 * Pauses debugger until the client has signalled the corresponding stage with
 * xe_eudebug_client_signal_stage. This is only for situations where the actual
 * event flow is not enough to coordinate between client/debugger and extra sync
 * mechanism is needed.
 */
void xe_eudebug_debugger_wait_stage(struct xe_eudebug_session *s, uint64_t stage)
{
	u64 stage_in;

	igt_debug("debugger xe client fd: %d pausing for stage %" PRIu64 "\n", s->debugger->master_fd,
		  stage);

	stage_in = wait_from_client(s->client, DEBUGGER_STAGE);
	igt_debug("debugger xe client fd: %d stage %" PRIu64 ", expected %" PRIu64 ", stage\n",
		  s->debugger->master_fd, stage_in, stage);

	igt_assert_eq(stage_in, stage);
}

/**
 * xe_eudebug_debugger_kill:
 * @d: pointer to the debugger
 * @sig: signal to send
 *
 * Sends @sig signal to the debugger thread.
 * Passes the debugger struct to signal handler.
 */
void xe_eudebug_debugger_kill(struct xe_eudebug_debugger *d, int sig)
{
	pthread_sigqueue(d->worker_thread, sig, (union sigval){ .sival_ptr = (void*)d });
}

/**
 * xe_eudebug_client_create:
 * @master_fd: xe client used to open the debugger connection
 * @work: function that opens xe device and executes arbitrary workload
 * @flags: flags stored in a client structure, can be used at will
 * of the caller, i.e. to provide the @work function an additional switch.
 * @data: test's private data, allocated with MAP_SHARED | MAP_ANONYMOUS,
 * can be shared between client and debugger. Accesible via client->ptr.
 * Can be NULL.
 *
 * Forks and creates the debugger process. @work won't be called until
 * xe_eudebug_client_start is called.
 *
 * Returns: newly created xe_eudebug_debugger structure with its
 * event log initialized.
 */
struct xe_eudebug_client *xe_eudebug_client_create(int master_fd, xe_eudebug_client_work_fn work,
						   uint64_t flags, void *data)
{
	struct xe_eudebug_client *c;

	c = calloc(1, sizeof(*c));
	igt_assert(c);

	c->flags = flags;
	igt_assert(!pipe(c->p_in));
	igt_assert(!pipe(c->p_out));
	c->seqno = 1;
	c->log = xe_eudebug_event_log_create("client", MAX_EVENT_LOG_SIZE);
	c->done = 0;
	c->ptr = data;
	c->master_fd = master_fd;
	c->timeout_ms = XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * MSEC_PER_SEC;
	c->allow_dead_client = false;
	pthread_mutex_init(&c->lock, NULL);

	igt_fork(child, 1) {
		int mypid;

		igt_assert_eq(c->pid, 0);

		close(c->p_out[0]);
		c->p_out[0] = -1;
		close(c->p_in[1]);
		c->p_in[1] = -1;

		mypid = getpid();
		client_signal(c, CLIENT_PID, mypid);

		c->pid = client_wait_token(c, CLIENT_RUN);
		igt_assert_eq(c->pid, mypid);
		igt_assert(work);

		igt_debug("client: work start\n");
		work(c);
		igt_debug("client: work end\n");

		client_signal(c, CLIENT_FINI, c->seqno);

		event_log_write_to_fd(c->log, c->p_out[1]);

		c->pid = client_wait_token(c, CLIENT_STOP);
		igt_assert_eq(c->pid, mypid);
	}

	close(c->p_out[1]);
	c->p_out[1] = -1;
	close(c->p_in[0]);
	c->p_in[0] = -1;

	c->pid = wait_from_client(c, CLIENT_PID);

	igt_info("client running with pid %d\n", c->pid);

	return c;
}

/**
 * xe_eudebug_client_stop:
 * @c: pointer to xe_eudebug_client structure
 *
 * Waits for the end of client's work and exits the proccess.
 */
void xe_eudebug_client_stop(struct xe_eudebug_client *c)
{
	if (c->pid) {
		int waitstatus;
		int ret;

		xe_eudebug_client_wait_done(c);

		token_signal(c->p_in, CLIENT_STOP, c->pid);
		ret = waitpid(c->pid, &waitstatus, 0);
		/* process may be gone already */
		if (!(ret == -1 && errno == ECHILD))
			igt_assert_eq(ret, c->pid);

		c->pid = 0;
	}
}

/**
 * xe_eudebug_client_destroy:
 * @c: pointer to xe_eudebug_client structure to be freed
 *
 * Frees the @c client structure. Note that it calls xe_eudebug_client_stop if
 * client proccess has not terminated yet.
 */
void xe_eudebug_client_destroy(struct xe_eudebug_client *c)
{
	xe_eudebug_client_stop(c);
	pipe_close(c->p_in);
	pipe_close(c->p_out);
	xe_eudebug_event_log_destroy(c->log);
	pthread_mutex_destroy(&c->lock);
	free(c);
}

/**
 * xe_eudebug_client_get_seqno:
 * @c: pointer to xe_eudebug_client structure
 *
 * Increments and returns current seqno value of the given client @c
 *
 * Returns: incremented seqno
 */
uint64_t xe_eudebug_client_get_seqno(struct xe_eudebug_client *c)
{
	uint64_t ret;
	pthread_mutex_lock(&c->lock);
	ret = c->seqno++;
	pthread_mutex_unlock(&c->lock);
	return ret;
}

/**
 * xe_eudebug_client_start:
 * @c: pointer to xe_eudebug_client structure
 *
 * Starts execution of client's work function within the client's proccess.
 */
void xe_eudebug_client_start(struct xe_eudebug_client *c)
{
	token_signal(c->p_in, CLIENT_RUN, c->pid);
}

/**
 * xe_eudebug_client_wait_done:
 * @c: pointer to xe_eudebug_client structure
 *
 * Waits for the client work end updates the event log.
 * Doesn't terminate the client's proccess yet.
 */
void xe_eudebug_client_wait_done(struct xe_eudebug_client *c)
{
	if (!c->done) {
		c->seqno = wait_from_client(c, CLIENT_FINI);
		event_log_read_from_fd(c->log, c->p_out[0]);
		c->done = 1;
	}
}

/**
 * xe_eudebug_client_signal_stage:
 * @c: pointer to the client
 * @stage: stage to signal
 *
 * Signals to debugger, waiting in xe_eudebug_debugger_wait_stage(),
 * releasing it to proceed.
 */
void xe_eudebug_client_signal_stage(struct xe_eudebug_client *c, uint64_t stage)
{
	token_signal(c->p_out, DEBUGGER_STAGE, stage);
}

/**
 * xe_eudebug_client_wait_stage:
 * @c: pointer to xe_eudebug_client structure
 * @stage: stage to wait on
 *
 * Pauses client until the debugger has signalled the corresponding stage with
 * xe_eudebug_debugger_signal_stage. This is only for situations where the
 * actual event flow is not enough to coordinate between client/debugger and extra
 * sync mechanism is needed.
 *
 */
void xe_eudebug_client_wait_stage(struct xe_eudebug_client *c, uint64_t stage)
{
	u64 stage_in;

	if (c->done) {
		igt_warn("client: %d already done before %" PRIu64 "\n", c->pid, stage);
		return;
	}

	igt_debug("client: %d pausing for stage %" PRIu64 "\n", c->pid, stage);

	stage_in = client_wait_token(c, CLIENT_STAGE);
	igt_debug("client: %d stage %" PRIu64 ", expected %" PRIu64 ", stage\n", c->pid,
		  stage_in, stage);

	igt_assert_eq(stage_in, stage);
}

/**
 * xe_eudebug_session_create:
 * @fd: Xe file descriptor
 * @work: function passed to the xe_eudebug_client_create
 * @flags: flags passed to client and debugger
 * @test_private: test's  data, allocated with MAP_SHARED | MAP_ANONYMOUS,
 * passed to client and debugger. Can be NULL.
 *
 * Creates session together with client and debugger structures.
 */
struct xe_eudebug_session *xe_eudebug_session_create(int fd,
						     xe_eudebug_client_work_fn work,
						     unsigned int flags,
						     void *test_private)
{
	struct xe_eudebug_session *s;

	s = calloc(1, sizeof(*s));
	igt_assert(s);

	s->client = xe_eudebug_client_create(fd, work, flags, test_private);
	s->debugger = xe_eudebug_debugger_create(fd, flags, test_private);
	s->flags = flags;

	return s;
}

/**
 * xe_eudebug_session_run:
 * @s: pointer to xe_eudebug_session structure
 *
 * Attaches debugger to client's proccess, starts debugger's
 * async event reader, starts client and once client finish
 * it stops debugger worker.
 */
void xe_eudebug_session_run(struct xe_eudebug_session *s)
{
	struct xe_eudebug_debugger *debugger = s->debugger;
	struct xe_eudebug_client *client = s->client;

	igt_assert_eq(xe_eudebug_debugger_attach(debugger, client), 0);

	xe_eudebug_debugger_start_worker(debugger);

	xe_eudebug_client_start(client);
	xe_eudebug_client_wait_done(client);

	xe_eudebug_debugger_stop_worker(debugger);

	xe_eudebug_event_log_print(debugger->log, true);
	xe_eudebug_event_log_print(client->log, true);
}

/**
 * xe_eudebug_session_check:
 * @s: pointer to xe_eudebug_session structure
 * @match_opposite: indicates whether check should match all
 * create and destroy events.
 * @filter: mask that represents events to be skipped during comparison, useful
 * for events like 'VM_BIND' since they can be asymmetric
 *
 * Validate debugger's log against the log created by the client.
 */
void xe_eudebug_session_check(struct xe_eudebug_session *s, bool match_opposite, uint32_t filter)
{
	xe_eudebug_event_log_compare(s->client->log, s->debugger->log, filter);

	if (match_opposite)
		xe_eudebug_event_log_match_opposite(s->debugger->log, filter);
}

/**
 * xe_eudebug_session_destroy:
 * @s: pointer to xe_eudebug_session structure
 *
 * Destroy session together with its debugger and client.
 */
void xe_eudebug_session_destroy(struct xe_eudebug_session *s)
{
	xe_eudebug_debugger_destroy(s->debugger);
	xe_eudebug_client_destroy(s->client);

	free(s);
}

#define to_base(x) ((struct drm_xe_eudebug_event *)&(x))

static void base_event(struct xe_eudebug_client *c,
		       struct drm_xe_eudebug_event *e,
		       uint32_t type,
		       uint32_t flags,
		       uint64_t size)
{
	e->type = type;
	e->flags = flags;
	e->seqno = xe_eudebug_client_get_seqno(c);
	e->len = size;
}

static void client_event(struct xe_eudebug_client *c, uint32_t flags, int client_fd)
{
	struct drm_xe_eudebug_event_client ec;

	base_event(c, to_base(ec), DRM_XE_EUDEBUG_EVENT_OPEN, flags, sizeof(ec));

	ec.client_handle = client_fd;

	xe_eudebug_event_log_write(c->log, (void *)&ec);
}

static void vm_event(struct xe_eudebug_client *c, uint32_t flags, int client_fd, uint32_t vm_id)
{
	struct drm_xe_eudebug_event_vm evm;

	base_event(c, to_base(evm), DRM_XE_EUDEBUG_EVENT_VM, flags, sizeof(evm));

	evm.client_handle = client_fd;
	evm.vm_handle = vm_id;

	xe_eudebug_event_log_write(c->log, (void *)&evm);
}

static void exec_queue_event(struct xe_eudebug_client *c, uint32_t flags,
			     int client_fd, uint32_t vm_id,
			     uint32_t exec_queue_handle, uint16_t class,
			     uint16_t width)
{
	struct drm_xe_eudebug_event_exec_queue ee;

	base_event(c, to_base(ee), DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE,
		   flags, sizeof(ee));

	ee.client_handle = client_fd;
	ee.vm_handle = vm_id;
	ee.exec_queue_handle = exec_queue_handle;
	ee.engine_class = class;
	ee.width = width;

	xe_eudebug_event_log_write(c->log, (void *)&ee);
}

static void metadata_event(struct xe_eudebug_client *c, uint32_t flags,
			   int client_fd, uint32_t id, uint64_t type, uint64_t len)
{
	struct drm_xe_eudebug_event_metadata em;

	base_event(c, to_base(em), DRM_XE_EUDEBUG_EVENT_METADATA,
		   flags, sizeof(em));

	em.client_handle = client_fd;
	em.metadata_handle = id;
	em.type = type;
	em.len = len;

	xe_eudebug_event_log_write(c->log, (void *)&em);
}

#define EU_DEBUG_TOGGLE "device/enable_eudebug"

static char *get_card_name(int fd, char *card_name, size_t len)
{
	char sysfs_path[PATH_MAX];
	char link_target[PATH_MAX];
	char *card_start;
	ssize_t link_len;

	if (!igt_sysfs_path(fd, sysfs_path, sizeof(sysfs_path)))
		return NULL;

	link_len = readlink(sysfs_path, link_target, sizeof(link_target) - 1);
	if (link_len < 0)
		return NULL;

	link_target[link_len] = '\0';

	card_start = strstr(link_target, "card");
	if (!card_start)
		return NULL;

	snprintf(card_name, len, "%s", card_start);
	card_start = strchr(card_name, '/');
	if (card_start)
		*card_start = '\0';

	return card_name;
}
/**
 * __xe_eudebug_enable_getset
 * @fd: xe client
 * @old: pointer to store current toggle value
 * @new: pointer to new toggle value
 *
 * Stores current eudebug feature state in @old if not NULL. Sets new eudebug
 * feature state to @new if not NULL. Asserts if both @old and @new are NULL.
 *
 * Returns: 0 on success, -errno on failure.
 */
int __xe_eudebug_enable_getset(int fd, bool *old, bool *new)
{
	int ret = 0;
	int sysfs;
	bool val_before;
	struct stat st;

	igt_assert(new || old);
	igt_assert_eq(fstat(fd, &st), 0);

	sysfs = igt_sysfs_open(fd);
	if (sysfs < 0)
		return -errno;

	if (!__igt_sysfs_get_boolean(sysfs, EU_DEBUG_TOGGLE, &val_before)) {
		ret = -errno;
		goto out;
	}

	igt_debug("enable_eudebug before: %d\n", val_before);

	if (old)
		*old = val_before;

	if (new) {
		if (__igt_sysfs_set_boolean(sysfs, EU_DEBUG_TOGGLE, *new))
			igt_assert_eq(igt_sysfs_get_boolean(sysfs, EU_DEBUG_TOGGLE), *new);
		else
			ret = -errno;
	}

out:
	close(sysfs);

	return ret;
}

/**
 * xe_eudebug_enable
 * @fd: xe client
 * @enable: state toggle - true to enable, false to disable
 *
 * Enables/disables eudebug capability by writing to
 * '/sys/class/drm/card<N>/device/enable_eudebug' sysfs entry.
 *
 * Returns: previous toggle value, i.e. true when eudebugging was enabled,
 * false when eudebugging was disabled.
 */
bool xe_eudebug_enable(int fd, bool enable)
{
	char card_name[NAME_MAX];
	bool old = false;
	int ret = 0;

	/* When disabling eudebug, close all active debugger sessions first. */
	if (!enable)
		close_all_debugger_fds();

	/* 'struct drm_driver.postclose' runs asynchronously to 'close', wait for it to complete */
	for (int i = 0; i < 10; ++i) {
		ret = __xe_eudebug_enable_getset(fd, &old, &enable);

		if (ret != -EBUSY)
			break;

		if (i < 9) {
			igt_debug("xe_eudebug_enable: Failed (%d), retrying...\n", ret);
			sleep(1);
		}
	}

	if (ret == -ENOENT) {
		if (get_card_name(fd, card_name, sizeof(card_name)))
			igt_skip("'/sys/class/drm/%s/" EU_DEBUG_TOGGLE
				 "' sysfs attribute not found, EU DEBUG not supported\n",
				 card_name);
		else
			igt_skip("'" EU_DEBUG_TOGGLE
				 "' sysfs attribute not found, EU DEBUG not supported\n");
	}

	igt_abort_on_f(ret, "xe_eudebug_enable: Failed to %s eudebug\n",
		       enable ? "enable" : "disable");

	return old;
}

/* Eu debugger wrappers around resource creating xe ioctls. */

/**
 * xe_eudebug_client_open_driver:
 * @c: pointer to xe_eudebug_client structure
 *
 * Calls drm_open_client(DRIVER_XE) and logs the corresponding
 * event in client's event log.
 *
 * Returns: valid DRM file descriptor
 */
int xe_eudebug_client_open_driver(struct xe_eudebug_client *c)
{
	int fd;

	igt_assert(c);
	fd = drm_reopen_driver(c->master_fd);
	client_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, fd);

	return fd;
}

/**
 * xe_eudebug_client_close_driver:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 *
 * Calls close driver and logs the corresponding event in
 * client's event log.
 */
void xe_eudebug_client_close_driver(struct xe_eudebug_client *c, int fd)
{
	igt_assert(c);
	client_event(c, DRM_XE_EUDEBUG_EVENT_DESTROY, fd);
	drm_close_driver(fd);
}

/**
 * xe_eudebug_client_vm_create:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @flags: vm bind flags
 * @ext: pointer to the first user extension
 *
 * Calls xe_vm_create() and logs corresponding events
 * (including vm set metadata events) in client's event log.
 *
 * Returns: valid vm handle
 */
uint32_t xe_eudebug_client_vm_create(struct xe_eudebug_client *c, int fd,
				     uint32_t flags, uint64_t ext)
{
	uint32_t vm;

	igt_assert(c);
	vm = xe_vm_create(fd, flags, ext);
	vm_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, fd, vm);

	return vm;
}

/**
 * xe_eudebug_client_vm_destroy:
 * @c: pointer to xe_eudebug_client structure
 * fd: xe client
 * vm: vm handle
 *
 * Calls xe_vm_destroy() and logs the corresponding event in
 * client's event log.
 */
void xe_eudebug_client_vm_destroy(struct xe_eudebug_client *c, int fd, uint32_t vm)
{
	igt_assert(c);
	xe_vm_destroy(fd, vm);
	vm_event(c, DRM_XE_EUDEBUG_EVENT_DESTROY, fd, vm);
}

/**
 * xe_eudebug_client_exec_queue_create:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @create: exec_queue create drm struct
 *
 * Calls xe exec queue create ioctl and logs the corresponding event in
 * client's event log.
 *
 * Returns: valid exec queue handle
 */
uint32_t xe_eudebug_client_exec_queue_create(struct xe_eudebug_client *c, int fd,
					     struct drm_xe_exec_queue_create *create)
{
	struct drm_xe_engine_class_instance *instances;
	struct drm_xe_ext_set_property *ext;
	bool send = false;
	uint16_t class;

	igt_assert(c);
	igt_assert(create);

	instances = from_user_pointer(create->instances);
	class = instances[0].engine_class;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, create), 0);

	for (ext = from_user_pointer(create->extensions); ext;
	     ext = from_user_pointer(ext->base.next_extension))
		if (ext->base.name == DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY &&
		    ext->property == DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG &&
		    ext->value & DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE)
			send = true;

	if (send)
		exec_queue_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, fd, create->vm_id,
				 create->exec_queue_id, class, create->width);

	return create->exec_queue_id;
}

/**
 * xe_eudebug_client_exec_queue_destroy:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @create: exec_queue create drm struct which was used for creation
 *
 * Calls xe exec_queue destroy ioctl and logs the corresponding event in
 * client's event log.
 */
void xe_eudebug_client_exec_queue_destroy(struct xe_eudebug_client *c, int fd,
					  struct drm_xe_exec_queue_create *create)
{
	struct drm_xe_engine_class_instance *instances;
	struct drm_xe_exec_queue_destroy destroy = {};
	struct drm_xe_ext_set_property *ext;
	bool send = false;
	uint16_t class;

	igt_assert(c);
	igt_assert(create);

	destroy.exec_queue_id = create->exec_queue_id;
	instances = from_user_pointer(create->instances);
	class = instances[0].engine_class;

	for (ext = from_user_pointer(create->extensions); ext;
	     ext = from_user_pointer(ext->base.next_extension))
		if (ext->base.name == DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY &&
		    ext->property == DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG &&
		    ext->value & DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE)
			send = true;

	if (send)
		exec_queue_event(c, DRM_XE_EUDEBUG_EVENT_DESTROY, fd, create->vm_id,
				 create->exec_queue_id, class, create->width);

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &destroy), 0);
}

/**
 * xe_eudebug_client_vm_bind_event:
 * @c: pointer to xe_eudebug_client structure
 * @event_flags: base event flags
 * @fd: xe client
 * @vm: vm handle
 * @bind_flags: bind flags of vm_bind_event
 * @num_binds: number of bind (operations) for event
 * @ref_seqno: base vm bind reference seqno
 * Logs vm bind event in client's event log.
 */
void xe_eudebug_client_vm_bind_event(struct xe_eudebug_client *c,
				     uint32_t event_flags, int fd,
				     uint32_t vm, uint32_t bind_flags,
				     uint32_t num_binds, u64 *ref_seqno)
{
	struct drm_xe_eudebug_event_vm_bind evmb;

	igt_assert(c);
	igt_assert(ref_seqno);

	base_event(c, to_base(evmb), DRM_XE_EUDEBUG_EVENT_VM_BIND,
		   event_flags, sizeof(evmb));
	evmb.client_handle = fd;
	evmb.vm_handle = vm;
	evmb.flags = bind_flags;
	evmb.num_binds = num_binds;

	*ref_seqno = evmb.base.seqno;

	xe_eudebug_event_log_write(c->log, (void *)&evmb);
}

/**
 * xe_eudebug_client_vm_bind_op_event:
 * @c: pointer to xe_eudebug_client structure
 * @event_flags: base event flags
 * @bind_ref_seqno: base vm bind reference seqno
 * @op_ref_seqno: output, the vm_bind_op event seqno
 * @addr: ppgtt address
 * @size: size of the binding
 * @num_extensions: number of vm bind op extensions
 *
 * Logs vm bind op event in client's event log.
 */
void xe_eudebug_client_vm_bind_op_event(struct xe_eudebug_client *c, uint32_t event_flags,
					uint64_t bind_ref_seqno, uint64_t *op_ref_seqno,
					uint64_t addr, uint64_t range,
					uint64_t num_extensions)
{
	struct drm_xe_eudebug_event_vm_bind_op op;

	igt_assert(c);
	igt_assert(op_ref_seqno);

	base_event(c, to_base(op), DRM_XE_EUDEBUG_EVENT_VM_BIND_OP,
		   event_flags, sizeof(op));
	op.vm_bind_ref_seqno = bind_ref_seqno;
	op.addr = addr;
	op.range = range;
	op.num_extensions = num_extensions;

	*op_ref_seqno = op.base.seqno;

	xe_eudebug_event_log_write(c->log, (void *)&op);
}

/**
 * xe_eudebug_client_vm_bind_op_metadata_event:
 * @c: pointer to xe_eudebug_client structure
 * @event_flags: base event flags
 * @op_ref_seqno: base vm bind op reference seqno
 * @metadata_handle: metadata handle
 * @metadata_cookie: metadata cookie
 *
 * Logs vm bind op metadata event in client's event log.
 */
void xe_eudebug_client_vm_bind_op_metadata_event(struct xe_eudebug_client *c,
						 uint32_t event_flags, uint64_t op_ref_seqno,
						 uint64_t metadata_handle, uint64_t metadata_cookie)
{
	struct drm_xe_eudebug_event_vm_bind_op_metadata op;

	igt_assert(c);

	base_event(c, to_base(op), DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA,
		   event_flags, sizeof(op));
	op.vm_bind_op_ref_seqno = op_ref_seqno;
	op.metadata_handle = metadata_handle;
	op.metadata_cookie = metadata_cookie;

	xe_eudebug_event_log_write(c->log, (void *)&op);
}

/**
 * xe_eudebug_client_vm_bind_ufence_event:
 * @c: pointer to xe_eudebug_client structure
 * @event_flags: base event flags
 * @ref_seqno: base vm bind event seqno
 *
 * Logs vm bind ufence event in client's event log.
 */
void xe_eudebug_client_vm_bind_ufence_event(struct xe_eudebug_client *c, uint32_t event_flags,
					    uint64_t ref_seqno)
{
	struct drm_xe_eudebug_event_vm_bind_ufence f;

	igt_assert(c);

	base_event(c, to_base(f), DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
		   event_flags, sizeof(f));
	f.vm_bind_ref_seqno = ref_seqno;

	xe_eudebug_event_log_write(c->log, (void *)&f);
}

static bool has_user_fence(const struct drm_xe_sync *sync, uint32_t num_syncs)
{
	while (num_syncs--)
		if (sync[num_syncs].type == DRM_XE_SYNC_TYPE_USER_FENCE)
			return true;

	return false;
}

#define for_each_metadata(__m, __ext)					\
	for ((__m) = from_user_pointer(__ext);				\
	     (__m);							\
	     (__m) = from_user_pointer((__m)->base.next_extension))	\
		if ((__m)->base.name == XE_VM_BIND_OP_EXTENSIONS_ATTACH_DEBUG)

static int  __xe_eudebug_client_vm_bind(struct xe_eudebug_client *c,
					int fd, uint32_t vm, uint32_t exec_queue,
					uint32_t bo, uint64_t offset,
					uint64_t addr, uint64_t size,
					uint32_t op, uint32_t flags,
					struct drm_xe_sync *sync,
					uint32_t num_syncs,
					uint32_t prefetch_region,
					uint8_t pat_index, uint64_t op_ext)
{
	struct drm_xe_vm_bind_op_ext_attach_debug *metadata;
	const bool ufence = has_user_fence(sync, num_syncs);
	const uint32_t bind_flags = ufence ?
		DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE : 0;
	uint64_t seqno = 0, op_seqno = 0, num_metadata = 0;
	uint32_t bind_base_flags = 0;
	int ret;

	for_each_metadata(metadata, op_ext)
		num_metadata++;

	switch (op) {
	case DRM_XE_VM_BIND_OP_MAP:
		bind_base_flags = DRM_XE_EUDEBUG_EVENT_CREATE;
		break;
	case DRM_XE_VM_BIND_OP_UNMAP:
		bind_base_flags = DRM_XE_EUDEBUG_EVENT_DESTROY;
		igt_assert_eq(num_metadata, 0);
		igt_assert_eq(ufence, false);
		break;
	default:
		/* XXX unmap all? */
		igt_assert(op);
		break;
	}

	ret = ___xe_vm_bind(fd, vm, exec_queue, bo, offset, addr, size,
			    op, flags, sync, num_syncs, prefetch_region,
			    pat_index, 0, op_ext);

	if (ret)
		return ret;

	if (!bind_base_flags)
		return -EINVAL;

	xe_eudebug_client_vm_bind_event(c, DRM_XE_EUDEBUG_EVENT_STATE_CHANGE,
					fd, vm, bind_flags, 1, &seqno);
	xe_eudebug_client_vm_bind_op_event(c, bind_base_flags,
					   seqno, &op_seqno, addr, size,
					   num_metadata);

	for_each_metadata(metadata, op_ext)
		xe_eudebug_client_vm_bind_op_metadata_event(c,
							    DRM_XE_EUDEBUG_EVENT_CREATE,
							    op_seqno,
							    metadata->metadata_id,
							    metadata->cookie);
	if (ufence)
		xe_eudebug_client_vm_bind_ufence_event(c, DRM_XE_EUDEBUG_EVENT_CREATE |
						       DRM_XE_EUDEBUG_EVENT_NEED_ACK,
						       seqno);
	return ret;
}

static void _xe_eudebug_client_vm_bind(struct xe_eudebug_client *c, int fd,
				       uint32_t vm, uint32_t bo,
				       uint64_t offset, uint64_t addr, uint64_t size,
				       uint32_t op,
				       uint32_t flags,
				       struct drm_xe_sync *sync,
				       uint32_t num_syncs,
				       uint64_t op_ext)
{
	const uint32_t exec_queue_id = 0;
	const uint32_t prefetch_region = 0;

	igt_assert_eq(__xe_eudebug_client_vm_bind(c, fd, vm, exec_queue_id, bo, offset,
						  addr, size, op, flags,
						  sync, num_syncs, prefetch_region,
						  DEFAULT_PAT_INDEX, op_ext),
		      0);
}

/**
 * xe_eudebug_client_vm_bind_flags
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @vm: vm handle
 * @bo: buffer object handle
 * @offset: offset within buffer object
 * @addr: ppgtt address
 * @size: size of the binding
 * @flags: vm_bind flags
 * @sync: sync objects
 * @num_syncs: number of sync objects
 * @op_ext: BIND_OP extensions
 *
 * Calls xe vm_bind ioctl and logs the corresponding event in client's event log.
 */
void xe_eudebug_client_vm_bind_flags(struct xe_eudebug_client *c, int fd, uint32_t vm,
				     uint32_t bo, uint64_t offset,
				     uint64_t addr, uint64_t size, uint32_t flags,
				     struct drm_xe_sync *sync, uint32_t num_syncs,
				     uint64_t op_ext)
{
	igt_assert(c);
	_xe_eudebug_client_vm_bind(c, fd, vm, bo, offset, addr, size,
				   DRM_XE_VM_BIND_OP_MAP, flags,
				   sync, num_syncs, op_ext);
}

/**
 * xe_eudebug_client_vm_bind
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @vm: vm handle
 * @bo: buffer object handle
 * @offset: offset within buffer object
 * @addr: ppgtt address
 * @size: size of the binding
 *
 * Calls xe vm_bind ioctl and logs the corresponding event in client's event log.
 */
void xe_eudebug_client_vm_bind(struct xe_eudebug_client *c, int fd, uint32_t vm,
			       uint32_t bo, uint64_t offset,
			       uint64_t addr, uint64_t size)
{
	const uint32_t flags = 0;
	struct drm_xe_sync *sync = NULL;
	const uint32_t num_syncs = 0;
	const uint64_t op_ext = 0;

	xe_eudebug_client_vm_bind_flags(c, fd, vm, bo, offset, addr, size, flags, sync, num_syncs,
					op_ext);
}

/**
 * xe_eudebug_client_vm_unbind_flags
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @vm: vm handle
 * @offset: offset
 * @addr: ppgtt address
 * @size: size of the binding
 * @flags: vm_bind flags
 * @sync: sync objects
 * @num_syncs: number of sync objects
 *
 * Calls xe vm_unbind ioctl and logs the corresponding event in client's event log.
 */
void xe_eudebug_client_vm_unbind_flags(struct xe_eudebug_client *c, int fd,
				       uint32_t vm, uint64_t offset,
				       uint64_t addr, uint64_t size, uint32_t flags,
				       struct drm_xe_sync *sync, uint32_t num_syncs)
{
	igt_assert(c);
	_xe_eudebug_client_vm_bind(c, fd, vm, 0, offset, addr, size,
				   DRM_XE_VM_BIND_OP_UNMAP, flags,
				   sync, num_syncs, 0);
}

/**
 * xe_eudebug_client_vm_unbind
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @vm: vm handle
 * @offset: offset
 * @addr: ppgtt address
 * @size: size of the binding
 *
 * Calls xe vm_unbind ioctl and logs the corresponding event in client's event log.
 */
void xe_eudebug_client_vm_unbind(struct xe_eudebug_client *c, int fd, uint32_t vm,
				 uint64_t offset, uint64_t addr, uint64_t size)
{
	const uint32_t flags = 0;
	struct drm_xe_sync *sync = NULL;
	const uint32_t num_syncs = 0;

	xe_eudebug_client_vm_unbind_flags(c, fd, vm, offset, addr, size,
					  flags, sync, num_syncs);
}

/**
 * xe_eudebug_client_metadata_create:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @type: debug metadata type
 * @len: size of @data
 * @data: debug metadata paylad
 *
 * Calls xe metadata create ioctl and logs the corresponding event in
 * client's event log.
 *
 * Return: valid debug metadata id.
 */
uint32_t xe_eudebug_client_metadata_create(struct xe_eudebug_client *c, int fd,
					   int type, size_t len, void *data)
{
	struct drm_xe_debug_metadata_create create = {
		.type = type,
		.user_addr = to_user_pointer(data),
		.len = len
	};

	igt_assert(c);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEBUG_METADATA_CREATE, &create), 0);

	metadata_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, fd, create.metadata_id, type, len);

	return create.metadata_id;
}

/**
 * xe_eudebug_client_metadata_destroy:
 * @c: pointer to xe_eudebug_client structure
 * @fd: xe client
 * @id: xe debug metadata handle
 * @type: debug metadata type
 * @len: size of debug metadata payload
 *
 * Calls xe metadata destroy ioctl and logs the corresponding event in
 * client's event log.
 */
void xe_eudebug_client_metadata_destroy(struct xe_eudebug_client *c, int fd,
					uint32_t id, int type, size_t len)
{
	struct drm_xe_debug_metadata_destroy destroy = { .metadata_id = id };

	igt_assert(c);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEBUG_METADATA_DESTROY, &destroy), 0);

	metadata_event(c, DRM_XE_EUDEBUG_EVENT_DESTROY, fd, id, type, len);
}

void xe_eudebug_ack_ufence(int debugfd,
			   const struct drm_xe_eudebug_event_vm_bind_ufence *f)
{
	struct drm_xe_eudebug_ack_event ack = { 0, };
	char event_str[XE_EUDEBUG_EVENT_STRING_MAX_LEN];

	igt_assert(f);

	ack.type = f->base.type;
	ack.seqno = f->base.seqno;

	xe_eudebug_event_to_str((void *)f, event_str, XE_EUDEBUG_EVENT_STRING_MAX_LEN);
	igt_debug("delivering ack for event: %s\n", event_str);
	igt_assert_eq(igt_ioctl(debugfd, DRM_XE_EUDEBUG_IOCTL_ACK_EVENT, &ack), 0);
}
