/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file adc.h
 * @brief ALP SDK analog-to-digital + digital-to-analog converter abstraction.
 *
 * One-shot ADC reads + DAC writes against studio-resolved channel
 * indices, plus DMA-backed continuous-acquisition streaming on SoMs
 * whose backend supports it.  The channel-config side (resolution,
 * gain, reference, acquisition time) lives in devicetree on the
 * Zephyr backend; this header only exposes the runtime knobs apps
 * actually tune.
 *
 * Backends:
 *   - Zephyr   : `adc_*` / `dac_*` driver classes.  On the V2N family
 *                (V2N + V2N-M1, both of which carry the GD32G553
 *                supervisor MCU) the SDK routes both through the GD32
 *                IO MCU bridge (per the 2026-05-12 hardware decision
 *                — see `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`).
 *   - Yocto    : Industrial I/O sysfs (`/sys/bus/iio/devices/`).
 *   - Baremetal: vendor HAL ADC sequencer + DAC channel registers.
 *
 * Typical usage (one-shot):
 * @code
 *     alp_adc_t *th = alp_adc_open(&(alp_adc_config_t){
 *         .channel_id = 0,
 *         .resolution_bits = 12,
 *         .reference  = ALP_ADC_REF_INTERNAL,
 *     });
 *     int32_t uv = 0;
 *     alp_adc_read_uv(th, &uv);   // 0..3300000 µV typical
 *
 *     alp_dac_t *out = alp_dac_open(&(alp_dac_config_t){
 *         .channel_id = ALP_E1M_DAC0,
 *         .initial_mv = 0u,
 *     });
 *     alp_dac_write_mv(out, 1650u);   // mid-rail on a 3.3 V reference
 *     alp_dac_close(out);
 * @endcode
 *
 * Typical usage (streaming, V2N family only today):
 * @code
 *     alp_adc_stream_t *s = alp_adc_stream_open(&(alp_adc_stream_config_t){
 *         .channel_id     = 0,
 *         .sample_rate_hz = 100000,
 *     });
 *     uint16_t buf[32];
 *     size_t got = 0;
 *     alp_adc_stream_read(s, buf, ARRAY_SIZE(buf), &got);  // drain ring
 *     ...
 *     alp_adc_stream_close(s);
 * @endcode
 */

#ifndef ALP_ADC_H
#define ALP_ADC_H

#include <stddef.h>
#include <stdint.h>

#include "alp/dsp.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reference voltage source. */
typedef enum {
    ALP_ADC_REF_INTERNAL   = 0,    /**< On-die bandgap reference. */
    ALP_ADC_REF_EXTERNAL_0 = 1,    /**< External pin pair 0 (VREF+ / VREF-). */
    ALP_ADC_REF_EXTERNAL_1 = 2,    /**< Secondary external reference where present. */
    ALP_ADC_REF_VDD        = 3     /**< Use VDD as the reference. */
} alp_adc_ref_t;

/** Opaque ADC channel handle.  Allocate via @ref alp_adc_open. */
typedef struct alp_adc alp_adc_t;

/** Configuration passed to @ref alp_adc_open. */
typedef struct {
    uint32_t      channel_id;      /**< Studio-resolved ADC channel index (0..7). */
    uint8_t       resolution_bits; /**< 8 / 10 / 12 / 14 / 16 typical. 0 = use DT default. */
    uint16_t      acquisition_us;  /**< Sample-and-hold time, microseconds. */
    alp_adc_ref_t reference;
    uint8_t       gain_num; /**< Gain numerator (e.g. 1 for 1/1). */
    uint8_t       gain_den; /**< Gain denominator (e.g. 6 for 1/6). */
    /** Hardware oversampling ratio (1 / 2 / 4 / 8 / 16 / 32 / 64 / 128 / 256).
     *  Backend rounds down to the nearest power-of-two it supports.  0 means
     *  "backend default".  Backends without HW oversampling ignore this
     *  field; the SoC-cap layer documents which SoMs honour it. */
    uint16_t oversampling_ratio;
    /** Extra sample-and-hold cycles at the ADC clock.  Backend rounds to
     *  its nearest discrete tap (8 taps on the GD32 IO MCU; vendor-defined
     *  elsewhere).  0 means "backend default".  Mutually independent from
     *  @c acquisition_us -- @c acquisition_us is a portable time-domain
     *  expression; @c sample_cycles is the backend-rounded discrete-tap
     *  expression for callers that already know which tap they want. */
    uint16_t sample_cycles;
} alp_adc_config_t;

/**
 * @brief Acquire and configure an ADC channel.
 *
 * Resolves @p cfg->channel_id via the `alp-adc<N>` devicetree alias,
 * applies the channel-config registers, and returns a handle ready
 * for one-shot reads.  When the backend supports the optional tuning
 * fields (@c oversampling_ratio, @c sample_cycles, non-default
 * @c resolution_bits) and any of them are non-zero, the SDK pushes
 * them to the channel before returning the handle.  For DMA-backed
 * continuous acquisition use @ref alp_adc_stream_open.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id < 8.
 * @return Open handle on success, or NULL if the channel can't be
 *         resolved, configured, or the pool is exhausted.
 */
alp_adc_t   *alp_adc_open(const alp_adc_config_t *cfg);

/**
 * @brief One-shot read returning the raw conversion result.
 *
 * The raw value is the ADC's native code in the configured
 * resolution.  No reference / gain scaling is applied — use
 * @ref alp_adc_read_uv if you want microvolts.
 *
 * @param[in]  adc      Handle from @ref alp_adc_open.
 * @param[out] raw_out  Receives the raw code.  Sign-extended on
 *                      ADCs with differential inputs.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_adc_read_raw(alp_adc_t *adc, int32_t *raw_out);

/**
 * @brief One-shot read converted to microvolts.
 *
 * Applies the configured reference voltage and gain to the raw code.
 * Convertible to engineering units by the caller via the sensor's
 * transfer function.
 *
 * @param[in]  adc     Handle from @ref alp_adc_open.
 * @param[out] uv_out  Receives the signed microvolt reading.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_adc_read_uv(alp_adc_t *adc, int32_t *uv_out);

/**
 * @brief Release an ADC handle back to the pool.
 *
 * Does not power-down the underlying converter; that's a SoC-wide
 * decision the studio's power-management layer handles.  NULL is a
 * no-op.
 *
 * @param[in] adc  Handle from @ref alp_adc_open, or NULL.
 */
void         alp_adc_close(alp_adc_t *adc);

/* ================================================================== */
/* Streaming ADC acquisition                                           */
/* ================================================================== */

/** Opaque streaming-ADC handle.  Allocate via @ref alp_adc_stream_open. */
typedef struct alp_adc_stream alp_adc_stream_t;

/** Configuration passed to @ref alp_adc_stream_open. */
typedef struct {
    uint32_t channel_id;     /**< Studio-resolved ADC channel index (0..7). */
    uint32_t sample_rate_hz; /**< Target sample rate (Hz).  Backend rounds
                                    *  down to its nearest achievable value and
                                    *  caps at the active SoM's hardware ceiling
                                    *  (~1.5 MSps on V2N at 12-bit). */
} alp_adc_stream_config_t;

/**
 * @brief Open a DMA-backed continuous-conversion stream on an ADC channel.
 *
 * The SDK reserves one of the backend's internal stream slots and
 * binds it to @p cfg->channel_id at @p cfg->sample_rate_hz.  On the
 * V2N family (V2N + V2N-M1) up to two slots are available concurrently
 * (one per GD32 DMA controller); on SoMs without a comparable HW
 * streaming backend the call returns NULL with last-error =
 * @ref ALP_ERR_NOSUPPORT.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL with @ref alp_last_error set to
 *         one of:
 *         - @ref ALP_ERR_INVAL on NULL cfg or zero sample-rate,
 *         - @ref ALP_ERR_OUT_OF_RANGE on channel_id >= 8,
 *         - @ref ALP_ERR_NOSUPPORT on SoMs without a streaming backend,
 *         - @ref ALP_ERR_NOT_READY when the backend transport is not
 *           configured (e.g. V2N supervisor with no bus ids set),
 *         - @ref ALP_ERR_BUSY when all stream slots are already in use,
 *         - @ref ALP_ERR_NOMEM when the handle pool is exhausted.
 */
alp_adc_stream_t *alp_adc_stream_open(const alp_adc_stream_config_t *cfg);

/**
 * @brief Drain pending samples from the stream's backend ring.
 *
 * The backend's per-call sample ceiling is hardware-defined (32 on
 * the V2N family's GD32 IO MCU); callers wanting more than that per
 * logical batch must loop.  A return value of @ref ALP_OK with
 * @p *got == 0 means the backend ring was empty since the last read
 * -- not an error.
 *
 * @param[in]  stream  Handle from @ref alp_adc_stream_open.
 * @param[out] mv      Caller buffer for the sample values (mV, u16).
 *                     Length >= @p cap.
 * @param[in]  cap     Capacity of @p mv (samples, not bytes).
 * @param[out] got     Number of samples actually copied; may be 0.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (stream closed or backend not
 *         reachable) / ALP_ERR_INVAL (NULL out pointers) /
 *         ALP_ERR_BUSY (backend ring overran -- poll faster) /
 *         ALP_ERR_IO.
 */
alp_status_t alp_adc_stream_read(alp_adc_stream_t *stream, uint16_t *mv, size_t cap, size_t *got);

/**
 * @brief Close the stream, releasing its backend slot + handle.
 *
 * Issues the backend's stream-end so the DMA channel + ring buffer
 * are freed before the handle returns to the pool.  NULL is a no-op.
 *
 * @param[in] stream  Handle from @ref alp_adc_stream_open, or NULL.
 */
void alp_adc_stream_close(alp_adc_stream_t *stream);

/* ================================================================== */
/* Streaming ADC with DSP pipeline (wave-2)                            */
/*                                                                     */
/* alp_adc_filter_t composes a streaming ADC source with a             */
/* filter-terminated DSP chain (FIR / IIR, optionally cascaded) so     */
/* the application receives FILTERED int16 mV samples without writing  */
/* the DSP plumbing itself.  The FFT-terminated sibling                */
/* alp_adc_spectrum_t lands in the v0.5.x (c) sub-commit alongside the */
/* spectrum-side of the wave-2 pipeline.                               */
/*                                                                     */
/* On SoMs with a HW DSP block on the bridge (V2N family today; AEN    */
/* via the wave-2 GD32-side wire-format extension landing v0.5.x), the */
/* chain runs on the bridge so raw samples never traverse the wire.   */
/* Today (v0.5.0): the host runs the chain over the existing          */
/* alp_adc_stream_t source.  SoMs without a streaming ADC backend at  */
/* all return NULL with last-error = ALP_ERR_NOSUPPORT, same as       */
/* @ref alp_adc_stream_open.                                            */
/* ================================================================== */

/** Opaque filter-stream handle.  Allocate via @ref alp_adc_filter_open. */
typedef struct alp_adc_filter alp_adc_filter_t;

/** Configuration passed to @ref alp_adc_filter_open. */
typedef struct {
    uint32_t channel_id;     /**< ADC channel index (0..7). */
    uint32_t sample_rate_hz; /**< Target acquisition rate (Hz). */
    /** DSP stages, filter-terminated (no FFT).  Copied into the
     *  internal chain at open time; caller may free immediately. */
    const alp_dsp_stage_t *stages;
    /** Stage count (1..@ref ALP_DSP_MAX_STAGES). */
    size_t n_stages;
} alp_adc_filter_config_t;

/**
 * @brief Open a streaming ADC source with a FIR/IIR DSP chain applied.
 *
 * Composes @ref alp_adc_stream_open + @ref alp_dsp_chain_open under
 * one handle.  The chain MUST NOT contain an FFT stage; spectral
 * output lands in @c alp_adc_spectrum_open (v0.5.x).
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 *
 * @return Handle on success; NULL with @ref alp_last_error set to:
 *         - @ref ALP_ERR_INVAL on NULL cfg, NULL stages, n_stages == 0,
 *           or an FFT-terminated chain.
 *         - @ref ALP_ERR_OUT_OF_RANGE on channel_id >= 8 or any
 *           per-stage bound violation.
 *         - @ref ALP_ERR_NOSUPPORT on SoMs without a streaming ADC.
 *         - @ref ALP_ERR_NOT_READY when the backend transport is not
 *           configured.
 *         - @ref ALP_ERR_BUSY when all stream slots are in use.
 *         - @ref ALP_ERR_NOMEM when the handle pool or chain pool is
 *           exhausted.
 */
alp_adc_filter_t *alp_adc_filter_open(const alp_adc_filter_config_t *cfg);

/**
 * @brief Drain filtered samples from the stream.
 *
 * Polls the underlying @ref alp_adc_stream_t, converts the raw
 * uint16 mV samples to int16, runs them through the DSP chain, and
 * writes the result into the caller's buffer.  Per-call sample
 * ceiling matches the backend stream
 * (@c GD32G553_BRIDGE_ADC_STREAM_READ_MAX on V2N's GD32 bridge);
 * callers wanting more than that per batch must loop.
 *
 * @param[in]  filter  Handle from @ref alp_adc_filter_open.
 * @param[out] out_mv  Caller buffer for filtered samples (int16 mV).
 * @param[in]  cap     Capacity of @p out_mv (samples).
 * @param[out] got     Number of samples actually written; may be 0.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_BUSY /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_adc_filter_read(alp_adc_filter_t *filter, int16_t *out_mv, size_t cap,
                                 size_t *got);

/**
 * @brief Close a filter handle.  NULL is a no-op.
 *
 * Releases the internal stream slot + DSP chain.  After this call
 * @p filter is invalid.
 */
void alp_adc_filter_close(alp_adc_filter_t *filter);

/** Opaque spectrum-stream handle.  Allocate via @ref alp_adc_spectrum_open. */
typedef struct alp_adc_spectrum alp_adc_spectrum_t;

/** Configuration passed to @ref alp_adc_spectrum_open. */
typedef struct {
    uint32_t channel_id;     /**< ADC channel index (0..7). */
    uint32_t sample_rate_hz; /**< Target acquisition rate (Hz). */
    /** DSP stages, FFT-terminated (optional WINDOW immediately
     *  before FFT).  Copied at open time. */
    const alp_dsp_stage_t *stages;
    /** Stage count (1..@ref ALP_DSP_MAX_STAGES). */
    size_t n_stages;
} alp_adc_spectrum_config_t;

/**
 * @brief Open a streaming ADC source with an FFT-terminated DSP chain.
 *
 * Composes @ref alp_adc_stream_open + @ref alp_dsp_chain_open under
 * one handle for spectral analysis.  The chain MUST end with
 * @ref ALP_DSP_STAGE_FFT; a @ref ALP_DSP_STAGE_WINDOW immediately
 * preceding the FFT is optional but recommended for narrow-spectral
 * analysis.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 *
 * @return Handle on success, or NULL with @ref alp_last_error set
 *         to ALP_ERR_INVAL (NULL cfg, NULL stages, n_stages==0, or
 *         a non-FFT-terminated chain) / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_NOT_READY / ALP_ERR_BUSY /
 *         ALP_ERR_NOMEM (same gates as @ref alp_adc_filter_open).
 */
alp_adc_spectrum_t *alp_adc_spectrum_open(const alp_adc_spectrum_config_t *cfg);

/**
 * @brief Read one block of FFT bins from the spectrum stream.
 *
 * Internally buffers up to N raw samples (N = the chain's FFT
 * n_points) before running the chain.  Until N samples have
 * accumulated, returns @ref ALP_OK with @p *got = 0 -- not an
 * error.  Each successful block resets the accumulator (no
 * overlap; overlapping windows land in a later revision).
 *
 * Output element count when a block is emitted:
 *  - @ref ALP_DSP_FFT_OUTPUT_COMPLEX:   2 * N (interleaved re,im pairs).
 *  - @ref ALP_DSP_FFT_OUTPUT_MAGNITUDE: N.
 *
 * @param[in]  spec   Handle from @ref alp_adc_spectrum_open.
 * @param[out] bins   Caller buffer for FFT bins (float).
 * @param[in]  cap    Capacity of @p bins (elements).
 * @param[out] got    Number of bin elements written (0 if buffering).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_OUT_OF_RANGE (cap < required) / ALP_ERR_IO /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_adc_spectrum_read_bins(alp_adc_spectrum_t *spec, float *bins, size_t cap,
                                        size_t *got);

/** Close a spectrum handle.  NULL is a no-op. */
void alp_adc_spectrum_close(alp_adc_spectrum_t *spec);

/* ================================================================== */
/* DAC                                                                 */
/* ================================================================== */

/** Opaque DAC channel handle.  Allocate via @ref alp_dac_open. */
typedef struct alp_dac alp_dac_t;

/** Configuration passed to @ref alp_dac_open. */
typedef struct {
    uint32_t      channel_id;       /**< Studio-resolved DAC channel index (ALP_E1M_DAC0..DAC1). */
    uint16_t      initial_mv;       /**< Initial output in millivolts; 0 = ground. */
} alp_dac_config_t;

/**
 * @brief Acquire and initialise a DAC channel at @c initial_mv.
 *
 * Resolves @p cfg->channel_id to the underlying converter (a Zephyr
 * dac_* device on most SoMs; the GD32 IO MCU bridge on V2N), borrows
 * a free handle from the SDK's pool, and primes the output.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL; @c channel_id must
 *                 be < @ref ALP_E1M_DAC_COUNT and resolvable on the
 *                 active SoM.
 * @return Open handle on success, or NULL on any of:
 *         - @p cfg is NULL
 *         - @c channel_id out of range or unresolvable
 *         - underlying converter not ready
 *         - handle pool exhausted
 */
alp_dac_t   *alp_dac_open(const alp_dac_config_t *cfg);

/**
 * @brief Set the DAC output in millivolts.
 *
 * The backend saturates at the DAC's reference rail (3.3 V typical
 * on the GD32 / V2N route) and rounds to the converter's hardware-
 * achievable resolution (12-bit on the GD32 DAC).
 *
 * @param[in] dac  Handle from @ref alp_dac_open.
 * @param[in] mv   Requested output in millivolts.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_dac_write_mv(alp_dac_t *dac, uint16_t mv);

/**
 * @brief Read back the currently-programmed DAC output in millivolts.
 *
 * Useful for verification + closed-loop monitors where the rounded
 * hardware setpoint may differ from the requested value.
 *
 * @param[in]  dac     Handle from @ref alp_dac_open.
 * @param[out] mv_out  Receives the programmed output (mV).
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_dac_read_mv(alp_dac_t *dac, uint16_t *mv_out);

/**
 * @brief Release a DAC handle back to the pool.
 *
 * Does not power-down the converter or alter the output level — that
 * stays at the last-programmed value until the next open() reprograms
 * it.  NULL is a no-op.
 *
 * @param[in] dac  Handle from @ref alp_dac_open, or NULL.
 */
void         alp_dac_close(alp_dac_t *dac);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_ADC_H */
