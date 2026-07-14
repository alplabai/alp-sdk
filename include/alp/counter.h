/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file counter.h
 * @brief Alp SDK counter / timer + quadrature-encoder abstraction.
 *
 * Two related concepts share this header because they map to the same
 * underlying SoC timer hardware on most E1M targets:
 *
 *   1. **Free-running counter** with an optional alarm callback —
 *      the building block for "wake me up in N microseconds" patterns.
 *   2. **Quadrature decoder** for incremental rotary encoders, mapped
 *      to `ENC0..ENC3` in `<alp/e1m_pinout.h>`.
 *
 * Backends:
 *   - Zephyr   : `counter_*` driver class for timers; `sensor_*`
 *                with `SENSOR_CHAN_ROTATION` for quadrature decode
 *                (Zephyr's QDEC subsystem).
 *   - Yocto    : `/dev/rtc*` for absolute alarms; input subsystem
 *                events for incremental encoders.
 *   - Baremetal: vendor HAL timer + qdec channels.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_COUNTER_H
#define ALP_COUNTER_H

#include <stdint.h>
#include <stdbool.h>

#include <alp/cap_instance.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Free-running counter                                                */
/* ------------------------------------------------------------------ */

/** Opaque counter handle.  Allocate via @ref alp_counter_open. */
typedef struct alp_counter alp_counter_t;

/** Configuration passed to @ref alp_counter_open. */
typedef struct {
	uint32_t counter_id; /**< Studio-resolved counter instance (0..3). */
} alp_counter_config_t;

/**
 * @brief Default-initialize an @ref alp_counter_config_t for counter @p id.
 *
 * The counter has no tunable fields beyond its identity, so the default
 * simply names the instance: @code alp_counter_config_t cfg =
 * ALP_COUNTER_CONFIG_DEFAULT(0); @endcode
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_COUNTER_CONFIG_DEFAULT(id) ((alp_counter_config_t){ .counter_id = (id) })

/**
 * @brief Alarm callback fired when a previously-scheduled deadline
 *        ticks elapse.
 *
 * The callback runs in interrupt context on M-class targets — keep
 * the body short and avoid blocking calls.
 *
 * @param[in] counter  Counter that fired.
 * @param[in] ticks    Counter value at the moment the alarm fired.
 * @param[in] user     Opaque pointer the caller passed into
 *                     @ref alp_counter_set_alarm.
 */
typedef void (*alp_counter_alarm_cb_t)(alp_counter_t *counter, uint32_t ticks, void *user);

/**
 * @brief Acquire a counter handle.
 *
 * Resolves @p cfg->counter_id via the `alp-counter<N>` devicetree
 * alias.  Does not start counting — call @ref alp_counter_start.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL if unresolved / pool empty.
 */
alp_counter_t *alp_counter_open(const alp_counter_config_t *cfg);

/**
 * @brief Start the free-running counter.
 *
 * @param[in] counter  Handle from @ref alp_counter_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_counter_start(alp_counter_t *counter);

/**
 * @brief Stop the counter.  The current value is preserved.
 *
 * @param[in] counter  Handle from @ref alp_counter_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY.
 */
alp_status_t alp_counter_stop(alp_counter_t *counter);

/**
 * @brief Read the current free-running tick count.
 *
 * @param[in]  counter    Handle from @ref alp_counter_open.
 * @param[out] ticks_out  Receives the current value.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_counter_get_value(alp_counter_t *counter, uint32_t *ticks_out);

/**
 * @brief Convert microseconds to native counter ticks (rounded down).
 *
 * @param[in]  counter    Handle from @ref alp_counter_open.
 * @param[in]  us         Microseconds to convert.
 * @param[out] ticks_out  Receives the equivalent tick count.
 * @return ALP_OK on success;
 *         ALP_ERR_NOT_READY if @p counter is closed;
 *         ALP_ERR_INVAL if @p ticks_out is NULL;
 *         ALP_ERR_NOSUPPORT on backends that don't advertise the
 *           counter's tick frequency to the host -- the V2N supervisor
 *           bridge returns this until protocol v0.3's
 *           `CMD_COUNTER_GET_FREQ` opcode lands.
 */
alp_status_t alp_counter_us_to_ticks(alp_counter_t *counter, uint32_t us, uint32_t *ticks_out);

/**
 * @brief Schedule a one-shot callback @p ticks_from_now ticks ahead.
 *
 * Replaces any previously-scheduled alarm.  At most one alarm per
 * handle.
 *
 * @param[in] counter         Handle from @ref alp_counter_open.
 * @param[in] ticks_from_now  Delay in counter ticks.
 * @param[in] cb              Callback to invoke.  Must not be NULL.
 * @param[in] user            Opaque pointer forwarded to @p cb.
 * @return ALP_OK on success;
 *         ALP_ERR_NOT_READY if @p counter is closed;
 *         ALP_ERR_INVAL if @p cb is NULL;
 *         ALP_ERR_BUSY if another alarm is already armed and the
 *           backend doesn't support replacement;
 *         ALP_ERR_NOSUPPORT on backends that have no path for ISR-
 *           context callbacks to reach the host -- the V2N supervisor
 *           bridge always returns this (the GD32 IO MCU has no
 *           interrupt line back to the Renesas host, so alarms
 *           fired in firmware ISR context cannot be relayed across
 *           the bridge in bounded time);
 *         ALP_ERR_IO on a backend failure.
 */
alp_status_t alp_counter_set_alarm(alp_counter_t         *counter,
                                   uint32_t               ticks_from_now,
                                   alp_counter_alarm_cb_t cb,
                                   void                  *user);

/**
 * @brief Cancel a pending alarm.  No-op if no alarm is armed.
 *
 * @param[in] counter  Handle from @ref alp_counter_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY.
 */
alp_status_t alp_counter_cancel_alarm(alp_counter_t *counter);

/**
 * @brief Stop the counter and release the handle.  NULL is a no-op.
 *
 * @param[in] counter  Handle from @ref alp_counter_open, or NULL.
 */
void alp_counter_close(alp_counter_t *counter);

/**
 * @brief Query the capabilities of an opened counter handle.
 *
 * @param counter  Handle from @ref alp_counter_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p counter is NULL.
 */
const alp_capabilities_t *alp_counter_capabilities(const alp_counter_t *counter);

/* ------------------------------------------------------------------ */
/* Quadrature decoder (incremental rotary encoder)                     */
/* ------------------------------------------------------------------ */

/** Opaque quadrature-encoder handle.  Allocate via @ref alp_qenc_open. */
typedef struct alp_qenc alp_qenc_t;

/** Configuration passed to @ref alp_qenc_open. */
typedef struct {
	uint32_t encoder_id;     /**< ENC0..ENC3 per `<alp/e1m_pinout.h>` (0..3). */
	uint16_t pulses_per_rev; /**< Mechanical resolution (informational). */
} alp_qenc_config_t;

/**
 * @brief Default-initialize an @ref alp_qenc_config_t for encoder @p id.
 *
 * Identity from @p id; canonical default: @c pulses_per_rev = 0 --
 * the field is informational only (the hardware decoder counts
 * quadrature edges regardless), so 0 ("unknown / not specified") is
 * a safe default rather than inventing a mechanical resolution
 * specific to some encoder model.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_QENC_CONFIG_DEFAULT(id) \
	((alp_qenc_config_t){ .encoder_id = (id), .pulses_per_rev = 0u })

/**
 * @brief Acquire a quadrature-decoder handle.
 *
 * Resolves @p cfg->encoder_id via the `alp-qenc<N>` devicetree alias.
 * Hardware decoders run continuously; the handle just gives you a way
 * to query the accumulated count.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL.
 * @return Open handle on success, or NULL on resolution failure.
 */
alp_qenc_t *alp_qenc_open(const alp_qenc_config_t *cfg);

/**
 * @brief Read the current accumulated position in counts.
 *
 * Sign indicates direction since the last reset; magnitude is
 * unbounded by hardware (the SDK accumulates in a 32-bit counter and
 * wraps modulo 2³²).
 *
 * @param[in]  enc      Handle from @ref alp_qenc_open.
 * @param[out] pos_out  Receives the signed accumulated count.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_qenc_get_position(alp_qenc_t *enc, int32_t *pos_out);

/**
 * @brief Reset the accumulated count to zero.
 *
 * @param[in] enc  Handle from @ref alp_qenc_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_qenc_reset_position(alp_qenc_t *enc);

/**
 * @brief Release the handle.  NULL is a no-op.
 *
 * @param[in] enc  Handle from @ref alp_qenc_open, or NULL.
 */
void alp_qenc_close(alp_qenc_t *enc);

/**
 * @brief Query the capabilities of an opened quadrature-encoder handle.
 *
 * @param enc  Handle from @ref alp_qenc_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p enc is NULL.
 */
const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *enc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_COUNTER_H */
