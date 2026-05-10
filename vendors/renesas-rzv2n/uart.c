/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Renesas RZ/V2N UART wrapper.  See i2c.c for rationale.
 * Real impl behind FSP's `r_sci_b_uart` lands v0.4.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_RENESAS_FSP)
#include "r_sci_uart.h"
/* Real UART body lands v0.4. */
#endif
