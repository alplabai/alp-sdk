/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- DAC outputs.
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
/* DAC channels.                                                      */
/* ----------------------------------------------------------------- */

/* E1M DAC0/DAC1 -> physical DAC peripheral + output + pad.  Sourced
 * from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`; PA4 is the
 * GD32G5x3's stock DAC0_OUT0 alt-function and PA6 is DAC1_OUT0 per
 * the datasheet's pin alt-function table. */
const gd32_dac_ch_t dac_channels[] = {
	[0] = { DAC0, DAC_OUT0, GPIOA, GPIO_PIN_4 }, /* E1M DAC0 -> PA4 */
	[1] = { DAC1, DAC_OUT0, GPIOA, GPIO_PIN_6 }, /* E1M DAC1 -> PA6 */
};
_Static_assert(sizeof(dac_channels) / sizeof(dac_channels[0]) == DAC_CHANNEL_COUNT,
               "dac_channels size must match DAC_CHANNEL_COUNT");

/* DAC VREF.  The V2N's analog supply is 1.8 V (maintainer-confirmed
 * 2026-05-13 against the schematic).  Revisit if a future hw-revision
 * moves to a buffered VREFINT source or a different rail.  Full-scale
 * code is 4095 for 12-bit alignment; code = (value_mv * 4095) /
 * VREF_mV with overflow clamped. */
#define DAC_VREF_MV    1800u
#define DAC_FULL_SCALE 4095u

int bridge_hw_dac_set(uint8_t channel, uint16_t value_mv)
{
	if (channel >= DAC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
	if (!vref_ready_check()) return BRIDGE_HW_ERR_IO; /* dead reference -- fail loud */
	/* mV -> 12-bit code, clamp on over-range (the host doesn't see
     * BRIDGE_HW_ERR_RANGE for this case -- saturating is friendlier
     * than rejecting a request the user can recover from by reading
     * back the actual programmed value). */
	uint32_t code = ((uint32_t)value_mv * DAC_FULL_SCALE) / DAC_VREF_MV;
	if (code > DAC_FULL_SCALE) code = DAC_FULL_SCALE;
	dac_data_set(
	    dac_channels[channel].periph, dac_channels[channel].out, DAC_ALIGN_12B_R, (uint16_t)code);
	return BRIDGE_HW_OK;
}

int bridge_hw_dac_get(uint8_t channel, uint16_t *value_mv)
{
	if (value_mv == 0) return BRIDGE_HW_ERR_INVAL;
	*value_mv = 0u;
	if (channel >= DAC_CHANNEL_COUNT) return BRIDGE_HW_ERR_RANGE;
	if (!vref_ready_check()) return BRIDGE_HW_ERR_IO; /* dead reference -- fail loud */
	/* dac_output_value_get reads the DAC's hold register (the value
     * currently driving the pad), not the input setpoint -- this is
     * what we want for read-back: callers see the actual code in
     * play, which may differ from the last `set` if the DAC was
     * concurrent-paired or DMA-driven elsewhere. */
	uint16_t code = dac_output_value_get(dac_channels[channel].periph, dac_channels[channel].out);
	if (code > DAC_FULL_SCALE) code = DAC_FULL_SCALE;
	*value_mv = (uint16_t)(((uint32_t)code * DAC_VREF_MV) / DAC_FULL_SCALE);
	return BRIDGE_HW_OK;
}
