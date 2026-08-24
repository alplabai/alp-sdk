/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1623: pure computation of the STATUS acknowledge write-back value,
 * pulled out of rv3028c7.c so it is testable with no I2C bus at all
 * (mirrors src/common/alp_checked_arith.h's dependency-free pattern).
 */

#ifndef RV3028C7_STATUS_ACK_H
#define RV3028C7_STATUS_ACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute the STATUS write-back value that acknowledges only the
 *        bits observed in @p status.
 *
 * STATUS is write-0-to-clear.  Writing back `~status & 0x7F` clears
 * only the bits that were actually observed, leaving any bit that
 * latches between the STATUS read and this write (e.g. a one-shot
 * ALARM/EXT_EVENT/BSF) set to 1 -- i.e. untouched -- so it survives
 * the acknowledge and is dispatched on the next call.  Bit 7 is
 * reserved and is always written 0.
 *
 * @param status STATUS byte read immediately before dispatch.
 * @return Value to write back to STATUS.
 */
static inline uint8_t rv3028c7_status_ack_value(uint8_t status)
{
	return (uint8_t)(~status & 0x7Fu);
}

#ifdef __cplusplus
}
#endif

#endif /* RV3028C7_STATUS_ACK_H */
