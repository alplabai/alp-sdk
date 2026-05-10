/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * uart-echo — read bytes from ALP_E1M_UART0 and write them back.
 * In CI we don't actually feed input, so the example does a
 * single non-blocking read with a short timeout to exercise the
 * read path then exits.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

int main(void) {
    printf("[uart] open ALP_E1M_UART0 @ 115200 8N1\n");

    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
        .port_id   = ALP_E1M_UART0,
        .baudrate  = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    });
    if (u == NULL) {
        printf("[uart] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[uart] done\n");
        return 0;
    }

    /* Brief non-blocking read attempt; harmless if no input. */
    uint8_t b = 0;
    alp_status_t s = alp_uart_read(u, &b, 1, 50);
    printf("[uart] read -> status=%d byte=0x%02x\n", (int)s, b);

    /* Write a known greeting back so a real terminal sees output. */
    static const uint8_t hello[] = "[uart] hello\r\n";
    s = alp_uart_write(u, hello, sizeof hello - 1);
    printf("[uart] write -> %d\n", (int)s);

    alp_uart_close(u);
    printf("[uart] done\n");
    return 0;
}
