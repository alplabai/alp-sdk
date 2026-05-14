/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DFRobot A02YYUW waterproof ultrasonic ranger driver (UART).
 * See <alp/chips/a02yyuw.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/a02yyuw.h"

alp_status_t a02yyuw_init(a02yyuw_t *dev, alp_uart_t *port)
{
    if (dev == NULL || port == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->port        = port;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t a02yyuw_read_distance(a02yyuw_t *dev,
                                   uint16_t  *distance_mm,
                                   uint32_t   timeout_ms)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (distance_mm == NULL) return ALP_ERR_INVAL;

    /* Sync on 0xFF header. */
    uint8_t      b      = 0;
    alp_status_t s      = ALP_OK;
    uint32_t     synced = 0;
    while (synced < 8 /* arbitrary cap on noise bytes */) {
        s = alp_uart_read(dev->port, &b, 1, timeout_ms);
        if (s != ALP_OK) return s;
        if (b == 0xFF) break;
        synced++;
    }
    if (b != 0xFF) return ALP_ERR_IO;

    uint8_t frame[3] = {0};
    s                = alp_uart_read(dev->port, frame, sizeof(frame), timeout_ms);
    if (s != ALP_OK) return s;

    const uint8_t hi  = frame[0];
    const uint8_t lo  = frame[1];
    const uint8_t chk = frame[2];
    if (((0xFF + hi + lo) & 0xFFu) != chk) return ALP_ERR_IO;

    *distance_mm = (uint16_t)(((uint16_t)hi << 8) | lo);
    return ALP_OK;
}

void a02yyuw_deinit(a02yyuw_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->port        = NULL;
}
