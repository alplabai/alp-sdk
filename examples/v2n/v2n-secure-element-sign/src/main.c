/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-secure-element-sign -- exercise the current OPTIGA Trust M
 * probe-only contract.
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
	printk("[se] v2n-secure-element-sign (probe-only)\n");

	/* BRD_I2C carries the Trust M alongside the PMICs + RTC.
     * 400 kHz is the standard Trust M bus rate; the chip supports
     * up to 1 MHz Fast-mode+ if the rest of the bus does too. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = 0u,
	    .bitrate_hz = 400000u,
	});
	if (bus == NULL) {
		printk("[se] RESULT FAIL: alp_i2c_open failed: %d\n", (int)alp_last_error());
		printk("[se] done\n");
		return 0;
	}

	/* Init probes I2C_STATE.  It does not issue OPEN_APPLICATION. */
	optiga_trust_m_t se;
	alp_status_t     s = optiga_trust_m_init(&se, bus, OPTIGA_TRUST_M_I2C_ADDR);
	if (s != ALP_OK) {
		printk("[se] RESULT FAIL: optiga_trust_m_init -> %d (Trust M not ACKing)\n", (int)s);
		alp_i2c_close(bus);
		printk("[se] done\n");
		return 0;
	}

	printk("[se] I2C_STATE probe -> ALP_OK\n");

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

	/* apdu[] content is irrelevant here: send_apdu validates only
     * pointers/lengths before returning NOSUPPORT, so any non-empty
     * frame exercises the same path.  resp_len is pre-loaded with the
     * poison value 123 (impossible for an 8-byte resp buffer) so the
     * check below proves the driver actually zeroes *resp_len on the
     * NOSUPPORT path rather than leaving the caller's stale value. */
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
