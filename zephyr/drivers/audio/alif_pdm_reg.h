/*
 * Copyright (C) 2025 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM -> retire onto
 * sdk-alif fork, BENCH-UNVERIFIED) ======
 * PDM register map for the vendored Alif Ensemble PDM fork-driver copy
 * (drivers/audio/alif_pdm.c, compatible "alif,alif_pdm").  hal_alif ships no PDM
 * / DMIC class driver, so this header is carried in-tree alongside the driver so
 * both survive a `west update`.  Retire onto the opt-in sdk-alif fork once the
 * pdm node is repointed AND bench-verified.  See
 * docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 */

#ifndef ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_REG_H_
#define ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_REG_H_

#define PDM_CONFIG_REGISTER      (0x0)  /* PDM Audio Control Register 0  */
#define PDM_CTL_REGISTER         (0x4)  /* PDM Audio Control Register 1  */
#define PDM_THRESHOLD_REGISTER   (0x8)  /* FIFO Watermark Register    */
#define PDM_FIFO_STATUS_REGISTER (0xC)  /* FIFO Status Register      */
#define PDM_ERROR_IRQ            (0x10) /* FIFO Error Interrupt Status Register  */
#define PDM_WARN_IRQ             (0x14) /* FIFO Warning Interrupt Status Register*/
#define PDM_AUDIO_DETECT_IRQ     (0x18) /* Audio Detection Interrupt Status Register */
#define PDM_INTERRUPT_REGISTER   (0x1C) /* Interrupt Enable Register  */
#define PDM_CH0_CH1_AUDIO_OUT    (0x20) /* Channels 0 and 1 Audio Output Register  */
#define PDM_CH2_CH3_AUDIO_OUT    (0x24) /* Channels 2 and 3 Audio Output Register  */
#define PDM_CH4_CH5_AUDIO_OUT    (0x28) /* Channels 4 and 5 Audio Output Register  */
#define PDM_CH6_CH7_AUDIO_OUT    (0x2C) /* Channels 6 and 7 Audio Output Register  */
#define PDM_CH_FIR_COEF          (0x40) /* Channel (n) FIR Filter Coefficient  */
#define PDM_CH_IIR_COEF_SEL      (0xC0) /* Channel (n) IIR Filter Coefficient  */
#define PDM_CH_PHASE             (0xC4) /* Channel (n) Phase Control Register  */
#define PDM_CH_GAIN              (0xC8) /* Channel (n) Gain Control Register  */
/* GAIN field is bits [11:0], unsigned 8.4 fixed-point (issue #2133 round
 * 4f -- Alif SVD AE822FA0E5597BS0_CM55_HP_View.svd, PDM_CH_GAIN register,
 * GAIN field; matches the Alif DFP's PDM_MAX_GAIN_CTRL 0xFFFU,
 * drivers/include/pdm.h). A value above this overflows the field and
 * wraps to 0, muting the channel. */
#define PDM_CH_GAIN_MAX (0xFFFU)
#define PDM_CH_PKDET_TH          (0xCC) /* Channel (n) Peak Detector Threshold Register  */
#define PDM_CH_PKDET_ITV         (0xD0) /* Channel (n) Peak Detector Interval Register  */

#define PDM_FIFO_CLEAR            (1U << 31U)   /* To clear FIFO clear bit  */
#define PDM_AUDIO_DETECT_IRQ_STAT (0xFFU << 8U) /* Audio detect interrupt  */
#define PDM_FIFO_ALMOST_FULL_IRQ  (0x1U << 0U)  /* FIFO almost full Interrupt*/
/* PDM_INTERRUPT_REGISTER's (enable) bit layout ONLY -- see
 * PDM_ERROR_IRQ_FIFO_OVERFLOW_STAT below for the DIFFERENT bit position
 * this same condition uses in the PDM_ERROR_IRQ STATUS register (issue
 * #2133 round 4d).
 */
#define PDM_FIFO_OVERFLOW_IRQ (0x1U << 1U) /* FIFO overflow Interrupt (enable reg) */
/* PDM_ERROR_IRQ (status register, offset 0x10) bit 0 = FIFO_OVERFLOW_IRQ,
 * read-clear (Alif SVD AE822FA0E5597BS0_CM55_HP_View.svd, PDM_ERROR_IRQ
 * register, FIFO_OVERFLOW_IRQ field, bitRange [0:0], readAction "clear";
 * matches the Alif DFP's PDM_INTERRUPT_STATUS_VALUE == 0x1U check against
 * this same register, drivers/source/pdm.c + include/pdm.h). NOT the same
 * bit position as PDM_FIFO_OVERFLOW_IRQ above -- that constant names bit 1
 * of the DIFFERENT PDM_INTERRUPT_REGISTER (enable) register layout; reusing
 * it to test PDM_ERROR_IRQ's status bit tested the wrong bit entirely
 * (issue #2133 round 4d).
 */
#define PDM_ERROR_IRQ_FIFO_OVERFLOW_STAT (0x1U << 0U)
#define PDM_BYPASS_IIR                   (2U)    /* Bypass DC blocking IIR filter*/
#define PDM_CHANNEL_ENABLE               (0xFFU) /* To check the which channel is enabled*/

#define PDM_CLK_MODE           (16U)  /* PDM clock frequency mode  */
#define MAX_DATA_ITEMS         (8U)   /* Max data items      */
#define MAX_NUM_CHANNELS       (8U)   /* Max number of channel   */
#define MAX_QUEUE_LEN          (100U) /* Max Queue length      */
#define PDM_CH_OFFSET          (0x100U)
#define PDM_CLK_MODE_MASK      (0xFU << 16U) /* PDM_CTL0[19:16] PDM_MODE: a FIELD */
#define PDM_FIFO_STAT_CNT_MASK (0xFU)        /* PDM_FIFO_STAT[3:0] CNT; 31-4 reserved */

#define PDM_CHANNEL_0 (1U << 0U)
#define PDM_CHANNEL_1 (1U << 1U)
#define PDM_CHANNEL_2 (1U << 2U)
#define PDM_CHANNEL_3 (1U << 3U)
#define PDM_CHANNEL_4 (1U << 4U)
#define PDM_CHANNEL_5 (1U << 5U)
#define PDM_CHANNEL_6 (1U << 6U)
#define PDM_CHANNEL_7 (1U << 7U)

#endif /* ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_REG_H_ */
