/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Renesas RZ/V2N GPIO wrapper.  See i2c.c for rationale.
 * Real impl behind FSP's `r_ioport` lands v0.4.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_RENESAS_FSP)
#include "r_ioport.h"
/* Real GPIO body lands v0.4. */
#endif
