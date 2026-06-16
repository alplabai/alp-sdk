/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * PI3WVR626 MIPI CSI 2:1 multiplexer helper -- thin GPIO wrapper.
 */

#include <string.h>

#include "alp/chips/cam_mux_pi3wvr626.h"

alp_status_t cam_mux_pi3wvr626_init(cam_mux_pi3wvr626_t *ctx, alp_gpio_t *sel_pin)
{
	if (ctx == NULL || sel_pin == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->sel_pin = sel_pin;

	/* Configure the SEL pin as a push-pull output.  Default state
     * is input A -- writing 0 first matches the chip's
     * power-on-reset convention and avoids an indeterminate
     * settling period before the first explicit select(). */
	alp_status_t s = alp_gpio_configure(sel_pin, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (s != ALP_OK) return s;
	s = alp_gpio_write(sel_pin, false);
	if (s != ALP_OK) return s;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t cam_mux_pi3wvr626_select(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t input)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	return alp_gpio_write(ctx->sel_pin, input == CAM_MUX_PI3WVR626_INPUT_B);
}

alp_status_t cam_mux_pi3wvr626_get(cam_mux_pi3wvr626_t *ctx, cam_mux_pi3wvr626_input_t *input_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (input_out == NULL) return ALP_ERR_INVAL;
	bool         level = false;
	alp_status_t s     = alp_gpio_read(ctx->sel_pin, &level);
	if (s != ALP_OK) return s;
	*input_out = level ? CAM_MUX_PI3WVR626_INPUT_B : CAM_MUX_PI3WVR626_INPUT_A;
	return ALP_OK;
}

void cam_mux_pi3wvr626_deinit(cam_mux_pi3wvr626_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->sel_pin     = NULL;
}
