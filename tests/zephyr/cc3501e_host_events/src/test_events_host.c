/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic host-side tests for the CC3501E async-event wrapper
 * (chips/cc3501e/cc3501e.c: cc3501e_poll_events).  These exercise the REAL
 * host driver -- its GET_PENDING_EVENTS request encoding, the 3-wire lockstep
 * transaction, and the packed-event reply decoding + per-event callback
 * dispatch -- against a software model of the firmware event-ring slave.
 *
 * The model lives in the test's alp_spi_transceive() stub: it plays the
 * firmware SPI-slave role in the CS-less lockstep and, on a GET_PENDING_EVENTS
 * request, packs whatever events the test queued (each { evt_opcode(1) |
 * len(1) | payload[len] }, per <alp/protocol/cc3501e.h>) into the reply DATA
 * and DRAINS them, exactly like the firmware ring.  We assert the host both
 * EMITS the right request and DECODES the packed list back into ordered
 * callbacks with the correct opcodes + payloads.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/protocol/cc3501e.h"

/* ---- software model of the firmware event-ring slave ----------------------- */

enum slave_phase {
	PH_REQ_HDR = 0, /* next transfer is a 4-byte request header  */
	PH_REQ_PL,      /* next transfer is the request payload      */
	PH_REPLY_HDR,   /* host reads the 4-byte reply header        */
	PH_REPLY_PL,    /* host reads the reply payload (status+data) */
};

/* A queued event in the model (mirrors firmware/cc3501e/src/event_ring.c). */
struct model_evt {
	uint8_t opcode;
	uint8_t len;
	uint8_t payload[16];
};

static struct {
	enum slave_phase phase;
	uint8_t          cmd;
	uint16_t         req_len;
	uint8_t          req_pl[ALP_CC3501E_MAX_PAYLOAD];

	uint8_t  reply_pl[ALP_CC3501E_MAX_PAYLOAD]; /* status byte + data */
	uint16_t reply_len;

	/* Event ring the model drains on GET_PENDING_EVENTS. */
	struct model_evt evt[32];
	size_t           evt_head;
	size_t           evt_count;
} slave;

static void slave_reset(void)
{
	memset(&slave, 0, sizeof(slave));
	slave.phase = PH_REQ_HDR;
}

static void model_queue_evt(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	struct model_evt *e = &slave.evt[(slave.evt_head + slave.evt_count) % 32u];
	e->opcode           = opcode;
	e->len              = len;
	if (len > 0u && payload != NULL) {
		memcpy(e->payload, payload, len);
	}
	slave.evt_count++;
}

/* Drain the model ring into the reply DATA, packing WHOLE entries only (the
 * firmware never splits a payload across replies). */
static void slave_dispatch(void)
{
	if (slave.cmd == ALP_CC3501E_CMD_GET_PENDING_EVENTS) {
		slave.reply_pl[0] = ALP_CC3501E_RESP_OK;
		size_t off        = 1u;
		while (slave.evt_count > 0u) {
			const struct model_evt *e    = &slave.evt[slave.evt_head];
			const size_t            need = (size_t)ALP_CC3501E_EVENT_HDR_BYTES + e->len;
			if (off + need > sizeof(slave.reply_pl)) {
				break; /* would overflow the reply -- leave it queued */
			}
			slave.reply_pl[off]      = e->opcode;
			slave.reply_pl[off + 1u] = e->len;
			memcpy(&slave.reply_pl[off + ALP_CC3501E_EVENT_HDR_BYTES], e->payload, e->len);
			off += need;
			slave.evt_head = (slave.evt_head + 1u) % 32u;
			slave.evt_count--;
		}
		slave.reply_len = (uint16_t)off;
		return;
	}
	/* Any other opcode: a bare OK status (the tests only drive events). */
	slave.reply_pl[0] = ALP_CC3501E_RESP_OK;
	slave.reply_len   = 1u;
}

/* ---- test doubles for the alp_* seams the host driver links against -------- */

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
		if (slave.req_len > 0u) {
			slave.phase = PH_REQ_PL;
		} else {
			slave_dispatch();
			slave.phase = PH_REPLY_HDR;
		}
		break;
	case PH_REQ_PL:
		memcpy(slave.req_pl, tx, len);
		if (rx != NULL) {
			memset(rx, ALP_CC3501E_SYNC_IDLE, len);
		}
		slave_dispatch();
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

/* ---- callback capture ------------------------------------------------------ */

struct captured_evt {
	uint8_t opcode;
	uint8_t len;
	uint8_t payload[16];
};

static struct captured_evt cap[32];
static size_t              cap_count;
static void               *cap_user;

static void capture_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	cap_user = user;
	if (cap_count >= 32u) {
		return;
	}
	struct captured_evt *c = &cap[cap_count++];
	c->opcode              = opcode;
	c->len                 = (uint8_t)len;
	if (len > 0u && payload != NULL) {
		memcpy(c->payload, payload, (len > 16u) ? 16u : len);
	}
}

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw;

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	memset(cap, 0, sizeof(cap));
	cap_count = 0;
	cap_user  = NULL;
	zassert_equal(cc3501e_init(&fw, fake_bus), ALP_OK, "init binds the (fake) bus");
}

/* ---- tests ----------------------------------------------------------------- */

/* No callback registered -> poll is a no-op that leaves the ring untouched and
 * clocks NO transaction (events stay queued until a sink is attached). */
ZTEST(cc3501e_host_events, test_poll_without_callback_is_noop)
{
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(slave.cmd, 0u, "no transaction was clocked (cb unset)");
	zassert_equal(slave.evt_count, 1u, "event still queued firmware-side");
	zassert_equal(cap_count, 0u, "no callback fired");
}

/* Empty ring -> OK, GET_PENDING_EVENTS emitted, zero callbacks. */
ZTEST(cc3501e_host_events, test_poll_empty_queue)
{
	zassert_equal(cc3501e_set_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_PENDING_EVENTS, "opcode 0x05 emitted");
	zassert_equal(slave.req_len, 0u, "GET_PENDING_EVENTS carries no request payload");
	zassert_equal(cap_count, 0u, "no events -> no callbacks");
}

/* Two payloadless Wi-Fi events are decoded + dispatched in FIFO order. */
ZTEST(cc3501e_host_events, test_poll_wifi_connect_then_disconnect)
{
	int marker = 42;
	zassert_equal(cc3501e_set_event_callback(&fw, capture_cb, &marker), ALP_OK, "set cb");
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	model_queue_evt(ALP_CC3501E_EVT_WIFI_DISCONNECTED, NULL, 0u);

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count, 2u, "both events dispatched");
	zassert_equal(cap[0].opcode, ALP_CC3501E_EVT_WIFI_CONNECTED, "first = connected");
	zassert_equal(cap[0].len, 0u, "connected has no payload");
	zassert_equal(cap[1].opcode, ALP_CC3501E_EVT_WIFI_DISCONNECTED, "second = disconnected");
	zassert_equal(cap_user, &marker, "user pointer threaded through");
	zassert_equal(slave.evt_count, 0u, "ring drained");
}

/* An event WITH a payload (e.g. a GPIO edge) round-trips the payload bytes. */
ZTEST(cc3501e_host_events, test_poll_event_with_payload)
{
	zassert_equal(cc3501e_set_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	const uint8_t gpio_evt[8] = { 6u, 1u, 0u, 0u, 0x11, 0x22, 0x33, 0x44 };
	model_queue_evt(ALP_CC3501E_EVT_GPIO_INTERRUPT, gpio_evt, sizeof(gpio_evt));

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count, 1u, "one event dispatched");
	zassert_equal(cap[0].opcode, ALP_CC3501E_EVT_GPIO_INTERRUPT, "gpio event opcode");
	zassert_equal(cap[0].len, sizeof(gpio_evt), "payload length preserved");
	zassert_mem_equal(cap[0].payload, gpio_evt, sizeof(gpio_evt), "payload bytes preserved");
}

/* #740: the callback is handed pointers INTO ctx->evt_buf, valid only for
 * that one call.  If the callback re-enters cc3501e_poll_events() on the
 * SAME ctx (the concrete reentrancy risk the issue calls out), the inner
 * call must be rejected with ALP_ERR_BUSY instead of racing/overwriting the
 * buffer the outer walk is still reading from. */
static alp_status_t reentrant_poll_rc = ALP_OK;
static size_t       reentrant_poll_calls;

static void reentrant_cb(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
	capture_cb(opcode, payload, len, user);
	reentrant_poll_calls++;
	/* Re-enter from inside the callback -- must not alias evt_buf. */
	reentrant_poll_rc = cc3501e_poll_events(&fw);
}

ZTEST(cc3501e_host_events, test_poll_reentrant_from_callback_is_rejected_740)
{
	zassert_equal(cc3501e_set_event_callback(&fw, reentrant_cb, NULL), ALP_OK, "set cb");
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	model_queue_evt(ALP_CC3501E_EVT_WIFI_DISCONNECTED, NULL, 0u);
	reentrant_poll_calls = 0;
	reentrant_poll_rc    = ALP_OK;

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "outer poll -> OK");
	zassert_equal(reentrant_poll_calls, 2u, "callback fired for both queued events");
	zassert_equal(reentrant_poll_rc, ALP_ERR_BUSY, "reentrant inner poll rejected -> BUSY");
	zassert_equal(cap_count, 2u, "both events still delivered correctly by the OUTER poll");
	zassert_equal(cap[0].opcode, ALP_CC3501E_EVT_WIFI_CONNECTED, "first = connected");
	zassert_equal(cap[1].opcode, ALP_CC3501E_EVT_WIFI_DISCONNECTED, "second = disconnected");

	/* Guard cleared after the outer call returns -- a later, non-reentrant
	 * poll works normally again. */
	zassert_false(fw.evt_busy, "evt_busy cleared once the outer poll returns");
	zassert_equal(cc3501e_set_event_callback(&fw, capture_cb, NULL), ALP_OK, "restore plain cb");
	cap_count = 0;
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll after reentrancy -> OK");
	zassert_equal(cap_count, 0u, "ring was already drained");
}

/* A second poll after a drain sees an empty ring (delivered exactly once). */
ZTEST(cc3501e_host_events, test_events_delivered_exactly_once)
{
	zassert_equal(cc3501e_set_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "first poll -> OK");
	zassert_equal(cap_count, 1u, "delivered once");

	cap_count = 0;
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "second poll -> OK");
	zassert_equal(cap_count, 0u, "nothing left to deliver");
}

ZTEST_SUITE(cc3501e_host_events, NULL, NULL, reset_before, NULL, NULL);
