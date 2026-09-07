/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-bmi323-regcheck -- settles WHY the BMI323 returns invalid accel data
 * (all three axes 0x8000) on real E1M-AEN803 silicon (serial 2026W36-0002),
 * for the E1M-AEN801/AEN803 (Alif Ensemble E8), bench RAM-run via J-Link.
 *
 * This is a DIAGNOSTIC INSTRUMENT, not a driver fix -- chips/bmi323/bmi323.c
 * is untouched by this app.
 *
 * The problem
 * -----------
 * examples/peripheral-io/i2c-device-hub reports on this silicon:
 *
 *   [devhub] BMI323 @0x68 id=0x43 accel{-32768,-32768,-32768} cfg_rc=0 rs=0
 *   STALE/RESET DATA
 *
 * The acc_mode repair in bmi323_set_accel() (chips/bmi323/bmi323.c) is
 * datasheet-correct (ACC_CONF bits[14:12] = 0b100, BST-BMI323-DS000-13
 * Rev 1.7 pp.21-22, 61, 88-91) and the compiled object provably contains it
 * -- yet every axis reads exactly 0x8000, the documented invalid/no-sample
 * sentinel (Rev 1.7, Table 2 p.9).
 *
 * CHIP_ID == 0x43 proves NOTHING about writes: CHIP_ID's reset value is
 * already 0x0043 (Rev 1.7, Table 36 p.61), readable straight out of POR
 * with no reset at all. A part that ACKs every write without applying any
 * of them looks electrically identical on a CHIP_ID check. Nobody has ever
 * read a BMI323 register back after configuring it -- this app does.
 *
 * What it does
 * ------------
 * Configures the accelerometer through the SAME public driver calls as
 * i2c-device-hub -- bmi323_init(), bmi323_set_accel() with the same ODR/FS,
 * the same wait -- then reads back three registers, in a specific order,
 * BEFORE touching the data registers:
 *
 *   1. ACC_CONF (0x20) -- the discriminator: did the acc_mode write land?
 *   2. ERR_REG  (0x01) -- bit0 fatal_err, bit5 acc_conf_err.
 *   3. STATUS   (0x02) -- bit7 drdy_acc, bit0 por_detected.
 *   4. Only THEN bmi323_read_accel() -- the accelerometer data itself.
 *
 * This order is NOT negotiable. STATUS bits are clear-on-read (Rev 1.7
 * p.66, "This flag is clear-on-read" on every STATUS field), and reading
 * ACC_DATA_X..Z ALSO clears drdy_acc, independently of a STATUS read (Rev
 * 1.7 p.23, "Accelerometer Data Ready Notification": "The flag
 * STATUS.drdy_acc is cleared when any of the registers ACC_DATA_X to
 * ACC_DATA_Z is read."). Reading the data registers first would silently
 * destroy the very evidence (drdy_acc) this app exists to capture -- so
 * ACC_CONF and ERR_REG (which have no read side effect) come first, STATUS
 * next, and the data registers last, always.
 *
 * ACC_CONF is also sampled once BEFORE bmi323_set_accel() runs, so the log
 * shows a genuine before/after, and STATUS is sampled a SECOND time ~100 ms
 * after the first (~17 ms) sample -- if drdy_acc is 0 at 17 ms but 1 at
 * 100 ms, the fix is a longer post-config wait, not a register-write bug,
 * and that distinction is free to capture here.
 *
 * How registers are read
 * -----------------------
 * chips/bmi323/bmi323.c's reg_read_u16() is `static` -- the driver has no
 * public raw-register accessor (its whole job is to hide the register
 * map), so this read-only bench probe talks to the part directly via
 * alp_i2c_write_read() at BMI323_I2C_ADDR_LOW (0x68), exactly as
 * chips/bmi323/bmi323.c's own reg_read16() does internally.
 *
 * The BMI323 read protocol prepends TWO DUMMY BYTES ahead of the requested
 * data on every read (BST-BMI323-DS000-13 Rev 1.7, Table 53 p.204: after
 * the repeated START + register address, the slave clocks out "dummy byte
 * ACK dummy byte ACK Register N Data[7:0] ACK Register N Data[15:8]"). For
 * one 16-bit register this app requests 2 (dummy) + 2 (data) = 4 bytes and
 * discards the first 2. Getting that discard count wrong is exactly what
 * produced a false 0x0000 CHIP_ID reading earlier this investigation (see
 * bmi323_init()'s comment in chips/bmi323/bmi323.c) -- a raw sweep that
 * didn't know about the two-byte prefix misread a live register as
 * "always zero" and made a dead write path look plausible for a whole
 * bench session.
 *
 * Verdict
 * -------
 * This app's job is to MEASURE, not to vindicate the sensor. RESULT PASS
 * means every read-back transaction completed (ALP_OK) and its value was
 * printed + decoded -- even if the decoded value proves the accelerometer
 * is broken. RESULT FAIL means a read-back ITSELF failed at the bus level
 * (NACK / timeout / rc != ALP_OK), which is the one outcome that means
 * this app could not gather the evidence it exists to gather.
 *
 * Bus
 * ---
 * BMI323 (U13) sits on the CARRIER bus -- SoC I2C0 pad group, portable
 * alias alp-i2c0 (ALP_E1M_I2C0 / EVK_I2C_BUS_SENSORS, portable bus index 0)
 * -- at 7-bit address 0x68 (EVK_I2C_ADDR_BMI323 == BMI323_I2C_ADDR_LOW;
 * confirmed on E1M-AEN803 serial 2026W36-0002, no 0x69 collision on this
 * respin batch, per <alp/boards/alp_e1m_evk_routes.h>). The board layer
 * already enables this bus (metadata/e1m_modules/aen/on-module-links.yaml
 * `e1m_i2c0` entry) -- this app's own overlay carries only the bench ITCM
 * retarget.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf's comment) when
 * the bench forces it; Flow C's app UART emits nothing on this bench
 * (e1m-aen-evk-03). BENCH-VALIDATION app -- not a customer teaching
 * example.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/bmi323.h"

/*
 * Raw register facts this app reads OUTSIDE the bmi323 driver's public API
 * -- chips/bmi323/bmi323.c's reg_read_u16() is `static`, and the driver
 * deliberately exposes no raw-register accessor, so a read-only bench probe
 * that needs ACC_CONF/ERR_REG/STATUS duplicates the three addresses it
 * needs here rather than growing the driver's public surface for one bench
 * app. Addresses + reset values verified against BST-BMI323-DS000-13
 * Rev 1.7, Table 36 (Register Map Overview), p.61.
 */
#define BMI323_REG_ERR_REG  0x01u
#define BMI323_REG_STATUS   0x02u
#define BMI323_REG_ACC_CONF 0x20u

/** ERR_REG bit0 (Rev 1.7 p.61 Table 36): a fatal, unrecoverable error. */
#define BMI323_ERR_FATAL_ERR 0x0001u
/** ERR_REG bit5 (Rev 1.7 p.61 Table 36): the ACC_CONF write was rejected
 *  (a reserved/invalid field encoding) -- the single most direct way this
 *  part can tell us "the configuration you sent was not accepted". */
#define BMI323_ERR_ACC_CONF_ERR 0x0020u

/** STATUS bit0 (Rev 1.7 p.66): '1' after power-up or soft-reset; clear-on-read. */
#define BMI323_STATUS_POR_DETECTED 0x0001u
/** STATUS bit7 (Rev 1.7 p.66): accelerometer data ready; clear-on-read, and
 *  ALSO cleared by reading ACC_DATA_X..Z (Rev 1.7 p.23) -- see the file
 *  header for why this drives the mandatory read order. */
#define BMI323_STATUS_DRDY_ACC 0x0080u

/**
 * One raw 16-bit register read, talking to the part directly (the driver's
 * equivalent, reg_read16(), is `static`). Per BST-BMI323-DS000-13 Rev 1.7
 * Table 53 p.204, a single-word I2C read is: write the register address,
 * repeated START, then the slave clocks out 2 DUMMY bytes followed by the
 * 2 data bytes (LSB first). We request 4 bytes total and keep only the
 * last 2 -- discarding the first 2 is mandatory; see the file header for
 * the false-0x0000 CHIP_ID reading that not doing this caused earlier in
 * this investigation.
 */
static alp_status_t read_reg16(alp_i2c_t *bus, uint8_t reg, uint16_t *out)
{
	uint8_t      buf[4] = { 0 }; /* [0..1] = dummy prefix (discarded), [2..3] = data LSB,MSB */
	alp_status_t s      = alp_i2c_write_read(bus, EVK_I2C_ADDR_BMI323, &reg, 1, buf, sizeof buf);
	if (s != ALP_OK) return s;
	*out = (uint16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
	return ALP_OK;
}

/** Print one ACC_CONF sample: raw hex + the three decoded fields (Rev 1.7
 *  p.88: acc_mode[14:12], acc_range/FS[6:4], acc_odr/ODR[3:0]). */
static void print_acc_conf(const char *label, uint16_t v)
{
	printk("ACC_CONF (0x20) %-6s: 0x%04x (acc_mode=0b%u%u%u fs=0x%x odr=0x%x)\n",
	       label,
	       v,
	       (v >> 14) & 0x1u,
	       (v >> 13) & 0x1u,
	       (v >> 12) & 0x1u,
	       (v >> 4) & 0x7u,
	       v & 0xFu);
}

int main(void)
{
	printk("\n=== AEN801/AEN803 BMI323 register read-back settlement "
	       "(carrier bus @0x%02x) ===\n",
	       EVK_I2C_ADDR_BMI323);

	/* Bring up the SDK runtime before anything else -- thin today, but
	 * future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	/* Carrier bus (ALP_E1M_I2C0 / EVK_I2C_BUS_SENSORS / alp-i2c0). Board-
	 * layer enabled; see the file header + this app's overlay for why no
	 * bus wiring lives here. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	if (bus == NULL) {
		printk("RESULT FAIL: alp_i2c_open(carrier bus) -> NULL, alp_last_error=%d\n",
		       (int)alp_last_error());
		return 0;
	}

	/* --- bmi323_init(): the same soft-reset + CHIP_ID bring-up i2c-device-hub
	 * uses. A correct CHIP_ID here proves the READ path only -- see the file
	 * header for why it says nothing about whether writes land. */
	bmi323_t     bmi;
	alp_status_t init_rc = bmi323_init(&bmi, bus, EVK_I2C_ADDR_BMI323);
	uint8_t      id      = 0;
	bmi323_read_id(&bmi, &id);
	printk("bmi323_init() -> rc=%d  CHIP_ID=0x%02x (reset value is ALSO 0x43 -- see file header)\n",
	       (int)init_rc,
	       id);
	if (init_rc != ALP_OK) {
		printk("RESULT FAIL: bmi323_init rc=%d -- part not answering at 0x%02x\n",
		       (int)init_rc,
		       EVK_I2C_ADDR_BMI323);
		alp_i2c_close(bus);
		return 0;
	}

	/* --- ACC_CONF, BEFORE bmi323_set_accel() runs -- the "going in" snapshot,
	 * so the after-value below is a genuine before/after, not just a single
	 * sample asserted to mean something. */
	uint16_t     acc_conf_before = 0;
	alp_status_t rc_conf_before  = read_reg16(bus, BMI323_REG_ACC_CONF, &acc_conf_before);
	print_acc_conf("before", acc_conf_before);

	/* --- Configure exactly as i2c-device-hub does: same driver call, same
	 * ODR, same full-scale. Going through the real public API (not a
	 * hand-rolled write) means the exact code path under test on silicon
	 * is the one the bug report exercised. */
	alp_status_t cfg_rc = bmi323_set_accel(&bmi, BMI323_ODR_100_HZ, BMI323_ACCEL_FS_2G);
	printk("bmi323_set_accel(ODR_100_HZ, FS_2G) -> rc=%d\n", (int)cfg_rc);

	/* Same wait i2c-device-hub uses after set_accel(): 15 ms here, on top of
	 * bmi323_set_accel()'s own internal 2 ms tA,SU wait -- so the read-back
	 * below happens ~17 ms after the ACC_CONF write, matching the hub's
	 * timing exactly. */
	k_msleep(15);

	/* --- Read-back #1 (~17 ms after the ACC_CONF write), in the MANDATORY
	 * order documented in the file header: ACC_CONF, then ERR_REG, then
	 * STATUS. Neither ACC_CONF nor ERR_REG has a read side effect; STATUS
	 * does (clear-on-read), so it must come after them and before any data
	 * register read. */
	uint16_t     acc_conf_after = 0;
	alp_status_t rc_conf_after  = read_reg16(bus, BMI323_REG_ACC_CONF, &acc_conf_after);
	print_acc_conf("after", acc_conf_after);

	/*
	 * The whole point of this app: decide, from the raw bits, which of the
	 * four documented outcomes this silicon shows.
	 */
	if (acc_conf_after == 0x4008u) {
		printk("ACC_CONF interpretation: 0x4008 -- the write LANDED (acc_mode=0b100 "
		       "normal, ODR=100Hz/0x8, FS=2G/0x0, per BST-BMI323-DS000-13 Rev 1.7 "
		       "pp.21-22,61,88-91). The driver is EXONERATED; the fault is elsewhere.\n");
	} else if (acc_conf_after == 0x0028u) {
		printk("ACC_CONF interpretation: 0x0028 -- this is the RESET value (Rev 1.7 "
		       "p.88, Table 36 p.61). No write landed, or something reset the part "
		       "after bmi323_set_accel() returned.\n");
	} else if (acc_conf_after == 0x1008u) {
		printk("ACC_CONF interpretation: 0x1008 -- the PRE-FIX encoding "
		       "(acc_mode=0b001<<12, an encoding not in Rev 1.7's mode table), the bug "
		       "this session's chips/bmi323/bmi323.c fix replaced. Would mean a stale "
		       "binary -- already ruled out for this build (see the object dump cited "
		       "in the task that spawned this app).\n");
	} else if ((uint16_t)((acc_conf_after >> 8) | (acc_conf_after << 8)) == 0x4008u ||
	           (uint16_t)((acc_conf_after >> 8) | (acc_conf_after << 8)) == 0x0028u) {
		printk("ACC_CONF interpretation: 0x%04x is the BYTE-SWAP of a known-good value "
		       "-- a write-path (or this read-back's own) endianness bug.\n",
		       acc_conf_after);
	} else {
		printk("ACC_CONF interpretation: 0x%04x matches none of the four documented "
		       "signatures -- decode acc_mode/fs/odr above by hand against "
		       "BST-BMI323-DS000-13 Rev 1.7 pp.21-22,61,88-91.\n",
		       acc_conf_after);
	}

	uint16_t     err_reg = 0;
	alp_status_t rc_err  = read_reg16(bus, BMI323_REG_ERR_REG, &err_reg);
	printk("ERR_REG   (0x01) : 0x%04x (fatal_err=%u acc_conf_err=%u)\n",
	       err_reg,
	       (err_reg & BMI323_ERR_FATAL_ERR) ? 1 : 0,
	       (err_reg & BMI323_ERR_ACC_CONF_ERR) ? 1 : 0);

	uint16_t     status_1   = 0;
	alp_status_t rc_status1 = read_reg16(bus, BMI323_REG_STATUS, &status_1);
	printk("STATUS    (0x02) @~17ms : 0x%04x (drdy_acc=%u por_detected=%u)\n",
	       status_1,
	       (status_1 & BMI323_STATUS_DRDY_ACC) ? 1 : 0,
	       (status_1 & BMI323_STATUS_POR_DETECTED) ? 1 : 0);

	/* --- STATUS again ~100 ms after set_accel() (83 ms more, on top of the
	 * ~17 ms already elapsed) -- free evidence for "is 15 ms just too short
	 * a wait" vs "the write never took effect at all". This must happen
	 * BEFORE the accel data read below: reading STATUS is clear-on-read, so
	 * this is the LAST chance to observe drdy_acc's natural state before an
	 * ACC_DATA_X..Z read clears it too (Rev 1.7 p.23). */
	k_msleep(83);
	uint16_t     status_2   = 0;
	alp_status_t rc_status2 = read_reg16(bus, BMI323_REG_STATUS, &status_2);
	printk("STATUS    (0x02) @~100ms: 0x%04x (drdy_acc=%u por_detected=%u)\n",
	       status_2,
	       (status_2 & BMI323_STATUS_DRDY_ACC) ? 1 : 0,
	       (status_2 & BMI323_STATUS_POR_DETECTED) ? 1 : 0);
	if (((status_1 & BMI323_STATUS_DRDY_ACC) == 0) && (status_2 & BMI323_STATUS_DRDY_ACC)) {
		printk("STATUS timing note: drdy_acc was 0 at ~17ms but 1 at ~100ms -- the fix "
		       "would be a LONGER post-config wait, not a register-write bug.\n");
	}

	/* --- 4. Only THEN the accelerometer data itself, through the same public
	 * driver call i2c-device-hub uses. This is last on purpose: it clears
	 * drdy_acc, so every STATUS observation above had to happen first. */
	bmi323_axes_t axes    = { 0 };
	alp_status_t  rc_axes = bmi323_read_accel(&bmi, &axes);
	printk("bmi323_read_accel() -> rc=%d accel{%d,%d,%d}\n", (int)rc_axes, axes.x, axes.y, axes.z);

	alp_i2c_close(bus);

	/*
	 * Verdict. This app's job is to MEASURE, not to vindicate the sensor:
	 * PASS requires every read-back TRANSACTION to have completed (rc ==
	 * ALP_OK), regardless of what value came back -- a run that proves the
	 * accelerometer is broken is still a successful measurement. FAIL means
	 * a read-back itself failed at the bus level (NACK/timeout), which is
	 * the one outcome that means this app could not gather its evidence.
	 */
	bool all_reads_ok = (rc_conf_before == ALP_OK) && (cfg_rc == ALP_OK) &&
	                    (rc_conf_after == ALP_OK) && (rc_err == ALP_OK) && (rc_status1 == ALP_OK) &&
	                    (rc_status2 == ALP_OK) && (rc_axes == ALP_OK);

	if (all_reads_ok) {
		printk("RESULT PASS: all read-backs obtained -- ACC_CONF before=0x%04x after=0x%04x, "
		       "ERR_REG=0x%04x, STATUS@17ms=0x%04x STATUS@100ms=0x%04x, "
		       "accel{%d,%d,%d}\n",
		       acc_conf_before,
		       acc_conf_after,
		       err_reg,
		       status_1,
		       status_2,
		       axes.x,
		       axes.y,
		       axes.z);
	} else {
		printk("RESULT FAIL: a read-back transaction itself failed "
		       "(conf_before rc=%d cfg rc=%d conf_after rc=%d err rc=%d "
		       "status1 rc=%d status2 rc=%d axes rc=%d)\n",
		       (int)rc_conf_before,
		       (int)cfg_rc,
		       (int)rc_conf_after,
		       (int)rc_err,
		       (int)rc_status1,
		       (int)rc_status2,
		       (int)rc_axes);
	}

	return 0;
}
