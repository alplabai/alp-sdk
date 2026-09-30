/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpio-qdec input-subsystem QEnc backend (issue #2095, the "v0.3
 * input-subsystem fast-path" zephyr_drv.c's own comment anticipates).
 *
 * zephyr_drv.c's sensor_sample_fetch()/SENSOR_CHAN_ROTATION path is
 * measured-broken on the Alif E8's UTIMER QEC hardware channel
 * (issue #2037): that channel is an unqualified edge counter, not a
 * quadrature decoder.  A plain-GPIO quadrature pair instead needs
 * Zephyr's software gpio-qdec input driver (zephyr/drivers/input/
 * input_gpio_qdec.c), which posts direction-correct INPUT_EV_REL
 * events rather than SENSOR_CHAN_ROTATION samples.
 *
 * This backend registers a per-device INPUT_CALLBACK_DEFINE for every
 * alp-qenc<N> alias whose devicetree node is compatible "gpio-qdec"
 * and accumulates the relative events into a position, atomically --
 * the input subsystem's callback runs on its own thread (or inline
 * under CONFIG_INPUT_MODE_SYNCHRONOUS), asynchronously with respect
 * to any get_position()/reset_position() caller.
 *
 * Selection: registered at a higher priority (110) than zephyr_drv's
 * 100, both under silicon_ref "*".  An alp-qenc<N> alias that is NOT
 * a gpio-qdec node (e.g. a real sensor-class QDEC) makes open()
 * return ALP_ERR_NOSUPPORT, which the qenc dispatcher's open-time
 * fall-through (mirrors src/security_dispatch.c's hash_open) walks
 * past to try zephyr_drv next -- so the existing sensor backend is
 * unaffected on channels this backend declines.
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "qenc_ops.h"

#define ALP_QENC_NODE(idx) DT_ALIAS(_CONCAT(alp_qenc, idx))

/* 1 iff the alp-qenc<idx> alias exists, is okay, and its node is
 * compatible "gpio-qdec" -- the only shape this backend serves. */
#define ALP_QENC_IS_GPIO_QDEC(idx) \
	UTIL_AND(DT_NODE_HAS_STATUS(ALP_QENC_NODE(idx), okay), \
	         DT_NODE_HAS_COMPAT(ALP_QENC_NODE(idx), gpio_qdec))

#define ALP_QENC_QDEC_DEV_OR_NULL(idx) \
	COND_CODE_1(ALP_QENC_IS_GPIO_QDEC(idx), (DEVICE_DT_GET(ALP_QENC_NODE(idx))), (NULL))

static const struct device *const _devs[] = {
	ALP_QENC_QDEC_DEV_OR_NULL(0),
	ALP_QENC_QDEC_DEV_OR_NULL(1),
	ALP_QENC_QDEC_DEV_OR_NULL(2),
	ALP_QENC_QDEC_DEV_OR_NULL(3),
};

/* One accumulator per encoder_id slot, independent of any open()
 * handle -- events can arrive whether or not a caller currently holds
 * a handle open, and reset_position() must zero the running count. */
static atomic_t _pos[ARRAY_SIZE(_devs)];

static void _on_rel_event(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_REL) return;
	atomic_add((atomic_t *)user_data, evt->value);
}

#define ALP_QENC_QDEC_CALLBACK(idx) \
	IF_ENABLED(ALP_QENC_IS_GPIO_QDEC(idx), \
	           (INPUT_CALLBACK_DEFINE_NAMED( \
	                DEVICE_DT_GET(ALP_QENC_NODE(idx)), _on_rel_event, &_pos[idx], idx);))

ALP_QENC_QDEC_CALLBACK(0)
ALP_QENC_QDEC_CALLBACK(1)
ALP_QENC_QDEC_CALLBACK(2)
ALP_QENC_QDEC_CALLBACK(3)

static alp_status_t
q_open(const alp_qenc_config_t *cfg, alp_qenc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	if (cfg->encoder_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	const struct device *dev = _devs[cfg->encoder_id];
	/* Not a gpio-qdec alias on this SoC/board -- decline so the
	 * dispatcher's fall-through tries the sensor backend next. */
	if (dev == NULL) return ALP_ERR_NOSUPPORT;
	if (!device_is_ready(dev)) return ALP_ERR_NOT_READY;
	st->dev           = (void *)dev;
	st->encoder_id    = cfg->encoder_id;
	st->last_position = (int32_t)atomic_get(&_pos[cfg->encoder_id]);
	return ALP_OK;
}

static alp_status_t q_get_position(alp_qenc_backend_state_t *st, int32_t *pos_out)
{
	int32_t pos       = (int32_t)atomic_get(&_pos[st->encoder_id]);
	st->last_position = pos;
	*pos_out          = pos;
	return ALP_OK;
}

static alp_status_t q_reset_position(alp_qenc_backend_state_t *st)
{
	atomic_set(&_pos[st->encoder_id], 0);
	st->last_position = 0;
	return ALP_OK;
}

static const alp_qenc_ops_t _ops = {
	.open           = q_open,
	.get_position   = q_get_position,
	.reset_position = q_reset_position,
	.close          = NULL,
};

ALP_BACKEND_REGISTER(qenc,
                     gpio_qdec,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 110,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
