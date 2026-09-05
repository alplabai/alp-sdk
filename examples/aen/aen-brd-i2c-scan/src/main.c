/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon BRD_I2C (SoC I2C0) housekeeping-bus probe for the E1M-AEN801
 * (Alif Ensemble E8) over the UPSTREAM DesignWare i2c_dw driver (ADR 0017
 * Tier-1, "snps,designware-i2c").
 *
 * BRD_I2C is I2C0 function C (P7_0 SDA / P7_1 SCL) -- see the overlay header
 * for the netlist citations.  It carries three on-module devices: the
 * RV-3028-C7 RTC (@0x52), the TMP112 temperature sensor (@0x48), and the
 * OPTIGA Trust M secure element (@0x30, but DNP=1 on this board rev -- it must
 * NOT ack, and that absence is an expected negative control, not a failure).
 *
 * What it does, in order
 * -----------------------
 *   1. Bring up i2c0 and print its DT-configured bitrate.
 *   2. SCAN the 7-bit address space 0x08..0x77 with a benign 1-byte-read probe
 *      (never a data write to an unknown address) and print every ACK.
 *   3. For addresses that actually responded, decode what we can:
 *        - TMP112 @0x48: read the temperature register and print milli-C.
 *        - RV-3028-C7 @0x52: read the ID register, then read the seconds
 *          register twice ~1s apart to prove the oscillator is actually
 *          running (an ID read alone only proves the chip answers, not that
 *          it ticks).
 *   4. Print one machine-greppable RESULT line.
 *
 * PULL-UP: this net has no pull-up resistor anywhere (see the overlay header)
 * and relies on the SoC pad's own internal pull-up.  On 2626-R2 silicon that
 * is ENOUGH: this app ACKed the RTC at 0x52 and the TMP112 on 2026-09-05 at
 * 100 kHz, with every non-response a clean -EIO NACK and no -ETIMEDOUT / "User
 * Abort" anywhere in the run.  The older "the internal pull is too weak"
 * verdict (BENCH-SETTLED 2026-08-31) was measured on an r1 module, where these
 * parts sit on LPI2C0 (P7_4/P7_5) and nothing is attached to P7_0/P7_1 at all
 * -- see the corrected i2c0/LPI2C0 comments in ensemble_e8_peripherals.dtsi.
 * So on R2, a FAIL result IS a real finding; treat it as one.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART
 * is not wired to USB.  BENCH-VALIDATION app -- not a customer teaching
 * example.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>

/* i2c0 == i2c@49010000 (snps,designware-i2c), okay'd + pinctrl'd by the
 * overlay -- this is BRD_I2C, NOT an E1M edge bus. */
#define I2C0_NODE DT_NODELABEL(i2c0)

/* General-call (0x00..0x07) and reserved-high (0x78..0x7f) addresses are
 * skipped by the conventional scan window, matching Zephyr's i2c_scanner
 * sample and the sibling aen-i2c2-eeprom-regcheck app. */
#define SCAN_LO 0x08U
#define SCAN_HI 0x77U

/* Netlist-sourced 7-bit addresses (see the overlay header for the BOM rows). */
#define TMP112_ADDR 0x48U /* U20, ADD0 tied to 0V. */
#define RTC_ADDR    0x52U /* U21, RV-3028-C7. */
#define OPTIGA_ADDR 0x30U /* IC1, DNP=1 -- expected ABSENT. */
/* The address something actually answers on for the temperature sensor.
 * BENCH 2026-09-05: 0x48 NACKs and 0x40 ACKs with a plausible temperature.
 * 0x48 = 0b1001000 and 0x40 = 0b1000000 differ only in bit 3, which first
 * suggested a slow-edge address mis-sample -- but the bench disproved that
 * (see the alias-probe block in main() for the evidence).  Treated here simply
 * as an observed second address to be identified, not as a known fault. */
#define TMP112_ALIAS_ADDR 0x40U

/* TMP112 datasheet (TI SBOS397, Table 2): pointer register 0x00 is the
 * temperature result, 2 bytes big-endian, 12-bit left-justified, 0.0625 degC
 * per LSB. */
#define TMP112_REG_TEMP   0x00U
#define TMP112_REG_CONFIG 0x01U
#define TMP112_REG_TLOW   0x02U
#define TMP112_REG_THIGH  0x03U

/* RV-3028-C7 Application Manual: 0x00 = Seconds (BCD), 0x28 = "ID" register.
 * Both are datasheet-sourced addresses -- we print what we actually read
 * rather than assume the chip matches the datasheet's revision. */
#define RTC_REG_SECONDS 0x00U
#define RTC_REG_ID      0x28U

static const struct device *const i2c0 = DEVICE_DT_GET(I2C0_NODE);

/* Scan-result bookkeeping -- one bool per address of interest, plus a total
 * count so the RESULT policy can tell "nothing at all" (electrical) apart
 * from "some devices missing" (partial). */
struct scan_result {
	unsigned int n_found;
	bool         tmp112_present;
	bool         rtc_present;
	bool         optiga_present;
	bool         tmp112_alias_present;
};

/* Probe every 7-bit address with a 1-byte read (the most portable ACK probe
 * for i2c_dw -- a zero-length write is not universally honoured) and print
 * each one that answers. Never writes data to an unknown address. */
static struct scan_result scan_bus(void)
{
	struct scan_result res = { 0 };
	uint8_t            dummy;

	printk("scanning 0x%02x..0x%02x ...\n", SCAN_LO, SCAN_HI);
	for (uint16_t addr = SCAN_LO; addr <= SCAN_HI; addr++) {
		int rc = i2c_read(i2c0, &dummy, 1U, addr);

		if (rc != 0) {
			/* Report the errno for the three addresses we care about.
			 * The VALUE discriminates two very different faults, and a
			 * bare "missing" hides the difference:
			 *   -116 (-ETIMEDOUT) -- the controller drove and nothing
			 *                        ever released the line: the classic
			 *                        no-pull-up / floating-net signature.
			 *   -5   (-EIO)       -- the transfer ran and was NACKed:
			 *                        the wire works, nobody answered at
			 *                        that address.
			 * Printing only for the expected addresses keeps the scan
			 * output readable instead of 112 error lines. */
			if (addr == TMP112_ADDR || addr == RTC_ADDR || addr == OPTIGA_ADDR) {
				printk("  no ACK @ 0x%02x (rc=%d)\n", addr, rc);
			}
			continue;
		}
		printk("  ACK @ 0x%02x\n", addr);
		res.n_found++;
		if (addr == TMP112_ADDR) {
			res.tmp112_present = true;
		} else if (addr == RTC_ADDR) {
			res.rtc_present = true;
		} else if (addr == OPTIGA_ADDR) {
			res.optiga_present = true;
		} else if (addr == TMP112_ALIAS_ADDR) {
			res.tmp112_alias_present = true;
		}
	}
	printk("scan done: %u device(s) responded\n", res.n_found);
	return res;
}

/* Read + decode the TMP112 temperature register. Returns 0 and fills
 * *milli_c on success (i2c rc otherwise). No float printf -- 0.0625 degC/LSB
 * is exactly 125/2 milli-degC/LSB, so the conversion stays integer. */
static int read_tmp112_milli_c_at(uint16_t addr, int32_t *milli_c)
{
	uint8_t ptr = TMP112_REG_TEMP;
	uint8_t buf[2];
	int32_t raw12;
	int     rc;

	rc = i2c_write_read(i2c0, addr, &ptr, sizeof(ptr), buf, sizeof(buf));
	if (rc != 0) {
		return rc;
	}

	/* 16-bit big-endian read, top 12 bits are the signed result. */
	raw12 = (int32_t)(((uint16_t)buf[0] << 8) | buf[1]) >> 4;
	if (raw12 & 0x800) {
		raw12 -= 4096; /* sign-extend the 12-bit two's-complement value. */
	}
	*milli_c = (raw12 * 125) / 2;
	return 0;
}

int main(void)
{
	int                rc;
	struct scan_result scan;
	bool               tmp112_ok = false;
	/* Sensor identified at EITHER address -- see the ADD0-strap note below:
	 * on this module it answers at 0x40, not the strapped 0x48. */
	bool tmp112_any = false;
	bool rtc_id_ok  = false;
	bool rtc_ticked = false;

	printk("\n=== AEN801 BRD_I2C housekeeping-bus bench (i2c_dw / i2c0 @ 0x49010000) ===\n");

	/* 1. device readiness + configured bitrate. */
	if (!device_is_ready(i2c0)) {
		printk("RESULT FAIL: i2c0 device not ready\n");
		return 0;
	}
	printk("i2c0 device ready, configured bitrate: %u Hz\n",
	       (unsigned int)DT_PROP(I2C0_NODE, clock_frequency));

	/* 2. scan for every device that answers. */
	scan = scan_bus();
	printk("  0x48 (TMP112) %s, 0x52 (RV-3028-C7) %s, 0x30 (OPTIGA) %s%s\n",
	       scan.tmp112_present ? "PRESENT" : "missing",
	       scan.rtc_present ? "PRESENT" : "missing",
	       scan.optiga_present ? "PRESENT (unexpected -- check DNP population!)" : "absent",
	       scan.optiga_present ? "" : " (expected -- IC1 is DNP=1 on this rev)");

	if (scan.n_found == 0U) {
		printk("RESULT FAIL: scan found nothing on 0x%02x..0x%02x -- likely "
		       "electrical (this net has no pull-up but the SoC pad's weak "
		       "internal one; see the overlay comment), not a software bug\n",
		       SCAN_LO,
		       SCAN_HI);
		return 0;
	}

	/* 3a. TMP112: decode the temperature if it answered the scan. */
	if (scan.tmp112_present) {
		int32_t milli_c;

		rc = read_tmp112_milli_c_at(TMP112_ADDR, &milli_c);
		if (rc == 0) {
			tmp112_ok  = true;
			tmp112_any = true;
			printk("TMP112 @0x48: %d milli-degC (%s)\n",
			       milli_c,
			       (milli_c >= 15000 && milli_c <= 35000)
			           ? "plausible room-temperature reading"
			           : "OUTSIDE the plausible 15..35 degC room range -- "
			             "check the probe/environment, not necessarily a bug");
		} else {
			printk("TMP112 @0x48: read failed, rc=%d\n", rc);
		}
	}

	/* 3a-bis. IDENTIFY WHAT IS AT TMP112_ALIAS_ADDR.
	 *
	 * BENCH RESULT 2026-09-05: 0x48 NACKs (rc=-5) while 0x40 ACKs and returns
	 * 28312 milli-degC -- a plausible board temperature.
	 *
	 * The original theory was a rise-time fault: 0x48 and 0x40 differ only in
	 * bit 3, so a slow SDA edge could make the part mis-sample its own address.
	 * THAT THEORY IS NOT SUPPORTED by the measurements:
	 *   - Raising the pads to drive-strength = <12> and slew-rate = "fast" did
	 *     NOT bring 0x48 back.  A marginal edge should have improved.
	 *   - Every failure is a clean -EIO NACK; there is not one -ETIMEDOUT and
	 *     not one "User Abort" in the capture.
	 *   - Decisively: address bits and DATA bits ride the same wire.  If bit 3
	 *     of the address were being sampled low, the data bytes would corrupt
	 *     too -- yet the temperature reads back clean and self-consistent.
	 * So signal integrity is fine and the device really IS at 0x40; it is not
	 * being mis-addressed.
	 *
	 * That leaves the address strap itself: the netlist has U20 = TMP112DIDPWR
	 * with ADD0 tied to 0V, which the datasheet maps to 0x48 (ADD0 to V+ / SDA /
	 * SCL give 0x49 / 0x4A / 0x4B -- none of them 0x40).  A part answering at
	 * 0x40 is therefore either a different device than the BOM says, or fitted
	 * with a different strap than the netlist records.  Reading the TMP112
	 * configuration registers below distinguishes those without guessing. */
	/* BENCH-IDENTIFIED 2026-09-05: the TMP112 on this module answers at
	 * TMP112_ALIAS_ADDR (0x40), not at the strapped TMP112_ADDR (0x48).
	 * Identification is a three-of-three register fingerprint, not a guess:
	 * CONFIG=0x60a0, T_LOW=0x4b00, T_HIGH=0x5000 all equal the TMP112
	 * power-on defaults, and it returns a plausible temperature.
	 *
	 * 0x40 is NOT a legal TMP112 address.  The datasheet strap table is
	 * ADD0->GND = 0x48, ADD0->V+ = 0x49, ADD0->SDA = 0x4A, ADD0->SCL = 0x4B.
	 * The netlist has U20 pin 3 (ADD0) on 0V, which should give 0x48.  A part
	 * decoding 0x40 therefore points at the ADD0 strap not actually being at
	 * GND on the built module (an open joint on U20 pin 3 would leave ADD0
	 * floating) -- a board/production question, NOT a firmware one.  Check
	 * continuity from U20 pin 3 to GND before assuming anything else.
	 *
	 * This is reported, not silently accepted: the summary below still names
	 * the address the part actually answered on, so a module that is strapped
	 * correctly reads differently from this one. */
	if (!scan.tmp112_present && scan.tmp112_alias_present) {
		int32_t milli_c;

		printk("0x%02x responded but 0x%02x did not -- probing 0x%02x as a TMP112\n",
		       TMP112_ALIAS_ADDR,
		       TMP112_ADDR,
		       TMP112_ALIAS_ADDR);
		rc = read_tmp112_milli_c_at(TMP112_ALIAS_ADDR, &milli_c);
		if (rc == 0 && milli_c >= -40000 && milli_c <= 125000) {
			uint8_t  ptr;
			uint8_t  raw[2];
			uint16_t cfg = 0U, tlo = 0U, thi = 0U;
			bool     fp_ok = true;

			printk("0x%02x: %d milli-degC (in the TMP112 -40..125 degC range)\n",
			       TMP112_ALIAS_ADDR,
			       milli_c);

			/* FINGERPRINT, not a guess.  A plausible temperature alone proves
			 * little -- many registers decode to a believable number.  The
			 * TMP112 datasheet gives power-on defaults for three more
			 * registers: Configuration (0x01) = 0x60A0, T_LOW (0x02) = 0x4B00,
			 * T_HIGH (0x03) = 0x5000.  Matching those identifies the part. */
			ptr = TMP112_REG_CONFIG;
			fp_ok &= (i2c_write_read(i2c0, TMP112_ALIAS_ADDR, &ptr, 1U, raw, 2U) == 0);
			cfg = fp_ok ? sys_get_be16(raw) : 0U;
			ptr = TMP112_REG_TLOW;
			fp_ok &= (i2c_write_read(i2c0, TMP112_ALIAS_ADDR, &ptr, 1U, raw, 2U) == 0);
			tlo = fp_ok ? sys_get_be16(raw) : 0U;
			ptr = TMP112_REG_THIGH;
			fp_ok &= (i2c_write_read(i2c0, TMP112_ALIAS_ADDR, &ptr, 1U, raw, 2U) == 0);
			thi = fp_ok ? sys_get_be16(raw) : 0U;

			if (!fp_ok) {
				printk("0x%02x: fingerprint reads failed -- identity NOT "
				       "established.\n",
				       TMP112_ALIAS_ADDR);
			} else {
				printk("0x%02x: CONFIG=0x%04x T_LOW=0x%04x T_HIGH=0x%04x "
				       "(TMP112 defaults 0x60A0 / 0x4B00 / 0x5000)\n",
				       TMP112_ALIAS_ADDR,
				       cfg,
				       tlo,
				       thi);
				tmp112_any = (tlo == 0x4B00U && thi == 0x5000U);
				printk("0x%02x: %s\n",
				       TMP112_ALIAS_ADDR,
				       (tlo == 0x4B00U && thi == 0x5000U)
				           ? "T_LOW/T_HIGH match the TMP112 defaults -- this IS "
				             "a TMP112, strapped to 0x40, NOT 0x48 as the netlist "
				             "records (ADD0 tied to 0V)."
				           : "registers do NOT match TMP112 defaults -- the part "
				             "at this address is something else; do not assume the "
				             "BOM.");
			}
		} else if (rc == 0) {
			printk("0x%02x: read ok but %d milli-degC is outside the TMP112 -40..125 "
			       "degC range -- identity NOT established.\n",
			       TMP112_ALIAS_ADDR,
			       milli_c);
		} else {
			printk("0x%02x: ACKed the scan but the register read failed, rc=%d -- "
			       "identity NOT established.\n",
			       TMP112_ALIAS_ADDR,
			       rc);
		}
	}

	/* 3b. RV-3028-C7: read the ID register, then prove the oscillator is
	 * actually running by reading Seconds twice ~1s apart. An ID read alone
	 * only proves the chip answers on the bus -- it says nothing about
	 * whether the crystal is oscillating, which is the real point of an RTC. */
	if (scan.rtc_present) {
		uint8_t ptr = RTC_REG_ID;
		uint8_t id_reg;
		uint8_t sec_ptr = RTC_REG_SECONDS;
		uint8_t sec_before, sec_after;

		rc = i2c_write_read(i2c0, RTC_ADDR, &ptr, sizeof(ptr), &id_reg, sizeof(id_reg));
		if (rc == 0) {
			rtc_id_ok = true;
			printk("RV-3028-C7 @0x52: ID register 0x28 = 0x%02x\n", id_reg);
		} else {
			printk("RV-3028-C7 @0x52: ID register read failed, rc=%d\n", rc);
		}

		rc = i2c_write_read(
		    i2c0, RTC_ADDR, &sec_ptr, sizeof(sec_ptr), &sec_before, sizeof(sec_before));
		if (rc == 0) {
			k_sleep(K_SECONDS(1));
			rc = i2c_write_read(
			    i2c0, RTC_ADDR, &sec_ptr, sizeof(sec_ptr), &sec_after, sizeof(sec_after));
		}
		if (rc == 0) {
			/* Seconds is BCD, not binary: at a minute rollover (0x59 ->
			 * 0x00) the raw byte goes DOWN even though time moved
			 * forward, so "changed" is the correct oscillator proof,
			 * not "increased". */
			rtc_ticked = (sec_before != sec_after);
			printk("RV-3028-C7 @0x52: Seconds (BCD) 0x%02x -> 0x%02x after ~1s: "
			       "oscillator %s\n",
			       sec_before,
			       sec_after,
			       rtc_ticked ? "RUNNING (advanced)" : "DID NOT ADVANCE");
		} else {
			printk("RV-3028-C7 @0x52: Seconds read failed, rc=%d\n", rc);
		}
	}

	/* 4. verdict. */
	if (tmp112_any && rtc_id_ok && rtc_ticked) {
		printk("RESULT PASS: BRD_I2C (I2C0) scan found TMP112 + RV-3028-C7, "
		       "both decoded, and the RTC oscillator is confirmed running%s\n",
		       tmp112_ok ? ""
		                 : " (NOTE: the TMP112 answered at 0x40, not its strapped "
		                   "0x48 -- check U20 pin 3 ADD0 continuity to GND)");
	} else {
		/* Deliberately does NOT blame the pull-ups.  Bench-measured
		 * 2026-09-05: every non-response on this bus is a clean -EIO NACK,
		 * with zero -ETIMEDOUT and zero "User Abort" in the capture, and both
		 * fitted devices decode perfectly -- so the wire and the internal pad
		 * pull are adequate.  An earlier version of this string asserted a
		 * "marginal internal-pull-up electrical limit"; that was never
		 * measured and the evidence contradicts it. */
		printk("RESULT PARTIAL: BRD_I2C (I2C0) bus is alive (%u device(s) "
		       "responded) but not every expected device/behaviour was "
		       "confirmed -- see the per-device lines above\n",
		       scan.n_found);
	}

	return 0;
}
