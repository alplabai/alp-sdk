/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clean-room register model for the Alif Ensemble LPTIMER (low-power timer),
 * driven as a Zephyr counter-class device (compatible "alif,lptimer").  On the
 * E1M-AEN801 / Ensemble E8 SoM this is the always-on LPTIMER block at
 * lptimer@42001000: four independent 32-bit DOWN-counters (channels 0..3),
 * clocked from the VBAT-domain low-frequency source (32 kHz / 128 kHz / external
 * / cascade), each with an underflow (count-reaches-zero) interrupt.
 *
 * Every #define below is TRANSCRIBED (value-only, clean-room -- the DFP source
 * is NOT copied) from the Alif DFP, each with an inline citation.  This block is
 * DISTINCT from the LPRTC (snps,dw-apb-rtc, lprtc@42000000, counter_dw_rtc.c)
 * and from the UTIMER (alif,utimer-counter): a separate always-on IP with its
 * own register map and its own four NVIC lines.
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 -- a thin in-tree Zephyr driver written directly over the
 * memory-mapped LPTIMER registers (the alifsemi/zephyr_alif fork ships NO
 * Zephyr LPTIMER counter driver and NO "alif,lptimer" binding, and hal_alif
 * exposes no Zephyr device for it, so there is no Tier-2 fork path to consume
 * or retire onto; the only upstream code is the proprietary DFP CMSIS driver,
 * which is not consumable).  BENCH-UNVERIFIED.  INTERIM until E8 bench, then
 * revisit.  See docs/adr/0017.
 * ====================================================================
 */

#ifndef ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_H_
#define ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_H_

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-channel register block (LPTIMER_LPTIMER_CHANNEL_CFG_Type, 0x14 bytes/ch).
 * Channel n config block starts at base + n * LPTIMER_CHANNEL_STRIDE.
 * Offsets transcribed from Alif DFP Device/soc/AE822FA0E5597/include/rtss_he/
 * soc.h (LPTIMER_LPTIMER_CHANNEL_CFG_Type: LPTIMER_LOADCOUNT @0x00,
 * LPTIMER_CURRENTVAL @0x04, LPTIMER_CONTROLREG @0x08, LPTIMER_EOI @0x0C; struct
 * Size = 20 (0x14)).
 */
#define LPTIMER_CH_LOADCOUNT   0x00U /* Alif DFP soc.h LPTIMER_LOADCOUNT  (RW, +0x00) */
#define LPTIMER_CH_CURRENTVAL  0x04U /* Alif DFP soc.h LPTIMER_CURRENTVAL (RO, +0x04) */
#define LPTIMER_CH_CONTROLREG  0x08U /* Alif DFP soc.h LPTIMER_CONTROLREG (RW, +0x08) */
#define LPTIMER_CH_EOI         0x0CU /* Alif DFP soc.h LPTIMER_EOI        (RO, +0x0C) */
#define LPTIMER_CHANNEL_STRIDE 0x14U /* Alif DFP soc.h CHANNEL_CFG_Type Size = 20 (0x14) */

/*
 * Block-global registers (after the four per-channel config blocks + reserved).
 * Offsets transcribed from Alif DFP soc.h LPTIMER_Type:
 * LPTIMERS_INTSTATUS @0xA0, LPTIMERS_EOI @0xA4, LPTIMERS_RAWINTSTATUS @0xA8.
 */
#define LPTIMER_INTSTATUS    0xA0U /* Alif DFP soc.h LPTIMERS_INTSTATUS    (RO, +0xA0) */
#define LPTIMER_EOI_ALL      0xA4U /* Alif DFP soc.h LPTIMERS_EOI          (RO, +0xA4) */
#define LPTIMER_RAWINTSTATUS 0xA8U /* Alif DFP soc.h LPTIMERS_RAWINTSTATUS (RO, +0xA8) */

/*
 * LPTIMER_CONTROLREG bit definitions.  Transcribed from Alif DFP
 * drivers/include/lptimer.h:
 *   LPTIMER_CONTROL_REG_TIMER_ENABLE_BIT         0x01U
 *   LPTIMER_CONTROL_REG_TIMER_MODE_BIT           0x02U
 *   LPTIMER_CONTROL_REG_TIMER_INTERRUPT_MASK_BIT 0x04U
 *   LPTIMER_CONTROL_REG_TIMER_PWM_BIT            0x08U
 *
 * MODE bit semantics (Alif DFP lptimer.h inline helpers):
 *   set_mode_freerunning() CLEARS the MODE bit -> free-running (load 0xFFFFFFFF,
 *   count down, reload on underflow); set_mode_userdefined() SETS it -> reload
 *   the user LOADCOUNT on underflow.
 */
#define LPTIMER_CTRL_ENABLE   BIT(0) /* Alif DFP lptimer.h ..._TIMER_ENABLE_BIT (0x01) */
#define LPTIMER_CTRL_MODE     BIT(1) /* Alif DFP lptimer.h ..._TIMER_MODE_BIT   (0x02) */
#define LPTIMER_CTRL_INT_MASK BIT(2) /* Alif DFP lptimer.h ..._TIMER_INTERRUPT_MASK_BIT (0x04) */
#define LPTIMER_CTRL_PWM      BIT(3) /* Alif DFP lptimer.h ..._TIMER_PWM_BIT    (0x08) */

/* Free-running reload value (Alif DFP lptimer.h lptimer_load_max_count: 0xFFFFFFFF). */
#define LPTIMER_LOAD_MAX 0xFFFFFFFFU /* Alif DFP lptimer.h load_max_count */

/*
 * VBAT TIMER_CLKSEL -- the per-channel LPTIMER input-clock select, in the
 * always-on VBAT domain (NOT in the LPTIMER block itself).
 *
 * Register: VBAT_BASE + 0x04.  VBAT_BASE = 0x1A609000 (Alif DFP soc.h
 * VBAT_BASE; also zephyr/soc-bridge/alif/soc_common.h:43), TIMER_CLKSEL at
 * struct offset +0x04 (Alif DFP soc.h VBAT_Type: GPIO_CTRL @0x00, TIMER_CLKSEL
 * @0x04) -> absolute 0x1A609004.
 *
 * Field layout (Alif DFP drivers/include/sys_ctrl_lptimer.h select_lptimer_clk):
 *   a 2-bit clock-source field per channel, channel n at bit (n << 2):
 *   ch0 bits[1:0], ch1 bits[5:4], ch2 bits[9:8], ch3 bits[13:12].
 * Source values (Alif DFP sys_ctrl_lptimer.h enum LPTIMER_CLK_SRC):
 *   0 = 32 kHz, 1 = 128 kHz, 2 = external, 3 = cascade.
 * NOTE: ch0 and ch2 do NOT support the cascade source (Alif DFP Driver_LPTIMER.c
 * ARM_LPTIMER_Initialize) -- enforced in the driver.
 */
#define LPTIMER_VBAT_TIMER_CLKSEL    0x1A609004U /* Alif DFP soc.h VBAT_BASE(0x1A609000)+0x04 */
#define LPTIMER_CLKSEL_FIELD_MSK     0x3U /* Alif DFP sys_ctrl_lptimer.h TIMER_CLKSEL_Msk (3<<0) */
#define LPTIMER_CLKSEL_FIELD_POS(ch) ((ch) << 2U) /* Alif DFP sys_ctrl_lptimer.h (channel << 2U) */

/* Clock-source selector values (Alif DFP sys_ctrl_lptimer.h enum LPTIMER_CLK_SRC). */
#define LPTIMER_CLK_SRC_32K     0x0U /* Alif DFP sys_ctrl_lptimer.h LPTIMER_CLK_SOURCE_32K */
#define LPTIMER_CLK_SRC_128K    0x1U /* Alif DFP sys_ctrl_lptimer.h LPTIMER_CLK_SOURCE_128K */
#define LPTIMER_CLK_SRC_EXT     0x2U /* Alif DFP sys_ctrl_lptimer.h LPTIMER_CLK_EXT_SOURCE */
#define LPTIMER_CLK_SRC_CASCADE 0x3U /* Alif DFP sys_ctrl_lptimer.h LPTIMER_CLK_SOURCE_CASCADE */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_COUNTER_ALIF_LPTIMER_H_ */
