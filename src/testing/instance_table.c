/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic per-class keyed instance table -- see instance_table.h.
 * Also hosts alp_testing_reset_all() (<alp/testing/common.h>): the
 * one place that knows both "every class's table" (this file's sweep
 * list) and "the virtual clock" (<alp/testing/clock.h>), so it is the
 * natural owner of the cross-class reset.
 */

#include <stddef.h>
#include <string.h>

#include <alp/testing/clock.h>
#include <alp/testing/common.h>

#include "instance_table.h"

/*
 * Sweep list of every table a class double has initialised.  A fixed
 * cap is fine: one entry per COMPILED-IN class double (GPIO today),
 * not per handle/instance.
 */
#ifndef ALP_TESTING_MAX_TABLES
#define ALP_TESTING_MAX_TABLES 16
#endif

static alp_testing_instance_table_t *g_tables[ALP_TESTING_MAX_TABLES];
static size_t                        g_table_count;

void alp_testing_instance_table_init(alp_testing_instance_table_t *t,
                                     void                         *slots,
                                     alp_testing_slot_hdr_t       *hdrs,
                                     size_t                        slot_size,
                                     size_t                        capacity,
                                     alp_testing_slot_reset_fn     reset_fn)
{
	t->slots     = slots;
	t->hdrs      = hdrs;
	t->slot_size = slot_size;
	t->capacity  = capacity;
	t->reset_fn  = reset_fn;
	t->next      = NULL;

	for (size_t i = 0; i < capacity; ++i) {
		hdrs[i].bound = false;
		hdrs[i].id    = 0;
	}

	/* Idempotent registration: a table pointer already on the sweep
	 * list (a class double re-initialising, e.g. re-run in a second
	 * test image) is not added twice. */
	for (size_t i = 0; i < g_table_count; ++i) {
		if (g_tables[i] == t) return;
	}
	if (g_table_count < ALP_TESTING_MAX_TABLES) {
		g_tables[g_table_count++] = t;
	}
}

static void *slot_at(alp_testing_instance_table_t *t, size_t idx)
{
	return (uint8_t *)t->slots + (idx * t->slot_size);
}

void *alp_testing_instance_table_touch(alp_testing_instance_table_t *t, uint32_t id)
{
	size_t free_idx = t->capacity;

	for (size_t i = 0; i < t->capacity; ++i) {
		if (t->hdrs[i].bound && t->hdrs[i].id == id) {
			return slot_at(t, i);
		}
		if (!t->hdrs[i].bound && free_idx == t->capacity) {
			free_idx = i;
		}
	}
	if (free_idx == t->capacity) {
		return NULL; /* every slot bound to a different id */
	}

	void *slot = slot_at(t, free_idx);
	memset(slot, 0, t->slot_size);
	if (t->reset_fn != NULL) t->reset_fn(slot);
	t->hdrs[free_idx].bound = true;
	t->hdrs[free_idx].id    = id;
	return slot;
}

void *alp_testing_instance_table_find(alp_testing_instance_table_t *t, uint32_t id)
{
	for (size_t i = 0; i < t->capacity; ++i) {
		if (t->hdrs[i].bound && t->hdrs[i].id == id) {
			return slot_at(t, i);
		}
	}
	return NULL;
}

void alp_testing_instance_table_reset(alp_testing_instance_table_t *t)
{
	for (size_t i = 0; i < t->capacity; ++i) {
		if (!t->hdrs[i].bound) continue;
		void *slot = slot_at(t, i);
		memset(slot, 0, t->slot_size);
		if (t->reset_fn != NULL) t->reset_fn(slot);
		t->hdrs[i].bound = false;
		t->hdrs[i].id    = 0;
	}
}

void alp_testing_instance_table_reset_all(void)
{
	for (size_t i = 0; i < g_table_count; ++i) {
		alp_testing_instance_table_reset(g_tables[i]);
	}
}

void alp_testing_reset_all(void)
{
	alp_testing_clock_reset();
	alp_testing_instance_table_reset_all();
}
