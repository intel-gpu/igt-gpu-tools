// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: VRAM page offline injection and verification
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: RAS
 * Functionality: memory page offline
 * Test category: functionality test
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "igt.h"
#include "igt_debugfs.h"
#include "igt_device.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"

#include "xe/xe_query.h"

#define FAULT_INJECT_DIR	"inject_mempage_offline"
#define TRIGGER_FILE		"inject_mempage_offline_trigger"
#define VRAM_BAD_PAGES		"vram_bad_pages"

static int count_bad_pages(int fd)
{
	char buf[4096];
	int count = 0;
	char *line;

	/* Read file into buf. It automatically null-terminates inside. */
	__igt_debugfs_read(fd, VRAM_BAD_PAGES, buf, sizeof(buf));

	/* Check if the buffer is empty or could not be read */
	if (buf[0] == '\0')
		return 0;

	/* Each entry line has format: "0x%08x : 0x%08x : %c\n" */
	line = buf;
	while ((line = strchr(line, '\n')) != NULL) {
		count++;
		line++;
	}

	/* Subtract the header line (max_pages : XXXX) */
	if (count > 0)
		count--;

	return count;
}

static bool bad_page_has_flag(int fd, char flag)
{
	char buf[4096];
	char *p;

	__igt_debugfs_read(fd, VRAM_BAD_PAGES, buf, sizeof(buf));

	if (buf[0] == '\0')
		return false;

	/* Look for the flag character after last ':' on entry lines */
	p = buf;
	while ((p = strstr(p, " : ")) != NULL) {
		p += 3;
		if (*p == flag)
			return true;
	}

	return false;
}

static uint64_t get_last_bad_page_pfn(int fd)
{
	char buf[4096];
	char *line, *last_entry = NULL;
	uint64_t pfn = 0;

	__igt_debugfs_read(fd, VRAM_BAD_PAGES, buf, sizeof(buf));

	if (buf[0] == '\0')
		return 0;

	/* Find last entry line (format: "0x%08llx : 0x%08llx : %c\n") */
	line = buf;
	while (*line) {
		if (line[0] == '0' && line[1] == 'x')
			last_entry = line;
		line = strchr(line, '\n');
		if (!line)
			break;
		line++;
	}

	if (last_entry)
		sscanf(last_entry, "0x%lx", &pfn);

	return pfn;
}

static void inject_fault(int fd)
{
	igt_debugfs_write(fd, FAULT_INJECT_DIR "/probability", "100");
	igt_debugfs_write(fd, FAULT_INJECT_DIR "/times", "1");
	igt_debugfs_write(fd, FAULT_INJECT_DIR "/verbose", "1");
	__igt_debugfs_write(fd, TRIGGER_FILE, "0", 1);
}

/**
 * SUBTEST: inject-single-page
 * Description: Inject a single VRAM page fault and verify it appears in vram_bad_pages
 */

/**
 * SUBTEST: inject-duplicate-page
 * Description: Inject two pages and verify both appear in vram_bad_pages
 */

int igt_main()
{
	int fd;
	uint16_t devid;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_assert(fd >= 0);
		devid = intel_get_drm_devid(fd);
		igt_require(intel_get_device_info(devid)->is_crescentisland);
		igt_require(igt_debugfs_exists(fd,
					       FAULT_INJECT_DIR "/probability",
					       O_WRONLY));
		igt_require(igt_debugfs_exists(fd, VRAM_BAD_PAGES, O_RDONLY));
		igt_require(igt_debugfs_exists(fd, TRIGGER_FILE, O_WRONLY));
	}

	igt_subtest("inject-single-page") {
		int initial_count, after_count;

		initial_count = count_bad_pages(fd);
		inject_fault(fd);
		after_count = count_bad_pages(fd);
		igt_assert_eq(after_count, initial_count + 1);
		igt_assert(bad_page_has_flag(fd, 'R'));
	}

	igt_subtest("inject-duplicate-page") {
		int initial_count, after_count;
		uint64_t pfn;
		char pfn_str[32];
		int debugfs_fd, ret;

		initial_count = count_bad_pages(fd);

		/* First injection — auto-pick unallocated page */
		inject_fault(fd);
		after_count = count_bad_pages(fd);
		igt_assert_eq(after_count, initial_count + 1);

		/* Get PFN of the page just injected */
		pfn = get_last_bad_page_pfn(fd);
		igt_assert(pfn != 0);

		/* Reset / override the allowed fault count to 2 (or more) */
		if (igt_debugfs_exists(fd, FAULT_INJECT_DIR "/times", O_WRONLY)) {
			igt_debugfs_write(fd, FAULT_INJECT_DIR "/times", "2");
		}
		/*
		 * Re-inject same PFN — should return -EEXIST (soft→hard
		 * promotion). Write to trigger with the PFN value.
		 */
		snprintf(pfn_str, sizeof(pfn_str), "0x%lx", pfn);
		debugfs_fd = igt_debugfs_open(fd, TRIGGER_FILE, O_WRONLY);
		igt_assert(debugfs_fd >= 0);
		ret = write(debugfs_fd, pfn_str, strlen(pfn_str));
		igt_assert(ret < 0 && errno == EEXIST);
		close(debugfs_fd);

		/* Count should not change — same page, not a new one */
		after_count = count_bad_pages(fd);
		igt_assert_eq(after_count, initial_count + 1);
	}

	igt_fixture() {
		igt_debugfs_write(fd, FAULT_INJECT_DIR "/probability", "0");
		drm_close_driver(fd);
		igt_xe_driver_unload();
		igt_assert_eq(igt_xe_driver_load(NULL), 0);
	}
}
