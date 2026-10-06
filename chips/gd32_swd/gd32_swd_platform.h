/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * INTERNAL: the platform seam of chips/gd32_swd.  NOT a public header (it is
 * deliberately outside include/): it hands out the reserved GD32 pads
 * (GD32G553_PAD_ID_SWDIO / _SWCLK / _NRST / _ATTN -- P70 / P71 / P74), which the
 * portable alp_gpio_open() refuses, so application code must not be able to
 * reach open_pad().  Included by chips/gd32_swd/gd32_swd.c, the platform glue
 * (src/zephyr/gd32_swd_pads.c, src/zephyr/v2n_supervisor.c) and the unit test.
 */

#ifndef ALP_CHIPS_GD32_SWD_PLATFORM_H
#define ALP_CHIPS_GD32_SWD_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "alp/chips/gd32_swd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How the driver opens the three GD32 pads.  A build without platform glue
 * answers ALP_ERR_NOSUPPORT from gd32_swd_init(). */
struct gd32_swd_platform {
	/* Open one reserved pad id; NULL when the board does not publish it. */
	alp_gpio_t *(*open_pad)(uint32_t pad_id);
	/* Switch the pad to an OUTPUT whose initial level is LOW with no high
	 * glitch (the NRST net is shared with the PMIC and open-drain). */
	alp_status_t (*configure_output_low)(alp_gpio_t *pad);
	/* Close a pad opened by open_pad. */
	void (*close_pad)(alp_gpio_t *pad);
};
typedef struct gd32_swd_platform gd32_swd_platform_t;

/* Platform glue entry: the pad seam, or NULL (weak default) when there is none. */
const gd32_swd_platform_t *gd32_swd_platform(void);

/* Session hook: an SWD session starts (true) or ends (false).  Weak no-op in the
 * driver; the platform glue that owns the GD32 bridge link overrides it
 * (src/zephyr/v2n_supervisor.c).  Called with true before gd32_swd_init() touches
 * any pad and with false when the pads are inputs again, so the bridge can be
 * closed, kept from re-initialising or renegotiating, and answer BUSY for the
 * whole session -- the bridge and this driver share P71. */
void gd32_swd_session_notify(bool active);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GD32_SWD_PLATFORM_H */
