/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2035: the BMP581 health criterion this demo's Sensors phase judges on,
 * pulled out of main.c into its own header so it is unit-testable without a
 * board -- main.c only builds against the real E1M-AEN801 target, but this
 * function takes plain register bytes, no I2C.
 *
 * A bench session once reported a BMP581 BROKEN off STATUS (0x28) reading
 * 0x02. That is DEEP STANDBY, the part's documented power-on/soft-reset
 * resting state (BST-BMP581-DS004-13 Rev 1.13 §4.3 p.16, §4.3.2 p.16), and
 * 0x02 is that register's own documented reset value (§7.22 p.58) -- not a
 * fault. Bosch's own bmp5_init() (BMP5_SensorAPI) and upstream Zephyr's
 * bmp581 driver both call the part ready on exactly two bits: nvm_rdy set
 * and nvm_err clear. This header matches that, and only that -- see
 * chips/bmp581/bmp581.c's "do not gate on core_rdy" note for why bit0
 * (core_rdy) plays no part here: it is defined once in the whole datasheet
 * and no Bosch or Zephyr procedure ever waits on it.
 */
#ifndef ALP_EVK_DEMO_BMP581_VERDICT_H
#define ALP_EVK_DEMO_BMP581_VERDICT_H

#include <stdbool.h>
#include <stdint.h>

#define BMP581_DIAG_REG_STATUS      0x28u
#define BMP581_DIAG_STATUS_CORE_RDY 0x01u /* bit0 -- NOT a readiness signal, see above. */
#define BMP581_DIAG_STATUS_NVM_RDY  0x02u /* bit1 */
#define BMP581_DIAG_STATUS_NVM_ERR  0x04u /* bit2 */

/* True iff STATUS's two documented health bits say the part is fit:
 * nvm_rdy set (NVM/trim data loaded) and nvm_err clear (no NVM fault).
 * Does NOT look at core_rdy, and does NOT know anything about a live
 * reading -- a caller still needs its own bus-op return codes and a
 * sanity check on the reading itself (this demo's bmp581_raw_invalid())
 * to catch a real bus failure or a bad conversion. */
static inline bool bmp581_status_is_healthy(uint8_t bstatus)
{
	return (bstatus & BMP581_DIAG_STATUS_NVM_RDY) != 0 &&
	       (bstatus & BMP581_DIAG_STATUS_NVM_ERR) == 0;
}

#endif /* ALP_EVK_DEMO_BMP581_VERDICT_H */
