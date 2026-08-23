/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Core module: lifecycle (init / reset / hard_reset / sync / get_version /
 * stream_write), the request/reply framing primitive (cc3501e_request), and
 * poll_by_repeat -- the retry wrapper every other cc3501e_<subsystem>.c
 * module builds its worker-routed helpers on (declared in
 * cc3501e_internal.h).
 *
 * Ships the call shape (init / reset / get_version / request /
 * set_event_callback / deinit) and the framing logic.  The actual
 * reset-pin pulse + WIFI.EN sequencing arrives once the EVK overlay
 * declares the Alif's P15_5 / P15_1_FLEX as GPIOs reachable via
 * alp_gpio_*; until then reset() returns NOSUPPORT cleanly.
 *
 * Wire framing matches the embedded firmware
 * (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c): the current E1M-AEN
 * rev wires only SCLK/MOSI/MISO (no CS, no host IRQ -- both arrive next
 * rev), so a request/reply is clocked as four deterministic fixed-count
 * transfers in lockstep (request header, request payload, reply header,
 * reply payload) with a settle gap before the reply read.  The reply
 * payload's first byte is the response status (mapped via
 * resp_to_status); the data follows.
 */

#include <string.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "cc3501e_internal.h"

static void
encode_header(uint8_t *frame, alp_cc3501e_cmd_t cmd, uint8_t flags, uint16_t payload_len)
{
	frame[0] = (uint8_t)cmd;
	frame[1] = flags;
	frame[2] = (uint8_t)(payload_len & 0xFF);
	frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
}

static uint16_t decode_header_payload_len(const uint8_t *frame)
{
	return (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
}

alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus         = bus;
	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL || ctx->enable_pin == NULL) {
		/* The studio's pin allocator (or hand-written firmware
         * via alp_gpio_open) must populate enable_pin / reset_pin
         * before reset() is meaningful.  Until then there's no
         * line to pulse. */
		return ALP_ERR_NOSUPPORT;
	}
	/* Reset sequence per TI SWRU626 §7.1.5 (CC3501E technical
     * reference manual):
     *
     *   1. Assert nRESET low while bringing rails down so the
     *      chip stays clamped through the supply transition.
     *   2. Drop WIFI_EN low; wait briefly for the rails to
     *      discharge (10us is comfortably above the rail RC).
     *   3. Raise WIFI_EN; wait ~5 ms for the supply ramps to
     *      stabilise (typical PMIC soft-start window).
     *   4. Hold nRESET low for >= 10 us per §7.1.5 after the
     *      supplies are valid.
     *   5. Release nRESET; wait the T1+T2+T3+T4 boot budget
     *      (~900 ms typical for BL1 + BL2 + Chain-of-Trust)
     *      before the first PING is meaningful.
     *
     * Total blocking time: ~905 ms.  Callers that don't want
     * the synchronous wait can call cc3501e_reset asynchronously
     * (kicked off from a worker thread) and poll via PING; v0.3.x
     * adds a non-blocking variant once the firmware's "boot done"
     * GPIO is wired. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	(void)alp_gpio_write(ctx->enable_pin, false);
	/* COLD-BOOT POWER SEQUENCE (2026-06-17): generous, cold-safe timings.
	 * The CC3501E's secure boot (BL1->BL2->vendor image) runs ONLY on a true
	 * cold power-on; on this E1M-AEN board VPA(3.3V) is gated by WIFI_EN via the
	 * U1 load switch and the HFXT(52 MHz) crystal must be stable before/through
	 * the SES launch.  The earlier 10us discharge / 5ms ramp were too aggressive
	 * for a clean cold POR (warm reset hid it), so widen every window:
	 *   - 50 ms discharge so the rails fully collapse => the CC35 sees a real POR
	 *     (not a brown-out that skips Chain-of-Trust re-init), and
	 *   - 100 ms after WIFI_EN so VPA + the crystal are fully settled before
	 *     nRESET is released (TI SWRU626 §2.2.2.1: all supplies valid before
	 *     nRESET), with a 1 ms asserted-low hold, and
	 *   - 1500 ms boot budget before the first PING. */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->enable_pin, true);
	alp_delay_ms(100u);
	/* nRESET stays low through the rail ramp; this assignment is
     * idempotent but kept explicit for clarity. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	alp_delay_ms(1u);
	(void)alp_gpio_write(ctx->reset_pin, true);
	alp_delay_ms(1500u);
	/* Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround -- TI SDK bug confirmed
	 * by the CC35 module vendor 2026-06-18: the FIRST boot after a cold power-on
	 * mis-reads the Puya flash (the bug is specific to 32/64Mbit Puya parts), so the
	 * secure boot never launches the vendor image (host sees reqhdr_rx=0xFFFFFFFF).
	 * Re-boot once with the rails kept up; the second boot reads the now-settled
	 * flash and launches normally.  Validated on silicon (cold reqhdr_rx
	 * 0xFFFFFFFF -> 0x5A5A5A5A, ping_ok climbing, after one hard reset).  The
	 * bringup soak calls cc3501e_hard_reset() again if a single re-boot is not
	 * enough.  Remove once TI ships the Puya flash fix. */
	alp_status_t s = cc3501e_hard_reset(ctx);
	if (s != ALP_OK) return s;

	/* Wire-protocol compatibility gate (issue #1371): firmware/cc3501e/DESIGN.md
     * has always documented "host refuses a mismatch" for GET_VERSION, but
     * nothing ever compared the reply against ALP_CC3501E_PROTOCOL_VERSION --
     * mirrors the GD32 bridge's major-version gate (gd32g553_init(),
     * GD32G553_HOST_PROTOCOL_MAJOR), except the CC3501E wire carries a single
     * flat uint16_t (protocol_meta.c), not a major/minor/patch triple, so
     * there is no gradation to be lenient about: any difference means the
     * host cannot know the frame layout it is about to parse.
     *
     * This is the ONLY place the comparison runs -- deliberately NOT inside
     * cc3501e_get_version() itself, which stays a bare round-trip.  Two
     * callers depend on that: the #1116 concurrency regression
     * (tests/zephyr/cc3501e_transport_lock) drives cc3501e_get_version()
     * directly against a modelled slave that never claims to speak
     * ALP_CC3501E_PROTOCOL_VERSION, and the cold-boot liveness soaks
     * (examples/aen/aen-cc3501e-bringup's soak loop, every 8th cycle;
     * examples/peripheral-io/alp-console's cc3501e_bridge_bringup retry) use
     * cc3501e_get_version() purely as "did the round trip complete", not as a
     * compat gate -- putting the check there would turn a liveness probe
     * into a hard failure on real hardware.
     *
     * A GET_VERSION round trip that does not complete at all (the common
     * case immediately after this reset -- the Puya cold-boot flash bug
     * documented above routinely needs a second, caller-driven
     * cc3501e_hard_reset() before the slave answers anything) is NOT a
     * version verdict: only an ANSWERED request can be compared, so leave
     * the context usable and let the caller's own retry loop keep trying. */
	uint16_t     fw_version = 0u;
	alp_status_t vs         = cc3501e_get_version(ctx, &fw_version);
	if (vs != ALP_OK) {
		return ALP_OK;
	}
	if (fw_version != ALP_CC3501E_PROTOCOL_VERSION) {
		/* Permanent, not transient: retrying cannot reconcile two binaries
         * that disagree about the wire, so unlike the transport-timeout case
         * above this clears initialised -- every later call on this ctx now
         * fails ALP_ERR_NOT_READY instead of talking a wrong frame layout to
         * a radio. */
		ctx->initialised = false;
		return ALP_ERR_VERSION;
	}
	return ALP_OK;
}

alp_status_t cc3501e_hard_reset(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL) return ALP_ERR_NOSUPPORT;
	/* Pulse nRESET while keeping WIFI_EN asserted so the module re-boots WITHOUT a
	 * cold power cycle (a cold cycle would re-trigger the Puya-flash bug).  This is
	 * the "second boot" of the cold-boot workaround and the retry primitive the
	 * bringup soak uses when a cold-booted module has not come up yet. */
	(void)alp_gpio_write(ctx->reset_pin, false); /* assert nRESET; rails stay up */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->reset_pin, true); /* release -> re-boot */

	/* BLIND boot settle -- NO clocking until the slave is armed.  This is the
	 * cold first-contact fix: on the CS-less fixed-count link, any byte the host
	 * clocks BEFORE the slave's SPI is armed (e.g. a readiness poll's probes) is
	 * not consumed by the slave, which sets a permanent 1-byte frame offset that
	 * cannot self-correct (clocking advances both sides equally).  So the host
	 * stays QUIET while the module boots + arms; the slave then parks driving the
	 * 0xA5 marker, and the caller's first PING lands at the slave's fresh frame
	 * boundary = aligned.  The Wi-Fi build cold-boots (Puya double-boot + crypto
	 * Board_init) in ~2-3 s; 3.5 s covers it with margin and no clocking. */
	alp_delay_ms(3500u);
	return ALP_OK;
}

/* Map a CC3501E response status byte (first reply-payload byte, per
 * <alp/protocol/cc3501e.h>) onto the SDK's alp_status_t. */
static alp_status_t resp_to_status(uint8_t resp)
{
	switch (resp) {
	case ALP_CC3501E_RESP_OK:
		return ALP_OK;
	case ALP_CC3501E_RESP_ERR_INVALID:
		return ALP_ERR_INVAL;
	case ALP_CC3501E_RESP_ERR_BUSY:
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_TIMEOUT:
		return ALP_ERR_TIMEOUT;
	case ALP_CC3501E_RESP_ERR_NO_MEM:
		return ALP_ERR_NOMEM;
	case ALP_CC3501E_RESP_ERR_NOT_READY:
		return ALP_ERR_NOT_READY;
	case ALP_CC3501E_RESP_ERR_VERSION:
		return ALP_ERR_VERSION;
	case ALP_CC3501E_RESP_ERR_STATE:
		/* Deterministic firmware reject (e.g. BLE_GATT_REGISTER's NimBLE
		 * ble_gatts_mutable() ordering guard) -- distinct from ERR_BUSY's
		 * "worker still running, re-poll" and from ERR_RADIO's "transport/
		 * radio fault, maybe transient".  Mapped to the SAME ALP_ERR_BUSY a
		 * caller sees for a retryable busy, but poll_by_repeat below treats
		 * it as TERMINAL (checks the raw resp, not just the mapped status)
		 * so it is never retried and never burns the poll budget. */
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_RADIO:
	case ALP_CC3501E_RESP_ERR_PROTOCOL:
	case ALP_CC3501E_RESP_ERR_INTERNAL:
	default:
		return ALP_ERR_IO;
	}
}

/* ---- transport-transaction lock (issue #1116) -----------------------------
 *
 * cc3501e_request() is the ONLY place the 4-phase SPI exchange runs, and
 * five independent callers (Wi-Fi, BLE, GPIO proxy, the console companion,
 * the OTA path) share one cc3501e_t.  Without serialisation here, two
 * concurrent transactions interleave on the CS-less link and desync it, or
 * one caller reads back another caller's reply -- see the tx_scratch /
 * rx_scratch fields cc3501e_request reads and writes with no protection
 * before this fix.
 *
 * Compiler-builtin atomics on a plain bool, not an OS mutex: this TU is
 * OS-agnostic (chips/cc3501e/cc3501e_core.c builds into both the Zephyr
 * module and the plain-CMake / Yocto libalp_chips.a -- see CMakeLists.txt's
 * ALP_SDK_CHIP_LIST comment: "none of the chips/<id>/<id>.c cores include a
 * Zephyr/vendor header"), so it cannot call k_mutex_lock.  Same rationale
 * and the same __atomic_* primitives as src/common/alp_slot_claim.h, which
 * the dispatcher pools use for the identical portability reason.
 *
 * The acquire is BOUNDED, mirroring src/zephyr/v2n_supervisor.c's
 * alp_z_v2n_supervisor_acquire() for the structurally identical "one
 * shared transport, many portable backends" problem: a caller stuck behind
 * a wedged transaction gets ALP_ERR_BUSY back instead of hanging forever.
 * CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS is a real Kconfig knob on
 * Zephyr (zephyr/kconfigs/chips.kconfig) -- the Zephyr build injects every
 * CONFIG_* symbol as a compiler macro for every TU in the zephyr_library,
 * including this OS-agnostic core, so no <zephyr/...> include is needed to
 * see it.  Non-Zephyr backends have no Kconfig, so they fall back to the
 * #ifndef default below (same "portable constant, Zephyr-overridable"
 * shape already used by CC3501E_PHASE_SETTLE_US etc. in this file). */
#ifndef CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS
#define CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS 100u
#endif

static bool cc3501e_lock_try(cc3501e_t *ctx)
{
	bool expected = false;
	return __atomic_compare_exchange_n(
	    &ctx->request_lock, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Bounded acquire: try once (the common uncontended case costs nothing),
 * then poll once per millisecond via the portable alp_delay_ms (which
 * yields the CPU on every OS backend, unlike alp_delay_us's busy-wait --
 * needed so a contending thread actually gets scheduled) until the
 * Kconfig-bounded budget elapses. */
static alp_status_t cc3501e_lock_acquire(cc3501e_t *ctx)
{
	if (cc3501e_lock_try(ctx)) return ALP_OK;
	for (uint32_t waited_ms = 0u; waited_ms < CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS;
	     waited_ms++) {
		alp_delay_ms(1u);
		if (cc3501e_lock_try(ctx)) return ALP_OK;
	}
	return ALP_ERR_BUSY;
}

static void cc3501e_lock_release(cc3501e_t *ctx)
{
	__atomic_store_n(&ctx->request_lock, false, __ATOMIC_RELEASE);
}
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	/* MOSI is don't-care while syncing; 0xFF reads as a reserved-range
	 * ("no-op probe") header on the slave, which re-arms its header phase
	 * (firmware P0-2) so it keeps driving the 0xA5 marker -- making this walk
	 * non-destructive. */
	uint8_t tx = 0xFFu;
	uint8_t rx = 0u;

	/* Worst case, clock through one full in-flight request+reply frame to
	 * reach the slave's parked header boundary; "parked" = a run of two
	 * header-widths of 0xA5 (rejects a stray 0xA5 byte inside reply data). */
	const uint32_t walk_max = 2u * (uint32_t)(ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD);
	const uint32_t run_need = 2u * (uint32_t)ALP_CC3501E_HEADER_BYTES;
	const uint32_t attempts = (timeout_ms > 0u) ? timeout_ms : 1u;

	/* Serialise against cc3501e_request() (issue #1116).  This walk clocks
	 * the SAME CS-less bus, one byte at a time, up to walk_max times -- a
	 * concurrent 4-phase request would interleave with it and desync the
	 * link exactly as two concurrent requests would.  cc3501e_request()'s
	 * doxygen now advertises thread-safety, so a caller may legitimately
	 * re-sync from a second thread; that has to be safe.
	 *
	 * Held across the WHOLE walk, not per attempt: re-aligning to the
	 * slave's header boundary is only meaningful if nothing else moves the
	 * bus underneath us.  A request that lands mid-sync therefore gets
	 * ALP_ERR_BUSY from its own bounded acquire rather than corrupting the
	 * recovery -- which is the honest answer, since the link is by
	 * definition not usable until the sync completes. */
	alp_status_t lrc = cc3501e_lock_acquire(ctx);
	if (lrc != ALP_OK) return lrc;

	alp_status_t rc = ALP_ERR_TIMEOUT;
	for (uint32_t a = 0u; a < attempts && rc == ALP_ERR_TIMEOUT; a++) {
		uint32_t run = 0u;
		for (uint32_t w = 0u; w < walk_max; w++) {
			if (alp_spi_transceive(ctx->bus, &tx, &rx, 1u) != ALP_OK) {
				rc = ALP_ERR_IO;
				break;
			}
			if (rx == ALP_CC3501E_SYNC_IDLE) {
				if (++run >= run_need) { /* aligned at a clean header boundary */
					rc = ALP_OK;
					break;
				}
			} else {
				run = 0u;
			}
		}
		if (rc == ALP_ERR_TIMEOUT) {
			alp_delay_ms(1u); /* let the slave drain any in-flight frame + re-arm header phase */
		}
	}

	cc3501e_lock_release(ctx);
	return rc;
}

/* Inter-phase settle (CS-less lockstep): time given to the CC3501E SPI-slave ISR
 * to arm the next fixed-count transfer (request payload, reply payload) before
 * the host clocks it.  ~µs is enough; 200 µs is comfortably safe and negligible
 * vs the per-request budget.  The r2 bridge (CS + host-IRQ) removes the need. */
/* 200 us suits the CALLBACK/DMA slave, which re-arms in its completion ISR.
 * A POLLED slave (OTA update mode) only re-arms when its service loop next
 * enters SPI_transfer, and READY stays HIGH for a moment after a transfer
 * completes -- so the host could clock the request-PAYLOAD phase into a slave
 * that was not listening yet.  Header-only frames (e.g. OTA_STATUS) have no
 * such phase, which is exactly why STATUS kept working while every WRITE
 * returned -5 (silicon 2026-08-21).  Widen it; the cost is a fixed per-phase
 * delay only when READY is not already observed HIGH. */
/* Back to 200 us.  Widening this to 2000 us to give a POLLED slave time to arm
 * REGRESSED normal mode -- it applies to every phase, and update-mode ENTRY
 * (which runs on the ordinary DMA bridge) then timed out at -4.  The polled
 * request-PAYLOAD race is real but must be fixed on the DEVICE side, where it
 * can be scoped to the polled path, not by slowing every host phase. */
#define CC3501E_PHASE_SETTLE_US 200u

/* READY gate for the r2 SS0 + host-IRQ bridge.  When ctx->ready_pin is
 * populated (the CC35 GPIO17 -> Alif P2_6 line is wired + opened as an input),
 * wait for it HIGH -- the slave drives it HIGH when its SPI slave is armed+idle
 * -- before clocking a reply phase, instead of a fixed settle gap.  This tracks
 * the slave's actual re-arm rather than guessing, so slow (Wi-Fi/BLE) replies
 * no longer need a conservative fixed delay.  Opt-in + degrades safely: a NULL
 * ready_pin (CS-less r1 boards) or a line that never asserts falls back to the
 * fixed gap.  See project_cc3501e_link_topology. */

/* Latched the first time READY is ever observed HIGH.  Until that happens -- and
 * permanently on a board that never asserts it -- the gate keeps its historical
 * behaviour (short burst, then the fixed gap), so an unwired line cannot stall
 * every phase.  Once the line has proven itself wired AND driven, the gate
 * becomes AUTHORITATIVE and waits for the slave to re-arm however long the
 * device op takes.  That is the difference between surviving a flash blackout
 * and clocking into a dead slave: a burst of 64 reads is ~microseconds, while a
 * psa_fwu erase/write is orders of magnitude longer. */
static bool g_ready_line_proven;

/* Set while the peer is in OTA update mode (polled slave).  A polled slave only
 * re-arms when its service loop next enters SPI_transfer, so READY is still HIGH
 * for a moment AFTER a phase completes.  A LEVEL gate therefore returns
 * immediately and the host clocks the next phase into a slave that is not
 * listening -- which is why header-only frames (OTA_STATUS) worked while every
 * payload-bearing OTA_WRITE returned -5 and the stream died at off=256 (silicon
 * 2026-08-21).  With this set the gate waits for a LOW->HIGH EDGE instead, i.e.
 * for the slave to actually drop and re-raise READY around its re-arm. */
static bool g_peer_polled;

void cc3501e_set_peer_polled(bool on)
{
	g_peer_polled = on;
}

/* Authoritative-wait budget once the line is proven.  This bounds ONE PHASE, and
 * its only job is to not clock into a slave that has not re-armed -- a re-arm is
 * microseconds.  It must NOT be sized to outlast a device-side flash blackout:
 * waiting out a multi-minute psa_fwu erase is the CALLER's hold-off loop's job.
 * Sizing this at 5 s was measured on silicon 2026-08-21 to make a single
 * 4-phase cc3501e_ota_status cost up to 20 s while the caller's budget charged
 * it 250 ms, turning a nominal 600 s BEGIN confirmation into a ~13 HOUR wait
 * that read as "BEGIN never returns". */
#define CC3501E_READY_WAIT_US 250000u
#define CC3501E_READY_POLL_US 200u
/* How long to wait for a polled peer to DROP ready before giving up on the
 * edge and treating the line as level-only. */
#define CC3501E_READY_EDGE_US 20000u

/* A POLLED slave (OTA update mode) re-arms only when its service loop next enters
 * SPI_transfer -- microseconds of processing, not an ISR -- so the host's fallback
 * settle has to cover that.  200 us is right for the DMA slave and too short here:
 * header-only frames survived while every payload-bearing OTA_WRITE lost its bytes
 * and the stream died at off=256 (silicon 2026-08-21).  Widening the settle for
 * EVERY peer regressed update-mode ENTRY to -4 (that handshake runs on the ordinary
 * DMA bridge), so it is scoped to polled peers only.  Independent of READY, which
 * is not readable on this bench (READY probe: rc=0 level=0). */
/* WAS 5000u.  That value was sized against a symptom, not a cause: "every
 * payload-bearing OTA_WRITE lost its bytes and the stream died at off=256" is
 * exactly the polled RX-FIFO desync that spi_fifo_reset() (firmware
 * transport_hw_ti_spi.c) now clears at the start of EVERY frame.  With the cause
 * fixed, this gate is back to doing only its stated job -- covering the slave's
 * re-arm, which this file's own comment describes as "microseconds".  At 5000us
 * it cost 4 gates x 5 ms = 20 ms per frame, ~2 frames per 256 B chunk. */
#define CC3501E_POLLED_SETTLE_US 200u

static void cc3501e_reply_gate(const cc3501e_t *ctx, uint32_t fallback_us)
{
	if (g_peer_polled && fallback_us < CC3501E_POLLED_SETTLE_US) {
		fallback_us = CC3501E_POLLED_SETTLE_US;
	}
	if (ctx->ready_pin != NULL) {
		bool           level     = false;
		const uint32_t budget_us = g_ready_line_proven ? CC3501E_READY_WAIT_US : 0u;
		uint32_t       waited_us = 0u;
		if (g_peer_polled && g_ready_line_proven) {
			/* Edge, not level: first wait for the slave to DROP ready (it is
			 * finishing the previous phase), then fall through to the normal
			 * wait-for-HIGH below, which now means "re-armed".  Bounded so a
			 * peer that never drops it degrades to the old level behaviour. */
			for (uint32_t low_us = 0u; low_us < CC3501E_READY_EDGE_US;
			     low_us += CC3501E_READY_POLL_US) {
				if (alp_gpio_read(ctx->ready_pin, &level) == ALP_OK && !level) {
					break;
				}
				alp_delay_us(CC3501E_READY_POLL_US);
			}
		}
		for (;;) {
			/* Opportunistic burst: catches an already-armed slave with no delay. */
			for (uint32_t i = 0; i < 64u; ++i) {
				if (alp_gpio_read(ctx->ready_pin, &level) == ALP_OK && level) {
					g_ready_line_proven = true;
					return;
				}
			}
			if (waited_us >= budget_us) {
				break;
			}
			alp_delay_us(CC3501E_READY_POLL_US);
			waited_us += CC3501E_READY_POLL_US;
		}
	}
	alp_delay_us(fallback_us);
}

/* The actual 4-phase exchange, WITHOUT taking ctx's transport lock -- the
 * caller must already hold it.  Split out of cc3501e_request() (issue
 * #1116 follow-up) so poll_by_repeat() below can bracket its OWN extra
 * ctx->rx_scratch[0] sentinel write/peek in the SAME lock acquisition as
 * the request itself, instead of leaving them either side of a
 * lock-acquire-per-call boundary where a second caller's request could
 * interleave with the sentinel write/peek even though every individual
 * cc3501e_request() call is itself now atomic. */
static alp_status_t cc3501e_request_locked(cc3501e_t        *ctx,
                                           alp_cc3501e_cmd_t cmd,
                                           const uint8_t    *tx_payload,
                                           size_t            tx_len,
                                           uint8_t          *rx_buf,
                                           size_t            rx_cap,
                                           size_t           *rx_len)
{
	alp_status_t s;

	/*
     * 3-wire deterministic framing (this HW rev wires only SCLK/MOSI/MISO
     * -- no CS, no host IRQ; CS + IRQ arrive next rev).  Each transfer's
     * length is derived from a header already exchanged, so master + slave
     * stay in lockstep without a CS edge.  Matches the firmware SPI-slave
     * state machine in firmware/cc3501e/hal/ti/transport_hw_ti_spi.c.
     *
     *   1. send request header (4)        3. read reply header (4)
     *   2. send request payload (tx_len)  4. read reply payload (status+data)
     */
	/* Gate READY before the REQUEST header too.  After the slave sends a reply it
	 * re-arms its header phase in its ISR; a spaced request (soak loop, bring-up)
	 * has ample idle time so the header always landed on an armed slave.  But a
	 * TIGHT back-to-back loop -- streaming via cc3501e_stream_write -- clocks the
	 * next header the instant the prior reply is read, racing that re-arm: the
	 * first frame acks, then every following frame desyncs (bench 2026-07-04:
	 * dma_stream_iters stuck at 1).  READY tracks the actual header-arm; on a
	 * CS-less r1 board with no ready_pin the fallback is the same short settle the
	 * other phases use. */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	encode_header(ctx->tx_scratch, cmd, ALP_CC3501E_FLAG_RESP_REQUIRED, (uint16_t)tx_len);
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) goto out;
	if (tx_len > 0) {
		/* Inter-phase settle (CS-less lockstep): the slave arms the request-PAYLOAD
		 * transfer in its SPI ISR only AFTER the header transfer completes.  Clocking
		 * the payload back-to-back (no gap) races that re-arm -> the payload bytes are
		 * dropped + the frame desyncs.  Header-only requests (PING / the argless worker
		 * ops) have no payload phase so they were fine; payload requests (OTA_WRITE,
		 * CONNECT, GPIO_WRITE) need this gap (root-caused on silicon 2026-06-19, where
		 * OTA streaming timed out per-chunk without it).
		 *
		 * This was the ONE phase still on a bare fixed delay while phases 1, 3 and 4
		 * consult READY.  It is also the phase that clocks the most bytes (a 260 B
		 * OTA_WRITE), so it is the worst one to send blind at a slave that has not
		 * re-armed.  Gate it like the others; the delay stays as the fallback. */
		cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
		s = alp_spi_transceive(ctx->bus, tx_payload, ctx->rx_scratch, tx_len);
		if (s != ALP_OK) goto out;
	}

	/* Wait for the slave to dispatch + arm its reply before we read: the
	 * READY gate tracks it via the host-IRQ line when wired, else a fixed gap. */
	cc3501e_reply_gate(ctx, 200u);

	/* Dummies for the read transactions (MOSI is don't-care on a read). */
	memset(ctx->tx_scratch, 0xFF, sizeof(ctx->tx_scratch));

	/* 3. Reply header -> learn the reply payload length. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) goto out;
	uint16_t resp_payload_len = decode_header_payload_len(ctx->rx_scratch);
	/* Desync detection (no CS to recover on): a valid reply header ECHOES the
     * request opcode (protocol_build_reply sets reply[0]=cmd) and declares a
     * payload in [1..MAX].  An all-0xA5 header means the slave is parked at a
     * frame boundary (we were misaligned); any other mismatch is lockstep
     * drift.  Either way, re-establish byte alignment so the NEXT request lands
     * clean, and report IO so the caller retries (the soak/bring-up loops do). */
	const bool hdr_ok = (ctx->rx_scratch[0] == (uint8_t)cmd) && (resp_payload_len >= 1u) &&
	                    (resp_payload_len <= ALP_CC3501E_MAX_PAYLOAD);
	if (!hdr_ok) {
		/* Do NOT byte-walk cc3501e_sync() here: on the 4-byte fixed-count lockstep
		 * the 1-byte walk PARKS the slave (proven on silicon -- reply_hdr stuck at
		 * 0xA5A5A5A5, link never recovers).  Return IO so the caller re-issues a
		 * clean 4-byte transaction instead (re-aligns when the slave is aligned). */
		s = ALP_ERR_IO;
		goto out;
	}

	/* Same READY gate before the reply PAYLOAD phase (the slave re-arms it in
	 * its ISR only after the reply-header transfer completes). */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	/* 4. Reply payload: status byte followed by the response data. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, resp_payload_len);
	if (s != ALP_OK) goto out;

	{
		const uint8_t resp     = ctx->rx_scratch[0];
		const size_t  data_len = (size_t)resp_payload_len - 1u;
		if (data_len > 0u && rx_buf != NULL) {
			const size_t n = (data_len > rx_cap) ? rx_cap : data_len;
			memcpy(rx_buf, &ctx->rx_scratch[1], n);
			if (rx_len != NULL) *rx_len = n;
		}
		/* #1378: a dead bus phase reads back literal 0x00 for every byte it
		 * clocks -- this repo's own silicon finding (see
		 * hal/ti/cc3501e_hw_ti_wifi.c's cc3501e_hw_wifi_lazy_start(), "the
		 * host then reads 0x00000000 from a dead link").  0x00 is ALSO
		 * ALP_CC3501E_RESP_OK, so a header that read intact (hdr_ok above --
		 * genuinely alive) followed by a payload phase that dies in the
		 * inter-phase gap (cc3501e_reply_gate above, CC3501E_PHASE_SETTLE_US)
		 * is silently indistinguishable from a real, successful bare-status
		 * reply: ALP_OK must require positive evidence the device framed a
		 * reply, not merely the absence of evidence that it did not.
		 *
		 * A content-based check cannot be applied generally here: several
		 * bare-OK replies legitimately ARE all-zero (WIFI_STATUS's
		 * disconnected-and-never-attempted state, DIAG_GET_STATS' zero
		 * counters right after boot, SOCK_RECV's zero-bytes-pending) --
		 * flagging those would trade a rare false ALP_OK for a routine false
		 * ALP_ERR_IO on paths that are correct today.  The check is therefore
		 * PER-OPCODE, and an opcode earns its place on the list below only by
		 * a firmware fact: its handler cannot EVER frame a synchronous bare
		 * RESP_OK, so seeing one here is self-evidently the dead-phase alias,
		 * not a value this driver has merely decided is improbable.
		 *
		 * WIFI_CONNECT_STA (0x12) -- #1378.  Its firmware handler
		 * (handle_worker_routed_payload's WORKER_IDLE case,
		 * firmware/cc3501e/src/protocol.c) UNCONDITIONALLY acks a fresh
		 * submit with RESP_ERR_BUSY.  Rejecting the alias here avoids handing
		 * the caller a false "submitted", which is exactly the #1376
		 * false-connect mechanism.  cc3501e_wifi_connect() no longer trusts
		 * this ack in either direction (it only trusts the independent
		 * WIFI_STATUS latch -- see cc3501e_wifi.c), so for that opcode this
		 * is defense in depth.
		 *
		 * WIFI_AP_START (0x14) -- #1385.  Same handler, same unconditional
		 * BUSY submit ack, AND the ONE path that could otherwise return a
		 * bare RESP_OK for this opcode is unreachable to the host: the drain
		 * (firmware/cc3501e/src/worker.c's worker_run_pending()) calls
		 * worker_reset() for exactly CONNECT_STA and AP_START *before*
		 * cc3501e_bridge_ready() re-arms the link, so the WORKER_DONE branch
		 * that would reply RESP_OK is wiped while the host is still held off
		 * and can never be collected.  Unlike CONNECT_STA this is NOT defense
		 * in depth: this rejection was cc3501e_wifi_ap_start()'s ONLY route to
		 * ALP_OK, so that wrapper no longer polls it -- it submits
		 * WIFI_AP_START exactly once and reports ALP_ERR_TIMEOUT
		 * unconditionally (see cc3501e_wifi.c), since there is no reply this
		 * opcode can ever frame as success.  Restoring a real success path
		 * still needs the same submit-once-then-confirm restructure
		 * cc3501e_wifi_connect() got, which firmware v4 cannot yet support
		 * (cc3501e_hw_wifi_ap_start() never writes the g_wifi_conn latch that
		 * WIFI_STATUS reads, so there is no independent AP channel to confirm
		 * against).  Still open on #1385.
		 *
		 * OTA_PROMOTE (0x46) is deliberately NOT on this list, despite being
		 * the sharpest case named in #1378/#1385: handle_ota_promote()
		 * (firmware/cc3501e/src/protocol_ota.c) returns
		 * hw_to_resp(cc3501e_hw_ota_promote()), and the TI HAL's
		 * cc3501e_hw_ota_promote() (hal/ti/cc3501e_hw_ti_ota.c) arms the
		 * deferred swap-reboot and returns CC3501E_HW_OK UNCONDITIONALLY --
		 * a bare RESP_OK (reply_data_len 0 -> payload len 1) is that opcode's
		 * ONLY success reply.  Rejecting it here would make
		 * cc3501e_ota_promote() always report ALP_ERR_IO and break firmware
		 * promotion outright.  Closing the alias for OTA_PROMOTE needs either
		 * a wire-level CRC/canary (a protocol version bump touching host and
		 * firmware) or host-side confirmation against OTA_STATUS (0x44) --
		 * neither is a transport-layer change.  Still open on #1385. */
		if (resp == ALP_CC3501E_RESP_OK && resp_payload_len == 1u &&
		    (cmd == ALP_CC3501E_CMD_WIFI_CONNECT_STA || cmd == ALP_CC3501E_CMD_WIFI_AP_START)) {
			s = ALP_ERR_IO;
		} else {
			s = resp_to_status(resp);
		}
	}

out:
	return s;
}

alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms)
{
	(void)timeout_ms; /* Reserved for a future IRQ-driven wait (next HW rev). */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (tx_len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/* Serialise the whole exchange (issue #1116): every phase in
	 * cc3501e_request_locked() reads or writes ctx->tx_scratch /
	 * ctx->rx_scratch, shared by every caller of this ctx.  Acquired AFTER
	 * the pure param checks above (so a bad call fails fast without
	 * touching the lock). */
	alp_status_t s = cc3501e_lock_acquire(ctx);
	if (s != ALP_OK) return s;
	s = cc3501e_request_locked(ctx, cmd, tx_payload, tx_len, rx_buf, rx_cap, rx_len);
	cc3501e_lock_release(ctx);
	return s;
}

alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out)
{
	if (version_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s =
	    cc3501e_request(ctx, ALP_CC3501E_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), &got, 100);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*version_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_stream_write(cc3501e_t *ctx, const uint8_t *data, size_t len)
{
	if (ctx == NULL || (data == NULL && len > 0u)) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES)) {
		return ALP_ERR_INVAL;
	}
	/* One framed bulk frame: the request PAYLOAD phase clocks @len bytes in a
	 * single transfer, which takes the host DMA path when @len >= the SPI DMA
	 * threshold (CONFIG_SPI_DW_ALIF_DMA_MIN_LEN).  The firmware sinks + acks it,
	 * so the link stays framed -- send these back-to-back for a bulk stream. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_STREAM_WRITE, data, len, NULL, 0u, NULL, 200u);
}

/* Poll-by-repeat backoff: how long to wait between BUSY repeats.
 *
 * This was a FLAT 50 ms, and it is the single biggest cost on every
 * worker-routed op.  The firmware's worker model is submit-then-collect: the
 * dispatch runs in the SPI callback (SWI/HWI context) and cannot call the radio
 * or IP stacks, so handle_worker_routed_* ALWAYS answers RESP_ERR_BUSY to the
 * submit and the host must come back for the result.  A flat gap therefore
 * charges 50 ms to every such op no matter how fast the worker actually
 * finished -- and most finish in well under a millisecond.  Measured on
 * silicon: a 487 B CMD_SOCK_RECV (one frame is capped at
 * MAX_PAYLOAD - recv_resp header - status = 487 B) costs two round trips plus
 * one gap, i.e. ~50 ms, which is ~9.7 kB/s against a 14 MHz link.
 *
 * So START short and BACK OFF exponentially to the old ceiling.  The ceiling
 * matters: cc3501e_ota_update's flush hold-off polls THROUGH a flash blackout,
 * where the device answers from an ISR the flash op has stopped, so every frame
 * clocked in that window goes into a dead slave.  Backing off to 50 ms keeps
 * the blackout frame count essentially unchanged (a 600 s hold-off gains ~6
 * extra frames in total) while collecting a ready result in ~1 ms. */
#define CC3501E_POLL_GAP_MIN_MS 1u
#define CC3501E_POLL_GAP_MS     50u

alp_status_t poll_by_repeat(cc3501e_t        *ctx,
                            alp_cc3501e_cmd_t cmd,
                            const uint8_t    *tx_payload,
                            size_t            tx_len,
                            uint8_t          *rx_buf,
                            size_t            rx_cap,
                            size_t           *rx_len,
                            uint32_t          timeout_ms)
{
	/* Same param checks cc3501e_request() runs, done ONCE here (cmd/
	 * tx_payload/tx_len are fixed across every retry below, unlike the
	 * transport lock which is per-attempt) rather than calling the public
	 * wrapper per iteration -- see the sentinel-race comment below. */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (tx_len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/* Budget is coarse-grained in CC3501E_POLL_GAP_MS slices; always make at
	 * least one attempt even with a zero timeout. */
	uint32_t     remaining   = (timeout_ms > 0u) ? timeout_ms : 1u;
	uint32_t     next_gap_ms = CC3501E_POLL_GAP_MIN_MS;
	alp_status_t s;
	for (;;) {
		/* Sentinel + peek bracketed in the SAME lock hold as the request
		 * itself (issue #1116 follow-up): both touch the shared
		 * ctx->rx_scratch[0] this ctx's other callers can also write, so
		 * lock-acquire-per-cc3501e_request()-call alone isn't enough --
		 * a second caller's request could still land between this
		 * sentinel write and this attempt's own request, or between this
		 * request and the peek below, corrupting the disambiguation this
		 * retry loop depends on. */
		s = cc3501e_lock_acquire(ctx);
		if (s != ALP_OK) return s;
		/* Re-zero per attempt, not just once before the loop: an attempt
		 * that copied out n bytes and then mapped to BUSY/IO would
		 * otherwise leave that stale count visible to a caller who reads
		 * *rx_len after this function finally returns TIMEOUT. */
		if (rx_len != NULL) *rx_len = 0;
		/* Sentinel: pre-set rx_scratch[0] to a byte the peek below never
		 * matches (0xFF).  Only a real reply payload overwrites it with the
		 * resp byte; a BUSY that comes from the transport (alp_spi_transceive
		 * -EBUSY) rather than resp_to_status() then leaves the sentinel, so it
		 * can never masquerade as RESP_ERR_STATE. */
		ctx->rx_scratch[0] = 0xFFu;
		s = cc3501e_request_locked(ctx, cmd, tx_payload, tx_len, rx_buf, rx_cap, rx_len);
		/* resp_to_status() maps BOTH RESP_ERR_BUSY (worker still running --
		 * genuinely retryable) and RESP_ERR_STATE (a deterministic firmware
		 * reject -- e.g. BLE_GATT_REGISTER's NimBLE ordering guard) to the
		 * same ALP_ERR_BUSY, since that is the correct final answer for
		 * both.  But only the FORMER is worth re-polling: retrying the
		 * latter just repeats the same reject until the budget is gone
		 * (register-while-advertising must fail promptly, not after burning
		 * the whole poll window).  Only a RESP_ERR_STATE reply writes 0x09
		 * into rx_scratch[0] (the sentinel above rules out a transport BUSY),
		 * so the peek disambiguates safely -- read here, still under the
		 * lock, not after release. */
		const bool terminal_reject =
		    (s == ALP_ERR_BUSY && ctx->rx_scratch[0] == ALP_CC3501E_RESP_ERR_STATE);
		cc3501e_lock_release(ctx);
		if (terminal_reject) {
			return s; /* terminal reject -- do not retry */
		}
		if (s != ALP_ERR_BUSY && s != ALP_ERR_IO) {
			return s; /* OK or a non-retryable error -- done. */
		}
		if (remaining == 0u) {
			return ALP_ERR_TIMEOUT;
		}
		uint32_t gap = (remaining < next_gap_ms) ? remaining : next_gap_ms;
		alp_delay_ms(gap);
		remaining -= gap;
		/* Double until the ceiling: fast for a result that is already staged,
		 * unchanged for a device that is genuinely away. */
		if (next_gap_ms < CC3501E_POLL_GAP_MS) {
			next_gap_ms =
			    (next_gap_ms * 2u > CC3501E_POLL_GAP_MS) ? CC3501E_POLL_GAP_MS : next_gap_ms * 2u;
		}
	}
}
