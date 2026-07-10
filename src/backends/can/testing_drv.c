/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN test double. Ordinary alp_can_ops_t backend registered at
 * the reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_CAN=y it outranks every real/proxy/fallback
 * CAN backend and alp_can_open() rides on it transparently -- see the
 * priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * open()          -- ALWAYS succeeds. Same deliberate ergonomic choice
 *                     as the GPIO/UART doubles (src/backends/gpio/
 *                     testing_drv.c, src/backends/uart/testing_drv.c):
 *                     a "*" silicon_ref double has no valid-bus-range
 *                     knowledge to reject a bus_id with, and the
 *                     injection API (<alp/testing/can.h>) needs
 *                     inject-before-open to work. Also clears the
 *                     slot's filter table -- mirrors the GPIO double's
 *                     open() re-binding callback wiring to THIS
 *                     handle, so a prior handle on the same bus id
 *                     (closed already, or this test re-opening) cannot
 *                     leave stale filters able to fire.
 * start()/stop()  -- no-ops, return ALP_OK. The dispatcher
 *                     (src/can_dispatch.c) already gates alp_can_send()
 *                     on its own `started` flag before this backend is
 *                     ever entered, so there is nothing left for this
 *                     double to enforce.
 * send()          -- captures the frame into a per-bus TX ring, read
 *                     back via alp_testing_can_tx_drain(), UNLESS the
 *                     injected bus-state (alp_testing_can_set_bus_state)
 *                     is ALP_CAN_STATE_BUS_OFF, in which case it
 *                     returns ALP_ERR_IO without capturing -- the
 *                     "ALP_ERR_IO on bus error" case <alp/can.h>
 *                     documents on alp_can_send(). Frames beyond the
 *                     capture ring's fixed capacity
 *                     (ALP_TESTING_CAN_TX_CAP) are silently dropped,
 *                     mirroring the UART double's TX overflow policy.
 * add_filter()    -- records the filter pattern + callback in the
 *                     bus's filter table, returning a monotonically
 *                     increasing id (never a reused array index -- see
 *                     the note on g_next_filter_id below).
 * remove_filter() -- clears the filter matching the id by VALUE (not
 *                     index), the same recovery z_remove_filter()
 *                     (zephyr_drv.c) uses for Zephyr's own opaque
 *                     filter ids. A frame injected after removal
 *                     cannot fire that filter's callback (no
 *                     use-after-remove).
 * close()         -- clears every filter still installed on the bus's
 *                     slot (mirrors z_close()'s "unregister every
 *                     filter still installed on the controller before
 *                     the sidecar is freed") + detaches be_data. The
 *                     bus's TX capture ring and injected bus-state are
 *                     NOT cleared (mirrors the UART double leaving
 *                     RX/TX state intact across a close/re-open on the
 *                     same id) -- only alp_testing_reset_all() clears
 *                     them.
 *
 * The injection API (<alp/testing/can.h>) and the ops table share the
 * same per-bus slot via the generic instance table
 * (src/testing/instance_table.h), keyed by bus_id -- exactly the id
 * the app passes to alp_can_open() -- so a test can inject an RX frame
 * or a bus-state fault before the app ever opens the bus.
 *
 * Deferred RX (alp_testing_can_inject_rx_at) reuses the GPIO double's
 * edge_at trampoline-pool pattern verbatim, INCLUDING its reset-hook
 * fix (issue #610 review, Fix 1): alp_testing_clock_reset() drops
 * pending clock events without firing them, so a deferred RX
 * scheduled-but-never-advanced-past would otherwise hold its
 * g_deferred slot forever across alp_testing_reset_all() calls without
 * the registered reset hook below.
 *
 * @par Cost: ROM ~1.5 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per call (linear slot scan) plus
 *      O(filters) per inject_rx() (bounded filter-table walk); fine
 *      for the handful of buses and filters a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>
#include <alp/peripheral.h>
#include <alp/testing/can.h>
#include <alp/testing/clock.h>

#include "can_ops.h"
#include "instance_table.h"
#include "reset_registry.h"
#include "virtual_clock.h"

#ifndef ALP_TESTING_CAN_MAX_BUSES
#define ALP_TESTING_CAN_MAX_BUSES 8
#endif

#ifndef ALP_TESTING_CAN_MAX_FILTERS
#define ALP_TESTING_CAN_MAX_FILTERS 8
#endif

#ifndef ALP_TESTING_CAN_TX_CAP
#define ALP_TESTING_CAN_TX_CAP 16
#endif

#ifndef ALP_TESTING_CAN_MAX_DEFERRED
#define ALP_TESTING_CAN_MAX_DEFERRED 8
#endif

/* One installed RX filter. `id` is a monotonically increasing counter
 * (alp_testing_can_slot_t.next_filter_id), NEVER the array index --
 * an index would let a filter removed-then-replaced hand out a
 * recycled id that still matches a caller's stale saved id, exactly
 * the bug z_remove_filter() (zephyr_drv.c) avoids by recovering
 * Zephyr's own opaque filter id by value instead of treating it as an
 * index. */
typedef struct {
	bool             in_use;
	int32_t          id;
	alp_can_filter_t filter;
	alp_can_rx_cb_t  cb;
	void            *user;
} alp_testing_can_filter_slot_t;

typedef struct {
	alp_testing_can_filter_slot_t filters[ALP_TESTING_CAN_MAX_FILTERS];
	int32_t                       next_filter_id;

	alp_can_frame_t tx[ALP_TESTING_CAN_TX_CAP]; /* capture ring for alp_can_send() */
	size_t          tx_head;
	size_t          tx_count;

	alp_can_state_t bus_state;
} alp_testing_can_slot_t;

static alp_testing_can_slot_t       g_slots[ALP_TESTING_CAN_MAX_BUSES];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_CAN_MAX_BUSES];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state
 * (no filters in_use, empty TX ring, ALP_CAN_STATE_ERROR_ACTIVE == 0)
 * -- the instance table's memset already produces it, so this has
 * nothing left to set. Kept (rather than passed as NULL) to mirror
 * the GPIO/UART doubles' explicit-reset style. */
static void slot_reset(void *slot_v)
{
	(void)slot_v;
}

/* Deferred-RX trampoline pool: alp_testing_clock_schedule() takes an
 * opaque ctx, so a pending inject_rx_at() needs somewhere to park its
 * (bus_id, frame) pair between scheduling and firing -- verbatim the
 * GPIO double's g_deferred pattern (src/backends/gpio/testing_drv.c). */
typedef struct {
	bool            used;
	uint32_t        bus_id;
	alp_can_frame_t frame;
} deferred_rx_t;

static deferred_rx_t g_deferred[ALP_TESTING_CAN_MAX_DEFERRED];

/* Reset hook (mirrors the GPIO double's reset_deferred_edges(), issue
 * #610 review Fix 1): without this, a scheduled-but-never-fired
 * inject_rx_at() would leak its g_deferred slot across
 * alp_testing_reset_all() forever, since alp_testing_clock_reset()
 * drops pending events without firing them. */
static void reset_deferred_rx(void)
{
	for (size_t i = 0; i < ALP_TESTING_CAN_MAX_DEFERRED; ++i) {
		g_deferred[i].used = false;
	}
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(
		    &g_table, g_slots, g_hdrs, sizeof(g_slots[0]), ALP_TESTING_CAN_MAX_BUSES, slot_reset);
		alp_testing_register_reset_hook(reset_deferred_rx);
		g_table_ready = true;
	}
	return &g_table;
}

/* (frame.id & mask) == (id & mask), plus an exact ext_id match -- the
 * rule <alp/can.h>'s alp_can_filter_t documents, with ext_id read the
 * way zephyr_drv.c's CAN_FILTER_IDE flag is: a discriminator, not a
 * don't-care. */
static bool frame_matches(const alp_can_frame_t *frame, const alp_can_filter_t *filter)
{
	if (frame->ext_id != filter->ext_id) return false;
	return (frame->id & filter->mask) == (filter->id & filter->mask);
}

alp_status_t alp_testing_can_inject_rx(uint32_t bus_id, const alp_can_frame_t *frame)
{
	if (frame == NULL) return ALP_ERR_INVAL;
	alp_testing_can_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	for (size_t i = 0; i < ALP_TESTING_CAN_MAX_FILTERS; ++i) {
		alp_testing_can_filter_slot_t *f = &slot->filters[i];
		if (f->in_use && f->cb != NULL && frame_matches(frame, &f->filter)) {
			f->cb(frame, f->user);
		}
	}
	return ALP_OK;
}

static void deferred_rx_fire(void *ctx)
{
	deferred_rx_t *d = (deferred_rx_t *)ctx;
	d->used          = false;
	(void)alp_testing_can_inject_rx(d->bus_id, &d->frame);
}

alp_status_t
alp_testing_can_inject_rx_at(uint32_t bus_id, uint64_t at_ms, const alp_can_frame_t *frame)
{
	if (frame == NULL) return ALP_ERR_INVAL;
	/* Touch now so the slot (and any earlier-registered filter) exists
	 * even if advance_ms fires before the app ever opens the bus. */
	if (alp_testing_instance_table_touch(table(), bus_id) == NULL) {
		return ALP_ERR_NOMEM;
	}

	deferred_rx_t *d = NULL;
	for (size_t i = 0; i < ALP_TESTING_CAN_MAX_DEFERRED; ++i) {
		if (!g_deferred[i].used) {
			d = &g_deferred[i];
			break;
		}
	}
	if (d == NULL) return ALP_ERR_NOMEM;

	d->used         = true;
	d->bus_id       = bus_id;
	d->frame        = *frame;
	alp_status_t rc = alp_testing_clock_schedule(at_ms, deferred_rx_fire, d);
	if (rc != ALP_OK) d->used = false;
	return rc;
}

alp_status_t alp_testing_can_set_bus_state(uint32_t bus_id, alp_can_state_t state)
{
	alp_testing_can_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	slot->bus_state = state;
	return ALP_OK;
}

alp_status_t alp_testing_can_get_bus_state(uint32_t bus_id, alp_can_state_t *state_out)
{
	if (state_out == NULL) return ALP_ERR_INVAL;
	alp_testing_can_slot_t *slot = alp_testing_instance_table_find(table(), bus_id);
	if (slot == NULL) return ALP_ERR_INVAL;
	*state_out = slot->bus_state;
	return ALP_OK;
}

size_t alp_testing_can_tx_drain(uint32_t bus_id, alp_can_frame_t *out, size_t cap)
{
	alp_testing_can_slot_t *slot = alp_testing_instance_table_find(table(), bus_id);
	if (slot == NULL) return 0;
	if (out == NULL && cap > 0) return 0;

	size_t n = (cap < slot->tx_count) ? cap : slot->tx_count;
	for (size_t i = 0; i < n; ++i) {
		out[i] = slot->tx[(slot->tx_head + i) % ALP_TESTING_CAN_TX_CAP];
	}
	slot->tx_head = (slot->tx_head + n) % ALP_TESTING_CAN_TX_CAP;
	slot->tx_count -= n;
	return n;
}

/* ---- alp_can_ops_t ---- */

static void clear_filters(alp_testing_can_slot_t *slot)
{
	for (size_t i = 0; i < ALP_TESTING_CAN_MAX_FILTERS; ++i) {
		slot->filters[i] = (alp_testing_can_filter_slot_t){ 0 };
	}
}

static alp_status_t
t_open(const alp_can_config_t *cfg, alp_can_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_can_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	/* Re-bind the filter wiring to THIS handle -- mirrors the GPIO
	 * double's open() re-binding cb/cb_user; a prior handle on the
	 * same bus id must not leave stale filters able to fire into it. */
	clear_filters(slot);
	slot->next_filter_id = 0;

	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = slot;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t t_start(alp_can_backend_state_t *st)
{
	(void)st;
	return ALP_OK;
}

static alp_status_t t_stop(alp_can_backend_state_t *st)
{
	(void)st;
	return ALP_OK;
}

static alp_status_t
t_send(alp_can_backend_state_t *st, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
	(void)timeout_ms; /* the double's TX capture ring never blocks -- see file header. */
	alp_testing_can_slot_t *slot = (alp_testing_can_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	if (slot->bus_state == ALP_CAN_STATE_BUS_OFF) return ALP_ERR_IO;

	if (slot->tx_count < ALP_TESTING_CAN_TX_CAP) { /* documented drop-on-overflow otherwise */
		size_t idx    = (slot->tx_head + slot->tx_count) % ALP_TESTING_CAN_TX_CAP;
		slot->tx[idx] = *frame;
		slot->tx_count++;
	}
	return ALP_OK;
}

static alp_status_t t_add_filter(alp_can_backend_state_t *st,
                                 const alp_can_filter_t  *filter,
                                 alp_can_rx_cb_t          cb,
                                 void                    *user,
                                 int32_t                 *filter_id_out)
{
	alp_testing_can_slot_t *slot = (alp_testing_can_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	int free_i = -1;
	for (int i = 0; i < ALP_TESTING_CAN_MAX_FILTERS; ++i) {
		if (!slot->filters[i].in_use) {
			free_i = i;
			break;
		}
	}
	if (free_i < 0) return ALP_ERR_NOMEM;

	alp_testing_can_filter_slot_t *f = &slot->filters[free_i];
	f->in_use                        = true;
	f->id                            = slot->next_filter_id++;
	f->filter                        = *filter;
	f->cb                            = cb;
	f->user                          = user;

	if (filter_id_out != NULL) *filter_id_out = f->id;
	return ALP_OK;
}

static alp_status_t t_remove_filter(alp_can_backend_state_t *st, int32_t filter_id)
{
	alp_testing_can_slot_t *slot = (alp_testing_can_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	for (size_t i = 0; i < ALP_TESTING_CAN_MAX_FILTERS; ++i) {
		alp_testing_can_filter_slot_t *f = &slot->filters[i];
		if (f->in_use && f->id == filter_id) {
			*f = (alp_testing_can_filter_slot_t){ 0 };
			return ALP_OK;
		}
	}
	return ALP_ERR_INVAL;
}

static void t_close(alp_can_backend_state_t *st)
{
	alp_testing_can_slot_t *slot = (alp_testing_can_slot_t *)st->be_data;
	if (slot == NULL) return;
	/* Cut the filter wiring so an injection that arrives after close
	 * cannot call back into a handle the dispatcher has already
	 * returned to the pool -- mirrors z_close()'s (zephyr_drv.c)
	 * "unregister every filter still installed" + the GPIO double's
	 * t_close(). */
	clear_filters(slot);
	st->be_data = NULL;
}

static const alp_can_ops_t _ops = {
	.open          = t_open,
	.start         = t_start,
	.stop          = t_stop,
	.send          = t_send,
	.add_filter    = t_add_filter,
	.remove_filter = t_remove_filter,
	.close         = t_close,
};

ALP_BACKEND_REGISTER(can,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
