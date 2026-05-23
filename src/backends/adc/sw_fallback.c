/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software ADC fallback backend.  Returns a deterministic saw-wave
 * on every read; lets examples/ compile and exercise the dispatch
 * path on native_sim without a real ADC.
 *
 * @par Cost: ROM ~1.5 KB, RAM 4 B (a single counter for the saw)
 * @par Performance: 1 sample per call; no DMA; no real conversion.
 *      For native_sim build/test only -- never use on production.
 */

#include <stdint.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"

static int32_t _saw_counter = 0;

static alp_status_t sw_open(const alp_adc_config_t *cfg,
                            alp_adc_backend_state_t *state,
                            alp_capabilities_t *caps_out)
{
    (void)cfg;
    state->reference_uv    = 3300000u;   /* 3.3 V reference */
    state->resolution_bits = 12u;
    state->be_data         = NULL;       /* stateless */
    caps_out->max_resolution_bits = 12u;
    caps_out->max_sample_rate     = 0u;
    caps_out->channel_count       = 8u;
    return ALP_OK;
}

static alp_status_t sw_read_raw(alp_adc_backend_state_t *state,
                                int32_t *raw_out)
{
    (void)state;
    *raw_out = _saw_counter;
    _saw_counter = (_saw_counter + 137) & 0x0FFF;   /* saw mod 4096 */
    return ALP_OK;
}

static const alp_adc_ops_t sw_ops = {
    .open     = sw_open,
    .read_raw = sw_read_raw,
    .close    = NULL,
};

ALP_BACKEND_REGISTER(adc, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &sw_ops,
                         .probe       = NULL,
                     });
