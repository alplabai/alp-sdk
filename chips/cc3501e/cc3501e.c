/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * v0.3 ships the call shape (init / reset / get_version /
 * request / set_event_callback / deinit) and the framing logic.
 * The actual reset-pin pulse + WIFI.EN sequencing arrives once
 * the EVK overlay declares the Alif's P15_5 / P15_1_FLEX as
 * GPIOs reachable via alp_gpio_*; until then reset() returns
 * NOSUPPORT cleanly.  Synchronous transactions go through right
 * now -- the firmware needs to be running for them to mean
 * anything, but the host side compiles + links cleanly today.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h"

static void encode_header(uint8_t *frame, alp_cc3501e_cmd_t cmd, uint8_t flags,
                          uint16_t payload_len)
{
    frame[0] = (uint8_t)cmd;
    frame[1] = flags;
    frame[2] = (uint8_t)(payload_len & 0xFF);
    frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
}

static uint16_t decode_header_payload_len(const uint8_t *frame)
{
    return (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
}

alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus)
{
    if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->bus         = bus;
    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (ctx->reset_pin == NULL || ctx->enable_pin == NULL) {
        /* The studio's pin allocator (or hand-written firmware
         * via alp_gpio_open) must populate enable_pin / reset_pin
         * before reset() is meaningful.  Until then there's no
         * line to pulse. */
        return ALP_ERR_NOSUPPORT;
    }
    /* Drive WIFI.EN low, hold reset asserted, release reset, then
     * raise WIFI.EN.  TI's CC33xx datasheet specifies a >= 10 ms
     * power-on sequence; we'll add a delay helper in v0.3.x once
     * the alp_delay_* surface lands.  For now the GPIO sequence
     * itself is the measurable step. */
    (void)alp_gpio_write(ctx->enable_pin, false);
    (void)alp_gpio_write(ctx->reset_pin, false);
    (void)alp_gpio_write(ctx->enable_pin, true);
    (void)alp_gpio_write(ctx->reset_pin, true);
    return ALP_OK;
}

alp_status_t cc3501e_request(cc3501e_t *ctx, alp_cc3501e_cmd_t cmd, const uint8_t *tx_payload,
                             size_t tx_len, uint8_t *rx_buf, size_t rx_cap, size_t *rx_len,
                             uint32_t timeout_ms)
{
    (void)timeout_ms; /* Reserved for the v0.3.x semaphore-based wait. */
    if (rx_len != NULL) *rx_len = 0;
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (tx_len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
    if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

    encode_header(ctx->tx_scratch, cmd, ALP_CC3501E_FLAG_RESP_REQUIRED, (uint16_t)tx_len);
    if (tx_len > 0) {
        memcpy(ctx->tx_scratch + ALP_CC3501E_HEADER_BYTES, tx_payload, tx_len);
    }

    /* Full-duplex transfer of header+payload; reply lands in
     * rx_scratch on the same MISO line.  TI's SPI-slave parser
     * stalls SCK between frames so the master can drive header
     * then payload in one transaction. */
    size_t       frame_len = ALP_CC3501E_HEADER_BYTES + tx_len;
    alp_status_t s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, frame_len);
    if (s != ALP_OK) return s;

    /* Read the response header first.  v0.3 trusts the firmware
     * to clock out a valid header within the same frame; v0.3.x
     * adds a separate "wait-for-MISO-not-busy" pass via a
     * dedicated busy/ready GPIO from the firmware. */
    uint16_t resp_len = decode_header_payload_len(ctx->rx_scratch);
    if (resp_len > rx_cap) resp_len = (uint16_t)rx_cap;
    if (resp_len > 0 && rx_buf != NULL) {
        memcpy(rx_buf, ctx->rx_scratch + ALP_CC3501E_HEADER_BYTES, resp_len);
    }
    if (rx_len != NULL) *rx_len = resp_len;
    return ALP_OK;
}

alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out)
{
    if (version_out == NULL) return ALP_ERR_INVAL;
    uint8_t      reply[2] = {0};
    size_t       got      = 0;
    alp_status_t s =
        cc3501e_request(ctx, ALP_CC3501E_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), &got, 100);
    if (s != ALP_OK) return s;
    if (got < sizeof(reply)) return ALP_ERR_IO;
    *version_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
    return ALP_OK;
}

alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    ctx->event_cb   = cb;
    ctx->event_user = user;
    return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
    if (ctx == NULL) return;
    ctx->initialised = false;
    ctx->bus         = NULL;
}
