/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * vendor-ext-composability -- one pad, three ways.
 *
 * The Alp SDK's vendor-extension layer (<alp/ext/<vendor>/...>) is
 * ADDITIVE, never exclusive.  Claiming a vendor-specific feature on a
 * pad does NOT lock the pad out of the portable surfaces: the same pad
 * stays usable as a plain digital GPIO and as its portable E1M
 * peripheral.  This example proves that on a single pad -- the Arduino
 * A1 analog input (EVK_ADC_ARDUINO_A1 = E1M_ADC1) -- by exercising all
 * three surfaces in turn:
 *
 *   Way 1  plain GPIO        alp_gpio_open(E1M_GPIO_ADC1)
 *                            -- every analog/timer pad has a universal
 *                               GPIO secondary at E1M_GPIO_<class><N>;
 *                               here ADC1's pad is index 43.
 *   Way 2  portable ADC      alp_adc_open(EVK_ADC_ARDUINO_A1)
 *                            -- the cross-vendor <alp/adc.h> surface,
 *                               identical on every E1M SoM.
 *   Way 3  vendor extension  alp_alif_adc_set_trigger_source(handle)
 *                            -- an Alif-only knob layered ON the Way-2
 *                               handle via <alp/ext/alif/adc.h>.  It
 *                               adds reach the portable API lacks; it
 *                               does not replace it.  On non-Alif
 *                               silicon it returns
 *                               ALP_ERR_NOT_PRESENT_ON_THIS_SOC and the
 *                               portable read from Way 2 is untouched.
 *
 * native_sim wires a GPIO-emul controller (so Way 1 actually toggles
 * the pad) but no ADC controller, so Ways 2/3 take the graceful
 * "skip (last_err=...)" path -- the same NULL-tolerant contract every
 * SDK open() honours.  On a real Alif EVK all three run.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/adc.h"
#include "alp/peripheral.h"                /* alp_gpio_* */
#include "alp/boards/alp_e1m_evk_routes.h" /* EVK_ADC_ARDUINO_A1 */

/* Defensive include: the Alif vendor-extension header only exists when
 * the build targets Alif silicon (CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7 or
 * similar).  __has_include keeps this example compiling on every SoM
 * -- the Way-3 block below compiles out cleanly off-Alif. */
#ifdef __has_include
#if __has_include(<alp/ext/alif/adc.h>)
#include <alp/ext/alif/adc.h>
#endif
#endif

int main(void)
{
	printf("[flex] vendor-ext composability: one pad (E1M_ADC1), three ways\n");

	/* ── Way 1: the pad as a plain digital GPIO ──────────────────
     * The analog peripheral is not "claimed" forever.  An app that
     * doesn't need ADC1's analog function drives its pad digitally
     * through the GPIO-secondary index E1M_GPIO_ADC1 (= 43 in the
     * positional e1m_pinout.h order).  We open, configure as a
     * push-pull output, drive it, then close -- releasing the pad. */
	alp_gpio_t *as_gpio = alp_gpio_open(E1M_GPIO_ADC1);
	if (as_gpio != NULL) {
		alp_gpio_configure(as_gpio, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
		alp_gpio_write(as_gpio, true);
		printf("[flex] way 1: E1M_GPIO_ADC1 driven as a digital GPIO output -- ok\n");
		alp_gpio_close(as_gpio);
	} else {
		printf("[flex] way 1: GPIO open skipped (last_err=%d)\n", (int)alp_last_error());
	}

	/* ── Way 2: the same pad as the portable E1M ADC ─────────────
     * Way 1 closed the GPIO handle, so the pad is free again.  Open
     * it as a 12-bit analog input through the portable <alp/adc.h>
     * surface -- the call is byte-identical on AEN, V2N, and every
     * other E1M-conformant SoM. */
	alp_adc_t *adc = alp_adc_open(&(alp_adc_config_t){
	    .channel_id      = EVK_ADC_ARDUINO_A1, /* = E1M_ADC1 */
	    .resolution_bits = 12,
	    .reference       = ALP_ADC_REF_INTERNAL,
	});
	if (adc == NULL) {
		/* native_sim has no ADC controller, so the alp-adc1 alias
         * resolves to a NULL spec -> NOT_READY.  Ways 2 + 3 are the
         * real-silicon path; the example still exits cleanly. */
		printf("[flex] way 2: ADC open skipped (last_err=%d; native_sim has no ADC)\n",
		       (int)alp_last_error());
		printf("[flex] done\n");
		return 0;
	}

	int32_t      uv = 0;
	alp_status_t s  = alp_adc_read_uv(adc, &uv);
	printf("[flex] way 2: E1M_ADC1 read as portable ADC -> status=%d uv=%d\n", (int)s, (int)uv);

	/* ── Way 3: an Alif vendor extension, layered ADDITIVELY ─────
     * The extension takes the SAME handle from Way 2 and adds a
     * hardware-trigger-source knob that the portable surface does not
     * expose.  It augments the portable API; it never supplants it --
     * the Way-2 read above already succeeded and would keep working
     * even if this call is unavailable. */
#ifdef ALP_EXT_ALIF_ADC_AVAILABLE
	alp_status_t es = alp_alif_adc_set_trigger_source(adc, ALP_ALIF_ADC_TRIGGER_TIMER0);
	printf("[flex] way 3: alif ext set_trigger_source -> %d "
	       "(additive on the Way-2 handle)\n",
	       (int)es);
#else
	printf("[flex] way 3: <alp/ext/alif/adc.h> not compiled in (non-Alif build); "
	       "the portable Way-2 ADC is unaffected\n");
#endif

	alp_adc_close(adc);
	printf("[flex] done\n");
	return 0;
}
