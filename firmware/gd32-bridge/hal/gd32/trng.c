/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- TRNG.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* TRNG (NIST SP800-90B) state + bring-up.                            */
/* ----------------------------------------------------------------- */

/* TRNG bring-up is INCREMENTAL: `trng_started` records that the unit
 * is configured + enabled; `trng_ready` that the first conditioned
 * word arrived (DRDY) with clean self-checks.  The split exists
 * because the NIST-mode pipeline's first DRDY (analog source startup
 * + 440-bit window fill + SHA-256 stage) can take longer than any
 * wait this firmware is allowed to spin inside a request handler --
 * the original single-shot bring-up waited ~1 ms at boot, timed out
 * on real silicon, latched trng_ready = false FOREVER and every
 * TRNG_READ answered STATUS_IO (HiL soak 2026-06-04: 0-for-437).
 * Now bring-up configures and returns; each read re-polls DRDY with
 * a short bound until conditioning completes, and a tripped
 * CECS/SECS self-check triggers a full reconfigure on the next call
 * instead of bricking the surface for the session. */
bool trng_started = false;
bool trng_ready   = false;

/* Coarse timeout for the PLL stable + TRNG DRDY polls.  Roughly
 * 100k iterations of `trng_flag_get` -- about half a millisecond
 * at the GD32G553's 216 MHz core clock when the TRNG is healthy
 * (DRDY trips in dozens of cycles, so the timeout is the abort
 * latch, not the typical-case bound). */
#define TRNG_INIT_TIMEOUT 100000u
#define TRNG_READY_TIMEOUT 65535u

/* Configure + enable the TRNG (no DRDY wait here -- see above).
 * Mirrors the vendor's `TRNG_NIST_mode` example: PLL Q / 2 clock
 * source, SHA-256 conditioning over a 440-bit input window, 256-bit
 * output stage. */
bool trng_start(void)
{
	uint32_t to;

	/* Vendor's `SystemInit()` (called from Reset_Handler before
     * main()) boots the PLL.  In normal operation PLLSTB is set by
     * the time we get here; bound the wait so a misconfigured
     * clock tree doesn't hang. */
	for (to = TRNG_INIT_TIMEOUT; to != 0u; --to) {
		if (SET == rcu_flag_get(RCU_FLAG_PLLSTB)) break;
	}
	if (to == 0u) return false;

	rcu_trng_clock_config(RCU_TRNG_CKPLLQ_DIV2);
	rcu_periph_clock_enable(RCU_TRNG);

	/* Self-tests on the analog noise source.  trng_clockerror_detection
     * arms the CECS flag so we'd see a clock outage during runtime. */
	trng_clockerror_detection_enable();

	trng_deinit();
	trng_conditioning_reset_enable();
	trng_mode_config(TRNG_MODSEL_NIST);
	trng_nist_seed_config(TRNG_NIST_SEED_ANALOG);
	trng_conditioning_input_bitwidth(TRNG_INMOD_440BIT);
	trng_conditioning_output_bitwidth(TRNG_OUTMOD_256BIT);
	trng_conditioning_algo_config(TRNG_ALGO_SHA256);
	trng_conditioning_enable();
	trng_postprocessing_enable();
	trng_enable();
	return true;
}

/* TRNG fault detector.  Mirrors the vendor's trng_ready_check: a
 * fault shows in FOUR places, and the two LATCHED interrupt flags are
 * the ones that persist -- a seed error parks the unit with SEIF +
 * STAT.ERRSTA set while the *current*-status SECS reads CLEAR again
 * (silicon 2026-06-04: TRNG_STAT == 0x48, DRDY never returning, and a
 * CECS/SECS-only check looping blind on DRDY until its budget died).
 * Recovery for any of them is the same full deinit + reconfigure. */
static bool trng_faulted(void)
{
	return (SET == trng_interrupt_flag_get(TRNG_INT_FLAG_CEIF)) ||
	       (SET == trng_interrupt_flag_get(TRNG_INT_FLAG_SEIF)) ||
	       (SET == trng_flag_get(TRNG_FLAG_CECS)) || (SET == trng_flag_get(TRNG_FLAG_SECS));
}

static void trng_demote(void)
{
	trng_deinit(); /* RCU reset clears the latched flags too */
	trng_started = false;
	trng_ready   = false;
}

/* Short, handler-safe DRDY poll: promotes `trng_ready` once the first
 * conditioned word lands.  Faults are checked BEFORE the DRDY wait --
 * in the latched-error state DRDY never comes, and burning the whole
 * poll budget on a dead unit kept the request handler busy long
 * enough to tar-pit unrelated commands. */
bool trng_poll_ready(void)
{
	uint32_t to;

	if (trng_faulted()) {
		trng_demote();
		return false;
	}
	for (to = TRNG_READY_TIMEOUT; to != 0u; --to) {
		if (SET == trng_flag_get(TRNG_FLAG_DRDY)) break;
	}
	if (to == 0u || trng_faulted()) {
		if (trng_faulted()) trng_demote();
		return false;
	}
	trng_ready = true;
	return true;
}

int bridge_hw_trng_read(uint8_t *dest, size_t len)
{
	if (dest == 0) return BRIDGE_HW_ERR_INVAL;
	if (len == 0u || len > 32u) return BRIDGE_HW_ERR_RANGE;

	/* Lazy readiness ladder (see trng_started/trng_ready above): a
     * unit that never started reconfigures here; a configured unit
     * that hasn't produced its first word yet gets another short DRDY
     * poll.  Each step is bounded handler-safe work; the host's reply
     * re-read schedule absorbs the one-time latency, and a persistent
     * failure keeps answering STATUS_IO honestly. */
	if (!trng_started) {
		trng_started = trng_start();
		if (!trng_started) return BRIDGE_HW_ERR_IO;
	}
	if (!trng_ready && !trng_poll_ready()) return BRIDGE_HW_ERR_IO;

	/* Pull 32-bit randoms and pack their LSB bytes into `dest`.  A
     * single trng_get_true_random_data() call drains one entry from
     * the TRNG's output FIFO and the unit refills autonomously; we
     * poll DRDY between pulls so a starved-noise condition doesn't
     * silently emit a stale word.
     *
     * The DRDY budget is SHARED across the whole request, NOT
     * per-word: a max-length pull is 8 words = one full 256-bit NIST
     * conditioning round, so the FIFO routinely runs dry mid-request
     * and per-word waits stack up to ~9 ms of handler time -- far
     * beyond the host's reply window.  The overrun reply then
     * tar-pitted the next several commands (functional tier
     * 2026-06-04: trng-32B plus exactly three followers failed on
     * every fresh boot).  When the budget runs out the unit is
     * HEALTHY, just mid-conditioning -- answer BUSY and let the host
     * retry into the next round. */
	uint32_t budget = 2u * TRNG_READY_TIMEOUT;
	size_t   off    = 0u;
	while (off < len) {
		if (trng_faulted()) {
			/* Fault (latched seed/clock error included) -- checked
             * BEFORE the DRDY wait, since a parked unit never raises
             * DRDY again.  Demote so the next call rebuilds (full
             * re-seed); fail THIS call loudly -- never pad with
             * non-random bytes. */
			trng_demote();
			return BRIDGE_HW_ERR_IO;
		}
		while ((RESET == trng_flag_get(TRNG_FLAG_DRDY)) && (budget != 0u)) {
			--budget;
		}
		if (RESET == trng_flag_get(TRNG_FLAG_DRDY)) {
			if (trng_faulted()) {
				trng_demote();
				return BRIDGE_HW_ERR_IO;
			}
			return BRIDGE_HW_ERR_BUSY; /* healthy, mid-conditioning */
		}

		uint32_t     word  = trng_get_true_random_data();
		const size_t chunk = (len - off >= 4u) ? 4u : (len - off);
		for (size_t i = 0; i < chunk; ++i) {
			dest[off++] = (uint8_t)(word & 0xFFu);
			word >>= 8;
		}
	}
	return BRIDGE_HW_OK;
}
