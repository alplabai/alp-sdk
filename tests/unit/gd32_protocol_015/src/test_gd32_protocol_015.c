/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32 bridge wire protocol v0.15 -- host driver against a byte-level
 * model of the bridge (alp_spi_write/read are defined here, so the real
 * gd32g553_init_ex() + spi_xfer_core() run unmodified).
 *
 * The model answers either wire generation:
 *   - minor 14: no 6-byte LINK_FEATURES, no BATCH / BEGIN2 / READ2 (a v0.14
 *     bridge answers the 6-byte form with STATUS_INVAL, as src/protocol.c
 *     does for `req_len != 1`);
 *   - minor 15: the §2.2 grant algorithm, BIG_FRAME size enforcement,
 *     BATCH validation + stop-at-first-error execution, READ2 framing and
 *     the ATTN edge (raised only once a fresh reply is staged).
 *
 * Literal wire bytes below come from the §9 vectors of the protocol-0.15
 * spec, generated with a CRC-16/CCITT-FALSE script (poly 0x1021, init
 * 0xFFFF, low byte first on the wire) -- none were computed by hand.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/chips/gd32g553.h"
#include "alp/protocol/crc16.h"

#define SOF 0xA5u

/* ---- simulated bridge ---------------------------------------------------- */

static struct {
	/* configuration */
	uint8_t minor;
	bool    ext_inval;        /* answer the 6-byte form with INVAL even at minor 15 */
	bool    stub;             /* stub backend: supported 0x13 (no ATTN, no STREAM2) */
	bool    debugger;         /* DHCSR.C_DEBUGEN set: refuse ATTN */
	bool    attn_signal;      /* false = the edge is "lost" (or never wired) */
	bool    level_forced;     /* ATTN reads stuck high */
	bool    leak_beyond_read; /* RX buffer already holds bytes past the clocked length */
	/* link state, as the firmware would hold it */
	bool     seq;
	uint8_t  counter;
	uint32_t granted;
	uint16_t mp;
	bool     attn_on;
	bool     level;
	/* streams */
	bool     stream_active[2];
	uint32_t r2_first, r2_dropped;
	uint8_t  r2_got;
	uint16_t r2_codes[121];
	int      r2_force_got; /* >= 0: put this raw `got` byte on the wire */
	/* BATCH: when raw_len > 0 the next BATCH answers OK with this payload */
	uint8_t batch_raw[260];
	size_t  batch_raw_len;
	/* a literal reply to serve verbatim for the next decoded request */
	uint8_t raw[260];
	size_t  raw_len;
	/* reply staging */
	uint8_t staged[260];
	size_t  staged_len;
	/* observation */
	uint32_t cmd_count[256];
	uint32_t ext_count;    /* 6-byte LINK_FEATURES requests */
	uint32_t legacy_count; /* 1-byte LINK_FEATURES requests */
	uint8_t  ext_req[16];
	size_t   ext_req_len;
	uint8_t  ext_rep[16];
	size_t   ext_rep_len;
	uint8_t  last_req[260];
	size_t   last_req_len;
	uint32_t nreq;
	size_t   max_write, max_read;
	uint64_t delay_us;
	uint8_t  begin2_req[12];
} E;

/* The ATTN hook the test hands the driver (interrupt + semaphore in a real
 * backend; a latch here). */
static struct {
	bool     latch;
	uint32_t arms, waits;
} H;

static uint64_t g_now_ms;

static void hook_arm(void *u)
{
	(void)u;
	H.latch = false;
	H.arms++;
}

static alp_status_t hook_wait(void *u, uint32_t timeout_ms)
{
	(void)u;
	(void)timeout_ms;
	H.waits++;
	if (H.latch) {
		H.latch = false;
		return ALP_OK;
	}
	g_now_ms += timeout_ms;
	return ALP_ERR_TIMEOUT;
}

static alp_status_t hook_level(void *u, bool *high)
{
	(void)u;
	*high = E.level || E.level_forced;
	return ALP_OK;
}

static const gd32g553_attn_hook_t g_hook = {
	.arm        = hook_arm,
	.wait       = hook_wait,
	.read_level = hook_level,
};

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void stage(uint8_t code, const uint8_t *payload, size_t n)
{
	if (E.raw_len > 0u) { /* serve a literal vector verbatim, once */
		memcpy(E.staged, E.raw, E.raw_len);
		E.staged_len = E.raw_len;
		E.raw_len    = 0u;
		return;
	}
	uint8_t stamp = 0u;
	if (E.seq) {
		E.counter = (uint8_t)((E.counter + 1u) & 0x0Fu);
		stamp     = E.counter;
	}
	E.staged[0] = SOF;
	E.staged[1] = (uint8_t)((stamp << 4) | code);
	if (n > 0u) memcpy(&E.staged[2], payload, n);
	const uint16_t crc   = alp_crc16_ccitt_false(E.staged, 2u + n);
	E.staged[2u + n]     = (uint8_t)(crc & 0xFFu);
	E.staged[2u + n + 1] = (uint8_t)(crc >> 8);
	E.staged_len         = 2u + n + 2u;
	/* ATTN rises only once the fresh reply is armed. */
	if (E.attn_on && E.attn_signal) {
		E.level = true;
		H.latch = true;
	}
}

static void fail(uint8_t code)
{
	stage(code, NULL, 0u);
}

static void emu_reboot(void)
{
	E.seq     = false;
	E.counter = 0u;
	E.granted = 0u;
	E.mp      = 65u;
	E.attn_on = false;
	E.level   = false;
	memset(E.stream_active, 0, sizeof(E.stream_active));
}

static void emu_reset(uint8_t minor)
{
	memset(&E, 0, sizeof(E));
	memset(&H, 0, sizeof(H));
	E.minor        = minor;
	E.attn_signal  = true;
	E.r2_force_got = -1;
	E.mp           = 65u;
}

/* READ2 fixed header + codes into `out`; returns the payload length. */
static size_t read2_payload(uint8_t *out, uint8_t max_samples)
{
	uint8_t got = E.r2_got;
	if (got > max_samples) got = max_samples;
	le32(&out[0], E.r2_first);
	le32(&out[4], E.r2_dropped);
	out[8]         = (E.r2_force_got >= 0) ? (uint8_t)E.r2_force_got : got;
	const size_t n = (E.r2_force_got >= 0) ? (size_t)E.r2_force_got : got;
	for (size_t i = 0; i < n; i++) {
		out[9 + 2 * i]     = (uint8_t)E.r2_codes[i];
		out[9 + 2 * i + 1] = (uint8_t)(E.r2_codes[i] >> 8);
	}
	return 9u + 2u * n;
}

/* Allow-list: op, fixed request length, max reply (READ2 computed). */
static bool batch_op_info(uint8_t op, uint8_t *req_len, uint16_t *rmax, bool *variable)
{
	static const struct {
		uint8_t op, rl, rm;
	} t[] = { { 0x00, 0, 0 }, { 0x10, 4, 4 }, { 0x11, 8, 0 }, { 0x20, 10, 0 }, { 0x21, 1, 8 },
		      { 0x24, 1, 8 }, { 0x3C, 2, 0 }, { 0x40, 0, 1 }, { 0x50, 4, 0 },  { 0x51, 1, 2 },
		      { 0x60, 1, 4 }, { 0x61, 1, 0 }, { 0x70, 1, 4 }, { 0x90, 12, 4 } };
	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		if (t[i].op == op) {
			*req_len  = t[i].rl;
			*rmax     = t[i].rm;
			*variable = (op == 0x3C);
			return true;
		}
	}
	return false;
}

static void emu_batch(const uint8_t *pl, size_t n)
{
	if (!(E.granted & GD32G553_LINK_FEAT_BATCH)) {
		fail(0x06);
		return;
	}
	if (n < 1u || pl[0] == 0u) {
		fail(0x01);
		return;
	}
	if (pl[0] > 16u) {
		fail(0x08);
		return;
	}
	const uint8_t count = pl[0];
	size_t        pos = 1u, worst = 1u;
	uint8_t       args_at[16];
	uint16_t      rm[16];
	bool          var[16];
	for (unsigned i = 0; i < count; i++) {
		if (pos + 2u > n) {
			fail(0x01);
			return;
		}
		uint8_t  req_len;
		uint16_t rmax;
		bool     variable;
		if (!batch_op_info(pl[pos], &req_len, &rmax, &variable) || pl[pos + 1] != req_len) {
			{
				fail(0x01);
				return;
			}
		}
		args_at[i] = (uint8_t)(pos + 2u);
		if (variable) rmax = (uint16_t)(9u + 2u * pl[pos + 3]);
		rm[i]  = rmax;
		var[i] = variable;
		worst += 2u + rmax;
		pos += 2u + req_len;
	}
	if (pos != n) {
		fail(0x01);
		return;
	} /* trailing byte */
	if (worst > E.mp) {
		fail(0x08);
		return;
	}

	if (E.batch_raw_len > 0u) {
		stage(0x00, E.batch_raw, E.batch_raw_len);
		E.batch_raw_len = 0u;
		return;
	}

	uint8_t out[260];
	size_t  w        = 1u;
	uint8_t executed = 0u;
	for (unsigned i = 0; i < count; i++) {
		executed++;
		uint8_t st  = 0u;
		size_t  len = rm[i];
		if (var[i]) {
			const uint8_t sid = pl[args_at[i]];
			if (sid > 1u || !E.stream_active[sid]) {
				st = 0x01u;
			} else {
				uint8_t tmp[260];
				len        = read2_payload(tmp, pl[args_at[i] + 1]);
				out[w]     = 0u;
				out[w + 1] = (uint8_t)len;
				memcpy(&out[w + 2], tmp, len);
				w += 2u + len;
				continue;
			}
		}
		if (st != 0u) {
			out[w++] = st;
			out[w++] = 0u;
			break; /* stop at the first non-OK sub-status */
		}
		out[w++] = 0u;
		out[w++] = (uint8_t)len;
		memset(&out[w], 0, len);
		w += len;
	}
	out[0] = executed;
	stage(0x00, out, w);
}

static void emu_link_features(const uint8_t *pl, size_t n)
{
	if (n == 1u) { /* legacy form: the whole word becomes features & 1 */
		E.legacy_count++;
		E.seq           = (pl[0] & 1u) != 0u;
		E.granted       = E.seq ? 1u : 0u;
		E.mp            = 65u;
		E.attn_on       = false;
		E.level         = false;
		const uint8_t g = (uint8_t)E.granted;
		stage(0x00, &g, 1u);
		return;
	}
	if (n != 6u || E.minor < 15u || E.ext_inval) {
		fail(0x01);
		return;
	}

	E.ext_count++;
	const uint32_t want      = rd32(pl);
	const uint16_t mp_req    = (uint16_t)(pl[4] | (pl[5] << 8));
	const uint32_t supported = E.stub ? 0x13u : 0x1Fu;
	uint32_t       g         = want & supported;
	if (!(g & 1u) || E.debugger) g &= ~GD32G553_LINK_FEAT_ATTN;
	uint16_t mp = 65u;
	if (g & GD32G553_LINK_FEAT_BIG_FRAME) {
		mp = (mp_req < 65u) ? 65u : (mp_req > 252u) ? 252u : mp_req;
		if (mp == 65u) g &= ~GD32G553_LINK_FEAT_BIG_FRAME;
	}
	/* arm before staging the reply */
	E.granted = g;
	E.mp      = mp;
	E.seq     = (g & 1u) != 0u;
	E.attn_on = (g & GD32G553_LINK_FEAT_ATTN) != 0u;
	if (!E.attn_on) E.level = false;
	uint8_t out[10];
	le32(&out[0], g);
	le32(&out[4], supported);
	out[8] = (uint8_t)mp;
	out[9] = (uint8_t)(mp >> 8);
	stage(0x00, out, sizeof(out));
	memcpy(E.ext_rep, E.staged, E.staged_len);
	E.ext_rep_len = E.staged_len;
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)
{
	(void)bus;
	zassert_true(len >= 4u && len <= 256u, "request frame %u B outside 4..256", (unsigned)len);
	if (len > E.max_write) E.max_write = len;
	E.level = false; /* CS fall drives ATTN low */
	if (tx[0] != SOF) return ALP_ERR_IO;
	const uint16_t crc = alp_crc16_ccitt_false(tx, len - 2u);
	if ((uint16_t)(tx[len - 2u] | (tx[len - 1u] << 8)) != crc) return ALP_OK; /* dropped */

	E.nreq++;
	memcpy(E.last_req, tx, len);
	E.last_req_len = len;

	const uint8_t  cmd = tx[1];
	const uint8_t *pl  = &tx[2];
	const size_t   n   = len - 4u;
	E.cmd_count[cmd]++;
	if (cmd == GD32G553_CMD_LINK_FEATURES && n == 6u) {
		memcpy(E.ext_req, tx, len);
		E.ext_req_len = len;
	}

	/* BIG_FRAME enforcement: only BATCH may exceed 65 B, up to mp. */
	if (n > 65u && cmd != GD32G553_CMD_BATCH) {
		fail(0x01);
		return ALP_OK;
	}
	if (cmd == GD32G553_CMD_BATCH && n > E.mp) {
		fail(0x01);
		return ALP_OK;
	}

	switch (cmd) {
	case GD32G553_CMD_GET_VERSION: {
		const uint8_t v[3] = { 0u, E.minor, 0u };
		stage(0x00u, v, sizeof(v));
		break;
	}
	case GD32G553_CMD_LINK_FEATURES:
		emu_link_features(pl, n);
		break;
	case GD32G553_CMD_BATCH:
		if (E.minor < 15u) {
			fail(0x06);
		} else {
			emu_batch(pl, n);
		}
		break;
	case GD32G553_CMD_ADC_STREAM_BEGIN2: {
		if (E.minor < 15u || !(E.granted & GD32G553_LINK_FEAT_ADC_STREAM2)) {
			fail(0x06);
			break;
		}
		if (n != 12u || pl[0] > 1u) {
			fail(0x01);
			break;
		}
		memcpy(E.begin2_req, pl, 12);
		E.stream_active[pl[0]] = true;
		/* 1 kHz, watermark 256, as in the spec's reply vector. */
		const uint8_t r[17] = { 0x40, 0x42, 0x0F, 0x00, 0xE8, 0x03, 0x00, 0x00, 0xFF,
			                    0x0F, 0x08, 0x07, 0x01, 0x00, 0x01, 0x00, 0x02 };
		stage(0x00, r, sizeof(r));
		break;
	}
	case GD32G553_CMD_ADC_STREAM_READ2: {
		if (E.minor < 15u || !(E.granted & GD32G553_LINK_FEAT_ADC_STREAM2)) {
			fail(0x06);
			break;
		}
		if (n != 2u || pl[0] > 1u || pl[1] == 0u) {
			fail(0x01);
			break;
		}
		if (pl[1] > (E.mp - 9u) / 2u) {
			fail(0x08);
			break;
		}
		if (!E.stream_active[pl[0]]) {
			fail(0x01);
			break;
		}
		uint8_t out[260];
		stage(0x00, out, read2_payload(out, pl[1]));
		break;
	}
	case GD32G553_CMD_ADC_STREAM_END:
		E.stream_active[pl[0] & 1u] = false;
		stage(0x00, NULL, 0u);
		break;
	default: /* PING, GPIO_WRITE, OTA_COMMIT, ...: empty OK reply */
		stage(0x00u, NULL, 0u);
		break;
	}
	return ALP_OK;
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)
{
	(void)bus;
	zassert_true(len <= 256u, "reply read %u B exceeds one 256 B frame", (unsigned)len);
	if (len > E.max_read) E.max_read = len;
	E.level = false; /* CS fall (a drain with no events) drops ATTN */
	memset(rx, 0, len);
	if (E.staged_len > 0u) memcpy(rx, E.staged, MIN(len, E.staged_len));
	/* Bytes past the clocked length that happen to hold the rest of the
	 * staged frame: stale RX-buffer content a parser must never trust. */
	if (E.leak_beyond_read && E.staged_len > len) {
		memcpy(rx + len, E.staged + len, MIN(E.staged_len, 256u) - len);
	}
	return ALP_OK; /* the reply is re-served until the next decode */
}

alp_status_t
alp_i2c_write_read(alp_i2c_t *b, uint8_t a, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
	(void)b;
	(void)a;
	(void)w;
	(void)wl;
	(void)r;
	(void)rl;
	return ALP_ERR_NOSUPPORT;
}

void alp_delay_us(uint32_t us)
{
	E.delay_us += us;
}

void alp_delay_ms(uint32_t ms)
{
	g_now_ms += ms;
}

uint64_t alp_uptime_ms(void)
{
	return g_now_ms += 1u;
}

/* ---- fixture ------------------------------------------------------------- */

static gd32g553_t C;
static int        g_spi_token;

static alp_status_t bring_up(bool with_attn)
{
	memset(&C, 0, sizeof(C));
	return gd32g553_init_ex(&C, (alp_spi_t *)&g_spi_token, NULL, 0x70u, with_attn ? &g_hook : NULL);
}

/* A context standing on a link whose negotiation state the test sets by
 * hand, so the literal §9 vectors (un-stamped) can be served verbatim. */
static void manual_link(uint32_t granted, uint16_t mp)
{
	memset(&C, 0, sizeof(C));
	C.initialised = true;
	C.spi         = (alp_spi_t *)&g_spi_token;
	C.granted     = granted;
	C.max_payload = mp;
	E.granted     = granted;
	E.mp          = mp;
}

static void each_before(void *unused)
{
	(void)unused;
	emu_reset(15u);
	g_now_ms = 0u;
}

static void each_after(void *unused)
{
	(void)unused;
	/* The host must never clock more than one 256 B frame per CS window,
	 * either direction (§3.3).  alp_spi_write/read assert it per call. */
	zassert_true(E.max_write <= 256u && E.max_read <= 256u);
}

ZTEST_SUITE(gd32_protocol_015, NULL, NULL, each_before, each_after, NULL);

/* ---- §9 vectors (CRC-16/CCITT-FALSE generated by script) ------------------- */

static const uint8_t V_LF_EXT_REQ_ALL[]       = { 0xA5, 0x81, 0x1F, 0x00, 0x00,
	                                              0x00, 0xFC, 0x00, 0xDA, 0x65 };
static const uint8_t V_LF_EXT_REP_ALL_SEQ1[]  = { 0xA5, 0x10, 0x1F, 0x00, 0x00, 0x00, 0x1F,
	                                              0x00, 0x00, 0x00, 0xFC, 0x00, 0x68, 0x50 };
static const uint8_t V_LF_EXT_REP_STUB_SEQ1[] = { 0xA5, 0x10, 0x13, 0x00, 0x00, 0x00, 0x13,
	                                              0x00, 0x00, 0x00, 0xFC, 0x00, 0xF5, 0xBC };
static const uint8_t V_BEGIN2_REQ[]           = { 0xA5, 0x3B, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x03,
	                                              0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x6D, 0x9B };
static const uint8_t V_BEGIN2_REP[]           = { 0xA5, 0x00, 0x40, 0x42, 0x0F, 0x00, 0xE8,
	                                              0x03, 0x00, 0x00, 0xFF, 0x0F, 0x08, 0x07,
	                                              0x01, 0x00, 0x01, 0x00, 0x02, 0x68, 0x43 };
static const uint8_t V_READ2_REQ_MAX121[]     = { 0xA5, 0x3C, 0x00, 0x79, 0x89, 0x8D };
static const uint8_t V_READ2_REP_GOT3[]       = { 0xA5, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	                                              0x00, 0x00, 0x00, 0x03, 0x00, 0x08, 0x01,
	                                              0x08, 0xFF, 0x0F, 0x47, 0xD4 };
static const uint8_t V_READ2_REP_EMPTY[]      = { 0xA5, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
	                                              0x00, 0x00, 0x00, 0x00, 0x46, 0x14 };
static const uint8_t V_READ2_REP_OVERRUN[] = { 0xA5, 0x00, 0x23, 0x01, 0x00, 0x00, 0x20, 0x00, 0x00,
	                                           0x00, 0x02, 0x00, 0x08, 0x01, 0x08, 0xD4, 0xAB };
static const uint8_t V_READ2_REP_DISCONT[] = { 0xA5, 0x00, 0x25, 0x01, 0x00, 0x00, 0xFF,
	                                           0xFF, 0xFF, 0xFF, 0x00, 0xA2, 0x34 };
static const uint8_t V_BATCH_REQ[]         = { 0xA5, 0x04, 0x03, 0x11, 0x08, 0x01, 0x00, 0x00,
	                                           0x00, 0x01, 0x00, 0x00, 0x00, 0x21, 0x01, 0x00,
	                                           0x3C, 0x02, 0x00, 0x10, 0x99, 0x04 };
static const uint8_t V_BATCH_REP_STOP[] = { 0xA5, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0xD4, 0x3A };

static void serve(const uint8_t *v, size_t n)
{
	memcpy(E.raw, v, n);
	E.raw_len = n;
}

/* The stamped vectors fall out of the model byte for byte: a divergence
 * means the driver sent a request the §9 vector does not describe. */
ZTEST(gd32_protocol_015, test_negotiation_6byte_form_with_attn_matches_vectors)
{
	E.attn_signal = true;
	zassert_equal(bring_up(true), ALP_OK);

	zassert_equal(E.ext_count, 1u, "exactly one 6-byte negotiation");
	zassert_equal(E.legacy_count, 0u, "no 1-byte form to a v0.15 peer");
	zassert_equal(E.ext_req_len, sizeof(V_LF_EXT_REQ_ALL));
	zassert_mem_equal(E.ext_req, V_LF_EXT_REQ_ALL, sizeof(V_LF_EXT_REQ_ALL));
	zassert_equal(E.ext_rep_len, sizeof(V_LF_EXT_REP_ALL_SEQ1));
	zassert_mem_equal(E.ext_rep, V_LF_EXT_REP_ALL_SEQ1, sizeof(V_LF_EXT_REP_ALL_SEQ1));

	zassert_equal(C.granted, 0x1Fu);
	zassert_equal(C.supported, 0x1Fu);
	zassert_equal(C.max_payload, 252u);
	zassert_true(C.seq_enabled);
	zassert_equal(C.seq_last, 1u, "baseline is the negotiation reply's stamp");
	zassert_true(C.attn_active, "grant reply arrived on an edge: self-test passed");
	zassert_false(C.attn_unusable);
}

ZTEST(gd32_protocol_015, test_negotiation_stub_backend_vector)
{
	E.stub = true;
	zassert_equal(bring_up(true), ALP_OK);
	zassert_mem_equal(E.ext_rep, V_LF_EXT_REP_STUB_SEQ1, sizeof(V_LF_EXT_REP_STUB_SEQ1));
	zassert_equal(C.granted, 0x13u);
	zassert_equal(C.supported, 0x13u);
	zassert_false(C.attn_active, "stub backend has no ATTN");
	zassert_false(C.attn_unusable, "a refused ATTN is not a failed self-test");
}

ZTEST(gd32_protocol_015, test_negotiation_without_attn_hook_never_requests_attn)
{
	zassert_equal(bring_up(false), ALP_OK);
	zassert_equal(E.ext_count, 1u);
	/* want = STATUS_SEQ|BIG_FRAME|ADC_STREAM2|BATCH = 0x1B, mp_req 252 */
	zassert_equal(E.ext_req[2], 0x1Bu);
	zassert_equal(E.ext_req[6], 0xFCu);
	zassert_equal(C.granted, 0x1Bu);
	zassert_false(C.attn_active);
	zassert_equal(H.arms, 0u, "no hook, no ATTN traffic");
}

ZTEST(gd32_protocol_015, test_attn_refused_while_debugger_attached)
{
	E.debugger = true;
	zassert_equal(bring_up(true), ALP_OK);
	zassert_equal(C.granted, 0x1Bu, "ATTN dropped from the grant, rest kept");
	zassert_equal(C.supported, 0x1Fu, "supported still lists ATTN: it was refused, not absent");
	zassert_false(C.attn_active);
}

ZTEST(gd32_protocol_015, test_fallback_against_v014_firmware_is_byte_identical)
{
	emu_reset(14u);
	zassert_equal(bring_up(true), ALP_OK);

	zassert_equal(E.ext_count, 0u, "the 6-byte form is never sent below minor 15");
	zassert_equal(E.legacy_count, 1u);
	zassert_equal(E.last_req_len, 5u, "1-byte form: SOF CMD features CRC(2)");
	zassert_equal(E.last_req[1], GD32G553_CMD_LINK_FEATURES);
	zassert_equal(E.last_req[2], GD32G553_LINK_FEAT_STATUS_SEQ);
}

ZTEST(gd32_protocol_015, test_fallback_v014_state_and_gated_apis)
{
	emu_reset(14u);
	zassert_equal(bring_up(true), ALP_OK);
	zassert_equal(C.granted, GD32G553_LINK_FEAT_STATUS_SEQ);
	zassert_equal(C.max_payload, 65u);
	zassert_true(C.seq_enabled);
	zassert_false(C.attn_active);

	const uint32_t              before = E.nreq;
	gd32g553_adc_stream2_info_t info;
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 1000u, 256u, &info), ALP_ERR_NOSUPPORT);
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[4];
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 4u, &first, &dropped, &got, codes),
	              ALP_ERR_NOSUPPORT);
	gd32g553_batch_op_t op = { .op = GD32G553_CMD_PING };
	zassert_equal(gd32g553_batch(&C, &op, 1u, NULL), ALP_ERR_NOSUPPORT);
	zassert_equal(E.nreq, before, "gated APIs must not touch the wire");
}

/* A peer that reports minor 15 yet answers the 6-byte form with INVAL (or a
 * pre-0.7 NOSUPPORT) lands on the legacy form (§8 step 3b). */
ZTEST(gd32_protocol_015, test_fallback_when_ext_form_answers_inval)
{
	E.ext_inval = true;
	zassert_equal(bring_up(true), ALP_OK);
	zassert_equal(E.cmd_count[GD32G553_CMD_LINK_FEATURES], 2u, "6-byte attempt, then 1-byte");
	zassert_equal(E.legacy_count, 1u);
	zassert_equal(C.granted, GD32G553_LINK_FEAT_STATUS_SEQ);
	zassert_equal(C.max_payload, 65u);
	zassert_false(C.attn_active);
	zassert_true(C.seq_enabled);
	zassert_equal(gd32g553_ping(&C), ALP_OK, "link is usable on the legacy framing");
}

/* ---- ATTN ------------------------------------------------------------------ */

ZTEST(gd32_protocol_015, test_attn_replaces_staging_gap)
{
	zassert_equal(bring_up(true), ALP_OK);
	zassert_true(C.attn_active);

	const uint64_t before_us = E.delay_us;
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_equal(E.delay_us, before_us, "reply on an edge: no staging gap, no ladder");
	zassert_true(H.waits >= 2u);

	/* Control: the same ping on a link without ATTN pays the 35 us gap. */
	zassert_equal(bring_up(false), ALP_OK);
	const uint64_t b2 = E.delay_us;
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_true(E.delay_us - b2 >= 35u, "no ATTN: 0.14 staging gap");
}

ZTEST(gd32_protocol_015, test_attn_selftest_failure_withdraws_attn)
{
	E.attn_signal = false; /* grant reply never rises */
	zassert_equal(bring_up(true), ALP_OK);
	zassert_equal(E.ext_count, 2u, "re-sent the extended form with ATTN cleared");
	zassert_equal(E.ext_req[2] & 0x04u, 0u, "second request has ATTN cleared");
	zassert_false(E.attn_on, "slave returned PA14 to SWCLK");
	zassert_false(C.attn_active);
	zassert_true(C.attn_unusable, "not asked again until the next init");
	zassert_equal(C.granted, 0x1Bu);
	zassert_equal(gd32g553_ping(&C), ALP_OK);
}

ZTEST(gd32_protocol_015, test_attn_lost_edge_falls_back_then_disables_after_three)
{
	zassert_equal(bring_up(true), ALP_OK);
	zassert_true(C.attn_active);
	E.attn_signal = false; /* edges stop; replies are still staged */

	const uint64_t before_us = E.delay_us;
	zassert_equal(gd32g553_ping(&C), ALP_OK, "lost edge: 0.14 drain rule still delivers");
	zassert_true(E.delay_us - before_us >= 35u, "fell back to the staging gap");
	zassert_true(C.attn_active, "one lost edge is not a fault");
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_true(C.attn_active, "two are not either");
	zassert_equal(E.ext_count, 1u);

	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_false(C.attn_active, "third consecutive lost edge");
	zassert_true(C.attn_unusable);
	zassert_equal(E.ext_count, 2u, "renegotiated without ATTN");
	zassert_equal(E.ext_req[2] & 0x04u, 0u);
	zassert_false(E.attn_on);
	zassert_equal(C.granted & GD32G553_LINK_FEAT_ATTN, 0u);
	zassert_equal(C.attn_timeouts, 3u);

	const uint32_t waits = H.waits;
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_equal(H.waits, waits, "no more edge waits once disabled");
}

ZTEST(gd32_protocol_015, test_attn_edge_resets_the_lost_edge_run)
{
	zassert_equal(bring_up(true), ALP_OK);
	for (int round = 0; round < 3; round++) {
		E.attn_signal = false;
		zassert_equal(gd32g553_ping(&C), ALP_OK);
		zassert_equal(gd32g553_ping(&C), ALP_OK);
		E.attn_signal = true;
		zassert_equal(gd32g553_ping(&C), ALP_OK); /* an edge: run restarts */
	}
	zassert_true(C.attn_active, "non-consecutive losses never reach the limit");
	zassert_equal(E.ext_count, 1u);
}

ZTEST(gd32_protocol_015, test_attn_stuck_high_disables_after_three)
{
	zassert_equal(bring_up(true), ALP_OK);
	E.level_forced = true; /* line reads high with no stream events armed */
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_true(C.attn_active);
	zassert_equal(gd32g553_ping(&C), ALP_OK);
	zassert_false(C.attn_active, "third consecutive stuck-high reading");
	zassert_equal(E.ext_count, 2u);
	zassert_equal(E.ext_req[2] & 0x04u, 0u);
	zassert_equal(C.attn_stuck, 3u);
}

ZTEST(gd32_protocol_015, test_attn_idle_edges_with_empty_read2_disable_after_eight)
{
	zassert_equal(bring_up(true), ALP_OK);
	gd32g553_adc_stream2_info_t info;
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 1000u, 256u, &info), ALP_OK);
	E.r2_got     = 0u;
	E.r2_dropped = 0u;

	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[8];
	for (int i = 0; i < 7; i++) {
		H.latch = true; /* an idle edge */
		zassert_equal(gd32g553_attn_wait_event(&C, 10u), ALP_OK);
		zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
		zassert_equal(got, 0u);
		E.r2_first = first;
	}
	zassert_true(C.attn_active, "seven empty idle edges are tolerated");
	H.latch = true;
	zassert_equal(gd32g553_attn_wait_event(&C, 10u), ALP_OK);
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	zassert_false(C.attn_active, "eighth consecutive empty idle edge");
	zassert_equal(E.ext_req[2] & 0x04u, 0u, "renegotiated without ATTN");
	zassert_equal(gd32g553_attn_wait_event(&C, 10u), ALP_ERR_NOSUPPORT);
}

ZTEST(gd32_protocol_015, test_attn_wait_event_needs_active_attn)
{
	zassert_equal(bring_up(false), ALP_OK);
	zassert_equal(gd32g553_attn_wait_event(&C, 10u), ALP_ERR_NOSUPPORT);
	zassert_equal(gd32g553_attn_wait_event(NULL, 10u), ALP_ERR_INVAL);
}

/* ---- BEGIN2 / READ2 ---------------------------------------------------------- */

ZTEST(gd32_protocol_015, test_begin2_request_and_reply_match_vectors)
{
	manual_link(0x1Fu, 252u);
	serve(V_BEGIN2_REP, sizeof(V_BEGIN2_REP));

	gd32g553_adc_stream2_info_t info;
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 1000u, 256u, &info), ALP_OK);
	zassert_equal(E.last_req_len, sizeof(V_BEGIN2_REQ));
	zassert_mem_equal(E.last_req, V_BEGIN2_REQ, sizeof(V_BEGIN2_REQ));

	zassert_equal(info.tick_hz, 1000000u);
	zassert_equal(info.period_ticks, 1000u);
	zassert_equal(info.full_scale, 4095u);
	zassert_equal(info.vref_mv, 1800u);
	zassert_equal(info.flags, GD32G553_STREAM2_FLAG_VREF_MEASURED);
	zassert_equal(info.watermark, 256u);
	zassert_equal(info.ring_depth, 512u);
}

ZTEST(gd32_protocol_015, test_begin2_local_argument_checks)
{
	manual_link(0x1Fu, 252u);
	const uint32_t              n0 = E.nreq;
	gd32g553_adc_stream2_info_t info;
	zassert_equal(gd32g553_adc_stream_begin2(&C, 2u, 0u, 1000u, 0u, &info), ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 0u, 0u, &info), ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 100001u, 0u, &info), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 8u, 1000u, 0u, &info), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 1000u, 100u, &info), ALP_ERR_INVAL);
	zassert_equal(E.nreq, n0, "rejected locally: no bus traffic");
	zassert_equal(gd32g553_adc_stream_begin2(NULL, 0u, 0u, 1000u, 0u, &info), ALP_ERR_NOT_READY);
}

static void begin_stream0(void)
{
	gd32g553_adc_stream2_info_t info;
	zassert_equal(gd32g553_adc_stream_begin2(&C, 0u, 0u, 1000u, 256u, &info), ALP_OK);
}

ZTEST(gd32_protocol_015, test_read2_literal_vectors_parse)
{
	manual_link(0x1Fu, 252u);
	E.stream_active[0] = true;
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[121];

	serve(V_READ2_REP_GOT3, sizeof(V_READ2_REP_GOT3));
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes), ALP_OK);
	zassert_mem_equal(E.last_req, V_READ2_REQ_MAX121, sizeof(V_READ2_REQ_MAX121));
	zassert_equal(first, 0x100u);
	zassert_equal(dropped, 0u);
	zassert_equal(got, 3u);
	zassert_equal(codes[0], 0x0800u);
	zassert_equal(codes[1], 0x0801u);
	zassert_equal(codes[2], 0x0FFFu);

	serve(V_READ2_REP_EMPTY, sizeof(V_READ2_REP_EMPTY));
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(first, 0x103u);
	zassert_equal(got, 0u, "CRC sits at offset 11 for got = 0");

	serve(V_READ2_REP_OVERRUN, sizeof(V_READ2_REP_OVERRUN));
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(first, 0x123u);
	zassert_equal(dropped, 0x20u, "overrun answers OK with data and a drop count");
	zassert_equal(got, 2u);
	zassert_equal(codes[1], 0x0801u);

	serve(V_READ2_REP_DISCONT, sizeof(V_READ2_REP_DISCONT));
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(dropped, GD32G553_READ2_DROPPED_UNKNOWN);
	zassert_equal(got, 0u);
	zassert_equal(first, 0x125u);
}

ZTEST(gd32_protocol_015, test_read2_variable_length_with_ladder_wait)
{
	/* Same parse through the model's own framing, with the CRC at 11 + 2*got
	 * for several `got`, and bytes after the CRC left as TX-underrun filler. */
	zassert_equal(bring_up(true), ALP_OK);
	begin_stream0();
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[121];
	uint32_t next = 0u;
	for (uint8_t want = 0u; want <= 121u; want = (uint8_t)(want < 4 ? want + 1 : 121)) {
		E.r2_got   = want;
		E.r2_first = next;
		for (unsigned i = 0; i < want; i++)
			E.r2_codes[i] = (uint16_t)(0x100u + i);
		zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes),
		              ALP_OK);
		zassert_equal(got, want);
		zassert_equal(first, next);
		for (unsigned i = 0; i < want; i++)
			zassert_equal(codes[i], 0x100u + i);
		next += want;
		if (want == 121u) break;
	}
	zassert_equal(C.stream2_gaps, 0u, "contiguous reads never trip the accounting invariant");
}

ZTEST(gd32_protocol_015, test_read2_got_above_max_samples_is_io)
{
	zassert_equal(bring_up(false), ALP_OK);
	begin_stream0();
	E.r2_force_got = 10; /* firmware claims 10 codes against max_samples 4 */
	/* The claimed frame's CRC sits beyond what the host clocked, in bytes
	 * that happen to be valid in the RX buffer: only the got > max_samples
	 * check keeps the parser from trusting them. */
	E.leak_beyond_read = true;
	E.r2_got           = 10;
	uint32_t first, dropped;
	uint8_t  got = 0xEE;
	uint16_t codes[4];
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 4u, &first, &dropped, &got, codes), ALP_ERR_IO);
	zassert_equal(got, 0u, "no samples are reported on an IO");
}

ZTEST(gd32_protocol_015, test_read2_accounting_invariant_counts_gaps)
{
	zassert_equal(bring_up(false), ALP_OK);
	begin_stream0();
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[8];

	E.r2_got     = 3;
	E.r2_first   = 0;
	E.r2_dropped = 0;
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	E.r2_got     = 2;
	E.r2_first   = 3 + 5;
	E.r2_dropped = 5; /* 5 dropped, accounted for */
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(C.stream2_gaps, 0u);
	/* sentinel: got = 0 and first_index stays at the previous D (3 + 5 + 2 = 10) */
	E.r2_got     = 0;
	E.r2_first   = 10;
	E.r2_dropped = 0xFFFFFFFFu;
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(C.stream2_gaps, 0u, "a sentinel at the expected index is not a gap");
	E.r2_got     = 0;
	E.r2_first   = 99;
	E.r2_dropped = 0xFFFFFFFu; /* skipped 89 unaccounted */
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(C.stream2_gaps, 1u, "first_index jumped without a matching dropped");
}

ZTEST(gd32_protocol_015, test_read2_u32_wrap_is_not_a_gap)
{
	zassert_equal(bring_up(false), ALP_OK);
	begin_stream0();
	C.stream2_next[0] = 0xFFFFFFFEu;
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[8];
	E.r2_got     = 4;
	E.r2_first   = 0xFFFFFFFEu;
	E.r2_dropped = 0;
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	E.r2_got     = 1;
	E.r2_first   = 2u;
	E.r2_dropped = 0; /* 0xFFFFFFFE + 4 wraps to 2 */
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 8u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(C.stream2_gaps, 0u);
}

ZTEST(gd32_protocol_015, test_read2_sample_ceiling_follows_the_link)
{
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[121];

	zassert_equal(bring_up(false), ALP_OK); /* BIG_FRAME granted: mp 252 */
	begin_stream0();
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 121u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 122u, &first, &dropped, &got, codes),
	              ALP_ERR_OUT_OF_RANGE);
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 0u, &first, &dropped, &got, codes),
	              ALP_ERR_INVAL);

	/* A link that kept 65 B (BIG_FRAME not granted) reads at most 28. */
	manual_link(GD32G553_LINK_FEAT_STATUS_SEQ | GD32G553_LINK_FEAT_ADC_STREAM2, 65u);
	E.stream_active[0] = true;
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 28u, &first, &dropped, &got, codes), ALP_OK);
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 29u, &first, &dropped, &got, codes),
	              ALP_ERR_OUT_OF_RANGE);
}

ZTEST(gd32_protocol_015, test_read2_firmware_status_codes_pass_through)
{
	zassert_equal(bring_up(false), ALP_OK);
	uint32_t first, dropped;
	uint8_t  got;
	uint16_t codes[4];
	/* stream 0 never started: the firmware answers INVAL (4-byte envelope) */
	zassert_equal(gd32g553_adc_stream_read2(&C, 0u, 4u, &first, &dropped, &got, codes),
	              ALP_ERR_INVAL);
}

/* ---- BATCH --------------------------------------------------------------------- */

ZTEST(gd32_protocol_015, test_batch_request_and_stop_at_first_error_match_vectors)
{
	manual_link(0x1Fu, 252u);
	const uint8_t       gpio_args[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
	const uint8_t       pwm_args[1]  = { 0 };
	const uint8_t       r2_args[2]   = { 0, 16 };
	uint8_t             r_pwm[8], r_r2[9 + 32];
	gd32g553_batch_op_t ops[3] = {
		{ .op = 0x11, .args = gpio_args, .args_len = 8 },
		{ .op = 0x21, .args = pwm_args, .args_len = 1, .reply = r_pwm, .reply_cap = sizeof(r_pwm) },
		{ .op = 0x3C, .args = r2_args, .args_len = 2, .reply = r_r2, .reply_cap = sizeof(r_r2) },
	};
	E.stream_active[0] = true;
	uint8_t executed   = 0;
	zassert_equal(gd32g553_batch(&C, ops, 3u, &executed), ALP_OK);
	zassert_equal(E.last_req_len, sizeof(V_BATCH_REQ));
	zassert_mem_equal(E.last_req, V_BATCH_REQ, sizeof(V_BATCH_REQ));
	zassert_equal(executed, 3u);

	/* stop-at-first-error: GPIO_WRITE ok, READ2 on stream 1 (inactive), PING */
	const uint8_t       s1_args[2] = { 1, 4 };
	uint8_t             r_s1[9 + 8];
	gd32g553_batch_op_t ops2[3] = {
		{ .op = 0x11, .args = gpio_args, .args_len = 8 },
		{ .op = 0x3C, .args = s1_args, .args_len = 2, .reply = r_s1, .reply_cap = sizeof(r_s1) },
		{ .op = 0x00 },
	};
	serve(V_BATCH_REP_STOP, sizeof(V_BATCH_REP_STOP));
	zassert_equal(gd32g553_batch(&C, ops2, 3u, &executed), ALP_OK);
	zassert_equal(executed, 2u, "executed counts the failing op, not the skipped one");
	zassert_equal(ops2[0].status, ALP_OK);
	zassert_equal(ops2[1].status, ALP_ERR_INVAL, "READ2 on an inactive stream");
	zassert_equal(ops2[1].reply_len, 0u, "a failing op carries no payload");
	zassert_equal(ops2[2].status, ALP_ERR_NOT_READY, "never ran");
}

ZTEST(gd32_protocol_015, test_batch_parses_sub_replies_through_the_model)
{
	zassert_equal(bring_up(true), ALP_OK);
	begin_stream0();
	E.r2_got      = 2;
	E.r2_first    = 0;
	E.r2_codes[0] = 0x0123;
	E.r2_codes[1] = 0x0456;

	const uint8_t       gpio_args[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
	const uint8_t       gr_args[4]   = { 0xFF, 0, 0, 0 };
	const uint8_t       r2_args[2]   = { 0, 16 };
	uint8_t             r_gr[4], r_r2[9 + 32];
	gd32g553_batch_op_t ops[4] = {
		{ .op = 0x00 },
		{ .op = 0x11, .args = gpio_args, .args_len = 8 },
		{ .op = 0x10, .args = gr_args, .args_len = 4, .reply = r_gr, .reply_cap = sizeof(r_gr) },
		{ .op = 0x3C, .args = r2_args, .args_len = 2, .reply = r_r2, .reply_cap = sizeof(r_r2) },
	};
	uint8_t executed = 0;
	zassert_equal(gd32g553_batch(&C, ops, 4u, &executed), ALP_OK);
	zassert_equal(executed, 4u);
	for (int i = 0; i < 4; i++)
		zassert_equal(ops[i].status, ALP_OK, "op %d", i);
	zassert_equal(ops[0].reply_len, 0u);
	zassert_equal(ops[2].reply_len, 4u);
	zassert_equal(ops[3].reply_len, 9u + 4u);
	zassert_equal(r_r2[8], 2u);
	zassert_equal((uint16_t)(r_r2[9] | (r_r2[10] << 8)), 0x0123u);
	zassert_equal((uint16_t)(r_r2[11] | (r_r2[12] << 8)), 0x0456u);
}

static void
batch_io_case(const uint8_t *raw, size_t n, uint8_t op, const uint8_t *args, uint8_t alen)
{
	zassert_equal(bring_up(false), ALP_OK);
	memcpy(E.batch_raw, raw, n);
	E.batch_raw_len = n;
	uint8_t             rep[16];
	gd32g553_batch_op_t o = {
		.op = op, .args = args, .args_len = alen, .reply = rep, .reply_cap = sizeof(rep)
	};
	zassert_equal(gd32g553_batch(&C, &o, 1u, NULL), ALP_ERR_IO);
}

ZTEST(gd32_protocol_015, test_batch_reply_io_checks)
{
	const uint8_t gr_args[4] = { 1, 0, 0, 0 };
	/* executed (2) > count (1), with entries short enough (two failing ops,
	 * len 0) to fit the clocked worst case -- only the executed check can
	 * reject this one */
	batch_io_case((const uint8_t[]){ 2, 1, 0, 1, 0 }, 5, 0x10, gr_args, 4);
	/* PING answers a payload byte: len 1 > its maximum 0 */
	batch_io_case((const uint8_t[]){ 1, 0, 1, 0xAA }, 4, 0x00, NULL, 0);
	/* GPIO_READ is a fixed 4-byte reply; 3 bytes is a mismatch */
	batch_io_case((const uint8_t[]){ 1, 0, 3, 1, 2, 3 }, 6, 0x10, gr_args, 4);
	/* a failing op must carry len 0 */
	batch_io_case((const uint8_t[]){ 1, 5, 1, 0xAA }, 4, 0x10, gr_args, 4);
	/* entry runs past what was clocked */
	batch_io_case((const uint8_t[]){ 1, 0, 0xFF }, 3, 0x10, gr_args, 4);
}

ZTEST(gd32_protocol_015, test_batch_local_validation_costs_no_bus_traffic)
{
	zassert_equal(bring_up(false), ALP_OK);
	const uint32_t      n0 = E.nreq;
	uint8_t             rep[300];
	const uint8_t       a8[8] = { 0 };
	gd32g553_batch_op_t o;

	/* nested BATCH, LINK_FEATURES, ADC_READ, OTA: not on the allow-list */
	const uint8_t not_allowed[] = { 0x04, 0x81, 0x30, 0x28, 0xF0, 0xF5, 0x35, 0x33 };
	for (size_t i = 0; i < sizeof(not_allowed); i++) {
		o = (gd32g553_batch_op_t){ .op = not_allowed[i], .args = a8, .args_len = 0 };
		zassert_equal(gd32g553_batch(&C, &o, 1u, NULL), ALP_ERR_INVAL, "op 0x%02x", not_allowed[i]);
	}
	/* wrong fixed request length */
	o = (gd32g553_batch_op_t){ .op = 0x11, .args = a8, .args_len = 4 };
	zassert_equal(gd32g553_batch(&C, &o, 1u, NULL), ALP_ERR_INVAL);
	/* reply buffer smaller than the op's maximum */
	o = (gd32g553_batch_op_t){
		.op = 0x21, .args = a8, .args_len = 1, .reply = rep, .reply_cap = 4
	};
	zassert_equal(gd32g553_batch(&C, &o, 1u, NULL), ALP_ERR_INVAL);
	/* count 0 / 17 */
	zassert_equal(gd32g553_batch(&C, &o, 0u, NULL), ALP_ERR_INVAL);
	gd32g553_batch_op_t many[17];
	for (int i = 0; i < 17; i++)
		many[i] = (gd32g553_batch_op_t){ .op = 0x00 };
	zassert_equal(gd32g553_batch(&C, many, 17u, NULL), ALP_ERR_OUT_OF_RANGE);
	/* worst-case reply over the link ceiling (2 x READ2 x 121 codes = 2 * 253 + 1 > 252) */
	const uint8_t       r2[2]  = { 0, 121 };
	gd32g553_batch_op_t two[2] = {
		{ .op = 0x3C, .args = r2, .args_len = 2, .reply = rep, .reply_cap = 251 },
		{ .op = 0x3C, .args = r2, .args_len = 2, .reply = rep, .reply_cap = 251 },
	};
	zassert_equal(gd32g553_batch(&C, two, 2u, NULL), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(E.nreq, n0, "every rejection above was local");
}

ZTEST(gd32_protocol_015, test_batch_firmware_validation_errors_surface)
{
	/* Defence in depth: the firmware re-validates.  Force its view to a
	 * smaller ceiling than the host believes and the outer status passes
	 * through (OUT_OF_RANGE: worst-case reply > its mp). */
	manual_link(0x1Fu, 252u);
	E.mp                      = 65u;
	const uint8_t       r2[2] = { 0, 60 };
	uint8_t             rep[9 + 120];
	gd32g553_batch_op_t o = {
		.op = 0x3C, .args = r2, .args_len = 2, .reply = rep, .reply_cap = sizeof(rep)
	};
	zassert_equal(gd32g553_batch(&C, &o, 1u, NULL), ALP_ERR_OUT_OF_RANGE);
}

/* ---- BIG_FRAME / reset / OTA -------------------------------------------------- */

ZTEST(gd32_protocol_015, test_legacy_opcodes_never_exceed_65_bytes)
{
	zassert_equal(bring_up(false), ALP_OK);
	/* An OTA chunk at the 65 B base ceiling must still work on a BIG link;
	 * the model answers INVAL to anything larger on a non-BATCH opcode. */
	uint8_t        data[GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES];
	const uint32_t n0 = E.nreq;
	memset(data, 0x5A, sizeof(data));
	(void)gd32g553_adc_dsp_stage_push(&C, 0u, 0u, 0u, data, sizeof(data));
	zassert_true(E.nreq > n0);
	zassert_true(E.max_write <= 4u + 65u, "request payloads stay <= 65 B off BATCH");
}

ZTEST(gd32_protocol_015, test_reset_drops_every_negotiated_item_and_renegotiates)
{
	zassert_equal(bring_up(true), ALP_OK);
	begin_stream0();
	zassert_equal(C.granted, 0x1Fu);

	emu_reboot(); /* GD32 reset: features off, replies stamped 0 */

	zassert_equal(gd32g553_gpio_write(&C, 1u, 1u), ALP_ERR_IO, "failed once, never re-sent");
	zassert_equal(E.cmd_count[GD32G553_CMD_GPIO_WRITE], 1u);
	zassert_equal(E.ext_count, 2u, "re-ran the 6-byte negotiation");
	zassert_equal(C.granted, 0x1Fu);
	zassert_equal(C.max_payload, 252u);
	zassert_true(C.attn_active, "ATTN re-requested; the pin stayed an input");
	zassert_equal(C.stream2_armed, 0u, "stream handles are gone");
	zassert_equal(gd32g553_gpio_write(&C, 1u, 0u), ALP_OK);
}

ZTEST(gd32_protocol_015, test_ota_commit_drops_negotiated_state)
{
	zassert_equal(bring_up(true), ALP_OK);
	zassert_equal(gd32g553_ota_commit(&C), ALP_OK);
	zassert_equal(C.granted, 0u);
	zassert_equal(C.max_payload, 65u);
	zassert_false(C.attn_active);
	zassert_false(C.seq_enabled);
}

ZTEST(gd32_protocol_015, test_stream_end_clears_event_arming)
{
	zassert_equal(bring_up(true), ALP_OK);
	begin_stream0();
	zassert_equal(C.stream2_armed, 1u);
	zassert_equal(gd32g553_adc_stream_end(&C, 0u), ALP_OK);
	zassert_equal(C.stream2_armed, 0u);
}
