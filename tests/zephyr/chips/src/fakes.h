/*
 * Copyright 2026 Alp Lab AB
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
void fake_lsm6dso_set_reg(uint8_t reg, uint8_t val);

/** Reset all registers to the chip's power-on defaults. */
void fake_lsm6dso_reset(void);

/* ------------------------------------------------------------------ */
/* fake SSD1306                                                        */
/* ------------------------------------------------------------------ */

/** Number of command bytes the driver has streamed since the last reset. */
size_t         fake_ssd1306_cmd_log_len(void);
const uint8_t *fake_ssd1306_cmd_log(void);

/** Number of pixel-data bytes the driver has streamed since the last reset. */
size_t         fake_ssd1306_data_log_len(void);
const uint8_t *fake_ssd1306_data_log(void);

void fake_ssd1306_reset_logs(void);

/* ------------------------------------------------------------------ */
/* fake BME280                                                         */
/* ------------------------------------------------------------------ */

/** Read the current contents of register @p reg in the fake device. */
uint8_t fake_bme280_get_reg(uint8_t reg);

/** Force-write register @p reg in the fake device. */
void fake_bme280_set_reg(uint8_t reg, uint8_t val);

/** Reset all registers + the synthetic calibration block. */
void fake_bme280_reset(void);

/* ------------------------------------------------------------------ */
/* fake RV-3028-C7                                                     */
/* ------------------------------------------------------------------ */

/** Read/write the RAM register file (STATUS, CONTROL_1/2, EEADDR/
 *  EEDATA/EECMD, the EEPROM_CLKOUT/EEPROM_BACKUP RAM mirror, ...). */
uint8_t fake_rv3028c7_get_reg(uint8_t reg);
void    fake_rv3028c7_set_reg(uint8_t reg, uint8_t val);

/** Read/write the separate EEPROM backing store, indexed by EEADDR --
 *  distinct from the RAM mirror above; only an EECMD 0x21/0x22 commit/
 *  readback touches this. */
uint8_t fake_rv3028c7_get_eeprom(uint8_t addr);
void    fake_rv3028c7_set_eeprom(uint8_t addr, uint8_t val);

/** Ordered log of every APPLIED register write (reg, val) since the
 *  last fake_rv3028c7_wlog_reset() / fake_rv3028c7_reset() -- a write
 *  that failed via an armed fault (fake_rv3028c7_fail_next_write) is
 *  NOT appended. */
size_t  fake_rv3028c7_wlog_len(void);
uint8_t fake_rv3028c7_wlog_reg(size_t i);
uint8_t fake_rv3028c7_wlog_val(size_t i);
void    fake_rv3028c7_wlog_reset(void);

/** Arm a one-shot NACK for the next write that matches EXACTLY
 *  (reg, val) -- not just "the next write" -- so a test can fail one
 *  step of a multi-write commit sequence without disturbing the
 *  steps before it. */
void fake_rv3028c7_fail_next_write(uint8_t reg, uint8_t val);

/** Reset registers, EEPROM backing store, the write log, and any
 *  armed fault. */
void fake_rv3028c7_reset(void);

/* ------------------------------------------------------------------ */
/* fake TCAL9538 / TCA6408A                                            */
/* ------------------------------------------------------------------ */
/* Two instances are wired in the overlay (0x73 TCAL9538 strap, 0x20
 * TCA6408A alt-strap); every accessor takes the 7-bit address to
 * disambiguate which one. */

uint8_t  fake_tcal9538_get_reg(uint8_t addr, uint8_t reg);
void     fake_tcal9538_set_reg(uint8_t addr, uint8_t reg, uint8_t val);
uint32_t fake_tcal9538_write_count(uint8_t addr, uint8_t reg);
uint32_t fake_tcal9538_read_count(uint8_t addr, uint8_t reg);
/** Total I2C transactions (of any shape) this instance has seen. */
uint32_t fake_tcal9538_total_transactions(uint8_t addr);
void     fake_tcal9538_reset(uint8_t addr);

/* ------------------------------------------------------------------ */
/* fake ICM-42670-P                                                    */
/* ------------------------------------------------------------------ */

uint8_t  fake_icm42670_get_reg(uint8_t reg);
void     fake_icm42670_set_reg(uint8_t reg, uint8_t val);
uint32_t fake_icm42670_write_count(uint8_t reg);
void     fake_icm42670_reset(void);

/* ------------------------------------------------------------------ */
/* fake BMI323                                                         */
/* ------------------------------------------------------------------ */
/* 16-bit-per-register wire protocol -- see fake_bmi323.c. */

uint16_t fake_bmi323_get_reg(uint8_t reg);
void     fake_bmi323_set_reg(uint8_t reg, uint16_t val);
uint32_t fake_bmi323_write_count(uint8_t reg);
void     fake_bmi323_reset(void);

/* ------------------------------------------------------------------ */
/* fake BMP581                                                         */
/* ------------------------------------------------------------------ */

uint8_t  fake_bmp581_get_reg(uint8_t reg);
void     fake_bmp581_set_reg(uint8_t reg, uint8_t val);
uint32_t fake_bmp581_write_count(uint8_t reg);
void     fake_bmp581_reset(void);

/* ------------------------------------------------------------------ */
/* fake INA236                                                         */
/* ------------------------------------------------------------------ */
/* 16-bit big-endian register wire protocol -- see fake_ina236.c. */

uint16_t fake_ina236_get_reg(uint8_t reg);
void     fake_ina236_set_reg(uint8_t reg, uint16_t val);
uint32_t fake_ina236_read_count(uint8_t reg);
void     fake_ina236_reset(void);

/* ------------------------------------------------------------------ */
/* fake TAS2563                                                        */
/* ------------------------------------------------------------------ */
/* Book/page-paged register file -- see fake_tas2563.c.  get_reg /
 * set_reg / write_count address BOOK 0 / PAGE 0, the only page with a
 * backing store; the write log below is how a test sees writes aimed
 * at any other book or page. */

/** One applied register write, tagged with the book/page selected at
 *  the time -- so a test can assert the ORDER and the paging of a
 *  sequence, not just its end state. */
struct fake_tas2563_write {
	uint8_t book;
	uint8_t page;
	uint8_t reg;
	uint8_t val;
};

/** Write-log capacity.  Long enough for the tuning-replay sequences
 *  the ztests exercise; writes past it are dropped, not wrapped. */
#define FAKE_TAS2563_LOG_MAX 64

uint8_t  fake_tas2563_get_reg(uint8_t reg);
void     fake_tas2563_set_reg(uint8_t reg, uint8_t val);
uint32_t fake_tas2563_write_count(uint8_t reg);

/** Currently selected book/page, as the last PAGE/BOOK write left it. */
uint8_t fake_tas2563_cur_book(void);
uint8_t fake_tas2563_cur_page(void);

/** Ordered log of every APPLIED write since the last reset.  A write
 *  that a fake_tas2563_fail_write_at() fault NACKed is NOT logged. */
size_t                           fake_tas2563_log_len(void);
const struct fake_tas2563_write *fake_tas2563_log(size_t i);
void                             fake_tas2563_log_reset(void);

/** Force the fake's current book/page selection without going through
 *  a PAGE/BOOK write -- models a device left mid-tuning by a previous
 *  firmware, which software shutdown would preserve (SLASET3D
 *  §7.3.11.2, p.34). */
void fake_tas2563_force_paging(uint8_t book, uint8_t page);

/** Arm a one-shot NACK for the next write to (@p book, @p page,
 *  @p reg), so a test can fail one step of a multi-write sequence
 *  without disturbing the steps before it. */
void fake_tas2563_fail_write_at(uint8_t book, uint8_t page, uint8_t reg);

/** Reset registers to their datasheet POR values, clear the write
 *  log, the counters, the book/page selection and any armed fault. */
void fake_tas2563_reset(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_TEST_FAKES_H */
