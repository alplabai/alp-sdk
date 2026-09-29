/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-sdk#2256: isp_configure()'s TPG branch (zephyr/drivers/video/isp_pico.c)
 * used to leave port->port_fmt.width/height at 0 for every TPG image, so
 * VSI_MPI_ISP_SetChnAttr got a 0x0 port rect and refused it with -EINVAL
 * ("Setting the Channel config failed!", bench-confirmed on E1M-AEN803).
 * isp_tpg_geometry_is_valid() is the dependency-free guard isp_configure()
 * now calls before accepting DT tpg-width/tpg-height. Boundary-tested here on
 * the host, with no MMIO/devicetree/hal_alif libisp link involved.
 *
 * This guard only catches the 0x0 regression -- it does NOT know the TPG
 * pattern's real internal frame size (that stays inside the closed
 * libisp/VSI middleware per the issue's finding 2), so it accepts any
 * non-zero geometry a board overlay supplies.
 */
#include <zephyr/ztest.h>

/*
 * isp_pico.h pulls in the upstream Zephyr video ctrl registry's
 * "video_ctrls.h" (see the test's CMakeLists.txt), whose struct video_ctrl
 * embeds a struct video_ctrl_range the header itself does not declare --
 * isp_pico.c gets it via <zephyr/drivers/video-controls.h>, included first
 * there; do the same here so isp_pico.h parses standalone.
 */
#include <zephyr/drivers/video-controls.h>
#include "isp_pico.h"

ZTEST_SUITE(isp_pico_tpg_geometry, NULL, NULL, NULL, NULL, NULL);

struct geometry_case {
	const char *name;
	uint32_t    width;
	uint32_t    height;
	bool        want_valid;
};

static const struct geometry_case cases[] = {
	/* The exact regression: DT tpg-width/tpg-height left unset. */
	{ "unset (0x0)", 0, 0, false },

	/* One dimension unset. */
	{ "zero width only", 0, 720, false },
	{ "zero height only", 1280, 0, false },

	/* A plausible silicon-confirmed geometry, once known. */
	{ "1280x720", 1280, 720, true },
	{ "1920x1080", 1920, 1080, true },

	/* Smallest non-degenerate geometry. */
	{ "1x1", 1, 1, true },
};

ZTEST(isp_pico_tpg_geometry, test_geometry_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct geometry_case *tc = &cases[i];
		bool                        ok = isp_tpg_geometry_is_valid(tc->width, tc->height);

		zassert_equal(ok,
		              tc->want_valid,
		              "case '%s': expected valid=%d, got %d",
		              tc->name,
		              tc->want_valid,
		              ok);
	}
}
