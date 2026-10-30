/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Google LLC.
 * Copyright © 2023 Collabora, Ltd.
 * Copyright © 2024 Red Hat, Inc.
 * Copyright © 2025 Intel Corporation
 */

#ifndef __IGT_CONFIGFS_H__
#define __IGT_CONFIGFS_H__

#include <stdbool.h>

const char *igt_configfs_mount(void);
int igt_configfs_open(const char *name);

int igt_configfs_create_device_group(int configfs_fd, const char *name, bool *preexisting);
void igt_configfs_close_device_group(int configfs_fd, const char *name, int device_fd,
				     bool preexisting);

#endif /* __IGT_CONFIGFS_H__ */
