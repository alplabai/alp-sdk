/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * can-loopback — bring up ALP_E1M_CAN0 in loopback mode, send a
 * frame, show that the rx callback receives it.
 *
 * Loopback mode is the canonical bring-up test for a CAN node:
 * the controller routes its own TX back to its own RX without
 * needing the bus to be wired or a partner on the line.  Once
 * loopback works, you've validated:
 *   - The controller is clocked and brought out of reset.
 *   - The TX path encodes frames correctly.
 *   - The RX path decodes frames correctly.
 *   - Filters / callbacks dispatch.
 * Move to bus mode (loopback=false) only after this works.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/can.h"
#include "alp/e1m_pinout.h"

/* Volatile because the rx callback runs from the CAN driver's RX
 * thread (Zephyr's `can_rx` worker) and the main loop polls. */
static volatile int rx_count = 0;

/* Receive callback.  Runs on the CAN host thread on Zephyr (not
 * an interrupt context, so printk-style logging is safe).  The
 * frame pointer is owned by the SDK and is reused after the
 * callback returns -- copy out anything you need to keep. */
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
        /* 500 kbps is the most common automotive default; bump to
         * 1 Mbps for industrial buses or down to 125 kbps for long
         * cable runs. */
        .bitrate_nominal_hz  = 500000,
        /* CLASSIC mode = ISO 11898-1, ≤ 8 byte payload.  Switch to
         * ALP_CAN_MODE_FD for ≤ 64 byte payload + bit-rate switch
         * (and set bitrate_data_hz appropriately). */
        .mode                = ALP_CAN_MODE_CLASSIC,
        /* Loopback off the wire -- self-test only.  Set to false
         * once you're driving real CAN_H/CAN_L. */
        .loopback            = true,
    });
    if (bus == NULL) {
        printf("[can] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[can] done\n");
        return 0;
    }

    /* Install a permissive filter (mask=0 matches every frame).
     * Real apps use mask/id pairs to filter to specific node
     * groups -- e.g. (id=0x100, mask=0xF80) accepts 0x100..0x17F. */
    alp_can_filter_t filt = { .id = 0, .mask = 0, .ext_id = false };
    int32_t fid = -1;   /* receives an opaque id we can use for remove_filter */
    alp_status_t s = alp_can_add_filter(bus, &filt, on_rx, NULL, &fid);
    printf("[can] add_filter -> status=%d fid=%d\n", (int)s, (int)fid);

    /* Move the controller from "configured" to "started" -- this is
     * when frames actually start flowing.  Filter must be installed
     * before start() to avoid losing the first frame. */
    s = alp_can_start(bus);
    printf("[can] start -> %d\n", (int)s);

    /* Send one classic-CAN frame: 11-bit ID 0x123, 8 byte payload.
     * In loopback, the controller routes this directly into its own
     * RX path; on a real bus, an external transceiver drives the
     * differential pair. */
    alp_can_frame_t tx = {
        .id = 0x123, .dlc = 8,
        .data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
    };
    s = alp_can_send(bus, &tx, 100);   /* 100 ms timeout for TX mailbox */
    printf("[can] send id=0x%03x -> %d\n", tx.id, (int)s);

    /* Spin-poll for the loopback frame.  Real apps would await an
     * event flag set by on_rx (k_event_post / k_event_wait). */
    for (int i = 0; i < 20 && rx_count == 0; i++) k_msleep(10);
    printf("[can] rx_count=%d\n", rx_count);

    /* Tear down: stop first (drains pending TX, halts RX), then
     * close (releases the handle).  Closing without stopping leaves
     * the controller in an error-passive state on some SoCs. */
    alp_can_stop(bus);
    alp_can_close(bus);
    printf("[can] done\n");
    return 0;
}
