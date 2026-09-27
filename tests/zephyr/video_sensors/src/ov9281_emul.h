/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only accessor into the OV9281 I2C emulator's register map -- see ov9281_emul.c.
 */
#ifndef TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV9281_EMUL_H_
#define TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV9281_EMUL_H_

#include <stdint.h>

#include <zephyr/drivers/emul.h>

/**
 * @brief Read one emulated OV9281 register.
 *
 * @param target The OV9281 emulator instance (EMUL_DT_GET(DT_NODELABEL(...))).
 * @param reg 16-bit CCI register address.
 * @param value Out param for the stored byte.
 * @return 0 on success, -EINVAL if @p reg is out of the emulated map or @p value is NULL.
 */
int ov9281_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value);

#endif /* TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV9281_EMUL_H_ */
