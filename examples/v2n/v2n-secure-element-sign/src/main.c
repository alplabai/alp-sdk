/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-secure-element-sign -- talk to the on-module OPTIGA Trust M.
 *
 * Three steps, each one layer deeper:
 *   1. optiga_trust_m_init() probes the chip's I2C_STATE register.
 *   2. optiga_trust_m_read_product_info() opens the Trust M application
 *      through Infineon's host library and reads the Coprocessor UID
 *      (data object 0xE0C2).
 *   3. optiga_trust_m_send_apdu() runs a raw APDU session: this app sends
 *      OpenApplication itself, then GetDataObject(0xE0C2), and checks the
 *      bytes match step 2.  The raw session is the escape hatch for
 *      commands the driver has no typed call for yet.
 *
 * Nothing here writes to the chip: no keys, no data objects, no
 * lifecycle change.
 *
 * RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
 * (metadata/e1m_modules/v2n/core-ownership.yaml) -- the CM33 must
 * never master it.  This is a Linux/Yocto user-space app on the
 * V2N Cortex-A55, following the same `alp_i2c_*` + chip-driver
 * pattern as examples/v2n/v2n-power-monitor.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/chips/optiga_trust_m.h"

/* BRD_I2C = Linux /dev/i2c-8: meta-alp-sdk's e1m-v2n-som.dtsi
 * aliases `i2c8 = &i2c8;`, so `alp_i2c_open(.bus_id = 8)` here opens
 * /dev/i2c-8.  A literal (not a board-header macro) because BRD_I2C
 * is a SoM-level bus, not one of the E1M-X-EVK carrier's own routed
 * pins. */
#define V2N_BRD_I2C_BUS_ID 8u

/* Trust M APDUs are [cmd][param][in-length, 2 bytes BE][in-data], and a
 * response is [status][undefined][out-length, 2 bytes BE][out-data].
 * Setting bit 7 of the command byte clears the chip's last-error code. */

/* OpenApplication (0xF0) with the Trust M application's 16-byte AID. */
static const uint8_t k_open_app[] = { 0xF0u, 0x00u, 0x00u, 0x10u, 0xD2u, 0x76u, 0x00u,
	                              0x00u, 0x04u, 0x47u, 0x65u, 0x6Eu, 0x41u, 0x75u,
	                              0x74u, 0x68u, 0x41u, 0x70u, 0x70u, 0x6Cu };

/* GetDataObject (0x81), read data: OID 0xE0C2, offset 0, 27 bytes. */
static const uint8_t k_get_uid[] = { 0x81u, 0x00u, 0x00u, 0x06u, 0xE0u,
	                             0xC2u, 0x00u, 0x00u, 0x00u, 0x1Bu };

#define APDU_RESP_HEADER 4u

static int fail(optiga_trust_m_t *se, alp_i2c_t *bus, const char *why, int s)
{
	printf("[se] RESULT FAIL: %s -> %d\n", why, s);
	if (se != NULL) optiga_trust_m_deinit(se);
	alp_i2c_close(bus);
	printf("[se] done\n");
	return 1;
}

int main(void)
{
	printf("[se] v2n-secure-element-sign\n");

	/* BRD_I2C carries the Trust M alongside the PMICs + RTC.
	 * 400 kHz is the standard Trust M bus rate; the chip supports
	 * up to 1 MHz Fast-mode+ if the rest of the bus does too. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = V2N_BRD_I2C_BUS_ID,
	    .bitrate_hz = 400000u,
	});
	if (bus == NULL) {
		printf("[se] RESULT FAIL: alp_i2c_open failed: %d\n", (int)alp_last_error());
		printf("[se] done\n");
		return 1;
	}

	/* 1. Probe: is the chip fitted and answering on its address? */
	optiga_trust_m_t se;
	alp_status_t     s = optiga_trust_m_init(&se, bus, OPTIGA_TRUST_M_I2C_ADDR);
	if (s != ALP_OK) return fail(NULL, bus, "optiga_trust_m_init (Trust M not ACKing)", s);
	printf("[se] I2C_STATE probe -> ALP_OK\n");

	/* 2. Coprocessor UID through the driver's typed call. */
	optiga_trust_m_product_info_t info;
	s = optiga_trust_m_read_product_info(&se, &info);
	if (s != ALP_OK) return fail(&se, bus, "read_product_info", s);
	printf("[se] UID: cim %02X platform %02X model %02X fw %02X%02X%02X%02X build %02X%02X\n",
	       info.cim_id,
	       info.platform_id,
	       info.model_id,
	       info.fw_id[0],
	       info.fw_id[1],
	       info.fw_id[2],
	       info.fw_id[3],
	       info.esw_build[0],
	       info.esw_build[1]);

	/* 3. The same object over a raw APDU session.  The session starts
	 *    on a fresh link, so OpenApplication comes first. */
	uint8_t resp[64];
	size_t  resp_len = 0;
	s = optiga_trust_m_send_apdu(&se, k_open_app, sizeof k_open_app, resp, sizeof resp,
	                             &resp_len, 1000u);
	if (s != ALP_OK || resp_len < 1u || resp[0] != 0x00u) {
		return fail(&se, bus, "raw OpenApplication", s != ALP_OK ? s : resp[0]);
	}
	s = optiga_trust_m_send_apdu(&se, k_get_uid, sizeof k_get_uid, resp, sizeof resp,
	                             &resp_len, 1000u);
	if (s != ALP_OK || resp_len != APDU_RESP_HEADER + sizeof info || resp[0] != 0x00u) {
		return fail(&se, bus, "raw GetDataObject(0xE0C2)", s != ALP_OK ? s : resp[0]);
	}
	if (memcmp(resp + APDU_RESP_HEADER, &info, sizeof info) != 0) {
		return fail(&se, bus, "raw UID differs from read_product_info", -1);
	}
	printf("[se] raw APDU UID matches (%u bytes)\n", (unsigned)sizeof info);

	optiga_trust_m_deinit(&se);
	alp_i2c_close(bus);
	printf("[se] RESULT PASS: Trust M probe, Coprocessor UID and raw APDU session work\n");
	printf("[se] done\n");
	return 0;
}
