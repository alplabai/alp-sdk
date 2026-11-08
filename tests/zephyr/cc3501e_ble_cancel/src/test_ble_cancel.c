/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #3103: a CC3501E alp_ble_gatt_read() blocked with UINT32_MAX must
 * return ALP_ERR_NOT_READY once another thread calls alp_ble_disconnect().
 * Goes through the public alp_ble_* dispatcher, the real CC3501E BLE
 * backend and the real driver; the fake slave answers every BLE_GATT_READ
 * with RESP_ERR_BUSY (a peer that never answers), everything else bare-OK.
 * Fails (join times out) if the backend passes NULL as the cancel flag.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include <alp/ble.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "cc3501e_reply_model.h"

enum slave_phase { PH_REQ_HDR = 0, PH_REQ_PL, PH_REPLY_HDR, PH_REPLY_PL };

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint16_t         req_len;
	uint8_t          reply_pl[ALP_CC3501E_MAX_PAYLOAD];
	uint16_t         reply_len;
} slave;

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)bus;
	if (len == 0u) return ALP_OK;
	switch (slave.phase) {
	case PH_REQ_HDR:
		slave.cmd     = tx[0];
		slave.req_len = (uint16_t)tx[2] | ((uint16_t)tx[3] << 8);
		if (slave.cmd == ALP_CC3501E_CMD_BLE_GATT_READ) {
			slave.reply_len = cc3501e_model_stage_legacy_reply(
			    slave.reply_pl, ALP_CC3501E_RESP_ERR_BUSY, NULL, 0u);
		} else {
			slave.reply_len =
			    cc3501e_model_stage_reply(slave.reply_pl, slave.cmd, ALP_CC3501E_RESP_OK, NULL, 0u);
		}
		if (rx != NULL) memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		slave.phase = (slave.req_len > 0u) ? PH_REQ_PL : PH_REPLY_HDR;
		break;
	case PH_REQ_PL:
		if (rx != NULL) memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		slave.phase = PH_REPLY_HDR;
		break;
	case PH_REPLY_HDR:
		rx[0]       = slave.cmd;
		rx[1]       = 0x00u;
		rx[2]       = (uint8_t)(slave.reply_len & 0xFFu);
		rx[3]       = (uint8_t)((slave.reply_len >> 8) & 0xFFu);
		slave.phase = PH_REPLY_PL;
		break;
	case PH_REPLY_PL:
		memcpy(rx, slave.reply_pl, len);
		slave.phase = PH_REQ_HDR;
		break;
	}
	return ALP_OK;
}

/* Real time, so the poll loop sleeps between attempts and yields to the
 * test thread. */
void alp_delay_ms(uint32_t ms)
{
	k_msleep(ms);
}
void alp_delay_us(uint32_t us)
{
	k_busy_wait(us);
}
uint64_t alp_uptime_ms(void)
{
	return (uint64_t)k_uptime_get();
}
uint64_t alp_uptime_us(void)
{
	return alp_uptime_ms() * 1000u;
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

static cc3501e_t fw;

static K_THREAD_STACK_DEFINE(reader_stack, 16384);
static struct k_thread reader_thread;
static alp_ble_conn_t *reader_conn;
static alp_status_t    reader_rc = ALP_OK;

static void reader_entry(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;
	uint8_t out[4];
	size_t  n = 0;
	reader_rc = alp_ble_gatt_read(reader_conn, 0x0042u, out, sizeof(out), &n, UINT32_MAX);
}

ZTEST(cc3501e_ble_cancel, test_disconnect_wakes_blocked_gatt_read_3103)
{
	memset(&slave, 0, sizeof(slave));
	zassert_equal(cc3501e_init(&fw, (alp_spi_t *)&fw), ALP_OK, "init");
	zassert_equal(alp_ble_cc3501e_attach(&fw), ALP_OK, "attach");

	alp_ble_t *ble = alp_ble_open();
	zassert_not_null(ble, "alp_ble_open (BLE_ENABLE)");
	const alp_ble_addr_t peer = { .addr = { 1, 2, 3, 4, 5, 6 }, .type = 0 };
	zassert_equal(alp_ble_connect(ble, &peer, 1000u, &reader_conn), ALP_OK, "connect");

	reader_rc = ALP_OK;
	k_thread_create(&reader_thread,
	                reader_stack,
	                K_THREAD_STACK_SIZEOF(reader_stack),
	                reader_entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(5),
	                0,
	                K_NO_WAIT);
	k_msleep(200);
	zassert_not_equal(k_thread_join(&reader_thread, K_NO_WAIT), 0, "read must still be blocked");

	(void)alp_ble_disconnect(reader_conn);

	if (k_thread_join(&reader_thread, K_SECONDS(2)) != 0) {
		k_thread_abort(&reader_thread);
		zassert_unreachable("disconnect must wake the blocked read");
	}
	zassert_equal(reader_rc, ALP_ERR_NOT_READY, "woken read -> NOT_READY (got %d)", reader_rc);
	alp_ble_close(ble);
}

ZTEST_SUITE(cc3501e_ble_cancel, NULL, NULL, NULL, NULL, NULL);
