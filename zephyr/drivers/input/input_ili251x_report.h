/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Pure, dependency-light parser for the ILI251x 0x10 touch-data report --
 * split out of input_ili251x.c so it is host-testable on native_sim without
 * a devicetree-instantiated device (the tests/unit/input_ili251x_report
 * precedent: zephyr/drivers/sdhc/sdhc_dwc.h's sdhc_dwc_realign_r2_response()
 * does the same split for the same reason).
 *
 * Byte layout (see the protocol facts this driver was clean-room authored
 * from -- ADR-0017-ADJACENT header block in input_ili251x.c):
 *   byte 0: 0 or 1 for a normal report, 2 if a 20-byte continuation packet
 *           follows (contacts 6-9; not read by this v1 single-touch parser).
 *   contact i's 5-byte record starts at offset (1 + 5*i):
 *     u16 BE at off:   bit15 = touching, bits13:0 = X (14-bit).
 *     u16 BE at off+2: Y (full 16-bit, no touch/reserved bit).
 *     u8  at off+4:    pressure, 0..0x0A.
 */

#ifndef ZEPHYR_DRIVERS_INPUT_INPUT_ILI251X_REPORT_H_
#define ZEPHYR_DRIVERS_INPUT_INPUT_ILI251X_REPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Contact 0 always lives in the first 31-byte read (offset 1), so parsing
 * it never needs the byte0==2 continuation bytes -- ponytail: contacts
 * 1-9 are unparsed (v1 is single-touch only); add real multi-touch slot
 * decoding here when an app needs more than one finger.
 */
#define ILI251X_CONTACT0_OFFSET 1U
#define ILI251X_CONTACT0_LEN    5U /* X(2) + Y(2) + pressure(1), bytes */
#define ILI251X_X_MASK          0x3FFFU
#define ILI251X_TOUCHING_BIT    BIT(15)

/** Parsed state of touch contact 0. */
struct ili251x_touch_report {
	uint16_t x;       /**< Raw controller X, controller-resolution units. */
	uint16_t y;       /**< Raw controller Y, controller-resolution units. */
	uint8_t pressure; /**< Contact pressure, 0..0x0A. */
	bool pressed;     /**< Contact 0 touching (bit 15 of the X word). */
};

/**
 * @brief Parse contact 0 out of an ILI251x 0x10 touch-data report.
 *
 * @param buf Report bytes, as read from register 0x10 (at least the first
 *            31-byte packet; the 20-byte continuation is never needed for
 *            contact 0 and is not consulted here).
 * @param len Number of valid bytes in @p buf.
 * @param out Parsed result. Zeroed (not pressed) if @p len is too short to
 *            contain contact 0's record.
 */
static inline void ili251x_parse_contact0(const uint8_t *buf, size_t len,
					  struct ili251x_touch_report *out)
{
	uint16_t x_word;

	if (len < ILI251X_CONTACT0_OFFSET + ILI251X_CONTACT0_LEN) {
		*out = (struct ili251x_touch_report){0};
		return;
	}

	x_word = sys_get_be16(&buf[ILI251X_CONTACT0_OFFSET]);
	out->pressed = (x_word & ILI251X_TOUCHING_BIT) != 0U;
	out->x = x_word & ILI251X_X_MASK;
	out->y = sys_get_be16(&buf[ILI251X_CONTACT0_OFFSET + 2]);
	out->pressure = buf[ILI251X_CONTACT0_OFFSET + 4];
}

#endif /* ZEPHYR_DRIVERS_INPUT_INPUT_ILI251X_REPORT_H_ */
