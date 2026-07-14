/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic end-to-end test for issue #742 (conversion-clean CC3501E Wi-Fi
 * security selection): drives the REAL CC3501E Wi-Fi backend ops vtable
 * (src/backends/wifi/cc3501e.c, reached via the REAL alp_backend_select()
 * registry selector -- the same production code path
 * alp_wifi_open()/alp_wifi_connect() use) and the REAL chip driver
 * (chips/cc3501e/cc3501e_wifi.c) against a software model of the firmware
 * SPI slave, and asserts the on-wire `security` byte the backend derives
 * from alp_wifi_credentials_t::psk (NULL/"" -> open, non-empty -> WPA2-PSK).
 *
 * This is a BEHAVIOURAL regression test for the fix, not just a
 * compile-warning check: the ternary this issue's diagnostic flagged
 * ((creds->psk == NULL || creds->psk[0] == '\0') ? ... : ...) picks the
 * SAME two values before and after the fix, so a warning-only check can't
 * tell a correct fix from an off-by-one break of the selection logic --
 * this test can.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/chips/cc3501e.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "backends/wifi/wifi_ops.h"

/* This test does NOT link src/wifi_dispatch.c (see CMakeLists.txt comment),
 * so it instantiates the wifi class-range entry itself -- the one thing
 * alp_backend_select("wifi", ...) needs to find src/backends/wifi/cc3501e.c's
 * ALP_BACKEND_REGISTER rows. */
ALP_BACKEND_DEFINE_CLASS(wifi);

/* ---- software model of the firmware slave (trimmed: CONNECT_STA only needs
 * a bare OK status reply, no data) ------------------------------------- */

enum slave_phase {
	PH_REQ_HDR = 0,
	PH_REQ_PL,
	PH_REPLY_HDR,
	PH_REPLY_PL,
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint16_t         req_len;
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD];
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase = PH_REQ_HDR;
}

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) {
		return ALP_OK;
	}
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd     = tx[0];
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave.phase = (slave.req_len > 0u) ? PH_REQ_PL : PH_REPLY_HDR;
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]       = slave.cmd; /* echo cmd */
		rx[1]       = 0x00u;
		rx[2]       = 1u; /* reply payload = 1 status byte */
		rx[3]       = 0x00u;
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		rx[0]       = ALP_CC3501E_RESP_OK;
		slave.phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

void alp_delay_us(uint32_t us)
{
	(void)us;
}
void alp_delay_ms(uint32_t ms)
{
	(void)ms;
}
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	return NULL;
}
alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

/* ---- fixture ----------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw;

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
	zassert_equal(alp_wifi_cc3501e_attach(&fw), ALP_OK, "attach the live bridge handle");
}

/* Selects the CC3501E backend the same way alp_wifi_open() does (by exact
 * silicon_ref) and drives its open() -- the production dispatch path, minus
 * the alp_wifi_t handle-pool bookkeeping this test doesn't need. Fails
 * loudly (not silently skips) if the registry didn't pick the CC3501E
 * backend, so a selector regression is attributable here too. */
static alp_wifi_backend_state_t open_cc3501e_state(void)
{
	const alp_backend_t *be = alp_backend_select("wifi", "alif:ensemble:e7");
	zassert_not_null(be, "CC3501E backend registered for alif:ensemble:e7");
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0, "picked the CC3501E backend");

	alp_wifi_backend_state_t state = { .be_data = NULL, .ops = (const alp_wifi_ops_t *)be->ops };
	alp_capabilities_t       caps  = { .flags = be->base_caps };
	zassert_equal(state.ops->open(&state, &caps), ALP_OK, "backend open -> OK");
	return state;
}

/* ---- tests --------------------------------------------------------------- */

ZTEST(cc3501e_wifi_backend_security, test_connect_no_psk_selects_open)
{
	alp_wifi_backend_state_t state = open_cc3501e_state();

	const alp_wifi_credentials_t creds = { .ssid = "OpenNet", .psk = NULL };
	zassert_equal(state.ops->connect(&state, &creds, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_WIFI_CONNECT_STA, "opcode 0x12 emitted");
	/* alp_cc3501e_wifi_connect_t: ssid_len(0) | psk_len(1) | security(2) | reserved(3). */
	zassert_equal(slave.req_pl[2], CC3501E_WIFI_CONNECT_SEC_OPEN, "NULL psk -> open (sec=0)");
	zassert_equal(slave.req_pl[1], 0u, "psk_len=0 for a NULL psk");

	state.ops->close(&state);
}

/* Boundary: an EMPTY (not NULL) psk string must also select open, not WPA2. */
ZTEST(cc3501e_wifi_backend_security, test_connect_empty_psk_selects_open)
{
	alp_wifi_backend_state_t state = open_cc3501e_state();

	const alp_wifi_credentials_t creds = { .ssid = "OpenNet", .psk = "" };
	zassert_equal(state.ops->connect(&state, &creds, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(slave.req_pl[2], CC3501E_WIFI_CONNECT_SEC_OPEN, "empty psk -> open (sec=0)");
	zassert_equal(slave.req_pl[1], 0u, "psk_len=0 for an empty psk");

	state.ops->close(&state);
}

ZTEST(cc3501e_wifi_backend_security, test_connect_with_psk_selects_wpa2_psk)
{
	alp_wifi_backend_state_t state = open_cc3501e_state();

	const alp_wifi_credentials_t creds = { .ssid = "SecureNet", .psk = "supersecret" };
	zassert_equal(state.ops->connect(&state, &creds, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(
	    slave.req_pl[2], CC3501E_WIFI_CONNECT_SEC_WPA2_PSK, "non-empty psk -> WPA2-PSK (sec=1)");
	zassert_equal(
	    slave.req_pl[1], (uint8_t)strlen("supersecret"), "psk_len matches the passphrase");

	state.ops->close(&state);
}

/* Boundary: a single-character psk (shortest non-empty) still selects WPA2. */
ZTEST(cc3501e_wifi_backend_security, test_connect_one_char_psk_selects_wpa2_psk)
{
	alp_wifi_backend_state_t state = open_cc3501e_state();

	const alp_wifi_credentials_t creds = { .ssid = "SecureNet", .psk = "x" };
	zassert_equal(state.ops->connect(&state, &creds, 100u), ALP_OK, "CONNECT -> OK");
	zassert_equal(
	    slave.req_pl[2], CC3501E_WIFI_CONNECT_SEC_WPA2_PSK, "1-char psk -> WPA2-PSK (sec=1)");

	state.ops->close(&state);
}

ZTEST_SUITE(cc3501e_wifi_backend_security, NULL, NULL, reset_before, NULL, NULL);
