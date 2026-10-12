/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32_swd connect-under-reset ordering (GD32 bridge protocol 0.15 §3.17,
 * rules H3/H4).  P71 -- the SWD clock -- is also the bridge ATTN pin, which
 * the GD32 drives while ATTN is granted, so the host may drive it only while
 * GD32_NRST (P74) holds the GD32 in reset:
 *
 *   notify(session) -> NRST output + low -> SWDIO/SWCLK outputs -> line reset
 *   -> DPIDR -> DHCSR.C_DEBUGEN -> DEMCR.VC_CORERESET -> NRST released
 *
 * and, on deinit, the pads go back to inputs.  The real driver runs against a
 * GPIO mock with a bit-level SW-DP target behind it (the driver's bit-bang
 * waveform is decoded from SWCLK rising edges), so the order is observed on
 * the pads, not inferred from the driver's source.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/chips/gd32_swd.h"
#include "../../../../chips/gd32_swd/gd32_swd_platform.h"

/* ---- the three opaque pad handles ---------------------------------------- */

struct alp_gpio {
	int id;
};
static struct alp_gpio P_SWDIO = { 0 }, P_SWCLK = { 1 }, P_NRST = { 2 };

/* ---- event log ------------------------------------------------------------- */

enum ev_type {
	EV_NOTIFY,      /* val = active */
	EV_CFG,         /* pin, val = direction (1 = output): a PLAIN configure */
	EV_CFG_LOW,     /* pin: switched to an OUTPUT with an INITIAL LOW level */
	EV_CLOSE,       /* pin closed */
	EV_NRST_WRITE,  /* val = level */
	EV_SWCLK_FIRST, /* first SWCLK write of the session */
	EV_MEMW,        /* pin = unused, addr/val of a target memory write */
};

struct ev {
	enum ev_type t;
	int          pin;
	uint32_t     addr;
	uint32_t     val;
};

#define EV_MAX 16384
static struct ev g_ev[EV_MAX];
static unsigned  g_nev;
static bool      g_swclk_seen;

static void log_ev(enum ev_type t, int pin, uint32_t addr, uint32_t val)
{
	if (g_nev < EV_MAX) g_ev[g_nev++] = (struct ev){ t, pin, addr, val };
}

/* index of the first event matching, or -1 */
static int find_ev(enum ev_type t, int pin, uint32_t val, int from)
{
	for (unsigned i = (from < 0) ? 0u : (unsigned)from; i < g_nev; i++) {
		if (g_ev[i].t == t && (pin < 0 || g_ev[i].pin == pin) && g_ev[i].val == val) {
			return (int)i;
		}
	}
	return -1;
}

static int find_memw(uint32_t addr)
{
	for (unsigned i = 0; i < g_nev; i++) {
		if (g_ev[i].t == EV_MEMW && g_ev[i].addr == addr) return (int)i;
	}
	return -1;
}

/* ---- failure injection ------------------------------------------------------ */

static bool     g_fail_nrst_write;
static bool     g_missing_pad; /* the board does not publish the pads */
static bool     g_no_halt;     /* the core ignores halt-on-reset */
static bool     g_fail_mem;    /* the target FAULTs writes to g_fail_addr */
static uint32_t g_fail_addr;

/* ---- bit-level SW-DP target -------------------------------------------------- */

enum phase { P_IDLE, P_HDR, P_TURN1, P_ACK, P_RDATA, P_TURN_R, P_TURN_W, P_WDATA };

static struct {
	enum phase phase;
	bool       swdio_out_level; /* the level the host drives */
	bool       host_drives;     /* SWDIO configured as output */
	unsigned   ones_run, zr;
	bool       armed; /* a transaction just ended: the next 1 is a start bit */
	uint8_t    hdr;
	unsigned   hdr_bits;
	unsigned   ack_i, ack_val_bits;
	uint32_t   ack; /* 1 = OK, 4 = FAULT */
	uint32_t   rdata;
	unsigned   rd_i;
	uint32_t   wdata;
	unsigned   wd_i;
	unsigned   skip_edges;
	bool       ap, rnw;
	uint8_t    addr;
	/* registers */
	uint32_t ctrlstat, select, csw, tar, rdbuff;
	bool     vc_armed; /* DEMCR.VC_CORERESET written */
	bool     halted;   /* the core stopped at its reset vector */
} T;

static bool parity32_t(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return (v & 1u) != 0u;
}

static void target_reset(void)
{
	memset(&T, 0, sizeof(T));
}

/* Decide the register access once the 8-bit header is in. */
static void decode_header(void)
{
	T.ap   = (T.hdr >> 1) & 1u;
	T.rnw  = (T.hdr >> 2) & 1u;
	T.addr = (uint8_t)((((T.hdr >> 3) & 1u) << 2) | (((T.hdr >> 4) & 1u) << 3));
	T.ack  = 1u; /* OK */
	if (T.ap && !T.rnw && T.addr == 0x0Cu && g_fail_mem && T.tar == g_fail_addr) T.ack = 4u;

	if (T.rnw) {
		if (!T.ap) {
			T.rdata = (T.addr == 0x00u)   ? 0x6BA02477u
			          : (T.addr == 0x04u) ? (T.ctrlstat | 0xF0000000u)
			          : (T.addr == 0x0Cu) ? T.rdbuff
			                              : 0u;
		} else {
			T.rdata = 0u; /* AP reads are pipelined: the real value comes via RDBUFF */
			/* DHCSR reads S_HALT (bit 17) once the core stopped at the vector. */
			T.rdbuff = (T.addr == 0x0Cu && T.tar == 0xE000EDF0u && T.halted) ? (1u << 17) | 1u : 0u;
		}
	}
}

static void apply_write(void)
{
	if (!T.ap) {
		if (T.addr == 0x04u) T.ctrlstat = T.wdata;
		if (T.addr == 0x08u) T.select = T.wdata;
		return;
	}
	if (T.addr == 0x00u) T.csw = T.wdata;
	if (T.addr == 0x04u) T.tar = T.wdata;
	if (T.addr == 0x0Cu) {
		log_ev(EV_MEMW, 0, T.tar, T.wdata);
		if (T.tar == 0xE000EDFCu && (T.wdata & 1u)) T.vc_armed = true;
	}
}

static void on_rising(void)
{
	if (T.skip_edges > 0u) {
		T.skip_edges--;
		return;
	}
	switch (T.phase) {
	case P_IDLE: {
		if (!T.host_drives) break;
		const bool bit = T.swdio_out_level;
		if (bit) {
			if (T.armed || (T.zr == 2u && T.ones_run == 0u)) {
				T.armed = false;
				T.phase =
				    P_HDR; /* start bit: after a transaction, or after line-reset + two zeros */
				T.hdr      = 1u;
				T.hdr_bits = 1u;
				T.zr       = 0u;
				break;
			}
			T.ones_run++;
			if (T.zr != 0u) T.zr = 0u;
		} else {
			if (T.ones_run >= 50u) {
				T.zr = 1u;
			} else if (T.zr != 0u) {
				T.zr++;
				if (T.zr > 2u) T.zr = 0u;
			}
			T.ones_run = 0u;
		}
		break;
	}
	case P_HDR:
		if (T.swdio_out_level) T.hdr |= (uint8_t)(1u << T.hdr_bits);
		if (++T.hdr_bits == 8u) {
			decode_header();
			T.phase = P_TURN1;
		}
		break;
	case P_TURN1:
		T.phase = P_ACK;
		T.ack_i = 0u;
		break;
	case P_ACK:
	case P_RDATA:
		break; /* progress is counted on the host's reads */
	case P_TURN_R:
		T.phase    = P_IDLE;
		T.ones_run = 0u;
		T.zr       = 0u;
		T.armed    = true;
		break;
	case P_TURN_W:
		T.phase = P_WDATA;
		T.wd_i  = 0u;
		T.wdata = 0u;
		break;
	case P_WDATA:
		if (T.wd_i < 32u && T.swdio_out_level) T.wdata |= (1u << T.wd_i);
		if (++T.wd_i == 33u) {
			apply_write();
			T.phase    = P_IDLE;
			T.ones_run = 0u;
			T.zr       = 0u;
			T.armed    = true;
		}
		break;
	}
}

static bool target_read_bit(void)
{
	if (T.phase == P_ACK) {
		const bool bit = (T.ack >> T.ack_i) & 1u;
		if (++T.ack_i == 3u) {
			T.skip_edges = 1u; /* the edge of this last ACK bit */
			if (T.ack != 1u) {
				T.phase = P_TURN_R; /* FAULT: the host just consumes the turnaround */
			} else if (T.rnw) {
				T.phase      = P_RDATA;
				T.rd_i       = 0u;
				T.skip_edges = 0u; /* data bits follow; their edges are ignored in P_RDATA */
			} else {
				T.phase = P_TURN_W;
			}
		}
		return bit;
	}
	if (T.phase == P_RDATA) {
		bool bit;
		if (T.rd_i < 32u) {
			bit = (T.rdata >> T.rd_i) & 1u;
		} else {
			bit = parity32_t(T.rdata);
		}
		if (++T.rd_i == 33u) {
			T.phase      = P_TURN_R;
			T.skip_edges = 1u; /* the edge of this last data bit */
		}
		return bit;
	}
	return false;
}

/* ---- the GPIO mock the driver bit-bangs --------------------------------------- */

/* Pad model: an output LATCH that survives direction changes (the GD32_NRST pad
 * is left with a STALE-HIGH latch by a pre-0.15 image).  g_latch_at_flip records
 * the latch at the instant a pad becomes an output; g_nrst_drove_high is set if
 * the NRST pad is ever an output while its latch is high. */
static bool g_latch[3];
static bool g_is_out[3];
static int  g_latch_at_flip[3];
static bool g_nrst_drove_high;

alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	(void)pull;
	if (dir == ALP_GPIO_OUTPUT && !g_is_out[pin->id]) {
		g_latch_at_flip[pin->id] = g_latch[pin->id] ? 1 : 0;
		if (pin == &P_NRST && g_latch[pin->id]) g_nrst_drove_high = true;
	}
	g_is_out[pin->id] = (dir == ALP_GPIO_OUTPUT);
	log_ev(EV_CFG, pin->id, 0u, (dir == ALP_GPIO_OUTPUT) ? 1u : 0u);
	if (pin == &P_SWDIO) T.host_drives = (dir == ALP_GPIO_OUTPUT);
	/* NRST released (high-impedance): a core armed for halt-on-reset stops at
	 * its vector (unless the test says it does not). */
	if (pin == &P_NRST && dir == ALP_GPIO_INPUT) T.halted = T.vc_armed && !g_no_halt;
	return ALP_OK;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	g_latch[pin->id] = level; /* P latch: written whatever the direction */
	if (pin == &P_NRST && g_is_out[pin->id] && level) g_nrst_drove_high = true;
	if (pin == &P_NRST) {
		log_ev(EV_NRST_WRITE, pin->id, 0u, level ? 1u : 0u);
		return g_fail_nrst_write ? ALP_ERR_IO : ALP_OK;
	}
	if (pin == &P_SWDIO) {
		T.swdio_out_level = level;
		return ALP_OK;
	}
	if (pin == &P_SWCLK) {
		if (!g_swclk_seen) {
			g_swclk_seen = true;
			log_ev(EV_SWCLK_FIRST, pin->id, 0u, 0u);
		}
		if (level) on_rising();
	}
	return ALP_OK;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	*level = (pin == &P_SWDIO) ? target_read_bit() : false;
	return ALP_OK;
}

/* The platform pad seam (src/zephyr/gd32_swd_pads.c in the real tree): the
 * driver opens its own pads, and switches NRST to an output with an INITIAL LOW
 * level in one step so the shared open-drain net is never driven high. */
static alp_gpio_t *mock_open_pad(uint32_t pad_id)
{
	if (g_missing_pad) return NULL;
	return (pad_id == GD32G553_PAD_ID_SWDIO)   ? &P_SWDIO
	       : (pad_id == GD32G553_PAD_ID_SWCLK) ? &P_SWCLK
	       : (pad_id == GD32G553_PAD_ID_NRST)  ? &P_NRST
	                                           : NULL;
}

/* The backend contract (src/backends/gpio/zephyr_drv.c z_configure_output_low):
 * latch LOW while the pad is still an input, THEN switch the direction -- the RZ
 * FSP writes PM before P, so a direction switch alone would drive the stale latch. */
static alp_status_t mock_configure_output_low(alp_gpio_t *pad)
{
	log_ev(EV_CFG_LOW, pad->id, 0u, 0u);
	g_latch[pad->id]         = false; /* 1. latch low (input) */
	g_latch_at_flip[pad->id] = 0;     /* 2. direction flips  */
	g_is_out[pad->id]        = true;
	return ALP_OK;
}

static void mock_close_pad(alp_gpio_t *pad)
{
	log_ev(EV_CLOSE, pad->id, 0u, 0u);
}

static const gd32_swd_platform_t g_mock_platform = {
	.open_pad             = mock_open_pad,
	.configure_output_low = mock_configure_output_low,
	.close_pad            = mock_close_pad,
};

const gd32_swd_platform_t *gd32_swd_platform(void)
{
	return &g_mock_platform;
}

/* The platform session hook (the V2N supervisor in the real tree). */
void gd32_swd_session_notify(bool active)
{
	log_ev(EV_NOTIFY, 0, 0u, active ? 1u : 0u);
}

/* ---- fixture ------------------------------------------------------------------- */

static void each_before(void *unused)
{
	(void)unused;
	g_nev             = 0u;
	g_swclk_seen      = false;
	g_missing_pad     = false;
	g_no_halt         = false;
	g_fail_nrst_write = false;
	g_fail_mem        = false;
	memset(g_latch, 0, sizeof(g_latch));
	memset(g_is_out, 0, sizeof(g_is_out));
	g_latch[2]         = true; /* NRST: stale-high latch left by an older image */
	g_latch_at_flip[0] = g_latch_at_flip[1] = g_latch_at_flip[2] = -1;
	g_nrst_drove_high                                            = false;
	target_reset();
}

ZTEST_SUITE(gd32_swd_connect_under_reset, NULL, NULL, each_before, NULL, NULL);

/* ---- tests --------------------------------------------------------------------- */

/* The driver takes no pin handles: it opens its own pads, so application code
 * has nothing to hand it (and cannot open those ids at all). */
ZTEST(gd32_swd_connect_under_reset, test_init_refuses_a_null_context_and_touches_nothing)
{
	zassert_equal(gd32_swd_init(NULL), ALP_ERR_INVAL);
	zassert_equal(g_nev, 0u);
}

/* A board that publishes no pads: nothing may be driven, the session closes. */
ZTEST(gd32_swd_connect_under_reset, test_init_without_published_pads_drives_nothing)
{
	g_missing_pad = true;
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_ERR_NOT_READY);
	zassert_equal(find_ev(EV_CFG, -1, 1u, 0), -1);
	zassert_equal(find_ev(EV_CFG_LOW, -1, 0u, 0), -1);
	zassert_equal(find_ev(EV_NRST_WRITE, -1, 0u, 0), -1);
	const int n_true  = find_ev(EV_NOTIFY, -1, 1u, 0);
	const int n_false = find_ev(EV_NOTIFY, -1, 0u, 0);
	zassert_true(n_true >= 0 && n_false > n_true);
}

/* NRST goes low BEFORE either SWD pad becomes an output, and it becomes an
 * output only with an INITIAL LOW level -- a plain "configure as output" keeps the
 * old latch (a pre-0.15 image left it high) and would drive the shared net high. */
ZTEST(gd32_swd_connect_under_reset, test_nrst_is_asserted_before_swd_pads_are_driven)
{
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);

	const int i_notify = find_ev(EV_NOTIFY, -1, 1u, 0);
	const int i_nrst_o = find_ev(EV_CFG_LOW, P_NRST.id, 0u, 0);
	const int i_nrst_l = find_ev(EV_NRST_WRITE, -1, 0u, 0);
	const int i_clk_o  = find_ev(EV_CFG, P_SWCLK.id, 1u, 0);
	const int i_dio_o  = find_ev(EV_CFG, P_SWDIO.id, 1u, 0);
	const int i_clk_w  = find_ev(EV_SWCLK_FIRST, -1, 0u, 0);
	zassert_true(i_notify >= 0 && i_nrst_o > i_notify, "session announced first");
	zassert_true(i_nrst_l > i_nrst_o, "NRST output-with-initial-LOW, then written low");
	zassert_equal(find_ev(EV_CFG, P_NRST.id, 1u, 0),
	              -1,
	              "NRST is NEVER switched to an output by a plain configure (latch could be high)");
	zassert_true(i_clk_o > i_nrst_l && i_dio_o > i_nrst_l,
	             "SWCLK/SWDIO become outputs only after NRST is low");
	zassert_true(i_clk_w > i_nrst_l, "no SWCLK edge before NRST is low");
	zassert_equal(find_ev(EV_NRST_WRITE, -1, 1u, 0), -1, "NRST is never driven high (open-drain)");
	zassert_true(swd.nrst_held, "still asserted after init: connect releases it");
	zassert_equal(find_ev(EV_CFG, P_NRST.id, 0u, 0), -1, "NRST not released yet");
	/* The pad starts with a stale-high latch: it must read LOW at the moment the
	 * direction flips to output, and the pad must never have driven high. */
	zassert_equal(g_latch_at_flip[P_NRST.id], 0, "latch low BEFORE NRST became an output");
	zassert_false(g_nrst_drove_high, "NRST pad never drove the shared net high");
	gd32_swd_deinit(&swd);
}

/* If NRST cannot be asserted the pads must never become outputs. */
ZTEST(gd32_swd_connect_under_reset, test_init_fails_without_touching_the_swd_pads_when_nrst_fails)
{
	g_fail_nrst_write = true;
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_ERR_IO);
	zassert_equal(find_ev(EV_CFG, P_SWCLK.id, 1u, 0), -1, "SWCLK never became an output");
	zassert_equal(find_ev(EV_CFG, P_SWDIO.id, 1u, 0), -1, "SWDIO never became an output");
	zassert_true(find_ev(EV_CFG, P_NRST.id, 0u, 0) >= 0, "NRST handed back to hi-Z");
	const int n_true  = find_ev(EV_NOTIFY, -1, 1u, 0);
	const int n_false = find_ev(EV_NOTIFY, -1, 0u, 0);
	zassert_true(n_true >= 0 && n_false > n_true, "session closed again");
}

/* The connect-under-reset order on the wire: debug enable, then halt-on-reset,
 * only THEN is NRST released, and the core is proven halted afterwards. */
ZTEST(gd32_swd_connect_under_reset,
      test_nrst_stays_asserted_through_connect_until_halt_on_reset_is_armed)
{
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);
	zassert_equal(gd32_swd_connect(&swd), ALP_OK);
	zassert_equal(swd.idcode, 0x6BA02477u, "the DP answered under reset");

	const int i_nrst_low = find_ev(EV_NRST_WRITE, -1, 0u, 0);
	const int i_dhcsr    = find_memw(0xE000EDF0u);
	const int i_demcr    = find_memw(0xE000EDFCu);
	const int i_release  = find_ev(EV_CFG, P_NRST.id, 0u, 0);
	zassert_true(i_dhcsr > i_nrst_low, "DHCSR written while NRST is still low");
	zassert_equal(g_ev[i_dhcsr].val, 0xA05F0001u, "DBGKEY | C_DEBUGEN");
	zassert_true(i_demcr > i_dhcsr, "DEMCR after DHCSR");
	zassert_equal(g_ev[i_demcr].val, 0x1u, "DEMCR.VC_CORERESET: halt on the reset vector");
	zassert_true(i_release > i_demcr, "NRST released only after halt-on-reset is armed");
	zassert_false(swd.nrst_held);
	zassert_true(T.halted, "the core stopped at its reset vector");
	gd32_swd_deinit(&swd);
}

/* If the core did NOT halt after NRST was released it is running application code
 * (and may grant ATTN): connect must fail and put it back under reset. */
ZTEST(gd32_swd_connect_under_reset,
      test_connect_fails_and_reasserts_nrst_when_the_core_did_not_halt)
{
	g_no_halt = true;
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);
	zassert_equal(gd32_swd_connect(&swd), ALP_ERR_IO);
	const int i_release = find_ev(EV_CFG, P_NRST.id, 0u, 0);
	zassert_true(i_release >= 0);
	zassert_true(find_ev(EV_CFG_LOW, P_NRST.id, 0u, i_release) > i_release,
	             "NRST driven low again (initial-low) after the failed halt");
	zassert_true(find_ev(EV_NRST_WRITE, -1, 0u, i_release) > i_release);
	zassert_true(swd.nrst_held, "left asserted");
	gd32_swd_deinit(&swd);
}

/* If halt-on-reset cannot be armed NRST must stay asserted: the GD32 is held
 * in reset rather than released to run application code (and ATTN). */
ZTEST(gd32_swd_connect_under_reset, test_nrst_stays_asserted_when_halt_on_reset_cannot_be_armed)
{
	g_fail_mem  = true;
	g_fail_addr = 0xE000EDFCu; /* DEMCR */
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);
	zassert_not_equal(gd32_swd_connect(&swd), ALP_OK);
	zassert_equal(find_ev(EV_CFG, P_NRST.id, 0u, 0), -1, "NRST not released");
	zassert_true(swd.nrst_held);

	gd32_swd_deinit(&swd); /* only deinit lets the GD32 go */
	zassert_true(find_ev(EV_CFG, P_NRST.id, 0u, 0) >= 0);
}

/* reset_and_run: NRST is asserted (initial-low), P70/P71 go back to inputs, and
 * ONLY THEN is NRST released -- the booting GD32 may grant ATTN and drive PA14
 * at once, which must not meet a host SWCLK output. */
ZTEST(gd32_swd_connect_under_reset,
      test_reset_and_run_returns_the_pads_to_inputs_before_releasing_nrst)
{
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);
	zassert_equal(gd32_swd_connect(&swd), ALP_OK);
	const int before = (int)g_nev;
	zassert_equal(gd32_swd_reset_and_run(&swd), ALP_OK);

	const int i_assert = find_ev(EV_CFG_LOW, P_NRST.id, 0u, before);
	zassert_true(i_assert >= before, "NRST asserted (initial low)");
	/* (SWDIO toggles direction during the best-effort halt-on-reset disarm that
	 * precedes the assert, so look for the input switch AFTER the assert.) */
	const int i_clk_in = find_ev(EV_CFG, P_SWCLK.id, 0u, i_assert);
	const int i_dio_in = find_ev(EV_CFG, P_SWDIO.id, 0u, i_assert);
	const int i_rel    = find_ev(EV_CFG, P_NRST.id, 0u, i_assert);
	zassert_true(i_clk_in > i_assert && i_dio_in > i_assert, "pads to inputs while NRST is held");
	zassert_true(i_rel > i_clk_in && i_rel > i_dio_in, "NRST released LAST");
	gd32_swd_deinit(&swd);
}

/* deinit gives P70/P71 back as inputs (P71 is the bridge ATTN pin again),
 * releases NRST, closes the pads and ends the session last. */
ZTEST(gd32_swd_connect_under_reset, test_deinit_returns_the_pads_to_inputs_and_closes_the_session)
{
	gd32_swd_t swd;
	zassert_equal(gd32_swd_init(&swd), ALP_OK);
	zassert_equal(gd32_swd_connect(&swd), ALP_OK);
	const unsigned before = g_nev;
	gd32_swd_deinit(&swd);

	const int i_clk_in = find_ev(EV_CFG, P_SWCLK.id, 0u, (int)before);
	const int i_dio_in = find_ev(EV_CFG, P_SWDIO.id, 0u, (int)before);
	const int i_close  = find_ev(EV_CLOSE, P_NRST.id, 0u, (int)before);
	const int i_end    = find_ev(EV_NOTIFY, -1, 0u, (int)before);
	zassert_true(i_clk_in >= 0, "SWCLK (P71) back to an input");
	zassert_true(i_dio_in >= 0, "SWDIO (P70) back to an input");
	const int i_rel = find_ev(EV_CFG, P_NRST.id, 0u, (int)before);
	zassert_true(i_rel > i_clk_in && i_rel > i_dio_in,
	             "P70/P71 back to inputs BEFORE NRST is released (as in reset_and_run)");
	zassert_true(i_close > i_clk_in && i_close > i_dio_in, "pads closed after they are inputs");
	zassert_true(i_end > i_close, "session closed last");
	zassert_false(swd.initialised);
}
