/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace I2S backend for <alp/i2s.h>.  Stub awaiting a real
 * ALSA-hwparams implementation over the kernel's I2S DAI.  Every
 * entry point stamps ALP_ERR_NOSUPPORT on the process-wide
 * last-error slot and returns the documented error sentinel so apps
 * detect-and-fallback via `alp_last_error()`.  Real backend tracked
 * in VERSIONS.md alongside the rest of the Yocto first-class
 * peripheral work.
 */

#if !defined(__linux__)
#error "peripheral_i2s.c (yocto backend) requires a Linux target"
#endif

#include <stddef.h>
#include <stdint.h>

#include "alp/i2s.h"
#include "alp/peripheral.h"
#include "alp_internal.h"

alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg)
{
    (void)cfg;
    alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_i2s_start(alp_i2s_t *i2s)
{
    (void)i2s;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_i2s_stop(alp_i2s_t *i2s)
{
    (void)i2s;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_i2s_write(alp_i2s_t *i2s, const void *block, size_t bytes, uint32_t timeout_ms)
{
    (void)i2s;
    (void)block;
    (void)bytes;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_i2s_read(alp_i2s_t *i2s, void *block, size_t bytes, size_t *bytes_out,
                         uint32_t timeout_ms)
{
    (void)i2s;
    (void)block;
    (void)bytes;
    (void)timeout_ms;
    if (bytes_out != NULL) {
        *bytes_out = 0;
    }
    return ALP_ERR_NOSUPPORT;
}

void alp_i2s_close(alp_i2s_t *i2s)
{
    (void)i2s;
}
