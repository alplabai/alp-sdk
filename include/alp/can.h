/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file can.h
 * @brief Alp SDK CAN / CAN-FD bus abstraction.
 *
 * The RZ/V2N family routes six CAN-FD channels; Alif Ensemble routes
 * two.  Both vendors expose ISO 11898-1 and CAN-FD via their HAL; this
 * wrapper gives the studio a uniform `alp_can_*` surface that doesn't
 * depend on which silicon implements the controller.
 *
 * Backends:
 *   - Zephyr   : `can_*` driver class (Zephyr 3.x classic + FD).
 *   - Yocto    : `socketcan` (`AF_CAN`).
 *   - Baremetal: vendor HAL CAN driver.
 *
 * Typical usage:
 * @code
 *     alp_can_t *bus = alp_can_open(&(alp_can_config_t){
 *         .bus_id              = 0,
 *         .bitrate_nominal_hz  = 500000,
 *         .bitrate_data_hz     = 2000000,
 *         .mode                = ALP_CAN_MODE_FD,
 *     });
 *     alp_can_start(bus);
 *     alp_can_send(bus, &(alp_can_frame_t){
 *         .id = 0x123, .dlc = 8, .data = {1,2,3,4,5,6,7,8}
 *     }, 100);
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_CAN_H
#define ALP_CAN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Operation mode.  CAN-FD enables higher data-phase bit rates. */
typedef enum {
	ALP_CAN_MODE_CLASSIC = 0, /**< ISO 11898-1 classic CAN, ≤ 8 byte payload. */
	ALP_CAN_MODE_FD      = 1  /**< CAN-FD, ≤ 64 byte payload + bit-rate switch. */
} alp_can_mode_t;

/** Maximum payload bytes per frame, by mode. */
#define ALP_CAN_MAX_DLC_CLASSIC 8
#define ALP_CAN_MAX_DLC_FD      64

/** A single CAN frame (TX or RX). */
typedef struct {
	uint32_t id;     /**< 11-bit (standard) or 29-bit (extended). */
	bool     ext_id; /**< true = 29-bit, false = 11-bit. */
	bool     rtr;    /**< Remote-transmission request. */
	bool     fd;     /**< CAN-FD frame. */
	bool     brs;    /**< Bit-rate switch (FD only). */
	uint8_t  dlc;    /**< Data length, 0..@ref ALP_CAN_MAX_DLC_FD. */
	uint8_t  data[ALP_CAN_MAX_DLC_FD];
} alp_can_frame_t;

/** Receive-side filter.  A frame matches when (frame.id & mask) == (id & mask). */
typedef struct {
	uint32_t id;     /**< Filter pattern. */
	uint32_t mask;   /**< Bits to compare (1 = must match). */
	bool     ext_id; /**< true = match 29-bit IDs only. */
} alp_can_filter_t;

/** Opaque CAN bus handle.  Allocate via @ref alp_can_open. */
typedef struct alp_can alp_can_t;

/** Configuration passed to @ref alp_can_open. */
typedef struct {
	uint32_t       bus_id;
	uint32_t       bitrate_nominal_hz; /**< Arbitration-phase bit rate. */
	uint32_t       bitrate_data_hz;    /**< Data-phase rate (FD); 0 if classic. */
	alp_can_mode_t mode;
	bool           loopback; /**< Local self-test mode. */
} alp_can_config_t;

/**
 * @brief Default-initialize an @ref alp_can_config_t for bus @p id.
 *
 * Identity from @p id; canonical defaults: @c bitrate_nominal_hz = 500 kHz
 * (a widely-interoperable classic-CAN rate), @c bitrate_data_hz = 0
 * (classic — no data-phase rate), @c mode = @ref ALP_CAN_MODE_CLASSIC,
 * @c loopback = false (on the wire, not local self-test). For CAN-FD set
 * @c mode = @ref ALP_CAN_MODE_FD and a non-zero @c bitrate_data_hz.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_CAN_CONFIG_DEFAULT(id)                                                                 \
	((alp_can_config_t){ .bus_id             = (id),                                               \
	                     .bitrate_nominal_hz = 500000u,                                            \
	                     .bitrate_data_hz    = 0u,                                                 \
	                     .mode               = ALP_CAN_MODE_CLASSIC,                               \
	                     .loopback           = false })

/**
 * @brief Receive-side dispatch callback.
 *
 * Invoked from the bus's RX worker thread when an incoming frame
 * matches an installed filter.  The frame pointer is owned by the
 * SDK and reused after the callback returns; copy out anything you
 * need to keep.
 *
 * @param[in] frame  Decoded incoming frame.
 * @param[in] user   Opaque pointer the caller passed into
 *                   @ref alp_can_add_filter.
 */
typedef void (*alp_can_rx_cb_t)(const alp_can_frame_t *frame, void *user);

/**
 * @brief Configure a CAN bus controller.
 *
 * Sets the nominal bit rate, optionally the data-phase bit rate
 * (CAN-FD), and the mode.  Does not enable RX/TX — call
 * @ref alp_can_start when ready.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL with non-zero
 *                 @c bitrate_nominal_hz and (for CAN-FD) non-zero
 *                 @c bitrate_data_hz.
 * @return Open handle on success, or NULL on:
 *         - invalid @p cfg
 *         - bus_id out of range or alias unset
 *         - controller rejected the requested timing
 *         - CAN-FD requested but `CONFIG_CAN_FD_MODE=n`
 */
alp_can_t *alp_can_open(const alp_can_config_t *cfg);

/**
 * @brief Enable bus RX/TX.  Required before send / receive succeeds.
 *
 * @param[in] can  Handle from @ref alp_can_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO (bus-off state).
 */
alp_status_t alp_can_start(alp_can_t *can);

/**
 * @brief Disable bus RX/TX.  Pending TX frames are discarded.
 *
 * @param[in] can  Handle from @ref alp_can_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY.
 */
alp_status_t alp_can_stop(alp_can_t *can);

/**
 * @brief Transmit a frame.  Blocks up to @p timeout_ms for slot availability.
 *
 * @param[in] can         Handle from @ref alp_can_open after @ref alp_can_start.
 * @param[in] frame       Frame to transmit.  @c dlc must respect the mode.
 * @param[in] timeout_ms  Max wait for a free TX mailbox.
 * @return ALP_OK on success;
 *         ALP_ERR_NOT_READY if @p can is NULL or stopped;
 *         ALP_ERR_INVAL if @p frame is NULL or DLC out of range;
 *         ALP_ERR_TIMEOUT if all TX mailboxes are occupied;
 *         ALP_ERR_IO on bus error (e.g. error-passive state).
 */
alp_status_t alp_can_send(alp_can_t *can, const alp_can_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief Install a receive filter and dispatch matching frames to @p cb.
 *
 * Stores the filter in the controller's hardware filter bank when
 * possible; otherwise falls back to software filtering.
 *
 * @param[in]  can             Handle from @ref alp_can_open.
 * @param[in]  filter          Filter pattern.
 * @param[in]  cb              Dispatch callback.  Must not be NULL.
 * @param[in]  user            Opaque pointer forwarded to @p cb.
 * @param[out] filter_id_out   Receives an opaque id for later removal.
 *                             May be NULL if the caller never removes.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOMEM
 *         (filter slots exhausted) / ALP_ERR_IO.
 */
alp_status_t alp_can_add_filter(alp_can_t              *can,
                                const alp_can_filter_t *filter,
                                alp_can_rx_cb_t         cb,
                                void                   *user,
                                int32_t                *filter_id_out);

/**
 * @brief Remove a previously-installed filter by id.
 *
 * @param[in] can        Handle from @ref alp_can_open.
 * @param[in] filter_id  Opaque id returned by @ref alp_can_add_filter.
 *
 * @return ALP_OK / ALP_ERR_INVAL (unknown id) / ALP_ERR_NOT_READY.
 */
alp_status_t alp_can_remove_filter(alp_can_t *can, int32_t filter_id);

/**
 * @brief Stop the bus, release the handle.  NULL is a no-op.
 *
 * @param[in] can  Handle from @ref alp_can_open, or NULL.
 */
void alp_can_close(alp_can_t *can);

/**
 * @brief Query the capabilities of an opened CAN handle.
 *
 * @param can  Handle from @ref alp_can_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p can is NULL.
 */
const alp_capabilities_t *alp_can_capabilities(const alp_can_t *can);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CAN_H */
