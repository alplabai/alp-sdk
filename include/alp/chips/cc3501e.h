/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cc3501e.h
 * @brief Alif-side host driver for the on-module TI CC3501E
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *        Wi-Fi 6 + BLE 5.4 coprocessor.
 *
 * Wraps the inter-chip SPI1 host-control protocol defined in
 * `<alp/protocol/cc3501e.h>` with a synchronous request/response
 * call shape plus an async-event callback.  Sits below
 * `<alp/iot.h>` and `<alp/ble.h>` -- those public surfaces
 * dispatch through this driver when the active SoM is an
 * E1M-AEN family member.
 *
 * Lifecycle:
 *
 * @code
 *   alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
 *       .bus_id = E1M_SPI1,   // inter-chip SPI bus
 *       .freq_hz = 8000000,
 *       .mode = ALP_SPI_MODE_0,
 *       .bits_per_word = 8,
 *   });
 *   cc3501e_t fw;
 *   cc3501e_init(&fw, bus);
 *   cc3501e_reset(&fw);            // pulses E_WIFI.NRST + WIFI.EN
 *   uint16_t version = 0;
 *   cc3501e_get_version(&fw, &version);
 *   if (version != ALP_CC3501E_PROTOCOL_VERSION) { ... }
 * @endcode
 *
 * The Alif drives `WIFI.EN` (P15_5) and `E_WIFI.NRST`
 * (P15_1_FLEX) through alp_gpio_*; the driver reads those pin
 * IDs from the studio-supplied config or falls back to
 * compile-time defaults baked from
 * `metadata/e1m_modules/aen/inter-chip.tsv`.
 */

#ifndef ALP_CHIPS_CC3501E_H
#define ALP_CHIPS_CC3501E_H

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

/** Retrieve the firmware's protocol version (compare against
 *  `ALP_CC3501E_PROTOCOL_VERSION` to confirm wire compatibility). */
alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out);

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
alp_status_t cc3501e_request(cc3501e_t *ctx, alp_cc3501e_cmd_t cmd, const uint8_t *tx_payload,
                             size_t tx_len, uint8_t *rx_buf, size_t rx_cap, size_t *rx_len,
                             uint32_t timeout_ms);

/** Register or replace the async-event callback.  Pass cb=NULL to detach. */
alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/** Free internal state.  Does not close the SPI bus -- caller owns it. */
void cc3501e_deinit(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_H */
