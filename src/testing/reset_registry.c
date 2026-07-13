/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reset-hook registry -- see reset_registry.h.
 */

#include <stddef.h>

#include "reset_registry.h"

#ifndef ALP_TESTING_MAX_RESET_HOOKS
#define ALP_TESTING_MAX_RESET_HOOKS 16
#endif

static alp_testing_reset_hook_fn g_hooks[ALP_TESTING_MAX_RESET_HOOKS];
static size_t                    g_hook_count;

void alp_testing_register_reset_hook(alp_testing_reset_hook_fn fn)
{
	if (fn == NULL) return;

	/* Idempotent registration -- same rationale + pattern as
	 * alp_testing_instance_table_init's sweep-list dedup: a hook
	 * pointer already on the list is not added twice. */
	for (size_t i = 0; i < g_hook_count; ++i) {
		if (g_hooks[i] == fn) return;
	}
	if (g_hook_count < ALP_TESTING_MAX_RESET_HOOKS) {
		g_hooks[g_hook_count++] = fn;
	}
}

void alp_testing_reset_hooks_run_all(void)
{
	for (size_t i = 0; i < g_hook_count; ++i) {
		g_hooks[i]();
	}
}
