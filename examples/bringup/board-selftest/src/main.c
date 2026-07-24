/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * board-selftest -- power-on board bring-up / self-test.
 *
 * The app a customer runs FIRST on a freshly-assembled board,
 * before chasing a peripheral that "doesn't work".  It answers the
 * four questions every bring-up starts with:
 *
 *   1. Is this the SoM I think it is?   (on-module EEPROM identity)
 *   2. Is the silicon alive + answering? (SoC secure-firmware ping)
 *   3. Is the core rail/clock sane?      (RUN operating-point profile)
 *   4. What's actually on the I2C bus?   (7-bit address scan)
 *
 * ...and prints a single pass/fail line so a field tech can read
 * "3 PASS, 1 SKIP, 0 FAIL" off a serial console without a debugger.
 *
 * Portability: this uses ONLY portable <alp/...> APIs -- no chip
 * driver, no vendor header -- so the SAME source builds and runs on
 * every E1M family (Ring 1).  Each check that a given backend can't
 * service returns @ref ALP_ERR_NOSUPPORT, which the self-test
 * reports as SKIP (not FAIL): "this backend has no probe for this",
 * not "the hardware is broken".  That distinction is the whole point
 * of a self-test -- a missing capability and a fault must not look
 * alike.
 *
 * What success looks like (real hardware, E1M-AEN801):
 *
 *   [selftest] === board self-test ===
 *   [selftest] SoM identity: E1M-AEN801 rev r1 sn AEN0000123 -> PASS
 *   [selftest] SoC identity: alif:ensemble:e8 (secure-fw OK) -> PASS
 *   [selftest] power profile: RUN core 800 mV @ 400 MHz -> PASS
 *   [selftest] i2c scan BOARD_I2C_SENSORS: probing 0x08..0x77
 *   [selftest]   found device @ 0x48
 *   [selftest] i2c scan BOARD_I2C_SENSORS: 1 device(s) -> PASS
 *   [selftest] result: 4 PASS, 0 SKIP, 0 FAIL
 *   [selftest] done
 *
 * On native_sim (CI lane): the SoC and power controllers have no
 * backend, so checks 2-3 return ALP_ERR_NOSUPPORT and report SKIP.
 * The emul I2C bus IS up (so check 1 talks to it) but no EEPROM
 * target is registered, so alp_hw_info_read returns
 * ALP_ERR_NOT_READY -- a genuine "identity manifest unreadable"
 * fault, reported as FAIL (the check can't tell "no EEPROM here"
 * from "broken EEPROM"; on real silicon a NAKing manifest IS a
 * fault).  The scan then walks the empty bus and PASSes with 0
 * devices.  The `[selftest] done` marker latches the harness
 * regardless of the verdict.
 */

#include <stdint.h>
#include <stdio.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"
#include "alp/power.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK   -> EVK_I2C_BUS_SENSORS  -> ALP_E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> ALP_E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* 7-bit I2C address scan window.  0x00..0x07 and 0x78..0x7F are
 * reserved by the I2C spec (general-call, 10-bit prefixes, etc.);
 * probing them is meaningless, so a scan walks 0x08..0x77 -- the
 * same window i2cdetect uses by default. */
#define I2C_SCAN_FIRST_ADDR 0x08u
#define I2C_SCAN_LAST_ADDR  0x77u

/* Per-check verdict.  SKIP is deliberately distinct from FAIL: a
 * backend that has no probe for a capability (ALP_ERR_NOSUPPORT)
 * has not FAILED the board -- it simply cannot answer.  A field
 * tech reading the report must not confuse the two. */
typedef enum {
	CHECK_PASS = 0,
	CHECK_SKIP = 1,
	CHECK_FAIL = 2,
} check_result_t;

/* Running tally, printed as the last report line. */
typedef struct {
	unsigned pass;
	unsigned skip;
	unsigned fail;
} tally_t;

/* Fold one check's verdict into the tally. */
static void tally_add(tally_t *t, check_result_t r)
{
	switch (r) {
	case CHECK_PASS:
		t->pass++;
		break;
	case CHECK_SKIP:
		t->skip++;
		break;
	default:
		t->fail++;
		break;
	}
}

/* Map a status the identity/power checks return onto a verdict.
 * NOSUPPORT means "no probe on this backend" -> SKIP; any other
 * non-OK means the probe existed but the read went wrong -> FAIL. */
static check_result_t verdict_from_status(alp_status_t s)
{
	if (s == ALP_OK) {
		return CHECK_PASS;
	}
	if (s == ALP_ERR_NOSUPPORT) {
		return CHECK_SKIP;
	}
	return CHECK_FAIL;
}

/* Check 1 -- SoM identity from the on-module EEPROM manifest.
 * A blank EEPROM (NOT_PROVISIONED), a NAKing/absent one (NOT_READY),
 * or a corrupt one (IO) is a real fault on hardware, reported as
 * FAIL.  Only NOSUPPORT -- no EEPROM bus configured on this build at
 * all -- is a SKIP.  On native_sim the emul bus is up but carries no
 * EEPROM target, so this returns NOT_READY and FAILs; that is
 * honest (native_sim genuinely has no identity manifest), and the
 * done-marker still latches. */
static check_result_t check_som_identity(void)
{
	alp_hw_info_t info;
	alp_status_t  s = alp_hw_info_read(&info);
	if (s == ALP_OK) {
		printf("[selftest] SoM identity: %s rev %s sn %s -> PASS\n",
		       info.som_sku,
		       info.som_hw_rev,
		       info.som_serial);
		return CHECK_PASS;
	}
	/* NOT_PROVISIONED / IO / NOT_READY are genuine board faults;
	 * NOSUPPORT is "no EEPROM bus on this build" (e.g. native_sim). */
	printf("[selftest] SoM identity: unreadable (%s) -> %s\n",
	       alp_status_name(s),
	       s == ALP_ERR_NOSUPPORT ? "SKIP" : "FAIL");
	return verdict_from_status(s);
}

/* Check 2 -- SoC liveness + identity.  The ping proves the secure /
 * system-controller firmware answers before we trust any identity
 * read; soc_ref is always populated (it's a build-time constant), so
 * we print it even when the runtime query can't reach the
 * controller. */
static check_result_t check_soc_identity(void)
{
	alp_status_t ping = alp_soc_secure_fw_ping();

	alp_soc_info_t soc;
	alp_status_t   s = alp_soc_info_read(&soc);
	if (s == ALP_OK) {
		printf("[selftest] SoC identity: %s (secure-fw %s) -> PASS\n",
		       soc.soc_ref,
		       soc.secure_fw_version);
		return CHECK_PASS;
	}
	/* soc_ref is filled even on NOSUPPORT, so name the silicon
	 * regardless -- it's the single most useful line for "did I
	 * flash the firmware for the right chip?". */
	printf("[selftest] SoC identity: %s (ping %s, read %s) -> %s\n",
	       soc.soc_ref,
	       alp_status_name(ping),
	       alp_status_name(s),
	       s == ALP_ERR_NOSUPPORT ? "SKIP" : "FAIL");
	return verdict_from_status(s);
}

/* Check 3 -- read the RUN operating-point profile.  This is a
 * read-only mailbox round-trip on SoCs whose power tree lives behind
 * a controller; it never changes the live operating point (that's
 * alp_power_profile_set, which we deliberately do NOT call in a
 * self-test).  A zero field means "not reported", which is fine. */
static check_result_t check_power_profile(void)
{
	alp_power_profile_t prof;
	alp_status_t        s = alp_power_profile_get(ALP_POWER_PROFILE_RUN, &prof);
	if (s == ALP_OK) {
		/* cpu_clk_hz is Hz; print MHz for readability.  Integer
		 * division is fine -- a self-test wants "roughly 400 MHz",
		 * not sub-Hz precision. */
		printf("[selftest] power profile: RUN core %u mV @ %u MHz -> PASS\n",
		       prof.rail_mv,
		       prof.cpu_clk_hz / 1000000u);
		return CHECK_PASS;
	}
	printf("[selftest] power profile: unavailable (%s) -> %s\n",
	       alp_status_name(s),
	       s == ALP_ERR_NOSUPPORT ? "SKIP" : "FAIL");
	return verdict_from_status(s);
}

/* Check 4 -- scan the management I2C bus for ACKing devices.
 * A 1-byte read is the least-intrusive probe: a present device
 * ACKs its address (ALP_OK), an empty address NACKs (ALP_ERR_IO).
 * We don't interpret the byte -- only the ACK matters for presence.
 *
 * Verdict policy: the SCAN itself passing/failing is about whether
 * the BUS works, not about how many devices answered.  Zero devices
 * on native_sim (no emul targets attached) is still a PASS of the
 * scan machinery; a NULL bus handle (alias unset / no controller)
 * is the SKIP/FAIL case. */
static check_result_t check_i2c_bus(void)
{
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 100000, /* 100 kHz -- the safe baseline for a bus
	                             whose device mix we don't yet know. */
	});
	if (bus == NULL) {
		/* No alp-i2c0 alias / no controller on this build. */
		alp_status_t s = alp_last_error();
		printf("[selftest] i2c scan BOARD_I2C_SENSORS: open failed (%s) -> %s\n",
		       alp_status_name(s),
		       s == ALP_ERR_NOSUPPORT ? "SKIP" : "FAIL");
		return s == ALP_ERR_NOSUPPORT ? CHECK_SKIP : CHECK_FAIL;
	}

	unsigned found = 0;
	printf("[selftest] i2c scan BOARD_I2C_SENSORS: probing 0x%02x..0x%02x\n",
	       I2C_SCAN_FIRST_ADDR,
	       I2C_SCAN_LAST_ADDR);
	for (uint8_t addr = I2C_SCAN_FIRST_ADDR; addr <= I2C_SCAN_LAST_ADDR; addr++) {
		uint8_t      byte = 0;
		alp_status_t s    = alp_i2c_read(bus, addr, &byte, sizeof(byte));
		if (s == ALP_OK) {
			printf("[selftest]   found device @ 0x%02x\n", addr);
			found++;
		}
	}

	alp_i2c_close(bus);
	printf("[selftest] i2c scan BOARD_I2C_SENSORS: %u device(s) -> PASS\n", found);
	return CHECK_PASS;
}

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[selftest] === board self-test ===\n");

	tally_t t = { 0 };
	tally_add(&t, check_som_identity());
	tally_add(&t, check_soc_identity());
	tally_add(&t, check_power_profile());
	tally_add(&t, check_i2c_bus());

	/* One machine-readable summary line.  A test harness or a field
	 * script greps this; a human reads the per-check lines above. */
	printf("[selftest] result: %u PASS, %u SKIP, %u FAIL\n", t.pass, t.skip, t.fail);

	/* The `done` marker latches the twister console harness
	 * regardless of the verdict -- a self-test that FAILS is still a
	 * successful RUN of the self-test.  Real firmware might instead
	 * pull a GPIO or refuse to continue booting on a FAIL. */
	printf("[selftest] done\n");
	return 0;
}
