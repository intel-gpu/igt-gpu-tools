// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#include <string.h>
#include <sys/timerfd.h>

#include "drmtest.h"
#include "igt.h"
#include "igt_core.h"
#include "igt_syncobj.h"
#include "intel_reg.h"

#include "xe_ioctl.h"
#include "xe_spin.h"
#include "xe_util.h"

#define XE_SPIN_MAX_CTX_TICKS (UINT32_MAX - 1000)

#define MI_SRM_CS_MMIO				(1 << 19)
#define MI_LRI_CS_MMIO				(1 << 19)
#define MI_LRR_DST_CS_MMIO			(1 << 19)
#define MI_LRR_SRC_CS_MMIO			(1 << 18)
#define CTX_TIMESTAMP 0x3a8
#define CS_GPR(x) (0x600 + 8 * (x))

enum { START_TS, NOW_TS };


uint32_t xe_spin_nsec_to_ticks(int fd, int gt_id, uint64_t nsec)
{
	uint32_t ticks = xe_nsec_to_ticks(fd, gt_id, nsec);

	igt_assert_lt_u64(ticks, XE_SPIN_MAX_CTX_TICKS);

	return ticks;
}

/**
 * xe_spin_init:
 * @spin: pointer to mapped bo in which spinner code will be written
 * @opts: pointer to spinner initialization options
 */
void xe_spin_init(struct xe_spin *spin, struct xe_spin_opts *opts)
{
	uint64_t loop_addr;
	uint64_t start_addr = opts->addr + offsetof(struct xe_spin, start);
	uint64_t end_addr = opts->addr + offsetof(struct xe_spin, end);
	uint64_t ticks_delta_addr = opts->addr + offsetof(struct xe_spin, ticks_delta);
	uint64_t pad_addr = opts->addr + offsetof(struct xe_spin, pad);
	uint64_t timestamp_addr = opts->addr + offsetof(struct xe_spin, timestamp);
	int b = 0;
	uint32_t devid;

	spin->start = 0;
	spin->end = 0xffffffff;
	spin->ticks_delta = 0;

	if (opts->ctx_ticks) {
		/* store start timestamp */
		spin->batch[b++] = MI_LOAD_REGISTER_IMM(1) | MI_LRI_CS_MMIO;
		spin->batch[b++] = CS_GPR(START_TS) + 4;
		spin->batch[b++] = 0;
		spin->batch[b++] = MI_LOAD_REGISTER_REG | MI_LRR_DST_CS_MMIO | MI_LRR_SRC_CS_MMIO;
		spin->batch[b++] = CTX_TIMESTAMP;
		spin->batch[b++] = CS_GPR(START_TS);
	}

	spin->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	spin->batch[b++] = start_addr;
	spin->batch[b++] = start_addr >> 32;
	spin->batch[b++] = 0xc0ffee;

	loop_addr = opts->addr + b * sizeof(uint32_t);

	if (opts->preempt)
		spin->batch[b++] = MI_ARB_CHECK;

	if (opts->write_timestamp) {
		spin->batch[b++] = MI_LOAD_REGISTER_REG | MI_LRR_DST_CS_MMIO | MI_LRR_SRC_CS_MMIO;
		spin->batch[b++] = CTX_TIMESTAMP;
		spin->batch[b++] = CS_GPR(NOW_TS);

		spin->batch[b++] = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_CS_MMIO;
		spin->batch[b++] = CS_GPR(NOW_TS);
		spin->batch[b++] = timestamp_addr;
		spin->batch[b++] = timestamp_addr >> 32;
	}

	if (opts->ctx_ticks) {
		spin->batch[b++] = MI_LOAD_REGISTER_IMM(1) | MI_LRI_CS_MMIO;
		spin->batch[b++] = CS_GPR(NOW_TS) + 4;
		spin->batch[b++] = 0;
		spin->batch[b++] = MI_LOAD_REGISTER_REG | MI_LRR_DST_CS_MMIO | MI_LRR_SRC_CS_MMIO;
		spin->batch[b++] = CTX_TIMESTAMP;
		spin->batch[b++] = CS_GPR(NOW_TS);

		/* delta = now - start; inverted to match COND_BBE */
		spin->batch[b++] = MI_MATH(4);
		spin->batch[b++] = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(NOW_TS));
		spin->batch[b++] = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(START_TS));
		spin->batch[b++] = MI_MATH_SUB;
		spin->batch[b++] = MI_MATH_STOREINV(MI_MATH_REG(NOW_TS), MI_MATH_REG_ACCU);

		/* Save delta for reading by COND_BBE */
		spin->batch[b++] = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_CS_MMIO;
		spin->batch[b++] = CS_GPR(NOW_TS);
		spin->batch[b++] = ticks_delta_addr;
		spin->batch[b++] = ticks_delta_addr >> 32;

		/* Delay between SRM and COND_BBE to post the writes */
		for (int n = 0; n < 8; n++) {
			spin->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			spin->batch[b++] = pad_addr;
			spin->batch[b++] = pad_addr >> 32;
			spin->batch[b++] = 0xc0ffee;
		}

		/* Break if delta [time elapsed] > ns */
		spin->batch[b++] = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | 2;
		spin->batch[b++] = ~(opts->ctx_ticks);
		spin->batch[b++] = ticks_delta_addr;
		spin->batch[b++] = ticks_delta_addr >> 32;
	}

	if (opts->mem_copy) {
		uint32_t src_width, src_pitch, dst_width, dst_pitch;

		src_width = opts->mem_copy->src->width;
		src_pitch = opts->mem_copy->src->pitch;
		dst_width = opts->mem_copy->dst->width;
		dst_pitch = opts->mem_copy->dst->pitch;

		if (src_width > dst_width) {
			igt_warn("src width must be <= dst width\n");
			src_width = dst_width;
		}

		if (src_width > SZ_256K) {
			igt_warn("src width must be less than 256K, limiting it\n");
			src_width = SZ_256K;
		}

		if (src_pitch > SZ_256K) {
			igt_warn("src pitch must be less than 256K, limiting it\n");
			src_pitch = SZ_256K;
		}

		if (dst_pitch > SZ_256K) {
			igt_warn("dst pitch must be less than 256K, limiting it\n");
			dst_pitch = SZ_256K;
		}

		spin->batch[b++] = MEM_COPY_CMD;
		spin->batch[b++] = src_width - 1;
		spin->batch[b++] = 1;  /* for byte copying this is ignored */
		spin->batch[b++] = src_pitch - 1;
		spin->batch[b++] = dst_pitch - 1;
		spin->batch[b++] = opts->mem_copy->src_offset;
		spin->batch[b++] = opts->mem_copy->src_offset << 32;
		spin->batch[b++] = opts->mem_copy->dst_offset;
		spin->batch[b++] = opts->mem_copy->dst_offset << 32;

		devid = intel_get_drm_devid(opts->mem_copy->fd);
		if (intel_graphics_ver(devid) >= IP_VER(20, 0))
			spin->batch[b++] = opts->mem_copy->src->mocs_index << XE2_MEM_COPY_SRC_MOCS_SHIFT |
					   opts->mem_copy->dst->mocs_index << XE2_MEM_COPY_DST_MOCS_SHIFT;
		else
			spin->batch[b++] = opts->mem_copy->src->mocs_index << GEN12_MEM_COPY_MOCS_SHIFT |
					   opts->mem_copy->dst->mocs_index;
	}

	spin->batch[b++] = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | 2;
	spin->batch[b++] = 0;
	spin->batch[b++] = end_addr;
	spin->batch[b++] = end_addr >> 32;

	spin->batch[b++] = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	spin->batch[b++] = loop_addr;
	spin->batch[b++] = loop_addr >> 32;

	igt_assert(b <= ARRAY_SIZE(spin->batch));
}

/**
 * xe_spin_started:
 * @spin: pointer to spinner mapped bo
 *
 * Returns: true if spinner is running, otherwise false.
 */
bool xe_spin_started(struct xe_spin *spin)
{
	return READ_ONCE(spin->start) != 0;
}

/**
 * xe_spin_wait_started:
 * @spin: pointer to spinner mapped bo
 *
 * Wait in userspace code until spinner won't start.
 */
void xe_spin_wait_started(struct xe_spin *spin)
{
	while (!xe_spin_started(spin))
		;
}

void xe_spin_end(struct xe_spin *spin)
{
	WRITE_ONCE(spin->end, 0);
}

/**
 * xe_spin_create:
 * @opt: controlling options such as allocator handle, exec_queue, vm etc
 *
 * igt_spin_new for xe, xe_spin_create submits a batch using xe_spin_init
 * which wraps around vm bind and unbinding the object associated to it.
 *
 * This returns a spinner after submitting a dummy load.
 */
igt_spin_t *
xe_spin_create(int fd, const struct igt_spin_factory *opt)
{
	size_t bo_size = xe_bb_size(fd, SZ_4K);
	uint64_t ahnd = opt->ahnd, addr;
	struct igt_spin *spin;
	struct xe_spin *xe_spin;
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};

	igt_assert(ahnd);
	spin = calloc(1, sizeof(struct igt_spin));
	igt_assert(spin);

	spin->driver = INTEL_DRIVER_XE;
	spin->syncobj = syncobj_create(fd, 0);
	spin->vm = opt->vm;
	spin->engine = opt->engine;
	spin->timerfd = -1;

	if (!spin->vm)
		spin->vm = xe_vm_create(fd, 0, 0);

	if (!spin->engine) {
		if (opt->hwe)
			spin->engine = xe_exec_queue_create(fd, spin->vm, opt->hwe, 0);
		else
			spin->engine = xe_exec_queue_create_class(fd, spin->vm, DRM_XE_ENGINE_CLASS_COPY);
	}

	spin->handle = xe_bo_create(fd, spin->vm, bo_size,
				    vram_if_possible(fd, 0),
				    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	xe_spin = xe_bo_map(fd, spin->handle, bo_size);
	addr = intel_allocator_alloc_with_strategy(ahnd, spin->handle, bo_size, 0, ALLOC_STRATEGY_LOW_TO_HIGH);
	xe_vm_bind_sync(fd, spin->vm, spin->handle, 0, addr, bo_size);

	xe_spin_init_opts(xe_spin, .addr = addr, .preempt = !(opt->flags & IGT_SPIN_NO_PREEMPTION));
	exec.exec_queue_id = spin->engine;
	exec.address = addr;
	sync.handle = spin->syncobj;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);
	xe_spin_wait_started(xe_spin);

	spin->bo_size = bo_size;
	spin->address = addr;
	spin->xe_spin = xe_spin;
	spin->opts = *opt;

	return spin;
}

static void xe_spin_sync_wait(int fd, struct igt_spin *spin)
{
	igt_assert(syncobj_wait(fd, &spin->syncobj, 1, INT64_MAX, 0, NULL));
}

/*
 * xe_spin_free:
 * @spin: spin state from igt_spin_new()
 *
 * Wrapper to free spinner created by xe_spin_create. It will
 * destroy vm, exec_queue and unbind the vm which was binded to
 * the exec_queue and bo.
 */
void xe_spin_free(int fd, struct igt_spin *spin)
{
	igt_assert(spin->driver == INTEL_DRIVER_XE);

	if (spin->timerfd >= 0) {
#ifdef ANDROID
		struct itimerspec its;

		memset(&its, 0, sizeof(its));
		its.it_value.tv_sec = 0;
		its.it_value.tv_nsec = 1;
		timerfd_settime(spin->timerfd, 0, &its, NULL);
#else
		pthread_cancel(spin->timer_thread);
#endif
		igt_assert(pthread_join(spin->timer_thread, NULL) == 0);
		close(spin->timerfd);
	}

	xe_spin_end(spin->xe_spin);
	xe_spin_sync_wait(fd, spin);
	xe_vm_unbind_sync(fd, spin->vm, 0, spin->address, spin->bo_size);
	syncobj_destroy(fd, spin->syncobj);
	gem_munmap(spin->xe_spin, spin->bo_size);
	gem_close(fd, spin->handle);

	if (!spin->opts.engine)
		xe_exec_queue_destroy(fd, spin->engine);

	if (!spin->opts.vm)
		xe_vm_destroy(fd, spin->vm);

	free(spin);
}

/**
 * xe_cork_create:
 * @fd: xe device fd
 * @hwe: Xe engine class instance if device is Xe
 * @vm: vm handle
 * @width: number of batch buffers
 * @num_placements: number of valid placements for this exec queue
 * @opts: controlling options such as allocator handle, debug.
 *
 * xe_cork_create create vm, bo, exec_queue and bind the buffer
 * using vmbind
 *
 * This returns xe_cork after binding buffer object.
 */

struct xe_cork *
xe_cork_create(int fd, struct drm_xe_engine_class_instance *hwe,
		uint32_t vm, uint16_t width, uint16_t num_placements,
		struct xe_cork_opts *opts)
{
	struct xe_cork *cork = calloc(1, sizeof(*cork));

	igt_assert(cork);
	igt_assert(width && num_placements &&
		   (width == 1 || num_placements == 1));
	igt_assert_lt(width, XE_MAX_ENGINE_INSTANCE);

	cork->class = hwe->engine_class;
	cork->width = width;
	cork->num_placements = num_placements;
	cork->vm = vm;
	cork->cork_opts = *opts;

	cork->exec.num_batch_buffer = width;
	cork->exec.num_syncs = 2;
	cork->exec.syncs = to_user_pointer(cork->sync);

	cork->sync[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	cork->sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	cork->sync[0].handle = syncobj_create(fd, 0);

	cork->sync[1].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	cork->sync[1].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	cork->sync[1].handle = syncobj_create(fd, 0);

	cork->bo_size = sizeof(struct xe_spin);
	cork->bo_size = xe_bb_size(fd, cork->bo_size);
	cork->bo = xe_bo_create(fd, cork->vm, cork->bo_size,
				vram_if_possible(fd, hwe->gt_id),
				DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	if (cork->cork_opts.ahnd) {
		for (unsigned int i = 0; i < width; i++)
			cork->addr[i] = intel_allocator_alloc_with_strategy(cork->cork_opts.ahnd,
					cork->bo, cork->bo_size, 0,
					ALLOC_STRATEGY_LOW_TO_HIGH);
	} else {
		for (unsigned int i = 0; i < width; i++)
			cork->addr[i] = 0x100000 + 0x100000 * hwe->engine_class;
	}

	cork->spin = xe_bo_map(fd, cork->bo, cork->bo_size);

	igt_assert_eq(__xe_exec_queue_create(fd, cork->vm, width, num_placements,
					     hwe, 0, &cork->exec_queue), 0);

	xe_vm_bind_async(fd, cork->vm, 0, cork->bo, 0, cork->addr[0], cork->bo_size,
			 cork->sync, 1);

	return cork;
}

/**
 * xe_cork_sync_start:
 *
 * @fd: xe device fd
 * @cork: pointer to xe_cork structure
 *
 * run the spinner using xe_spin_init submit batch using xe_exec
 * and wait for fence using syncobj_wait
 */
void xe_cork_sync_start(int fd, struct xe_cork *cork)
{
	igt_assert(cork);

	cork->spin_opts.addr = cork->addr[0];
	cork->spin_opts.write_timestamp = true;
	cork->spin_opts.preempt = true;
	xe_spin_init(cork->spin, &cork->spin_opts);

	/* reuse sync[0] as in-fence for exec */
	cork->sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;

	cork->exec.exec_queue_id = cork->exec_queue;

	if (cork->width > 1)
		cork->exec.address = to_user_pointer(cork->addr);
	else
		cork->exec.address = cork->addr[0];

	xe_exec(fd, &cork->exec);

	xe_spin_wait_started(cork->spin);
	igt_assert(!syncobj_wait(fd, &cork->sync[1].handle, 1, 1, 0, NULL));

	if (cork->cork_opts.debug)
		igt_info("%d: spinner started\n", cork->class);
}

/*
 * xe_cork_sync_end
 *
 * @fd: xe device fd
 * @cork: pointer to xe_cork structure
 *
 * Wrapper to end spinner created by xe_cork_create. It will
 * unbind the vm which was binded to the exec_queue and bo.
 */
void xe_cork_sync_end(int fd, struct xe_cork *cork)
{
	igt_assert(cork);

	if (cork->ended)
		igt_warn("Don't attempt call end twice %d\n", cork->ended);

	xe_spin_end(cork->spin);

	igt_assert(syncobj_wait(fd, &cork->sync[1].handle, 1, INT64_MAX, 0, NULL));

	cork->sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	syncobj_reset(fd, &cork->sync[0].handle, 1);

	xe_vm_unbind_async(fd, cork->vm, 0, 0, cork->addr[0], cork->bo_size, cork->sync, 1);
	igt_assert(syncobj_wait(fd, &cork->sync[0].handle, 1, INT64_MAX, 0, NULL));

	cork->ended = true;

	if (cork->cork_opts.debug)
		igt_info("%d: spinner ended (timestamp=%u)\n", cork->class,
			cork->spin->timestamp);
}

/*
 * xe_cork_destroy
 *
 * @fd: xe device fd
 * @cork: pointer to xe_cork structure
 *
 * It will destroy vm, exec_queue and free the cork.
 */
void xe_cork_destroy(int fd, struct xe_cork *cork)
{
	igt_assert(cork);

	syncobj_destroy(fd, cork->sync[0].handle);
	syncobj_destroy(fd, cork->sync[1].handle);
	xe_exec_queue_destroy(fd, cork->exec_queue);

	if (cork->cork_opts.ahnd)
		intel_allocator_free(cork->cork_opts.ahnd, cork->bo);

	munmap(cork->spin, cork->bo_size);
	gem_close(fd, cork->bo);

	free(cork);
}
