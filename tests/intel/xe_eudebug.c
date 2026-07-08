// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Test EU Debugger functionality
 * Category: Core
 * Mega feature: EUdebug
 * Sub-category: EUdebug framework
 * Functionality: eu debugger framework
 * Test category: functionality test
 */

#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include "igt.h"
#include "igt_sysfs.h"
#include "intel_pat.h"
#include "lib/igt_syncobj.h"
#include "xe/xe_eudebug.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * SUBTEST: sysfs-toggle
 * Functionality: enable
 * Description:
 *	Exercise the debugger enable/disable sysfs toggle logic
 */
static void test_sysfs_toggle(int fd)
{
	xe_eudebug_enable(fd, false);
	igt_assert(!xe_eudebug_debugger_available(fd));

	xe_eudebug_enable(fd, true);
	igt_assert(xe_eudebug_debugger_available(fd));
	xe_eudebug_enable(fd, true);
	igt_assert(xe_eudebug_debugger_available(fd));

	xe_eudebug_enable(fd, false);
	igt_assert(!xe_eudebug_debugger_available(fd));
	xe_eudebug_enable(fd, false);
	igt_assert(!xe_eudebug_debugger_available(fd));

	xe_eudebug_enable(fd, true);
	igt_assert(xe_eudebug_debugger_available(fd));
}

#define STAGE_PRE_DEBUG_RESOURCES_DONE	1
#define STAGE_DISCOVERY_DONE		2

#define CREATE_VMS			(1 << 0)
#define CREATE_EXEC_QUEUES		(1 << 1)
#define VM_BIND				(1 << 2)
#define VM_BIND_VM_DESTROY		(1 << 3)
#define VM_BIND_EXTENDED		(1 << 4)
#define VM_METADATA			(1 << 5)
#define VM_BIND_METADATA		(1 << 6)
#define VM_BIND_OP_MAP_USERPTR		(1 << 7)
#define VM_BIND_DELAY_UFENCE_ACK	(1 << 8)
#define VM_BIND_UFENCE_RECONNECT	(1 << 9)
#define VM_BIND_UFENCE_SIGINT_CLIENT	(1 << 10)
#define TEST_FAULTABLE			(1 << 30)
#define TEST_DISCOVERY			(1 << 31)

#define PAGE_SIZE SZ_4K
#define MDATA_SIZE (WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM * PAGE_SIZE)

#define BO_ADDR 0x1a0000

static struct drm_xe_vm_bind_op_ext_attach_debug *
basic_vm_bind_metadata_ext_prepare(int fd, struct xe_eudebug_client *c, uint8_t **data)
{
	const uint32_t data_size = MDATA_SIZE;
	struct drm_xe_vm_bind_op_ext_attach_debug *ext;
	int i;

	*data = calloc(data_size, sizeof(**data));
	igt_assert(*data);

	for (i = 0; i < data_size; i++)
		(*data)[i] = (i + i / PAGE_SIZE) % 256;

	ext = calloc(WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM, sizeof(*ext));
	igt_assert(ext);

	for (i = 0; i < WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM; i++) {
		ext[i].base.name = XE_VM_BIND_OP_EXTENSIONS_ATTACH_DEBUG;
		ext[i].metadata_id = xe_eudebug_client_metadata_create(c, fd, i,
								       (i + 1) * PAGE_SIZE, *data);
		ext[i].cookie = i;

		if (i < WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM - 1)
			ext[i].base.next_extension = to_user_pointer(&ext[i + 1]);
	}
	return ext;
}

static void basic_vm_bind_metadata_ext_del(int fd, struct xe_eudebug_client *c,
					   struct drm_xe_vm_bind_op_ext_attach_debug *ext,
					   uint8_t *data)
{
	for (int i = 0; i < WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM; i++)
		xe_eudebug_client_metadata_destroy(c, fd, ext[i].metadata_id, i,
						   (i + 1) * PAGE_SIZE);
	free(ext);
	free(data);
}

static void basic_vm_bind_client(int fd, struct xe_eudebug_client *c)
{
	struct drm_xe_vm_bind_op_ext_attach_debug *ext = NULL;
	uint32_t vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	size_t bo_size = xe_get_default_alignment(fd);
	bool test_discovery = c->flags & TEST_DISCOVERY;
	bool test_metadata = c->flags & VM_BIND_METADATA;
	uint32_t bo = xe_bo_create(fd, 0, bo_size,
				   system_memory(fd), 0);
	uint64_t addr = 0x1a0000;
	uint8_t *data = NULL;

	if (test_metadata)
		ext = basic_vm_bind_metadata_ext_prepare(fd, c, &data);

	xe_eudebug_client_vm_bind_flags(c, fd, vm, bo, 0, addr,
					bo_size, 0, NULL, 0, to_user_pointer(ext));

	if (test_discovery) {
		xe_eudebug_client_signal_stage(c, STAGE_PRE_DEBUG_RESOURCES_DONE);
		xe_eudebug_client_wait_stage(c, STAGE_DISCOVERY_DONE);
	}

	xe_eudebug_client_vm_unbind(c, fd, vm, 0, addr, bo_size);

	if (test_metadata)
		basic_vm_bind_metadata_ext_del(fd, c, ext, data);

	gem_close(fd, bo);
	xe_eudebug_client_vm_destroy(c, fd, vm);
}

static void basic_vm_bind_vm_destroy_client(int fd, struct xe_eudebug_client *c)
{
	uint32_t vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	size_t bo_size = xe_get_default_alignment(fd);
	bool test_discovery = c->flags & TEST_DISCOVERY;
	uint32_t bo = xe_bo_create(fd, 0, bo_size,
				   system_memory(fd), 0);
	uint64_t addr = 0x1a0000;

	if (test_discovery) {
		vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);

		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, NULL, 0);

		xe_vm_destroy(fd, vm);

		xe_eudebug_client_signal_stage(c, STAGE_PRE_DEBUG_RESOURCES_DONE);
		xe_eudebug_client_wait_stage(c, STAGE_DISCOVERY_DONE);
	} else {
		vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
		xe_eudebug_client_vm_bind(c, fd, vm, bo, 0, addr, bo_size);
		xe_eudebug_client_vm_destroy(c, fd, vm);
	}

	gem_close(fd, bo);
}

#define BO_ITEMS 4096
#define MIN_BO_SIZE (BO_ITEMS * sizeof(uint64_t))

union buf_id {
	uint32_t fd;
	void *userptr;
};

struct bind_list {
	int fd;
	uint32_t vm;
	union buf_id *bo;
	struct drm_xe_vm_bind_op *bind_ops;
	unsigned int n;
};

static void *bo_get_ptr(int fd, struct drm_xe_vm_bind_op *o)
{
	void *ptr;

	if (o->op != DRM_XE_VM_BIND_OP_MAP_USERPTR)
		ptr = xe_bo_map(fd, o->obj, o->range);
	else
		ptr = (void *)(uintptr_t)o->userptr;

	igt_assert(ptr);

	return ptr;
}

static void bo_put_ptr(int fd, struct drm_xe_vm_bind_op *o, void *ptr)
{
	if (o->op != DRM_XE_VM_BIND_OP_MAP_USERPTR)
		munmap(ptr, o->range);
}

static void bo_prime(int fd, struct drm_xe_vm_bind_op *o)
{
	uint64_t *d;
	uint64_t i;

	d = bo_get_ptr(fd, o);

	for (i = 0; i < o->range / sizeof(*d); i++)
		d[i] = o->addr + i;

	bo_put_ptr(fd, o, d);
}

static void bo_check(int fd, struct drm_xe_vm_bind_op *o)
{
	uint64_t *d;
	uint64_t i;

	d = bo_get_ptr(fd, o);

	for (i = 0; i < o->range / sizeof(*d); i++)
		igt_assert_eq(d[i], o->addr + i + 1);

	bo_put_ptr(fd, o, d);
}

static union buf_id *vm_create_objects(int fd, uint32_t bo_placement, uint32_t vm,
				       unsigned int size, unsigned int n)
{
	union buf_id *bo;
	unsigned int i;

	bo = calloc(n, sizeof(*bo));
	igt_assert(bo);

	for (i = 0; i < n; i++) {
		if (bo_placement) {
			bo[i].fd = xe_bo_create(fd, vm, size, bo_placement,
						DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			igt_assert(bo[i].fd);
		} else {
			bo[i].userptr = aligned_alloc(PAGE_SIZE, size);
			igt_assert(bo[i].userptr);
		}
	}

	return bo;
}

static struct bind_list *create_bind_list(int fd, uint32_t bo_placement,
					  uint32_t vm, unsigned int n,
					  unsigned int target_size)
{
	unsigned int i = target_size ?: MIN_BO_SIZE;
	const unsigned int bo_size = max_t(bo_size, xe_get_default_alignment(fd), i);
	bool is_userptr = !bo_placement;
	struct bind_list *bl;

	bl = malloc(sizeof(*bl));
	bl->fd = fd;
	bl->vm = vm;
	bl->bo = vm_create_objects(fd, bo_placement, vm, bo_size, n);
	bl->n = n;
	bl->bind_ops = calloc(n, sizeof(*bl->bind_ops));
	igt_assert(bl->bind_ops);

	for (i = 0; i < n; i++) {
		struct drm_xe_vm_bind_op *o = &bl->bind_ops[i];

		if (is_userptr) {
			o->userptr = (uintptr_t)bl->bo[i].userptr;
			o->op = DRM_XE_VM_BIND_OP_MAP_USERPTR;
		} else {
			o->obj = bl->bo[i].fd;
			o->op = DRM_XE_VM_BIND_OP_MAP;
		}

		o->range = bo_size;
		o->addr = BO_ADDR + 2 * i * bo_size;
		o->pat_index = intel_get_pat_idx_wb(fd);
	}

	for (i = 0; i < bl->n; i++) {
		struct drm_xe_vm_bind_op *o = &bl->bind_ops[i];

		igt_debug("bo %d: addr 0x%llx, range 0x%llx\n", i, o->addr, o->range);
		bo_prime(fd, o);
	}

	return bl;
}

static void do_bind_list(struct xe_eudebug_client *c,
			 struct bind_list *bl, bool sync)
{
	struct drm_xe_sync uf_sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = 1337,
	};
	uint64_t ref_seqno = 0, op_ref_seqno = 0;
	uint64_t *fence_data;
	int i;

	if (sync) {
		fence_data = aligned_alloc(xe_get_default_alignment(bl->fd),
					   sizeof(*fence_data));
		igt_assert(fence_data);
		uf_sync.addr = to_user_pointer(fence_data);
		memset(fence_data, 0, sizeof(*fence_data));
	}

	xe_vm_bind_array(bl->fd, bl->vm, 0, bl->bind_ops, bl->n, &uf_sync, sync ? 1 : 0);
	xe_eudebug_client_vm_bind_event(c, DRM_XE_EUDEBUG_EVENT_STATE_CHANGE,
					bl->fd, bl->vm, 0, bl->n, &ref_seqno);
	for (i = 0; i < bl->n; i++)
		xe_eudebug_client_vm_bind_op_event(c, DRM_XE_EUDEBUG_EVENT_CREATE,
						   ref_seqno,
						   &op_ref_seqno,
						   bl->bind_ops[i].addr,
						   bl->bind_ops[i].range,
						   0);

	if (sync) {
		xe_wait_ufence(bl->fd, fence_data, uf_sync.timeline_value, 0,
			       XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC);
		free(fence_data);
	}
}

static void free_bind_list(struct xe_eudebug_client *c, struct bind_list *bl)
{
	unsigned int i;

	for (i = 0; i < bl->n; i++) {
		igt_debug("%d: checking 0x%llx (%lld)\n",
			  i, bl->bind_ops[i].addr, bl->bind_ops[i].addr);
		bo_check(bl->fd, &bl->bind_ops[i]);
		if (bl->bind_ops[i].op == DRM_XE_VM_BIND_OP_MAP_USERPTR)
			free(bl->bo[i].userptr);
		xe_eudebug_client_vm_unbind(c, bl->fd, bl->vm, 0,
					    bl->bind_ops[i].addr,
					    bl->bind_ops[i].range);
	}

	free(bl->bind_ops);
	free(bl->bo);
	free(bl);
}

static void vm_bind_client(int fd, struct xe_eudebug_client *c)
{
	uint64_t op_ref_seqno, ref_seqno;
	struct bind_list *bl;
	bool test_discovery = c->flags & TEST_DISCOVERY;
	size_t bo_size = 3 * xe_get_default_alignment(fd);
	uint32_t bo[2] = {
		xe_bo_create(fd, 0, bo_size, system_memory(fd), 0),
		xe_bo_create(fd, 0, bo_size, system_memory(fd), 0),
	};
	uint32_t vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	uint64_t addr[] = {0x2a0000, 0x3a0000};
	uint64_t rebind_bo_offset = 2 * bo_size / 3;
	uint64_t size = bo_size / 3;
	int i = 0;

	if (test_discovery) {
		xe_vm_bind_async(fd, vm, 0, bo[0], 0, addr[0], bo_size, NULL, 0);

		xe_vm_unbind_async(fd, vm, 0, 0, addr[0] + size, size, NULL, 0);

		xe_vm_bind_async(fd, vm, 0, bo[1], 0, addr[1], bo_size, NULL, 0);

		xe_vm_bind_async(fd, vm, 0, bo[1], rebind_bo_offset, addr[1], size, NULL, 0);

		bl = create_bind_list(fd, system_memory(fd), vm, 4, 0);
		xe_vm_bind_array(bl->fd, bl->vm, 0, bl->bind_ops, bl->n, NULL, 0);

		xe_vm_unbind_all_async(fd, vm, 0, bo[0], NULL, 0);

		xe_eudebug_client_vm_bind_event(c, DRM_XE_EUDEBUG_EVENT_STATE_CHANGE,
						bl->fd, bl->vm, 0, bl->n + 2, &ref_seqno);

		xe_eudebug_client_vm_bind_op_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, ref_seqno,
						   &op_ref_seqno, addr[1], size, 0);
		xe_eudebug_client_vm_bind_op_event(c, DRM_XE_EUDEBUG_EVENT_CREATE, ref_seqno,
						   &op_ref_seqno, addr[1] + size, size * 2, 0);

		for (i = 0; i < bl->n; i++)
			xe_eudebug_client_vm_bind_op_event(c, DRM_XE_EUDEBUG_EVENT_CREATE,
							   ref_seqno, &op_ref_seqno,
							   bl->bind_ops[i].addr,
							   bl->bind_ops[i].range, 0);

		xe_eudebug_client_signal_stage(c, STAGE_PRE_DEBUG_RESOURCES_DONE);
		xe_eudebug_client_wait_stage(c, STAGE_DISCOVERY_DONE);
	} else {
		xe_eudebug_client_vm_bind(c, fd, vm, bo[0], 0, addr[0], bo_size);
		xe_eudebug_client_vm_unbind(c, fd, vm, 0, addr[0] + size, size);

		xe_eudebug_client_vm_bind(c, fd, vm, bo[1], 0, addr[1], bo_size);
		xe_eudebug_client_vm_bind(c, fd, vm, bo[1], rebind_bo_offset, addr[1], size);

		bl = create_bind_list(fd, system_memory(fd), vm, 4, 0);
		do_bind_list(c, bl, false);
	}

	xe_vm_unbind_all_async(fd, vm, 0, bo[1], NULL, 0);

	xe_eudebug_client_vm_bind_event(c, DRM_XE_EUDEBUG_EVENT_STATE_CHANGE, fd, vm, 0,
					1, &ref_seqno);
	xe_eudebug_client_vm_bind_op_event(c, DRM_XE_EUDEBUG_EVENT_DESTROY, ref_seqno,
					   &op_ref_seqno, 0, 0, 0);

	gem_close(fd, bo[0]);
	gem_close(fd, bo[1]);
	xe_eudebug_client_vm_destroy(c, fd, vm);
}

static void run_basic_client(struct xe_eudebug_client *c)
{
	int fd, i;

	fd = xe_eudebug_client_open_driver(c);

	if (c->flags & CREATE_VMS) {
		const uint32_t flags[] = {
			DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE | DRM_XE_VM_CREATE_FLAG_LR_MODE,
			DRM_XE_VM_CREATE_FLAG_LR_MODE,
		};
		uint32_t vms[ARRAY_SIZE(flags)];

		for (i = 0; i < ARRAY_SIZE(flags); i++)
			vms[i] = xe_eudebug_client_vm_create(c, fd, flags[i], 0);

		for (i--; i >= 0; i--)
			xe_eudebug_client_vm_destroy(c, fd, vms[i]);
	}

	if (c->flags & CREATE_EXEC_QUEUES) {
		struct drm_xe_exec_queue_create *create;
		struct drm_xe_engine_class_instance *hwe;
		struct drm_xe_ext_set_property eq_ext = {
			.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
			.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
			.value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE,
		};
		uint32_t vm;

		create = calloc(xe_number_engines(fd), sizeof(*create));

		vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);

		i = 0;
		xe_eudebug_for_each_engine(fd, hwe) {
			create[i].instances = to_user_pointer(hwe);
			create[i].vm_id = vm;
			create[i].width = 1;
			create[i].num_placements = 1;
			create[i].extensions = to_user_pointer(&eq_ext);
			xe_eudebug_client_exec_queue_create(c, fd, &create[i++]);
		}

		while (--i >= 0)
			xe_eudebug_client_exec_queue_destroy(c, fd, &create[i]);

		xe_eudebug_client_vm_destroy(c, fd, vm);
	}

	if (c->flags & VM_BIND || c->flags & VM_BIND_METADATA)
		basic_vm_bind_client(fd, c);

	if (c->flags & VM_BIND_EXTENDED)
		vm_bind_client(fd, c);

	if (c->flags & VM_BIND_VM_DESTROY)
		basic_vm_bind_vm_destroy_client(fd, c);

	xe_eudebug_client_close_driver(c, fd);
}

static int read_event(int debugfd, struct drm_xe_eudebug_event *event)
{
	int ret;

	ret = igt_ioctl(debugfd, DRM_XE_EUDEBUG_IOCTL_READ_EVENT, event);
	if (ret < 0)
		return -errno;

	return ret;
}

static int __read_event(int debugfd, struct drm_xe_eudebug_event *event)
{
	int ret;

	ret = ioctl(debugfd, DRM_XE_EUDEBUG_IOCTL_READ_EVENT, event);
	if (ret < 0)
		return -errno;

	return ret;
}

static int poll_event(int fd, int timeout_ms)
{
	int ret;

	struct pollfd p = {
		.fd = fd,
		.events = POLLIN,
		.revents = 0,
	};

	ret = poll(&p, 1, timeout_ms);
	if (ret == -1)
		return -errno;

	return ret == 1 && (p.revents & POLLIN);
}

static int __debug_connect(int fd, int *debugfd, struct drm_xe_eudebug_connect *param)
{
	int ret = 0;

	*debugfd = igt_ioctl(fd, DRM_IOCTL_XE_EUDEBUG_CONNECT, param);

	if (*debugfd < 0) {
		ret = -errno;
		igt_assume(ret != 0);
	}

	errno = 0;
	return ret;
}

/**
 * SUBTEST: basic-connect
 * Functionality: attach
 * Description:
 *	Exercise XE_EUDEBUG_CONNECT ioctl with passing
 *	valid and invalid params.
 */
static void test_connect(int fd)
{
	struct drm_xe_eudebug_connect param = {};
	int debugfd, ret;
	pid_t *pid;

	pid = mmap(NULL, sizeof(pid_t), PROT_WRITE,
		   MAP_SHARED | MAP_ANON, -1, 0);

	/* get fresh unrelated pid */
	igt_fork(child, 1)
		*pid = getpid();

	igt_waitchildren();
	param.pid = *pid;
	munmap(pid, sizeof(pid_t));

	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert(debugfd == -1);
	igt_assert_eq(ret, param.pid ? -ENOENT : -EINVAL);

	param.pid = 0;
	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert(debugfd == -1);
	igt_assert_eq(ret, -EINVAL);

	param.pid = getpid();
	param.version = -1;
	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert(debugfd == -1);
	igt_assert_eq(ret, -EINVAL);

	param.version = 0;
	param.flags = ~0;
	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert(debugfd == -1);
	igt_assert_eq(ret, -EINVAL);

	param.flags = 0;
	param.extensions = ~0;
	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert(debugfd == -1);
	igt_assert_eq(ret, -EINVAL);

	param.extensions = 0;
	ret = __debug_connect(fd, &debugfd, &param);
	igt_assert_neq(debugfd, -1);
	igt_assert_eq(ret, 0);

	close(debugfd);
}

static void switch_user(__uid_t uid, __gid_t gid)
{
	struct group *gr;
	__gid_t gr_v;

	/* Users other then root need to belong to video group */
	gr = getgrnam("video");
	igt_assert(gr);

	/* Drop all */
	igt_assert_eq(setgroups(1, &gr->gr_gid), 0);
	igt_assert_eq(setgid(gid), 0);
	igt_assert_eq(setuid(uid), 0);

	igt_assert_eq(getgroups(1, &gr_v), 1);
	igt_assert_eq(gr_v, gr->gr_gid);
	igt_assert_eq(getgid(), gid);
	igt_assert_eq(getuid(), uid);

	igt_assert_eq(prctl(PR_SET_DUMPABLE, 1L), 0);
}

/**
 * SUBTEST: connect-user
 * Functionality: attach
 * Description:
 *	Verify unprivileged XE_EUDEBUG_CONNECT ioctl.
 *	Check:
 *	 - user debugger to user workload connection
 *	 - user debugger to other user workload connection
 *	 - user debugger to privileged workload connection
 */
static void test_connect_user(int fd)
{
	struct drm_xe_eudebug_connect param = {};
	struct passwd *pwd, *pwd2;
	const char *user1 = "lp";
	const char *user2 = "mail";
	int debugfd, ret, i;
	int p1[2], p2[2];
	__uid_t u1, u2;
	__gid_t g1, g2;
	int newfd;
	pid_t pid;

#define NUM_USER_TESTS 4
#define P_APP 0
#define P_GDB 1
	struct conn_user {
		/* u[0] - process uid, u[1] - gdb uid */
		__uid_t u[P_GDB + 1];
		/* g[0] - process gid, g[1] - gdb gid */
		__gid_t g[P_GDB + 1];
		/* Expected fd from open */
		int ret;
		/* Skip this test case */
		int skip;
		const char *desc;
	} test[NUM_USER_TESTS] = {};

	igt_assert(!pipe(p1));
	igt_assert(!pipe(p2));

	pwd = getpwnam(user1);
	igt_require(pwd);
	u1 = pwd->pw_uid;
	g1 = pwd->pw_gid;

	/*
	 * Keep a copy of needed contents as it is a static
	 * memory area and subsequent calls will overwrite
	 * what's in.
	 * However getpwnam() returns NULL if cannot find
	 * user in passwd.
	 */
	setpwent();
	pwd2 = getpwnam(user2);
	if (pwd2) {
		u2 = pwd2->pw_uid;
		g2 = pwd2->pw_gid;
	}

	test[0].skip = !pwd;
	test[0].u[P_GDB] = u1;
	test[0].g[P_GDB] = g1;
	test[0].ret = -EACCES;
	test[0].desc = "User GDB to Root App";

	test[1].skip = !pwd;
	test[1].u[P_APP] = u1;
	test[1].g[P_APP] = g1;
	test[1].u[P_GDB] = u1;
	test[1].g[P_GDB] = g1;
	test[1].ret = 0;
	test[1].desc = "User GDB to User App";

	test[2].skip = !pwd;
	test[2].u[P_APP] = u1;
	test[2].g[P_APP] = g1;
	test[2].ret = 0;
	test[2].desc = "Root GDB to User App";

	test[3].skip = !pwd2;
	test[3].u[P_APP] = u1;
	test[3].g[P_APP] = g1;
	test[3].u[P_GDB] = u2;
	test[3].g[P_GDB] = g2;
	test[3].ret = -EACCES;
	test[3].desc = "User GDB to Other User App";

	if (!pwd2)
		igt_warn("User %s not available in the system. Skipping subtests: %s.\n",
			 user2, test[3].desc);

	for (i = 0; i < NUM_USER_TESTS; i++) {
		if (test[i].skip) {
			igt_debug("Subtest %s skipped\n", test[i].desc);
			continue;
		}
		igt_debug("Executing connection: %s\n", test[i].desc);
		igt_fork(child, 2) {
			if (!child) {
				if (test[i].u[P_APP])
					switch_user(test[i].u[P_APP], test[i].g[P_APP]);

				pid = getpid();
				/* Signal the PID */
				igt_assert(write(p1[1], &pid, sizeof(pid)) == sizeof(pid));
				/* wait with exit */
				igt_assert(read(p2[0], &pid, sizeof(pid)) == sizeof(pid));
			} else {
				if (test[i].u[P_GDB])
					switch_user(test[i].u[P_GDB], test[i].g[P_GDB]);

				igt_assert(read(p1[0], &pid, sizeof(pid)) == sizeof(pid));
				param.pid = pid;

				newfd = drm_open_driver(DRIVER_XE);
				ret = __debug_connect(newfd, &debugfd, &param);

				/* Release the app first */
				igt_assert(write(p2[1], &pid, sizeof(pid)) == sizeof(pid));

				igt_assert_eq(ret, test[i].ret);
				if (!ret)
					close(debugfd);
			}
		}
		igt_waitchildren();
	}
	close(p1[0]);
	close(p1[1]);
	close(p2[0]);
	close(p2[1]);
#undef NUM_USER_TESTS
#undef P_APP
#undef P_GDB
}

/**
 * SUBTEST: basic-close
 * Functionality: attach
 * Description:
 *	Test whether eudebug can be reattached after closure.
 */
static void test_close(int fd)
{
	struct drm_xe_eudebug_connect param = { 0,  };
	int debug_fd1, debug_fd2;
	int fd2;

	param.pid = getpid();

	igt_assert_eq(__debug_connect(fd, &debug_fd1, &param), 0);
	igt_assert(debug_fd1 >= 0);
	igt_assert_eq(__debug_connect(fd, &debug_fd2, &param), -EBUSY);
	igt_assert_eq(debug_fd2, -1);

	close(debug_fd1);
	fd2 = drm_open_driver(DRIVER_XE);

	igt_assert_eq(__debug_connect(fd2, &debug_fd2, &param), 0);
	igt_assert(debug_fd2 >= 0);
	close(fd2);
	close(debug_fd2);
	close(debug_fd1);
}

/**
 * SUBTEST: basic-read-event
 * Functionality: events
 * Description:
 *	Synchronously exercise eu debugger event polling and reading.
 */
#define MAX_EVENT_SIZE (32 * 1024)
static void test_read_event(int fd)
{
	struct drm_xe_eudebug_event *event;
	struct xe_eudebug_debugger *d;
	struct xe_eudebug_client *c;

	event = calloc(1, MAX_EVENT_SIZE);
	igt_assert(event);

	c = xe_eudebug_client_create(fd, run_basic_client, 0, NULL);
	d = xe_eudebug_debugger_create(fd, 0, NULL);

	igt_assert_eq(xe_eudebug_debugger_attach(d, c), 0);
	igt_assert_eq(poll_event(d->fd, 500), 0);

	/* Negative read event - no events to read */
	event->len = MAX_EVENT_SIZE;
	event->type = DRM_XE_EUDEBUG_EVENT_READ;
	igt_assert_eq(read_event(d->fd, event), -ENOENT);

	xe_eudebug_client_start(c);

	/* Positive read event - client create */
	igt_assert_eq(poll_event(d->fd, 500), 1);
	event->type = DRM_XE_EUDEBUG_EVENT_READ;
	igt_assert_eq(read_event(d->fd, event), 0);
	igt_assert_eq(event->type, DRM_XE_EUDEBUG_EVENT_OPEN);
	igt_assert_eq(event->flags, DRM_XE_EUDEBUG_EVENT_CREATE);

	igt_assert_eq(poll_event(d->fd, 500), 1);

	event->flags = 0;

	/* Negative read event - bad event type */
	event->len = MAX_EVENT_SIZE;
	event->type = DRM_XE_EUDEBUG_EVENT_NONE;
	igt_assert_eq(read_event(d->fd, event), -EINVAL);

	event->type = DRM_XE_EUDEBUG_EVENT_READ;

	/* Negative read event - bad event len */
	event->len = 0;
	igt_assert_eq(read_event(d->fd, event), -EINVAL);
	igt_assert_eq(0, event->len);

	/* Negative read event - bad event len */
	event->len = sizeof(*event) - 1;
	igt_assert_eq(read_event(d->fd, event), -EINVAL);

	/* Negative read event - insufficient event len */
	event->len = sizeof(*event);
	igt_assert_eq(read_event(d->fd, event), -EMSGSIZE);
	/* event->len should now contain exact len */
	igt_assert_lt(sizeof(*event), event->len);

	/* Negative read event - insufficient event len */
	event->len = event->len - 1;
	igt_assert_eq(read_event(d->fd, event), -EMSGSIZE);
	/* event->len should now contain exact len */
	igt_assert_lt(sizeof(*event), event->len);

	/* Positive read event - client destroy */
	igt_assert_eq(read_event(d->fd, event), 0);
	igt_assert_eq(event->type, DRM_XE_EUDEBUG_EVENT_OPEN);
	igt_assert_eq(event->flags, DRM_XE_EUDEBUG_EVENT_DESTROY);

	fcntl(d->fd, F_SETFL, fcntl(d->fd, F_GETFL) | O_NONBLOCK);
	igt_assert(fcntl(d->fd, F_GETFL) & O_NONBLOCK);

	/* Negative read event - no events to read in non blocking mode */
	igt_assert_eq(poll_event(d->fd, 500), 0);
	event->len = MAX_EVENT_SIZE;
	event->flags = 0;
	event->type = DRM_XE_EUDEBUG_EVENT_READ;
	igt_assert_eq(__read_event(d->fd, event), -EAGAIN);

	xe_eudebug_client_wait_done(c);
	xe_eudebug_client_stop(c);

	/* Negative read event - no client process in non blocking mode */
	igt_assert_eq(poll_event(d->fd, 500), 0);
	igt_assert_eq(__read_event(d->fd, event), -EAGAIN);

	xe_eudebug_debugger_destroy(d);
	xe_eudebug_client_destroy(c);

	free(event);
}

/**
 * SUBTEST: basic-client
 * Functionality: attach
 * Description:
 *	Attach the debugger to process which opens and closes xe drm client.
 *
 * SUBTEST: basic-client-th
 * Functionality: attach
 * Description:
 *	Create client basic resources (vms) in multiple threads
 *
 * SUBTEST: multiple-sessions
 * Functionality: multisessions
 * Description:
 *	Simultaneously attach many debuggers to many processes.
 *	Each process opens and closes xe drm client and creates few resources.
 *
 * SUBTEST: basic-exec-queues
 * Functionality: exec queues events
 * Description:
 *	Attach the debugger to process which creates and destroys a few exec-queues.
 *
 * SUBTEST: basic-vms
 * Functionality: VM events
 * Description:
 *	Attach the debugger to process which creates and destroys a few vms.
 *
 * SUBTEST: basic-vm-bind
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs synchronous vm bind
 *	and vm unbind.
 *
 * SUBTEST: basic-vm-bind-vm-destroy
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs vm bind, and destroys
 *	the vm without unbinding. Make sure that we don't get unbind events.
 *
 * SUBTEST: basic-vm-bind-extended
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs bind, bind array, rebind,
 *	partial unbind, unbind and unbind all operations.
 *
 * SUBTEST: multigpu-basic-client
 * Mega feature: MultiGPU
 * Functionality: attach multiGPU
 * Description:
 *	Attach the debugger to process which opens and closes xe drm client on all Xe devices.
 *
 * SUBTEST: multigpu-basic-client-many
 * Mega feature: MultiGPU
 * Functionality: attach multiGPU
 * Description:
 *	Simultaneously attach many debuggers to many processes on all Xe devices.
 *	Each process opens and closes xe drm client and creates few resources.
 */

static void test_basic_sessions(int fd, unsigned int flags, int count, bool match_opposite)
{
	struct xe_eudebug_session **s;
	int i;

	s = calloc(count, sizeof(*s));

	igt_assert(s);

	for (i = 0; i < count; i++)
		s[i] = xe_eudebug_session_create(fd, run_basic_client, flags, NULL);

	for (i = 0; i < count; i++)
		xe_eudebug_session_run(s[i]);

	for (i = 0; i < count; i++)
		xe_eudebug_session_check(s[i], match_opposite, 0);

	for (i = 0; i < count; i++)
		xe_eudebug_session_destroy(s[i]);
}

/**
 * SUBTEST: basic-vm-bind-discovery
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs vm-bind before attaching
 *	and check if the discovery process reports it.
 *
 * SUBTEST: basic-vm-bind-metadata-discovery
 * Functionality: VM bind metadata
 * Description:
 *	Attach the debugger to a process that performs vm-bind with metadata attached
 *	before attaching and check if the discovery process reports it.
 *
 * SUBTEST: basic-vm-bind-vm-destroy-discovery
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs vm bind, and destroys
 *	the vm without unbinding before attaching. Make sure that we don't get
 *	any bind/unbind and vm create/destroy events.
 *
 * SUBTEST: basic-vm-bind-extended-discovery
 * Functionality: VM bind event
 * Description:
 *	Attach the debugger to a process that performs bind, bind array, rebind,
 *	partial unbind, and unbind all operations before attaching. Ensure that
 *	we get a only a singe 'VM_BIND' event from the discovery worker.
 */
static void test_basic_discovery(int fd, unsigned int flags, bool match_opposite)
{
	struct xe_eudebug_debugger *d;
	struct xe_eudebug_session *s;
	struct xe_eudebug_client *c;

	s = xe_eudebug_session_create(fd, run_basic_client, flags | TEST_DISCOVERY, NULL);

	c = s->client;
	d = s->debugger;

	xe_eudebug_client_start(c);
	xe_eudebug_debugger_wait_stage(s, STAGE_PRE_DEBUG_RESOURCES_DONE);

	igt_assert_eq(xe_eudebug_debugger_attach(d, c), 0);
	xe_eudebug_debugger_start_worker(d);

	/* give the worker time to do it's job */
	sleep(2);
	xe_eudebug_debugger_signal_stage(d, STAGE_DISCOVERY_DONE);

	xe_eudebug_client_wait_done(c);

	xe_eudebug_debugger_stop_worker(d);

	xe_eudebug_event_log_print(d->log, true);
	xe_eudebug_event_log_print(c->log, true);

	xe_eudebug_session_check(s, match_opposite, 0);
	xe_eudebug_session_destroy(s);
}

#define RESOURCE_COUNT 16
#define PRIMARY_THREAD			(1 << 0)
#define DISCOVERY_CLOSE_CLIENT		(1 << 1)
#define DISCOVERY_DESTROY_RESOURCES	(1 << 2)
#define DISCOVERY_VM_BIND		(1 << 3)
#define DISCOVERY_SIGINT		(1 << 4)
static void run_discovery_client(struct xe_eudebug_client *c)
{
	int fd[RESOURCE_COUNT], i;
	bool skip_sleep = c->flags & (DISCOVERY_DESTROY_RESOURCES | DISCOVERY_CLOSE_CLIENT);
	uint64_t addr = 0x1a0000;

	srand(getpid());

	for (i = 0; i < RESOURCE_COUNT; i++) {
		struct drm_xe_engine_class_instance *hwe = NULL;

		fd[i] = xe_eudebug_client_open_driver(c);

		/* Get first */
		xe_eudebug_for_each_engine(fd[i], hwe)
			break;

		igt_assert(hwe);

		/*
		 * Give the debugger a break in event stream after every
		 * other client, that allows to read discovery and dettach in quiet.
		 */
		if (random() % 2 == 0 && !skip_sleep)
			sleep(1);

		for (int j = 0; j < RESOURCE_COUNT; j++) {
			uint32_t vm = xe_eudebug_client_vm_create(c, fd[i],
								  DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
			struct drm_xe_ext_set_property eq_ext = {
				.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
				.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
				.value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE,
			};
			struct drm_xe_exec_queue_create create = {
				.width = 1,
				.num_placements = 1,
				.vm_id = vm,
				.instances = to_user_pointer(hwe),
				.extensions = to_user_pointer(&eq_ext),
			};
			const unsigned int bo_size = max_t(bo_size,
							   xe_get_default_alignment(fd[i]),
							   MIN_BO_SIZE);
			uint32_t bo = xe_bo_create(fd[i], 0, bo_size, system_memory(fd[i]), 0);

			xe_eudebug_client_exec_queue_create(c, fd[i], &create);

			if (c->flags & DISCOVERY_VM_BIND) {
				xe_eudebug_client_vm_bind(c, fd[i], vm, bo, 0, addr, bo_size);
				addr += bo_size;
			}

			if (c->flags & DISCOVERY_DESTROY_RESOURCES) {
				xe_eudebug_client_exec_queue_destroy(c, fd[i], &create);
				xe_eudebug_client_vm_destroy(c, fd[i], create.vm_id);
				gem_close(fd[i], bo);
			}
		}

		if (c->flags & DISCOVERY_CLOSE_CLIENT)
			xe_eudebug_client_close_driver(c, fd[i]);
	}
}

/**
 * SUBTEST: discovery-%s
 * Functionality: event discovery
 * Description: Race discovery against %arg[1] and the debugger dettach.
 *
 * arg[1]:
 *
 * @race:		resources creation
 * @race-sigint:	resources creation with client interruption
 * @race-vmbind:	vm-bind operations
 * @empty:		resources destruction
 * @empty-clients:	client closure
 */
static void *discovery_race_thread(void *data)
{
	struct {
		uint64_t client_handle;
		int vm_count;
		int exec_queue_count;
		int vm_bind_op_count;
	} clients[RESOURCE_COUNT];
	struct xe_eudebug_session *s = data;
	int expected = RESOURCE_COUNT * (1 + 2 * RESOURCE_COUNT);
	const int tries = 100;
	bool done = false;
	int ret = 0;

	for (int try = 0; try < tries && !done; try++) {
		ret = xe_eudebug_debugger_attach(s->debugger, s->client);

		if (ret == -EBUSY) {
			usleep(100000);
			continue;
		}

		igt_assert_eq(ret, 0);

		if (random() % 2) {
			struct drm_xe_eudebug_event *e = NULL;
			int max_worker_waits = 30;
			int i = -1;

			xe_eudebug_debugger_start_worker(s->debugger);

			if (!s->client->done) {
				/*
				 * Thread can starve for more than one second. Make
				 * sure we get at least one event before stopping.
				 */
				do
					if (s->client->flags & DISCOVERY_SIGINT)
						usleep(100000);
					else
						sleep(1);
				while (!READ_ONCE(s->debugger->event_count) &&
				       --max_worker_waits);

			igt_assert(READ_ONCE(s->debugger->event_count));
			}

			xe_eudebug_debugger_stop_worker(s->debugger);

			igt_debug("Resources discovered: %" PRIu64 "\n", s->debugger->event_count);
			if (!s->client->done) {
				xe_eudebug_for_each_event(e, s->debugger->log) {
					if (e->type == DRM_XE_EUDEBUG_EVENT_OPEN) {
						struct drm_xe_eudebug_event_client *eo = (void *)e;

						if (i >= 0) {
							igt_assert_eq(clients[i].vm_count,
								      RESOURCE_COUNT);

							igt_assert_eq(clients[i].exec_queue_count,
								      RESOURCE_COUNT);

							if (s->client->flags & DISCOVERY_VM_BIND)
								igt_assert_eq(clients[i].vm_bind_op_count,
									      RESOURCE_COUNT);
						}

						igt_assert(++i < RESOURCE_COUNT);
						clients[i].client_handle = eo->client_handle;
						clients[i].vm_count = 0;
						clients[i].exec_queue_count = 0;
						clients[i].vm_bind_op_count = 0;
					}

					if (e->type == DRM_XE_EUDEBUG_EVENT_VM)
						clients[i].vm_count++;

					if (e->type == DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE)
						clients[i].exec_queue_count++;

					if (e->type == DRM_XE_EUDEBUG_EVENT_VM_BIND_OP)
						clients[i].vm_bind_op_count++;
				};

				igt_assert_lte(0, i);
			}
			for (int j = 0; j < i; j++)
				for (int k = 0; k < i; k++) {
					if (k == j)
						continue;

					igt_assert_neq(clients[j].client_handle,
						       clients[k].client_handle);
				}

			if (s->debugger->event_count >= expected)
				done = true;
		}

		xe_eudebug_debugger_detach(s->debugger);
		s->debugger->log->head = 0;
		s->debugger->event_count = 0;
	}

	/* Primary thread must read everything */
	if (s->flags & PRIMARY_THREAD) {
		while ((ret = xe_eudebug_debugger_attach(s->debugger, s->client)) == -EBUSY)
			usleep(100000);

		igt_assert_eq(ret, 0);

		xe_eudebug_debugger_start_worker(s->debugger);
		xe_eudebug_client_wait_done(s->client);

		if (READ_ONCE(s->debugger->event_count) != expected)
			sleep(5);

		xe_eudebug_debugger_stop_worker(s->debugger);
		xe_eudebug_debugger_detach(s->debugger);
	}

	return NULL;
}

static void test_race_discovery(int fd, unsigned int flags, int clients)
{
	const int debuggers_per_client = 3;
	int count = clients * debuggers_per_client;
	struct xe_eudebug_session *sessions, *s;
	struct xe_eudebug_client *c;
	struct timespec t = {};
	pthread_t *threads;
	int i, j;

	sessions = calloc(count, sizeof(*sessions));
	threads = calloc(count, sizeof(*threads));

	for (i = 0; i < clients; i++) {
		c = xe_eudebug_client_create(fd, run_discovery_client, flags, NULL);
		for (j = 0; j < debuggers_per_client; j++) {
			s = &sessions[i * debuggers_per_client + j];
			s->client = c;
			s->debugger = xe_eudebug_debugger_create(fd, flags, NULL);
			s->flags = flags | (!j ? PRIMARY_THREAD : 0);
		}
	}

	for (i = 0; i < count; i++) {
		if (sessions[i].flags & PRIMARY_THREAD)
			xe_eudebug_client_start(sessions[i].client);

		pthread_create(&threads[i], NULL, discovery_race_thread, &sessions[i]);
	}

	if (flags & DISCOVERY_SIGINT) {
		sleep(2);
		sessions[0].client->done = 1;
		kill(sessions[0].client->pid, SIGINT);
	}

	for (i = 0; i < count; i++) {
		igt_assert_eq(clock_gettime(CLOCK_REALTIME, &t), 0);
		t.tv_sec += XE_EUDEBUG_DEFAULT_TIMEOUT_SEC;
		igt_assert_eq(pthread_timedjoin_np(threads[i], NULL, &t), 0);
	}

	for (i = count - 1; i > 0; i--) {
		if (sessions[i].flags & PRIMARY_THREAD) {
			igt_assert_eq(sessions[i].client->seqno - 1,
				      sessions[i].debugger->event_count);

			xe_eudebug_event_log_compare(sessions[0].debugger->log,
						     sessions[i].debugger->log,
						     XE_EUDEBUG_FILTER_EVENT_VM_BIND);

			xe_eudebug_client_destroy(sessions[i].client);
		}
		xe_eudebug_debugger_destroy(sessions[i].debugger);
	}
}

static void *attach_dettach_thread(void *data)
{
	struct xe_eudebug_session *s = data;
	const int tries = 100;
	int ret = 0;

	for (int try = 0; try < tries; try++) {
		ret = xe_eudebug_debugger_attach(s->debugger, s->client);

		if (ret == -EBUSY) {
			usleep(100000);
			continue;
		}

		igt_assert_eq(ret, 0);

		if (random() % 2 == 0) {
			xe_eudebug_debugger_start_worker(s->debugger);
			xe_eudebug_debugger_stop_worker(s->debugger);
		}

		xe_eudebug_debugger_detach(s->debugger);
		s->debugger->log->head = 0;
		s->debugger->event_count = 0;
	}

	return NULL;
}

static void test_empty_discovery(int fd, unsigned int flags, int clients)
{
	struct xe_eudebug_session **s;
	pthread_t *threads;
	int i, expected = flags & DISCOVERY_CLOSE_CLIENT ? 0 : RESOURCE_COUNT;
	struct timespec t = {};

	igt_assert(flags & (DISCOVERY_DESTROY_RESOURCES | DISCOVERY_CLOSE_CLIENT));

	s = calloc(clients, sizeof(struct xe_eudebug_session *));
	threads = calloc(clients, sizeof(*threads));

	for (i = 0; i < clients; i++)
		s[i] = xe_eudebug_session_create(fd, run_discovery_client, flags, NULL);

	for (i = 0; i < clients; i++) {
		xe_eudebug_client_start(s[i]->client);

		pthread_create(&threads[i], NULL, attach_dettach_thread, s[i]);
	}

	for (i = 0; i < clients; i++) {
		igt_assert_eq(clock_gettime(CLOCK_REALTIME, &t), 0);
		t.tv_sec += XE_EUDEBUG_DEFAULT_TIMEOUT_SEC;
		igt_assert_eq(pthread_timedjoin_np(threads[i], NULL, &t), 0);
	}

	for (i = 0; i < clients; i++) {
		xe_eudebug_client_wait_done(s[i]->client);
		igt_assert_eq(xe_eudebug_debugger_attach(s[i]->debugger, s[i]->client), 0);

		xe_eudebug_debugger_start_worker(s[i]->debugger);
		xe_eudebug_debugger_stop_worker(s[i]->debugger);
		xe_eudebug_debugger_detach(s[i]->debugger);

		igt_assert_eq(s[i]->debugger->event_count, expected);

		xe_eudebug_session_destroy(s[i]);
	}
}

static void ufence_ack_trigger(struct xe_eudebug_debugger *d,
			       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE)
		xe_eudebug_ack_ufence(d->fd, ef);
}

typedef void (*client_run_t)(struct xe_eudebug_client *);

static void test_client_with_trigger(int fd, unsigned int flags, int count,
				     client_run_t client_fn, int type,
				     xe_eudebug_trigger_fn trigger_fn,
				     struct drm_xe_engine_class_instance *hwe,
				     bool match_opposite, uint32_t event_filter)
{
	struct xe_eudebug_session **s;
	int i;

	s = calloc(count, sizeof(*s));

	igt_assert(s);

	for (i = 0; i < count; i++)
		s[i] = xe_eudebug_session_create(fd, client_fn, flags, hwe);

	if (trigger_fn)
		for (i = 0; i < count; i++)
			xe_eudebug_debugger_add_trigger(s[i]->debugger, type, trigger_fn);

	for (i = 0; i < count; i++)
		xe_eudebug_debugger_add_trigger(s[i]->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
						ufence_ack_trigger);

	for (i = 0; i < count; i++)
		xe_eudebug_session_run(s[i]);

	for (i = 0; i < count; i++)
		xe_eudebug_session_check(s[i], match_opposite, event_filter);

	for (i = 0; i < count; i++)
		xe_eudebug_session_destroy(s[i]);
}

struct thread_fn_args {
	struct xe_eudebug_client *client;
	int fd;
};

static void *basic_client_th(void *data)
{
	struct thread_fn_args *f = data;
	struct xe_eudebug_client *c = f->client;
	uint32_t *vms;
	int fd, i, num_vms;

	fd = f->fd;
	igt_assert(fd);

	num_vms = 2 + rand() % 16;
	vms = calloc(num_vms, sizeof(*vms));
	igt_assert(vms);
	igt_debug("Create %d client vms\n", num_vms);

	for (i = 0; i < num_vms; i++)
		vms[i] = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);

	for (i = 0; i < num_vms; i++)
		xe_eudebug_client_vm_destroy(c, fd, vms[i]);

	free(vms);

	return NULL;
}

static void run_basic_client_th(struct xe_eudebug_client *c)
{
	struct thread_fn_args *args;
	int i, num_threads, fd;
	pthread_t *threads;

	args = calloc(1, sizeof(*args));
	igt_assert(args);

	num_threads = 2 + random() % 16;
	igt_debug("Run on %d threads\n", num_threads);
	threads = calloc(num_threads, sizeof(*threads));
	igt_assert(threads);

	fd = xe_eudebug_client_open_driver(c);
	args->client = c;
	args->fd = fd;

	for (i = 0; i < num_threads; i++)
		pthread_create(&threads[i], NULL, basic_client_th, args);

	for (i = 0; i < num_threads; i++)
		pthread_join(threads[i], NULL);

	xe_eudebug_client_close_driver(c, fd);
	free(args);
	free(threads);
}

static void test_basic_sessions_th(int fd, unsigned int flags, int num_clients, bool match_opposite)
{
	test_client_with_trigger(fd, flags, num_clients, run_basic_client_th, 0, NULL, NULL,
				 match_opposite, 0);
}

static void vm_access_client(struct xe_eudebug_client *c)
{
	struct drm_xe_engine_class_instance *hwe = c->ptr;
	uint32_t vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	uint32_t bo_placement;
	struct bind_list *bl;
	uint32_t vm;
	int fd, i, j;

	igt_debug("Using %s\n", xe_engine_class_string(hwe->engine_class));

	fd = xe_eudebug_client_open_driver(c);

	vm_flags |= (c->flags & TEST_FAULTABLE) ? DRM_XE_VM_CREATE_FLAG_FAULT_MODE : 0;
	vm = xe_eudebug_client_vm_create(c, fd, vm_flags, 0);

	if (c->flags & VM_BIND_OP_MAP_USERPTR)
		bo_placement = 0;
	else
		bo_placement = vram_if_possible(fd, hwe->gt_id);

	for (j = 0; j < 5; j++) {
		unsigned int target_size = MIN_BO_SIZE * (1 << j);

		bl = create_bind_list(fd, bo_placement, vm, 4, target_size);
		do_bind_list(c, bl, true);

		for (i = 0; i < bl->n; i++)
			xe_eudebug_client_wait_stage(c, bl->bind_ops[i].addr);

		free_bind_list(c, bl);
	}
	xe_eudebug_client_vm_destroy(c, fd, vm);

	xe_eudebug_client_close_driver(c, fd);
}

static void debugger_test_vma(struct xe_eudebug_debugger *d,
			      uint64_t client_handle,
			      uint64_t vm_handle,
			      uint64_t va_start,
			      uint64_t va_length)
{
	struct drm_xe_eudebug_vm_open vo = { 0, };
	uint64_t *v1, *v2;
	uint64_t items = va_length / sizeof(uint64_t);
	uint64_t offset;
	int fd;
	int r, i;

	v1 = malloc(va_length);
	igt_assert(v1);
	v2 = malloc(va_length);
	igt_assert(v2);

	vo.client_handle = client_handle;
	vo.vm_handle = vm_handle;

	fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
	igt_assert_lte(0, fd);

	r = pread(fd, v1, va_length, va_start);
	igt_assert_eq(r, va_length);

	for (i = 0; i < items; i++)
		igt_assert_eq(v1[i], va_start + i);

	/* random unaligned offset within the vm */
	offset = 1 + random() % (va_length / 2);
	r = pread(fd, (char *)v1 + offset, va_length - offset, va_start + offset);
	igt_assert_eq(r, va_length - offset);
	for (i = 0; i < items; i++)
		igt_assert_eq(v1[i], va_start + i);

	for (i = 0; i < items; i++)
		v1[i] = va_start + i + 1;

	r = pwrite(fd, v1, offset, va_start);
	igt_assert_eq(r, offset);

	r = pwrite(fd, (char *)v1 + offset, va_length - offset, va_start + offset);
	igt_assert_eq(r, va_length - offset);

	lseek(fd, va_start, SEEK_SET);
	r = read(fd, v2, va_length);
	igt_assert_eq(r, va_length);

	for (i = 0; i < items; i++)
		igt_assert_eq(v1[i], v2[i]);

	fsync(fd);

	close(fd);
	free(v1);
	free(v2);
}

static void vm_trigger(struct xe_eudebug_debugger *d,
		       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_op *eo = (void *)e;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		struct drm_xe_eudebug_event_vm_bind *eb;

		igt_debug("vm bind op event received with ref %lld, addr 0x%llx, range 0x%llx\n",
			  eo->vm_bind_ref_seqno,
			  eo->addr,
			  eo->range);

		eb = (struct drm_xe_eudebug_event_vm_bind *)
			xe_eudebug_event_log_find_seqno(d->log, eo->vm_bind_ref_seqno);
		igt_assert(eb);

		debugger_test_vma(d, eb->client_handle, eb->vm_handle,
				  eo->addr, eo->range);
		xe_eudebug_debugger_signal_stage(d, eo->addr);
	}
}

/**
 * SUBTEST: basic-vm-access
 * Functionality: VM access
 * Description:
 *      Exercise XE_EUDEBUG_VM_OPEN with pread and pwrite into the
 *      vm fd, concerning many different offsets inside the vm,
 *      and many virtual addresses of the vm_bound object.
 *
 * SUBTEST: basic-vm-access-faultable
 * Functionality: VM access with FAULTABLE_VM
 * Description:
 *      Fault variation of test basic-vm-access.
 *
 * SUBTEST: basic-vm-access-userptr
 * Functionality: VM access
 * Description:
 *      Exercise XE_EUDEBUG_VM_OPEN with pread and pwrite into the
 *      vm fd, concerning many different offsets inside the vm,
 *      and many virtual addresses of the vm_bound object, but backed
 *      by userptr.
 *
 * SUBTEST: basic-vm-access-userptr-faultable
 * Functionality: VM access with FAULTABLE_VM
 * Description:
 *      Fault variation of test basic-vm-access-userptr.
 */
static void test_vm_access(int fd, unsigned int flags, int num_clients)
{
	struct drm_xe_engine_class_instance *hwe;

	igt_require(!(flags & TEST_FAULTABLE) || !xe_supports_faults(fd));

	xe_eudebug_for_each_engine(fd, hwe)
		test_client_with_trigger(fd, flags, num_clients,
					 vm_access_client,
					 DRM_XE_EUDEBUG_EVENT_VM_BIND_OP,
					 vm_trigger, hwe,
					 false,
					 XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP |
					 XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);
}

static void debugger_test_vma_parameters(struct xe_eudebug_debugger *d,
					 uint64_t client_handle,
					 uint64_t vm_handle,
					 uint64_t va_start,
					 uint64_t va_length)
{
	struct drm_xe_eudebug_vm_open vo = { 0, };
	uint64_t *v;
	uint64_t items = va_length / sizeof(uint64_t);
	int fd;
	int r, i;

	v = malloc(va_length);
	igt_assert(v);

	/* Negative VM open - bad client handle */
	vo.client_handle = client_handle + 123;
	vo.vm_handle = vm_handle;
	fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
	igt_assert_lt(fd, 0);

	/* Negative VM open - bad vm handle */
	vo.client_handle = client_handle;
	vo.vm_handle = vm_handle + 123;
	fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
	igt_assert_lt(fd, 0);

	/* Positive VM open */
	vo.client_handle = client_handle;
	vo.vm_handle = vm_handle;
	fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
	igt_assert_lte(0, fd);

	/* Negative pread - bad fd */
	r = pread(fd + 123, v, va_length, va_start);
	igt_assert_lt(r, 0);

	/* Negative pread - bad va_start */
	r = pread(fd, v, va_length, 0);
	igt_assert_lt(r, 0);

	/* Negative pread - bad va_start */
	r = pread(fd, v, va_length, va_start - 1);
	igt_assert_lt(r, 0);

	/* Positive pread - zero va_length */
	r = pread(fd, v, 0, va_start);
	igt_assert_eq(r, 0);

	/* Negative pread - out of range */
	r = pread(fd, v, va_length + 1, va_start);
	igt_assert_eq(r, va_length);

	/* Negative pread - bad va_start */
	r = pread(fd, v, 1, va_start + va_length);
	igt_assert_lt(r, 0);

	/* Positive pread - whole range */
	r = pread(fd, v, va_length, va_start);
	igt_assert_eq(r, va_length);

	/* Positive pread */
	r = pread(fd, v, 1, va_start + va_length - 1);
	igt_assert_eq(r, 1);

	for (i = 0; i < items; i++)
		igt_assert_eq(v[i], va_start + i);

	for (i = 0; i < items; i++)
		v[i] = va_start + i + 1;

	/* Negative pwrite - bad fd */
	r = pwrite(fd + 123, v, va_length, va_start);
	igt_assert_lt(r, 0);

	/* Negative pwrite - bad va_start */
	r = pwrite(fd, v, va_length, -1);
	igt_assert_lt(r, 0);

	/* Negative pwrite - zero va_start */
	r = pwrite(fd, v, va_length, 0);
	igt_assert_lt(r, 0);

	/* Negative pwrite - bad va_length */
	r = pwrite(fd, v, va_length + 1, va_start);
	igt_assert_eq(r, va_length);

	/* Positive pwrite - zero va_length */
	r = pwrite(fd, v, 0, va_start);
	igt_assert_eq(r, 0);

	/* Positive pwrite */
	r = pwrite(fd, v, va_length, va_start);
	igt_assert_eq(r, va_length);
	fsync(fd);

	close(fd);
	free(v);
}

static void vm_trigger_access_parameters(struct xe_eudebug_debugger *d,
					 struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_op *eo = (void *)e;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		struct drm_xe_eudebug_event_vm_bind *eb;

		igt_debug("vm bind op event received with ref %lld, addr 0x%llx, range 0x%llx\n",
			  eo->vm_bind_ref_seqno,
			  eo->addr,
			  eo->range);

		eb = (struct drm_xe_eudebug_event_vm_bind *)
			xe_eudebug_event_log_find_seqno(d->log, eo->vm_bind_ref_seqno);
		igt_assert(eb);

		debugger_test_vma_parameters(d, eb->client_handle, eb->vm_handle, eo->addr,
					     eo->range);
		xe_eudebug_debugger_signal_stage(d, eo->addr);
	}
}

/**
 * SUBTEST: basic-vm-access-parameters
 * Functionality: VM access
 * Description:
 *      Check negative scenarios of VM_OPEN ioctl and pread/pwrite usage
 *      with bo backing storage.
 *
 * SUBTEST: basic-vm-access-parameters-faultable
 * Functionality: VM access with FAULTABLE_VM
 * Description:
 *	Fault variation of test basic-vm-access-parameters.
 *
 * SUBTEST: basic-vm-access-parameters-userptr
 * Functionality: VM access
 * Description:
 *      Check negative scenarios of VM_OPEN ioctl and pread/pwrite usage
 *      with userptr backing storage.
 *
 * SUBTEST: basic-vm-access-parameters-userptr-faultable
 * Functionality: VM access with FAULTABLE_VM
 * Description:
 *      Fault variation of test basic-vm-access-parameters-userptr.
 */
static void test_vm_access_parameters(int fd, unsigned int flags, int num_clients)
{
	struct drm_xe_engine_class_instance *hwe;

	igt_require(!(flags & TEST_FAULTABLE) || !xe_supports_faults(fd));

	xe_eudebug_for_each_engine(fd, hwe)
		test_client_with_trigger(fd, flags, num_clients,
					 vm_access_client,
					 DRM_XE_EUDEBUG_EVENT_VM_BIND_OP,
					 vm_trigger_access_parameters, hwe,
					 false,
					 XE_EUDEBUG_FILTER_EVENT_VM_BIND_OP |
					 XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);
}

static void metadata_access_client(struct xe_eudebug_client *c)
{
	const uint64_t addr = 0x1a0000;
	struct drm_xe_vm_bind_op_ext_attach_debug *ext;
	uint8_t *data;
	size_t bo_size;
	uint32_t bo, vm;
	int fd, i;

	fd = xe_eudebug_client_open_driver(c);

	bo_size = xe_get_default_alignment(fd);
	vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	bo = xe_bo_create(fd, vm, bo_size, system_memory(fd), 0);

	ext = basic_vm_bind_metadata_ext_prepare(fd, c, &data);

	xe_eudebug_client_vm_bind_flags(c, fd, vm, bo, 0, addr,
					bo_size, 0, NULL, 0, to_user_pointer(ext));

	for (i = 0; i < WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM; i++)
		xe_eudebug_client_wait_stage(c, i);

	xe_eudebug_client_vm_unbind(c, fd, vm, 0, addr, bo_size);

	basic_vm_bind_metadata_ext_del(fd, c, ext, data);

	close(bo);
	xe_eudebug_client_vm_destroy(c, fd, vm);

	xe_eudebug_client_close_driver(c, fd);
}

static void debugger_test_metadata(struct xe_eudebug_debugger *d,
				   uint64_t client_handle,
				   uint64_t metadata_handle,
				   uint64_t type,
				   uint64_t len)
{
	struct drm_xe_eudebug_read_metadata rm = {
		.client_handle = client_handle,
		.metadata_handle = metadata_handle,
		.size = len,
	};
	uint8_t *data;
	int i;

	data = malloc(len);
	igt_assert(data);

	rm.ptr = to_user_pointer(data);

	igt_assert_eq(igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_READ_METADATA, &rm), 0);

	/* syntetic check, test sets different size per metadata type */
	igt_assert_eq((type + 1) * PAGE_SIZE, rm.size);

	for (i = 0; i < rm.size; i++)
		igt_assert_eq(data[i], (i + i / PAGE_SIZE) % 256);

	free(data);
}

static void metadata_read_trigger(struct xe_eudebug_debugger *d,
				  struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_metadata *em = (void *)e;

	/* syntetic check, test sets different size per metadata type */
	igt_assert_eq((em->type + 1) * PAGE_SIZE, em->len);

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		debugger_test_metadata(d, em->client_handle, em->metadata_handle,
				       em->type, em->len);
		xe_eudebug_debugger_signal_stage(d, em->type);
	}
}

static void metadata_read_on_vm_bind_trigger(struct xe_eudebug_debugger *d,
					     struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_op_metadata *em = (void *)e;
	struct drm_xe_eudebug_event_vm_bind_op *eo = (void *)e;
	struct drm_xe_eudebug_event_vm_bind *eb;

	/* For testing purpose client sets metadata_cookie = type */

	/*
	 * Metadata event has a reference to vm-bind-op event which has a reference
	 * to vm-bind event which contains proper client-handle.
	 */
	eo = (struct drm_xe_eudebug_event_vm_bind_op *)
		xe_eudebug_event_log_find_seqno(d->log, em->vm_bind_op_ref_seqno);
	igt_assert(eo);
	eb = (struct drm_xe_eudebug_event_vm_bind *)
		xe_eudebug_event_log_find_seqno(d->log, eo->vm_bind_ref_seqno);
	igt_assert(eb);

	debugger_test_metadata(d,
			       eb->client_handle,
			       em->metadata_handle,
			       em->metadata_cookie,
			       MDATA_SIZE); /* max size */

	xe_eudebug_debugger_signal_stage(d, em->metadata_cookie);
}

/**
 * SUBTEST: read-metadata
 * Functionality: metadata
 * Description:
 *      Exercise DRM_XE_EUDEBUG_IOCTL_READ_METADATA and debug metadata create|destroy events.
 */
static void test_metadata_read(int fd, unsigned int flags, int num_clients)
{
	test_client_with_trigger(fd, flags, num_clients, metadata_access_client,
				 DRM_XE_EUDEBUG_EVENT_METADATA, metadata_read_trigger,
				 NULL, true, 0);
}

/**
 * SUBTEST: attach-debug-metadata
 * Functionality: metadata
 * Description:
 *      Read debug metadata when vm_bind has it attached.
 */
static void test_metadata_attach(int fd, unsigned int flags, int num_clients)
{
	test_client_with_trigger(fd, flags, num_clients, metadata_access_client,
				 DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA,
				 metadata_read_on_vm_bind_trigger,
				 NULL, true, 0);
}

#define STAGE_CLIENT_WAIT_ON_UFENCE_DONE 1337

#define UFENCE_EVENT_COUNT_EXPECTED 4
#define UFENCE_EVENT_COUNT_MAX 100

struct ufence_bind {
	struct drm_xe_sync f;
	uint64_t addr;
	uint64_t range;
	uint64_t value;
	struct {
		uint64_t vm_sync;
	} *fence_data;
};

static void client_wait_ufences(struct xe_eudebug_client *c,
				int fd, struct ufence_bind *binds, int count)
{
	const int64_t default_fence_timeout_ns = 500 * NSEC_PER_MSEC;
	int64_t timeout_ns;
	int err;

	/* Ensure that wait on unacked ufence times out */
	for (int i = 0; i < count; i++) {
		struct ufence_bind *b = &binds[i];

		timeout_ns = default_fence_timeout_ns;
		err = __xe_wait_ufence(fd, &b->fence_data->vm_sync, b->f.timeline_value,
				       0, &timeout_ns);
		igt_assert_eq(err, -ETIME);
		igt_assert_neq(b->fence_data->vm_sync, b->f.timeline_value);
		igt_debug("wait #%d blocked on ack\n", i);
	}

	/* Wait on fence timed out, now tell the debugger to ack */
	xe_eudebug_client_signal_stage(c, STAGE_CLIENT_WAIT_ON_UFENCE_DONE);

	/* Check that ack unblocks ufence */
	for (int i = 0; i < count; i++) {
		struct ufence_bind *b = &binds[i];

		timeout_ns = XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC;
		err = __xe_wait_ufence(fd, &b->fence_data->vm_sync, b->f.timeline_value,
				       0, &timeout_ns);
		igt_assert_eq(err, 0);
		igt_assert_eq(b->fence_data->vm_sync, b->f.timeline_value);
		igt_debug("wait #%d completed\n", i);
	}
}

static struct ufence_bind *create_binds_with_ufence(int fd, int count)
{
	struct ufence_bind *binds;

	binds = calloc(count, sizeof(*binds));
	igt_assert(binds);

	for (int i = 0; i < count; i++) {
		struct ufence_bind *b = &binds[i];

		b->range = 0x1000;
		b->addr = 0x100000 + b->range * i;
		b->fence_data = aligned_alloc(xe_get_default_alignment(fd),
					      sizeof(*b->fence_data));
		igt_assert(b->fence_data);
		memset(b->fence_data, 0, sizeof(*b->fence_data));

		b->f.type = DRM_XE_SYNC_TYPE_USER_FENCE;
		b->f.flags = DRM_XE_SYNC_FLAG_SIGNAL;
		b->f.addr = to_user_pointer(&b->fence_data->vm_sync);
		b->f.timeline_value = UFENCE_EVENT_COUNT_EXPECTED + i;
	}

	return binds;
}

static void destroy_binds_with_ufence(struct ufence_bind *binds, int count)
{
	for (int i = 0; i < count; i++)
		free(binds[i].fence_data);

	free(binds);
}

static void basic_ufence_client(struct xe_eudebug_client *c)
{
	const unsigned int n = UFENCE_EVENT_COUNT_EXPECTED;
	int fd = xe_eudebug_client_open_driver(c);
	uint32_t vm = xe_eudebug_client_vm_create(c, fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	size_t bo_size = n * xe_get_default_alignment(fd);
	uint32_t bo = xe_bo_create(fd, 0, bo_size,
				   system_memory(fd), 0);
	struct ufence_bind *binds = create_binds_with_ufence(fd, n);

	for (int i = 0; i < n; i++) {
		struct ufence_bind *b = &binds[i];

		xe_eudebug_client_vm_bind_flags(c, fd, vm, bo, 0, b->addr, b->range, 0,
						&b->f, 1, 0);
	}

	client_wait_ufences(c, fd, binds, n);

	for (int i = 0; i < n; i++) {
		struct ufence_bind *b = &binds[i];

		xe_eudebug_client_vm_unbind(c, fd, vm, 0, b->addr, b->range);
	}

	destroy_binds_with_ufence(binds, n);
	gem_close(fd, bo);
	xe_eudebug_client_vm_destroy(c, fd, vm);
	xe_eudebug_client_close_driver(c, fd);
}

struct ufence_priv {
	struct drm_xe_eudebug_event_vm_bind_ufence ufence_events[UFENCE_EVENT_COUNT_MAX];
	uint64_t ufence_event_seqno[UFENCE_EVENT_COUNT_MAX];
	uint64_t ufence_event_vm_addr_start[UFENCE_EVENT_COUNT_MAX];
	uint64_t ufence_event_vm_addr_range[UFENCE_EVENT_COUNT_MAX];
	unsigned int ufence_event_count;
	unsigned int vm_bind_op_count;
	pthread_mutex_t mutex;
};

static struct ufence_priv *ufence_priv_create(void)
{
	pthread_mutexattr_t attr;
	struct ufence_priv *priv;

	priv = mmap(0, ALIGN(sizeof(*priv), PAGE_SIZE),
		    PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(priv);
	memset(priv, 0, sizeof(*priv));

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(&priv->mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	return priv;
}

static void ufence_priv_destroy(struct ufence_priv *priv)
{
	pthread_mutex_destroy(&priv->mutex);
	munmap(priv, ALIGN(sizeof(*priv), PAGE_SIZE));
}

static void ack_fences(struct xe_eudebug_debugger *d)
{
	struct ufence_priv *priv = d->ptr;

	for (int i = 0; i < priv->ufence_event_count; i++)
		xe_eudebug_ack_ufence(d->fd, &priv->ufence_events[i]);
}

static void basic_ufence_trigger(struct xe_eudebug_debugger *d,
				 struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;
	struct ufence_priv *priv = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		char event_str[XE_EUDEBUG_EVENT_STRING_MAX_LEN];
		struct drm_xe_eudebug_event_vm_bind *eb;

		xe_eudebug_event_to_str(e, event_str, XE_EUDEBUG_EVENT_STRING_MAX_LEN);
		igt_debug("ufence event received: %s\n", event_str);

		xe_eudebug_assert_f(d, priv->ufence_event_count < UFENCE_EVENT_COUNT_EXPECTED,
				    "surplus ufence event received: %s\n", event_str);
		xe_eudebug_assert(d, ef->vm_bind_ref_seqno);

		memcpy(&priv->ufence_events[priv->ufence_event_count++], ef, sizeof(*ef));

		eb = (struct drm_xe_eudebug_event_vm_bind *)
			xe_eudebug_event_log_find_seqno(d->log, ef->vm_bind_ref_seqno);
		xe_eudebug_assert_f(d, eb, "vm bind event with seqno (%lld) not found\n",
				    ef->vm_bind_ref_seqno);
		xe_eudebug_assert_f(d, eb->flags & DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE,
				    "vm bind event does not have ufence: %s\n", event_str);
	}
}

static int wait_for_ufence_events(struct ufence_priv *priv, int timeout_ms)
{
	int ret = -ETIMEDOUT;

	igt_for_milliseconds(timeout_ms) {
		pthread_mutex_lock(&priv->mutex);
		if (priv->ufence_event_count == UFENCE_EVENT_COUNT_EXPECTED)
			ret = 0;
		pthread_mutex_unlock(&priv->mutex);

		if (!ret)
			break;
		usleep(1000);
	}

	return ret;
}

/**
 * SUBTEST: basic-vm-bind-ufence
 * Functionality: VM bind event
 * Description:
 *      Give user fence in application and check if ufence ack works
 *
 * SUBTEST: basic-vm-bind-ufence-delay-ack
 * Functionality: VM bind event
 * Description:
 *	Give user fence in application and check if delayed ufence ack works
 *
 * SUBTEST: basic-vm-bind-ufence-reconnect
 * Functionality: VM bind event
 * Description:
 *	Give user fence in application, hold it, drop the debugger connection and check if anything
 *	breaks. Expect that held acks are released when connection is dropped.
 *
 * SUBTEST: basic-vm-bind-ufence-sigint-client
 * Functionality: SIGINT
 * Description:
 *	Give user fence in application, hold it, send SIGINT to client and check if anything breaks.
 */
static void test_basic_ufence(int fd, unsigned int flags)
{
	struct xe_eudebug_debugger *d;
	struct xe_eudebug_session *s;
	struct xe_eudebug_client *c;
	struct ufence_priv *priv;
	uint32_t filter = XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE;

	priv = ufence_priv_create();
	s = xe_eudebug_session_create(fd, basic_ufence_client, flags, priv);
	c = s->client;
	d = s->debugger;

	xe_eudebug_debugger_add_trigger(d,
					DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					basic_ufence_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(d, c), 0);
	xe_eudebug_debugger_start_worker(d);
	xe_eudebug_client_start(c);

	xe_eudebug_debugger_wait_stage(s, STAGE_CLIENT_WAIT_ON_UFENCE_DONE);
	xe_eudebug_assert_f(d, wait_for_ufence_events(priv, XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * MSEC_PER_SEC) == 0,
			    "missing ufence events\n");

	if (flags & VM_BIND_DELAY_UFENCE_ACK)
		sleep(XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * 4 / 5);

	if (flags & VM_BIND_UFENCE_SIGINT_CLIENT) {
		filter = XE_EUDEBUG_FILTER_ALL;
		kill(c->pid, SIGINT);
		c->pid = 0;
		c->done = 1;
	} else if (flags & VM_BIND_UFENCE_RECONNECT) {
		filter = XE_EUDEBUG_FILTER_EVENT_VM_BIND | XE_EUDEBUG_FILTER_EVENT_VM |
				XE_EUDEBUG_FILTER_EVENT_OPEN;
		xe_eudebug_debugger_detach(d);
		xe_eudebug_client_wait_done(c);
		igt_assert_eq(xe_eudebug_debugger_attach(d, c), 0);
	} else {
		ack_fences(d);
	}

	xe_eudebug_client_wait_done(c);
	xe_eudebug_debugger_stop_worker(d);

	xe_eudebug_event_log_print(d->log, true);
	xe_eudebug_event_log_print(c->log, true);

	xe_eudebug_session_check(s, true, filter);

	xe_eudebug_session_destroy(s);
	ufence_priv_destroy(priv);
}

struct vm_bind_clear_thread_priv {
	struct drm_xe_engine_class_instance *hwe;
	struct xe_eudebug_client *c;
	pthread_t thread;
	uint64_t region;
	unsigned long sum;
};

struct vm_bind_clear_priv {
	unsigned long unbind_count;
	unsigned long bind_count;
	unsigned long sum;
};

static struct vm_bind_clear_priv *vm_bind_clear_priv_create(void)
{
	struct vm_bind_clear_priv *priv;

	priv = mmap(0, ALIGN(sizeof(*priv), PAGE_SIZE),
		    PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(priv);
	memset(priv, 0, sizeof(*priv));

	return priv;
}

static void vm_bind_clear_priv_destroy(struct vm_bind_clear_priv *priv)
{
	munmap(priv, ALIGN(sizeof(*priv), PAGE_SIZE));
}

static void *vm_bind_clear_thread(void *data)
{
	const uint32_t CS_GPR0 = 0x600;
	const size_t batch_size = 16;
	struct drm_xe_sync uf_sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct vm_bind_clear_thread_priv *priv = data;
	int fd = xe_eudebug_client_open_driver(priv->c);
	uint32_t gtt_size = 1ull << min_t(uint32_t, xe_va_bits(fd), 48);
	uint32_t vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	size_t bo_size = xe_bb_size(fd, batch_size);
	unsigned long count = 0;
	uint64_t *fence_data;
	uint32_t vm;

	vm_flags |= (priv->c->flags & TEST_FAULTABLE) ? DRM_XE_VM_CREATE_FLAG_FAULT_MODE : 0;
	vm = xe_eudebug_client_vm_create(priv->c, fd, vm_flags, 0);

	/* init uf_sync */
	fence_data = aligned_alloc(xe_get_default_alignment(fd), sizeof(*fence_data));
	igt_assert(fence_data);
	uf_sync.timeline_value = 1337;

	igt_debug("Run on: %s%u\n", xe_engine_class_string(priv->hwe->engine_class),
		  priv->hwe->engine_instance);

	igt_until_timeout(5) {
		struct drm_xe_ext_set_property eq_ext = {
			.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
			.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
			.value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE,
		};
		struct drm_xe_exec_queue_create eq_create = { 0 };
		uint32_t clean_bo = 0;
		uint32_t batch_bo = 0;
		uint64_t clean_offset, batch_offset;
		uint32_t exec_queue;
		uint32_t *map, *cs;
		uint64_t delta;

		/* calculate offsets (vma addresses) */
		batch_offset = (random() * SZ_2M) & (gtt_size - 1);
		/* XXX: for some platforms/memory regions batch offset '0' can be problematic */
		if (batch_offset == 0)
			batch_offset = SZ_2M;

		do {
			clean_offset = (random() * SZ_2M) & (gtt_size - 1);
			if (clean_offset == 0)
				clean_offset = SZ_2M;
		} while (clean_offset == batch_offset);

		batch_offset += random() % SZ_2M & -bo_size;
		clean_offset += random() % SZ_2M & -bo_size;

		delta = (random() % bo_size) & -4;

		uf_sync.addr = to_user_pointer(fence_data);
		/* prepare clean bo */
		clean_bo = xe_bo_create(fd, vm, bo_size, priv->region,
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		memset(fence_data, 0, sizeof(*fence_data));
		xe_eudebug_client_vm_bind_flags(priv->c, fd, vm, clean_bo, 0, clean_offset, bo_size,
						0, &uf_sync, 1, 0);
		xe_wait_ufence(fd, fence_data, uf_sync.timeline_value, 0,
			       XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC);

		/* prepare batch bo */
		batch_bo = xe_bo_create(fd, vm, bo_size, priv->region,
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		memset(fence_data, 0, sizeof(*fence_data));
		xe_eudebug_client_vm_bind_flags(priv->c, fd, vm, batch_bo, 0, batch_offset, bo_size,
						0, &uf_sync, 1, 0);
		xe_wait_ufence(fd, fence_data, uf_sync.timeline_value, 0,
			       XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC);

		map = xe_bo_map(fd, batch_bo, bo_size);

		cs = map;
		*cs++ = MI_NOOP | 0xc5a3;
		*cs++ = MI_LOAD_REGISTER_MEM_CMD | MI_LRI_LRM_CS_MMIO | 2;
		*cs++ = CS_GPR0;
		*cs++ = clean_offset + delta;
		*cs++ = (clean_offset + delta) >> 32;
		*cs++ = MI_STORE_REGISTER_MEM_CMD | MI_LRI_LRM_CS_MMIO | 2;
		*cs++ = CS_GPR0;
		*cs++ = batch_offset;
		*cs++ = batch_offset >> 32;
		*cs++ = MI_BATCH_BUFFER_END;

		/* execute batch */
		eq_create.width = 1;
		eq_create.num_placements = 1;
		eq_create.vm_id = vm;
		eq_create.instances = to_user_pointer(priv->hwe);
		eq_create.extensions = to_user_pointer(&eq_ext);
		exec_queue = xe_eudebug_client_exec_queue_create(priv->c, fd, &eq_create);

		uf_sync.addr = (cs - map) * 4 + batch_offset;
		xe_exec_sync(fd, exec_queue, batch_offset, &uf_sync, 1);
		xe_wait_ufence(fd, (uint64_t *)cs, uf_sync.timeline_value, exec_queue,
			       XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC);

		igt_assert_eq(*map, 0);

		/* cleanup */
		xe_eudebug_client_exec_queue_destroy(priv->c, fd, &eq_create);
		munmap(map, bo_size);

		xe_eudebug_client_vm_unbind(priv->c, fd, vm, 0, batch_offset, bo_size);
		gem_close(fd, batch_bo);

		xe_eudebug_client_vm_unbind(priv->c, fd, vm, 0, clean_offset, bo_size);
		gem_close(fd, clean_bo);

		count++;
	}

	priv->sum = count;

	free(fence_data);
	xe_eudebug_client_close_driver(priv->c, fd);
	return NULL;
}

static void vm_bind_clear_client(struct xe_eudebug_client *c)
{
	int fd = xe_eudebug_client_open_driver(c);
	struct xe_device *xe_dev = xe_device_get(fd);
	int count = xe_number_engines(fd) * xe_dev->mem_regions->num_mem_regions;
	uint64_t memreg = all_memory_regions(fd);
	struct vm_bind_clear_priv *priv = c->ptr;
	int current = 0;
	struct drm_xe_engine_class_instance *engine;
	struct vm_bind_clear_thread_priv *threads;
	uint64_t region;

	threads = calloc(count, sizeof(*threads));
	igt_assert(threads);
	priv->sum = 0;

	xe_for_each_mem_region(fd, memreg, region) {
		xe_eudebug_for_each_engine(fd, engine) {
			threads[current].c = c;
			threads[current].hwe = engine;
			threads[current].region = region;

			pthread_create(&threads[current].thread, NULL,
				       vm_bind_clear_thread, &threads[current]);
			current++;
		}
	}

	for (current = 0; current < count; current++)
		pthread_join(threads[current].thread, NULL);

	xe_for_each_mem_region(fd, memreg, region) {
		unsigned long sum = 0;

		for (current = 0; current < count; current++)
			if (threads[current].region == region)
				sum += threads[current].sum;

		igt_info("%s sampled %lu objects\n", xe_region_name(region), sum);
		priv->sum += sum;
	}

	free(threads);
	xe_device_put(fd);
	xe_eudebug_client_close_driver(c, fd);
}

static void vm_bind_clear_test_trigger(struct xe_eudebug_debugger *d,
				       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_op *eo = (void *)e;
	struct vm_bind_clear_priv *priv = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		if (random() & 1) {
			struct drm_xe_eudebug_vm_open vo = { 0, };
			uint32_t v = 0xc1c1c1c1;

			struct drm_xe_eudebug_event_vm_bind *eb;
			int fd, delta, r;

			igt_debug("vm bind op event received with ref %lld, addr 0x%llx, range 0x%llx\n",
				  eo->vm_bind_ref_seqno, eo->addr, eo->range);

			eb = (struct drm_xe_eudebug_event_vm_bind *)
				xe_eudebug_event_log_find_seqno(d->log, eo->vm_bind_ref_seqno);
			igt_assert(eb);

			vo.client_handle = eb->client_handle;
			vo.vm_handle = eb->vm_handle;

			fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
			igt_assert_lte(0, fd);

			delta = (random() % eo->range) & -4;
			r = pread(fd, &v, sizeof(v), eo->addr + delta);
			igt_assert_eq(r, sizeof(v));
			igt_assert_eq_u32(v, 0);

			close(fd);
		}
		priv->bind_count++;
	}

	if (e->flags & DRM_XE_EUDEBUG_EVENT_DESTROY)
		priv->unbind_count++;
}

static void vm_bind_clear_ack_trigger(struct xe_eudebug_debugger *d,
				      struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;

	xe_eudebug_ack_ufence(d->fd, ef);
}

/**
 * SUBTEST: vm-bind-clear
 * Functionality: memory access
 * Description:
 *      Check that fresh buffers we vm_bind into the ppGTT are always clear.
 *
 * SUBTEST: vm-bind-clear-faultable
 * Functionality: memory access with FAULTABLE_VM
 * Description:
 *      Fault variation of test vm-bind-clear.
 */
static void test_vm_bind_clear(int fd, uint32_t flags)
{
	struct vm_bind_clear_priv *priv;
	struct xe_eudebug_session *s;

	igt_require(!(flags & TEST_FAULTABLE) || !xe_supports_faults(fd));

	priv = vm_bind_clear_priv_create();
	s = xe_eudebug_session_create(fd, vm_bind_clear_client, flags, priv);

	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_OP,
					vm_bind_clear_test_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger, DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					vm_bind_clear_ack_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	xe_eudebug_client_wait_done(s->client);
	xe_eudebug_debugger_stop_worker(s->debugger);

	igt_assert_eq(priv->bind_count, priv->unbind_count);
	igt_assert_eq(priv->sum * 2, priv->bind_count);

	xe_eudebug_session_destroy(s);
	vm_bind_clear_priv_destroy(priv);
}

#define UFENCE_CLIENT_VM_TEST_VAL_START 0xaaaaaaaa
#define UFENCE_CLIENT_VM_TEST_VAL_END 0xbbbbbbbb

static void vma_ufence_client(struct xe_eudebug_client *c)
{
	const unsigned int n = UFENCE_EVENT_COUNT_EXPECTED;
	int fd = xe_eudebug_client_open_driver(c);
	struct ufence_bind *binds = create_binds_with_ufence(fd, n);
	uint32_t vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	size_t bo_size = xe_get_default_alignment(fd);
	uint64_t items = bo_size / sizeof(uint32_t);
	uint32_t bo[UFENCE_EVENT_COUNT_EXPECTED];
	uint32_t *ptr[UFENCE_EVENT_COUNT_EXPECTED];
	uint32_t vm;

	vm_flags |= (c->flags & TEST_FAULTABLE) ? DRM_XE_VM_CREATE_FLAG_FAULT_MODE : 0;
	vm = xe_eudebug_client_vm_create(c, fd, vm_flags, 0);

	for (int i = 0; i < n; i++) {
		bo[i] = xe_bo_create(fd, 0, bo_size,
				     system_memory(fd), 0);
		ptr[i] = xe_bo_map(fd, bo[i], bo_size);
		igt_assert(ptr[i]);
		memset(ptr[i], UFENCE_CLIENT_VM_TEST_VAL_START, bo_size);
	}

	for (int i = 0; i < n; i++) {
		struct ufence_bind *b = &binds[i];

		xe_eudebug_client_vm_bind_flags(c, fd, vm, bo[i], 0, b->addr, b->range, 0,
						&b->f, 1, 0);
	}

	/* Wait for acks on ufences */
	for (int i = 0; i < n; i++) {
		int err;
		int64_t timeout_ns;
		struct ufence_bind *b = &binds[i];

		timeout_ns = XE_EUDEBUG_DEFAULT_TIMEOUT_SEC * NSEC_PER_SEC;
		err = __xe_wait_ufence(fd, &b->fence_data->vm_sync, b->f.timeline_value,
				       0, &timeout_ns);
		igt_assert_eq(err, 0);
		igt_assert_eq(b->fence_data->vm_sync, b->f.timeline_value);
		igt_debug("wait #%d completed\n", i);

		for (int j = 0; j < items; j++)
			igt_assert_eq(ptr[i][j], UFENCE_CLIENT_VM_TEST_VAL_END);
	}

	for (int i = 0; i < n; i++) {
		struct ufence_bind *b = &binds[i];

		xe_eudebug_client_vm_unbind(c, fd, vm, 0, b->addr, b->range);
	}

	destroy_binds_with_ufence(binds, n);

	for (int i = 0; i < n; i++) {
		munmap(ptr[i], bo_size);
		gem_close(fd, bo[i]);
	}

	xe_eudebug_client_vm_destroy(c, fd, vm);
	xe_eudebug_client_close_driver(c, fd);
}

static void debugger_test_vma_ufence(struct xe_eudebug_debugger *d,
				     uint64_t client_handle,
				     uint64_t vm_handle,
				     uint64_t va_start,
				     uint64_t va_length)
{
	struct drm_xe_eudebug_vm_open vo = { 0, };
	uint32_t *v1, *v2;
	uint32_t items = va_length / sizeof(uint32_t);
	int fd;
	int r, i;

	v1 = malloc(va_length);
	igt_assert(v1);
	v2 = malloc(va_length);
	igt_assert(v2);

	vo.client_handle = client_handle;
	vo.vm_handle = vm_handle;

	fd = igt_ioctl(d->fd, DRM_XE_EUDEBUG_IOCTL_VM_OPEN, &vo);
	igt_assert_lte(0, fd);

	r = pread(fd, v1, va_length, va_start);
	igt_assert_eq(r, va_length);

	for (i = 0; i < items; i++)
		igt_assert_eq(v1[i], UFENCE_CLIENT_VM_TEST_VAL_START);

	memset(v1, UFENCE_CLIENT_VM_TEST_VAL_END, va_length);

	r = pwrite(fd, v1, va_length, va_start);
	igt_assert_eq(r, va_length);

	lseek(fd, va_start, SEEK_SET);
	r = read(fd, v2, va_length);
	igt_assert_eq(r, va_length);

	for (i = 0; i < items; i++)
		igt_assert_eq_u64(v1[i], v2[i]);

	fsync(fd);

	close(fd);
	free(v1);
	free(v2);
}

static void vma_ufence_op_trigger(struct xe_eudebug_debugger *d,
				  struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_op *eo = (void *)e;
	struct ufence_priv *priv = d->ptr;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		char event_str[XE_EUDEBUG_EVENT_STRING_MAX_LEN];
		struct drm_xe_eudebug_event_vm_bind *eb;
		unsigned int op_count = priv->vm_bind_op_count++;

		xe_eudebug_event_to_str(e, event_str, XE_EUDEBUG_EVENT_STRING_MAX_LEN);
		igt_debug("vm bind op event: ref %lld, addr 0x%llx, range 0x%llx, op_count %u\n",
			  eo->vm_bind_ref_seqno,
			  eo->addr,
			  eo->range,
			  op_count);
		igt_debug("vm bind op event received: %s\n", event_str);
		xe_eudebug_assert(d, eo->vm_bind_ref_seqno);
		eb = (struct drm_xe_eudebug_event_vm_bind *)
			xe_eudebug_event_log_find_seqno(d->log, eo->vm_bind_ref_seqno);

		xe_eudebug_assert_f(d, eb, "vm bind event with seqno (%lld) not found\n",
				    eo->vm_bind_ref_seqno);
		xe_eudebug_assert_f(d, eb->flags & DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE,
				    "vm bind event does not have ufence: %s\n", event_str);

		priv->ufence_event_seqno[op_count] = eo->vm_bind_ref_seqno;
		priv->ufence_event_vm_addr_start[op_count] = eo->addr;
		priv->ufence_event_vm_addr_range[op_count] = eo->range;
	}
}

static void vma_ufence_trigger(struct xe_eudebug_debugger *d,
			       struct drm_xe_eudebug_event *e)
{
	struct drm_xe_eudebug_event_vm_bind_ufence *ef = (void *)e;
	struct ufence_priv *priv = d->ptr;
	unsigned int ufence_count = priv->ufence_event_count;

	if (e->flags & DRM_XE_EUDEBUG_EVENT_CREATE) {
		char event_str[XE_EUDEBUG_EVENT_STRING_MAX_LEN];
		struct drm_xe_eudebug_event_vm_bind *eb;
		uint64_t addr = priv->ufence_event_vm_addr_start[ufence_count];
		uint64_t range = priv->ufence_event_vm_addr_range[ufence_count];

		xe_eudebug_event_to_str(e, event_str, XE_EUDEBUG_EVENT_STRING_MAX_LEN);
		igt_debug("ufence event received: %s\n", event_str);

		xe_eudebug_assert_f(d, priv->ufence_event_count < UFENCE_EVENT_COUNT_EXPECTED,
				    "surplus ufence event received: %s\n", event_str);
		xe_eudebug_assert(d, ef->vm_bind_ref_seqno);

		memcpy(&priv->ufence_events[priv->ufence_event_count++], ef, sizeof(*ef));

		eb = (struct drm_xe_eudebug_event_vm_bind *)
			xe_eudebug_event_log_find_seqno(d->log, ef->vm_bind_ref_seqno);
		xe_eudebug_assert_f(d, eb, "vm bind event with seqno (%lld) not found\n",
				    ef->vm_bind_ref_seqno);
		xe_eudebug_assert_f(d, eb->flags & DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE,
				    "vm bind event does not have ufence: %s\n", event_str);
		igt_debug("vm bind ufence event received with ref %lld, addr 0x%" PRIu64 ", range 0x%" PRIu64 "\n",
			  ef->vm_bind_ref_seqno,
			  addr,
			  range);
		debugger_test_vma_ufence(d, eb->client_handle, eb->vm_handle,
					 addr, range);

		xe_eudebug_ack_ufence(d->fd, ef);
	}
}

/**
 * SUBTEST: vma-ufence
 * Functionality: check ufence blocking
 * Description:
 *      Intercept vm bind after receiving ufence event, then access target vm and write to it.
 *      Then check on client side if the write was successful.
 *
 * SUBTEST: vma-ufence-faultable
 * Functionality: check ufence blocking with FAULTABLE_VM
 * Description:
 *      Fault variation of test vma-ufence.
 */
static void test_vma_ufence(int fd, unsigned int flags)
{
	struct xe_eudebug_session *s;
	struct ufence_priv *priv;

	igt_require(!(flags & TEST_FAULTABLE) || !xe_supports_faults(fd));

	priv = ufence_priv_create();
	s = xe_eudebug_session_create(fd, vma_ufence_client, flags, priv);

	xe_eudebug_debugger_add_trigger(s->debugger,
					DRM_XE_EUDEBUG_EVENT_VM_BIND_OP,
					vma_ufence_op_trigger);
	xe_eudebug_debugger_add_trigger(s->debugger,
					DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE,
					vma_ufence_trigger);

	igt_assert_eq(xe_eudebug_debugger_attach(s->debugger, s->client), 0);
	xe_eudebug_debugger_start_worker(s->debugger);
	xe_eudebug_client_start(s->client);

	xe_eudebug_client_wait_done(s->client);
	xe_eudebug_debugger_stop_worker(s->debugger);

	xe_eudebug_event_log_print(s->debugger->log, true);
	xe_eudebug_event_log_print(s->client->log, true);

	xe_eudebug_session_check(s, true, XE_EUDEBUG_FILTER_EVENT_VM_BIND_UFENCE);

	xe_eudebug_session_destroy(s);
	ufence_priv_destroy(priv);
}

/**
 * SUBTEST: basic-exec-queues-enable
 * Functionality: exec queues events
 * Description:
 *      Test the exec queue property of enabling eudebug
 */
static void test_basic_exec_queues_enable(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_ext_set_property eq_ext = {
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG,
		.value = 0,
	};
	const uint64_t success_value = DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE;
	const uint64_t ext = to_user_pointer(&eq_ext);
	uint32_t vm_non_lr, vm_lr;

	vm_non_lr = xe_vm_create(fd, 0, 0);
	igt_assert_lte(0, vm_non_lr);

	vm_lr = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	igt_assert_lte(0, vm_lr);

	xe_for_each_engine(fd, hwe) {
		uint32_t id;
		int ret, i;

		for (i = 0; i <= 64; i++) {
			if (i == 0)
				eq_ext.value = 0;
			else
				eq_ext.value = (1 << (i - 1));

			ret = __xe_exec_queue_create(fd, vm_lr, 1, 1,
						     hwe, ext, &id);
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE &&
			    hwe->engine_class != DRM_XE_ENGINE_CLASS_RENDER) {
				igt_assert_eq(ret, -EINVAL);
				continue;
			}

			if (eq_ext.value == success_value) {
				igt_assert_lte(0, ret);
				xe_exec_queue_destroy(fd, id);

				/* Wrong type of vm */
				ret = __xe_exec_queue_create(fd, vm_non_lr, 1, 1,
							     hwe, ext, &id);
				igt_assert_eq(ret, -EINVAL);

				xe_eudebug_enable(fd, false);
				ret = __xe_exec_queue_create(fd, vm_lr, 1, 1,
							     hwe, ext, &id);
				igt_assert_eq(ret, -EPERM);
				xe_eudebug_enable(fd, true);
			} else {
				igt_assert_eq(ret, -EINVAL);
			}
		}
	}

	xe_vm_destroy(fd, vm_lr);
	xe_vm_destroy(fd, vm_non_lr);
}

int igt_main()
{
	bool was_enabled;
	bool *multigpu_was_enabled;
	int fd, gpu_count;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		was_enabled = xe_eudebug_enable(fd, true);

		igt_install_exit_handler(igt_drm_debug_mask_reset_exit_handler);
		update_debug_mask_if_ci(DRM_UT_KMS);
	}

	igt_subtest("sysfs-toggle")
		test_sysfs_toggle(fd);

	igt_subtest("basic-connect")
		test_connect(fd);

	igt_subtest("connect-user")
		test_connect_user(fd);

	igt_subtest("basic-close")
		test_close(fd);

	igt_subtest("basic-read-event")
		test_read_event(fd);

	igt_subtest("basic-client")
		test_basic_sessions(fd, 0, 1, true);

	igt_subtest("basic-client-th")
		test_basic_sessions_th(fd, 0, 1, true);


	igt_subtest_group() {
		uint32_t flags[] = {0, TEST_FAULTABLE};
		const char *suffix[] = {"", "-faultable"};

		for (int i = 0; i < ARRAY_SIZE(flags); i++) {
			igt_subtest_f("basic-vm-access%s", suffix[i])
				test_vm_access(fd, flags[i], 1);

			igt_subtest_f("basic-vm-access-userptr%s", suffix[i])
				test_vm_access(fd, VM_BIND_OP_MAP_USERPTR | flags[i], 1);

			igt_subtest_f("basic-vm-access-parameters%s", suffix[i])
				test_vm_access_parameters(fd, flags[i], 1);

			igt_subtest_f("basic-vm-access-parameters-userptr%s", suffix[i])
				test_vm_access_parameters(fd, VM_BIND_OP_MAP_USERPTR | flags[i], 1);

			igt_subtest_f("vma-ufence%s", suffix[i])
				test_vma_ufence(fd, flags[i]);

			igt_subtest_f("vm-bind-clear%s", suffix[i])
				test_vm_bind_clear(fd, flags[i]);
		}
	}

	igt_subtest("multiple-sessions")
		test_basic_sessions(fd, CREATE_VMS | CREATE_EXEC_QUEUES, 4, true);

	igt_subtest("basic-vms")
		test_basic_sessions(fd, CREATE_VMS, 1, true);

	igt_subtest("basic-exec-queues-enable")
		test_basic_exec_queues_enable(fd);

	igt_subtest("basic-exec-queues")
		test_basic_sessions(fd, CREATE_EXEC_QUEUES, 1, true);

	igt_subtest("basic-vm-bind")
		test_basic_sessions(fd, VM_BIND, 1, true);

	igt_subtest("basic-vm-bind-ufence")
		test_basic_ufence(fd, 0);

	igt_subtest("basic-vm-bind-ufence-delay-ack")
		test_basic_ufence(fd, VM_BIND_DELAY_UFENCE_ACK);

	igt_subtest("basic-vm-bind-ufence-reconnect")
		test_basic_ufence(fd, VM_BIND_UFENCE_RECONNECT);

	igt_subtest("basic-vm-bind-ufence-sigint-client")
		test_basic_ufence(fd, VM_BIND_UFENCE_SIGINT_CLIENT);

	igt_subtest("basic-vm-bind-discovery")
		test_basic_discovery(fd, VM_BIND, true);

	igt_subtest("basic-vm-bind-metadata-discovery")
		test_basic_discovery(fd, VM_BIND_METADATA, true);

	igt_subtest("basic-vm-bind-vm-destroy")
		test_basic_sessions(fd, VM_BIND_VM_DESTROY, 1, false);

	igt_subtest("basic-vm-bind-vm-destroy-discovery")
		test_basic_discovery(fd, VM_BIND_VM_DESTROY, false);

	igt_subtest("basic-vm-bind-extended")
		test_basic_sessions(fd, VM_BIND_EXTENDED, 1, true);

	igt_subtest("basic-vm-bind-extended-discovery")
		test_basic_discovery(fd, VM_BIND_EXTENDED, true);

	igt_subtest("read-metadata")
		test_metadata_read(fd, 0, 1);

	igt_subtest("attach-debug-metadata")
		test_metadata_attach(fd, 0, 1);

	igt_subtest("discovery-race")
		test_race_discovery(fd, 0, 4);

	igt_subtest("discovery-race-vmbind")
		test_race_discovery(fd, DISCOVERY_VM_BIND, 4);

	igt_subtest("discovery-race-sigint")
		test_race_discovery(fd, DISCOVERY_SIGINT, 1);

	igt_subtest("discovery-empty")
		test_empty_discovery(fd, DISCOVERY_CLOSE_CLIENT, 16);

	igt_subtest("discovery-empty-clients")
		test_empty_discovery(fd, DISCOVERY_DESTROY_RESOURCES, 16);

	igt_fixture() {
		xe_eudebug_enable(fd, was_enabled);
		drm_close_driver(fd);
	}

	igt_subtest_group() {
		igt_fixture() {
			gpu_count = drm_prepare_filtered_multigpu(DRIVER_XE);

			multigpu_was_enabled = malloc(gpu_count * sizeof(bool));
			igt_assert(multigpu_was_enabled);
			for (int i = 0; i < gpu_count; i++) {
				fd = drm_open_filtered_card(i);
				multigpu_was_enabled[i] = xe_eudebug_enable(fd, true);
				close(fd);
			}
		}

		igt_subtest("multigpu-basic-client") {
			igt_require(gpu_count >= 2);
			igt_multi_fork(child, gpu_count) {
				fd = drm_open_filtered_card(child);
				igt_assert_f(fd > 0, "cannot open gpu-%d, errno=%d\n",
					     child, errno);
				igt_assert(is_xe_device(fd));

				test_basic_sessions(fd, 0, 1, true);
				close(fd);
			}
			igt_waitchildren();
		}

		igt_subtest("multigpu-basic-client-many") {
			igt_require(gpu_count >= 2);
			igt_multi_fork(child, gpu_count) {
				fd = drm_open_filtered_card(child);
				igt_assert_f(fd > 0, "cannot open gpu-%d, errno=%d\n",
					     child, errno);
				igt_assert(is_xe_device(fd));

				test_basic_sessions(fd, 0, 4, true);
				close(fd);
			}
			igt_waitchildren();
		}

		igt_fixture() {
			for (int i = 0; i < gpu_count; i++) {
				fd = drm_open_filtered_card(i);
				xe_eudebug_enable(fd, multigpu_was_enabled[i]);
				close(fd);
			}
			free(multigpu_was_enabled);
		}
	}
}
