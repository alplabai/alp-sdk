/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr gpio_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/gpio_dispatch.c; the
 * backend's open fills state->dev, resolves the pin's gpio_dt_spec
 * via the alp,pin-array DT node, and stashes its per-pool sidecar
 * (gpio_callback + gpio_dt_spec) so the Zephyr-side IRQ glue can
 * thread back through CONTAINER_OF to the portable handle.
 *
 * Pin lookup is via the `alp,pin-array` devicetree node -- a single
 * node carrying a `gpios` property with one phandle/spec per pin id.
 * See zephyr/dts/bindings/alp,pin-array.yaml.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "gpio_ops.h"

#define DT_DRV_COMPAT alp_pin_array

#define ALP_PIN_NODE DT_INST(0, alp_pin_array)

#if DT_NODE_EXISTS(ALP_PIN_NODE)
#define ALP_PIN_AVAILABLE 1
#define ALP_PIN_COUNT     DT_PROP_LEN(ALP_PIN_NODE, gpios)
#else
#define ALP_PIN_AVAILABLE 0
#define ALP_PIN_COUNT     0
#endif

#if ALP_PIN_AVAILABLE
#define ALP_PIN_BY_IDX(idx) GPIO_DT_SPEC_GET_BY_IDX(ALP_PIN_NODE, gpios, idx)

#define ALP_PIN_ENTRY(i, _) ALP_PIN_BY_IDX(i),

static const struct gpio_dt_spec alp_pins[] = { LISTIFY(ALP_PIN_COUNT, ALP_PIN_ENTRY, ()) };
#endif

/* Exported for other backends (e.g. spi/zephyr_drv.c) that need to
 * resolve a chip-select gpio_dt_spec from the same alp,pin-array node. */
bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out)
{
#if ALP_PIN_AVAILABLE
	if (pin_id >= ARRAY_SIZE(alp_pins)) return false;
	*out = alp_pins[pin_id];
	return true;
#else
	(void)pin_id;
	(void)out;
	return false;
#endif
}

#ifndef CONFIG_ALP_SDK_MAX_GPIO_HANDLES
#define CONFIG_ALP_SDK_MAX_GPIO_HANDLES 16
#endif

/* Per-handle Zephyr sidecar.  Indexed by the same pool slot as the
 * portable handle in src/gpio_dispatch.c; the dispatcher passes the
 * pool-relative index via state->be_data (set at open() time). */
typedef struct {
	struct gpio_dt_spec  spec;
	struct gpio_callback zcb;
	struct alp_gpio     *owner; /* back-ref so the ISR thunk can invoke cb */
	bool                 in_use;
} alp_z_gpio_side_t;

static alp_z_gpio_side_t _sides[CONFIG_ALP_SDK_MAX_GPIO_HANDLES];

static alp_z_gpio_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
		if (!_sides[i].in_use) {
			_sides[i]        = (alp_z_gpio_side_t){ 0 };
			_sides[i].in_use = true;
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(alp_z_gpio_side_t *s)
{
	if (s != NULL) s->in_use = false;
}

static gpio_flags_t _to_gpio_flags(alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	gpio_flags_t f = (dir == ALP_GPIO_OUTPUT) ? GPIO_OUTPUT : GPIO_INPUT;
	if (pull == ALP_GPIO_PULL_UP) f |= GPIO_PULL_UP;
	if (pull == ALP_GPIO_PULL_DOWN) f |= GPIO_PULL_DOWN;
	return f;
}

static gpio_flags_t _to_gpio_irq_flags(alp_gpio_edge_t edge)
{
	switch (edge) {
	case ALP_GPIO_EDGE_RISING:
		return GPIO_INT_EDGE_RISING;
	case ALP_GPIO_EDGE_FALLING:
		return GPIO_INT_EDGE_FALLING;
	case ALP_GPIO_EDGE_BOTH:
		return GPIO_INT_EDGE_BOTH;
	default:
		return GPIO_INT_DISABLE;
	}
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static void _isr_thunk(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	alp_z_gpio_side_t *s = CONTAINER_OF(cb, alp_z_gpio_side_t, zcb);
	struct alp_gpio   *h = s->owner;
	if (h != NULL && h->cb != NULL) {
		h->cb(h, h->cb_user);
	}
}

static alp_status_t
z_open(uint32_t pin_id, alp_gpio_backend_state_t *st, alp_capabilities_t *caps_out)
{
	struct gpio_dt_spec spec;
	if (!alp_z_gpio_resolve(pin_id, &spec)) return ALP_ERR_INVAL;
	if (!device_is_ready(spec.port)) return ALP_ERR_NOT_READY;

	alp_z_gpio_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;
	s->spec  = spec;
	s->owner = CONTAINER_OF(st, struct alp_gpio, state);

	st->dev         = (void *)spec.port;
	st->pin_id      = pin_id;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
z_configure(alp_gpio_backend_state_t *st, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	return _errno_to_alp(gpio_pin_configure_dt(&s->spec, _to_gpio_flags(dir, pull)));
}

static alp_status_t z_write(alp_gpio_backend_state_t *st, bool level)
{
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	return _errno_to_alp(gpio_pin_set_dt(&s->spec, level ? 1 : 0));
}

static alp_status_t z_read(alp_gpio_backend_state_t *st, bool *level)
{
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	int v = gpio_pin_get_dt(&s->spec);
	if (v < 0) return _errno_to_alp(v);
	*level = (v != 0);
	return ALP_OK;
}

static alp_status_t
z_irq_enable(alp_gpio_backend_state_t *st, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)
{
	(void)cb;
	(void)user; /* stashed in the portable handle by the dispatcher */
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;

	gpio_init_callback(&s->zcb, _isr_thunk, BIT(s->spec.pin));
	int err = gpio_add_callback(s->spec.port, &s->zcb);
	if (err != 0) return _errno_to_alp(err);

	err = gpio_pin_interrupt_configure_dt(&s->spec, _to_gpio_irq_flags(edge));
	if (err != 0) {
		gpio_remove_callback(s->spec.port, &s->zcb);
	}
	return _errno_to_alp(err);
}

static alp_status_t z_irq_disable(alp_gpio_backend_state_t *st)
{
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;
	int err = gpio_pin_interrupt_configure_dt(&s->spec, GPIO_INT_DISABLE);
	gpio_remove_callback(s->spec.port, &s->zcb);
	return _errno_to_alp(err);
}

static void z_close(alp_gpio_backend_state_t *st)
{
	alp_z_gpio_side_t *s = (alp_z_gpio_side_t *)st->be_data;
	if (s == NULL) return;
	/* If the owner still has an active edge the dispatcher hasn't
     * called irq_disable for us -- tear down defensively before
     * releasing the sidecar slot. */
	if (s->owner != NULL && s->owner->edge != ALP_GPIO_EDGE_NONE) {
		(void)gpio_pin_interrupt_configure_dt(&s->spec, GPIO_INT_DISABLE);
		(void)gpio_remove_callback(s->spec.port, &s->zcb);
	}
	_free_side(s);
	st->be_data = NULL;
}

static const alp_gpio_ops_t _ops = {
	.open        = z_open,
	.configure   = z_configure,
	.write       = z_write,
	.read        = z_read,
	.enable_irq  = z_irq_enable,
	.disable_irq = z_irq_disable,
	.close       = z_close,
};

/* Delegation hook for the CC3501E GPIO proxy backend
 * (src/backends/gpio/cc3501e_proxy.c): pins NOT routed to the inter-chip
 * bridge fall through to this platform driver.  Exposed so the proxy reuses
 * the real Zephyr pin I/O instead of re-implementing it. */
const alp_gpio_ops_t *alp_z_gpio_ops(void)
{
	return &_ops;
}

ALP_BACKEND_REGISTER(gpio,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
