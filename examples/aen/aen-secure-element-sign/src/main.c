/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-secure-element-sign -- talk to the OPTIGA Trust M on the E1M-AEN
 * (Alif Ensemble) SoM.
 *
 * On the E1M-AEN the Trust M sits on **BRD_I2C** -- the on-module
 * housekeeping bus (SoC I2C0, function C, P7_1 SCL / P7_0 SDA --
 * CORRECTED #1848, earlier revisions of this file believed it was the
 * slave-only Alif LPI2C0) -- at 7-bit address 0x30, alongside the RTC +
 * TMP112.
 * (A different, separate bus carries the EEPROM -- SoC I2C2, see
 * docs/bring-up-aen.md §5.1.)  This example's board target is rtss_he
 * (M55-HE); this example's own overlay wires BRD_I2C (portable bus 2 --
 * 0 and 1 are already the E1M edge I2C buses).  The chip and the sign
 * flow are identical to the V2N variant -- everything goes through the
 * SoM-portable <alp/...> API, so the only AEN-specific facts are the
 * bus + the owning core.
 *
 * The BRD_I2C routing above is R2-sourced (E1M-AEN-2626-R2 netlist +
 * ADTS0013); no R1 netlist is available, and the bench unit on hand is
 * r1, so it still needs an on-unit probe.  It is also open-drain with NO
 * confirmed external pull-up (see the board overlay's pinctrl comment) --
 * do not add bias-pull-down.
 *
 * The driver probes I2C_STATE, then reads the Coprocessor UID through
 * Infineon's host library.  examples/v2n/v2n-secure-element-sign also
 * runs a raw APDU session; it is the bench-verified variant (E1M-V2M103).
 * Nothing here writes to the chip.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/optiga_trust_m.h"

int main(void)
{
	printk("[se] aen-secure-element-sign (probe-only)\n");

	/* BRD_I2C carries the Trust M alongside the RTC + TMP112 (the
     * EEPROM is on a separate bus, SoC I2C2 -- see docs/bring-up-aen.md
     * §5.1).  On the E1M-AEN this is SoC I2C0 (#1848), surfaced as
     * portable bus 2 by this example's own board overlay (0 and 1 are
     * already the E1M edge I2C buses).  100 kHz, matching the board DT:
     * BRD_I2C has no confirmed external pull-up, so there is no fast-mode
     * margin here -- do not raise this past standard-mode. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 2u,
	    .bitrate_hz = 100000u,
	});
	if (bus == NULL) {
		/* NOT_READY vs anything else matters here: NOT_READY means "the BRD_I2C
		 * backend isn't wired up on this build/board" -- an environment gap,
		 * not a driver bug -- so it's a SKIP. Any other code means the open
		 * itself is broken and should fail the run. */
		const alp_status_t last = alp_last_error();
		if (last == ALP_ERR_NOT_READY) {
			printk("[se] RESULT SKIP: alp_i2c_open failed: %d "
			       "(BRD_I2C not ready on this bench)\n",
			       (int)last);
		} else {
			printk("[se] RESULT FAIL: alp_i2c_open failed: %d\n", (int)last);
		}
		printk("[se] done\n");
		return 0;
	}

	/* Init probes I2C_STATE only -- it does not issue OPEN_APPLICATION, so it
	 * never touches the Trust M's application/security state. This makes the
	 * probe safe to run against a part that's already provisioned in the
	 * field: it can only observe that something ACKs at 0x30. */
	optiga_trust_m_t se;
	alp_status_t     s = optiga_trust_m_init(&se, bus, OPTIGA_TRUST_M_I2C_ADDR);
	if (s != ALP_OK) {
		/* Same SKIP/FAIL split as above, one level down: NOT_READY here means
		 * the bus opened but nothing ACKed at 0x30 -- expected on the current
		 * AEN bench batch, which is OPTIGA-DNI (not populated), not a defect. */
		if (s == ALP_ERR_NOT_READY) {
			printk("[se] RESULT SKIP: optiga_trust_m_init -> %d "
			       "(Trust M not ACKing; current AEN bench assemblies may be OPTIGA-DNI)\n",
			       (int)s);
		} else {
			printk("[se] RESULT FAIL: optiga_trust_m_init -> %d\n", (int)s);
		}
		alp_i2c_close(bus);
		printk("[se] done\n");
		return 0;
	}

	printk("[se] I2C_STATE probe -> ALP_OK\n");

	/* Coprocessor UID (data object 0xE0C2): opens the Trust M application
	 * through the host library and reads it.  Read-only. */
	optiga_trust_m_product_info_t info;
	s = optiga_trust_m_read_product_info(&se, &info);
	if (s != ALP_OK) {
		printk("[se] RESULT FAIL: read_product_info -> %d\n", (int)s);
		optiga_trust_m_deinit(&se);
		alp_i2c_close(bus);
		printk("[se] done\n");
		return 0;
	}
	printk("[se] UID: cim %02X platform %02X model %02X fw %02X%02X%02X%02X build %02X%02X\n",
	       info.cim_id,
	       info.platform_id,
	       info.model_id,
	       info.fw_id[0],
	       info.fw_id[1],
	       info.fw_id[2],
	       info.fw_id[3],
	       info.esw_build[0],
	       info.esw_build[1]);

	optiga_trust_m_deinit(&se);
	alp_i2c_close(bus);
	printk("[se] RESULT PASS: Trust M probe and Coprocessor UID read work\n");
	printk("[se] done\n");
	return 0;
}
