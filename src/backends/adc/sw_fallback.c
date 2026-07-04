/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software ADC fallback backend.  Registered (priority 0, "*") only
 * to keep the adc class section non-empty for the registry's
 * __start_/__stop_ linker bounds.  native_sim has no real ADC
 * controller, so open() reports ALP_ERR_NOT_READY (relayed as NULL +
 * last_error, matching the adc-voltmeter example's native_sim path);
 * the saw-wave read_raw below is retained but unreachable while open
 * fails.
 *
 * @par Cost: ROM ~1.5 KB, RAM 4 B (a single counter for the saw)
 * @par Performance: open short-circuits to NOT_READY; no conversion.
 *      For native_sim build/test only -- never use on production.
 */

#include <stdint.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"

static int32_t _saw_counter = 0;

static alp_status_t
sw_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *state, alp_capabilities_t *caps_out)
{
	/* No real ADC controller / channel->pin resolution on native_sim:
     * report the device isn't ready so the dispatcher returns NULL +
     * last_error = NOT_READY (the adc-voltmeter example expects exactly
     * this on native_sim). */
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOT_READY;
}

static alp_status_t sw_read_raw(alp_adc_backend_state_t *state, int32_t *raw_out)
{
	(void)state;
	*raw_out     = _saw_counter;
	_saw_counter = (_saw_counter + 137) & 0x0FFF; /* saw mod 4096 */
	return ALP_OK;
}

static const alp_adc_ops_t sw_ops = {
	.open     = sw_open,
	.read_raw = sw_read_raw,
	.close    = NULL,
};

/* Export the adc static-archive anchor the dispatcher references (#368). */
ALP_BACKEND_ANCHOR_DEFINE(adc);

ALP_BACKEND_REGISTER(adc,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &sw_ops,
                         .probe       = NULL,
                     });
