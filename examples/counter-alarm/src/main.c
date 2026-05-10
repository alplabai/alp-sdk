/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * counter-alarm — schedule a one-shot alarm 100 ms ahead.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/counter.h"

static volatile bool fired = false;

static void on_alarm(alp_counter_t *c, uint32_t ticks, void *user) {
    (void)c; (void)user;
    fired = true;
    printk("[counter] alarm fired @ ticks=%u\n", ticks);
}

int main(void) {
    printf("[counter] open id=0\n");

    alp_counter_t *c = alp_counter_open(&(alp_counter_config_t){
        .counter_id = 0,
    });
    if (c == NULL) {
        printf("[counter] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[counter] done\n");
        return 0;
    }

    alp_status_t s = alp_counter_start(c);
    printf("[counter] start -> %d\n", (int)s);

    uint32_t ticks_100ms = 0;
    s = alp_counter_us_to_ticks(c, 100000, &ticks_100ms);
    printf("[counter] 100ms = %u ticks (status=%d)\n",
           ticks_100ms, (int)s);

    s = alp_counter_set_alarm(c, ticks_100ms, on_alarm, NULL);
    printf("[counter] set_alarm -> %d\n", (int)s);

    /* Wait up to 250 ms for the callback. */
    for (int i = 0; i < 25 && !fired; i++) k_msleep(10);
    printf("[counter] fired=%d\n", (int)fired);

    alp_counter_close(c);
    printf("[counter] done\n");
    return 0;
}
