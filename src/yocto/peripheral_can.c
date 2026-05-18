/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace CAN backend for <alp/can.h>.  Stub awaiting a real
 * SocketCAN (`AF_CAN` / PF_CAN) implementation.  Every entry point
 * stamps ALP_ERR_NOSUPPORT on the process-wide last-error slot and
 * returns the documented error sentinel so apps detect-and-fallback
 * via `alp_last_error()`.  Real backend tracked in VERSIONS.md
 * alongside the rest of the Yocto first-class peripheral work.
 */

#if !defined(__linux__)
#error "peripheral_can.c (yocto backend) requires a Linux target"
#endif

#include <stddef.h>
#include <stdint.h>

#include "alp/can.h"
#include "alp/peripheral.h"
#include "alp_internal.h"

alp_can_t *alp_can_open(const alp_can_config_t *cfg)
{
    (void)cfg;
    alp_internal_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
}

alp_status_t alp_can_start(alp_can_t *can)
{
    (void)can;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_can_stop(alp_can_t *can)
{
    (void)can;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_can_send(alp_can_t *can, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
    (void)can;
    (void)frame;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_can_add_filter(alp_can_t *can, const alp_can_filter_t *filter,
                                alp_can_rx_cb_t cb, void *user, int32_t *filter_id_out)
{
    (void)can;
    (void)filter;
    (void)cb;
    (void)user;
    if (filter_id_out != NULL) {
        *filter_id_out = -1;
    }
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_can_remove_filter(alp_can_t *can, int32_t filter_id)
{
    (void)can;
    (void)filter_id;
    return ALP_ERR_NOSUPPORT;
}

void alp_can_close(alp_can_t *can)
{
    (void)can;
}
