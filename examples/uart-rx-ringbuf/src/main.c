/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * uart-rx-ringbuf — exercise the opt-in LwRB-backed RX path on
 * the console UART (EVK_UART_PORT_DEBUG).
 *
 * The classic alp_uart_read() blocks the calling thread until at
 * least one byte arrives.  That's fine for command-line prompts but
 * wrong for any task that needs to do other work while bytes trickle
 * in -- a chatty sensor talking at 9600 baud, a debug shell
 * multiplexed with a CAN bus drain, a GPS NMEA stream parsed once
 * per second from a worker thread, etc.
 *
 * The ringbuf helper splits the producer from the consumer:
 *
 *     UART RX IRQ ────► LwRB ─────► consumer thread
 *     (in the SDK)                   (alp_uart_rx_ringbuf_pop)
 *
 * The IRQ runs whenever a byte arrives -- regardless of which thread
 * is current -- and stuffs it into the caller-supplied backing
 * store.  The consumer pops batched bytes whenever convenient; if it
 * falls behind, the LwRB simply drops the overflow on the floor
 * (back-pressure, not corruption).  Sizing the backing store covers
 * the worst-case drain latency at the worst-case baud rate.
 *
 * For interactive consoles 256 bytes is plenty (humans type slowly).
 * For sensor streams a rule of thumb is:
 *
 *     backing_size = baud_rate / 10 * worst_case_drain_latency_s
 *
 * The /10 captures the standard 1 start + 8 data + 1 stop framing.
 *
 * Build (in-tree):
 *
 *     west build -b native_sim/native/64 examples/uart-rx-ringbuf \
 *         -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
 *     west build -t run
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"

/* EVK_UART_PORT_DEBUG is a board-macro from the generated routes
 * header (= E1M_UART0); rebind it in board.yaml `pins:` to port this
 * app to another board without touching the code below. */
#include "alp/boards/alp_e1m_evk_routes.h"

/* Backing store for the ring.  64 bytes is enough for the CI run;
 * production apps size against the worst-case drain latency formula
 * in the header comment above. */
static uint8_t rx_backing[64];

int main(void)
{
    printf("[ringbuf] open EVK_UART_PORT_DEBUG @ 115200 8N1\n");

    /* The classic open() — no different from the uart-echo example.
     * The ringbuf is a *layer on top*, not a replacement.  Apps that
     * mix polled reads with ringbuf reads on the same handle work,
     * though the typical pattern is one or the other. */
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
        .port_id   = EVK_UART_PORT_DEBUG, /* = E1M_UART0 */
        .baudrate  = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    });
    if (u == NULL) {
        printf("[ringbuf] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[ringbuf] done\n");
        return 0;
    }

    /* Attach the ring.  The backing store is caller-owned -- the SDK
     * never allocates a buffer behind the user's back.  This makes
     * the memory contract explicit (apps can audit their RAM budget)
     * and lets the buffer live in a specific region (e.g. DTCM /
     * tightly-coupled SRAM on Cortex-M55 for lowest IRQ-to-consumer
     * latency). */
    alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(
        u, rx_backing, sizeof(rx_backing));
    if (rb == NULL) {
        /* On builds without CONFIG_ALP_SDK_UART_RX_RINGBUF the
         * attach helper returns NULL with ALP_ERR_NOSUPPORT.  The
         * example's prj.conf flips that config on so we shouldn't
         * see this in CI -- but real-world apps should still
         * defend against it. */
        printf("[ringbuf] attach failed: alp_last_error=%d\n",
               (int)alp_last_error());
        alp_uart_close(u);
        printf("[ringbuf] done\n");
        return 0;
    }

    /* Background work happens here.  In a real app this is where
     * the main loop runs -- service other peripherals, run inference,
     * advance state machines, etc.  Meanwhile the UART IRQ
     * silently drains the controller FIFO into the ring. */
    printf("[ringbuf] attached; backing=%zu bytes\n", sizeof(rx_backing));

    /* k_msleep emulates "doing real work for a while".  On native_sim
     * we won't actually receive any bytes during this nap, but the
     * code path is identical to a production loop. */
    k_msleep(50);

    /* Drain whatever the IRQ has staged.  Non-blocking: if the ring
     * is empty, got=0 and we move on.  This is the pattern apps
     * follow at every wakeup -- pop opportunistically, never block
     * waiting for the ring. */
    uint8_t scratch[32];
    size_t  got = 0;
    alp_status_t s = alp_uart_rx_ringbuf_pop(rb, scratch, sizeof(scratch), &got);
    printf("[ringbuf] pop -> status=%d got=%zu count_remaining=%zu\n",
           (int)s, got, alp_uart_rx_ringbuf_count(rb));

    /* Detach when the ringbuf is no longer needed.  Disables the
     * IRQ-driven RX path so the underlying alp_uart_t reverts to
     * normal polled-read semantics.  After detach the caller's
     * backing store may be reused or freed. */
    alp_uart_rx_ringbuf_detach(rb);

    /* Closing the port releases the alp_uart_t handle but does not
     * power down the controller -- same as uart-echo. */
    alp_uart_close(u);
    printf("[ringbuf] done\n");
    return 0;
}
