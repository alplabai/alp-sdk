/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO test double.  Ordinary alp_gpio_ops_t backend registered at
 * the reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_GPIO=y it outranks every real/proxy/fallback
 * GPIO backend and alp_gpio_open() rides on it transparently -- see
 * the priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * open()       -- ALWAYS succeeds (never NOSUPPORT: that would trigger
 *                  alp_backend_select_next() fallthrough to the next
 *                  backend instead of serving the test).  Binds (or
 *                  creates) the pin's instance-table slot.
 * configure()  -- no-op, returns ALP_OK (direction/pull are not
 *                  observable through this double).
 * write()      -- records the driven level + increments the write count.
 * read()       -- returns the virtual input level last set via
 *                  alp_testing_gpio_set_input() / an injected edge.
 * enable_irq() -- arms the edge + stashes the dispatcher's cb/user.
 * disable_irq()-- clears the armed edge + cb/user.
 * close()      -- clears the armed edge + cb/user + owner so a
 *                  subsequent injection on the same pin id cannot fire
 *                  into a closed handle (no use-after-close).
 *
 * The injection API (<alp/testing/gpio.h>) and the ops table share
 * the same per-pin slot via the generic instance table
 * (src/testing/instance_table.h), keyed by pin_id -- exactly the id
 * the app passes to alp_gpio_open() -- so a test can inject before
 * the app ever opens the pin.
 *
 * @par Cost: ROM ~1 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per call (linear slot scan); fine for
 *      the handful of pins a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/testing/clock.h>
#include <alp/testing/gpio.h>

#include "gpio_ops.h"
#include "instance_table.h"
#include "virtual_clock.h"

#ifndef ALP_TESTING_GPIO_MAX_PINS
#define ALP_TESTING_GPIO_MAX_PINS 32
#endif

/* struct alp_gpio's first member is `state` (offset 0), so recovering
 * the owning handle from the alp_gpio_backend_state_t* the ops table
 * is called with is a plain offset-0 cast -- spelled out via offsetof
 * rather than assumed, so a future field reorder fails loudly instead
 * of silently miscasting. */
#define ALP_TESTING_OWNER_OF(state_ptr)                                                            \
	((struct alp_gpio *)((char *)(state_ptr) - offsetof(struct alp_gpio, state)))

typedef struct {
	bool             input_level;
	bool             output_level;
	uint32_t         write_count;
	alp_gpio_edge_t  armed_edge;
	alp_gpio_cb_t    cb;
	void            *cb_user;
	struct alp_gpio *owner;
} alp_testing_gpio_slot_t;

static alp_testing_gpio_slot_t      g_slots[ALP_TESTING_GPIO_MAX_PINS];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_GPIO_MAX_PINS];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Slot fields memset to 0 by the instance table before this runs, so
 * this only needs to set the one field whose zero value ISN'T the
 * "never armed" state. */
static void slot_reset(void *slot_v)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)slot_v;
	slot->armed_edge              = ALP_GPIO_EDGE_NONE;
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(
		    &g_table, g_slots, g_hdrs, sizeof(g_slots[0]), ALP_TESTING_GPIO_MAX_PINS, slot_reset);
		g_table_ready = true;
	}
	return &g_table;
}

/* Fires the armed callback if `edge` matches what's armed on `slot`
 * (an armed ALP_GPIO_EDGE_BOTH matches either polarity), then flips
 * the virtual input level to match the edge direction. */
static void fire_edge(alp_testing_gpio_slot_t *slot, alp_gpio_edge_t edge)
{
	bool matches      = (slot->armed_edge == edge) || (slot->armed_edge == ALP_GPIO_EDGE_BOTH);
	slot->input_level = (edge == ALP_GPIO_EDGE_RISING);
	if (matches && slot->cb != NULL && slot->owner != NULL) {
		slot->cb(slot->owner, slot->cb_user);
	}
}

alp_status_t alp_testing_gpio_set_input(uint32_t pin_id, bool level)
{
	alp_testing_gpio_slot_t *slot = alp_testing_instance_table_touch(table(), pin_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	slot->input_level = level;
	return ALP_OK;
}

alp_status_t alp_testing_gpio_edge(uint32_t pin_id, alp_gpio_edge_t edge)
{
	if (edge != ALP_GPIO_EDGE_RISING && edge != ALP_GPIO_EDGE_FALLING) {
		return ALP_ERR_INVAL;
	}
	alp_testing_gpio_slot_t *slot = alp_testing_instance_table_touch(table(), pin_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	fire_edge(slot, edge);
	return ALP_OK;
}

/* Deferred-edge trampoline pool: alp_testing_clock_schedule() takes an
 * opaque ctx, so a pending edge_at() needs somewhere to park its
 * (pin_id, edge) pair between scheduling and firing. */
typedef struct {
	bool            used;
	uint32_t        pin_id;
	alp_gpio_edge_t edge;
} deferred_edge_t;

#ifndef ALP_TESTING_GPIO_MAX_DEFERRED
#define ALP_TESTING_GPIO_MAX_DEFERRED 8
#endif

static deferred_edge_t g_deferred[ALP_TESTING_GPIO_MAX_DEFERRED];

static void deferred_edge_fire(void *ctx)
{
	deferred_edge_t *d = (deferred_edge_t *)ctx;
	d->used            = false;
	(void)alp_testing_gpio_edge(d->pin_id, d->edge);
}

alp_status_t alp_testing_gpio_edge_at(uint32_t pin_id, uint64_t at_ms, alp_gpio_edge_t edge)
{
	if (edge != ALP_GPIO_EDGE_RISING && edge != ALP_GPIO_EDGE_FALLING) {
		return ALP_ERR_INVAL;
	}
	/* Touch now so the slot (and any earlier-injected input level)
	 * exists even if advance_ms fires before the app ever opens the
	 * pin. */
	if (alp_testing_instance_table_touch(table(), pin_id) == NULL) {
		return ALP_ERR_NOMEM;
	}

	deferred_edge_t *d = NULL;
	for (size_t i = 0; i < ALP_TESTING_GPIO_MAX_DEFERRED; ++i) {
		if (!g_deferred[i].used) {
			d = &g_deferred[i];
			break;
		}
	}
	if (d == NULL) return ALP_ERR_NOMEM;

	d->used         = true;
	d->pin_id       = pin_id;
	d->edge         = edge;
	alp_status_t rc = alp_testing_clock_schedule(at_ms, deferred_edge_fire, d);
	if (rc != ALP_OK) d->used = false;
	return rc;
}

alp_status_t alp_testing_gpio_get_output(uint32_t pin_id, bool *level)
{
	if (level == NULL) return ALP_ERR_INVAL;
	alp_testing_gpio_slot_t *slot = alp_testing_instance_table_find(table(), pin_id);
	if (slot == NULL) return ALP_ERR_INVAL;
	*level = slot->output_level;
	return ALP_OK;
}

alp_status_t alp_testing_gpio_write_count(uint32_t pin_id, uint32_t *n)
{
	if (n == NULL) return ALP_ERR_INVAL;
	alp_testing_gpio_slot_t *slot = alp_testing_instance_table_find(table(), pin_id);
	if (slot == NULL) return ALP_ERR_INVAL;
	*n = slot->write_count;
	return ALP_OK;
}

/* ---- alp_gpio_ops_t ---- */

static alp_status_t
t_open(uint32_t pin_id, alp_gpio_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_gpio_slot_t *slot = alp_testing_instance_table_touch(table(), pin_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	/* Re-bind the callback wiring to THIS handle; a prior handle on
	 * the same pin id (closed already, or this test is re-opening)
	 * must not receive callbacks meant for the new one. */
	slot->armed_edge = ALP_GPIO_EDGE_NONE;
	slot->cb         = NULL;
	slot->cb_user    = NULL;
	slot->owner      = ALP_TESTING_OWNER_OF(st);

	st->dev         = NULL;
	st->pin_id      = pin_id;
	st->be_data     = slot;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
t_configure(alp_gpio_backend_state_t *st, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	(void)st;
	(void)dir;
	(void)pull;
	return ALP_OK;
}

static alp_status_t t_write(alp_gpio_backend_state_t *st, bool level)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	slot->output_level = level;
	slot->write_count++;
	return ALP_OK;
}

static alp_status_t t_read(alp_gpio_backend_state_t *st, bool *level)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	*level = slot->input_level;
	return ALP_OK;
}

static alp_status_t
t_irq_enable(alp_gpio_backend_state_t *st, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	slot->armed_edge = edge;
	slot->cb         = cb;
	slot->cb_user    = user;
	return ALP_OK;
}

static alp_status_t t_irq_disable(alp_gpio_backend_state_t *st)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	slot->armed_edge = ALP_GPIO_EDGE_NONE;
	slot->cb         = NULL;
	slot->cb_user    = NULL;
	return ALP_OK;
}

static void t_close(alp_gpio_backend_state_t *st)
{
	alp_testing_gpio_slot_t *slot = (alp_testing_gpio_slot_t *)st->be_data;
	if (slot == NULL) return;
	/* Cut the callback wiring so an injection that arrives after
	 * close (arm-then-close-then-inject) cannot call back into a
	 * handle the dispatcher has already returned to the pool. */
	slot->armed_edge = ALP_GPIO_EDGE_NONE;
	slot->cb         = NULL;
	slot->cb_user    = NULL;
	slot->owner      = NULL;
	st->be_data      = NULL;
}

static const alp_gpio_ops_t _ops = {
	.open        = t_open,
	.configure   = t_configure,
	.write       = t_write,
	.read        = t_read,
	.enable_irq  = t_irq_enable,
	.disable_irq = t_irq_disable,
	.close       = t_close,
};

ALP_BACKEND_REGISTER(gpio,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
