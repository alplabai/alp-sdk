/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SPI-slave transport wiring (the
 * DEFAULT host-control link).  Built ONLY for CC3501E_HAL_BACKEND=ti.
 *
 * Drives the SILICON-FREE byte seams in src/transport_spi.c from the
 * CC3501E's SPI peripheral via TI Drivers (SPI_open in SPI_SLAVE +
 * SPI_MODE_CALLBACK).  Inter-chip pins (metadata/e1m_modules/aen/
 * inter-chip.tsv): SCLK/MOSI/MISO on CC3501E GPIO_27/28/29; Alif is
 * master, CC3501E is slave.
 *
 * ===================== 3-WIRE FRAMING (this rev) =====================
 * The current E1M-AEN rev wires ONLY SCLK/MOSI/MISO -- no CS, no host
 * IRQ/READY line (CS + IRQ are planned for the next board rev, which
 * will harden framing + enable async events; see docs/cc3501e-bridge.md
 * "Selectable host-control transport").  With no CS edge to delimit
 * transactions, framing is purely by FIXED CLOCK COUNT in deterministic
 * lockstep -- each side derives the next transfer's length from a header
 * it already exchanged:
 *
 *   1. master clocks 4    -> request header   (slave reads payload_len)
 *   2. master clocks N    -> request payload   (N = that payload_len)
 *   3. master clocks 4    -> reply header      (master reads reply len)
 *   4. master clocks M    -> reply payload      (M = that reply len)
 *
 * No CS means the CC3501E's SS pad must be tied to its asserted level
 * on the SoM so the slave is permanently selected, and SPI_transfer()
 * must complete on clock-count alone -- CONFIRM against SWRU626 §18.
 * No IRQ means the host POLLS for the reply (it adds a settle gap then
 * reads the reply header); slow (Wi-Fi/BLE) replies + async events wait
 * on the next-rev IRQ line.  For the v0.1 META group dispatch is instant
 * so the settle gap suffices.  The completed request frame is replayed
 * through the byte seams, so framing/dispatch (and the host test) are
 * identical to the stub path.
 *
 * CONFIG_SPI_0 is the SysConfig anchor for the inter-chip SPI instance,
 * resolved at bench-build time from the E1M-AEN board file -- confirm it
 * maps to GPIO_27/28/29.
 * =====================================================================
 */

#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include <ti/drivers/SPI.h>

#include "../../src/protocol.h"
#include "../../src/transport.h"

/* Deterministic lockstep phases (see file header). */
enum spi_phase {
    PH_REQ_HEADER = 0, /* clocking the 4-byte request header   */
    PH_REQ_PAYLOAD,    /* clocking payload_len request bytes   */
    PH_REPLY_HEADER,   /* clocking the 4-byte reply header     */
    PH_REPLY_PAYLOAD,  /* clocking the reply payload           */
};

static SPI_Handle     spi;
static enum spi_phase phase;

/* One in-flight request frame + its staged reply (header + max payload). */
static uint8_t  frame_buf[CC3501E_FRAME_MAX_BYTES];
static uint8_t  reply_buf[CC3501E_FRAME_MAX_BYTES];
static size_t   reply_len;
static uint16_t cur_payload_len;

/* Arm a fixed-count slave transfer.  For RX, tx is NULL (the SPI
 * default fill clocks out on MISO); for TX, rx is NULL (the host's
 * MOSI dummies are discarded).  Non-blocking in SPI_MODE_CALLBACK. */
static void arm_transfer(void *rx, const void *tx, size_t count)
{
    static SPI_Transaction t; /* retained for the transfer's duration */
    t.count = count;
    t.txBuf = (void *)tx;
    t.rxBuf = rx;
    t.arg   = NULL;
    (void)SPI_transfer(spi, &t);
}

/* Replay the captured request frame through the silicon-free seams
 * (which build the staged reply), then drain that reply into reply_buf. */
static void dispatch_frame(size_t frame_len)
{
    spi_slave_cs_low();
    for (size_t i = 0; i < frame_len; i++) {
        spi_slave_rx_byte(frame_buf[i]);
    }
    spi_slave_cs_high();

    reply_len = 0u;
    while (spi_slave_tx_pending() && reply_len < sizeof(reply_buf)) {
        reply_buf[reply_len++] = spi_slave_tx_next_byte();
    }
    /* The reply is always header(4) + payload(>=1 status byte). */
}

/* SPI transfer-complete callback (driver SWI/HWI context).  Advances
 * the request-header -> request-payload -> reply-header -> reply-payload
 * lockstep. */
static void on_transfer(SPI_Handle h, SPI_Transaction *t)
{
    (void)h;
    (void)t;

    switch (phase) {
    case PH_REQ_HEADER: {
        /* Bound the declared payload to the wire ceiling so a garbage
         * length can't overrun the RX into frame_buf; an over-long
         * declared length then fails the seam's captured-vs-declared
         * check as RESP_ERR_PROTOCOL. */
        uint16_t plen = (uint16_t)frame_buf[2] | ((uint16_t)frame_buf[3] << 8);
        if (plen > ALP_CC3501E_MAX_PAYLOAD) {
            plen = ALP_CC3501E_MAX_PAYLOAD;
        }
        cur_payload_len = plen;
        if (plen == 0u) {
            dispatch_frame(ALP_CC3501E_HEADER_BYTES);
            phase = PH_REPLY_HEADER;
            arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
        } else {
            phase = PH_REQ_PAYLOAD;
            arm_transfer(&frame_buf[ALP_CC3501E_HEADER_BYTES], NULL, plen);
        }
        break;
    }
    case PH_REQ_PAYLOAD:
        dispatch_frame((size_t)ALP_CC3501E_HEADER_BYTES + cur_payload_len);
        phase = PH_REPLY_HEADER;
        arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
        break;

    case PH_REPLY_HEADER:
        /* Reply header clocked out; now the reply payload (status + data
         * = reply_len - 4 bytes, always >= 1). */
        phase = PH_REPLY_PAYLOAD;
        arm_transfer(NULL, &reply_buf[ALP_CC3501E_HEADER_BYTES],
                     reply_len - ALP_CC3501E_HEADER_BYTES);
        break;

    case PH_REPLY_PAYLOAD:
    default:
        /* Whole reply clocked.  Re-arm for the next request header.
         * (A pending CMD_RESET is actioned by cc3501e_hw_tick() on the
         * next idle wakeup, after this ack has gone out.) */
        phase = PH_REQ_HEADER;
        arm_transfer(frame_buf, NULL, ALP_CC3501E_HEADER_BYTES);
        break;
    }
}

void bridge_transport_spi_hw_init(void)
{
    SPI_Params params;
    SPI_Params_init(&params);
    params.mode                = SPI_SLAVE;
    params.transferMode        = SPI_MODE_CALLBACK;
    params.transferCallbackFxn = on_transfer;
    params.frameFormat         = SPI_POL0_PHA0; /* mode 0, per the host driver / chip manifest */
    params.dataSize            = 8;

    spi                        = SPI_open(CONFIG_SPI_0, &params);
    if (spi == NULL) {
        /* No console this early; the host's PING simply never completes
         * and bring-up code reports the dead link. */
        return;
    }

    /* Arm the first request header. */
    phase = PH_REQ_HEADER;
    arm_transfer(frame_buf, NULL, ALP_CC3501E_HEADER_BYTES);
}
