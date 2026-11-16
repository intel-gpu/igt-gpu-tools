// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <semaphore.h>
#include <errno.h>
#include <assert.h>

#include <amdgpu.h>
#include <amdgpu_drm.h>

#include "igt.h"
#include "drmtest.h"

#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_deadlock_helpers.h"
#include "lib/amdgpu/compute_utils/amd_dispatch.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"
#include "lib/amdgpu/amd_platform.h"
#include "lib/amdgpu/amd_utils.h"

#define NUM_CHILD_PROCESSES 4

#define SHARED_CHILD_DESCRIPTOR 3

#define TEST_TIMEOUT 20 //100 seconds

enum  process_type {
	PROCESS_UNKNOWN,
	PROCESS_TEST,
	PROCESS_BACKGROUND,
};

struct job_struct {
	unsigned int error;
	enum amd_ip_block_type ip;
	unsigned int ring_id;
	int reset_err_result;
	/* additional data if necessary */
};

enum error_code_bits {
	ERROR_CODE_SET_BIT,
};

enum reset_code_bits {
	NO_RESET_SET_BIT,
	QUEUE_RESET_SET_BIT,
	GPU_RESET_BEGIN_SET_BIT,
	GPU_RESET_END_SUCCESS_SET_BIT,
	GPU_RESET_END_FAILURE_SET_BIT,

	ALL_RESET_BITS = 0x1f,
};

struct shmbuf {
	sem_t sem_mutex;
	sem_t sem_state_mutex;
	sem_t sync_sem_enter;
	sem_t sync_sem_exit;
	int count;
	bool sub_test_completed;
	bool sub_test_is_skipped;
	bool sub_test_is_existed;
	unsigned int test_flags;
	int test_error_code;
	bool reset_completed;
	unsigned int reset_flags;
	struct job_struct bad_job;
	struct job_struct good_job;

};

// Local memory structure for communication between child process and worker thread
struct localbuf {
	pthread_mutex_t mutex;		// Mutex for condition variable
	pthread_cond_t cond_var;	// Condition variable to signal context availability
	int context_ready;			// Flag to indicate if the context is ready
	amdgpu_context_handle amdgpu_context;// Pointer to amdgpu context
};

struct thread_params {
	amdgpu_device_handle device;
	struct shmbuf *sh_mem;
	int num_of_tests;
	struct localbuf local_mem;

};

static struct thread_params*
allocate_thread_param(amdgpu_device_handle device, struct shmbuf *sh_mem,
					int num_of_tests)
{
	struct thread_params	*param = NULL;

	param = malloc(sizeof(struct thread_params));
	memset(param, 0, sizeof(struct thread_params));

	param->device = device;
	param->sh_mem = sh_mem;
	param->num_of_tests = num_of_tests;
	pthread_mutex_init(&param->local_mem.mutex, NULL);
	pthread_cond_init(&param->local_mem.cond_var, NULL);
	param->local_mem.amdgpu_context = NULL;
	param->local_mem.context_ready = 0;

	return param;
}

static void
free_thread_param(struct thread_params *param)
{
	if (param) {
		pthread_cond_destroy(&param->local_mem.cond_var);
		pthread_mutex_destroy(&param->local_mem.mutex);
		free(param);
	}
}

static inline
void set_bit(int nr, uint32_t *addr)
{
	*addr |= (1U << nr);
}

static inline
void clear_bit(int nr, uint32_t *addr)
{
	*addr &= ~(1U << nr);
}

static inline
int test_bit(int nr, const uint32_t *addr)
{
	return ((*addr >> nr) & 1U) != 0;
}

static void
sync_point_signal(sem_t *psem, int num_signals)
{
	int i;

	for (i = 0; i < num_signals; i++)
		sem_post(psem);
}

static void
set_reset_state(struct shmbuf *sh_mem, bool reset_state, enum reset_code_bits bit)
{
	sem_wait(&sh_mem->sem_state_mutex);
	sh_mem->reset_completed = reset_state;
	if (reset_state)
		set_bit(bit, &sh_mem->reset_flags);
	else
		clear_bit(bit, &sh_mem->reset_flags);

	sem_post(&sh_mem->sem_state_mutex);
}

static bool
get_reset_state(struct shmbuf *sh_mem, unsigned int *flags)
{
	bool reset_state;

	sem_wait(&sh_mem->sem_state_mutex);
	reset_state = sh_mem->reset_completed || sh_mem->sub_test_is_skipped;
	*flags = sh_mem->reset_flags;
	sem_post(&sh_mem->sem_state_mutex);
	return reset_state;
}

static bool
is_subtest_skipped(struct shmbuf *sh_mem)
{
	bool skipped;

	sem_wait(&sh_mem->sem_state_mutex);
	skipped = sh_mem->sub_test_is_skipped;
	sem_post(&sh_mem->sem_state_mutex);

	return skipped;
}

static void
skip_sub_test(struct shmbuf *sh_mem)
{
	sem_wait(&sh_mem->sem_state_mutex);
	sh_mem->sub_test_is_skipped = true;
	sh_mem->sub_test_is_existed = true;
	sem_post(&sh_mem->sem_state_mutex);
}

static void
set_test_state(struct shmbuf *sh_mem, bool test_state,
		int error_code, enum error_code_bits bit)
{
	sem_wait(&sh_mem->sem_state_mutex);
	sh_mem->sub_test_completed = test_state;
	sh_mem->test_error_code = error_code;
	if (test_state)
		set_bit(bit, &sh_mem->test_flags);
	else
		clear_bit(bit, &sh_mem->test_flags);
	sem_post(&sh_mem->sem_state_mutex);
}

static bool
get_test_state(struct shmbuf *sh_mem, int *error_code, unsigned int *flags)
{
	bool test_state;

	sem_wait(&sh_mem->sem_state_mutex);
	test_state = sh_mem->sub_test_completed || sh_mem->sub_test_is_skipped;
	*error_code = sh_mem->test_error_code;
	*flags = sh_mem->test_flags;
	sem_post(&sh_mem->sem_state_mutex);
	return test_state;
}

static void
sync_point_enter(struct shmbuf *sh_mem)
{

	sem_wait(&sh_mem->sem_mutex);
	sh_mem->count++;
	sem_post(&sh_mem->sem_mutex);

	if (sh_mem->count == NUM_CHILD_PROCESSES)
		sync_point_signal(&sh_mem->sync_sem_enter, NUM_CHILD_PROCESSES);

	sem_wait(&sh_mem->sync_sem_enter);
}

static void
sync_point_exit(struct shmbuf *sh_mem)
{
	sem_wait(&sh_mem->sem_mutex);
	sh_mem->count--;
	sem_post(&sh_mem->sem_mutex);

	if (sh_mem->count == 0)
		sync_point_signal(&sh_mem->sync_sem_exit, NUM_CHILD_PROCESSES);

	sem_wait(&sh_mem->sync_sem_exit);
}

static bool
is_dispatch_shader_test(unsigned int err, char error_str[128], bool *is_dispatch)
{
	static const struct error_struct {
		enum cmd_error_type err;
		bool is_shader_err;
		const char *err_str;
	} arr_err[] = {
		{ CMD_STREAM_EXEC_SUCCESS,                   false, "CMD_STREAM_EXEC_SUCCESS" },
		{ CMD_STREAM_EXEC_INVALID_OPCODE,            false, "CMD_STREAM_EXEC_INVALID_OPCODE" },
		{ CMD_STREAM_EXEC_INVALID_PACKET_LENGTH,     false, "CMD_STREAM_EXEC_INVALID_PACKET_LENGTH" },
		{ CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE, false, "CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE" },
		{ CMD_STREAM_EXEC_INVALID_PACKET_EOP_QUEUE,  false, "CMD_STREAM_EXEC_INVALID_PACKET_EOP_QUEUE" },
		{ CMD_STREAM_TRANS_BAD_REG_ADDRESS,          false, "CMD_STREAM_TRANS_BAD_REG_ADDRESS" },
		{ CMD_STREAM_TRANS_BAD_MEM_ADDRESS,          false, "CMD_STREAM_TRANS_BAD_MEM_ADDRESS" },
		{ CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC,  false, "CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC" },
		{ BACKEND_SE_GC_SHADER_EXEC_SUCCESS,         true,  "BACKEND_SE_GC_SHADER_EXEC_SUCCESS" },
		{ BACKEND_SE_GC_SHADER_INVALID_SHADER,       true,  "BACKEND_SE_GC_SHADER_INVALID_SHADER" },
		{ BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR, true,  "BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR" },
		{ BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING, true, "BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING" },
		{ BACKEND_SE_GC_SHADER_INVALID_USER_DATA,    true,  "BACKEND_SE_GC_SHADER_INVALID_USER_DATA" }
	};

	const int arr_size = ARRAY_SIZE(arr_err);
	const struct error_struct *p;
	bool ret = false;

	for (p = &arr_err[0]; p < &arr_err[arr_size]; p++) {
		if (p->err == err) {
			*is_dispatch = p->is_shader_err;
			strcpy(error_str, p->err_str);
			ret = true;
			break;
		}
	}
	return ret;
}


static bool
get_ip_type(unsigned int ip, char ip_str[64])
{
	static const struct ip_struct {
		enum amd_ip_block_type ip;
		const char *ip_str;
	} arr_ip[] = {
		{ AMD_IP_GFX,       "AMD_IP_GFX" },
		{ AMD_IP_COMPUTE,   "AMD_IP_COMPUTE" },
		{ AMD_IP_DMA,       "AMD_IP_DMA" },
		{ AMD_IP_UVD,       "AMD_IP_UVD" },
		{ AMD_IP_VCE,       "AMD_IP_VCE" },
		{ AMD_IP_UVD_ENC,   "AMD_IP_UVD_ENC" },
		{ AMD_IP_VCN_DEC,   "AMD_IP_VCN_DEC" },
		{ AMD_IP_VCN_ENC,   "AMD_IP_VCN_ENC" },
		{ AMD_IP_VCN_JPEG,  "AMD_IP_VCN_JPEG" },
		{ AMD_IP_VPE,       "AMD_IP_VPE" }
	};

	const int arr_size = ARRAY_SIZE(arr_ip);
	const struct ip_struct *p;
	bool ret = false;

	for (p = &arr_ip[0]; p < &arr_ip[arr_size]; p++) {
		if (p->ip == ip) {
			strcpy(ip_str, p->ip_str);
			ret = true;
			break;
		}
	}
	return ret;
}

static int
read_next_job(struct shmbuf *sh_mem, struct job_struct *job, bool is_good)
{
	sem_wait(&sh_mem->sem_state_mutex);
	if (is_good)
		*job = sh_mem->good_job;
	else
		*job = sh_mem->bad_job;
	sem_post(&sh_mem->sem_state_mutex);
	return 0;
}

static void wait_for_complete_iteration(struct shmbuf *sh_mem)
{
	int error_code;
	unsigned int flags;
	unsigned int reset_flags;

	while (1) {
		if (get_test_state(sh_mem, &error_code, &flags) &&
			get_reset_state(sh_mem, &reset_flags))
			break;
		sleep(1);
	}

}

static void set_next_test_to_run(struct shmbuf *sh_mem, unsigned int error,
		enum amd_ip_block_type ip_good, enum amd_ip_block_type ip_bad,
		unsigned int ring_id_good, unsigned int ring_id_bad,
		const struct reset_err_result *result)
{
	char error_str[128];
	char ip_good_str[64];
	char ip_bad_str[64];

	bool is_dispatch;

	is_dispatch_shader_test(error, error_str, &is_dispatch);

	get_ip_type(ip_good, ip_good_str);
	get_ip_type(ip_bad, ip_bad_str);

	//set jobs
	sem_wait(&sh_mem->sem_state_mutex);
	sh_mem->bad_job.error = error;
	sh_mem->bad_job.ip = ip_bad;
	sh_mem->bad_job.ring_id = ring_id_bad;
	if (ip_bad == AMD_IP_GFX)
		sh_mem->bad_job.reset_err_result = result->gfx_reset_result;
	else if (ip_bad == AMD_IP_COMPUTE)
		sh_mem->bad_job.reset_err_result = result->compute_reset_result;
	else
		sh_mem->bad_job.reset_err_result = result->sdma_reset_result;

	sh_mem->good_job.error = CMD_STREAM_EXEC_SUCCESS;
	sh_mem->good_job.ip = ip_good;
	sh_mem->good_job.reset_err_result = 0;
	sh_mem->good_job.ring_id = ring_id_good;
	sh_mem->sub_test_is_skipped = false;
	sh_mem->sub_test_is_existed = true;
	sem_post(&sh_mem->sem_state_mutex);

	//sync and wait for complete
	sync_point_enter(sh_mem);
	wait_for_complete_iteration(sh_mem);
	sync_point_exit(sh_mem);
	igt_warn_on_f(sh_mem->reset_flags == 1U << NO_RESET_SET_BIT,
		"Testing does not trigger reset \n");
}

static void set_next_test_to_skip(struct shmbuf *sh_mem)
{
	skip_sub_test(sh_mem);
	sync_point_enter(sh_mem);
	wait_for_complete_iteration(sh_mem);
	sync_point_exit(sh_mem);
}

static int
shared_mem_destroy(struct shmbuf *shmp, int shm_fd, bool unmap, char shm_name[256])
{
	int ret = 0;

	if (shmp && unmap) {
		munmap(shmp, sizeof(struct shmbuf));
		sem_destroy(&shmp->sem_mutex);
		sem_destroy(&shmp->sem_state_mutex);
		sem_destroy(&shmp->sync_sem_enter);
		sem_destroy(&shmp->sync_sem_exit);
	}
	if (shm_fd > 0)
		close(shm_fd);

	shm_unlink(shm_name);

	return ret;
}

static int
shared_mem_create(struct shmbuf **ppbuf, char shm_name[256])
{
	int shm_fd = -1;
	struct shmbuf *shmp = NULL;
	bool unmap = false;

	// Create a shared memory object
	shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1)
		goto error;


	// Configure the size of the shared memory object
	if (ftruncate(shm_fd, sizeof(struct shmbuf)) == -1)
		goto error;

	// Map the shared memory object
	shmp = mmap(0, sizeof(struct shmbuf), PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shmp == MAP_FAILED)
		goto error;

	unmap = true;
	if (sem_init(&shmp->sem_mutex, 1, 1) == -1) {
		unmap = true;
		goto error;
	}
	if (sem_init(&shmp->sem_state_mutex, 1, 1) == -1)
		goto error;

	if (sem_init(&shmp->sync_sem_enter, 1, 0) == -1)
		goto error;

	if (sem_init(&shmp->sync_sem_exit, 1, 0) == -1)
		goto error;

	shmp->count = 0;
	shmp->sub_test_completed = false;
	shmp->reset_completed = false;
	shmp->sub_test_is_skipped = false;
	shmp->sub_test_is_existed = false;

	*ppbuf = shmp;
	return shm_fd;

error:
	shared_mem_destroy(shmp,  shm_fd,  unmap, shm_name);
	return shm_fd;
}

static int
shared_mem_open(struct shmbuf **ppbuf)
{
	int shm_fd = -1;
	struct shmbuf *shmp = NULL;

	shmp = mmap(NULL, sizeof(*shmp), PROT_READ | PROT_WRITE, MAP_SHARED,
			SHARED_CHILD_DESCRIPTOR, 0);
	if (shmp == MAP_FAILED)
		goto error;
	else
		shm_fd = SHARED_CHILD_DESCRIPTOR;

	*ppbuf = shmp;

	return shm_fd;
error:
	return shm_fd;
}

static bool
is_queue_reset_tests_enable(const struct amdgpu_gpu_info *gpu_info, uint32_t version)
{
	bool enable = true;

	if (version < 9)
		enable = false;

	if (enable && amd_is_lockdep_enabled())
			enable = false;

	return enable;
}

/**
 * is_sub_test_queue_reset_enable - check whether a subtest should run on this ASIC
 * @gpu_info: GPU information from amdgpu_query_gpu_info
 * @it: dynamic_test entry whose exclude_filter[] is checked
 *
 * Returns true if the subtest should run, false if the current ASIC matches
 * one of the exclude filters and the subtest must be skipped.
 */
static bool
is_sub_test_queue_reset_enable(const struct amdgpu_gpu_info *gpu_info,
		const struct dynamic_test *it)
{
	int i;
	bool enable = true;
	int chip_id;
	char error_str[128];
	bool is_dispatch;

	for (i = 0; i < _MAX_NUM_ASIC_ID_EXCLUDE_FILTER; i++) {
		if (gpu_info->family_id == it->exclude_filter[i].family_id) {
			chip_id = gpu_info->chip_external_rev - gpu_info->chip_rev;
			if (chip_id >= it->exclude_filter[i].chip_id_begin &&
			    chip_id < it->exclude_filter[i].chip_id_end) {
				enable = false;
				is_dispatch_shader_test(it->test, error_str, &is_dispatch);
				igt_info("PID %d SKIP subtest %s family_id 0x%x chip_id 0x%x range [0x%x, 0x%x) excluded\n",
					 getpid(), error_str,
					 gpu_info->family_id, chip_id,
					 it->exclude_filter[i].chip_id_begin,
					 it->exclude_filter[i].chip_id_end);
				break;
			}
		}
	}

	return enable;
}

static int
amdgpu_write_linear(amdgpu_device_handle device, amdgpu_context_handle context_handle,
		const struct amdgpu_ip_block_version *ip_block,
		const struct job_struct *job, struct amdgpu_cs_err_codes *err_codes)
{
	const int pm4_dw = 256;
	struct amdgpu_ring_context *ring_context;
	int write_length, expect_failure;
	int oversize_dw;
	int r;

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	/* The firmware triggers a badop interrupt to prevent CP/ME from hanging.
	 * And it needs to be VIMID reset when receiving the interrupt.
	 * But for a long badop packet, fw still hangs, which is a fw bug.
	 * So please use a smaller size packet for temporary testing.
	 */
	if ((job->ip == AMD_IP_GFX) && (job->error == CMD_STREAM_EXEC_INVALID_OPCODE)) {
		write_length = 10;
		expect_failure = 0;
	} else {
		write_length = 128;
		expect_failure = job->error == CMD_STREAM_EXEC_SUCCESS ? 0 : 1;
	}
	/* setup parameters */
	ring_context->write_length =  write_length;
	/* Allocate enough for OVERSIZE case: write_length*2 data + header DWORDs */
	oversize_dw = ring_context->write_length * 2 + 16;
	ring_context->pm4 = calloc(oversize_dw > pm4_dw ? oversize_dw : pm4_dw, sizeof(*ring_context->pm4));
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	ring_context->ring_id = job->ring_id;
	igt_assert(ring_context->pm4);
	ring_context->context_handle = context_handle;
	r = amdgpu_bo_alloc_and_map(device,
					ring_context->write_length * sizeof(uint32_t),
					4096, AMDGPU_GEM_DOMAIN_GTT,
					AMDGPU_GEM_CREATE_CPU_GTT_USWC, &ring_context->bo,
					(void **)&ring_context->bo_cpu,
					&ring_context->bo_mc,
					&ring_context->va_handle);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo_cpu, 0, ring_context->write_length * sizeof(uint32_t));
	ring_context->resources[0] = ring_context->bo;
	ip_block->funcs->bad_write_linear(ip_block->funcs, ring_context,
			&ring_context->pm4_dw, job->error);

	r = amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context,
			expect_failure);
	err_codes->err_code_cs_submit = ring_context->err_codes.err_code_cs_submit;
	err_codes->err_code_wait_for_fence = ring_context->err_codes.err_code_wait_for_fence;

	amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
			ring_context->bo_mc, ring_context->write_length * sizeof(uint32_t));
	free(ring_context->pm4);
	free(ring_context);
	return r;
}

static int
run_monitor_thread(amdgpu_device_handle device, struct shmbuf *sh_mem,
		int num_of_tests, struct thread_params *param)
{
	int ret;
	int test_counter = 0;
	uint64_t init_flags, in_process_flags;
	uint32_t after_reset_state, after_reset_hangs;
	int state_machine = 0;
	int error_code;
	unsigned int flags;
	int64_t cnt = 0;
	time_t start, end;
	double elapsed = 0;
	amdgpu_context_handle local_context = NULL;

	after_reset_state = after_reset_hangs = 0;
	init_flags = in_process_flags = 0;

	while (num_of_tests > 0) {
		sync_point_enter(sh_mem);
		if (is_subtest_skipped(sh_mem)) {
			sync_point_exit(sh_mem);
			num_of_tests--;
			test_counter++;
			continue;
		}
		state_machine = 0;
		error_code = 0;
		flags = 0;
		set_reset_state(sh_mem, false, ALL_RESET_BITS);

		pthread_mutex_lock(&param->local_mem.mutex);
		while (!param->local_mem.context_ready)
			pthread_cond_wait(&param->local_mem.cond_var, &param->local_mem.mutex);

		local_context = param->local_mem.amdgpu_context;
		pthread_mutex_unlock(&param->local_mem.mutex);

		time(&start);
		while (1) {
			if (is_subtest_skipped(sh_mem))
				break;

			if (state_machine == 0) {
				amdgpu_cs_query_reset_state2(local_context, &init_flags);

				if (init_flags & AMDGPU_CTX_QUERY2_FLAGS_RESET)
					state_machine = 1;

				if (init_flags & AMDGPU_CTX_QUERY2_FLAGS_RESET_IN_PROGRESS)
					state_machine = 2;

			} else if (state_machine == 1) {
				amdgpu_cs_query_reset_state(local_context,
						&after_reset_state, &after_reset_hangs);
				amdgpu_cs_query_reset_state2(local_context,
						&in_process_flags);

				//TODO refactor this block !
				igt_assert_eq(in_process_flags & AMDGPU_CTX_QUERY2_FLAGS_RESET, 1);
				if (get_test_state(sh_mem, &error_code, &flags) &&
						test_bit(ERROR_CODE_SET_BIT, &flags)) {
					if (error_code == -ENODATA) {
						set_reset_state(sh_mem, true, QUEUE_RESET_SET_BIT);
						break;
					} else {
						if (error_code != -ECANCELED && error_code != -ETIME) {
							set_reset_state(sh_mem, true, GPU_RESET_END_FAILURE_SET_BIT);
							break;
						} else {
							set_reset_state(sh_mem, true, GPU_RESET_BEGIN_SET_BIT);
							state_machine = 2; //gpu reset stage
						}
					}
				}
			} else if (state_machine == 2) {
				amdgpu_cs_query_reset_state(local_context,
						&after_reset_state, &after_reset_hangs);
				amdgpu_cs_query_reset_state2(local_context,
						&in_process_flags);
				/* here we should start timer and wait for some time until
				 * the flag AMDGPU_CTX_QUERY2_FLAGS_RESET disappear
				 */
				if (!(in_process_flags & AMDGPU_CTX_QUERY2_FLAGS_RESET_IN_PROGRESS)) {
					set_reset_state(sh_mem, true, GPU_RESET_END_SUCCESS_SET_BIT);
					break;
				}
			}
			cnt++;
			if (cnt % 1000000 == 0) {
				time(&end);
				elapsed = difftime(end, start);
				if (elapsed >= TEST_TIMEOUT) {
					set_reset_state(sh_mem, true, NO_RESET_SET_BIT);
					break;
				}
			}
		}
		elapsed = 0;
		sync_point_exit(sh_mem);
		num_of_tests--;
		test_counter++;
	}
	return ret;
}

static void *
worker_thread(void *arg)
{
	struct thread_params *param = (struct thread_params *)arg;

	run_monitor_thread(param->device, param->sh_mem, param->num_of_tests, param);

	return NULL;
}


static int
run_test_child(amdgpu_device_handle device, struct shmbuf *sh_mem,
		int num_of_tests, uint32_t version, struct thread_params *param)
{
	int ret, r;
	bool bool_ret;
	int test_counter = 0;
	char error_str[128];
	bool is_dispatch = false;
	unsigned int reset_flags;

	struct job_struct job;
	const struct amdgpu_ip_block_version *ip_block_test = NULL;
	struct amdgpu_cs_err_codes err_codes;
	amdgpu_context_handle local_context = NULL;

	while (num_of_tests > 0) {
		sync_point_enter(sh_mem);
		if (is_subtest_skipped(sh_mem)) {
			sync_point_exit(sh_mem);
			num_of_tests--;
			test_counter++;
			continue;
		}
		set_test_state(sh_mem, false, 0, ERROR_CODE_SET_BIT);
		read_next_job(sh_mem, &job, false);
		bool_ret = is_dispatch_shader_test(job.error,  error_str, &is_dispatch);
		igt_assert_eq(bool_ret, 1);
		ip_block_test = get_ip_block(device, job.ip);
		err_codes.err_code_cs_submit = 0;
		err_codes.err_code_wait_for_fence = 0;

		r = amdgpu_cs_ctx_create(device, &local_context);
		igt_assert_eq(r, 0);
		pthread_mutex_lock(&param->local_mem.mutex);
		param->local_mem.amdgpu_context = local_context;
		param->local_mem.context_ready = 1; // Mark the context as ready
		pthread_cond_signal(&param->local_mem.cond_var); // Notify the worker thread
		pthread_mutex_unlock(&param->local_mem.mutex);

		if (is_dispatch) {
			ret = amdgpu_memcpy_dispatch_test(device, local_context, job.ip, job.ring_id, 0,version,
					job.error, &err_codes, false);
		} else {
			ret = amdgpu_write_linear(device, local_context,
					ip_block_test, &job, &err_codes);
		}

		num_of_tests--;
		set_test_state(sh_mem, true, ret, ERROR_CODE_SET_BIT);

		while (1) {
			/*we may have GPU reset vs queue reset */
			if (get_reset_state(sh_mem, &reset_flags))
				break;
			sleep(1);
		}


		sync_point_exit(sh_mem);

		pthread_mutex_lock(&param->local_mem.mutex);
		param->local_mem.context_ready = 0;
		param->local_mem.amdgpu_context = NULL;
		pthread_mutex_unlock(&param->local_mem.mutex);

		amdgpu_cs_ctx_free(local_context);
		test_counter++;
		igt_assert_eq(err_codes.err_code_cs_submit, 0);
		//igt_assert_eq(err_codes.err_code_wait_for_fence, job.reset_err_result);
		//igt_info("====Test %s ip %s wait_for_fence %d==============\n", error_str,
		//		job.ip == AMD_IP_GFX ? "AMD_IP_GFX" : "AMD_IP_COMPUTE",
		//		err_codes.err_code_wait_for_fence);
	}
	return ret;
}

static int
run_background(amdgpu_device_handle device, struct shmbuf *sh_mem,
					int num_of_tests)
{
#define NUM_ITERATION 10000
	char error_str[128];
	bool is_dispatch = false;
	unsigned int reset_flags;

	int r, counter = 0;
	amdgpu_context_handle context_handle = NULL;
	struct job_struct job;
	const struct amdgpu_ip_block_version *ip_block_test = NULL;
	int error_code;
	unsigned int flags;
	struct amdgpu_cs_err_codes err_codes;

	while (num_of_tests > 0) {
		sync_point_enter(sh_mem);
		if (is_subtest_skipped(sh_mem)) {
			sync_point_exit(sh_mem);
			num_of_tests--;
			continue;
		}
		read_next_job(sh_mem, &job, true);
		ip_block_test = get_ip_block(device, job.ip);
		is_dispatch_shader_test(job.error,  error_str, &is_dispatch);
		r = amdgpu_cs_ctx_create(device, &context_handle);
		igt_assert_eq(r, 0);
		while (1) {
			r = amdgpu_write_linear(device, context_handle,  ip_block_test, &job, &err_codes);

			if (counter > NUM_ITERATION && counter % NUM_ITERATION == 0)
				igt_debug("+++BACKGROUND++ amdgpu_write_linear for %s ring_id %d ret %d counter %d\n",
						job.ip == AMD_IP_GFX ? "AMD_IP_GFX":"AMD_IP_COMPUTE", job.ring_id, r, counter);

			if (get_test_state(sh_mem, &error_code, &flags) &&
				get_reset_state(sh_mem, &reset_flags)) {
				//if entire gpu reset then stop back ground jobs
				break;
			}
			/*
			 * A "good" background job can be collateral-cancelled by
			 * the concurrent queue reset triggered by the bad job.
			 * Tolerate every reset-induced error so the helper never
			 * aborts this process - its death would break the
			 * NUM_CHILD_PROCESSES barrier and hang the whole test.
			 */
			if (r != -ECANCELED && r != -ETIME && r != -ENODATA &&
			    r != -EHWPOISON)
				igt_assert_eq(r, 0);
			//igt_assert_eq(err_codes.err_code_wait_for_fence, job.reset_err_result);
			counter++;

		}
		r = amdgpu_cs_ctx_free(context_handle);
		sync_point_exit(sh_mem);
		num_of_tests--;
	}
	return r;
}


static int
run_all(amdgpu_device_handle device, enum process_type process,
		struct shmbuf *sh_mem,  int num_of_tests, uint32_t version,
		pid_t *monitor_child, pid_t *test_child)
{
	struct thread_params *param = NULL;
	pthread_t worker;

	if (process == PROCESS_TEST) {
		*test_child = fork();
		if (*test_child == -1) {
			igt_fail(IGT_EXIT_FAILURE);
		} else if (*test_child == 0) {
			*test_child = getppid();
			param = allocate_thread_param(device, sh_mem, num_of_tests);
			if (pthread_create(&worker, NULL, worker_thread, param) != 0) {
				igt_info("********Failed to create worker thread");
				igt_fail(IGT_EXIT_FAILURE);
			}
			run_test_child(device, sh_mem, num_of_tests, version, param);
			pthread_join(worker, NULL);
			free_thread_param(param);
			igt_success();
			igt_exit();

		}
	} else if (process == PROCESS_BACKGROUND) {
		run_background(device, sh_mem, num_of_tests);
		igt_success();
		igt_exit();
	}
	return 0;
}

static bool
get_command_line(char cmdline[2048], int *pargc, char ***pppargv, char **ppath)
{
	ssize_t total_length = 0;
	char *tmpline;
	char **argv = NULL;
	char *path  = NULL;
	int length_cmd[16] = {0};
	int i, argc = 0;
	ssize_t num_read;

	int fd = open("/proc/self/cmdline", O_RDONLY);

	if (fd == -1) {
		igt_info("**** Error opening /proc/self/cmdline");
		return false;
	}

	num_read = read(fd, cmdline, 2048 - 1);
	close(fd);

	if (num_read == -1) {
		igt_info("Error reading /proc/self/cmdline");
		return false;
	}
	cmdline[num_read] = '\0';

	tmpline = cmdline;
	memset(length_cmd, 0, sizeof(length_cmd));

	/*assumption that last parameter has 2 '\0' at the end*/
	for (i = 0; total_length < num_read - 2; i++) {
		length_cmd[i] = strlen(tmpline);
		total_length += length_cmd[i];
		tmpline += length_cmd[i] + 1;
		argc++;
	}
	*pargc = argc;
	if (argc == 0 || argc > 20) {
		/* not support yet fancy things */
		return false;
	}
	/* always do 2 extra for additional parameter */
	argv = (char **)malloc(sizeof(argv) * (argc + 2));
	memset(argv, 0, sizeof(argv) * (argc + 2));
	tmpline = cmdline;
	for (i = 0; i < argc; i++) {
		argv[i] = (char *)malloc(sizeof(char) * length_cmd[i] + 1);
		memcpy(argv[i], tmpline, length_cmd[i]);
		argv[i][length_cmd[i]] = 0;
		if (i == 0) {
			path = (char *)malloc(sizeof(char) * length_cmd[0] + 1);
			memcpy(path, tmpline, length_cmd[0]);
			path[length_cmd[0]] = 0;
		}
		argv[i][length_cmd[i]] = 0;
		tmpline += length_cmd[i] + 1;
	}
	*pppargv = argv;
	*ppath = path;

	return true;
}

#define BACKGROUND	"background"

static bool
is_background_parameter_found(int argc, char **argv)
{
	bool ret = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(BACKGROUND, argv[i]) == 0) {
			ret = true;
			break;
		}
	}
	return ret;
}

#define RUNSUBTEST	"--run-subtest"

static bool
is_run_subtest_parameter_found(int argc, char **argv)
{
	bool ret = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(RUNSUBTEST, argv[i]) == 0) {
			ret = true;
			break;
		}
	}
	return ret;
}

#define ONDEVICE	"--device"
static int
is_run_device_parameter_found(int argc, char **argv)
{
	int i;
	int res = 0;
	char *p = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(ONDEVICE, argv[i]) == 0) {
			/* Get the sum for a specific device as a unique identifier */
			p = argv[i+1];
			while (*p) {
				res += *p;
				p++;
			}
			break;
		}
	}

	return res;
}


static bool
add_background_parameter(int *pargc, char **argv)
{
	int argc = *pargc;
	int len = strlen(BACKGROUND);

	argv[argc] = (char *)malloc(sizeof(char) * len + 1);
	memcpy(argv[argc], BACKGROUND, len);
	argv[argc][len] = 0;
	*pargc = argc + 1;
	return true;
}

static void
free_command_line(int argc, char **argv, char *path)
{
	int i;

	for (i = 0; i <= argc; i++)
		free(argv[i]);

	free(argv);
	free(path);

}

static int
launch_background_process(int argc, char **argv, char *path, pid_t *ppid, int shm_fd)
{
	int status;
	posix_spawn_file_actions_t action;

	for (int i = 0; i < argc; i++) {
		/* The background process only runs when a queue reset is actually triggered. */
		if (strstr(argv[i], "list-subtests") != NULL)
			return 0;
	}
	posix_spawn_file_actions_init(&action);
	posix_spawn_file_actions_adddup2(&action, shm_fd, SHARED_CHILD_DESCRIPTOR);
	status = posix_spawn(ppid, path, &action, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&action);
	if (status != 0)
		igt_fail(IGT_EXIT_FAILURE);
	return status;
}

static bool
adjust_begin_index(unsigned int *ring_begin, unsigned int available_good_rings)
{
	unsigned int ring_id = *ring_begin;

	if (!((1 << ring_id) & available_good_rings)) {
		*ring_begin = 0;
		return true;
	}
	return false;
}

static bool
allow_same_ring_id(bool are_rings_of_differant_ip, unsigned int ring_id_bad, unsigned int ring_id_good)
{
	if (are_rings_of_differant_ip == true)
		return true;

	return ring_id_bad != ring_id_good;
}

static bool
get_next_rings(unsigned int *ring_begin_good, unsigned int *ring_begin_bad,
			unsigned int available_good_rings, unsigned int available_bad_rings,
			bool are_rings_of_differant_ip, unsigned int *good_job_ring,
			unsigned int *bad_job_ring)
{
	unsigned int ring_id_good, ring_id_bad;

	for (ring_id_good = *ring_begin_good; ring_id_good < 32; ring_id_good++) {
		if ((1 << ring_id_good) & available_good_rings) {
			*good_job_ring = ring_id_good;

			for (ring_id_bad = *ring_begin_bad; ring_id_bad < 32; ring_id_bad++) {
				if ((1 << ring_id_bad) & available_bad_rings &&
					allow_same_ring_id(are_rings_of_differant_ip, ring_id_bad, ring_id_good)) {
					*bad_job_ring = ring_id_bad;

					// Update the starting points for the next call
					*ring_begin_good = ring_id_good + 1;
					*ring_begin_bad = ring_id_bad + 1;

					if (adjust_begin_index(ring_begin_bad, available_bad_rings))
						*ring_begin_good += 1;

					adjust_begin_index(ring_begin_good, available_good_rings);

					return true;
				}
			}

			// Reset ring_begin_bad for the next good ring
			*ring_begin_bad = 0;
		}
	}

	return false;
}

static void
reset_rings_numbers(unsigned int *ring_id_good, unsigned int *ring_id_bad,
						unsigned int *ring_id_job_good, unsigned int *ring_id_job_bad)
{
	*ring_id_good = 0;
	*ring_id_bad = 0;
	*ring_id_job_good = 0;
	*ring_id_job_bad = 0;
}

static int
get_num_of_tests(struct dynamic_test *arr_err, enum amd_ip_block_type *ip_tests, int num_ip)
{
	int i, cnt=0;

	for (i = 0; i < num_ip; i++) {
		for (struct dynamic_test *it = arr_err; it->name; it++) {
			if(*ip_tests == AMD_IP_DMA && (!it->support_sdma))
				continue;
			cnt++;
		}
		ip_tests++;
	}

	return cnt;
}

int igt_main()
{
	char cmdline[2048];
	int argc = 0;
	char **argv = NULL;
	char *path = NULL;
	enum  process_type process = PROCESS_UNKNOWN;
	pid_t pid_background = 0;
	pid_t monitor_child = 0, test_child = 0;
	int testExitMethod, backgrounExitMethod;
	posix_spawn_file_actions_t action;
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int fd = -1;
	int fd_shm = -1;
	struct shmbuf *sh_mem = NULL;

	int r;
	char shm_name[256] = {0};
	bool arr_cap[AMD_IP_MAX] = {0};
	struct amd_lockdep_state lockdep;
	uint32_t reset;
	unsigned int ring_id_good;
	unsigned int ring_id_bad;
	unsigned int ring_id_job_good;
	unsigned int ring_id_job_bad;
	int expect_error;
	struct pci_addr pci ;

	enum amd_ip_block_type ip_tests[3] = {AMD_IP_COMPUTE/*keep first*/, AMD_IP_GFX, AMD_IP_DMA};
	enum amd_ip_block_type ip_background = AMD_IP_COMPUTE;
	struct drm_amdgpu_info_hw_ip info[ARRAY_SIZE(ip_tests)] = {0};

	struct dynamic_test arr_err[] = {
			{CMD_STREAM_EXEC_INVALID_PACKET_LENGTH, "CMD_STREAM_EXEC_INVALID_PACKET_LENGTH",
				"Stressful-and-multiple-cs-of-bad and good length-operations-using-multiple-processes",
				{ {FAMILY_AI, 0x32, 0xFF} },
				{-ECANCELED, -ECANCELED, -ECANCELED }, true},
			{CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE, "CMD_STREAM_EXEC_INVALID_PACKET_LENGTH_OVERSIZE",
				"Stressful-and-multiple-cs-of-bad and good length-operations-using-multiple-processes",
				{ {FAMILY_AI, 0x32, 0xFF} },
				{-ECANCELED, -ECANCELED, -ECANCELED }, true},
			{CMD_STREAM_EXEC_INVALID_OPCODE, "CMD_STREAM_EXEC_INVALID_OPCODE",
				"Stressful-and-multiple-cs-of-bad and good opcode-operations-using-multiple-processes",
				{}, {-ECANCELED, -ECANCELED, -ECANCELED }, true },
			//TODO  not job timeout, debug why for n31.
			//{CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC,"CMD_STREAM_TRANS_BAD_MEM_ADDRESS_BY_SYNC",
			//	"Stressful-and-multiple-cs-of-bad and good mem-sync-operations-using-multiple-processes"},
			//TODO amdgpu: device lost from bus! for n31
			//{CMD_STREAM_TRANS_BAD_REG_ADDRESS,"CMD_STREAM_TRANS_BAD_REG_ADDRESS",
			//	"Stressful-and-multiple-cs-of-bad and good reg-operations-using-multiple-processes"},
			{BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR, "BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR",
				"Stressful-and-multiple-cs-of-bad and good shader-operations-using-multiple-processes",
				{}, {-ENODATA, -ENODATA, -ENODATA }, false },
			//TODO  KGQ cannot recover by queue reset, it maybe need a fw bugfix on naiv31
			//{BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING,"BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING",
			//	"Stressful-and-multiple-cs-of-bad and good shader-operations-using-multiple-processes"},
			{BACKEND_SE_GC_SHADER_INVALID_USER_DATA, "BACKEND_SE_GC_SHADER_INVALID_USER_DATA",
				"Stressful-and-multiple-cs-of-bad and good shader-operations-using-multiple-processes",
				{}, {-ENODATA, -ENODATA, -ENODATA }, false },
			{BACKEND_SE_GC_SHADER_INVALID_SHADER, "BACKEND_SE_GC_SHADER_INVALID_SHADER",
				"Stressful-and-multiple-cs-of-bad and good shader-operations-using-multiple-processes",
				{}, {-ENODATA, -ENODATA, -ENODATA }, false },
			{}
	};

	int const_num_of_tests;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		log_total_time(true, igt_test_name());
		posix_spawn_file_actions_init(&action);

		if (!get_command_line(cmdline, &argc, &argv, &path))
			igt_fail(IGT_EXIT_FAILURE);

		if (is_run_subtest_parameter_found(argc, argv))
			const_num_of_tests = 1;
		else
			const_num_of_tests = get_num_of_tests(&arr_err[0], &ip_tests[0], ARRAY_SIZE(ip_tests));

		r = is_run_device_parameter_found(argc, argv);
		snprintf(shm_name, sizeof(shm_name), "/queue_reset_shm_%d", r);
		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);

		for (int i = 0; i < ARRAY_SIZE(ip_tests); i++) {
			r = amdgpu_query_hw_ip_info(device, ip_tests[i], 0, &info[i]);
			igt_assert_eq(r, 0);
		}

		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);

		asic_rings_readness(device, 1, arr_cap);
		amd_platform_filter_init(&gpu_info);
		igt_skip_on(!is_queue_reset_tests_enable(&gpu_info, info[1].hw_ip_version_major));

		if (!is_background_parameter_found(argc, argv)) {
			add_background_parameter(&argc, argv);
			fd_shm = shared_mem_create(&sh_mem, shm_name);
			igt_require(fd_shm != -1);
			launch_background_process(argc, argv, path, &pid_background, fd_shm);
			process = PROCESS_TEST;
			igt_skip_on(get_pci_addr_from_fd(fd, &pci));
			igt_info("PCI Address: domain %04x, bus %02x, device %02x, function %02x\n",
							pci.domain, pci.bus, pci.device, pci.function);
		} else {
			process = PROCESS_BACKGROUND;
		}
		if (process == PROCESS_BACKGROUND)
			fd_shm = shared_mem_open(&sh_mem);

		igt_require(fd_shm != -1);
		igt_require(sh_mem != NULL);

		run_all(device, process, sh_mem, const_num_of_tests,
				info[0].hw_ip_version_major, &monitor_child, &test_child);
	}
	//print expect error table for each test
	if (!argc) {// --list-subtests arg is 0
		for (int i = 0; i < ARRAY_SIZE(ip_tests); i++)
			for (struct dynamic_test *it = &arr_err[0]; it->name; it++) {
				expect_error = ip_tests[i] == AMD_IP_COMPUTE ?
						it->result.compute_reset_result : it->result.gfx_reset_result;
				igt_info("test ip: %s,  test: %s,  expected error:%d \n",
					ip_tests[i] == AMD_IP_COMPUTE ? "COMPUTE" : "GFX", it->name, expect_error);
			}
	}

	for (int i = 0; i < ARRAY_SIZE(ip_tests); i++) {
		reset_rings_numbers(&ring_id_good, &ring_id_bad, &ring_id_job_good, &ring_id_job_bad);
		for (struct dynamic_test *it = &arr_err[0]; it->name; it++) {
			if(ip_tests[i] == AMD_IP_DMA && (!it->support_sdma))
				continue;
			igt_describe("Stressful-and-multiple-cs-of-bad-and-good-length-operations-using-multiple-processes");
			igt_subtest_with_dynamic_f("amdgpu-%s-%s", ip_tests[i] == AMD_IP_COMPUTE ? "COMPUTE":
					ip_tests[i] == AMD_IP_GFX ? "GFX" : "SDMA", it->name) {
				amd_lockdep_begin(&lockdep);
				reset = AMDGPU_RESET_TYPE_PER_QUEUE;
				if (arr_cap[ip_tests[i]] && is_sub_test_queue_reset_enable(&gpu_info, it) &&
				    is_reset_enable(ip_tests[i], reset, &pci) &&
						get_next_rings(&ring_id_good, &ring_id_bad, info[0].available_rings,
						info[i].available_rings, ip_background != ip_tests[i], &ring_id_job_good, &ring_id_job_bad)) {
					igt_dynamic_f("amdgpu-%s-ring-good-%d-bad-%d-%s", it->name, ring_id_job_good, ring_id_job_bad,
							ip_tests[i] == AMD_IP_COMPUTE ? "COMPUTE":
							ip_tests[i] == AMD_IP_GFX ? "GFX" : "SDMA")
					set_next_test_to_run(sh_mem, it->test, ip_background, ip_tests[i], ring_id_job_good, ring_id_job_bad, &it->result);
				} else {
					set_next_test_to_skip(sh_mem);
				}
				amd_lockdep_end(&lockdep);
			}
		}
	}

	if (sh_mem && (!sh_mem->sub_test_is_existed))
		set_next_test_to_skip(sh_mem);

	igt_fixture() {
		if (process == PROCESS_TEST)
			waitpid(test_child, &testExitMethod, 0);
		waitpid(pid_background, &backgrounExitMethod, 0);
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
		shared_mem_destroy(sh_mem, fd_shm, true, shm_name);
		posix_spawn_file_actions_destroy(&action);

		free_command_line(argc, argv, path);
		log_total_time(false, igt_test_name());
	}
}
