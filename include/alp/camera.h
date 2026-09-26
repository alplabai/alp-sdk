/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.h
 * @brief Alp SDK camera abstraction.
 *
 * Real backends are registered on Zephyr, each gated by its own
 * Kconfig symbol (`CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO` /
 * `_V2N_N44_ISP` / `_ALIF_ISP`, all `depends on VIDEO`) -- without
 * `CONFIG_VIDEO` only `zephyr_stub` below links and every call
 * returns ALP_ERR_NOT_IMPLEMENTED:
 *   - **zephyr_video** (silicon_ref `"*"`, priority 50): portable
 *     `drivers/video/` wrapper.  open/start/stop/capture/release/
 *     close route to real silicon on any board with a `video_*`
 *     sensor driver; `configure_isp` is ALP_ERR_NOSUPPORT (the
 *     portable video class has no in-line ISP knobs).
 *   - **v2n_n44_isp** (silicon_ref `"renesas:rzv2n:n44"`, priority
 *     100): same sensor pipeline as zephyr_video, real once the
 *     V2N N44 SoC port wires its MIPI CSI-2 IP up to
 *     `drivers/video/` (not yet landed), plus the N44 on-die ISP's
 *     `configure_isp` (AE/AWB/AF + tuning offsets latch and return
 *     ALP_OK; the MMIO pokes land once the N44 ISP register map is
 *     public).
 *   - **alif_isp_pico** (silicon_ref `"alif:ensemble:e8"`, priority
 *     100): same real sensor pipeline, plus the E8 VeriSilicon
 *     ISP-Pico's `configure_isp` (same latch-and-ALP_OK posture).
 *     OPT-IN (`CONFIG_ALP_SDK_CAMERA_ALIF_ISP`, default n) --
 *     depends on `VIDEO_ISP_VSI`.  Negotiates a YUV ISP MI output
 *     (converting to RGB565 on the CPU when the caller asked for
 *     RGB565; passing a native YUV format through unmodified
 *     otherwise), drives AWB/AE through the standard Zephyr video
 *     ctrl registry, and keeps the driver's incoming-buffer fifo fed
 *     -- see that backend's file header for the full sequence.
 *     Runtime capture is proven end to end on isp_pico.c's own bench
 *     app (examples/aen/aen-isp-capture, runs 69-145) and, through
 *     this PORTABLE header, on examples/aen/aen-isp-ov5647-viewfinder --
 *     30/30 colour frames captured with AE+AWB on E1M-AEN803 + OV5647
 *     (runs 156-166).
 *   - **zephyr_stub** (silicon_ref `"*"`, priority 0): tracked
 *     fallback for silicon none of the above cover -- every op
 *     returns ALP_ERR_NOT_IMPLEMENTED (issue #223).
 *
 * On Yocto and baremetal only `zephyr_stub` is linked today, so
 * every call there returns ALP_ERR_NOT_IMPLEMENTED / NULL.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 added alp_camera_configure_isp -- surface tentative pending real hardware feedback.  Base capture path stable; ISP block experimental.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_CAMERA_H
#define ALP_CAMERA_H

#include <stdint.h>
#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alp_camera alp_camera_t;

typedef struct {
	uint32_t     camera_id;
	uint16_t     width;
	uint16_t     height;
	uint8_t      fps; /**< Requested frame rate, in frames/second. 0 = let the
	                   *   backend pick its own default; nonzero is a
	                   *   REQUEST, not a guarantee -- the backend settles on
	                   *   the nearest rate its sensor/mode actually supports
	                   *   (e.g. the OV5647 only reaches one of a fixed rate
	                   *   table). The settled rate is not reported back to
	                   *   the caller yet (issue #2279). Not every backend
	                   *   honors this field at all yet -- see issue #2278. */
	alp_pixfmt_t format;
} alp_camera_config_t;

/**
 * @brief Default-initialize an @ref alp_camera_config_t for camera @p id.
 *
 * Identity from @p id.  There is no universally-common sensor
 * resolution, so @c width / @c height default to 0 as an explicit
 * "you must choose" sentinel -- @ref alp_camera_open rejects an
 * out-of-range configuration, so a caller who forgets to set them
 * fails loudly rather than opening at an unintended size.  @c fps
 * defaults to 30 (the common video frame rate) and @c format defaults
 * to @ref ALP_PIXFMT_RGB565 (the widely-supported embedded-camera
 * default -- @ref ALP_PIXFMT_MONO_VLSB, the enum's zero value, is a
 * narrow SSD1306-specific format and would be a misleading default
 * here). Set @c width / @c height before calling open().
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_CAMERA_CONFIG_DEFAULT(id) \
	((alp_camera_config_t){ \
	    .camera_id = (id), .width = 0u, .height = 0u, .fps = 30u, .format = ALP_PIXFMT_RGB565 })

typedef struct {
	void    *data;
	size_t   size;
	uint64_t timestamp_us;
} alp_camera_frame_t;

/**
 * @brief Open a camera capture handle.
 *
 * @param[in] cfg  Capture configuration; @c camera_id selects the
 *                 device, width/height/fps/format request the stream
 *                 shape.  Must be non-NULL.
 *
 * @return Open handle on success, or NULL with @ref alp_last_error
 *         set to one of ALP_ERR_INVAL (NULL cfg), ALP_ERR_NOMEM
 *         (handle pool exhausted), ALP_ERR_NOT_PRESENT_ON_THIS_SOC
 *         (no backend registered for the active silicon),
 *         ALP_ERR_NOT_IMPLEMENTED (registered backend has no open
 *         hook), or a backend-specific failure from the open call
 *         itself -- e.g. ALP_ERR_INVAL (zephyr_video / v2n_n44_isp /
 *         alif_isp_pico: out-of-range @c camera_id), ALP_ERR_NOT_READY
 *         (zephyr_video: no camera aliased in devicetree for the
 *         requested @c camera_id), or ALP_ERR_NOT_IMPLEMENTED
 *         (zephyr_stub, on silicon with no real backend).
 */
alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg);

/**
 * @brief Start streaming.
 *
 * Frames become available via @ref alp_camera_capture after this call
 * returns ALP_OK.  Idempotent if the stream is already running.
 *
 * @param[in] c  Handle from @ref alp_camera_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_start(alp_camera_t *c);

/**
 * @brief Stop streaming.
 *
 * Backend may keep the handle warm for a subsequent
 * @ref alp_camera_start.  In-flight frames captured via
 * @ref alp_camera_capture remain valid until released.
 *
 * @param[in] c  Handle from @ref alp_camera_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_stop(alp_camera_t *c);

/**
 * @brief Block until next frame is available.
 *
 * Caller does not own the frame buffer; release via
 * @ref alp_camera_release once the data is consumed.
 *
 * @param[in]  c           Handle from @ref alp_camera_open.
 * @param[out] out         Receives the frame descriptor (buffer
 *                         pointer + size + capture timestamp).
 *                         Must be non-NULL.
 * @param[in]  timeout_ms  Max wait in milliseconds; UINT32_MAX for
 *                         "wait indefinitely".
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL out or handle) /
 *         ALP_ERR_NOT_READY (stream not started) / ALP_ERR_TIMEOUT /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_capture(alp_camera_t *c, alp_camera_frame_t *out, uint32_t timeout_ms);

/**
 * @brief Return the frame buffer to the backend after consumption.
 *
 * After release the frame's @c data pointer is invalid -- backends
 * may immediately reuse the buffer for the next capture.
 *
 * @param[in] c      Handle from @ref alp_camera_open.
 * @param[in] frame  Frame descriptor previously filled by
 *                   @ref alp_camera_capture.  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *frame);

/**
 * @brief Close the handle and release backend resources.  Idempotent.
 *
 * NULL is a no-op.  In-flight frames are implicitly released; their
 * @c data pointers become invalid immediately on return.
 *
 * @param[in] c  Handle from @ref alp_camera_open, or NULL.
 */
void alp_camera_close(alp_camera_t *c);

/**
 * @brief Query the capabilities of an opened camera handle.
 *
 * @param c  Handle from @ref alp_camera_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p c is NULL.
 */
const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *c);

/* ================================================================== */
/* ISP (Image Signal Processor) configuration                          */
/*                                                                     */
/* AEN-family E4 / E6 / E8 ship a dedicated ISP                       */
/* (Alif's hardened VeriSilicon ISP Pico (vsi,isp-pico) path) that    */
/* Zephyr's portable                                                  */
/* drivers/video/ class doesn't expose at the on-chip-ISP level --     */
/* the existing class covers sensor bridges + format negotiation but   */
/* not in-line image processing.  Customers migrating from V2N to     */
/* AEN otherwise silently lose ISP acceleration.                       */
/*                                                                     */
/* The minimal v0.5 surface declared below covers the headline ISP    */
/* primitives most cameras want enabled by default; finer-grained     */
/* tuning (per-channel gain tables, lens-shading-correction LUTs,     */
/* white-balance setpoints) can be added in follow-up commits as     */
/* customer demand surfaces.                                          */
/* ================================================================== */

/** Coarse ISP feature toggles.  All-zeros = bypass / passthrough --
 *  the sensor's raw frame reaches the application unchanged.  Each
 *  bit enables one major ISP pipeline stage.  Field-level meanings:
 *   - auto_exposure: AE convergence loop adjusts exposure +
 *     analog/digital gain.
 *   - auto_white_balance: AWB statistics drive the per-channel
 *     gain block.
 *   - auto_focus: drives the lens VCM (cameras with a focusable
 *     lens module).
 *   - lens_shading: applies the lens vignetting-correction LUT.
 *   - dead_pixel_correction: replaces flagged stuck pixels with
 *     neighbour-averaged values.
 *   - noise_reduction: 2D / 3D temporal NR (backend-defined). */
typedef struct {
	bool auto_exposure;
	bool auto_white_balance;
	bool auto_focus;
	bool lens_shading;
	bool dead_pixel_correction;
	bool noise_reduction;
	/** Picture-tuning offsets, -128..+127.  Applied after the
     *  auto-* feedback loops resolve to their setpoints.
     *  Field-level meanings:
     *   - brightness: pre-gamma luma offset.
     *   - contrast: luma scale around mid-grey.
     *   - saturation: chroma scale around grey (0 = monochrome). */
	int8_t  brightness;
	int8_t  contrast;
	int8_t  saturation;
	uint8_t reserved;
} alp_camera_isp_config_t;

/**
 * @brief Apply an ISP configuration to an open camera stream.
 *
 * Safe to call before or after @ref alp_camera_start; backends
 * latch the config and apply on the next frame boundary.
 * Backends without an on-die ISP return @ref ALP_ERR_NOSUPPORT.
 *
 * @param[in] camera  Handle from @ref alp_camera_open.
 * @param[in] isp     Configuration.  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_NOSUPPORT (backend lacks an ISP -- V2N today,
 *         AEN-family E3 / E5 / E7 silicon without the
 *         optional ISP fabric) / ALP_ERR_IO.
 */
alp_status_t alp_camera_configure_isp(alp_camera_t *camera, const alp_camera_isp_config_t *isp);

/* ================================================================== */
/* Trigger mode (issue #2287)                                          */
/*                                                                     */
/* Some global-shutter sensors (e.g. Sony IMX296) can take frame       */
/* timing from an external pulse instead of running free-run --       */
/* useful for synchronising capture to another event (a rotary        */
/* encoder tick, another camera, a lab strobe).  Previously this was  */
/* reachable only through a driver-private Zephyr video CID; the      */
/* surface below makes it part of the portable <alp/camera.h> API so  */
/* an app (e.g. examples/connectivity/camera-mjpeg-stream) can drive  */
/* it without touching Zephyr's drivers/video/ class directly.        */
/* ================================================================== */

/** Camera frame-timing source. */
typedef enum {
	/** Sensor runs its own internal timing; frames arrive at the
	 *  negotiated frame rate with no external input required. This
	 *  is every backend's default at @ref alp_camera_open -- a
	 *  backend that ever switches a sensor to @ref
	 *  ALP_CAMERA_TRIGGER_EXTERNAL restores this mode again in its
	 *  own @ref alp_camera_close, AFTER it has fully stopped the
	 *  stream (not merely after a caller's own @ref alp_camera_stop,
	 *  which is not required before @ref alp_camera_close): a sensor
	 *  that still believes it is streaming when the reset is
	 *  attempted can reject a trigger-mode write outright (IMX296's
	 *  "via sensor standby" restriction being one such case), so the
	 *  restore is only reliable once streaming has actually torn
	 *  down first. A later @ref alp_camera_open never inherits a
	 *  prior session's trigger setting even on the same underlying
	 *  hardware device. */
	ALP_CAMERA_TRIGGER_FREE_RUN = 0,
	/** Frame timing comes from pulses on the sensor module's
	 *  external trigger input -- the app/carrier board must drive
	 *  that line itself (see @ref alp_camera_set_trigger_mode).
	 *  Not every sensor/backend supports this mode. On some sensors
	 *  (e.g. IMX296 fast-trigger mode) the external pulse's WIDTH is
	 *  the exposure time itself, not just the frame-start signal --
	 *  on those sensors, exposure/gain controls and any auto-exposure
	 *  loop the backend or ISP would otherwise run no longer apply
	 *  once this mode is active; consult the sensor driver's own
	 *  documentation for which behaviour it implements. */
	ALP_CAMERA_TRIGGER_EXTERNAL = 1,
} alp_camera_trigger_t;

/**
 * @brief Select the camera's frame-timing source.
 *
 * Valid only while the stream is stopped: call after @ref alp_camera_open
 * and before @ref alp_camera_start (or after a matching @ref
 * alp_camera_stop). While the stream is running the mode switch is
 * rejected with @ref ALP_ERR_BUSY -- most sensors that support this
 * control can only switch between free-run and external-trigger timing
 * through their own standby state, so there is no safe way to apply the
 * change mid-stream.
 *
 * In @ref ALP_CAMERA_TRIGGER_EXTERNAL mode, frame timing comes entirely
 * from pulses the application (or the carrier board it runs on) drives on
 * the sensor module's own trigger input -- this API does not generate
 * those pulses itself, only arms the sensor to expect them. Once
 * streaming, @ref alp_camera_capture blocks until a triggered frame
 * actually arrives; with no trigger pulses it behaves exactly like an
 * unusually slow free-run stream and eventually returns @ref
 * ALP_ERR_TIMEOUT at the caller's requested timeout.
 *
 * Requesting @ref ALP_CAMERA_TRIGGER_FREE_RUN against a sensor/backend with
 * no trigger control at all returns @ref ALP_OK, not @ref ALP_ERR_NOSUPPORT
 * -- such a sensor is already free-running (its only possible mode), so
 * asking for the mode it is already in is a no-op success rather than a
 * failure. @ref ALP_ERR_NOSUPPORT is reserved for a genuinely unreachable
 * mode: requesting @ref ALP_CAMERA_TRIGGER_EXTERNAL on hardware that cannot
 * enter it.
 *
 * @param[in] c     Handle from @ref alp_camera_open.
 * @param[in] mode  Requested trigger mode.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL @p c, or @p mode outside @ref
 *         alp_camera_trigger_t) / ALP_ERR_NOT_READY (handle not open) /
 *         ALP_ERR_BUSY (stream already running) / ALP_ERR_NOSUPPORT
 *         (@ref ALP_CAMERA_TRIGGER_EXTERNAL requested but backend or sensor
 *         has no trigger-mode control) / ALP_ERR_IO.
 */
alp_status_t alp_camera_set_trigger_mode(alp_camera_t *c, alp_camera_trigger_t mode);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CAMERA_H */
