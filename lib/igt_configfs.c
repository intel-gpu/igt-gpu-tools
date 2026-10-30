// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Google LLC.
 * Copyright © 2023 Collabora, Ltd.
 * Copyright © 2024 Red Hat, Inc.
 * Copyright © 2025 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "igt_aux.h"
#include "igt_configfs.h"
#include "igt_fs.h"

/**
 * SECTION:igt_configfs
 * @short_description: Support code for configfs features
 * @title: configfs
 * @include: igt_configfs.h
 *
 * This library provides helpers to access configfs features.
 */

/*
 * General configfs helpers
 */

static const char *__igt_configfs_mount(void)
{
	if (igt_is_mountpoint("/sys/kernel/config"))
		return "/sys/kernel/config";

	if (igt_is_mountpoint("/config"))
		return "/config";

	if (mount("config", "/sys/kernel/config", "configfs", 0, 0))
		return NULL;

	return "/sys/kernel/config";
}

/**
 * igt_configfs_mount:
 *
 * This attempts to locate where configfs is mounted on the filesystem,
 * and if not found, will then try to mount configfs at /sys/kernel/config.
 *
 * Returns:
 * The path to the configfs mount point (e.g. /sys/kernel/config)
 */
const char *igt_configfs_mount(void)
{
	static const char *path;

	if (!path)
		path = __igt_configfs_mount();

	return path;
}

/**
 * igt_configfs_open: open configfs path
 * @name: name of the configfs directory
 *
 * Opens the configfs directory corresponding to the name
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_configfs_open(const char *name)
{
	char path[PATH_MAX];
	const char *configfs_path;

	configfs_path = igt_configfs_mount();
	if (!configfs_path) {
		igt_debug("configfs not mounted, errno=%d\n", errno);
		return -1;
	}

	snprintf(path, sizeof(path), "%s/%s", configfs_path, name);

	return open(path, O_DIRECTORY | O_RDONLY);
}

/**
 * igt_configfs_create_device_group: create or reuse a device's configfs group
 * @configfs_fd: configfs directory fd
 * @name: name of the device group
 * @preexisting: set to true when @name already existed
 *
 * Creates the configfs group @name under @configfs_fd. If the group already
 * exists, it is opened and reused instead.
 *
 * Returns:
 * The group directory fd.
 */
int igt_configfs_create_device_group(int configfs_fd, const char *name,
				     bool *preexisting)
{
	mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	int device_fd;

	device_fd = igt_fs_create_dir(configfs_fd, name, mode);
	if (device_fd == -EEXIST) {
		if (preexisting)
			*preexisting = true;

		device_fd = openat(configfs_fd, name, O_DIRECTORY);
		if (device_fd < 0)
			device_fd = -errno;
	}
	igt_assert_f(device_fd >= 0,
		     "Failed to create/open configfs group for %s: %s\n",
		     name, strerror(-device_fd));

	return device_fd;
}

/**
 * igt_configfs_close_device_group: close a device's configfs group
 * @configfs_fd: configfs directory fd, as returned by igt_configfs_open()
 * @name: name of the device group
 * @device_fd: group directory fd
 * @preexisting: whether the group already existed before it was created by
 * igt_configfs_create_device_group()
 *
 * Closes @device_fd and removes the @name group, unless it was pre-existing.
 */
void igt_configfs_close_device_group(int configfs_fd, const char *name,
				     int device_fd, bool preexisting)
{
	int ret;

	close(device_fd);

	if (!preexisting) {
		ret = igt_fs_remove_dir(configfs_fd, name);
		if (ret)
			igt_debug("Failed to remove configfs group %s: %s\n",
				  name, strerror(-ret));
	}
}
