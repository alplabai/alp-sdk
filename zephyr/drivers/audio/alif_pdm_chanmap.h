/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr dmic channel-map -> Alif PDM hardware channel-enable mask
 * translation for the vendored Alif PDM driver (see alif_pdm.c for the full
 * ADR 0017 provenance banner). Split into its own tiny header, alongside
 * alif_pdm_burst_plan.h, so tests/unit/alif_pdm_chanmap can exercise the
 * exact translation dmic_alif_pdm_configure() runs, on the host, with no
 * DEVICE_MMIO/devicetree/PDM-instance involved.
 *
 * ------------------------------- the mapping -------------------------------
 * Zephyr's dmic_build_channel_map()/dmic_parse_channel_map()
 * (zephyr/include/zephyr/audio/dmic.h) encode each LOGICAL channel as a
 * (PDM controller index, L/R) pair, one nibble per logical channel. This
 * header turns that pair into the Alif PDM_CONFIG_REGISTER HARDWARE
 * channel-enable bit (alif_pdm_reg.h: PDM_CHANNEL_0..7, one bit per HW
 * channel; pdm_alif.h: PDM_MASK_CHANNEL_0..7 name the same bits).
 *
 * hw_channel = (pdm_controller * 2) + lr
 *
 * Grounded two ways, not guessed:
 *   1. The register map itself: PDM_CH0_CH1_AUDIO_OUT, PDM_CH2_CH3_AUDIO_OUT,
 *      PDM_CH4_CH5_AUDIO_OUT, PDM_CH6_CH7_AUDIO_OUT (alif_pdm_reg.h) each pack
 *      TWO hw channels per register -- one PDM controller's stereo (L/R)
 *      output per register, in controller order. That is exactly *2 spacing.
 *   2. The bench-proven examples/aen/aen-pdm-mic-alif, verified on
 *      E1M-AEN803 silicon (docs/test-plan.md: "Live varying PCM captured"):
 *      the E1M-AEN801 SoM routes PDM_C0/D0 to pdm controller 0 and PDM_C2/D2
 *      to pdm controller 2 (metadata/e1m_modules/aen/from-alif.tsv, see the
 *      board overlay), and the working raw bitmask enables HW channels 0,1
 *      for that first pair and 4,5 for the second -- (0*2,0*2+1) and
 *      (2*2,2*2+1). Controller 1 (unused on this SoM) would span HW 2,3.
 *
 * Up to ALIF_PDM_MAX_CONTROLLERS (4) PDM controllers x 2 (L/R) = the 8 HW
 * channels MAX_NUM_CHANNELS names -- a pdm index at or above that cannot be
 * expressed by this register and is rejected, never silently truncated.
 */
#ifndef ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_CHANMAP_H_
#define ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_CHANMAP_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>

#define ALIF_PDM_MAX_CONTROLLERS 4

/**
 * @brief Translate a standard Zephyr dmic channel map into the Alif PDM
 *        hardware channel-enable mask (PDM_CONFIG_REGISTER bits 0..7).
 *
 * @param chan_map_lo  dmic_cfg.channel.req_chan_map_lo (logical channels 0..7)
 * @param chan_map_hi  dmic_cfg.channel.req_chan_map_hi (logical channels 8..15;
 *                     unreachable on this 8-hw-channel part, passed through
 *                     to dmic_parse_channel_map() for completeness only)
 * @param req_num_chan dmic_cfg.channel.req_num_chan -- number of logical
 *                     channels to decode
 * @param mask_out     Receives the 8-bit hardware channel-enable mask on
 *                     success. Left untouched on failure.
 *
 * @return 0 on success. -EINVAL if @p req_num_chan is 0 or exceeds
 *         MAX_NUM_CHANNELS (8), or any requested logical channel names a PDM
 *         controller this hardware cannot express
 *         (>= ALIF_PDM_MAX_CONTROLLERS) -- callers MUST fail the configure
 *         call rather than fall back to a partial/best-effort mask.
 */
static inline int alif_pdm_chanmap_translate(uint32_t chan_map_lo, uint32_t chan_map_hi,
					      uint8_t req_num_chan, uint8_t *mask_out)
{
	uint8_t mask = 0;
	uint8_t ch;

	if (req_num_chan == 0 || req_num_chan > 8) {
		return -EINVAL;
	}

	for (ch = 0; ch < req_num_chan; ch++) {
		uint8_t pdm;
		enum pdm_lr lr;

		dmic_parse_channel_map(chan_map_lo, chan_map_hi, ch, &pdm, &lr);
		if (pdm >= ALIF_PDM_MAX_CONTROLLERS) {
			return -EINVAL;
		}
		mask |= (uint8_t)(1U << ((pdm * 2U) + (uint8_t)lr));
	}

	*mask_out = mask;
	return 0;
}

#endif /* ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_CHANMAP_H_ */
