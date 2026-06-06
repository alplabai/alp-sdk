/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- system power modes.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.3) -- system power-mode set                             */
/* ----------------------------------------------------------------- */

/* ALP_POWER_WAKE_* bits the firmware supports (mirrors the wire
 * encoding in <alp/power.h>).
 *
 * Mapping notes per V2N hardware reality:
 *
 *   - GPIO : routes through PMU_WAKEUP_PIN0..4 -- five fixed pads
 *            on the GD32G553 + board wires the desired triggers
 *            onto them.  Landed §C.15c.
 *   - RTC  : RTC alarm 0 fires on a scheduled wallclock; the
 *            wakeup timer also surfaces under this bit so the
 *            firmware uses the timer (simpler than absolute-time
 *            alarms in a partial bring-up).  Landed §C.25.
 *   - TIMER: any non-zero `wake_after_ms` -- same RTC wakeup-timer
 *            path as RTC.  The bit is redundant when wake_after_ms
 *            > 0 (the timer wakes the chip implicitly per the
 *            <alp/power.h> contract); honouring the bit explicitly
 *            lets a caller arm a "wake on next tick" without a
 *            specific deadline.  Landed §C.25.
 *   - UART_RX / USB / ETH_LINK : no hardware path on the GD32G5
 *            (no LPUART wake / no USB OTG / no MAC).  Future SoCs
 *            on the bridge slot could populate these via the same
 *            opcode; today the firmware rejects them so the host
 *            knows the request is moot.
 */
#define POWER_WAKE_RTC 0x00000001u
#define POWER_WAKE_GPIO 0x00000002u
#define POWER_WAKE_UART_RX 0x00000004u
#define POWER_WAKE_TIMER 0x00000008u
#define POWER_WAKE_USB 0x00000010u
#define POWER_WAKE_ETH_LINK 0x00000020u
#define POWER_WAKE_MASK_SUPPORTED (POWER_WAKE_RTC | POWER_WAKE_GPIO | POWER_WAKE_TIMER)
#define POWER_WAKE_MASK_HW_GATED (POWER_WAKE_UART_RX | POWER_WAKE_USB | POWER_WAKE_ETH_LINK)

/* RTC wakeup timer LSB: with IRC32K (~32 kHz internal) clock and
 * the /16 divider, the timer ticks at 32000/16 = 2000 Hz -- 0.5 ms
 * per tick.  Max wake = 65535 / 2000 = 32.7 s.  Longer waits would
 * need the CKSPRE_2EXP16 mode which sits in a future commit. */
#define POWER_WAKE_LSB_HZ 2000u
#define POWER_WAKE_TIMER_MAX_MS (65535u * 1000u / POWER_WAKE_LSB_HZ)

/* One-time RTC + LSI bring-up that arms the wakeup timer.  Idempotent
 * across multiple power_mode_set calls -- the LSI stays enabled, the
 * RTC source latches to IRC32K once.  Failure (LSI never stabilises,
 * write-protected register won't unlock) leaves rtc_wakeup_ready
 * false and bridge_hw_power_mode_set returns NOSUPPORT for any
 * timer-bearing call. */
static bool rtc_wakeup_ready = false;

static bool rtc_wakeup_init_once(void)
{
    if (rtc_wakeup_ready) return true;

    /* Bring up IRC32K (internal LSI) as the RTC clock source. */
    rcu_osci_on(RCU_IRC32K);
    /* Spin until IRC32K stabilises -- typical < 50 us, the upper
     * bound keeps a dead oscillator from hanging the bridge. */
    uint32_t to = 200000u;
    while (--to && RESET == rcu_flag_get(RCU_FLAG_IRC32KSTB)) {
        /* spin */
    }
    if (to == 0u) return false;

    rcu_periph_clock_enable(RCU_PMU);
    pmu_backup_write_enable();
    rcu_rtc_clock_config(RCU_RTCSRC_IRC32K);
    rcu_periph_clock_enable(RCU_RTC);

    rtc_wakeup_ready = true;
    return true;
}

static int rtc_wakeup_arm_ms(uint32_t wake_after_ms)
{
    if (!rtc_wakeup_init_once()) return BRIDGE_HW_ERR_IO;
    if (wake_after_ms > POWER_WAKE_TIMER_MAX_MS) return BRIDGE_HW_ERR_RANGE;

    /* Compute ticks (round up so a sub-LSB request still waits at
     * least one tick rather than zero). */
    uint32_t ticks = (wake_after_ms * POWER_WAKE_LSB_HZ + 999u) / 1000u;
    if (ticks == 0u) ticks = 1u;
    if (ticks > 65535u) ticks = 65535u;

    /* The vendor sequence: disable the wakeup timer, switch its
     * clock source, set the counter, re-enable.  rtc_wakeup_disable
     * may return ERROR if the WTWF flag never sets; treat as IO. */
    if (SUCCESS != rtc_wakeup_disable()) return BRIDGE_HW_ERR_IO;
    if (SUCCESS != rtc_wakeup_clock_set(WAKEUP_RTCCK_DIV16)) return BRIDGE_HW_ERR_IO;
    if (SUCCESS != rtc_wakeup_timer_set((uint16_t)(ticks - 1u))) return BRIDGE_HW_ERR_IO;
    rtc_wakeup_enable();
    return BRIDGE_HW_OK;
}

static void power_wake_pins_enable(uint32_t wake_bitmap)
{
    if ((wake_bitmap & POWER_WAKE_GPIO) == 0u) return;
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN0);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN1);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN2);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN3);
    pmu_wakeup_pin_enable(PMU_WAKEUP_PIN4);
}

int bridge_hw_power_mode_set(uint8_t mode, uint32_t wake_bitmap, uint32_t wake_after_ms)
{
    /* Mode 0 (run) + mode 1 (sleep) are accepted no-ops -- main()'s
     * `for (;;) { __WFI(); bridge_hw_tick(); }` already runs the CPU
     * in WFI between transport interrupts, which IS "sleep" on the
     * GD32G5.  Mode 2 (deep-sleep) + mode 3 (standby) call into the
     * vendor's PMU helpers below.
     *
     * Wake-source semantics: `wake_bitmap` enumerates the explicit
     * sources the host wants armed; `wake_after_ms` is a timed
     * fallback that arms the RTC wakeup timer regardless of the
     * bitmap (per the <alp/power.h> contract: the timer is implicit
     * when wake_after_ms > 0).  Unsupported bits (UART_RX / USB /
     * ETH_LINK on the GD32G5 baseline) reject so the host knows the
     * request was not honoured. */
    if ((wake_bitmap & POWER_WAKE_MASK_HW_GATED) != 0u) return BRIDGE_HW_ERR_NOTIMPL;

    switch (mode) {
    case 0u: /* run -- no-op */
    case 1u: /* sleep -- already in WFI between transport ISRs */
        return BRIDGE_HW_OK;
    case 2u: /* deep-sleep */
        rcu_periph_clock_enable(RCU_PMU);
        power_wake_pins_enable(wake_bitmap);
        if (wake_after_ms != 0u || (wake_bitmap & (POWER_WAKE_RTC | POWER_WAKE_TIMER)) != 0u) {
            const uint32_t ms = (wake_after_ms != 0u) ? wake_after_ms : POWER_WAKE_TIMER_MAX_MS;
            int            rc = rtc_wakeup_arm_ms(ms);
            if (rc != BRIDGE_HW_OK) return rc;
        }
        /* PMU_LDO_LOWPOWER drops the core LDO into its low-power
         * regulation point during deepsleep (saves a few hundred
         * uA at the cost of a slightly slower wakeup); WFI_CMD
         * issues the actual `wfi` instruction that suspends the
         * core.  Returns here once a wakeup source fires. */
        pmu_to_deepsleepmode(PMU_LDO_LOWPOWER, WFI_CMD);
        return BRIDGE_HW_OK;
    case 3u: /* standby */
        rcu_periph_clock_enable(RCU_PMU);
        power_wake_pins_enable(wake_bitmap);
        if (wake_after_ms != 0u || (wake_bitmap & (POWER_WAKE_RTC | POWER_WAKE_TIMER)) != 0u) {
            const uint32_t ms = (wake_after_ms != 0u) ? wake_after_ms : POWER_WAKE_TIMER_MAX_MS;
            int            rc = rtc_wakeup_arm_ms(ms);
            if (rc != BRIDGE_HW_OK) return rc;
        }
        /* Standby powers down the core + SRAM (except backup) and
         * wakes via reset -- pmu_to_standbymode() never returns;
         * the SoC re-runs Reset_Handler when a wakeup source fires.
         * The caller's host link will see the bridge re-issue its
         * handshake on the next transport packet, which is the
         * documented contract. */
        pmu_to_standbymode();
        /* Unreachable in normal operation; keep the return so the
         * compiler doesn't warn about a missing terminator. */
        return BRIDGE_HW_OK;
    default:
        return BRIDGE_HW_ERR_INVAL;
    }
    (void)POWER_WAKE_MASK_SUPPORTED;
}
