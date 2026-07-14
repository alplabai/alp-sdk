/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file adc.h
 * @brief Injection API for the ADC test double (priority-255 backend).
 *
 * `src/backends/adc/testing_drv.c` registers a `silicon_ref="*"`
 * backend at priority 255 (see @ref ALP_BACKEND_REGISTER), so with
 * `CONFIG_ALP_SDK_TESTING_ADC=y` it wins @ref alp_backend_select for
 * every channel id and the portable `<alp/adc.h>` `alp_adc_*` API
 * rides on it transparently -- no real or emulated ADC controller
 * needed.  This header is the test-side control surface: it queues
 * raw conversion codes a channel reads back via @ref alp_adc_read_raw
 * (and, through the dispatcher's raw -> uV math in
 * `src/adc_dispatch.c`, @ref alp_adc_read_uv).
 *
 * Every function keys off the same @p channel_id the app passes to
 * @ref alp_adc_open, and both injectors are create-on-first-touch --
 * a test may queue a raw sample or arm a fault BEFORE the app opens
 * the channel, so power-on / cold-read scenarios are expressible.
 *
 * @par The ADC-specific must -- raw -> uV conversion:
 *      Unlike the GPIO/UART doubles, `alp_adc_read_uv()` does real
 *      arithmetic on top of this double's raw samples
 *      (`raw * reference_uv / ((1 << resolution_bits) - 1)`,
 *      `src/adc_dispatch.c`).  `testing_drv.c`'s `open()` therefore
 *      sets `state->reference_uv` / `state->resolution_bits` from
 *      @p cfg (falling back to a sane default -- 3.3 V / 12-bit --
 *      when @p cfg leaves them at "use the backend default", i.e.
 *      `resolution_bits == 0`) instead of leaving them at zero: a
 *      zeroed `resolution_bits` would make the dispatcher's
 *      full-scale divisor `(1 << 0) - 1 == 0` and every
 *      @ref alp_adc_read_uv call fail with @ref ALP_ERR_NOT_READY
 *      instead of exercising the conversion this double exists to
 *      test.
 *
 * @par The latch model:
 *      @ref alp_testing_adc_queue_raw appends to a per-channel FIFO
 *      consumed strictly in order by @ref alp_adc_read_raw /
 *      @ref alp_adc_read_uv, one sample per read -- same insertion-
 *      order contract as the UART double's RX queue.  Once the FIFO
 *      runs dry, the LAST value popped from it latches: every further
 *      read keeps returning that same value (like a held ADC input)
 *      instead of failing or reading stale zeros, until either more
 *      samples are queued or @ref alp_testing_reset_all clears the
 *      channel.  A channel that has never had a sample queued (and
 *      has therefore never popped one) latches at 0.
 */

#ifndef ALP_TESTING_ADC_H
#define ALP_TESTING_ADC_H

#include <stddef.h>
#include <stdint.h>

#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Queue raw ADC codes an open (or not-yet-open) channel reads
 *        back, in order, from @ref alp_adc_read_raw / @ref alp_adc_read_uv.
 *
 * Appended to the channel's FIFO behind anything already queued --
 * consumed strictly FIFO, one entry per read, matching a real
 * converter's one-sample-per-conversion cadence.  All-or-nothing: if
 * @p n does not fit in the FIFO's remaining capacity, nothing is
 * queued and @ref ALP_ERR_NOMEM is returned, so a test never has to
 * reason about a partially-applied sequence.
 *
 * @param[in] channel_id  The same id the app passes to @ref alp_adc_open.
 * @param[in] raw         Source codes; copied, so the caller's buffer
 *                         need not outlive this call.  Signed, matching
 *                         @ref alp_adc_read_raw's @c int32_t output.
 * @param[in] n           Sample count.  0 is a no-op that still
 *                         create-on-first-touches the channel.
 *
 * @return ALP_OK on success; ALP_ERR_INVAL if @p raw is NULL with
 *         @p n > 0; ALP_ERR_NOMEM if @p n exceeds the FIFO's
 *         remaining capacity or the instance table is full.
 */
alp_status_t alp_testing_adc_queue_raw(uint32_t channel_id, const int32_t *raw, size_t n);

/**
 * @brief Arm the channel's NEXT read to fail with @p err instead of
 *        returning a sample.
 *
 * Single-shot: consumed by exactly one @ref alp_adc_read_raw /
 * @ref alp_adc_read_uv call (whichever the app makes first), after
 * which the channel resumes popping its FIFO / returning its latched
 * value normally.  Takes precedence over the FIFO -- an armed fault
 * fires even if samples are queued behind it, and firing it does NOT
 * consume or advance past the queued sample it pre-empted.
 *
 * @param[in] channel_id  The same id the app passes to @ref alp_adc_open.
 * @param[in] err         Status the next read returns.  Any
 *                         @ref alp_status_t value; @ref ALP_ERR_IO is
 *                         the typical conversion-fault choice.
 *
 * @return ALP_OK on success; ALP_ERR_NOMEM if the instance table is full.
 */
alp_status_t alp_testing_adc_fail_next(uint32_t channel_id, alp_status_t err);

#ifdef __cplusplus
}
#endif

#endif /* ALP_TESTING_ADC_H */
