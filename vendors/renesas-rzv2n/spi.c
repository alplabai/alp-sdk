/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Renesas RZ/V2N SPI wrapper.  See vendors/renesas-rzv2n/i2c.c
 * for the gating / scaffolding rationale.  V0.4 adds the real impl
 * once the FSP `r_rspi` driver is pulled into the build.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_RENESAS_FSP)
#include "r_spi.h"
/* Real SPI body lands in v0.4 once the FSP pack is wired into CI. */
#endif
