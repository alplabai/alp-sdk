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

	/* #1740: when set, pad every reply up to an 8-byte multiple with zeros,
	 * exactly as the firmware's protocol_build_reply() does for DMA burst
	 * alignment -- including folding the pad INTO the declared reply_len. */
	bool pad_replies;

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
static void model_apply_reply_padding(void);

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
		if (slave.pad_replies) {
			model_apply_reply_padding();
		}
		return;
	}
	/* Any other opcode: a bare OK status (the tests only drive events). */
	slave.reply_pl[0] = ALP_CC3501E_RESP_OK;
	slave.reply_len   = 1u;
}

/* #1740: mirror protocol_build_reply()'s CC3501E_REPLY_PAD alignment. */
#define MODEL_REPLY_PAD 8u
static void model_apply_reply_padding(void)
{
	const uint16_t rem = (uint16_t)(slave.reply_len % MODEL_REPLY_PAD);
	if (rem == 0u) {
		return;
	}
	const uint16_t pad = (uint16_t)(MODEL_REPLY_PAD - rem);
	if ((size_t)slave.reply_len + pad > sizeof(slave.reply_pl)) {
		return;
	}
	memset(&slave.reply_pl[slave.reply_len], 0, pad);
	slave.reply_len = (uint16_t)(slave.reply_len + pad);
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

/* Second, independent sink (issue #1723).  The defect this models is the SDK's
 * own console companion registering on the same context as the application: the
 * old single-slot API let the second registration silently displace the first,
 * so one of the two consumers received nothing. */
static size_t  cap2_count;
static uint8_t cap2_last_opcode;
static void   *cap2_user;

static void capture_cb2(uint8_t opcode, const uint8_t *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	cap2_user        = user;
	cap2_last_opcode = opcode;
	cap2_count++;
}

/* ---- fixture --------------------------------------------------------------- */

static cc3501e_t  fw;
static alp_spi_t *fake_bus = (alp_spi_t *)&fw;

static void reset_before(void *fixture)
{
	(void)fixture;
	slave_reset();
	memset(cap, 0, sizeof(cap));
	cap_count        = 0;
	cap_user         = NULL;
	cap2_count       = 0;
	cap2_last_opcode = 0;
	cap2_user        = NULL;
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
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(slave.cmd, ALP_CC3501E_CMD_GET_PENDING_EVENTS, "opcode 0x05 emitted");
	zassert_equal(slave.req_len, 0u, "GET_PENDING_EVENTS carries no request payload");
	zassert_equal(cap_count, 0u, "no events -> no callbacks");
}

/* Two payloadless Wi-Fi events are decoded + dispatched in FIFO order. */
ZTEST(cc3501e_host_events, test_poll_wifi_connect_then_disconnect)
{
	int marker = 42;
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, &marker), ALP_OK, "set cb");
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
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
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

/* #1740: an EMPTY ring still returns a PADDED reply -- the firmware pads to an
 * 8-byte multiple for DMA burst alignment and folds the pad into the declared
 * payload length, so the host receives 7 zero bytes of DATA (measured on an
 * AEN801: "[evtdbg] got=7 raw=00 00 00 00").  Walked naively those are three
 * {opcode 0, len 0} entries -- ~5.8 phantom events/second on an idle bench,
 * fanned out to every subscriber.  0x00 is not a defined ALP_CC3501E_EVT_*
 * opcode, so it must terminate the walk. */
ZTEST(cc3501e_host_events, test_padded_empty_reply_dispatches_nothing_1740)
{
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	slave.pad_replies = true;

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(slave.reply_len, 8u, "firmware padded the bare-OK reply to 8 B");
	zassert_equal(cap_count, 0u, "padding must NOT be decoded as events");
}

/* #1740 companion: a REAL entry followed by padding still dispatches exactly
 * once.  Padding is appended after the data, so the guard must stop at the pad
 * without swallowing the entry that precedes it. */
ZTEST(cc3501e_host_events, test_real_event_then_padding_dispatches_once_1740)
{
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	slave.pad_replies = true;
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(slave.reply_len, 8u, "status + 1 entry (2 B) padded up to 8 B");
	zassert_equal(cap_count, 1u, "exactly one real event, pad ignored");
	zassert_equal(cap[0].opcode, ALP_CC3501E_EVT_WIFI_CONNECTED, "the real opcode");
	zassert_equal(cap[0].len, 0u, "no payload");
}

ZTEST(cc3501e_host_events, test_poll_reentrant_from_callback_is_rejected_740)
{
	zassert_equal(cc3501e_add_event_callback(&fw, reentrant_cb, NULL), ALP_OK, "set cb");
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
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "restore plain cb");
	cap_count = 0;
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll after reentrancy -> OK");
	zassert_equal(cap_count, 0u, "ring was already drained");
}

/* #740: evt_buf's per-context storage move (mirrors wifi_scan_buf/ble_scan_buf)
 * had no dedicated regression test of its own -- only the busy-guard test above
 * exercised this file, so reverting JUST the storage move (ctx->evt_buf back to
 * a function-local `static`) while keeping the busy guard would go completely
 * uncaught. Byte-compare ctx A's raw decode buffer against the independently
 * reconstructed wire bytes (fails on the pre-fix `static` buffer, which never
 * writes ctx->evt_buf), then run an independent second context's OWN real poll
 * with genuinely different staged content and confirm ctx A's buffer is
 * unaffected -- same discriminating shape as test_wifi_scan_buf_is_per_context_740. */
ZTEST(cc3501e_host_events, test_evt_buf_is_per_context_740)
{
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb ctx A");
	const uint8_t gpio_evt[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	model_queue_evt(ALP_CC3501E_EVT_GPIO_INTERRUPT, gpio_evt, sizeof(gpio_evt));

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll ctx A -> OK");
	zassert_equal(cap_count, 2u, "both events dispatched on ctx A");

	uint8_t wire_a[ALP_CC3501E_MAX_PAYLOAD];
	size_t  wire_a_off   = 0u;
	wire_a[wire_a_off++] = ALP_CC3501E_EVT_WIFI_CONNECTED;
	wire_a[wire_a_off++] = 0u;
	wire_a[wire_a_off++] = ALP_CC3501E_EVT_GPIO_INTERRUPT;
	wire_a[wire_a_off++] = (uint8_t)sizeof(gpio_evt);
	memcpy(&wire_a[wire_a_off], gpio_evt, sizeof(gpio_evt));
	wire_a_off += sizeof(gpio_evt);

	zassert_mem_equal(fw.evt_buf,
	                  wire_a,
	                  wire_a_off,
	                  "ctx A's cc3501e_poll_events must decode into ctx->evt_buf itself "
	                  "(#740) -- fails against the pre-fix function-local `static` buffer, "
	                  "which never touches this field");

	uint8_t snapshot[ALP_CC3501E_MAX_PAYLOAD];
	memcpy(snapshot, fw.evt_buf, sizeof(snapshot));

	/* Independent second context runs its OWN real poll, through the same
	 * driver entry point, staged with genuinely different content. */
	cc3501e_t ctx_b;
	zassert_equal(cc3501e_init(&ctx_b, fake_bus), ALP_OK, "init ctx B");
	slave_reset();
	memset(cap, 0, sizeof(cap));
	cap_count = 0;
	zassert_equal(cc3501e_add_event_callback(&ctx_b, capture_cb, NULL), ALP_OK, "set cb ctx B");
	const uint8_t b_payload[2] = { 0x55, 0x66 };
	model_queue_evt(ALP_CC3501E_EVT_WIFI_DISCONNECTED, b_payload, sizeof(b_payload));
	zassert_equal(cc3501e_poll_events(&ctx_b), ALP_OK, "poll ctx B -> OK");
	zassert_equal(cap_count, 1u, "ctx B's distinct staged event delivered");
	zassert_equal(
	    cap[0].opcode, ALP_CC3501E_EVT_WIFI_DISCONNECTED, "ctx B decoded ITS OWN staged event");

	zassert_mem_equal(fw.evt_buf,
	                  snapshot,
	                  sizeof(snapshot),
	                  "ctx A's evt_buf must be unaffected by ctx B's OWN real poll (#740)");
}

/* A second poll after a drain sees an empty ring (delivered exactly once). */
ZTEST(cc3501e_host_events, test_events_delivered_exactly_once)
{
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "set cb");
	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);

	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "first poll -> OK");
	zassert_equal(cap_count, 1u, "delivered once");

	cap_count = 0;
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "second poll -> OK");
	zassert_equal(cap_count, 0u, "nothing left to deliver");
}

/* #1723: two subscribers on ONE context BOTH receive every event.
 *
 * This is the shape that actually shipped broken: the application registers in
 * main(), then the console companion registers on the same ctx from its own
 * init path.  Under the old single-slot API the second registration silently
 * replaced the first, so the application's callback never ran again while
 * cc3501e_poll_events() kept returning ALP_OK -- undetectable from the caller.
 * On that code this test fails on the FIRST subscriber's count, which is
 * exactly the symptom (bench-confirmed: ring drained, application saw none). */
ZTEST(cc3501e_host_events, test_two_subscribers_both_receive_events_1723)
{
	int marker_a = 0;
	int marker_b = 0;
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, &marker_a), ALP_OK, "sub A");
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb2, &marker_b), ALP_OK, "sub B");

	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	model_queue_evt(ALP_CC3501E_EVT_WIFI_DISCONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");

	zassert_equal(cap_count, 2u, "subscriber A got BOTH events (the #1723 regression)");
	zassert_equal(cap2_count, 2u, "subscriber B got both events too");
	zassert_equal(cap_user, &marker_a, "A keeps its own user pointer");
	zassert_equal(cap2_user, &marker_b, "B keeps its own user pointer");
	zassert_equal(cap2_last_opcode, ALP_CC3501E_EVT_WIFI_DISCONNECTED, "B saw the last event");
}

/* #1723: registering the SAME (cb, user) pair twice must not double-deliver. */
ZTEST(cc3501e_host_events, test_duplicate_registration_is_idempotent_1723)
{
	int marker = 0;
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, &marker), ALP_OK, "first add");
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, &marker), ALP_OK, "duplicate add");

	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count, 1u, "one event delivered ONCE, not twice");

	/* Same callback with a DIFFERENT user is a distinct subscription. */
	int other = 0;
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, &other), ALP_OK, "distinct user");
	cap_count = 0;
	model_queue_evt(ALP_CC3501E_EVT_WIFI_DISCONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count, 2u, "both subscriptions fire");
}

/* #1723: unsubscribing stops delivery; removing an unknown pair is reported. */
ZTEST(cc3501e_host_events, test_remove_event_callback_1723)
{
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb, NULL), ALP_OK, "sub A");
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb2, NULL), ALP_OK, "sub B");
	zassert_equal(cc3501e_remove_event_callback(&fw, capture_cb, NULL), ALP_OK, "remove A");
	zassert_equal(cc3501e_remove_event_callback(&fw, capture_cb, NULL),
	              ALP_ERR_NOT_FOUND,
	              "removing it again reports NOT_FOUND");

	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count, 0u, "removed subscriber gets nothing");
	zassert_equal(cap2_count, 1u, "the remaining subscriber still gets it");
}

/* #1723: the slot array is finite, and exhausting it REFUSES the new
 * registration rather than displacing a subscriber that is already working --
 * failing closed is what makes the limit debuggable instead of silent. */
ZTEST(cc3501e_host_events, test_subscriber_slots_fail_closed_1723)
{
	static int markers[CC3501E_EVENT_SUBSCRIBERS];
	for (size_t i = 0; i < CC3501E_EVENT_SUBSCRIBERS; i++) {
		zassert_equal(
		    cc3501e_add_event_callback(&fw, capture_cb, &markers[i]), ALP_OK, "slot fills");
	}
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb2, NULL),
	              ALP_ERR_NOMEM,
	              "one past the last slot is refused");

	model_queue_evt(ALP_CC3501E_EVT_WIFI_CONNECTED, NULL, 0u);
	zassert_equal(cc3501e_poll_events(&fw), ALP_OK, "poll -> OK");
	zassert_equal(cap_count,
	              (size_t)CC3501E_EVENT_SUBSCRIBERS,
	              "every ACCEPTED subscriber still receives the event");
	zassert_equal(cap2_count, 0u, "the refused one receives nothing");

	/* Freeing a slot makes room again. */
	zassert_equal(cc3501e_remove_event_callback(&fw, capture_cb, &markers[0]), ALP_OK, "free");
	zassert_equal(cc3501e_add_event_callback(&fw, capture_cb2, NULL), ALP_OK, "now it fits");
}

/* #1723: a NULL callback is rejected outright -- unsubscribing goes through
 * cc3501e_remove_event_callback, so NULL can never occupy a slot. */
ZTEST(cc3501e_host_events, test_null_callback_rejected_1723)
{
	zassert_equal(cc3501e_add_event_callback(&fw, NULL, NULL), ALP_ERR_INVAL, "add NULL");
	zassert_equal(cc3501e_remove_event_callback(&fw, NULL, NULL), ALP_ERR_INVAL, "remove NULL");
}

ZTEST_SUITE(cc3501e_host_events, NULL, NULL, reset_before, NULL, NULL);
