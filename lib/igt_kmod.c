/*
 * Copyright © 2016-2023 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "assembler/brw_compat.h"	/* [un]likely() */

#include "igt_aux.h"
#include "igt_core.h"
#include "igt_debugfs.h"
#include "igt_hook.h"
#include "igt_kmod.h"
#include "igt_ktap.h"
#include "igt_sysfs.h"
#include "igt_taints.h"

/**
 * SECTION:igt_kmod
 * @short_description: Wrappers around libkmod for module loading/unloading
 * @title: kmod
 * @include: igt.h
 *
 * This library provides helpers to load/unload module driver.
 *
 * Note on loading/reloading:
 *
 * Loading/unload/reloading the driver requires that resources to /dev/dri to
 * be released (closed). A potential mistake would be to submit commands to the
 * GPU by having a fd returned by @drm_open_driver, which is closed by atexit
 * signal handler so reloading/unloading the driver will fail if performed
 * afterwards. One possible solution to this issue is to use
 * @__drm_open_driver() or use @igt_set_module_param() to set module parameters
 * dynamically.
 */

static void squelch(void *data, int priority,
		    const char *file, int line, const char *fn,
		    const char *format, va_list args)
{
}

static struct kmod_ctx *kmod_ctx(void)
{
	static struct kmod_ctx *ctx;
	const char **config_paths = NULL;
	char *config_paths_str;
	char *dirname;

	if (ctx)
		goto out;

	dirname = getenv("IGT_KMOD_DIRNAME");
	if (dirname)
		igt_debug("kmod dirname = %s\n", dirname);

	config_paths_str = getenv("IGT_KMOD_CONFIG_PATHS");
	if (config_paths_str)
		igt_debug("kmod config paths = %s\n", config_paths_str);

	if (config_paths_str) {
		unsigned count = !!strlen(config_paths_str);
		unsigned i;
		char* p;

		p = config_paths_str;
		while ((p = strchr(p, ':'))) p++, count++;


		config_paths = malloc(sizeof(*config_paths) * (count + 1));
		igt_assert(config_paths != NULL);

		p = config_paths_str;
		for (i = 0; i < count; ++i) {
			igt_assert(p != NULL);
			config_paths[i] = p;

			if ((p = strchr(p, ':')))
				*p++ = '\0';
		}
		config_paths[i] = NULL;
	}

	ctx = kmod_new(dirname, config_paths);
	igt_assert(ctx != NULL);

	free(config_paths);

	kmod_set_log_fn(ctx, squelch, NULL);
out:
	return ctx;
}

/**
 * igt_kmod_is_loaded:
 * @mod_name: The name of the module.
 *
 * Returns: True in case the module has been found or false otherwise.
 *
 * Function to check the existance of module @mod_name in list of loaded kernel
 * modules.
 *
 */
bool
igt_kmod_is_loaded(const char *mod_name)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_list *mod, *list;
	bool ret = false;

	if (kmod_module_new_from_loaded(ctx, &list) < 0)
		goto out;

	kmod_list_foreach(mod, list) {
		struct kmod_module *kmod = kmod_module_get_module(mod);
		const char *kmod_name = kmod_module_get_name(kmod);

		if (!strcmp(kmod_name, mod_name)) {
			kmod_module_unref(kmod);
			ret = true;
			break;
		}
		kmod_module_unref(kmod);
	}
	kmod_module_unref_list(list);
out:
	return ret;
}

static bool
igt_kmod_is_loading(struct kmod_module *kmod)
{
	return kmod_module_get_initstate(kmod) == KMOD_MODULE_COMING;
}

static int modprobe(struct kmod_module *kmod, const char *options)
{
	unsigned int flags;

	flags = 0;
	if (options) /* force a fresh load to set the new options */
		flags |= KMOD_PROBE_FAIL_ON_LOADED;

	return kmod_module_probe_insert_module(kmod, flags, options,
					       NULL, NULL, NULL);
}

/**
 * igt_kmod_has_param:
 * @mod_name: The name of the module
 * @param: The name of the parameter
 *
 * Returns: true if the module has the parameter, false otherwise.
 */
bool igt_kmod_has_param(const char *module_name, const char *param)
{
	struct kmod_module *kmod;
	struct kmod_list *d, *pre;
	bool result = false;

	if (kmod_module_new_from_name(kmod_ctx(), module_name, &kmod))
		return false;

	pre = NULL;
	if (!kmod_module_get_info(kmod, &pre))
		goto out;

	kmod_list_foreach(d, pre) {
		const char *key, *val;

		key = kmod_module_info_get_key(d);
		if (strcmp(key, "parmtype"))
			continue;

		val = kmod_module_info_get_value(d);
		if (val && strncmp(val, param, strlen(param)) == 0) {
			result = true;
			break;
		}
	}
	kmod_module_info_free_list(pre);

out:
	kmod_module_unref(kmod);
	return result;
}

/**
 * igt_kmod_load:
 * @mod_name: The name of the module
 * @opts: Parameters for the module. NULL in case no parameters
 * are to be passed, or a '\0' terminated string otherwise.
 *
 * This function loads a kernel module using the name specified in @mod_name.
 *
 * Returns: 0 in case of success or -errno in case the module could not
 * be loaded.
 */
int
igt_kmod_load(const char *mod_name, const char *opts)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_module *kmod;
	int err = 0;

	err = kmod_module_new_from_name(ctx, mod_name, &kmod);
	if (err < 0)
		goto out;

	err = modprobe(kmod, opts);
	if (err < 0) {
		switch (err) {
		case -EEXIST:
			igt_debug("Module %s already inserted\n",
				  kmod_module_get_name(kmod));
			break;
		case -ENOENT:
			igt_debug("Unknown symbol in module %s or "
				  "unknown parameter\n",
				  kmod_module_get_name(kmod));
			break;
		default:
			igt_debug("Could not insert %s (%s)\n",
				  kmod_module_get_name(kmod), strerror(-err));
			break;
		}
	}
out:
	kmod_module_unref(kmod);
	return err < 0 ? err : 0;
}

static int igt_kmod_unload_r(struct kmod_module *kmod)
{
#define MAX_TRIES	20
#define SLEEP_DURATION	500000
	struct kmod_list *holders, *pos;
	int err = 0, tries;
	const char *mod_name = kmod_module_get_name(kmod);

	if (kmod_module_get_initstate(kmod) == KMOD_MODULE_BUILTIN)
		return 0;

	holders = kmod_module_get_holders(kmod);
	kmod_list_foreach(pos, holders) {
		struct kmod_module *it = kmod_module_get_module(pos);
		err = igt_kmod_unload_r(it);
		kmod_module_unref(it);
		if (err < 0)
			break;
	}
	kmod_module_unref_list(holders);
	if (err < 0)
		return err;

	if (igt_kmod_is_loading(kmod)) {
		igt_debug("%s still initializing\n", mod_name);
		err = igt_wait(!igt_kmod_is_loading(kmod), 10000, 100);
		if (err < 0) {
			igt_debug("%s failed to complete init within the timeout\n",
				  mod_name);
			return err;
		}
	}

	for (tries = 0; tries < MAX_TRIES; tries++) {
		err = kmod_module_remove_module(kmod, 0);

		/* Only loop in the following cases */
		if (err != -EBUSY && err != -EAGAIN)
			break;

		igt_debug("Module %s failed to unload with err: %d on attempt: %i\n",
			  mod_name, err, tries + 1);

		if (tries < MAX_TRIES - 1)
			usleep(SLEEP_DURATION);
	}

	if (err == -ENOENT)
		igt_debug("Module %s could not be found or does not exist. err: %d\n",
			  mod_name, err);
	else if (err == -ENOTSUP)
		igt_debug("Module %s cannot be unloaded. err: %d\n",
			  mod_name, err);
	else if (err)
		igt_debug("Module %s failed to unload with err: %d after ~%.1fms\n",
			  mod_name, err, SLEEP_DURATION*tries/1000.);
	else if (tries)
		igt_debug("Module %s unload took ~%.1fms over %i attempts\n",
			  mod_name, SLEEP_DURATION*tries/1000., tries + 1);
	else
		igt_debug("Module %s unloaded immediately\n", mod_name);

	return err;
}

static void igt_drop_devcoredump(const char *driver)
{
	char sysfspath[PATH_MAX];
	DIR *dir;
	char *devcoredump;
	FILE *data;
	struct dirent *entry;
	int len, ret;

	len = snprintf(sysfspath, sizeof(sysfspath),
		       "/sys/bus/pci/drivers/%s", driver);

	igt_assert(len < sizeof(sysfspath));

	 /* Not a PCI module */
	if (access(sysfspath, F_OK))
		return;

	devcoredump = sysfspath + len;

	dir = opendir(sysfspath);
	igt_assert(dir);

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_type != DT_LNK ||
		    strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		ret = snprintf(devcoredump, sizeof(sysfspath) - len,
			       "/%s/devcoredump", entry->d_name);

		igt_assert(ret < sizeof(sysfspath) - len);

		if (access(sysfspath, F_OK) != -1) {
			igt_info("Removing devcoredump before module unload: %s\n",
				 sysfspath);

			strcat(sysfspath, "/data");
			data = fopen(sysfspath, "w");
			igt_assert(data);

			/*
			 * Write anything to devcoredump/data to
			 * force its deletion
			 */
			fprintf(data, "1\n");
			fclose(data);
		}
	}
	closedir(dir);
}

/**
 * igt_kmod_unload:
 * @mod_name: Module name.
 *
 * Returns: 0 in case of success or -errno otherwise.
 *
 * Removes the module @mod_name.
 */
int
igt_kmod_unload(const char *mod_name)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_module *kmod;
	int err;

	igt_drop_devcoredump(mod_name);

	err = kmod_module_new_from_name(ctx, mod_name, &kmod);
	if (err < 0) {
		igt_debug("Could not use module %s (%s)\n", mod_name,
			  strerror(-err));
		goto out;
	}

	err = igt_kmod_unload_r(kmod);
	if (err < 0) {
		igt_debug("Could not remove module %s (%s)\n", mod_name,
			  strerror(-err));
	}

out:
	kmod_module_unref(kmod);
	return err < 0 ? err : 0;
}

/**
 *
 * igt_kmod_list_loaded: List all modules currently loaded.
 *
 */
void
igt_kmod_list_loaded(void)
{
	struct kmod_ctx *ctx = kmod_ctx();
	struct kmod_list *module, *list;

	if (kmod_module_new_from_loaded(ctx, &list) < 0)
		return;

	igt_info("Module\t\t      Used by\n");

	kmod_list_foreach(module, list) {
		struct kmod_module *kmod = kmod_module_get_module(module);
		struct kmod_list *module_deps, *module_deps_list;

		igt_info("%-24s", kmod_module_get_name(kmod));
		module_deps_list = kmod_module_get_holders(kmod);
		if (module_deps_list) {

			kmod_list_foreach(module_deps, module_deps_list) {
				struct kmod_module *kmod_dep;

				kmod_dep = kmod_module_get_module(module_deps);
				igt_info("%s", kmod_module_get_name(kmod_dep));

				if (kmod_list_next(module_deps_list, module_deps))
					igt_info(",");

				kmod_module_unref(kmod_dep);
			}
		}
		kmod_module_unref_list(module_deps_list);

		igt_info("\n");
		kmod_module_unref(kmod);
	}

	kmod_module_unref_list(list);
}

static void *strdup_realloc(char *origptr, const char *strdata)
{
	size_t nbytes = strlen(strdata) + 1;
	char *newptr = realloc(origptr, nbytes);

	memcpy(newptr, strdata, nbytes);
	return newptr;
}

/**
 * igt_intel_driver_load:
 * @opts: options to pass to Intel driver
 *
 * Loads an Intel driver and its dependencies.
 *
 */
int
igt_intel_driver_load(const char *opts, const char *driver)
{
	int ret;

	if (opts)
		igt_info("Reloading %s with %s\n\n", driver, opts);

	ret = igt_kmod_load(driver, opts);
	if (ret) {
		igt_debug("Could not load %s\n", driver);
		return ret;
	}

	bind_fbcon(true);
	igt_kmod_load("snd_hda_intel", NULL);

	return 0;
}

static int igt_always_unload_audio_driver(char **who)
{
	int ret;
	const char *sound[] = {
		"snd_hda_intel",
		"snd_hdmi_lpe_audio",
		NULL,
	};

	/*
	 * With old Kernels, the dependencies between audio and DRM drivers
	 * are not shown. So, it may not be mandatory to remove the audio
	 * driver before unload/unbind the DRM one. So, let's print warnings,
	 * but return 0 on errors, as, if the dependency is mandatory, this
	 * will be detected later when trying to unbind/unload the DRM driver.
	 */
	for (const char **m = sound; *m; m++) {
		if (igt_kmod_is_loaded(*m)) {
			if (who)
				*who = strdup_realloc(*who, *m);

			ret = igt_lsof_kill_audio_processes();
			if (ret) {
				igt_warn("Could not stop %d audio process(es)\n", ret);
				igt_kmod_list_loaded();
				igt_lsof("/dev/snd");
				return 0;
			}

			ret = pipewire_pulse_start_reserve();
			if (ret)
				igt_warn("Failed to notify pipewire_pulse\n");

			igt_kmod_unbind("snd_hda_intel", NULL);

			ret = igt_kmod_unload(*m);
			pipewire_pulse_stop_reserve();
			if (ret) {
				igt_warn("Could not unload audio driver %s\n", *m);
				igt_kmod_list_loaded();
				igt_lsof("/dev/snd");
				return 0;
			}
		}
	}
	return 0;
}

int igt_audio_driver_unload(char **who)
{
	/*
	 * Currently, there's no way to check if the audio driver binds into the
	 * DRM one. So, always remove audio drivers that  might be binding.
	 * This may change in future, once kernel/module gets fixed. So, let's
	 * keep this boilerplace, in order to make easier to add the new code,
	 * once upstream is fixed.
	 */
	return igt_always_unload_audio_driver(who);
}

int __igt_intel_driver_unload(char **who, const char *driver)
{
	int ret;

	const char *aux[] = {
		/* gen5: ips uses symbol_get() so only a soft module dependency */
		"intel_ips",
		NULL,
	};

	ret = igt_audio_driver_unload(who);
	if (ret)
		return ret;

	for (const char **m = aux; *m; m++) {
		if (!igt_kmod_is_loaded(*m))
			continue;

		ret = igt_kmod_unload(*m);
		if (ret) {
			if (who)
				*who = strdup_realloc(*who, *m);

			return ret;
		}
	}

	if (igt_kmod_is_loaded(driver)) {
		igt_kmod_unbind(driver, NULL);

		ret = igt_kmod_unload(driver);
		if (ret) {
			if (who)
				*who = strdup_realloc(*who, driver);

			return ret;
		}
	}

	return 0;
}

/**
 * igt_kmod_unbind: Unbind driver from devices. Currently supports only PCI bus
 * @mod_name: name of the module to unbind
 * @pci_device: if provided, unbind only this device, otherwise unbind all devices
 */
int igt_kmod_unbind(const char *mod_name, const char *pci_device)
{
	struct igt_hook *igt_hook = NULL;
	char path[PATH_MAX];
	struct dirent *de;
	int dirlen;
	DIR *dir;

	dirlen = snprintf(path, sizeof(path), "/sys/module/%s/drivers/pci:%s/",
			  mod_name, mod_name);
	igt_assert(dirlen < sizeof(path));

	dir = opendir(path);

	/* Module not loaded, nothing to unbind */
	if (!dir)
		return 0;

	while ((de = readdir(dir))) {
		bool ret;

		if (de->d_type != DT_LNK || !isdigit(de->d_name[0]))
			continue;

		if (pci_device && strcmp(pci_device, de->d_name) != 0)
			continue;

		ret = igt_sysfs_set(dirfd(dir), "unbind", de->d_name);
		igt_assert(ret);
	}

	closedir(dir);

	igt_hook = igt_core_get_igt_hook();
	igt_hook_event_notify(igt_hook, &(struct igt_hook_evt){
		.evt_type = IGT_HOOK_POST_KMOD_UNBIND,
		.target_name = mod_name,
	});

	return 0;
}

/**
 * igt_kmod_bind: Bind driver to device
 * @mod_name: name of the module to rebind
 * @pci_device: device to bind
 *
 * Module should already be loaded
 */
int igt_kmod_bind(const char *mod_name, const char *pci_device)
{
	char path[PATH_MAX];
	int dirlen, dirfd;
	int ret;

	dirlen = snprintf(path, sizeof(path), "/sys/module/%s/drivers/pci:%s/",
			  mod_name, mod_name);
	igt_assert(dirlen < sizeof(path));

	dirfd = open(path, O_RDONLY | O_CLOEXEC);
	if (dirfd < 0)
		return dirfd;

	ret = igt_sysfs_set(dirfd, "bind", pci_device);

	close(dirfd);

	return ret;
}

/**
 * igt_kmod_rebind: Unbind driver from devices and bind it again
 * @mod_name: name of the module to rebind
 * @pci_device: device to rebind to
 */
int igt_kmod_rebind(const char *mod_name, const char *pci_device)
{
	return igt_kmod_unbind(mod_name, pci_device) ||
		igt_kmod_bind(mod_name, pci_device);
}

/**
 * igt_intel_driver_unload:
 *
 * Unloads an Intel driver and its dependencies.
 */
int
igt_intel_driver_unload(const char *driver)
{
	char *who = NULL;
	int ret;

	ret = __igt_intel_driver_unload(&who, driver);
	if (ret) {
		igt_warn("Could not unload %s\n", who);
		igt_kmod_list_loaded();
		igt_lsof("/dev/dri");
		igt_lsof("/dev/snd");
		free(who);
		return ret;
	}
	free(who);

	if (igt_kmod_is_loaded(driver)) {
		igt_warn("%s.ko still loaded!\n", driver);
		return -EBUSY;
	}

	return 0;
}

int igt_xe_driver_unload(void)
{
	igt_audio_driver_unload(NULL);
	igt_kmod_unbind("xe", NULL);

	igt_kmod_unload("xe");
	if (igt_kmod_is_loaded("xe"))
		return IGT_EXIT_FAILURE;

	return IGT_EXIT_SUCCESS;
}

/**
 * igt_amdgpu_driver_load:
 * @opts: options to pass to amdgpu driver
 *
 * Returns: IGT_EXIT_SUCCESS or IGT_EXIT_FAILURE.
 *
 * Loads the amdgpu driver and its dependencies.
 *
 */
int
igt_amdgpu_driver_load(const char *opts)
{
	if (opts)
		igt_info("Reloading amdgpu with %s\n\n", opts);

	if (igt_kmod_load("amdgpu", opts)) {
		igt_warn("Could not load amdgpu\n");
		return IGT_EXIT_FAILURE;
	}

	bind_fbcon(true);

	return IGT_EXIT_SUCCESS;
}

/**
 * igt_amdgpu_driver_unload:
 *
 * Returns: IGT_EXIT_SUCCESS on success, IGT_EXIT_FAILURE on failure
 * and IGT_EXIT_SKIP if amdgpu could not be unloaded.
 *
 * Unloads the amdgpu driver and its dependencies.
 *
 */
int
igt_amdgpu_driver_unload(void)
{
	bind_fbcon(false);

	if (igt_kmod_is_loaded("amdgpu")) {
		if (igt_kmod_unload("amdgpu")) {
			igt_warn("Could not unload amdgpu\n");
			igt_kmod_list_loaded();
			igt_lsof("/dev/dri");
			return IGT_EXIT_SKIP;
		}
	}

	igt_kmod_unload("drm_kms_helper");
	igt_kmod_unload("drm");

	if (igt_kmod_is_loaded("amdgpu")) {
		igt_warn("amdgpu.ko still loaded!\n");
		return IGT_EXIT_FAILURE;
	}

	return IGT_EXIT_SUCCESS;
}

static void kmsg_dump(int fd)
{
	char record[4096 + 1];

	if (fd == -1) {
		igt_warn("Unable to retrieve kernel log (from /dev/kmsg)\n");
		return;
	}

	record[sizeof(record) - 1] = '\0';

	for (;;) {
		const char *start, *end;
		ssize_t r;

		r = read(fd, record, sizeof(record) - 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EPIPE) {
				igt_warn("kmsg truncated: too many messages. You may want to increase log_buf_len in kmcdline\n");
				continue;
			}

			if (errno != EAGAIN)
				igt_warn("kmsg truncated: unknown error (%m)\n");

			break;
		}

		start = strchr(record, ';');
		if (start) {
			start++;
			end = strchrnul(start, '\n');
			igt_warn("%.*s\n", (int)(end - start), start);
		}
	}
}

static void tests_add(struct igt_kselftest_list *tl, struct igt_list_head *list)
{
	struct igt_kselftest_list *pos;

	igt_list_for_each_entry(pos, list, link)
		if (pos->number > tl->number)
			break;

	igt_list_add_tail(&tl->link, &pos->link);
}

void igt_kselftest_get_tests(struct kmod_module *kmod,
			     const char *filter,
			     struct igt_list_head *tests)
{
	const char *param_prefix = "igt__";
	const int prefix_len = strlen(param_prefix);
	struct kmod_list *d, *pre;
	struct igt_kselftest_list *tl;

	pre = NULL;
	if (!kmod_module_get_info(kmod, &pre))
		return;

	kmod_list_foreach(d, pre) {
		const char *key, *val;
		char *colon;
		int offset;

		key = kmod_module_info_get_key(d);
		if (strcmp(key, "parmtype"))
			continue;

		val = kmod_module_info_get_value(d);
		if (!val || strncmp(val, param_prefix, prefix_len))
			continue;

		offset = strlen(val) + 1;
		tl = malloc(sizeof(*tl) + offset);
		if (!tl)
			continue;

		memcpy(tl->param, val, offset);
		colon = strchr(tl->param, ':');
		*colon = '\0';

		tl->number = 0;
		tl->name = tl->param + prefix_len;
		if (sscanf(tl->name, "%u__%n",
			   &tl->number, &offset) == 1)
			tl->name += offset;

		if (filter && strncmp(tl->name, filter, strlen(filter))) {
			free(tl);
			continue;
		}

		tests_add(tl, tests);
	}
	kmod_module_info_free_list(pre);
}

static int open_parameters(const char *module_name)
{
	char path[256];

	snprintf(path, sizeof(path), "/sys/module/%s/parameters", module_name);
	return open(path, O_RDONLY);
}

static void kunit_debugfs_path(char *kunit_path)
{
	const char *debugfs_path = igt_debugfs_mount();

	if (igt_debug_on(!debugfs_path))
		return;

	if (igt_debug_on(strlen(debugfs_path) + strlen("/kunit/") >= PATH_MAX))
		return;

	strcpy(stpcpy(kunit_path, debugfs_path), "/kunit/");
}

static bool kunit_set_filtering(const char *filter_glob, const char *filter,
				const char *filter_action)
{
	bool ret = true;
	int params;

	params = open_parameters("kunit");
	if (igt_debug_on(params < 0))
		return false;

	/*
	 * Default values of the KUnit base module filtering parameters
	 * are all NULLs.  Reapplying those NULLs over sysfs once
	 * overwritten with non-NULL strings seems not possible.
	 * As a workaround, we use non-NULL strings that exhibit
	 * the same behaviour as if default NULLs were in place.
	 */
	if (igt_debug_on(!igt_sysfs_set(params, "filter_glob", filter_glob ?: "*")) ||
	    igt_debug_on(!igt_sysfs_set(params, "filter", filter ?: "module!=none")) ||
	    igt_debug_on(!igt_sysfs_set(params, "filter_action", filter_action ?: "")))
		ret = false;

	close(params);

	return ret;
}

static void kunit_result_free(struct igt_ktap_result **r,
			      char **suite_name, char **case_name)
{
	if (!*r)
		return;

	igt_list_del(&(*r)->link);

	if ((*r)->suite_name != *suite_name) {
		free(*suite_name);
		*suite_name = (*r)->suite_name;
	}

	if ((*r)->case_name != *case_name) {
		free(*case_name);
		*case_name = (*r)->case_name;
	}

	free((*r)->msg);
	free(*r);
	*r = NULL;
}

static void kunit_results_free(struct igt_list_head *results,
			       char **suite_name, char **case_name)
{
	struct igt_ktap_result *r, *rn;

	igt_list_for_each_entry_safe(r, rn, results, link)
		kunit_result_free(&r, suite_name, case_name);

	free(*case_name);
	free(*suite_name);
}

static int kunit_get_results(struct igt_list_head *results, const char *debugfs_path,
			     const char *suite, struct igt_ktap_results **ktap)
{
	char results_path[PATH_MAX];
	FILE *results_stream;
	char *buf = NULL;
	size_t size = 0;
	ssize_t len;
	int err;

	if (igt_debug_on(strlen(debugfs_path) + strlen(suite) + strlen("/results") >= PATH_MAX))
		return -ENOSPC;

	strcpy(stpcpy(stpcpy(results_path, debugfs_path), suite), "/results");
	results_stream = fopen(results_path, "r");
	if (igt_debug_on(!results_stream))
		return -errno;

	*ktap = igt_ktap_alloc(results);
	if (igt_debug_on(!*ktap)) {
		err = -ENOMEM;
		goto out_fclose;
	}

	while (len = getline(&buf, &size, results_stream), len > 0) {
		err = igt_ktap_parse(buf, *ktap);
		if (err != -EINPROGRESS)
			break;
	}

	free(buf);

	igt_ktap_free(ktap);
out_fclose:
	fclose(results_stream);

	return err;
}

static void kunit_get_tests(struct igt_list_head *tests,
			    struct igt_ktest *tst,
			    const char *suite,
			    const char *opts,
			    const char *debugfs_path,
			    DIR **debugfs_dir,
			    struct igt_ktap_results **ktap)
{
	struct igt_ktap_result *r, *rn;
	struct dirent *subdir;
	unsigned long taints;

	*debugfs_dir = opendir(debugfs_path);
	if (igt_debug_on(!*debugfs_dir))
		return;

	/*
	 * To get a list of test cases provided by a kunit test module, ask the
	 * generic kunit module to respond with SKIP result for each test found.
	 * We could also try to use action=list kunit parameter to get the
	 * listing, however, parsing a structured KTAP report -- something that
	 * we already can do perfectly -- seems to be more safe than extracting
	 * a free text list of unknown length from /dev/kmsg.
	 */
	if (igt_debug_on(!kunit_set_filtering(suite, "module=none", "skip")))
		return;

	if (!suite) {
		seekdir(*debugfs_dir, 2);	/* directory itself and its parent */
		errno = 0;
		igt_skip_on_f(readdir(*debugfs_dir) || errno,
			      "Require empty KUnit debugfs directory\n");
		rewinddir(*debugfs_dir);
	}

	igt_skip_on(modprobe(tst->kmod, opts));
	igt_skip_on(igt_kernel_tainted(&taints));

	while (subdir = readdir(*debugfs_dir), subdir) {
		if (!(subdir->d_type & DT_DIR))
			continue;

		if (!strcmp(subdir->d_name, ".") || !strcmp(subdir->d_name, ".."))
			continue;

		if (suite && strcmp(subdir->d_name, suite))
			continue;

		igt_warn_on_f(kunit_get_results(tests, debugfs_path, subdir->d_name, ktap),
			      "parsing KTAP report from test suite \"%s\" failed\n",
			      subdir->d_name);

		if (suite)
			break;
	}

	closedir(*debugfs_dir);
	*debugfs_dir = NULL;

	igt_list_for_each_entry_safe(r, rn, tests, link)
		igt_require_f(r->code == IGT_EXIT_SKIP,
			      "Unexpected non-SKIP result while listing test cases\n");
}

static void __igt_kunit(struct igt_ktest *tst,
			const char *subtest,
			const char *opts,
			const char *debugfs_path,
			struct igt_list_head *tests,
			struct igt_ktap_results **ktap)
{
	struct igt_ktap_result *t;

	igt_list_for_each_entry(t, tests, link) {
		char *suite_name = NULL, *case_name = NULL;
		IGT_LIST_HEAD(results);
		unsigned long taints;

		igt_dynamic_f("%s%s%s",
			      strcmp(t->suite_name, subtest) ?  t->suite_name : "",
			      strcmp(t->suite_name, subtest) ? "-" : "",
			      t->case_name) {
			struct igt_ktap_result *r = NULL;
			char glob[1024];
			int i;

			igt_skip_on(kmod_module_remove_module(tst->kmod, 0));
			igt_skip_on(igt_kernel_tainted(&taints));

			igt_assert_lt(snprintf(glob, sizeof(glob), "%s.%s",
					       t->suite_name, t->case_name),
				      sizeof(glob));
			igt_assert(kunit_set_filtering(glob, NULL, NULL));

			igt_assert_eq(modprobe(tst->kmod, opts), 0);
			igt_assert_eq(igt_kernel_tainted(&taints), 0);

			igt_assert_eq(kunit_get_results(&results, debugfs_path,
							t->suite_name, ktap), 0);

			for (i = 0; i < 2; i++) {
				kunit_result_free(&r, &suite_name, &case_name);
				igt_fail_on(igt_list_empty(&results));

				r = igt_list_first_entry(&results, r, link);

				igt_fail_on_f(strcmp(r->suite_name, t->suite_name),
					      "suite_name expected: %s, got: %s\n",
					      t->suite_name, r->suite_name);
				igt_fail_on_f(strcmp(r->case_name, t->case_name),
					      "case_name expected: %s, got: %s\n",
					      t->case_name, r->case_name);

				if (r->code != IGT_EXIT_INVALID)
					break;

				/* result from parametrized test case */
			}

			igt_assert_neq(r->code, IGT_EXIT_INVALID);

			if (r->msg && *r->msg) {
				igt_skip_on_f(r->code == IGT_EXIT_SKIP,
					      "%s\n", r->msg);
				igt_fail_on_f(r->code == IGT_EXIT_FAILURE,
					      "%s\n", r->msg);
				igt_abort_on_f(r->code == IGT_EXIT_ABORT,
					      "%s\n", r->msg);
			} else {
				igt_skip_on(r->code == IGT_EXIT_SKIP);
				igt_fail_on(r->code == IGT_EXIT_FAILURE);
				if (r->code == IGT_EXIT_ABORT)
					igt_fail(r->code);
			}
			igt_assert_eq(r->code, IGT_EXIT_SUCCESS);
		}

		kunit_results_free(&results, &suite_name, &case_name);

		if (igt_debug_on(igt_kernel_tainted(&taints))) {
			igt_info("Kernel tainted, not executing more selftests.\n");
			break;
		}
	}
}

/**
 * igt_kunit:
 * @module_name: the name of the module
 * @suite: the name of test suite to be executed, also used as subtest name;
 *	   if NULL then test cases from all test suites provided by the module
 *	   are executed as dynamic sub-subtests of one IGT subtest, which name
 *	   is derived from the module name by cutting off its optional trailing
 *	   _test or _kunit suffix
 * @opts: options to load the module
 *
 * Loads the test module, parses its (k)tap dmesg output, then unloads it
 */
void igt_kunit(const char *module_name, const char *suite, const char *opts)
{
	char debugfs_path[PATH_MAX] = { '\0', };
	struct igt_ktest tst = { .kmsg = -1, };
	struct igt_ktap_results *ktap = NULL;
	DIR *debugfs_dir = NULL;
	char *subtest;
	IGT_LIST_HEAD(tests);

	/*
	 * If the caller (an IGT test) provides no test suite name then
	 * we take the module name, drop the trailing "_test" or "_kunit"
	 * suffix, if any, and use the result as our IGT subtest name.
	 */
	if (suite) {
		subtest = strdup(suite);
	} else {
		subtest = strdup(module_name);
		if (!igt_debug_on(!subtest)) {
			char *suffix = strstr(subtest, "_test");

			if (!suffix)
				suffix = strstr(subtest, "_kunit");

			if (suffix)
				*suffix = '\0';
		}
	}

	/* We need the base KUnit module loaded if not built-in */
	igt_ignore_warn(igt_kmod_load("kunit", NULL));
	kunit_debugfs_path(debugfs_path);

	igt_fixture() {
		igt_require(subtest);
		igt_require(*debugfs_path);

		igt_skip_on(igt_ktest_init(&tst, module_name));
		igt_skip_on(igt_ktest_begin(&tst));

		igt_assert(igt_list_empty(&tests));
	}

	/*
	 * We need to use igt_subtest here, as otherwise it may crash with:
	 * "skipping is allowed only in fixtures, subtests or igt_simple_main()"
	 * if used on igt_main(). This is also needed in order to provide
	 * proper namespace for dynamic subtests, with is required for CI
	 * and for documentation.
	 */
	igt_subtest_with_dynamic(subtest) {
		kunit_get_tests(&tests, &tst, suite, opts,
				debugfs_path, &debugfs_dir, &ktap);
		__igt_kunit(&tst, subtest, opts, debugfs_path, &tests, &ktap);
	}

	igt_fixture() {
		char *suite_name = NULL, *case_name = NULL;

		igt_ktap_free(&ktap);

		kunit_results_free(&tests, &suite_name, &case_name);

		if (debugfs_dir)
			closedir(debugfs_dir);

		igt_ktest_end(&tst);
	}

	free(subtest);
	igt_ktest_fini(&tst);
}

int igt_ktest_init(struct igt_ktest *tst,
		   const char *module_name)
{
	struct kmod_list *l = NULL;
	int err;

	memset(tst, 0, sizeof(*tst));

	tst->module_name = strdup(module_name);
	if (!tst->module_name)
		return 1;

	tst->kmsg = -1;

	err = kmod_module_new_from_lookup(kmod_ctx(), module_name, &l);

	/*
	 * Check for -ENOSYS to workaround bug in kmod_module_new_from_lookup()
	 * from libkmod <= 29
	 */
	if (err < 0 && err != -ENOSYS)
		return err;

	/*
	 * Lookup may not resolve to a module when used to just list subtests,
	 * where module is not available. Fallback to _new_from_name().
	 */
	if (!l)
		return kmod_module_new_from_name(kmod_ctx(), module_name, &tst->kmod);

	tst->kmod = kmod_module_get_module(l);
	kmod_module_unref_list(l);

	return 0;
}

int igt_ktest_begin(struct igt_ktest *tst)
{
	int err;

	if (strcmp(tst->module_name, "i915") == 0)
		igt_i915_driver_unload();

	err = kmod_module_remove_module(tst->kmod, 0);
	igt_require(err == 0 || err == -ENOENT);

	tst->kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);

	return 0;
}

int igt_kselftest_execute(struct igt_ktest *tst,
			  struct igt_kselftest_list *tl,
			  const char *options,
			  const char *result)
{
	unsigned long taints;
	char buf[1024];
	int err;

	igt_skip_on(igt_kernel_tainted(&taints));

	lseek(tst->kmsg, 0, SEEK_END);

	snprintf(buf, sizeof(buf), "%s=1 %s", tl->param, options ?: "");

	err = modprobe(tst->kmod, buf);
	if (err == 0 && result) {
		int dir = open_parameters(tst->module_name);
		igt_sysfs_scanf(dir, result, "%d", &err);
		close(dir);
	}
	if (err == -ENOTTY) /* special case */
		err = 0;
	if (err)
		kmsg_dump(tst->kmsg);

	kmod_module_remove_module(tst->kmod, 0);

	errno = 0;
	igt_assert_f(err == 0,
		     "kselftest \"%s %s\" failed: %s [%d]\n",
		     tst->module_name, buf, strerror(-err), -err);

	igt_assert_eq(igt_kernel_tainted(&taints), 0);

	return err;
}

void igt_ktest_end(struct igt_ktest *tst)
{
	kmod_module_remove_module(tst->kmod, 0);
	close(tst->kmsg);
}

void igt_ktest_fini(struct igt_ktest *tst)
{
	free(tst->module_name);
	kmod_module_unref(tst->kmod);
}

static const char *unfilter(const char *filter, const char *name)
{
	if (!filter)
		return name;

	name += strlen(filter);
	if (!isalpha(*name))
		name++;

	return name;
}

void igt_kselftests(const char *module_name,
		    const char *options,
		    const char *result,
		    const char *filter,
		    igt_kselftest_wrap_t wrapper)
{
	struct igt_ktest tst;
	IGT_LIST_HEAD(tests);
	struct igt_kselftest_list *tl, *tn;

	if (igt_ktest_init(&tst, module_name) != 0)
		return;

	igt_fixture()
		igt_require(igt_ktest_begin(&tst) == 0);

	igt_kselftest_get_tests(tst.kmod, filter, &tests);
	igt_subtest_with_dynamic(filter ?: "all-tests") {
		igt_list_for_each_entry_safe(tl, tn, &tests, link) {
			const char *dynamic_name = unfilter(filter, tl->name);
			unsigned long taints;

			igt_dynamic_f("%s", dynamic_name) {
				if (wrapper)
					wrapper(dynamic_name, &tst, tl);
				else
					igt_kselftest_execute(&tst, tl,
							      options, result);
			}
			free(tl);

			if (igt_kernel_tainted(&taints)) {
				igt_info("Kernel tainted, not executing more selftests.\n");
				break;
			}
		}
	}

	igt_fixture() {
		igt_ktest_end(&tst);
		igt_require(!igt_list_empty(&tests));
	}

	igt_ktest_fini(&tst);
}
