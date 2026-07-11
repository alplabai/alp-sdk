/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-secure-element-sign -- exercise the current OPTIGA Trust M
 * probe-only contract on the E1M-AEN (Alif Ensemble) SoM.
 *
 * On the E1M-AEN the Trust M sits on **BRD_I2C** -- the on-module
 * housekeeping bus (the Alif LPI2C0 / "LP-island" I2C, P7_4 SCL_A /
 * P7_5 SDA_A) -- at 7-bit address 0x30, alongside the RTC + EEPROM +
 * TMP112.  Because BRD_I2C lives in the low-power domain it is owned
 * by the **M55-HE** subsystem (hence this example's board target is
 * rtss_he).  The chip and the sign flow are identical to the V2N
 * variant -- everything goes through the SoM-portable <alp/...> API,
 * so the only AEN-specific facts are the bus + the owning core.
 *
 * The in-tree v0.3 driver probes the Trust M I2C_STATE register only.
 * Product-info and raw-APDU helpers are declared for the planned host
 * library integration, but return ALP_ERR_NOSUPPORT today after
 * argument validation.  This example keeps the historical name while
 * making that probe-only contract explicit.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/optiga_trust_m.h"

int main(void)
{
	printk("[se] aen-secure-element-sign (probe-only)\n");

	/* BRD_I2C carries the Trust M alongside the RTC + EEPROM + TMP112.
     * On the E1M-AEN this is the Alif LPI2C0 (the LP-island I2C),
     * surfaced as portable bus 0.  400 kHz is the standard Trust M bus
     * rate; the chip supports up to 1 MHz Fast-mode+ if the rest of
     * the bus does too. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 0u,
	    .bitrate_hz = 400000u,
	});
	if (bus == NULL) {
		/* NOT_READY vs anything else matters here: NOT_READY means "the LPI2C0
		 * backend isn't wired up on this build/board" -- an environment gap,
		 * not a driver bug -- so it's a SKIP. Any other code means the open
		 * itself is broken and should fail the run. */
		const alp_status_t last = alp_last_error();
		if (last == ALP_ERR_NOT_READY) {
			printk("[se] RESULT SKIP: alp_i2c_open failed: %d "
			       "(BRD_I2C/LPI2C0 not ready on this bench)\n",
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

	/* GET_DATA_OBJECT needs the full Infineon APDU transport, which isn't
	 * integrated yet -- the driver validates args then returns NOSUPPORT
	 * rather than fabricating a response. NOSUPPORT is therefore the PASS
	 * value here: this asserts the stub's contract hasn't silently drifted
	 * (e.g. started returning stale/zeroed info instead of refusing). */
	optiga_trust_m_product_info_t info;
	s = optiga_trust_m_read_product_info(&se, &info);
	printk("[se] read_product_info -> %d (expected NOSUPPORT)\n", (int)s);
	if (s != ALP_ERR_NOSUPPORT) {
		printk("[se] RESULT FAIL: product-info path no longer matches the probe-only contract\n");
		optiga_trust_m_deinit(&se);
		alp_i2c_close(bus);
		printk("[se] done\n");
		return 0;
	}

	/* Same contract check for the raw-APDU path: this APDU (SET DATA OBJECT,
	 * tag 0x11) is a stand-in -- it never reaches the wire, since the driver
	 * rejects it at argument validation before any I2C transaction. resp_len
	 * is pre-poisoned (123) so the check below proves the driver zeroes it
	 * on the NOSUPPORT path rather than leaving stale data. */
	uint8_t apdu[4]  = { 0x31u, 0x11u, 0x00u, 0x00u };
	uint8_t resp[8]  = { 0 };
	size_t  resp_len = 123u;
	s = optiga_trust_m_send_apdu(&se, apdu, sizeof apdu, resp, sizeof resp, &resp_len, 1000u);
	printk("[se] send_apdu -> %d resp_len=%u (expected NOSUPPORT, zero bytes)\n",
	       (int)s,
	       (unsigned)resp_len);
	if (s != ALP_ERR_NOSUPPORT || resp_len != 0u) {
		printk("[se] RESULT FAIL: raw-APDU path no longer matches the probe-only contract\n");
		optiga_trust_m_deinit(&se);
		alp_i2c_close(bus);
		printk("[se] done\n");
		return 0;
	}

	optiga_trust_m_deinit(&se);
	alp_i2c_close(bus);
	printk("[se] RESULT PASS: Trust M I2C_STATE probe works; product-info/raw-APDU are "
	       "cleanly blocked with ALP_ERR_NOSUPPORT\n");
	printk("[se] done\n");
	return 0;
}
