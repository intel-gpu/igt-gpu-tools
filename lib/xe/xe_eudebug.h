/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <xe_drm.h>
#include <xe_drm_eudebug.h>

#include "igt_core.h"
#include "igt_list.h"

struct xe_eudebug_event_log {
	uint8_t *log;
	unsigned int head;
	unsigned int max_size;
	char name[80];
	pthread_mutex_t lock;
};

enum xe_eudebug_debugger_worker_state {
	DEBUGGER_WORKER_INACTIVE = 0,
	DEBUGGER_WORKER_ACTIVE,
	DEBUGGER_WORKER_QUITTING,
};

struct xe_eudebug_debugger {
	int fd;
	uint64_t flags;

	/* Used to smuggle private data */
	void *ptr;

	struct xe_eudebug_event_log *log;

	uint64_t event_count;

	uint64_t target_pid;

	struct igt_list_head triggers;

	int master_fd;

	pthread_t worker_thread;
	enum xe_eudebug_debugger_worker_state worker_state;

	bool received_sigint;
	bool handled_sigint;
	bool received_signal;

	int p_client[2];
};

struct xe_eudebug_client {
	int pid;
	uint64_t seqno;
	uint64_t flags;

	/* Used to smuggle private data */
	void *ptr;

	struct xe_eudebug_event_log *log;

	int done;
	int p_in[2];
	int p_out[2];

	/* Used to pickup right device (the one used in debugger) */
	int master_fd;

	int timeout_ms;

	bool allow_dead_client;

	pthread_mutex_t lock;
};

struct xe_eudebug_session {
	uint64_t flags;
	struct xe_eudebug_client *client;
	struct xe_eudebug_debugger *debugger;
};

typedef void (*xe_eudebug_client_work_fn)(struct xe_eudebug_client *);
typedef void (*xe_eudebug_trigger_fn)(struct xe_eudebug_debugger *,
				      struct drm_xe_eudebug_event *);

#define xe_eudebug_for_each_engine(fd__, hwe__) \
	xe_for_each_engine(fd__, hwe__) \
		if (hwe__->engine_class == DRM_XE_ENGINE_CLASS_RENDER || \
		    hwe__->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE)

#define MAX_EVENT_SIZE (32 * 1024)

static inline struct drm_xe_eudebug_event *
next_event(struct drm_xe_eudebug_event *e, struct xe_eudebug_event_log *l)
{
	uint8_t *start;
	uint8_t *end;

	igt_assert(l);
	igt_assert(l->log);
	igt_assert(l->max_size);
	igt_assert(l->head <= l->max_size);

	if (!l->head)
		return NULL;

	if (!e)
		return (struct drm_xe_eudebug_event *)l->log;

	start = (uint8_t *)e;

	if (start == l->log + l->head)
		return NULL;

	igt_assert(start >= l->log);
	igt_assert(start < l->log + l->head);
	igt_assert(e->len);
	igt_assert(e->len <= MAX_EVENT_SIZE);

	end = (uint8_t *)e + e->len;

	if (end == l->log + l->head)
		return NULL;

	igt_assert(end < l->log + l->head);

	return (struct drm_xe_eudebug_event *)end;
}

#define xe_eudebug_for_each_event(_e, _log)	\
    for ((_e) = next_event((_e), (_log)); \
         (_e); \
         (_e) = next_event((_e), (_log)))

#define xe_eudebug_assert(d, c)						\
	do {								\
		if (!(c)) {						\
			xe_eudebug_event_log_print((d)->log, true);	\
			igt_assert(c);					\
		}							\
	} while (0)

#define xe_eudebug_assert_f(d, c, f...)					\
	do {								\
		if (!(c)) {						\
			xe_eudebug_event_log_print((d)->log, true);	\
			igt_assert_f(c, f);				\
		}							\
	} while (0)

#define XE_EUDEBUG_EVENT_STRING_MAX_LEN		4096

/*
 * Default abort timeout to use across xe_eudebug lib and tests if no specific
 * timeout value is required.
 */
#define XE_EUDEBUG_DEFAULT_TIMEOUT_SEC		60ULL

#define XE_EUDEBUG_FILTER_EVENT_NONE			BIT(DRM_XE_EUDEBUG_EVENT_NONE)
#define XE_EUDEBUG_FILTER_EVENT_READ			BIT(DRM_XE_EUDEBUG_EVENT_READ)
#define XE_EUDEBUG_FILTER_EVENT_OPEN			BIT(DRM_XE_EUDEBUG_EVENT_OPEN)
#define XE_EUDEBUG_FILTER_EVENT_VM			BIT(DRM_XE_EUDEBUG_EVENT_VM)
#define XE_EUDEBUG_FILTER_EVENT_EXEC_QUEUE		BIT(DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE)
#define XE_EUDEBUG_FILTER_EVENT_EXEC_QUEUE_PLACEMENTS	BIT(DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE_PLACEMENTS)
#define XE_EUDEBUG_FILTER_EVENT_EU_ATTENTION		BIT(DRM_XE_EUDEBUG_EVENT_EU_ATTENTION)
#define XE_EUDEBUG_FILTER_EVENT_VM_BIND			BIT(DRM_XE_EUDEBUG_EVENT_VM_BIND)
#define XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP		BIT(DRM_XE_EUDEBUG_EVENT_VM_BIND_OP)
#define XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE		BIT(DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE)
#define XE_EUDEBUG_FILTER_EVENT_METADATA		BIT(DRM_XE_EUDEBUG_EVENT_METADATA)
#define XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP_METADATA	BIT(DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA)
#define XE_EUDEBUG_FILTER_EVENT_PAGEFAULT		BIT(DRM_XE_EUDEBUG_EVENT_PAGEFAULT)
#define XE_EUDEBUG_FILTER_ALL				GENMASK(DRM_XE_EUDEBUG_EVENT_PAGEFAULT, 0)
#define XE_EUDEBUG_EVENT_IS_FILTERED(_e, _f)		((1UL << (_e)) & (_f))

const char *xe_eudebug_event_to_str(struct drm_xe_eudebug_event *e, char *buf, size_t len);
struct drm_xe_eudebug_event *
xe_eudebug_event_log_find_seqno(struct xe_eudebug_event_log *l, uint64_t seqno);
struct xe_eudebug_event_log *
xe_eudebug_event_log_create(const char *name, unsigned int max_size);
void xe_eudebug_event_log_destroy(struct xe_eudebug_event_log *l);
void xe_eudebug_event_log_print(struct xe_eudebug_event_log *l, bool debug);
void xe_eudebug_event_log_compare(struct xe_eudebug_event_log *c, struct xe_eudebug_event_log *d,
				  uint32_t filter);
void xe_eudebug_event_log_write(struct xe_eudebug_event_log *l, struct drm_xe_eudebug_event *e);
void xe_eudebug_event_log_match_opposite(struct xe_eudebug_event_log *l, uint32_t filter);

bool xe_eudebug_debugger_available(int fd);
struct xe_eudebug_debugger *
xe_eudebug_debugger_create(int xe, uint64_t flags, void *data);
void xe_eudebug_debugger_destroy(struct xe_eudebug_debugger *d);
int xe_eudebug_debugger_attach(struct xe_eudebug_debugger *d, struct xe_eudebug_client *c);
int xe_eudebug_debugger_reattach(struct xe_eudebug_debugger *d, pid_t pid);
void xe_eudebug_debugger_start_worker(struct xe_eudebug_debugger *d);
void xe_eudebug_debugger_stop_worker(struct xe_eudebug_debugger *d);
void xe_eudebug_debugger_detach(struct xe_eudebug_debugger *d);
void xe_eudebug_debugger_set_data(struct xe_eudebug_debugger *c, void *ptr);
void xe_eudebug_debugger_add_trigger(struct xe_eudebug_debugger *d, int type,
				     xe_eudebug_trigger_fn fn);
void xe_eudebug_debugger_remove_trigger(struct xe_eudebug_debugger *d, int type,
				     xe_eudebug_trigger_fn fn);
void xe_eudebug_debugger_signal_stage(struct xe_eudebug_debugger *d, uint64_t stage);
void xe_eudebug_debugger_wait_stage(struct xe_eudebug_session *s, uint64_t stage);
void xe_eudebug_debugger_kill(struct xe_eudebug_debugger *d, int sig);

struct xe_eudebug_client *
xe_eudebug_client_create(int xe, xe_eudebug_client_work_fn work, uint64_t flags, void *data);
void xe_eudebug_client_destroy(struct xe_eudebug_client *c);
void xe_eudebug_client_start(struct xe_eudebug_client *c);
void xe_eudebug_client_stop(struct xe_eudebug_client *c);
void xe_eudebug_client_wait_done(struct xe_eudebug_client *c);
void xe_eudebug_client_signal_stage(struct xe_eudebug_client *c, uint64_t stage);
void xe_eudebug_client_wait_stage(struct xe_eudebug_client *c, uint64_t stage);

uint64_t xe_eudebug_client_get_seqno(struct xe_eudebug_client *c);
void xe_eudebug_client_set_data(struct xe_eudebug_client *c, void *ptr);

int __xe_eudebug_enable_getset(int fd, bool *old, bool *new);
bool xe_eudebug_enable(int fd, bool enable);

int xe_eudebug_client_open_driver(struct xe_eudebug_client *c);
void xe_eudebug_client_close_driver(struct xe_eudebug_client *c, int fd);
uint32_t xe_eudebug_client_vm_create(struct xe_eudebug_client *c, int fd,
				     uint32_t flags, uint64_t ext);
void xe_eudebug_client_vm_destroy(struct xe_eudebug_client *c, int fd, uint32_t vm);
uint32_t xe_eudebug_client_exec_queue_create(struct xe_eudebug_client *c, int fd,
					     struct drm_xe_exec_queue_create *create);
void xe_eudebug_client_exec_queue_destroy(struct xe_eudebug_client *c, int fd,
					  struct drm_xe_exec_queue_create *create);
void xe_eudebug_client_vm_bind_event(struct xe_eudebug_client *c, uint32_t event_flags, int fd,
				     uint32_t vm, uint32_t bind_flags,
				     uint32_t num_ops, uint64_t *ref_seqno);
void xe_eudebug_client_vm_bind_op_event(struct xe_eudebug_client *c, uint32_t event_flags,
					uint64_t ref_seqno, uint64_t *op_ref_seqno,
					uint64_t addr, uint64_t range,
					uint64_t num_extensions);
void xe_eudebug_client_vm_bind_op_metadata_event(struct xe_eudebug_client *c,
						 uint32_t event_flags, uint64_t op_ref_seqno,
						 uint64_t metadata_handle, uint64_t metadata_cookie);
void xe_eudebug_client_vm_bind_ufence_event(struct xe_eudebug_client *c, uint32_t event_flags,
					    uint64_t ref_seqno);
void xe_eudebug_ack_ufence(int debugfd,
			   const struct drm_xe_eudebug_event_vm_bind_ufence *f);

void xe_eudebug_client_vm_bind_flags(struct xe_eudebug_client *c, int fd, uint32_t vm,
				     uint32_t bo, uint64_t offset,
				     uint64_t addr, uint64_t size, uint32_t flags,
				     struct drm_xe_sync *sync, uint32_t num_syncs,
				     uint64_t op_ext);
void xe_eudebug_client_vm_bind(struct xe_eudebug_client *c, int fd, uint32_t vm,
			       uint32_t bo, uint64_t offset,
			       uint64_t addr, uint64_t size);
void xe_eudebug_client_vm_unbind_flags(struct xe_eudebug_client *c, int fd,
				       uint32_t vm, uint64_t offset,
				       uint64_t addr, uint64_t size, uint32_t flags,
				       struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_eudebug_client_vm_unbind(struct xe_eudebug_client *c, int fd, uint32_t vm,
				 uint64_t offset, uint64_t addr, uint64_t size);

uint32_t xe_eudebug_client_metadata_create(struct xe_eudebug_client *c, int fd,
					   int type, size_t len, void *data);
void xe_eudebug_client_metadata_destroy(struct xe_eudebug_client *c, int fd,
					uint32_t id, int type, size_t len);

struct xe_eudebug_session *xe_eudebug_session_create(int fd,
						     xe_eudebug_client_work_fn work,
						     unsigned int flags,
						     void *test_private);
void xe_eudebug_session_destroy(struct xe_eudebug_session *s);
void xe_eudebug_session_run(struct xe_eudebug_session *s);
void xe_eudebug_session_check(struct xe_eudebug_session *s, bool match_opposite, uint32_t filter);
