/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_SPIN_H
#define XE_SPIN_H

#include <stdint.h>
#include <stdbool.h>

#include "xe_query.h"
#include "lib/igt_dummyload.h"
#include "lib/intel_blt.h"

/* Wrapper to integrate with igt_dummyload, aka igt_spin */
igt_spin_t *xe_spin_create(int fd, const struct igt_spin_factory *opt);
void xe_spin_free(int fd, struct igt_spin *spin);

/*
 * xe_spin: abstract a bo mapped in the GPU that when exec'ed will spin the
 * engine in which it's exec'ed
 */

/**
 * struct xe_spin_mem_copy
 * @src: source BLT object
 * @dst: destination BLT object
 * @src_offset: source offset
 * @dst_offset: destination offset
 *
 * Used to perform memory copy with the spinner.
 */
struct xe_spin_mem_copy {
	int fd;
	struct blt_mem_object *src;
	struct blt_mem_object *dst;
	uint64_t src_offset;
	uint64_t dst_offset;
};

/**
 * struct xe_spin_opts
 * @addr: offset of spinner within vm
 * @preempt: allow spinner to be preempted or not
 * @ctx_ticks: number of ticks after which spinner is stopped, applied if > 0
 * @mem_copy: container of objects used for memory copy (optional)
 *
 * Used to initialize struct xe_spin spinner behavior.
 */
struct xe_spin_opts {
	uint64_t addr;
	bool preempt;
	uint32_t ctx_ticks;
	bool write_timestamp;
	struct xe_spin_mem_copy *mem_copy;
};

/* Mapped GPU object */
struct xe_spin {
	uint32_t batch[128];
	uint64_t pad;
	uint32_t start;
	uint32_t end;
	uint32_t ticks_delta;
	uint64_t exec_sync;
	uint32_t timestamp;
};

uint32_t xe_spin_nsec_to_ticks(int fd, int gt_id, uint64_t nsec);
void xe_spin_init(struct xe_spin *spin, struct xe_spin_opts *opts);
#define xe_spin_init_opts(fd, ...) \
	xe_spin_init(fd, &((struct xe_spin_opts){__VA_ARGS__}))
bool xe_spin_started(struct xe_spin *spin);
void xe_spin_wait_started(struct xe_spin *spin);
void xe_spin_end(struct xe_spin *spin);

/*
 * xe_cork: higher level API that simplifies exec'ing an xe_spin by taking care
 * of vm creation, exec call, etc.
 */

struct xe_cork_opts {
	uint64_t ahnd;
	bool debug;
};

struct xe_cork {
	struct xe_spin *spin;
	int fd;
	uint32_t vm;
	uint32_t bo;
	uint32_t exec_queue;
	uint32_t syncobj;
	uint64_t addr[XE_MAX_ENGINE_INSTANCE];
	struct drm_xe_sync sync[2];
	struct drm_xe_exec exec;
	size_t bo_size;
	struct xe_spin_opts spin_opts;
	struct xe_cork_opts cork_opts;
	bool ended;
	uint16_t class;
	uint16_t width;
	uint16_t num_placements;
};

struct xe_cork *xe_cork_create(int fd, struct drm_xe_engine_class_instance *hwe,
			       uint32_t vm, uint16_t width, uint16_t num_placements,
			       struct xe_cork_opts *opts);
#define xe_cork_create_opts(fd, hwe, vm, width, num_placements, ...) \
	xe_cork_create(fd, hwe, vm, width, num_placements, \
			&((struct xe_cork_opts){__VA_ARGS__}))
void xe_cork_destroy(int fd, struct xe_cork *ctx);
void xe_cork_sync_start(int fd, struct xe_cork *ctx);
void xe_cork_sync_end(int fd, struct xe_cork *ctx);

#endif	/* XE_SPIN_H */
