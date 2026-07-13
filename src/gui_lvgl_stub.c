/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/gui.h> -- alp_gui_lvgl_attach() guard-clause implementation.
 *
 * The real LVGL display/input hand-off (binding an lv_display_t /
 * lv_disp_drv_t to alp_display_blit()) is not implemented yet -- see
 * issue #23's "alp_display_lvgl_attach(handle, lv_disp_drv_t *)" item
 * and the "planned alp_gui_lvgl_attach() bridge" note in
 * src/backends/display/zephyr_drv.c.  Until that lands, this
 * OS-agnostic guard clause ships the two contract paths that ARE
 * fully specified today (NULL display -> INVAL; no build wires the
 * real bridge yet -> NOSUPPORT) so the symbol links and degrades
 * predictably instead of leaving every caller with an unresolved
 * reference.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/23
 */

#include <stddef.h>

#include "alp/gui.h"

alp_status_t alp_gui_lvgl_attach(alp_display_t *display)
{
	if (display == NULL) return ALP_ERR_INVAL;

	/* No backend wires the real LVGL hand-off yet -- ALP_HAS_LVGL
     * today only controls whether <lvgl.h> is pulled into the header
     * (see <alp/gui.h>).  Degrade to NOSUPPORT rather than claim a
     * binding that doesn't exist. */
	return ALP_ERR_NOSUPPORT;
}
