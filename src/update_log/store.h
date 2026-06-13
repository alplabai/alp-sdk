/* SPDX-License-Identifier: Apache-2.0
 * Engine seams. The engine touches no hardware -- only these. NOT public. */
#ifndef ALP_UPDATE_LOG_STORE_H
#define ALP_UPDATE_LOG_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "alp/peripheral.h"

/* Keyed blob store. Host: RAM. Secure (future): TF-M Protected Storage. */
typedef struct {
    alp_status_t (*put)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    alp_status_t (*get)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len);
    alp_status_t (*erase)(void *ctx, const char *key);
    void *ctx;
} alp_secure_store_if;

/* Monotonic counter. Host: in-process. Secure (future): PSA NV / OPTIGA. */
typedef struct {
    alp_status_t (*read)(void *ctx, uint32_t id, uint64_t *out_val);
    alp_status_t (*increment)(void *ctx, uint32_t id, uint64_t *out_val);
    void *ctx;
} alp_monotonic_counter_if;

#endif /* ALP_UPDATE_LOG_STORE_H */
