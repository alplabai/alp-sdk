/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only accessor into the OV5647 I2C emulator's register map and ordered write log -- see
 * ov5647_emul.c.
 */
#ifndef TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV5647_EMUL_H_
#define TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV5647_EMUL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

/**
 * @brief Read one emulated OV5647 register.
 *
 * @param target The OV5647 emulator instance (EMUL_DT_GET(DT_NODELABEL(...))).
 * @param reg 16-bit CCI register address.
 * @param value Out param for the stored byte.
 * @return 0 on success, -EINVAL if @p reg is out of the emulated map or @p value is NULL.
 */
int ov5647_emul_get_reg(const struct emul *target, uint16_t reg, uint8_t *value);

/**
 * @brief Directly poke one emulated OV5647 register, bypassing the driver.
 *
 * #2277: stands in for whatever OUTSIDE the ov5647.c driver's own I2C traffic last touched a
 * register -- e.g. a vendor ISP library's own sensor-attach table, or (as bench-observed) the
 * sensor's own on-chip AEC overwriting its exposure registers once AEC-manual goes inactive --
 * so a test can reproduce "something clobbered this register" without needing that external
 * write path present in a native_sim unit test.
 *
 * @param target The OV5647 emulator instance.
 * @param reg 16-bit CCI register address.
 * @param value Byte to store.
 * @return 0 on success, -EINVAL if @p reg is out of the emulated map.
 */
int ov5647_emul_set_reg(const struct emul *target, uint16_t reg, uint8_t value);

/* One recorded register write, in the order the driver issued it -- see ov5647_emul_log_get(). */
struct ov5647_emul_write {
	uint16_t reg;
	uint8_t  value;
};

/**
 * @brief Clear the ordered write log.
 *
 * Call at the start of a test that wants to inspect only the writes ITS OWN driver call made,
 * not whatever earlier tests (or ov5647_init() at boot) left behind.
 *
 * @param target The OV5647 emulator instance.
 */
void ov5647_emul_clear_log(const struct emul *target);

/**
 * @brief Number of register writes recorded since the last ov5647_emul_clear_log() (or boot).
 *
 * Capped at the log's fixed capacity -- see ov5647_emul.c.
 *
 * @param target The OV5647 emulator instance.
 * @return The recorded write count.
 */
size_t ov5647_emul_log_count(const struct emul *target);

/**
 * @brief Read one entry from the ordered write log.
 *
 * @param target The OV5647 emulator instance.
 * @param index Zero-based index; 0 is the oldest recorded write since the last clear.
 * @param out Out param for the recorded register/value pair.
 * @return 0 on success, -EINVAL if @p index is out of range or @p out is NULL.
 */
int ov5647_emul_log_get(const struct emul *target, size_t index, struct ov5647_emul_write *out);

#endif /* TESTS_ZEPHYR_VIDEO_SENSORS_SRC_OV5647_EMUL_H_ */
