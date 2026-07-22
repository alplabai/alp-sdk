/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private declarations shared between bme280.c and its ztest coverage.
 * Not installed, not part of the public API -- <alp/chips/bme280.h> is
 * the public surface.
 */

#ifndef BME280_INTERNAL_H
#define BME280_INTERNAL_H

#include <stdint.h>

/*
 * #797 coverage: the H4/H5 humidity coefficients share a nibble of
 * calibration register E5 (BST-BME280-DS002 v1.6 §5.4 Table 20) --
 * H4 = [E4][7:0] << 4 | [E5][3:0], H5 = [E6][7:0] << 4 | [E5][7:4].
 * The fake i2c-emul target that would otherwise exercise this through
 * bme280_init() is compiled out (see tests/zephyr/chips/CMakeLists.txt),
 * so the unpack is factored out of load_calibration() into a pure
 * function directly testable with a known E4/E5/E6 byte triple.
 *
 * bytes[0] = E4, bytes[1] = E5, bytes[2] = E6.  *h4 / *h5 receive the
 * sign-extended 12-bit results.
 */
void bme280_unpack_h4_h5(const uint8_t bytes[3], int16_t *h4, int16_t *h5);

#endif /* BME280_INTERNAL_H */
