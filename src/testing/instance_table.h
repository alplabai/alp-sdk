/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic per-class keyed instance table backing every
 * alp/testing class double.  NOT a public header.
 *
 * Each class (GPIO today; more follow this same pattern) owns one
 * table: a caller-supplied array of homogeneous slots plus a parallel
 * header array this module uses to track which public instance id
 * (the same uint32 the app passes to alp_<class>_open) each slot is
 * bound to.  Lookups support create-on-first-touch
 * (alp_testing_instance_table_touch) so an injector API can populate
 * a slot BEFORE the app ever opens that instance, and a pure lookup
 * (alp_testing_instance_table_find) for accessors that must fail
 * ALP_ERR_INVAL on an id nothing has touched yet.
 *
 * Every table created via alp_testing_instance_table_init registers
 * itself on a static list so alp_testing_reset_all() (common.c) can
 * wipe every class's state in one sweep without each class needing
 * its own entry in that function.
 */

#ifndef ALP_TESTING_INSTANCE_TABLE_H
#define ALP_TESTING_INSTANCE_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Per-slot bookkeeping, parallel to the caller's slot array. */
typedef struct {
	uint32_t id;
	bool     bound;
} alp_testing_slot_hdr_t;

/** Re-initialises one slot to its "never touched" state (zero
 *  counters, cleared callbacks, etc.) -- called by
 *  alp_testing_instance_table_touch the first time a given index is
 *  bound, and again by a table-wide reset for every bound slot. */
typedef void (*alp_testing_slot_reset_fn)(void *slot);

typedef struct alp_testing_instance_table {
	void                              *slots; /* caller-owned array[capacity] of slot_size */
	alp_testing_slot_hdr_t            *hdrs;  /* caller-owned array[capacity] */
	size_t                             slot_size;
	size_t                             capacity;
	alp_testing_slot_reset_fn          reset_fn;
	struct alp_testing_instance_table *next; /* reset_all sweep list */
} alp_testing_instance_table_t;

/*
 * One-time setup.  `slots` / `hdrs` are storage the CALLER declares
 * statically (e.g. `static alp_testing_gpio_slot_t g_slots[32];`) --
 * this module never allocates.  Registers `t` on the reset_all sweep
 * list; safe to call more than once for the same `t` (idempotent --
 * re-running it just re-links onto the list, which
 * alp_testing_instance_table_reset_all tolerates by walking a
 * de-duplicated static list built at first init per table pointer).
 */
void alp_testing_instance_table_init(alp_testing_instance_table_t *t,
                                     void                         *slots,
                                     alp_testing_slot_hdr_t       *hdrs,
                                     size_t                        slot_size,
                                     size_t                        capacity,
                                     alp_testing_slot_reset_fn     reset_fn);

/*
 * Find the slot bound to `id`, binding + resetting the first free
 * slot if none is bound yet (create-on-first-touch).  Returns NULL
 * only when every slot is already bound to a DIFFERENT id (table full).
 */
void *alp_testing_instance_table_touch(alp_testing_instance_table_t *t, uint32_t id);

/* Find the slot bound to `id` without creating one.  NULL if `id`
 * has never been touched. */
void *alp_testing_instance_table_find(alp_testing_instance_table_t *t, uint32_t id);

/* Unbind every slot in `t`, invoking reset_fn on each previously-bound
 * one first. */
void alp_testing_instance_table_reset(alp_testing_instance_table_t *t);

/* Reset every table ever passed to alp_testing_instance_table_init().
 * Called by alp_testing_reset_all(); not part of the public surface. */
void alp_testing_instance_table_reset_all(void);

#endif /* ALP_TESTING_INSTANCE_TABLE_H */
