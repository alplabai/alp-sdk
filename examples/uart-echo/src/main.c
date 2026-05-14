/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * uart-echo — read bytes from E1M_UART0 and write them back.
 *
 * In CI we don't actually feed input, so the example does a single
 * non-blocking read with a short timeout to exercise the read path
 * then exits cleanly so the twister console harness can assert
 * `[uart] done`.  On a real terminal you'd see the static greeting
 * written to stdout and any keys you typed echoed back if you
 * extended the loop.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

int main(void) {
    printf("[uart] open E1M_UART0 @ 115200 8N1\n");

    /* The 8-N-1 framing is the lowest common denominator for serial
     * consoles -- 8 data bits, no parity, 1 stop bit.  Override
     * data_bits / parity / stop_bits when interfacing with chips
     * that demand a non-standard framing (some industrial PLCs use
     * 7-E-1, RS-485 buses with multidrop addressing use 9-bit
     * frames, etc.). */
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
        .port_id   = E1M_UART0,
        .baudrate  = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    });
    if (u == NULL) {
        /* No alp-uart0 alias on this build -> NULL handle.
         * On native_sim with the overlay we ship, the alias maps
         * to the host-stdin/stdout virtual UART so this branch
         * isn't taken. */
        printf("[uart] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[uart] done\n");
        return 0;
    }

    /* Read with a 50 ms timeout -- plenty for an interactive
     * terminal but short enough that CI doesn't stall.  The byte
     * value is meaningful only when status==ALP_OK; otherwise the
     * caller must treat it as unspecified. */
    uint8_t b = 0;
    alp_status_t s = alp_uart_read(u, &b, 1, 50);
    printf("[uart] read -> status=%d byte=0x%02x\n", (int)s, b);

    /* Write a known greeting so a human running the example on
     * real hardware sees output.  alp_uart_write blocks until the
     * full buffer is sent or the driver returns an error. */
    static const uint8_t hello[] = "[uart] hello\r\n";
    s = alp_uart_write(u, hello, sizeof hello - 1);
    printf("[uart] write -> %d\n", (int)s);

    /* Close releases the handle but does NOT power down the
     * peripheral or drop the line -- on real hardware the UART
     * keeps running idle high.  If you need to truly shut down,
     * disable the corresponding Zephyr device via PM API after
     * close. */
    alp_uart_close(u);
    printf("[uart] done\n");
    return 0;
}
