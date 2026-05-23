/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "handles.h"

/* Each pool is a static array + mutex.  A handle is "in use" iff
 * its in_use flag is set; close() simply clears the flag.  Mutex is
 * only held during acquire/release, not during transfers — Zephyr's
 * own driver locking covers the underlying device. */

#define DEFINE_POOL(kind, size, type)                                      \
    static struct type kind##_slots[size];                                 \
    static struct k_mutex kind##_lock;                                     \
    static int kind##_init(void) {                                         \
        k_mutex_init(&kind##_lock);                                        \
        return 0;                                                          \
    }                                                                      \
    SYS_INIT(kind##_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT); \
    struct type *alp_z_##kind##_pool_acquire(void) {                       \
        struct type *out = NULL;                                           \
        k_mutex_lock(&kind##_lock, K_FOREVER);                             \
        for (size_t i = 0; i < ARRAY_SIZE(kind##_slots); i++) {            \
            if (!kind##_slots[i].in_use) {                                 \
                kind##_slots[i] = (struct type){0};                        \
                kind##_slots[i].in_use = true;                             \
                out = &kind##_slots[i];                                    \
                break;                                                     \
            }                                                              \
        }                                                                  \
        k_mutex_unlock(&kind##_lock);                                      \
        return out;                                                        \
    }                                                                      \
    void alp_z_##kind##_pool_release(struct type *h) {                     \
        if (h == NULL) return;                                             \
        k_mutex_lock(&kind##_lock, K_FOREVER);                             \
        h->in_use = false;                                                 \
        k_mutex_unlock(&kind##_lock);                                      \
    }

/* DEFINE_POOL(adc, ...) removed in Slice 1 -- the new src/adc_dispatch.c
 * owns its own handle pool keyed on the registry's alp_adc layout. */
DEFINE_POOL(dac,     CONFIG_ALP_SDK_MAX_DAC_HANDLES,     alp_dac)
DEFINE_POOL(adc_stream, CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES, alp_adc_stream)

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)
DEFINE_POOL(uart_rx_ringbuf, CONFIG_ALP_SDK_MAX_UART_RX_RINGBUF_HANDLES, alp_uart_rx_ringbuf)
#endif
