/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n_cm33_release_ca55() -- CM33-side release of CA55 core 0 on the RZ/V2N
 * in CM33 cold boot (BOOTSELCPU low).  See ca55_release.c for the register
 * facts, their source, and what is still a TODO.
 */
#ifndef V2N_CA55_RELEASE_H
#define V2N_CA55_RELEASE_H

#include <stdint.h>

#include "alp/peripheral.h"

/**
 * @brief Release CA55 core 0 from reset with its reset vector at @p entry_addr.
 *
 * Call ONLY after da9292_ch2_sequence() returned ALP_OK: a rail failure must
 * leave the CA55 held, and that gate is the caller's job.  The image at
 * @p entry_addr (CM33-staged TF-A BL2 in SRAM2 or DDR) must already be in
 * place; this function neither copies nor verifies it.
 *
 * Fails closed: every error before the CPG reset-release writes leaves the
 * CA55 held.  Errors after them cannot re-hold a running core.
 *
 * @param entry_addr  40-bit CA55 reset vector (RVBARADDR0), 4-byte aligned,
 *                    non-zero.
 * @retval ALP_OK             Cluster and core 0 accepted the power request.
 * @retval ALP_ERR_INVAL      @p entry_addr zero, unaligned or above 40 bits.
 * @retval ALP_ERR_NOSUPPORT  The PD_OTHERS/PD_CA55/PD_DDR0 AWO to ALL_ON entry
 *                            is not implemented yet (see the TODO); nothing
 *                            was written.
 * @retval ALP_ERR_TIMEOUT    A bounded poll expired.
 * @retval ALP_ERR_IO         The cluster or core denied the power request.
 */
alp_status_t v2n_cm33_release_ca55(uint64_t entry_addr);

#endif /* V2N_CA55_RELEASE_H */
