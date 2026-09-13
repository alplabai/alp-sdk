/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-rtc-control2-probe -- READ-ONLY on-silicon settlement of the
 * RV-3028-C7 CONTROL_2 12_24 fix, for the E1M-AEN801 (Alif Ensemble E8),
 * bench RAM-run via J-Link.
 *
 * What it settles
 * ----------------
 * Three RV-3028-C7 driver changes landed this session (chips/rv3028c7/
 * rv3028c7.c); none has run on real silicon. This app settles the FIRST one
 * and, by hard constraint below, must never touch the third:
 *
 *   1. rv3028c7_init() now CLEARS CONTROL_2 (0x10) bit 0x02 -- the real
 *      12_24 field, where 0 = 24-hour (RV-3028-C7 Application Manual
 *      Rev. 1.4, register table p.24) -- instead of setting bit 0x40, which
 *      is CLKIE (clock-out sync interrupt enable, same p.24 table), a bit a
 *      previous driver revision mislabelled "24H" and cleared for the wrong
 *      reason entirely.
 *   2. rv3028c7_set_int_enable(SRC_CLKF, ...) now targets 0x40 (real CLKIE)
 *      instead of 0x80 (TSE, Time Stamp Enable) -- NOT exercised here; this
 *      app never calls rv3028c7_set_int_enable() at all (see the hard
 *      constraint below).
 *   3. EEPROM-backed writes (BSIE / CLKOUT routing) now use a conditional
 *      single-byte commit -- NOT exercised here, and must not be: see the
 *      hard constraint below.
 *
 * The app reads CONTROL_2 raw, BEFORE and AFTER rv3028c7_init() runs, and
 * decodes bit 0x02 both times: PASS requires it to read 0 (24-hour mode)
 * afterwards. It also reads STATUS (0x0E) and reports PORF via
 * rv3028c7_was_cold_start(), and the ID register (0x28), printing the HID
 * nibble (id & 0xF0, the hardware-identity claim) and the VID nibble
 * (id & 0x0F, a production-line code, not identity) separately -- per the
 * RV-3028-C7 Application Manual Rev. 1.4 section 3.14, cited already for
 * this exact part + address in docs/bring-up-aen.md's BRD_I2C bench table
 * (ID 0x28 = 0x44 measured 2026-09-05, HID=0x4 matched, VID=0x4 is not an
 * identity claim).
 *
 * HARD CONSTRAINT -- this app must NEVER trigger an EEPROM commit. It does
 * not call rv3028c7_set_int_enable() (not even on RV3028C7_SRC_CLKF, whose
 * enable bit lives in CONTROL_2 and would NOT itself commit EEPROM -- only
 * the RV3028C7_SRC_BSF path does, because BSIE lives in the non-volatile
 * EEPROM_BACKUP register) and it does not call rv3028c7_route_clkout()
 * (CLKOUT routing IS EEPROM-backed unconditionally). RV-3028-C7 Application
 * Manual Rev. 1.4 p.98 (nCYCLE) specs only 100 write cycles minimum at the
 * VDD 5.5 V / 85 C hot corner -- burning cycles to satisfy curiosity in a
 * bench probe is not acceptable. Every register access this app makes is a
 * READ, except the two writes rv3028c7_init() itself performs internally
 * (a STATUS PORF-clear and a CONTROL_2 12_24-clear) -- both plain,
 * non-EEPROM registers (see rv3028c7_init() in chips/rv3028c7/rv3028c7.c).
 * Reads are free; commits are not.
 *
 * Bus
 * ---
 * The RTC sits on the ON-MODULE BRD bus -- SoC I2C0, portable alias
 * alp-i2c2 (portable bus index 2; 0 and 1 are the E1M EDGE I2C buses,
 * ALP_E1M_I2C0/ALP_E1M_I2C1) -- at 7-bit address 0x52
 * (RV3028C7_I2C_ADDR). NOT the carrier bus (ALP_E1M_I2C0/alp-i2c0): BRD_I2C
 * has no E1M edge route of its own (#1848), so opening the wrong bus index
 * here finds nothing rather than erroring loudly. The board layer already
 * enables this bus (metadata/e1m_modules/aen/on-module-links.yaml `brd_i2c`
 * entry) -- this app's own overlay carries only the bench ITCM retarget.
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
#include "alp/chips/rv3028c7.h"

/*
 * Raw register facts this app reads OUTSIDE the rv3028c7 driver's public
 * API -- the driver deliberately exposes no raw-register accessor (its
 * whole job is to hide the register map), so a read-only bench probe that
 * needs CONTROL_2/STATUS/ID duplicates the three addresses it needs here
 * rather than growing the driver's public surface for one bench app.
 * Addresses per the RV-3028-C7 Application Manual Rev. 1.4 register
 * overview, section 3.2, p.12 -- the same table chips/rv3028c7/rv3028c7.c
 * cites for its own private RV3028_REG_* constants; keep these two tables
 * in step if the register map here ever needs to grow.
 */
#define RV3028_REG_STATUS    0x0Eu
#define RV3028_REG_CONTROL_2 0x10u
#define RV3028_REG_ID        0x28u

/** Power-on reset flag (RV-3028-C7 Application Manual Rev. 1.4 p.22) --
 *  the part's only "is the stored time trustworthy" indicator. */
#define RV3028_STATUS_PORF 0x01u

/** Hour format: 0 = 24h, 1 = 12h -- the REAL 12_24 field (Application
 *  Manual Rev. 1.4, register table p.24). This is the bit under test. */
#define RV3028_CTRL2_12_24 0x02u

/** Clock-out sync interrupt enable (same p.24 table) -- the bit a previous
 *  driver revision mislabelled "24H" and cleared instead of 0x02 above.
 *  rv3028c7_init() never touches this bit; reported here purely so a
 *  silicon run shows it stayed at whatever it powered on with, which is
 *  the negative-control half of settling which bit the fix actually
 *  clears. */
#define RV3028_CTRL2_CLKIE 0x40u

/** One raw register read: write the register address, repeated-START read
 *  one byte back. None of CONTROL_2/STATUS/ID have read-side-effects on
 *  this part -- unlike RV3028_REG_SECONDS.. under a stalled-bus condition
 *  (rv3028c7_get_time()'s p.53 note), a plain register read here is always
 *  non-mutating. */
static alp_status_t read_reg(alp_i2c_t *bus, uint8_t reg, uint8_t *out)
{
	return alp_i2c_write_read(bus, RV3028C7_I2C_ADDR, &reg, 1, out, 1);
}

int main(void)
{
	printk("\n=== AEN801 RV-3028-C7 CONTROL_2 12_24 settlement "
	       "(read-only, BRD_I2C @0x%02x) ===\n",
	       RV3028C7_I2C_ADDR);

	/* Bring up the SDK runtime before anything else -- thin today, but
	 * future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	/* BRD_I2C -- portable bus 2 (alp-i2c2), board-layer enabled; see the
	 * file header + this app's overlay for why no bus wiring lives here. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 2u,
	    .bitrate_hz = 100000u,
	});
	if (bus == NULL) {
		printk("RESULT FAIL: alp_i2c_open(BRD_I2C) -> NULL, alp_last_error=%d\n",
		       (int)alp_last_error());
		return 0;
	}

	/* 1. CONTROL_2 + STATUS + ID, raw, BEFORE rv3028c7_init() runs -- the
	 *    "going in" snapshot. */
	uint8_t      ctrl2_before = 0, status_before = 0, id = 0;
	alp_status_t rc_c2b = read_reg(bus, RV3028_REG_CONTROL_2, &ctrl2_before);
	alp_status_t rc_stb = read_reg(bus, RV3028_REG_STATUS, &status_before);
	alp_status_t rc_id  = read_reg(bus, RV3028_REG_ID, &id);
	printk("raw CONTROL_2 (0x10) before init: rc=%d val=0x%02x (12_24=%u CLKIE=%u)\n",
	       (int)rc_c2b,
	       ctrl2_before,
	       (ctrl2_before & RV3028_CTRL2_12_24) ? 1 : 0,
	       (ctrl2_before & RV3028_CTRL2_CLKIE) ? 1 : 0);
	printk("raw STATUS (0x0E) before init:    rc=%d val=0x%02x (PORF=%u)\n",
	       (int)rc_stb,
	       status_before,
	       (status_before & RV3028_STATUS_PORF) ? 1 : 0);
	printk("raw ID (0x28):                    rc=%d val=0x%02x "
	       "(HID=0x%02x VID=0x%02x -- Application Manual Rev. 1.4 sec3.14: "
	       "HID is the hardware-identity field, VID is a production-line "
	       "code, not identity)\n",
	       (int)rc_id,
	       id,
	       id & 0xF0u,
	       id & 0x0Fu);
	if (rc_c2b != ALP_OK || rc_stb != ALP_OK || rc_id != ALP_OK) {
		printk("RESULT FAIL: a pre-init raw register read failed "
		       "(ctrl2 rc=%d status rc=%d id rc=%d) -- RTC not answering at 0x%02x\n",
		       (int)rc_c2b,
		       (int)rc_stb,
		       (int)rc_id,
		       RV3028C7_I2C_ADDR);
		alp_i2c_close(bus);
		return 0;
	}

	/* 2. rv3028c7_init() -- the ONLY write path this app exercises. It
	 *    clears PORF if set and (per this session's fix) clears CONTROL_2
	 *    bit 0x02 to force 24-hour mode. Going through the driver's public
	 *    API rather than a hand-rolled write means the exact code path
	 *    under test on silicon is the real init() sequence. */
	rv3028c7_t   ctx;
	alp_status_t rc_init = rv3028c7_init(&ctx, bus);
	printk("rv3028c7_init() -> rc=%d\n", (int)rc_init);
	if (rc_init != ALP_OK) {
		printk("RESULT FAIL: rv3028c7_init rc=%d\n", (int)rc_init);
		alp_i2c_close(bus);
		return 0;
	}

	bool         cold_start = false;
	alp_status_t rc_cs      = rv3028c7_was_cold_start(&ctx, &cold_start);
	printk("rv3028c7_was_cold_start() -> rc=%d cold_start=%s (PORF was %s going in)\n",
	       (int)rc_cs,
	       cold_start ? "true" : "false",
	       cold_start ? "SET" : "clear");

	/* 3. CONTROL_2 + STATUS again, AFTER init -- the settlement read. */
	uint8_t      ctrl2_after = 0, status_after = 0;
	alp_status_t rc_c2a = read_reg(bus, RV3028_REG_CONTROL_2, &ctrl2_after);
	alp_status_t rc_sta = read_reg(bus, RV3028_REG_STATUS, &status_after);
	printk("raw CONTROL_2 (0x10) after init:  rc=%d val=0x%02x (12_24=%u CLKIE=%u)\n",
	       (int)rc_c2a,
	       ctrl2_after,
	       (ctrl2_after & RV3028_CTRL2_12_24) ? 1 : 0,
	       (ctrl2_after & RV3028_CTRL2_CLKIE) ? 1 : 0);
	printk("raw STATUS (0x0E) after init:     rc=%d val=0x%02x (PORF=%u)\n",
	       (int)rc_sta,
	       status_after,
	       (status_after & RV3028_STATUS_PORF) ? 1 : 0);

	alp_i2c_close(bus);

	/*
	 * Verdict. The fix under test is "rv3028c7_init() clears the REAL
	 * 12_24 field (0x02), not the mislabelled 0x40 CLKIE bit". PASS
	 * requires every read above to have succeeded, CONTROL_2 bit 0x02 to
	 * read 0 after init (24-hour mode), and PORF to read 0 after init
	 * (was_cold_start's clear side-effect landed on silicon, not just in
	 * ctx). CLKIE (0x40) is reported above for visibility only -- it is
	 * NOT part of the verdict: rv3028c7_init() never touches it, so
	 * whatever it powered on with is expected to survive untouched
	 * either way, and that is itself the negative control for "the fix
	 * targets the right bit".
	 */
	bool all_reads_ok = (rc_c2a == ALP_OK) && (rc_sta == ALP_OK);
	bool is_24h       = (ctrl2_after & RV3028_CTRL2_12_24) == 0;
	bool porf_cleared = (status_after & RV3028_STATUS_PORF) == 0;

	if (all_reads_ok && is_24h && porf_cleared) {
		printk("RESULT PASS: rv3028c7_init() clears CONTROL_2 bit 0x02 (12_24) on real "
		       "silicon -- RTC in 24-hour mode (before=0x%02x after=0x%02x), PORF cleared "
		       "(before=0x%02x after=0x%02x), ID=0x%02x (HID=0x%02x VID=0x%02x)\n",
		       ctrl2_before,
		       ctrl2_after,
		       status_before,
		       status_after,
		       id,
		       id & 0xF0u,
		       id & 0x0Fu);
	} else {
		printk("RESULT FAIL: CONTROL_2/STATUS did not settle as expected after "
		       "rv3028c7_init() (all_reads_ok=%d is_24h=%d porf_cleared=%d; "
		       "ctrl2 before=0x%02x after=0x%02x; status before=0x%02x after=0x%02x)\n",
		       all_reads_ok,
		       is_24h,
		       porf_cleared,
		       ctrl2_before,
		       ctrl2_after,
		       status_before,
		       status_after);
	}

	return 0;
}
