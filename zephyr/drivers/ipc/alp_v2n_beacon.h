/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alp_v2n_beacon.h
 * @brief CM33 half of the RZ/V2N A55 <-> CM33 AMP link that every CM33
 *        firmware attaching to src/backends/rpc/yocto_uio_drv.c shares: the
 *        liveness beacon in the rsctbl page and the MHU-B reverse doorbell.
 *
 * The beacon layout, magic and attach-epoch protocol are in
 * <alp/protocol/amp_beacon.h>; the doorbell register map is in
 * <alp/protocol/v2n_mhu_doorbell.h>.  Used by the ipc_service backend
 * (zephyr/drivers/ipc/ipc_alp_v2n_rpmsg.c) and by the raw OpenAMP example
 * (examples/multicore/rpmsg-v2n/m33_sm), so the two cannot drift apart.
 */

#ifndef ALP_V2N_BEACON_H
#define ALP_V2N_BEACON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Publish the v2 beacon (attach epoch 0, version 2, magic last) and
 *        start the ~1 Hz heartbeat.
 *
 * Call as early as possible: a Linux-side read of the beacon then proves the
 * CM33 is alive, the rsctbl window is backed and the address translation is
 * right.
 */
void alp_v2n_beacon_start(void);

/**
 * @brief Flip the attach epoch's parity.
 *
 * Call after the virtio device is bound (even -> odd) and after it is torn
 * down on an A55 attach reset (odd -> even).  The A55 backend requests a
 * reset whenever it reads odd and waits for even.
 */
void alp_v2n_beacon_epoch_bump(void);

/** @brief Ring the A55 (MHU-B reverse doorbell, see v2n_mhu_doorbell.h). */
void alp_v2n_doorbell_ring(void);

#ifdef __cplusplus
}
#endif

#endif /* ALP_V2N_BEACON_H */
