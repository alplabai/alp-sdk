/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file version.h
 * @brief Alp SDK release version + ABI feature-test macros.
 *
 * Compile-time version identification for application code that must
 * adapt to the SDK it builds against (`#if ALP_VERSION_AT_LEAST(...)`)
 * plus per-class ABI-tier macros mirroring the `@par ABI status:`
 * marker each public header carries (see docs/abi-markers.md).
 *
 * The numeric version macros track `metadata/sdk_version.yaml` (the
 * single source of truth for the release version).  They are rewritten
 * by `scripts/bump_version.py` on every release bump and drift-checked
 * by `scripts/check_version_doc_sync.py` -- do not hand-edit the
 * numbers.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.8 surface; macro names and the composite encoding are
 *      frozen.  Values change every release by design; per-class
 *      ALP_ABI_STATUS_* values change only via a deliberate
 *      promotion PR that also updates docs/abi-markers.md.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_VERSION_H
#define ALP_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SDK release version.  MAJOR/MINOR/PATCH follow SemVer per
 * docs/release-policy.md.  Rewritten by scripts/bump_version.py.
 */
#define ALP_VERSION_MAJOR 0 /**< SDK major version (ABI-breaking changes). */
#define ALP_VERSION_MINOR 10 /**< SDK minor version (additive ABI changes). */
#define ALP_VERSION_PATCH 1 /**< SDK patch version (fixes, no surface change). */

/** SDK version as a string literal, e.g. "0.8.1". */
#define ALP_VERSION_STRING "0.10.1"

/**
 * @brief Encode a MAJOR.MINOR.PATCH triple into one comparable integer.
 *
 * Same encoding as @ref ALP_VERSION -- `(major << 16) | (minor << 8) | patch`.
 * Usable in `#if` preprocessor conditionals.
 */
#define ALP_VERSION_ENCODE(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

/** SDK version as one comparable integer (see @ref ALP_VERSION_ENCODE). */
#define ALP_VERSION ALP_VERSION_ENCODE(ALP_VERSION_MAJOR, ALP_VERSION_MINOR, ALP_VERSION_PATCH)

/**
 * @brief Compile-time check that the SDK is at least the given version.
 *
 * @code
 * #if ALP_VERSION_AT_LEAST(0, 9, 0)
 *     // use a surface introduced in v0.9.0
 * #endif
 * @endcode
 */
#define ALP_VERSION_AT_LEAST(major, minor, patch) \
	(ALP_VERSION >= ALP_VERSION_ENCODE((major), (minor), (patch)))

/*
 * ABI tiers.  Every public header declares one of these via its
 * `@par ABI status:` marker; the ALP_ABI_STATUS_* macros below expose
 * that classification to the preprocessor so application code can
 * feature-test, e.g.:
 *
 *     #if ALP_ABI_STATUS_STORAGE == ALP_ABI_STABLE
 *         // rely on <alp/storage.h> staying frozen until the next major
 *     #endif
 */
#define ALP_ABI_EXPERIMENTAL 1 /**< Surface may change in any minor release. */
#define ALP_ABI_STABLE       2 /**< Frozen; removals/renames require a major bump. */

/*
 * Per-class ABI status.  Values MUST match the `@par ABI status:`
 * prose in the class's public header (drift is a review defect --
 * a promotion PR updates the header marker, docs/abi-markers.md,
 * and this table together).
 */

/* <alp/peripheral.h> -- v0.1 surface, locked since v0.1. */
#define ALP_ABI_STATUS_GPIO ALP_ABI_STABLE /**< GPIO half of <alp/peripheral.h>. */
#define ALP_ABI_STATUS_I2C  ALP_ABI_STABLE /**< I2C half of <alp/peripheral.h>. */
#define ALP_ABI_STATUS_SPI  ALP_ABI_STABLE /**< SPI half of <alp/peripheral.h>. */
#define ALP_ABI_STATUS_UART ALP_ABI_STABLE /**< UART half of <alp/peripheral.h>. */

/*
 * Stable peripheral / service classes (one header each).  adc.h's
 * base surface is stable; its newer filter/spectrum additions stay
 * [ABI-EXPERIMENTAL] at function granularity (see the header).
 */
#define ALP_ABI_STATUS_ADC       ALP_ABI_STABLE /**< <alp/adc.h> (base surface). */
#define ALP_ABI_STATUS_DAC       ALP_ABI_STABLE /**< <alp/dac.h>. */
#define ALP_ABI_STATUS_PWM       ALP_ABI_STABLE /**< <alp/pwm.h>. */
#define ALP_ABI_STATUS_CAN       ALP_ABI_STABLE /**< <alp/can.h>. */
#define ALP_ABI_STATUS_RTC       ALP_ABI_STABLE /**< <alp/rtc.h>. */
#define ALP_ABI_STATUS_WDT       ALP_ABI_STABLE /**< <alp/wdt.h>. */
#define ALP_ABI_STATUS_COUNTER   ALP_ABI_STABLE /**< <alp/counter.h>. */
#define ALP_ABI_STATUS_I2S       ALP_ABI_STABLE /**< <alp/i2s.h>. */
#define ALP_ABI_STATUS_AUDIO     ALP_ABI_STABLE /**< <alp/audio.h>. */
#define ALP_ABI_STATUS_BLE       ALP_ABI_STABLE /**< <alp/ble.h>. */
#define ALP_ABI_STATUS_IOT       ALP_ABI_STABLE /**< <alp/iot.h>. */
#define ALP_ABI_STATUS_SECURITY  ALP_ABI_STABLE /**< <alp/security.h>. */
#define ALP_ABI_STATUS_INFERENCE ALP_ABI_STABLE /**< <alp/inference.h>. */
#define ALP_ABI_STATUS_MPROC     ALP_ABI_STABLE /**< <alp/mproc.h>. */
#define ALP_ABI_STATUS_HW_INFO   ALP_ABI_STABLE /**< <alp/hw_info.h>. */
#define ALP_ABI_STATUS_GUI       ALP_ABI_STABLE /**< <alp/gui.h>. */
#define ALP_ABI_STATUS_RPC       ALP_ABI_STABLE /**< <alp/rpc.h>. */

/* Experimental classes -- may change in any minor release. */
#define ALP_ABI_STATUS_STORAGE    ALP_ABI_EXPERIMENTAL /**< <alp/storage.h>. */
#define ALP_ABI_STATUS_USB        ALP_ABI_EXPERIMENTAL /**< <alp/usb.h>. */
#define ALP_ABI_STATUS_POWER      ALP_ABI_EXPERIMENTAL /**< <alp/power.h>. */
#define ALP_ABI_STATUS_CAMERA     ALP_ABI_EXPERIMENTAL /**< <alp/camera.h>. */
#define ALP_ABI_STATUS_DISPLAY    ALP_ABI_EXPERIMENTAL /**< <alp/display.h>. */
#define ALP_ABI_STATUS_DSP        ALP_ABI_EXPERIMENTAL /**< <alp/dsp.h>. */
#define ALP_ABI_STATUS_GPU2D      ALP_ABI_EXPERIMENTAL /**< <alp/gpu2d.h>. */
#define ALP_ABI_STATUS_TMU        ALP_ABI_EXPERIMENTAL /**< <alp/tmu.h>. */
#define ALP_ABI_STATUS_MODEL      ALP_ABI_EXPERIMENTAL /**< <alp/model.h>. */
#define ALP_ABI_STATUS_UPDATE_LOG ALP_ABI_EXPERIMENTAL /**< <alp/update_log.h>. */
#define ALP_ABI_STATUS_BACKEND    ALP_ABI_EXPERIMENTAL /**< <alp/backend.h>. */
#define ALP_ABI_STATUS_CAP        ALP_ABI_EXPERIMENTAL /**< <alp/cap.h> + <alp/cap_instance.h>. */

/**
 * @brief Return the SDK release version as a string.
 *
 * Runtime counterpart of @ref ALP_VERSION_STRING -- lets an
 * application report the SDK it was *linked* against (useful when
 * the SDK is consumed as a shared library and the header used at
 * compile time may be older than the runtime).
 *
 * @return Pointer to a static NUL-terminated "MAJOR.MINOR.PATCH"
 *         string; never NULL.
 */
const char *alp_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_VERSION_H */
