/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for the DRP-AI driver's uapi header
 * (linux/drpai.h, shipped into the RZ/V sysroot by meta-rz-drpai's
 * drpai_1.4.0 recipe -- see src/yocto/inference_drpai.cpp's own file
 * header comment).  NOT vendor/kernel source: that recipe is a Yocto
 * layer, not installed on a stock CI runner (issue #1747, same
 * "no PR gate builds this backend" problem the ORT/DEEPX fakes solve).
 *
 * Reconstructed clean-room from what src/yocto/inference_drpai.cpp's
 * own doc comment documents itself as using: a `drpai_data_t` with
 * `.address`/`.size` fields (DRPAI_GET_DRPAI_AREA's out-param) and the
 * ioctl request macro itself. The request value need not match the
 * real driver's -- this file's `::ioctl()` call is never reached with
 * a live fd in these tests (there is no /dev/drpai0 on a CI runner, so
 * `_drpai_mem_start()` always fails at the `::open()` above it; see
 * inference_drpai_regression.cpp's header comment).
 */
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "drpai_test_seam.h"

typedef struct drpai_data {
	unsigned long address;
	unsigned long size;
} drpai_data_t;

#define DRPAI_GET_DRPAI_AREA 0x4010c401UL

/* inference_drpai.cpp reaches the device with `::open("/dev/drpai0")` and
 * `::ioctl()`.  Route both to the fakes below for the file that includes
 * this header (it is included AFTER the real <fcntl.h>/<sys/ioctl.h>, so
 * only later call sites are rewritten).  With drpai_test::g_device_present
 * false they behave like a host with no DRP-AI: ENOENT. */
inline int alp_fake_drpai_open(const char *path, int flags)
{
	(void)path;
	if (!drpai_test::g_device_present) {
		errno = ENOENT;
		return -1;
	}
	return ::open("/dev/null", flags);
}

inline int alp_fake_drpai_ioctl(int fd, unsigned long req, drpai_data_t *area)
{
	(void)fd;
	if (req != DRPAI_GET_DRPAI_AREA) {
		errno = ENOTTY;
		return -1;
	}
	area->address = static_cast<unsigned long>(drpai_test::g_area_base);
	area->size    = static_cast<unsigned long>(drpai_test::g_area_size);
	return 0;
}

#define open(path, flags) alp_fake_drpai_open((path), (flags))
#define ioctl(fd, req, a) alp_fake_drpai_ioctl((fd), (req), (a))
