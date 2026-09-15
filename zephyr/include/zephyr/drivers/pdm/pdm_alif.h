/*
 * Copyright (C) 2025 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PDM_PDM_ALIF_H_
#define ZEPHYR_INCLUDE_DRIVERS_PDM_PDM_ALIF_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDM_MAX_FIR_COEFFICIENT 18

/* These names encode only the PDM bit clock, NOT the resulting sample rate
 * (issue #2133 round 2 -- PDM_MODE_STANDARD_VOICE_512_CLK_FRQ was assumed to
 * mean "16 kHz" from its name alone and shipped that way; it is actually
 * 8 kHz). Fs = clock / decimation. Grounded per-mode below (HWRM Table
 * 15-118); modes without a comment have not been checked against that table
 * in this repo yet -- don't assume the name states the rate.
 */
#define PDM_MODE_MICROPHONE_SLEEP                  0x00UL /* 128 kHz clk; sampling disabled */
#define PDM_MODE_STANDARD_VOICE_512_CLK_FRQ        0x01UL /* 512 kHz clk, decim 64 -> 8 kHz Fs */
#define PDM_MODE_HIGH_QUALITY_512_CLK_FRQ          0x02UL /* 512 kHz clk, decim 32 -> 16 kHz Fs */
#define PDM_MODE_HIGH_QUALITY_768_CLK_FRQ          0x03UL /* 768 kHz clk, decim 48 -> 16 kHz Fs */
#define PDM_MODE_HIGH_QUALITY_1024_CLK_FRQ         0x04UL /* 1024 kHz clk, decim 64 -> 16 kHz Fs */
#define PDM_MODE_WIDE_BANDWIDTH_AUDIO_1536_CLK_FRQ 0x05UL /* 1536 kHz clk, decim 48 -> 32 kHz Fs */
#define PDM_MODE_FULL_BANDWIDTH_AUDIO_2400_CLK_FRQ 0x06UL /* 2400 kHz clk, decim 50 -> 48 kHz Fs */
#define PDM_MODE_FULL_BANDWIDTH_AUDIO_3071_CLK_FRQ 0x07UL /* 3072 kHz clk, decim 64 -> 48 kHz Fs */
#define PDM_MODE_ULTRASOUND_4800_CLOCK_FRQ 0x08UL /* 4800 kHz clk; Fs/decim not yet grounded here */
#define PDM_MODE_ULTRASOUND_96_SAMPLING_RATE \
	0x09UL /* 4800 kHz clk, decim 25 -> 192 kHz Fs; the "96" names the bandwidth, not Fs */

/* PDM channels */
#define PDM_AUDIO_CHANNEL_0 (0x00)
#define PDM_AUDIO_CHANNEL_1 (0x01)
#define PDM_AUDIO_CHANNEL_2 (0x02)
#define PDM_AUDIO_CHANNEL_3 (0x03)
#define PDM_AUDIO_CHANNEL_4 (0x04)
#define PDM_AUDIO_CHANNEL_5 (0x05)
#define PDM_AUDIO_CHANNEL_6 (0x06)
#define PDM_AUDIO_CHANNEL_7 (0x07)

/* PDM mask channels */
#define PDM_MASK_CHANNEL_0 (1 << PDM_AUDIO_CHANNEL_0)
#define PDM_MASK_CHANNEL_1 (1 << PDM_AUDIO_CHANNEL_1)
#define PDM_MASK_CHANNEL_2 (1 << PDM_AUDIO_CHANNEL_2)
#define PDM_MASK_CHANNEL_3 (1 << PDM_AUDIO_CHANNEL_3)
#define PDM_MASK_CHANNEL_4 (1 << PDM_AUDIO_CHANNEL_4)
#define PDM_MASK_CHANNEL_5 (1 << PDM_AUDIO_CHANNEL_5)
#define PDM_MASK_CHANNEL_6 (1 << PDM_AUDIO_CHANNEL_6)
#define PDM_MASK_CHANNEL_7 (1 << PDM_AUDIO_CHANNEL_7)

/**
 * @brief: These channel configurations are specific to each channels
 */
struct pdm_ch_config {
	uint8_t  ch_num;                               /* Channel number */
	uint32_t ch_fir_coef[PDM_MAX_FIR_COEFFICIENT]; /* Channel FIR filter Coefficient */
	uint32_t ch_iir_coef;                          /* Channel IIR Filter Coefficient */
};

/**
 * @brief	PDM channel configurations
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param 	cnfg: Pointer to pdm_ch_config
 */
void pdm_channel_config(const struct device *dev, struct pdm_ch_config *cnfg);

/**
 * @brief	PDM module can be programmed to operate in different modes.
		PDM modes applies to all channels. Therefore, all channels
		operate in the same mode at a given time.
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param	mode	: select pdm mode.
 */
void pdm_mode(const struct device *dev, uint8_t mode);

/**
 * @brief	PDM channel phase control
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param	ch_num	: pdm channel
 * @param	ch_phase: pdm channel phase control value
 */
void pdm_set_ch_phase(const struct device *dev, uint8_t ch_num, uint32_t ch_phase);

/**
 * @brief	PDM channel gain control
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param	ch_num	: pdm channel
 * @param	ch_gain	: pdm channel gain control value
 */
void pdm_set_ch_gain(const struct device *dev, uint8_t ch_num, uint32_t ch_gain);

/**
 * @brief	PDM channel Peak detector threshold
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param	ch_num	: pdm channel
 * @param	ch_peak_detect_th : pdm channel Peak detector threshold value
 */
void pdm_set_peak_detect_th(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_th);

/**
 * @brief	PDM channel Peak detector interval
 * @param	dev	: Pointer to the device structure for the driver instance.
 * @param	ch_num	: pdm channel
 * @param	ch_peak_detect_itv: pdm channel Peak detector interval value
 */
void pdm_set_peak_detect_itv(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_itv);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_PDM_PDM_ALIF_H_ */
