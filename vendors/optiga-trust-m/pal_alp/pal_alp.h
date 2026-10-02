/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-side hooks of the OPTIGA Trust M PAL (pal_alp.c).  Internal to
 * chips/optiga_trust_m; not a public header.
 */
#ifndef ALP_OPTIGA_PAL_ALP_H
#define ALP_OPTIGA_PAL_ALP_H

#include <stdbool.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Point the library's single IFX I2C context at @p bus / @p addr_7bit. */
void alp_optiga_pal_bind(alp_i2c_t *bus, uint8_t addr_7bit);

/** Run the library's pending timed callback if it is due.
 *  @return true if a callback ran. */
bool alp_optiga_pal_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_OPTIGA_PAL_ALP_H */
