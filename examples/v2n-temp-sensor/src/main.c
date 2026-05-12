/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-temp-sensor -- read the on-module TMP112 temperature sensor
 * once per second and print the value.  Classic V2N starter app.
 *
 * On V2N + V2N-M1 the TMP112 sits on BRD_I2C at 7-bit address
 * 0x40.  Same chip on E1M-AEN (on the Alif LPI2C bus); the example
 * is portable -- pick the right bus id for your board.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/tmp112.h"

int main(void) {
    printf("[temp] v2n-temp-sensor\n");

    /* On V2N the TMP112 is on BRD_I2C (Renesas RIIC8).  Pick the
     * bus_id the studio-resolved alias points at on real hardware. */
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,
        .bitrate_hz = 400000u,
    });
    if (bus == NULL) {
        printf("[temp] alp_i2c_open failed: err=%d\n",
               (int)alp_last_error());
        return 0;
    }

    tmp112_t sensor;
    alp_status_t s = tmp112_init(&sensor, bus, 0x40u);
    if (s != ALP_OK) {
        /* Bus reachable but the TMP112 isn't ACKing -- either the
         * chip isn't populated, the address is wrong, or the bus
         * has another device colliding.  i2c-scanner can confirm
         * which devices ACK. */
        printf("[temp] tmp112_init -> %d "
               "(populated? right address?)\n", (int)s);
        alp_i2c_close(bus);
        return 0;
    }

    /* Read + print 10 samples one second apart.  Real production
     * firmware would post the value to a logging subsystem or
     * publish over MQTT. */
    for (int i = 0; i < 10; ++i) {
        int16_t milli_c = 0;
        s = tmp112_read_temp_milli_c(&sensor, &milli_c);
        if (s == ALP_OK) {
            int whole = milli_c / 1000;
            int frac  = (milli_c < 0 ? -milli_c : milli_c) % 1000;
            printf("[temp] sample %d: %d.%03d degC\n", i, whole, frac);
        } else {
            printf("[temp] sample %d: read failed (status=%d)\n",
                   i, (int)s);
        }
        k_msleep(1000);
    }

    tmp112_deinit(&sensor);
    alp_i2c_close(bus);
    printf("[temp] done\n");
    return 0;
}
