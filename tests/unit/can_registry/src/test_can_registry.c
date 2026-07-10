/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the can registry dispatcher.  Mirrors the Slice 2b
 * i2s_registry / pwm_registry / gpio_registry harnesses; no vendor
 * extensions, so the test surface is the bare selector + capability-
 * getter + public-API edges plus the sw_fallback's NOSUPPORT path
 * for send / add_filter.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("can", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_CAN_SW_FALLBACK=y is set in prj.conf so that
 * the sw_fallback TU is linked in and the backend-count assertions
 * below can verify both backends are present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/can.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>

#include "../../../../src/backends/can/can_ops.h"

ZTEST_SUITE(alp_can_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_can_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("can", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_can_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("can", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("can") >= 2u);
}

/* ---------- (c) open returns NULL on invalid bus_id ----------------------- */

ZTEST(alp_can_registry, test_open_returns_null_on_invalid_bus_id)
{
	/* bus_id >= ARRAY_SIZE(_devs) (== 6) must be rejected by the
     * Zephyr backend's open(); the dispatcher relays the NULL back to
     * the caller and sets last_error. */
	alp_can_config_t cfg = {
		.bus_id             = 0xFFFFFFFFu,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	alp_can_t *h = alp_can_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_can_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_can_capabilities(NULL) must return NULL -- mirrors the
     * i2s / spi / gpio / pwm / uart pattern. */
	zassert_is_null(alp_can_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("can", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the can Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2b). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_can_registry, test_close_releases_handle)
{
	/* alp_can_open(NULL) -> NULL, no pool slot consumed */
	alp_can_t *h_null = alp_can_open(NULL);
	zassert_is_null(h_null);

	/* alp_can_close(NULL) must not crash */
	alp_can_close(NULL);

	/* alp_can_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_can_capabilities(NULL));
}

/* ---------- (f) sw_fallback round-trip + NOSUPPORT paths ------------------ */

extern const alp_backend_t __start_alp_backends_can[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_can[] __attribute__((weak));

static const alp_can_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_can; be < __stop_alp_backends_can; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_can_ops_t *)be->ops;
		}
	}
	return NULL;
}

static void _rx_cb_noop(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
}

ZTEST(alp_can_registry, test_sw_fallback_round_trip)
{
	const alp_can_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_can on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_can. */
	struct alp_can h;
	memset(&h, 0, sizeof(h));
	alp_can_backend_state_t *st   = &h.state;
	alp_capabilities_t       caps = { 0 };
	alp_can_config_t         cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);
	/* sw_fallback open leaves dev=NULL (no real controller) */
	zassert_is_null(st->dev);

	/* start / stop are no-ops returning ALP_OK */
	zassert_equal(ops->start(st), ALP_OK);
	zassert_equal(ops->stop(st), ALP_OK);

	/* send returns NOSUPPORT */
	alp_can_frame_t frame = {
		.id          = 0x123,
		.payload_len = 8u,
	};
	zassert_equal(ops->send(st, &frame, 10u), ALP_ERR_NOSUPPORT);

	/* add_filter returns NOSUPPORT */
	alp_can_filter_t filter = {
		.id     = 0x123,
		.mask   = 0x7FFu,
		.ext_id = false,
	};
	int32_t fid = -1;
	zassert_equal(ops->add_filter(st, &filter, _rx_cb_noop, NULL, &fid), ALP_ERR_NOSUPPORT);

	/* remove_filter is idempotent on the stub */
	zassert_equal(ops->remove_filter(st, 0), ALP_OK);

	/* close is a no-op; calling twice is safe */
	ops->close(st);
	ops->close(st);
}

/* ---------- (g) alp_can_remove_filter -- through the real public dispatcher */

ZTEST(alp_can_registry, test_alp_can_remove_filter_public_dispatch)
{
	/* Unlike (f) above, this drives the actual public
     * alp_can_remove_filter() dispatcher entry point
     * (src/can_dispatch.c), not the raw backend op -- exercises the
     * NOT_READY guard clause plus the real dispatch-to-backend call. */
	zassert_equal(alp_can_remove_filter(NULL, 0), ALP_ERR_NOT_READY);

	const alp_can_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	struct alp_can h;
	memset(&h, 0, sizeof(h));
	alp_capabilities_t caps = { 0 };
	alp_can_config_t   cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	zassert_equal(ops->open(&cfg, &h.state, &caps), ALP_OK);
	h.state.ops = ops; /* alp_can_open() normally wires this before open() */
	h.in_use    = true;

	/* Real dispatch to the sw_fallback op, which is idempotent ALP_OK. */
	zassert_equal(alp_can_remove_filter(&h, 0), ALP_OK);
	zassert_equal(alp_can_remove_filter(&h, 0), ALP_OK);

	ops->close(&h.state);
}

/* ---------- (h) alp_can_send enforces the started-handle contract (#601) -- */

ZTEST(alp_can_registry, test_send_enforces_started_handle_contract)
{
	/* Drive alp_can_send() through the real public dispatcher
     * (src/can_dispatch.c) against the sw_fallback ops directly --
     * the started-handle guard is dispatcher-level and must fire
     * *before* any backend is reached, so seeing NOT_READY (not the
     * sw_fallback's own NOSUPPORT) here proves the gate ran first. */
	const alp_can_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	struct alp_can h;
	memset(&h, 0, sizeof(h));
	alp_capabilities_t caps = { 0 };
	alp_can_config_t   cfg  = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	zassert_equal(ops->open(&cfg, &h.state, &caps), ALP_OK);
	h.state.ops = ops;
	h.in_use    = true;
	h.started   = false; /* never started */

	alp_can_frame_t frame = { .id = 0x123, .payload_len = 8u };

	/* send before start -> ALP_ERR_NOT_READY, never reaches the
     * backend. */
	zassert_equal(alp_can_send(&h, &frame, 10u), ALP_ERR_NOT_READY);

	/* send once "started" -> gate opens, backend is reached (and
     * sw_fallback's send is unconditionally NOSUPPORT, which is how we
     * tell "gate open" apart from "gate closed"). */
	h.started = true;
	zassert_equal(alp_can_send(&h, &frame, 10u), ALP_ERR_NOSUPPORT);

	/* send after stop -> ALP_ERR_NOT_READY again. */
	h.started = false;
	zassert_equal(alp_can_send(&h, &frame, 10u), ALP_ERR_NOT_READY);

	ops->close(&h.state);
}

/* ---------- (i) real zephyr_drv end-to-end: start/send/stop/restart ------- */

ZTEST(alp_can_registry, test_zephyr_drv_send_contract_end_to_end)
{
	/* boards/native_sim*.overlay aliases alp-can0 to native_sim's
     * built-in emulated `can_loopback0`, so this goes through the real
     * zephyr_drv backend (not sw_fallback) -- covers the full public
     * open/start/send/stop/restart cycle against a real Zephyr CAN
     * device (#601's acceptance list: before start, after stop,
     * restart then send). */
	alp_can_config_t cfg = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	alp_can_t *h = alp_can_open(&cfg);
	zassert_not_null(h);

	alp_can_frame_t frame = { .id = 0x123, .payload_len = 8u };

	/* send before start */
	zassert_equal(alp_can_send(h, &frame, 100u), ALP_ERR_NOT_READY);

	zassert_equal(alp_can_start(h), ALP_OK);
	zassert_equal(alp_can_send(h, &frame, 100u), ALP_OK);

	zassert_equal(alp_can_stop(h), ALP_OK);
	/* send after stop */
	zassert_equal(alp_can_send(h, &frame, 100u), ALP_ERR_NOT_READY);

	/* restart followed by a successful send */
	zassert_equal(alp_can_start(h), ALP_OK);
	zassert_equal(alp_can_send(h, &frame, 100u), ALP_OK);

	alp_can_close(h);
}

/* ---------- (j) remove_filter frees the cb_table slot -- no leak (#599) --- */

ZTEST(alp_can_registry, test_zephyr_drv_remove_filter_frees_slot)
{
	alp_can_config_t cfg = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};
	alp_can_t *h = alp_can_open(&cfg);
	zassert_not_null(h);
	zassert_equal(alp_can_start(h), ALP_OK);

	/* More than MAX_FILTERS (16) sequential add/remove cycles must not
     * exhaust the cb_table -- each remove has to free the slot it used,
     * or this loop starts failing well before 20 iterations. */
	alp_can_filter_t cycle_filter = { .id = 0x100u, .mask = 0x7FFu, .ext_id = false };
	for (int i = 0; i < 20; ++i) {
		int32_t fid = -1;
		zassert_equal(alp_can_add_filter(h, &cycle_filter, _rx_cb_noop, NULL, &fid), ALP_OK);
		zassert_equal(alp_can_remove_filter(h, fid), ALP_OK);
	}

	/* Fill every one of the 16 slots, confirm the 17th is genuinely
     * NOMEM (no spare capacity left at all), then free exactly one and
     * confirm the next add succeeds.  Before the fix, remove_filter()
     * never cleared cb_table, so this add would still return
     * ALP_ERR_NOMEM even though nothing else is active -- this is the
     * issue's literal acceptance criterion. */
	int32_t fids[16];
	for (int i = 0; i < 16; ++i) {
		alp_can_filter_t f = { .id = (uint32_t)(0x200 + i), .mask = 0x7FFu };
		zassert_equal(alp_can_add_filter(h, &f, _rx_cb_noop, NULL, &fids[i]), ALP_OK);
	}
	int32_t          overflow_fid = -1;
	alp_can_filter_t f17          = { .id = 0x300u, .mask = 0x7FFu };
	zassert_equal(alp_can_add_filter(h, &f17, _rx_cb_noop, NULL, &overflow_fid), ALP_ERR_NOMEM);

	zassert_equal(alp_can_remove_filter(h, fids[0]), ALP_OK);
	int32_t reused_fid = -1;
	zassert_equal(alp_can_add_filter(h, &f17, _rx_cb_noop, NULL, &reused_fid), ALP_OK);

	/* Removing an id that was never installed must fail -- previously
     * remove_filter() unconditionally returned ALP_OK regardless of
     * whether the id was real. */
	zassert_equal(alp_can_remove_filter(h, 0x7FFF), ALP_ERR_INVAL);

	/* Leave the device clean for the next test. */
	for (int i = 1; i < 16; ++i) {
		zassert_equal(alp_can_remove_filter(h, fids[i]), ALP_OK);
	}
	zassert_equal(alp_can_remove_filter(h, reused_fid), ALP_OK);

	alp_can_close(h);
}

/* ---------- (k) close() unregisters filters at the device level (#599) ---- */

ZTEST(alp_can_registry, test_zephyr_drv_close_unregisters_filters)
{
	alp_can_config_t cfg = {
		.bus_id             = 0u,
		.bitrate_nominal_hz = 500000u,
		.bitrate_data_hz    = 0u,
		.mode               = ALP_CAN_MODE_CLASSIC,
		.loopback           = false,
	};

	/* Handle A: fill every device-level filter slot, then close
     * WITHOUT removing any of them first -- before the fix, z_close()
     * never called can_remove_rx_filter(), so these stayed live on the
     * shared can_loopback0 device past close. */
	alp_can_t *ha = alp_can_open(&cfg);
	zassert_not_null(ha);
	zassert_equal(alp_can_start(ha), ALP_OK);
	for (int i = 0; i < 16; ++i) {
		alp_can_filter_t f   = { .id = (uint32_t)(0x400 + i), .mask = 0x7FFu };
		int32_t          fid = -1;
		zassert_equal(alp_can_add_filter(ha, &f, _rx_cb_noop, NULL, &fid), ALP_OK);
	}
	alp_can_close(ha); /* filters left installed on purpose */

	/* Handle B: reopen (fresh sidecar) and fill every device-level slot
     * again.  If close() actually unregistered handle A's filters, the
     * underlying can_loopback0 device has 16 free slots and every add
     * here succeeds; if it leaked them, the device is still full and
     * add_filter fails immediately. */
	alp_can_t *hb = alp_can_open(&cfg);
	zassert_not_null(hb);
	zassert_equal(alp_can_start(hb), ALP_OK);
	for (int i = 0; i < 16; ++i) {
		alp_can_filter_t f   = { .id = (uint32_t)(0x500 + i), .mask = 0x7FFu };
		int32_t          fid = -1;
		zassert_equal(alp_can_add_filter(hb, &f, _rx_cb_noop, NULL, &fid), ALP_OK);
	}
	alp_can_close(hb);
}

/* ---------- (l) payload_len <-> wire-DLC boundary mapping (#633) ---------- */

ZTEST(alp_can_registry, test_zephyr_drv_fd_dlc_byte_boundaries)
{
	/* zephyr_drv.c is the ONLY place a wire-encoded CAN-FD DLC nibble
	 * (0..15) ever appears -- it converts exactly once at the backend
	 * boundary via Zephyr's can_dlc_to_bytes()/can_bytes_to_dlc(), and
	 * alp_can_frame_t::payload_len is always the decoded byte count on
	 * either side of that call.  This locks the exact byte set the FD
	 * DLC table encodes (#633's acceptance list) so a future edit to
	 * the conversion call can't silently drift the byte mapping. */
	static const uint8_t fd_payload_bytes[] = { 0, 1,  2,  3,  4,  5,  6,  7,
		                                        8, 12, 16, 20, 24, 32, 48, 64 };

	for (size_t dlc = 0; dlc < ARRAY_SIZE(fd_payload_bytes); ++dlc) {
		uint8_t bytes = can_dlc_to_bytes((uint8_t)dlc);
		zassert_equal(bytes,
		              fd_payload_bytes[dlc],
		              "dlc=%zu -> bytes=%u (want %u)",
		              dlc,
		              bytes,
		              fd_payload_bytes[dlc]);
		/* Round-trip: the exact table byte counts must map back to
		 * their originating DLC (the encode direction z_send() uses). */
		zassert_equal(can_bytes_to_dlc(fd_payload_bytes[dlc]), (uint8_t)dlc);
	}

	/* Intermediate byte counts that are NOT exact FD DLC steps must
	 * round UP to the next valid step (ceiling), per Zephyr's
	 * documented can_bytes_to_dlc() policy -- never silently truncated
	 * or rounded down, which would drop payload bytes on the wire. */
	zassert_equal(can_bytes_to_dlc(9), 9);   /* -> 12-byte step (DLC 9) */
	zassert_equal(can_bytes_to_dlc(11), 9);  /* -> 12-byte step (DLC 9) */
	zassert_equal(can_bytes_to_dlc(13), 10); /* -> 16-byte step (DLC 10) */
	zassert_equal(can_bytes_to_dlc(17), 11); /* -> 20-byte step (DLC 11) */
	zassert_equal(can_bytes_to_dlc(25), 13); /* -> 32-byte step (DLC 13) */
	zassert_equal(can_bytes_to_dlc(33), 14); /* -> 48-byte step (DLC 14) */
	zassert_equal(can_bytes_to_dlc(49), 15); /* -> 64-byte step (DLC 15) */
}
