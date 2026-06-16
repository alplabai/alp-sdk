/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-abstraction shim consumed by firmware/cc3501e/src/.
 * Each function maps a firmware-level operation onto the CC3501E
 * silicon via TI's SimpleLink CC33xx SDK / driverlib.
 *
 * The default implementations in hal/cc3501e_hw_stub.c keep the
 * protocol path exercisable on the host with no TI SDK (every
 * HW-touching op returns CC3501E_HW_ERR_NOTIMPL).  The real
 * implementations live under hal/ti/ (CC3501E_HAL_BACKEND=ti, linked
 * against TI's SimpleLink SDK on the bench build).
 *
 * v0.1 ("bring-up") surface is intentionally tiny: chip init, the idle
 * housekeeping tick, the factory MAC read, and a deferred self-reset.
 * The Wi-Fi / BLE / GPIO-proxy / camera-enable HAL surfaces land in
 * v0.2+ alongside their protocol handlers (see docs/cc3501e-bridge.md
 * "v0.x roadmap").
 */

#ifndef CC3501E_BRIDGE_HAL_CC3501E_HW_H
#define CC3501E_BRIDGE_HAL_CC3501E_HW_H

#include <stdint.h>

/* Return codes.  0 = success; negatives are operation-independent
 * failure classes (mirrors the gd32-bridge BRIDGE_HW_* convention). */
#define CC3501E_HW_OK          0
#define CC3501E_HW_ERR_NOTIMPL -1 /* op not wired on this backend (stub)   */
#define CC3501E_HW_ERR_IO      -2 /* peripheral / radio access failed       */
#define CC3501E_HW_ERR_INVAL   -3 /* bad argument                           */

/* --------------------------------------------------------------- */
/* Lifecycle                                                         */
/* --------------------------------------------------------------- */

/* One-time chip bring-up: clocks, power, and any board-level pin setup
 * the firmware owns before a transport is started.  No-op on the stub. */
void cc3501e_hw_init(void);

/* Periodic housekeeping, run on each WFI wakeup from main().  No-op on
 * this firmware rev; reserved for watchdog kick / deferred work. */
void cc3501e_hw_tick(void);

/* --------------------------------------------------------------- */
/* Meta operations                                                   */
/* --------------------------------------------------------------- */

/* Read the CC3501E's 6-byte factory MAC into @p mac (TI wire order).
 * Returns CC3501E_HW_OK on success, CC3501E_HW_ERR_NOTIMPL on the stub
 * backend (the protocol layer maps that to RESP_ERR_NOT_READY). */
int cc3501e_hw_get_mac(uint8_t mac[6]);

/* Request a soft self-reset.  The implementation DEFERS the actual
 * reboot until the in-flight reply has been clocked back to the host
 * (so the host sees the CMD_RESET ack), then resets the chip.  No-op on
 * the stub backend. */
void cc3501e_hw_request_reset(void);

#endif /* CC3501E_BRIDGE_HAL_CC3501E_HW_H */
