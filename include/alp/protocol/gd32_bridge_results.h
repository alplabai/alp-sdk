/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gd32_bridge_results.h
 * @brief Linux-readable result record of the CM33 GD32-bridge test apps.
 *
 * The CM33 test apps @c v2n-gd32-bridge-functional and
 * @c v2n-gd32-bridge-hil-soak have no console on the V2N / V2M SoMs and
 * keep their verdict in CM33 secure SRAM0, which only a CM33 J-Link can
 * read.  Linux cannot: it sees neither SRAM0 nor a console.  This header
 * defines a compact, versioned copy of the verdict that lives in the one
 * place Linux CAN read -- the OpenAMP `rsctbl` window (A55 `0x4F700000`,
 * CM33-NS `0x9F700000`), right below the liveness beacon the provisioning
 * functional test (`cm33_running`) already reads:
 *
 *     rsctbl + 0x000 .. 0xEFF   resource table (unused by these apps)
 *     rsctbl + 0xF00 .. 0xF4F   THIS RECORD  (20 words, 80 bytes)
 *     rsctbl + 0xF50 .. 0xFEF   free
 *     rsctbl + 0xFF0 .. 0xFFF   liveness beacon (magic, kind, heartbeat)
 *
 * No new carve-out: the window is already reserved `no-map` for Linux and
 * declared once in the SoC metadata.  These apps do not run OpenAMP, so the
 * low end of the page holds no resource table.
 *
 * All words are little-endian u32.  The record is written with a seqlock:
 * the writer makes @c seq odd while it updates and even when done, so a
 * reader that sees an odd @c seq, or a @c seq that changed across its
 * read, simply reads again.  The record keeps its contents across a CM33
 * reset, so ALP_GD32_RESULTS_MAGIC is cleared first and written LAST at
 * init: a reader never pairs a fresh magic with stale counters.
 */

#ifndef ALP_PROTOCOL_GD32_BRIDGE_RESULTS_H_
#define ALP_PROTOCOL_GD32_BRIDGE_RESULTS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte offset of the record inside the `rsctbl` window. */
#define ALP_GD32_RESULTS_OFFSET 0xF00u
/** Record magic, ASCII "GD3R" read as a big-endian number. */
#define ALP_GD32_RESULTS_MAGIC 0x47443352u
/** Layout version; bumped when a word moves or changes meaning. */
#define ALP_GD32_RESULTS_LAYOUT 1u
/** Record size in 32-bit words. */
#define ALP_GD32_RESULTS_WORDS 20u

/** @c kind: single-pass functional app. */
#define ALP_GD32_RESULTS_KIND_FUNCTIONAL 1u
/** @c kind: HIL soak app. */
#define ALP_GD32_RESULTS_KIND_SOAK 2u

/** @c state 0: the app started but the bridge link is not up yet (init retries). */
/** @c state: tests running (functional) / settling past the host boot (soak). */
#define ALP_GD32_RESULTS_STATE_RUNNING 1u
/** @c state: functional tests done, PWM staircase running / soak cycling. */
#define ALP_GD32_RESULTS_STATE_DONE 2u
/** @c state: the SPI bus or the bridge link never came up. */
#define ALP_GD32_RESULTS_STATE_NO_LINK 0xDEADu

/** @c flags bit: the ATTN link feature was granted. */
#define ALP_GD32_RESULTS_FLAG_ATTN_GRANTED (1u << 0)
/** @c flags bit: ATTN is granted and passed its self-test (data-ready line in use). */
#define ALP_GD32_RESULTS_FLAG_ATTN_ACTIVE (1u << 1)
/** @c flags bit: ATTN was granted but failed its self-test, so the link fell back to the
 *  0.14 drain rule. */
#define ALP_GD32_RESULTS_FLAG_ATTN_FALLBACK (1u << 2)
/** @c flags bit: the last BATCH test passed on a link that granted BATCH. */
#define ALP_GD32_RESULTS_FLAG_BATCH_OK (1u << 3)
/** @c flags bit: the last ADC_STREAM2 test passed on a link that granted it. */
#define ALP_GD32_RESULTS_FLAG_STREAM2_OK (1u << 4)

/**
 * @brief The record, exactly as it sits in memory (word 0 first).
 *
 * Counters are cumulative since the app started.  Fields a given app does
 * not produce stay 0 (the soak leaves nothing of the functional table, the
 * functional app leaves the soak counters).
 */
typedef struct {
	uint32_t magic;          /**< @ref ALP_GD32_RESULTS_MAGIC; valid only when non-zero. */
	uint32_t layout;         /**< @ref ALP_GD32_RESULTS_LAYOUT. */
	uint32_t seq;            /**< Seqlock: odd while the writer updates. */
	uint32_t kind;           /**< `ALP_GD32_RESULTS_KIND_*`. */
	uint32_t state;          /**< `ALP_GD32_RESULTS_STATE_*`. */
	uint32_t tests_pass;     /**< Tests that passed (soak: test executions). */
	uint32_t tests_fail;     /**< Tests that failed (soak: test executions). */
	uint32_t tests_skip;     /**< Self-gating tests skipped because the feature was not granted. */
	uint32_t fw_version;     /**< Bridge firmware `major << 16 | minor << 8 | patch`. */
	uint32_t features;       /**< Negotiated `GD32G553_LINK_FEAT_*` word (granted). */
	uint32_t max_payload;    /**< Negotiated SPI payload ceiling in bytes (65 or 252). */
	uint32_t flags;          /**< `ALP_GD32_RESULTS_FLAG_*`. */
	uint32_t read2_first;    /**< `first_index` of the last ADC_STREAM2 READ2 reply. */
	uint32_t read2_dropped;  /**< Samples the bridge reported dropped, summed over READ2 replies. */
	uint32_t read2_gaps;     /**< READ2 replies that broke the host-side continuity invariant. */
	uint32_t soak_cycles;    /**< Soak: completed cycles. */
	uint32_t soak_errors;    /**< Soak: failed test executions (any status). */
	uint32_t soak_timeouts;  /**< Soak: lost ATTN edges (fallbacks to the 0.14 drain rule). */
	uint32_t soak_elapsed_s; /**< Soak: seconds since the first cycle began. */
	uint32_t reserved;       /**< 0. */
} alp_gd32_results_t;

_Static_assert(sizeof(alp_gd32_results_t) == ALP_GD32_RESULTS_WORDS * 4u,
               "alp_gd32_results_t must stay 20 words");

/** Byte offset of the liveness beacon inside the `rsctbl` window. */
#define ALP_GD32_RESULTS_BEACON_OFFSET 0xFF0u
/** Beacon magic the provisioning `cm33_running` check reads. */
#define ALP_GD32_RESULTS_BEACON_MAGIC 0xA10D0683u
/** Beacon image kind the `cm33_running` check accepts (the idle-shim value, 0x100). */
#define ALP_GD32_RESULTS_BEACON_KIND 0x100u

/**
 * @brief Clear the record and publish an empty one for @p kind.
 *
 * Call once at the start of `main()`, before any test runs.
 *
 * @param[in] window Base of the `rsctbl` window as the CM33 sees it
 *                   (`DT_REG_ADDR(DT_NODELABEL(rsctbl))`).
 * @param[in] kind   `ALP_GD32_RESULTS_KIND_*`.
 */
static inline void alp_gd32_results_init(void *window, uint32_t kind)
{
	volatile uint32_t *w = (volatile uint32_t *)((uintptr_t)window + ALP_GD32_RESULTS_OFFSET);

	w[offsetof(alp_gd32_results_t, magic) / 4u] = 0u;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (unsigned i = 1u; i < ALP_GD32_RESULTS_WORDS; ++i) {
		w[i] = 0u;
	}
	w[offsetof(alp_gd32_results_t, layout) / 4u] = ALP_GD32_RESULTS_LAYOUT;
	w[offsetof(alp_gd32_results_t, kind) / 4u]   = kind;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	w[0] = ALP_GD32_RESULTS_MAGIC; /* last: the record is valid from here on */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/**
 * @brief Publish a new snapshot of the record under the seqlock.
 *
 * @p snap supplies every word from @c state on; @c magic, @c layout,
 * @c seq and @c kind stay as @ref alp_gd32_results_init left them.
 *
 * @param[in] window Same base passed to @ref alp_gd32_results_init.
 * @param[in] snap   The application's current counters.
 */
static inline void alp_gd32_results_publish(void *window, const alp_gd32_results_t *snap)
{
	volatile uint32_t *w   = (volatile uint32_t *)((uintptr_t)window + ALP_GD32_RESULTS_OFFSET);
	const uint32_t    *src = (const uint32_t *)snap;
	const unsigned     seq = offsetof(alp_gd32_results_t, seq) / 4u;

	w[seq] = w[seq] + 1u; /* odd: update in flight */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (unsigned i = offsetof(alp_gd32_results_t, state) / 4u; i < ALP_GD32_RESULTS_WORDS; ++i) {
		w[i] = src[i];
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	w[seq] = w[seq] + 1u; /* even: stable */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/**
 * @brief Start the liveness beacon the provisioning `cm33_running` check reads.
 *
 * Same words as `firmware/alp-stock-shim`: magic (written last), image kind
 * @ref ALP_GD32_RESULTS_BEACON_KIND, heartbeat 0.  Pair with
 * @ref alp_gd32_results_beacon_tick once a second.
 *
 * @param[in] window Base of the `rsctbl` window as the CM33 sees it.
 */
static inline void alp_gd32_results_beacon_init(void *window)
{
	volatile uint32_t *b =
	    (volatile uint32_t *)((uintptr_t)window + ALP_GD32_RESULTS_BEACON_OFFSET);

	b[2] = 0u;
	b[1] = ALP_GD32_RESULTS_BEACON_KIND;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	b[0] = ALP_GD32_RESULTS_BEACON_MAGIC;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/**
 * @brief Advance the beacon heartbeat by one.
 *
 * Call about once a second from a context that never blocks on the bridge
 * (a k_timer expiry), so the heartbeat keeps advancing while the app waits
 * on a slow SPI transaction.  `cm33_running` accepts 1..4 counts in 2 s.
 *
 * @param[in] window Base of the `rsctbl` window as the CM33 sees it.
 */
static inline void alp_gd32_results_beacon_tick(void *window)
{
	volatile uint32_t *b =
	    (volatile uint32_t *)((uintptr_t)window + ALP_GD32_RESULTS_BEACON_OFFSET);

	b[2] = b[2] + 1u;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#ifdef __cplusplus
}
#endif

#endif /* ALP_PROTOCOL_GD32_BRIDGE_RESULTS_H_ */
