/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reset-hook registry backing alp_testing_reset_all()
 * (instance_table.c).  NOT a public header.
 *
 * Some class doubles keep side-state that lives OUTSIDE the generic
 * per-class instance table (instance_table.h) -- e.g. the GPIO
 * double's deferred-edge trampoline pool
 * (src/backends/gpio/testing_drv.c: g_deferred), which
 * alp_testing_gpio_edge_at() marks used and only frees when the
 * scheduled virtual-clock event FIRES.  alp_testing_clock_reset()
 * drops pending clock events without firing them, so that pool would
 * otherwise leak a slot every time a test schedules an edge_at() it
 * never advances the clock past -- contradicting
 * <alp/testing/common.h>'s "wipes everything" contract.
 *
 * Any future class double with this shape (state
 * alp_testing_reset_all() cannot reach through the clock or an
 * instance table) registers a reset hook here ONCE -- e.g. from its
 * own one-time lazy-init path, the same place it registers its
 * instance table -- and alp_testing_reset_all() runs every registered
 * hook after it resets the clock and every instance table. A double
 * whose entire footprint is one instance table needs NO hook at all;
 * alp_testing_instance_table_reset_all() already covers it. Adding a
 * hook never requires editing alp_testing_reset_all() itself.
 */

#ifndef ALP_TESTING_RESET_REGISTRY_H
#define ALP_TESTING_RESET_REGISTRY_H

/** One class double's "clear my side-state" callback. */
typedef void (*alp_testing_reset_hook_fn)(void);

/*
 * Register `fn` to run on every alp_testing_reset_all(), after the
 * virtual clock and every instance table have been reset. Idempotent:
 * registering the same function pointer more than once (a class
 * double's lazy-init path re-entered, or a second test image linking
 * the same TU) is a no-op, so hook count/order stay stable across
 * repeated ztest runs in one binary. Fixed capacity
 * (ALP_TESTING_MAX_RESET_HOOKS, default 16); a NULL `fn` is ignored.
 */
void alp_testing_register_reset_hook(alp_testing_reset_hook_fn fn);

/* Run every hook registered via alp_testing_register_reset_hook(), in
 * registration order. Called by alp_testing_reset_all()
 * (instance_table.c); not part of the public surface. */
void alp_testing_reset_hooks_run_all(void);

#endif /* ALP_TESTING_RESET_REGISTRY_H */
