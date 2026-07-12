/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- cross-TU seam for the cc3501e_hw_ti_*.c
 * hardware-subsystem split (issue #703, #461 Phase B).
 *
 * The pre-split cc3501e_hw_ti.c kept a handful of items file-static; two of
 * them are now genuinely shared between the split translation units:
 *
 *   - reply_drained / ota_reboot_pending / ota_reboot_rc: the deferred-
 *     reboot latch.  Owned + defined by cc3501e_hw_ti.c (the platform/
 *     lifecycle TU -- cc3501e_hw_tick() sequences the actual reboot), but
 *     armed from cc3501e_hw_ti_ota.c's FINISH/PROMOTE bodies.
 *   - cc3501e_hw_ota_pump(): defined in cc3501e_hw_ti_ota.c, called from
 *     cc3501e_hw_tick() (cc3501e_hw_ti.c) on every idle wakeup.
 *   - cc3501e_hw_wifi_lazy_start(): defined in cc3501e_hw_ti_wifi.c, called
 *     from cc3501e_hw_ti_ble.c's BLE_ENABLE body (BLE shares the HIF with
 *     Wi-Fi, so Wlan_Start must run first -- WIFI_BLE_INTEGRATION.md).
 *
 * NOT a public HAL surface -- only the ti backend's own .c files include
 * this.  The public cc3501e_hw_* contract is unchanged and stays in
 * ../cc3501e_hw.h.
 */

#ifndef CC3501E_HAL_TI_CC3501E_HW_TI_INTERNAL_H
#define CC3501E_HAL_TI_CC3501E_HW_TI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* Deferred-reboot latch (defined in cc3501e_hw_ti.c -- see cc3501e_hw_tick).
 * reply_drained: the in-flight reply has fully clocked back to the host
 * (cc3501e_hw_notify_reply_sent); gates BOTH the CMD_RESET reboot and the
 * OTA swap-reboot below so the host always sees the ack first.
 * ota_reboot_pending / ota_reboot_rc: armed by the OTA FINISH/PROMOTE
 * bodies (cc3501e_hw_ti_ota.c); cc3501e_hw_tick() performs the actual
 * psa_fwu_request_reboot() and publishes its result. */
extern volatile bool   reply_drained;
extern volatile bool   ota_reboot_pending;
extern volatile int8_t ota_reboot_rc;

/* Runs a queued OTA op's flash work off the SPI ISR (cc3501e_hw_ti_ota.c);
 * called from cc3501e_hw_tick() on the bring-up task. */
void cc3501e_hw_ota_pump(void);

/* Bring the CC35xx Wi-Fi host up to STA role once (cc3501e_hw_ti_wifi.c).
 * Shared with cc3501e_hw_ti_ble.c: BLE_ENABLE lazy-starts Wi-Fi first
 * (shared HIF) before bringing up the NimBLE host.  Returns CC3501E_HW_OK /
 * CC3501E_HW_ERR_IO (see ../cc3501e_hw.h). */
int cc3501e_hw_wifi_lazy_start(void);

#endif /* CC3501E_HAL_TI_CC3501E_HW_TI_INTERNAL_H */
