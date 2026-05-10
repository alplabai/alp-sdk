/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * qenc-readout — open ALP_E1M_ENC0 and poll the accumulated
 * position for ~1 second.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/counter.h"

int main(void) {
    printf("[qenc] open ALP_E1M_ENC0\n");

    alp_qenc_t *enc = alp_qenc_open(&(alp_qenc_config_t){
        .encoder_id     = 0,        /* ALP_E1M_ENC0 */
        .pulses_per_rev = 24,       /* mechanical resolution hint */
    });
    if (enc == NULL) {
        printf("[qenc] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[qenc] done\n");
        return 0;
    }

    alp_status_t s = alp_qenc_reset_position(enc);
    printf("[qenc] reset_position -> %d\n", (int)s);

    /* Poll the position 10 times at 100 ms intervals. */
    for (int i = 0; i < 10; i++) {
        int32_t pos = 0;
        s = alp_qenc_get_position(enc, &pos);
        printf("[qenc] t=%d ms  status=%d  pos=%d\n",
               (i + 1) * 100, (int)s, (int)pos);
        k_msleep(100);
    }

    alp_qenc_close(enc);
    printf("[qenc] done\n");
    return 0;
}
