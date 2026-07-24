/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file jpeg.h
 * @brief Alp SDK portable JPEG-encoder abstraction.
 *
 * Encodes a source frame into a JPEG byte stream in one call.  The
 * caller declares the frame's buffer LAYOUT via
 * @c alp_jpeg_encode_req_t::format (@ref alp_pixfmt_t) -- fully-planar
 * Y/U/V or semi-planar NV12, see the struct doc.  Maps to the Alif
 * Ensemble E8's Hantro JPEG hardware encoder on AEN-family SoMs; every
 * other SoM is served by a portable software encoder fallback so
 * customer code compiles + runs identically across silicon (each
 * backend accepts a different subset of layouts -- query
 * @ref alp_jpeg_caps_t::pixfmt_mask rather than assuming one).
 *
 * Task 1 of the encoder rollout shipped only the portable surface, the
 * class dispatcher, and a NOT_IMPLEMENTED stub backend.  Task 2 adds a
 * portable software baseline-JPEG encoder (4:2:0 + 4:0:0 only,
 * `src/backends/jpeg/sw_baseline.c`) that now wins backend selection
 * on every SoM without JPEG hardware, so @ref alp_jpeg_encode actually
 * encodes out of the box.  Tasks 3-4 add the Alif Ensemble E8 Hantro
 * VC9000E hardware backend (`src/backends/jpeg/alif_hantro.c`), which
 * outranks the software backend on AEN silicon, plus the explicit
 * @c format field so the two backends' differing buffer-layout needs
 * (planar vs. NV12) are a declared part of the request instead of an
 * implicit per-backend reinterpretation of the same pointers.
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
 * @brief One encode request: a source frame plus quality target.
 *
 * @c format (@ref alp_pixfmt_t) selects the source buffer's memory
 * LAYOUT -- which of @c y_plane / @c u_plane / @c v_plane are live, and
 * how they relate to each other:
 *
 *   - @ref ALP_PIXFMT_YUV420_PLANAR (I420) -- fully planar: @c y_plane /
 *     @c u_plane / @c v_plane are three independent buffers, each with
 *     its own stride.  This is what the portable software backend
 *     (src/backends/jpeg/sw_baseline.c) consumes.
 *   - @ref ALP_PIXFMT_NV12 -- semi-planar: @c y_plane is the base of a
 *     single contiguous buffer (Y plane, then at @c y_stride * @c height
 *     an interleaved U/V plane); @c u_plane / @c v_plane are NOT
 *     consulted and must be NULL.  This is what the Alif Hantro VC9000E
 *     hardware backend (src/backends/jpeg/alif_hantro.c) latches as its
 *     single raw-frame pointer.
 *
 * @c subsample stays a SEPARATE field from @c format on purpose: it is
 * the chroma SAMPLING RATIO (4:0:0 mono / 4:2:0 / 4:2:2) a backend
 * encodes at, orthogonal to which buffer LAYOUT carries the samples in.
 * Both currently-registered pixel formats happen to be 4:2:0-only, but
 * collapsing the two fields would force inventing @ref alp_pixfmt_t
 * values for "mono, planar layout" and "4:2:2, planar layout" that
 * <alp/peripheral.h>'s display/camera callers have no use for -- @c
 * format only needs to grow when a new BUFFER LAYOUT shows up, @c
 * subsample only needs to grow when a new SAMPLING RATIO shows up.
 * @c u_plane / @c v_plane are unused (and must be NULL) when
 * @c subsample is @ref ALP_JPEG_SUBSAMPLE_400, independent of @c format.
 */
typedef struct {
	uint16_t             width;  /**< 8..16384. */
	uint16_t             height; /**< 8..16384. */
	alp_pixfmt_t         format; /**< Source buffer layout -- see the struct doc above. */
	alp_jpeg_subsample_t subsample;
	uint8_t              quality; /**< 1..100. */
	const void          *y_plane;
	uint32_t             y_stride;
	const void          *u_plane; /**< NULL when subsample is _400, or format is _NV12. */
	uint32_t             u_stride;
	const void          *v_plane; /**< NULL when subsample is _400, or format is _NV12. */
	uint32_t             v_stride;
} alp_jpeg_encode_req_t;

/** Capabilities of an opened JPEG-encoder handle. */
typedef struct {
	bool     hw_accelerated;  /**< true if a hardware engine backs this handle;
	                           *   see the DMA-reachable-buffer @note on
	                           *   @ref alp_jpeg_encode. */
	bool     mjpeg_supported; /**< true if the backend can chain frames as M-JPEG. */
	uint16_t max_width;
	uint16_t max_height;
	uint32_t subsample_mask; /**< bit i set => (1u<<ALP_JPEG_SUBSAMPLE_x) supported. */
	uint32_t pixfmt_mask;    /**< bit i set => (1u<<ALP_PIXFMT_x) accepted as @c
	                          *   alp_jpeg_encode_req_t::format by this backend. */
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
 * @brief Encode one source frame into a JPEG byte stream.
 *
 * @param[in]  h         Handle from @ref alp_jpeg_open.
 * @param[in]  req       Frame + quality descriptor.  Must be non-NULL,
 *                       with non-zero @c width / @c height and a
 *                       @c format the opened backend's
 *                       @ref alp_jpeg_caps_t::pixfmt_mask advertises.
 * @param[out] out_buf   Destination buffer for the encoded stream.
 * @param[in]  out_cap   Capacity of @p out_buf, in bytes.
 * @param[out] out_len   Receives the encoded length on success; on
 *                       @ref ALP_ERR_NOMEM the backend sets it to the
 *                       required size when known.
 *
 * @note A hardware-accelerated backend (@ref alp_jpeg_caps_t::hw_accelerated
 *       true) DMAs directly to/from the caller-supplied buffers -- it does
 *       NOT bounce them through an intermediate copy or translate the
 *       pointer.  When @c hw_accelerated is true, @p req's input plane(s)
 *       (@c y_plane / @c u_plane / @c v_plane) AND @p out_buf must reside in
 *       memory the hardware's DMA master can reach: globally-addressable
 *       system RAM, NOT core-local tightly-coupled memory (TCM), and should
 *       be cache-coherent.  A backend that detects a buffer it cannot DMA to
 *       returns @ref ALP_ERR_NOSUPPORT rather than encode incorrectly (or
 *       hang); the caller can relocate the buffer or fall back to a
 *       software backend (@c hw_accelerated false), which has no such
 *       placement restriction.
 *
 * @return @ref ALP_OK on success, or one of:
 *         - @ref ALP_ERR_INVAL -- NULL @p h / @p req / @p out_buf /
 *           @p out_len, or a zero @c width / @c height.
 *         - @ref ALP_ERR_NOSUPPORT -- @c format or @c subsample the
 *           backend can't do, or (hardware backends only) a buffer that
 *           is not DMA-reachable -- see the @note above.
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
