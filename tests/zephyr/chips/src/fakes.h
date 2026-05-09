/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-side inspection helpers exposed by the fake i2c-emul targets
 * in this test (fake_lsm6dso.c / fake_ssd1306.c / fake_bme280.c).
 * Only used by tests/zephyr/chips/src/main.c.
 */

#ifndef ALP_TEST_FAKES_H
#define ALP_TEST_FAKES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* fake LSM6DSO                                                        */
/* ------------------------------------------------------------------ */

/** Read the current contents of register @p reg in the fake device. */
uint8_t fake_lsm6dso_get_reg(uint8_t reg);

/** Force-write register @p reg in the fake device (e.g. to seed
 *  synthetic accel/gyro samples before the driver reads them). */
void    fake_lsm6dso_set_reg(uint8_t reg, uint8_t val);

/** Reset all registers to the chip's power-on defaults. */
void    fake_lsm6dso_reset(void);

/* ------------------------------------------------------------------ */
/* fake SSD1306                                                        */
/* ------------------------------------------------------------------ */

/** Number of command bytes the driver has streamed since the last reset. */
size_t        fake_ssd1306_cmd_log_len(void);
const uint8_t *fake_ssd1306_cmd_log(void);

/** Number of pixel-data bytes the driver has streamed since the last reset. */
size_t        fake_ssd1306_data_log_len(void);
const uint8_t *fake_ssd1306_data_log(void);

void          fake_ssd1306_reset_logs(void);

/* ------------------------------------------------------------------ */
/* fake BME280                                                         */
/* ------------------------------------------------------------------ */

/** Read the current contents of register @p reg in the fake device. */
uint8_t fake_bme280_get_reg(uint8_t reg);

/** Force-write register @p reg in the fake device. */
void    fake_bme280_set_reg(uint8_t reg, uint8_t val);

/** Reset all registers + the synthetic calibration block. */
void    fake_bme280_reset(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_TEST_FAKES_H */
