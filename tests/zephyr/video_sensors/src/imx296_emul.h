/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only accessor into the IMX296 I2C emulator's register map and ordered write log -- see
 * imx296_emul.c.
 */
#ifndef TESTS_ZEPHYR_VIDEO_SENSORS_SRC_IMX296_EMUL_H_
#define TESTS_ZEPHYR_VIDEO_SENSORS_SRC_IMX296_EMUL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

/**
 * @brief Read one emulated IMX296 register.
 *
 * @param target The IMX296 emulator instance (EMUL_DT_GET(DT_NODELABEL(...))).
 * @param reg 16-bit CCI register address.
 * @param value Out param for the stored byte.
 * @return 0 on success, -EINVAL if @p reg is out of the emulated map or @p value is NULL.
 */
int imx296_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value);

/* One recorded register write, in the order the driver issued it -- see imx296_emul_log_get(). */
struct imx296_emul_write {
	uint16_t reg;
	uint8_t  value;
};

/**
 * @brief Clear the ordered write log.
 *
 * Call at the start of a test that wants to inspect only the writes ITS OWN driver call made,
 * not whatever earlier tests (or imx296_init() at boot) left behind.
 *
 * @param target The IMX296 emulator instance.
 */
void imx296_emul_clear_log(const struct emul *target);

/**
 * @brief Number of register writes recorded since the last imx296_emul_clear_log() (or boot).
 *
 * Capped at the log's fixed capacity -- see imx296_emul.c.
 *
 * @param target The IMX296 emulator instance.
 * @return The recorded write count.
 */
size_t imx296_emul_log_count(const struct emul *target);

/**
 * @brief Read one entry from the ordered write log.
 *
 * @param target The IMX296 emulator instance.
 * @param index Zero-based index; 0 is the oldest recorded write since the last clear.
 * @param out Out param for the recorded register/value pair.
 * @return 0 on success, -EINVAL if @p index is out of range or @p out is NULL.
 */
int imx296_emul_log_get(const struct emul *target, size_t index, struct imx296_emul_write *out);

#endif /* TESTS_ZEPHYR_VIDEO_SENSORS_SRC_IMX296_EMUL_H_ */
