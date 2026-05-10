/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * can-loopback — bring up CAN0 in loopback mode, send a frame,
 * show that the rx callback receives it.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/can.h"
#include "alp/e1m_pinout.h"

static volatile int rx_count = 0;

static void on_rx(const alp_can_frame_t *f, void *user) {
    (void)user;
    rx_count++;
    printk("[can] rx id=0x%03x dlc=%u data[0..3]=%02x %02x %02x %02x\n",
           f->id, f->dlc, f->data[0], f->data[1], f->data[2], f->data[3]);
}

int main(void) {
    printf("[can] open ALP_E1M_CAN0 @ 500 kbps loopback\n");

    alp_can_t *bus = alp_can_open(&(alp_can_config_t){
        .bus_id              = ALP_E1M_CAN0,
        .bitrate_nominal_hz  = 500000,
        .mode                = ALP_CAN_MODE_CLASSIC,
        .loopback            = true,
    });
    if (bus == NULL) {
        printf("[can] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[can] done\n");
        return 0;
    }

    alp_can_filter_t filt = { .id = 0, .mask = 0, .ext_id = false };
    int32_t fid = -1;
    alp_status_t s = alp_can_add_filter(bus, &filt, on_rx, NULL, &fid);
    printf("[can] add_filter -> status=%d fid=%d\n", (int)s, (int)fid);

    s = alp_can_start(bus);
    printf("[can] start -> %d\n", (int)s);

    alp_can_frame_t tx = {
        .id = 0x123, .dlc = 8,
        .data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
    };
    s = alp_can_send(bus, &tx, 100);
    printf("[can] send id=0x%03x -> %d\n", tx.id, (int)s);

    /* Wait briefly for the loopback frame to land on the rx path. */
    for (int i = 0; i < 20 && rx_count == 0; i++) k_msleep(10);
    printf("[can] rx_count=%d\n", rx_count);

    alp_can_stop(bus);
    alp_can_close(bus);
    printf("[can] done\n");
    return 0;
}
