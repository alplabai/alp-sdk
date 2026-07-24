/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file jpeg.h
 * @brief Alp SDK portable JPEG-encoder abstraction.
 *
 * Encodes a planar Y/U/V frame into a JPEG byte stream in one call.
 * Maps to the Alif Ensemble E8's Hantro JPEG hardware encoder on
 * AEN-family SoMs; every other SoM is served by a portable software
 * encoder fallback so customer code compiles + runs identically
 * across silicon.
 *
 * Task 1 of the encoder rollout shipped only the portable surface, the
 * class dispatcher, and a NOT_IMPLEMENTED stub backend.  Task 2 adds a
 * portable software baseline-JPEG encoder (4:2:0 + 4:0:0 only,
 * `src/backends/jpeg/sw_baseline.c`) that now wins backend selection
 * on every SoM without JPEG hardware, so @ref alp_jpeg_encode actually
 * encodes out of the box; the Alif Ensemble E8 Hantro VC9000E hardware
 * backend (Tasks 3-4) will outrank it on AEN silicon once it lands.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.13 new -- portable surface + stub (Task 1), then the
 *      software baseline encoder (Task 2) in the same v0.13 cycle.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_JPEG_H
#define ALP_JPEG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque JPEG-encoder handle. */
typedef struct alp_jpeg alp_jpeg_t;

/** Chroma subsampling scheme requested for an encode. */
typedef enum {
	ALP_JPEG_SUBSAMPLE_400 = 0, /**< Monochrome / Y-only. */
	ALP_JPEG_SUBSAMPLE_420 = 1, /**< 4:2:0. */
	ALP_JPEG_SUBSAMPLE_422 = 2, /**< 4:2:2. */
} alp_jpeg_subsample_t;

/** Encoder-open configuration. */
typedef struct {
	uint32_t engine_id;  /**< 0 = default JPEG engine. */
	uint16_t max_width;  /**< Expected max frame width; 0 = backend default. */
	uint16_t max_height; /**< Expected max frame height; 0 = backend default. */
} alp_jpeg_config_t;

/**
 * @brief Default-initialize an @ref alp_jpeg_config_t.
 *
 * @c engine_id selects the default (first) engine; @c max_width /
 * @c max_height default to 0, letting the backend pick its own
 * working-buffer sizing.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ --
 *       the SDK's toolchains; standard through C23).
 */
#define ALP_JPEG_CONFIG_DEFAULT \
	((alp_jpeg_config_t){ .engine_id = 0u, .max_width = 0u, .max_height = 0u })

/**
 * @brief One encode request: a planar Y/U/V frame plus quality target.
 *
 * @c u_plane / @c v_plane are unused (and must be NULL) when
 * @c subsample is @ref ALP_JPEG_SUBSAMPLE_400.
 */
typedef struct {
	uint16_t             width;  /**< 8..16384. */
	uint16_t             height; /**< 8..16384. */
	alp_jpeg_subsample_t subsample;
	uint8_t              quality; /**< 1..100. */
	const void          *y_plane;
	uint32_t             y_stride;
	const void          *u_plane; /**< NULL when subsample is _400. */
	uint32_t             u_stride;
	const void          *v_plane; /**< NULL when subsample is _400. */
	uint32_t             v_stride;
} alp_jpeg_encode_req_t;

/** Capabilities of an opened JPEG-encoder handle. */
typedef struct {
	bool     hw_accelerated;  /**< true if a hardware engine backs this handle. */
	bool     mjpeg_supported; /**< true if the backend can chain frames as M-JPEG. */
	uint16_t max_width;
	uint16_t max_height;
	uint32_t subsample_mask; /**< bit i set => (1u<<ALP_JPEG_SUBSAMPLE_x) supported. */
} alp_jpeg_caps_t;

/**
 * @brief Open a JPEG-encoder handle.
 *
 * @param[in] cfg  Open configuration.  Must be non-NULL.
 *
 * @return Open handle on success, or NULL with @ref alp_last_error set
 *         to one of @ref ALP_ERR_INVAL (NULL cfg), @ref ALP_ERR_NOMEM
 *         (handle pool exhausted), @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC
 *         (no jpeg backend registered), or @ref ALP_ERR_NOT_IMPLEMENTED
 *         (backend has no open()).
 */
alp_jpeg_t *alp_jpeg_open(const alp_jpeg_config_t *cfg);

/**
 * @brief Encode one planar Y/U/V frame into a JPEG byte stream.
 *
 * @param[in]  h         Handle from @ref alp_jpeg_open.
 * @param[in]  req       Frame + quality descriptor.  Must be non-NULL,
 *                       with non-zero @c width / @c height.
 * @param[out] out_buf   Destination buffer for the encoded stream.
 * @param[in]  out_cap   Capacity of @p out_buf, in bytes.
 * @param[out] out_len   Receives the encoded length on success; on
 *                       @ref ALP_ERR_NOMEM the backend sets it to the
 *                       required size when known.
 *
 * @return @ref ALP_OK on success, or one of:
 *         - @ref ALP_ERR_INVAL -- NULL @p h / @p req / @p out_buf /
 *           @p out_len, or a zero @c width / @c height.
 *         - @ref ALP_ERR_NOSUPPORT -- subsampling the backend can't do.
 *         - @ref ALP_ERR_NOMEM -- @p out_cap too small for the result.
 *         - @ref ALP_ERR_IO -- hardware-engine fault.
 *         - @ref ALP_ERR_NOT_IMPLEMENTED -- backend has no encode path.
 *         - @ref ALP_ERR_NOT_READY -- handle not open / mid-close.
 */
alp_status_t alp_jpeg_encode(alp_jpeg_t                  *h,
                             const alp_jpeg_encode_req_t *req,
                             void                        *out_buf,
                             size_t                       out_cap,
                             size_t                      *out_len);

/**
 * @brief Query the capabilities of an opened JPEG-encoder handle.
 *
 * @param[in]  h    Handle from @ref alp_jpeg_open.
 * @param[out] out  Receives the capability descriptor.  Must be non-NULL.
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL if @p h or @p out is NULL.
 */
alp_status_t alp_jpeg_capabilities(const alp_jpeg_t *h, alp_jpeg_caps_t *out);

/**
 * @brief Close the handle and release backend resources.  Idempotent.
 *
 * NULL is a no-op.
 *
 * @param[in] h  Handle from @ref alp_jpeg_open, or NULL.
 */
void alp_jpeg_close(alp_jpeg_t *h);

#ifdef __cplusplus
}
#endif

#endif /* ALP_JPEG_H */
