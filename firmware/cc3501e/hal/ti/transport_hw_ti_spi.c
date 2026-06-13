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
 * inter-chip.tsv): the host SPI1 control link lands on CC3501E
 * GPIO_27/28/29 (SCLK/MOSI/MISO); the Alif is master, the CC3501E is
 * slave.
 *
 * TI Drivers SPI is transfer-oriented (DMA), not byte-at-a-time, so a
 * frame arrives as a completed SPI_transfer rather than a per-byte ISR.
 * Because the cc3501e frame header carries an explicit payload_len, we
 * clock each frame in two fixed-count slave transfers -- a 4-byte
 * HEADER, then a payload of exactly the declared length -- then a third
 * transfer clocks the staged reply back.  A READY GPIO gives the host
 * the flow-control edge between chunks (raised when a transfer is armed,
 * lowered the instant its callback fires); this is the firmware READY
 * line that chips/cc3501e/cc3501e.c's bring-up rework consumes.  The
 * completed frame is replayed through the byte seams so the framing /
 * dispatch logic (and its host test) is identical to the stub path.
 *
 * Board anchors CONFIG_SPI_0 and CONFIG_GPIO_CC3501E_HOST_READY come
 * from the SDK's SysConfig output (ti_drivers_config.h) generated for
 * the E1M-AEN board -- resolved at bench-build time, not invented here.
 * Confirm the inter-chip SPI instance maps to GPIO_27/28/29 and the
 * READY pad against the board's SysConfig + SWRU626 §18 (SPI).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>

#include "../../src/protocol.h"
#include "../../src/transport.h"

/* Frame-clocking phases (see file header). */
enum spi_phase {
    PH_HEADER = 0, /* awaiting the 4-byte request header             */
    PH_PAYLOAD,    /* awaiting payload_len request payload bytes     */
    PH_REPLY,      /* clocking the staged reply back to the host     */
};

static SPI_Handle     spi;
static enum spi_phase phase;

/* One in-flight request frame + its staged reply.  Sized to the wire
 * ceiling (header + max payload). */
static uint8_t  frame_buf[CC3501E_FRAME_MAX_BYTES];
static uint8_t  reply_buf[CC3501E_FRAME_MAX_BYTES];
static size_t   reply_len;
static uint16_t cur_payload_len;

/* READY edge: high = a slave transfer is armed and the host may clock. */
static inline void host_ready(bool armed)
{
    GPIO_write(CONFIG_GPIO_CC3501E_HOST_READY, armed ? 1 : 0);
}

/* Arm a fixed-count slave RX (host clocks in; we send the 0xFF idle
 * fill the SysConfig SPI default provides for a NULL txBuf) or TX (host
 * clocks dummies; rxBuf NULL discards).  Raises READY once queued. */
static void arm_transfer(void *rx, const void *tx, size_t count)
{
    static SPI_Transaction t; /* retained for the duration of the transfer */
    t.count = count;
    t.txBuf = (void *)tx;
    t.rxBuf = rx;
    t.arg   = NULL;
    (void)SPI_transfer(spi, &t);
    host_ready(true);
}

/* Replay the captured request frame through the silicon-free seams,
 * which build the staged reply, then drain that reply into reply_buf. */
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
}

/* SPI transfer-complete callback (driver SWI/HWI context).  Advances
 * the header -> payload -> reply state machine. */
static void on_transfer(SPI_Handle h, SPI_Transaction *t)
{
    (void)h;
    (void)t;
    host_ready(false); /* transfer done; nothing armed yet */

    switch (phase) {
    case PH_HEADER: {
        /* frame_buf[0..3] hold the header.  Bound the declared payload
         * to the wire ceiling so a garbage length can't overrun the
         * RX into frame_buf; an over-long declared length then fails
         * the seam's captured-vs-declared check as RESP_ERR_PROTOCOL. */
        uint16_t plen = (uint16_t)frame_buf[2] | ((uint16_t)frame_buf[3] << 8);
        if (plen > ALP_CC3501E_MAX_PAYLOAD) {
            plen = ALP_CC3501E_MAX_PAYLOAD;
        }
        cur_payload_len = plen;
        if (plen == 0u) {
            dispatch_frame(ALP_CC3501E_HEADER_BYTES);
            phase = PH_REPLY;
            arm_transfer(NULL, reply_buf, reply_len);
        } else {
            phase = PH_PAYLOAD;
            arm_transfer(&frame_buf[ALP_CC3501E_HEADER_BYTES], NULL, plen);
        }
        break;
    }
    case PH_PAYLOAD:
        dispatch_frame((size_t)ALP_CC3501E_HEADER_BYTES + cur_payload_len);
        phase = PH_REPLY;
        arm_transfer(NULL, reply_buf, reply_len);
        break;

    case PH_REPLY:
    default:
        /* Reply fully clocked.  Re-arm for the next request header.
         * (A pending CMD_RESET is actioned by cc3501e_hw_tick() on the
         * next idle wakeup, after this ack has gone out.) */
        phase = PH_HEADER;
        arm_transfer(frame_buf, NULL, ALP_CC3501E_HEADER_BYTES);
        break;
    }
}

void bridge_transport_spi_hw_init(void)
{
    /* READY starts low until the first header RX is armed. */
    GPIO_setConfig(CONFIG_GPIO_CC3501E_HOST_READY, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);

    SPI_Params params;
    SPI_Params_init(&params);
    params.mode                = SPI_SLAVE;
    params.transferMode        = SPI_MODE_CALLBACK;
    params.transferCallbackFxn = on_transfer;
    params.frameFormat         = SPI_POL0_PHA0; /* mode 0, per the host driver / chip manifest */
    params.dataSize            = 8;

    spi                        = SPI_open(CONFIG_SPI_0, &params);
    if (spi == NULL) {
        /* No console this early; leave READY low so the host's PING
         * never completes and bring-up code reports the dead link. */
        return;
    }

    /* Arm the first request header. */
    phase = PH_HEADER;
    arm_transfer(frame_buf, NULL, ALP_CC3501E_HEADER_BYTES);
}
