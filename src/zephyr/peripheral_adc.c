/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Streaming ADC backend for <alp/adc.h>.
 *
 * Owns alp_adc_stream_open / _read / _close on Zephyr targets.
 * V2N family (V2N + V2N-M1): routes through the GD32G553 supervisor
 * MCU's DMA-backed stream slots.  Other SoMs: returns
 * ALP_ERR_NOSUPPORT (a polling-thread software fallback is on the
 * wave-2 roadmap).
 *
 * One-shot ADC (alp_adc_open / read_raw / read_uv / close) lives in
 * src/adc_dispatch.c plus the per-backend sources under
 * src/backends/adc/ as of the Slice 1 migration (2026-05-22).  This
 * file shares no symbols with the dispatcher; both compile together
 * under CONFIG_ALP_SDK_PERIPH_ADC.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/adc.h"
#include "alp/dsp.h"
#include "alp/soc_caps.h"
#include "alp_slot_claim.h"
#include "handles.h"
#include "v2n_supervisor.h"

/* On V2N every E1M ADC channel is GD32-driven (Renesas SoC's
 * ALP_SOC_ADC_COUNT = 24 but the board exposes the 8 E1M channels
 * via the GD32 IO MCU per gd32-io-mcu-map.tsv).  The bridge already
 * returns mV-corrected readings, so the V2N path uses mV as the
 * "raw" value (16-bit unsigned, sign-extended into int32) and
 * alp_adc_read_uv multiplies by 1000 to honour the public contract. */
#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
#define ALP_ADC_HAS_BRIDGE_PATH 1
#else
#define ALP_ADC_HAS_BRIDGE_PATH 0
#endif

/* DT spec table and errno_to_alp removed -- they were only used by
 * the one-shot ADC path, which moved to src/backends/adc/alif_e7.c
 * during the Slice 1 migration.  Streaming uses gd32g553_adc_stream_*
 * directly with explicit alp_status_t returns. */

#if ALP_ADC_HAS_BRIDGE_PATH
/* Stream-slot bitmap.  The GD32G553 firmware exposes exactly
 * GD32G553_BRIDGE_ADC_STREAM_COUNT (= 2) DMA-backed streams; the
 * portable surface tracks allocation locally so alp_adc_stream_open
 * doesn't have to probe the firmware with a speculative
 * STREAM_BEGIN that could collide with another caller's existing
 * slot.  Same bitmap covers V2N and V2N-M1 (shared supervisor). */
static struct k_mutex bridge_stream_lock;
static uint8_t        bridge_streams_used;

static int bridge_stream_lock_init(void)
{
	k_mutex_init(&bridge_stream_lock);
	return 0;
}
SYS_INIT(bridge_stream_lock_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static int bridge_stream_alloc_slot(void)
{
	k_mutex_lock(&bridge_stream_lock, K_FOREVER);
	int slot = -1;
	for (int i = 0; i < (int)GD32G553_BRIDGE_ADC_STREAM_COUNT; ++i) {
		if (!(bridge_streams_used & (1u << i))) {
			bridge_streams_used |= (uint8_t)(1u << i);
			slot = i;
			break;
		}
	}
	k_mutex_unlock(&bridge_stream_lock);
	return slot;
}

static void bridge_stream_free_slot(uint8_t slot)
{
	k_mutex_lock(&bridge_stream_lock, K_FOREVER);
	bridge_streams_used &= (uint8_t)~(1u << slot);
	k_mutex_unlock(&bridge_stream_lock);
}
#endif /* ALP_ADC_HAS_BRIDGE_PATH */

/* One-shot ADC (alp_adc_open / read_raw / read_uv / close) is served by the
 * registry-based src/adc_dispatch.c +
 * src/backends/adc/{alif_e7,gd32_bridge,sw_fallback}.c.  This file hosts
 * only the streaming ADC implementation (alp_adc_stream_*).
 */

/* ====================================================================== */
/* Streaming ADC -- DMA-backed continuous acquisition.                     */
/*                                                                         */
/* V2N family (V2N + V2N-M1): both SoMs carry the GD32G553 supervisor MCU, */
/* whose firmware exposes GD32G553_BRIDGE_ADC_STREAM_COUNT concurrent      */
/* DMA-backed streams (one slot per DMA controller).  The portable        */
/* surface wraps STREAM_BEGIN / STREAM_READ / STREAM_END via the shared    */
/* supervisor singleton.                                                   */
/*                                                                         */
/* Other SoMs: the Zephyr `adc_*` driver class has no portable streaming   */
/* primitive that matches this contract, so alp_adc_stream_open returns    */
/* NULL with last-error = ALP_ERR_NOSUPPORT.  A future polling-thread      */
/* software fallback (timer + ring buffer) lives on the wave-2 roadmap.    */
/* ====================================================================== */

alp_adc_stream_t *alp_adc_stream_open(const alp_adc_stream_config_t *cfg)
{
	alp_z_clear_last_error();

	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

#if ALP_ADC_HAS_BRIDGE_PATH
	if (cfg->channel_id >= 8u) {
		alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
		return NULL;
	}
	if (cfg->sample_rate_hz == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	/* Reserve a backend slot before touching the supervisor.  Returns
     * -1 when both DMA-backed streams are already in use. */
	int slot = bridge_stream_alloc_slot();
	if (slot < 0) {
		alp_z_set_last_error(ALP_ERR_BUSY);
		return NULL;
	}

	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		bridge_stream_free_slot((uint8_t)slot);
		alp_z_set_last_error(s);
		return NULL;
	}
	s = gd32g553_adc_stream_begin(
	    ctx, (uint8_t)slot, (uint8_t)cfg->channel_id, cfg->sample_rate_hz);
	alp_z_v2n_supervisor_release();

	if (s != ALP_OK) {
		bridge_stream_free_slot((uint8_t)slot);
		alp_z_set_last_error(s);
		return NULL;
	}

	struct alp_adc_stream *h = alp_z_adc_stream_pool_acquire();
	if (h == NULL) {
		/* Roll the bridge stream back so the slot is reusable. */
		if (alp_z_v2n_supervisor_acquire(&ctx) == ALP_OK) {
			(void)gd32g553_adc_stream_end(ctx, (uint8_t)slot);
			alp_z_v2n_supervisor_release();
		}
		bridge_stream_free_slot((uint8_t)slot);
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}

	h->via_bridge     = true;
	h->stream_id      = (uint8_t)slot;
	h->channel        = (uint8_t)cfg->channel_id;
	h->channel_id     = cfg->channel_id;
	h->sample_rate_hz = cfg->sample_rate_hz;
	/* Publish LAST, with release semantics: alp_handle_op_enter()'s
	 * acquire-load of `lifecycle` is what a reader pairs with, so
	 * anything that observes OPEN also observes stream_id/channel/rate
	 * above.  The pool hands out a slot whose lifecycle is UNOPENED
	 * (zeroed at acquire, and set back to UNOPENED by close before the
	 * slot is released), so until this store lands a read on this
	 * handle correctly reports ALP_ERR_NOT_READY. */
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
#else
	alp_z_set_last_error(ALP_ERR_NOSUPPORT);
	return NULL;
#endif /* ALP_ADC_HAS_BRIDGE_PATH */
}

/* Body of alp_adc_stream_read_mv(), split out so the op guard around it
 * is a single enter/leave pair rather than one leave per early return --
 * the shape that made the counted region easy to get wrong.  Runs only
 * with the caller's alp_handle_op_enter() count held, which is what
 * keeps `stream` alive across the supervisor acquire below. */
static alp_status_t
adc_stream_read_mv_body(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got)
{
	if (mv == NULL) return ALP_ERR_INVAL;
	if (cap == 0u) return ALP_OK;

#if ALP_ADC_HAS_BRIDGE_PATH
	if (stream->via_bridge) {
		/* Backend caps per-call at GD32G553_BRIDGE_ADC_STREAM_READ_MAX
         * (= 32); callers wanting more loop in their own thread. */
		const uint8_t want     = (cap > (size_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX)
		                             ? (uint8_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX
		                             : (uint8_t)cap;
		uint8_t       got_this = 0u;

		gd32g553_t  *ctx = NULL;
		alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
		if (s != ALP_OK) return s;
		s = gd32g553_adc_stream_read(ctx, stream->stream_id, want, &got_this, mv);
		alp_z_v2n_supervisor_release();
		if (s != ALP_OK) return s;
		*got = got_this;
		return ALP_OK;
	}
#else
	/* No bridge backend on this SoM: the wrapper already validated and
	 * zeroed *got, so nothing here reads the handle. */
	(void)stream;
	(void)got;
#endif
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_adc_stream_read_mv(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got)
{
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0u;
	/* Count the op BEFORE reading any field of *stream.  The read blocks
	 * inside alp_z_v2n_supervisor_acquire() and then issues a GD32G553
	 * transaction keyed on stream->stream_id; an unguarded check would
	 * let alp_adc_stream_close() free the slot and a third thread's
	 * alp_adc_stream_open() re-own it during that window, so the bridge
	 * read would be issued against the NEW owner's stream_id and land
	 * that channel's samples in this caller's buffer with ALP_OK.  #1634 */
	if (stream == NULL || !alp_handle_op_enter(&stream->lifecycle, &stream->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	const alp_status_t rc = adc_stream_read_mv_body(stream, mv, cap, got);
	alp_handle_op_leave(&stream->active_ops);
	return rc;
}

void alp_adc_stream_close(alp_adc_stream_t *stream)
{
	if (stream == NULL) return;
	/* CAS OPEN -> CLOSING, then sleep-poll until every read that entered
	 * before the CAS has left.  The drain must be the sleeping variant:
	 * a read can sit in alp_z_v2n_supervisor_acquire() for up to
	 * CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS, and a closer that
	 * busy-spun there would never yield the core to a lower-priority
	 * reader on Zephyr's preemptive scheduler (issue #1114).
	 *
	 * Also makes close idempotent: a second close loses the CAS and
	 * no-ops, so bridge_stream_free_slot() below cannot release one
	 * bridge stream slot twice and hand it to an unrelated opener. */
	if (!alp_handle_begin_close_blocking(&stream->lifecycle, &stream->active_ops)) {
		return;
	}
#if ALP_ADC_HAS_BRIDGE_PATH
	if (stream->via_bridge) {
		gd32g553_t *ctx = NULL;
		if (alp_z_v2n_supervisor_acquire(&ctx) == ALP_OK) {
			(void)gd32g553_adc_stream_end(ctx, stream->stream_id);
			alp_z_v2n_supervisor_release();
		}
		bridge_stream_free_slot(stream->stream_id);
	}
#endif
	/* UNOPENED before the slot goes back to the pool, so the next
	 * claimer inherits a lifecycle that gates reads off until its own
	 * open publishes OPEN. */
	alp_lifecycle_set(&stream->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_z_adc_stream_pool_release(stream);
}

/* ====================================================================== */
/* Streaming ADC with DSP pipeline (wave-2)                                */
/*                                                                         */
/* alp_adc_filter_t composes alp_adc_stream_t + alp_dsp_chain_t under one  */
/* caller-facing handle.  The chain runs on the host today; the GD32-side */
/* bridge-offload path (CMD_ADC_STREAM_CONFIGURE_DSP, opcode 0x36 reserved */
/* in v0.5.0) lands in v0.5.x once the wire payload format finalises.  On */
/* SoMs without a streaming ADC backend the open returns NULL with        */
/* last-error = ALP_ERR_NOSUPPORT, same as alp_adc_stream_open.            */
/* ====================================================================== */

/* The filter impl needs the DSP chain machinery + a streaming ADC
 * backend.  When either is absent, fall back to NOSUPPORT stubs --
 * the symbols are exported unconditionally so apps linking against
 * <alp/adc.h> stay link-clean. */
#if defined(CONFIG_ALP_SDK_DSP) && ALP_ADC_HAS_BRIDGE_PATH
#define ALP_ADC_HAS_FILTER_PATH 1
#else
#define ALP_ADC_HAS_FILTER_PATH 0
#endif

#if ALP_ADC_HAS_FILTER_PATH

#define ALP_ADC_FILTER_POOL_SIZE 2u

struct alp_adc_filter {
	alp_adc_stream_t *stream;
	alp_dsp_chain_t  *chain;
	/* Same open/op/close guard as struct alp_adc_stream (issue #1634):
	 * a filter read reaches the GD32G553 through alp_adc_stream_read_mv()
	 * on `stream`, so it inherits that call's blocking window, and
	 * alp_adc_filter_close() below closes `stream` and `chain` out from
	 * under it.  Guarding the stream alone is not enough -- the filter
	 * slot itself is pooled and recycled, and a reader that got past a
	 * bare in_use check would go on to dereference filter->chain after
	 * alp_adc_filter_pool_release() nulled it.
	 *
	 * lifecycle/active_ops before in_use: the layout convention shared
	 * with every other guarded handle (see struct alp_counter). */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

static struct alp_adc_filter alp_adc_filter_pool[ALP_ADC_FILTER_POOL_SIZE];

/* issue #1115 round-2 dev review: this used to be a plain
 * `if (!in_use) return &slot;` scan that returned a pointer WITHOUT
 * claiming it -- in_use was only set at the very end of
 * alp_adc_filter_open() below, so the whole open() body (DSP chain
 * open + ADC stream open) was a TOCTOU window a second concurrent
 * open() could scan into and get the same slot back. Same shape as
 * dsp/sw_fallback.c's acquire_be_slot(): claim atomically here so a
 * second opener can never observe this slot as free again until it is
 * released. */
static struct alp_adc_filter *alp_adc_filter_pool_acquire(void)
{
	for (size_t i = 0u; i < ALP_ADC_FILTER_POOL_SIZE; i++) {
		if (alp_slot_try_claim(&alp_adc_filter_pool[i].in_use)) {
			return &alp_adc_filter_pool[i];
		}
	}
	return NULL;
}

static void alp_adc_filter_pool_release(struct alp_adc_filter *f)
{
	if (f == NULL) return;
	f->stream = NULL;
	f->chain  = NULL;
	alp_slot_release(&f->in_use);
}

alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
	alp_z_clear_last_error();

	if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	struct alp_adc_filter *f = alp_adc_filter_pool_acquire();
	if (f == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}

	/* Open the DSP chain first.  Validation rejects FFT-terminated
     * chains via the apply_samples probe below; an FFT chain returns
     * ALP_ERR_NOSUPPORT and we surface that as ALP_ERR_INVAL because
     * the caller used the wrong open() entry point. */
	alp_dsp_chain_t *chain = alp_dsp_chain_open(cfg->stages, cfg->n_stages);
	if (chain == NULL) {
		/* alp_last_error already stamped by alp_dsp_chain_open. */
		alp_adc_filter_pool_release(f);
		return NULL;
	}
	int16_t      probe_in  = 0;
	int16_t      probe_out = 0;
	size_t       probe_got = 0u;
	alp_status_t s = alp_dsp_chain_apply_samples(chain, &probe_in, 1u, &probe_out, 1u, &probe_got);
	if (s == ALP_ERR_NOSUPPORT) {
		/* Caller passed an FFT-terminated chain. */
		alp_dsp_chain_close(chain);
		alp_adc_filter_pool_release(f);
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	/* Open the underlying stream. */
	const alp_adc_stream_config_t scfg = {
		.channel_id     = cfg->channel_id,
		.sample_rate_hz = cfg->sample_rate_hz,
	};
	alp_adc_stream_t *stream = alp_adc_stream_open(&scfg);
	if (stream == NULL) {
		/* alp_last_error stamped by alp_adc_stream_open. */
		alp_dsp_chain_close(chain);
		alp_adc_filter_pool_release(f);
		return NULL;
	}

	/* alp_slot_try_claim() in alp_adc_filter_pool_acquire() already set
	 * in_use=true; no re-store needed (and doing a plain, non-atomic
	 * store here would race a concurrent reader of in_use). */
	f->stream = stream;
	f->chain  = chain;
	/* Release-store LAST: a reader that observes OPEN also observes the
	 * stream/chain pointers above.  Every failure path above releases
	 * the slot with lifecycle still UNOPENED, so a half-built filter is
	 * never readable. */
	alp_lifecycle_set(&f->lifecycle, ALP_HANDLE_LC_OPEN);
	return f;
}

/* Body of alp_adc_filter_read_mv() -- see adc_stream_read_mv_body()'s
 * comment for why the guard wraps a helper instead of threading a leave
 * through each early return.  Runs with the caller's op count held. */
static alp_status_t
adc_filter_read_mv_body(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
	if (out_mv == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (cap == 0u) return ALP_OK;

	/* Drain raw samples in chunks bounded by the backend ceiling. */
	uint16_t     raw[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
	const size_t want =
	    (cap < GD32G553_BRIDGE_ADC_STREAM_READ_MAX) ? cap : GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
	size_t       got_raw = 0u;
	alp_status_t s       = alp_adc_stream_read_mv(filter->stream, raw, want, &got_raw);
	if (s != ALP_OK) return s;
	if (got_raw == 0u) return ALP_OK;

	/* Convert uint16 mV samples (0..3300 typical) to int16. */
	int16_t in_buf[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
	for (size_t i = 0u; i < got_raw; i++) {
		in_buf[i] = (int16_t)raw[i];
	}

	/* Run the chain; chain.apply_samples writes int16 mV out. */
	size_t got_filtered = 0u;
	s = alp_dsp_chain_apply_samples(filter->chain, in_buf, got_raw, out_mv, cap, &got_filtered);
	if (s != ALP_OK) return s;
	*got = got_filtered;
	return ALP_OK;
}

alp_status_t
alp_adc_filter_read_mv(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0u;
	/* Counted before the first field read, so filter->stream and
	 * filter->chain cannot be torn down and the slot re-owned while the
	 * body is blocked in the bridge read below.  #1634 */
	if (filter == NULL || !alp_handle_op_enter(&filter->lifecycle, &filter->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	const alp_status_t rc = adc_filter_read_mv_body(filter, out_mv, cap, got);
	alp_handle_op_leave(&filter->active_ops);
	return rc;
}

void alp_adc_filter_close(alp_adc_filter_t *filter)
{
	if (filter == NULL) return;
	/* Drain in-flight filter reads before closing the stream and chain
	 * they are using -- a reader is parked inside alp_adc_stream_read_mv()
	 * on filter->stream for most of its life.  Sleep-poll, not spin
	 * (#1114), and idempotent on a second close (#1634). */
	if (!alp_handle_begin_close_blocking(&filter->lifecycle, &filter->active_ops)) {
		return;
	}
	if (filter->stream != NULL) {
		alp_adc_stream_close(filter->stream);
	}
	if (filter->chain != NULL) {
		alp_dsp_chain_close(filter->chain);
	}
	alp_lifecycle_set(&filter->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_adc_filter_pool_release(filter);
}

#else /* !ALP_ADC_HAS_FILTER_PATH */

alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg)
{
	alp_z_clear_last_error();
	/* Argument validation first so callers passing bad cfg get a
     * precise INVAL even on SoMs without a bridge backend. */
	if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	alp_z_set_last_error(ALP_ERR_NOSUPPORT);
	return NULL;
}

alp_status_t
alp_adc_filter_read_mv(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap, size_t *got)
{
	/* Mirror the bridge-path contract's pre-checks even when the
     * backend isn't wired -- callers passing a NULL got / NULL
     * handle deserve the precise diagnosis, not a NOSUPPORT smear. */
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0u;
	if (filter == NULL) return ALP_ERR_NOT_READY;
	(void)out_mv;
	(void)cap;
	return ALP_ERR_NOSUPPORT;
}

void alp_adc_filter_close(alp_adc_filter_t *filter)
{
	(void)filter;
}

#endif /* ALP_ADC_HAS_FILTER_PATH */

/* ====================================================================== */
/* alp_adc_spectrum_t -- FFT-terminated chain (wave-2 §2B.1(c))            */
/*                                                                         */
/* Composes alp_adc_stream_t + alp_dsp_chain_t (FFT-terminated) under one  */
/* handle.  Internally accumulates N samples (N = the chain's FFT          */
/* n_points) before running chain.apply_bins for one non-overlapping       */
/* block per read.  On V2N the chain runs on the host today; the GD32-    */
/* side HW-FFT offload path (CMD_ADC_STREAM_CONFIGURE_DSP) lands once the */
/* wire payload format finalises.  Off-V2N or without CONFIG_ALP_SDK_DSP: */
/* surfaces NOSUPPORT after the INVAL pre-checks.                          */
/* ====================================================================== */

#if ALP_ADC_HAS_FILTER_PATH

#define ALP_ADC_SPECTRUM_POOL_SIZE 2u

struct alp_adc_spectrum {
	alp_adc_stream_t    *stream;
	alp_dsp_chain_t     *chain;
	uint16_t             fft_n_points;
	alp_dsp_fft_output_t fft_output;
	size_t               accumulated;
	int16_t              samples[ALP_DSP_MAX_FFT_POINTS];
	/* Same open/op/close guard as the filter above (issue #1634), and
	 * this handle has more to lose: read_bins accumulates ACROSS calls
	 * into `samples`/`accumulated`, and it loops on
	 * alp_adc_stream_read_mv() until a full FFT block is in hand, so an
	 * unguarded reader can be parked in that loop for many bridge
	 * round-trips while a close recycles the slot and a new owner
	 * rewrites fft_n_points -- which is the bound on the `samples`
	 * writes in that loop.
	 *
	 * lifecycle/active_ops before in_use: shared layout convention. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

static struct alp_adc_spectrum alp_adc_spectrum_pool[ALP_ADC_SPECTRUM_POOL_SIZE];

/* issue #1115 round-2 dev review: same unclaimed-acquire TOCTOU shape
 * as alp_adc_filter_pool_acquire() above -- claim atomically at
 * acquisition, not with a plain scan-then-set-at-the-end-of-open(). */
static struct alp_adc_spectrum *alp_adc_spectrum_pool_acquire(void)
{
	for (size_t i = 0u; i < ALP_ADC_SPECTRUM_POOL_SIZE; i++) {
		if (alp_slot_try_claim(&alp_adc_spectrum_pool[i].in_use)) {
			return &alp_adc_spectrum_pool[i];
		}
	}
	return NULL;
}

static void alp_adc_spectrum_pool_release(struct alp_adc_spectrum *s)
{
	if (s == NULL) return;
	s->stream      = NULL;
	s->chain       = NULL;
	s->accumulated = 0u;
	alp_slot_release(&s->in_use);
}

alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
	alp_z_clear_last_error();

	if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	/* The terminal stage MUST be FFT for spectrum_open.  Chain
     * validation itself rejects FFT-not-terminal and WINDOW-not-
     * before-FFT, so probing the caller's last-stage kind here
     * catches the wrong-entry-point case early (before allocating
     * a chain slot). */
	const alp_dsp_stage_t *last = &cfg->stages[cfg->n_stages - 1u];
	if (last->kind != ALP_DSP_STAGE_FFT) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	struct alp_adc_spectrum *s = alp_adc_spectrum_pool_acquire();
	if (s == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}

	alp_dsp_chain_t *chain = alp_dsp_chain_open(cfg->stages, cfg->n_stages);
	if (chain == NULL) {
		/* alp_last_error stamped by alp_dsp_chain_open. */
		alp_adc_spectrum_pool_release(s);
		return NULL;
	}

	const alp_adc_stream_config_t scfg = {
		.channel_id     = cfg->channel_id,
		.sample_rate_hz = cfg->sample_rate_hz,
	};
	alp_adc_stream_t *stream = alp_adc_stream_open(&scfg);
	if (stream == NULL) {
		/* alp_last_error stamped by alp_adc_stream_open. */
		alp_dsp_chain_close(chain);
		alp_adc_spectrum_pool_release(s);
		return NULL;
	}

	/* alp_slot_try_claim() in alp_adc_spectrum_pool_acquire() already set
	 * in_use=true; no re-store needed. */
	s->stream       = stream;
	s->chain        = chain;
	s->fft_n_points = last->u.fft.n_points;
	s->fft_output   = last->u.fft.output_format;
	s->accumulated  = 0u;
	/* Release-store LAST, so a reader that observes OPEN also observes
	 * fft_n_points -- the bound it indexes `samples` with. */
	alp_lifecycle_set(&s->lifecycle, ALP_HANDLE_LC_OPEN);
	return s;
}

/* Body of alp_adc_spectrum_read_bins() -- guarded by its wrapper below;
 * see adc_stream_read_mv_body() for why the split exists. */
static alp_status_t
adc_spectrum_read_bins_body(alp_adc_spectrum_t *spec, float *bins, size_t cap, size_t *got)
{
	if (bins == NULL) return ALP_ERR_INVAL;

	/* Required output element count per block.  Reject early if the
     * caller's buffer can't hold one block. */
	const size_t need = (spec->fft_output == ALP_DSP_FFT_OUTPUT_COMPLEX)
	                        ? (size_t)(2u * spec->fft_n_points)
	                        : (size_t)spec->fft_n_points;
	if (cap < need) return ALP_ERR_OUT_OF_RANGE;

	/* Drain raw mV samples into the accumulator until we have a
     * full FFT block.  If the stream's backend ring is empty, this
     * pass produces no bins (got = 0). */
	while (spec->accumulated < spec->fft_n_points) {
		const size_t want_total = spec->fft_n_points - spec->accumulated;
		const size_t want       = (want_total < GD32G553_BRIDGE_ADC_STREAM_READ_MAX)
		                              ? want_total
		                              : (size_t)GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
		uint16_t     raw[GD32G553_BRIDGE_ADC_STREAM_READ_MAX];
		size_t       got_raw = 0u;
		alp_status_t s       = alp_adc_stream_read_mv(spec->stream, raw, want, &got_raw);
		if (s != ALP_OK) return s;
		if (got_raw == 0u) {
			/* Backend ring was empty; caller should poll again
             * later.  Not an error -- partial accumulation persists
             * across calls. */
			return ALP_OK;
		}
		for (size_t i = 0u; i < got_raw; i++) {
			spec->samples[spec->accumulated + i] = (int16_t)raw[i];
		}
		spec->accumulated += got_raw;
	}

	/* Run the chain over the accumulated block. */
	size_t       got_bins = 0u;
	alp_status_t s        = alp_dsp_chain_apply_bins(
	    spec->chain, spec->samples, spec->fft_n_points, bins, cap, &got_bins);
	/* Reset the accumulator for the next non-overlapping block. */
	spec->accumulated = 0u;
	if (s != ALP_OK) return s;
	*got = got_bins;
	return ALP_OK;
}

alp_status_t
alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap, size_t *got)
{
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0u;
	/* Counted before the first field read: the body's accumulate loop
	 * blocks on the bridge repeatedly and writes spec->samples between
	 * those blocks, so the slot must stay this caller's for the whole
	 * call, not just for the entry check.  #1634 */
	if (spec == NULL || !alp_handle_op_enter(&spec->lifecycle, &spec->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	const alp_status_t rc = adc_spectrum_read_bins_body(spec, bins, cap, got);
	alp_handle_op_leave(&spec->active_ops);
	return rc;
}

void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
	if (spec == NULL) return;
	/* Drain in-flight read_bins calls before closing the stream and
	 * chain they hold, and before alp_adc_spectrum_pool_release() resets
	 * `accumulated`.  Sleep-poll, not spin (#1114); idempotent on a
	 * second close (#1634). */
	if (!alp_handle_begin_close_blocking(&spec->lifecycle, &spec->active_ops)) {
		return;
	}
	if (spec->stream != NULL) {
		alp_adc_stream_close(spec->stream);
	}
	if (spec->chain != NULL) {
		alp_dsp_chain_close(spec->chain);
	}
	alp_lifecycle_set(&spec->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_adc_spectrum_pool_release(spec);
}

#else /* !ALP_ADC_HAS_FILTER_PATH */

alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->stages == NULL || cfg->n_stages == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	/* Mirror the filter-side wrong-entry-point detection so callers
     * get the same INVAL when they pass a filter-terminated chain to
     * the spectrum surface, regardless of whether the bridge path is
     * built. */
	const alp_dsp_stage_t *last = &cfg->stages[cfg->n_stages - 1u];
	if (last->kind != ALP_DSP_STAGE_FFT) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	alp_z_set_last_error(ALP_ERR_NOSUPPORT);
	return NULL;
}

alp_status_t
alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap, size_t *got)
{
	/* Mirror the bridge-path contract's pre-checks for diagnostic
     * fidelity on backends without HW dispatch. */
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0u;
	if (spec == NULL) return ALP_ERR_NOT_READY;
	(void)bins;
	(void)cap;
	return ALP_ERR_NOSUPPORT;
}

void alp_adc_spectrum_close(alp_adc_spectrum_t *spec)
{
	(void)spec;
}

#endif /* ALP_ADC_HAS_FILTER_PATH */
