/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file core.h
 * @brief CC3501E driver context, lifecycle, and request primitive.
 *
 * Shared types every other subheader under `alp/chips/cc3501e/` depends on:
 * the driver context (@ref cc3501e_t), the async-event callback typedef,
 * and the init / reset / sync / version lifecycle.  Included by the
 * `<alp/chips/cc3501e.h>` umbrella; also includable on its own by code
 * that only needs the context type + lifecycle (e.g. a backend that
 * receives an already-initialised handle).
 */

#ifndef ALP_CHIPS_CC3501E_CORE_H
#define ALP_CHIPS_CC3501E_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc3501e cc3501e_t;

/** Async event callback -- runs on the driver's RX thread.
 *  @p cmd is the event opcode (one of `ALP_CC3501E_EVT_*`),
 *  @p payload + @p len carry the event-specific data described
 *  in `<alp/protocol/cc3501e.h>`. */
typedef void (*cc3501e_event_cb_t)(uint8_t cmd, const uint8_t *payload, size_t len, void *user);

struct cc3501e {
	bool               initialised;
	alp_spi_t         *bus;        /**< SPI1 to the CC3501E (Alif master). */
	alp_gpio_t        *enable_pin; /**< WIFI.EN (P15_5).  May be NULL on boards that tie it on. */
	alp_gpio_t        *reset_pin;  /**< E_WIFI.NRST (P15_1_FLEX). */
	alp_gpio_t        *ready_pin;  /**< OPTIONAL host-IRQ/READY in (CC35 GPIO17 -> Alif P2_6):
	                                *   HIGH when the SPI slave is armed+idle.  When populated,
	                                *   cc3501e_request() waits on it before each reply phase
	                                *   instead of a fixed settle gap.  NULL = legacy fixed gap. */
	cc3501e_event_cb_t event_cb;
	void              *event_user;
	uint8_t            rx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
	uint8_t            tx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
};

/**
 * @brief Initialise the driver and bind it to an open SPI1 bus.
 *
 * Does not enable the radio -- call @ref cc3501e_reset to bring
 * the firmware up.  @p bus must remain valid for the lifetime
 * of @p ctx.
 */
alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus);

/** Pulse the firmware's reset line then de-assert WIFI.EN.  Blocks
 *  until the firmware's PING reply arrives or the timeout expires. */
alp_status_t cc3501e_reset(cc3501e_t *ctx);

/**
 * @brief Warm hard reset: pulse nRESET with WIFI_EN kept asserted (rails stay up).
 *
 * Re-boots the module WITHOUT a cold power cycle.  This is the "second boot" of the
 * CC3501E Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround: a cold power-on
 * mis-reads the Puya flash on the FIRST boot (TI SDK bug, 32/64Mbit Puya parts), so
 * the secure boot never launches the vendor image; a hard reset re-boots with the
 * flash settled and the image launches.  @ref cc3501e_reset already issues one such
 * re-boot after the cold power-up; call this again (e.g. from a soak/retry loop) if
 * a single re-boot has not brought the link up.  Remove once TI ships the flash fix.
 *
 * @param ctx Initialised driver context (must have @c reset_pin populated).
 * @return ALP_OK after the re-boot budget elapses; ALP_ERR_NOSUPPORT if no reset pin.
 */
alp_status_t cc3501e_hard_reset(cc3501e_t *ctx);

/**
 * @brief (Re)establish byte alignment on the CS-less 3-wire link.
 *
 * With no chip-select to delimit transactions, the master and slave keep
 * framing by fixed clock count alone; a missed/extra clock (or a slave
 * that booted mid-transaction) leaves them byte-misaligned with no edge to
 * recover on.  This walks the SPI byte phase until it observes the slave's
 * header-idle marker (@ref ALP_CC3501E_SYNC_IDLE, driven only when the
 * slave is parked at a clean request-header boundary), confirming with two
 * consecutive aligned reads to reject a stray marker byte inside reply
 * data.  Call it before the first request after reset, and on any
 * desync the request path detects (reply header that doesn't echo the
 * command).
 *
 * @param ctx         Initialised driver context.
 * @param timeout_ms  Coarse upper bound on re-sync effort (each ~ms covers
 *                    one full-frame byte-walk attempt).
 * @return ALP_OK once aligned; ALP_ERR_TIMEOUT if the slave never parked
 *         (e.g. unpowered / not running its firmware).
 */
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms);

/** Retrieve the firmware's protocol version (compare against
 *  `ALP_CC3501E_PROTOCOL_VERSION` to confirm wire compatibility). */
alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out);

/**
 * @brief Send one FRAMED bulk-data frame to the CC3501E stream sink (proto v2).
 *
 * Wraps @ref ALP_CC3501E_CMD_STREAM_WRITE -- the request payload (@p len bytes)
 * is clocked in a single SPI transfer, so it rides the host peripheral-DMA path
 * when @p len reaches the SPI DMA threshold (@c CONFIG_SPI_DW_ALIF_DMA_MIN_LEN).
 * The firmware sinks + acks the frame, so unlike raw throwaway clocking the link
 * stays framed and never desyncs.  Send frames back-to-back for a bulk stream.
 *
 * @param ctx   Initialised, reset driver context.
 * @param data  Bulk bytes to send (may be NULL only if @p len is 0).
 * @param len   Byte count, at most @c ALP_CC3501E_MAX_PAYLOAD minus the header.
 * @return ALP_OK on ack; ALP_ERR_INVAL on a bad arg / oversized frame; the
 *         mapped firmware status otherwise.
 */
alp_status_t cc3501e_stream_write(cc3501e_t *ctx, const uint8_t *data, size_t len);

/** Issue a synchronous command + wait for the response.
 *
 *  @param ctx         CC3501E driver context (must be initialised first).
 *  @param cmd         Command opcode (one of @c ALP_CC3501E_CMD_* ).
 *  @param tx_payload  Outbound payload bytes (may be NULL with len 0).
 *  @param tx_len      Outbound payload length in bytes.
 *  @param rx_buf      Reply buffer (response payload, less the
 *                     frame header).  Truncated to @p rx_cap.
 *  @param rx_cap      Capacity of @p rx_buf in bytes.
 *  @param rx_len      Receives bytes copied (may be NULL).
 *  @param timeout_ms  Max wait. */
alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_CORE_H */
