/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-testable coverage for ili251x_parse_contact0()
 * (zephyr/drivers/input/input_ili251x_report.h) -- the pure parser
 * input_ili251x.c's ili251x_process() calls to decode contact 0 out of an
 * ILI251x 0x10 touch-data report.
 */
#include <zephyr/ztest.h>

#include "input_ili251x_report.h"

ZTEST_SUITE(input_ili251x_report, NULL, NULL, NULL, NULL, NULL);

/* No contact down: bit 15 of the X word clear -> pressed false. */
ZTEST(input_ili251x_report, test_no_touch)
{
	uint8_t buf[ILI251X_CONTACT0_OFFSET + ILI251X_CONTACT0_LEN] = {
		0,          /* byte 0: no continuation */
		0x12, 0x34, /* X word (BE): bit15 clear -> not touching */
		0x00, 0x00, /* Y word (BE) */
		0x00,       /* pressure */
	};
	struct ili251x_touch_report report;

	ili251x_parse_contact0(buf, sizeof(buf), &report);

	zassert_false(report.pressed, "bit15 clear must decode to not-pressed");
}

/* Contact 0 touching: bit15 set, X masked to the low 14 bits (bit14 is
 * reserved/ignored, not part of X -- protocol facts).
 */
ZTEST(input_ili251x_report, test_contact0_touching_x_masked_to_14_bits)
{
	uint8_t buf[ILI251X_CONTACT0_OFFSET + ILI251X_CONTACT0_LEN] = {
		0,    0xBF, 0xFF, /* X word (BE) = 0xBFFF: bit15 set, all 14 X bits set */
		0x00, 0x00, 0x00,
	};
	struct ili251x_touch_report report;

	ili251x_parse_contact0(buf, sizeof(buf), &report);

	zassert_true(report.pressed, "bit15 set must decode to pressed");
	zassert_equal(report.x, 0x3FFF, "X must be masked to the low 14 bits, got 0x%04x", report.x);
}

/* Big-endian decoding: X/Y/pressure at known offsets, values chosen so a
 * little-endian misread would produce different (wrong) numbers.
 */
ZTEST(input_ili251x_report, test_be_decoding)
{
	uint8_t buf[ILI251X_CONTACT0_OFFSET + ILI251X_CONTACT0_LEN] = {
		0,    0x80, 0x05, /* X word (BE) = 0x8005: pressed, X = 0x0005 = 5 */
		0x01, 0x02,       /* Y word (BE) = 0x0102 = 258 (LE would read 0x0201 = 513) */
		0x07,             /* pressure = 7 */
	};
	struct ili251x_touch_report report;

	ili251x_parse_contact0(buf, sizeof(buf), &report);

	zassert_true(report.pressed, NULL);
	zassert_equal(report.x, 5, "got X=%u", report.x);
	zassert_equal(report.y, 258, "got Y=%u", report.y);
	zassert_equal(report.pressure, 7, "got pressure=%u", report.pressure);
}

/* Too short a buffer to contain contact 0's record -> zeroed, not-pressed
 * (defensive default; the driver never passes a report this short in
 * practice, but the parser must not read out of bounds).
 */
ZTEST(input_ili251x_report, test_short_buffer_is_zeroed)
{
	uint8_t                     buf[3] = { 0, 0xFF, 0xFF };
	struct ili251x_touch_report report = { .x = 1, .y = 1, .pressure = 1, .pressed = true };

	ili251x_parse_contact0(buf, sizeof(buf), &report);

	zassert_false(report.pressed, NULL);
	zassert_equal(report.x, 0, NULL);
	zassert_equal(report.y, 0, NULL);
	zassert_equal(report.pressure, 0, NULL);
}
