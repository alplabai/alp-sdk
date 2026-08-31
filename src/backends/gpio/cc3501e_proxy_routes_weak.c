/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Alp Lab AB
 *
 * WEAK empty default for the CC3501E GPIO proxy's board route table.
 *
 * This is its OWN translation unit, deliberately separate from
 * cc3501e_proxy.c (which only ever sees the `extern` declaration in
 * <alp/chips/cc3501e/gpio.h>).  A weak `const` object defined in the same
 * TU that reads it gives the compiler a definition of the object it can
 * see at the read site; at -Os (CONFIG_SIZE_OPTIMIZATIONS=y, the AEN
 * default) the compiler folded cc3501e_gpio_route_count's zero initializer
 * straight into cc3501e_proxy.c's route_lookup() loop bound and eliminated
 * the loop entirely -- so a board's STRONG override in a third TU (e.g.
 * examples/aen/aen-cc3501e-gpio/src/cc3501e_gpio_routes.c) was linked but
 * never read (issue #1860, measured on the real target build: the proxy
 * object carried the weak symbol with zero relocations against it, and
 * the final ELF had neither symbol).
 *
 * Splitting the weak default into this file removes the compiler's only
 * chance to see both the definition and the read in the same
 * translation unit, so route_lookup() always compiles a genuine
 * indirect load through whichever definition the linker resolves --
 * weak (this file, nothing routed) or a board's strong override.
 *
 * Any future board-override table that follows this same shape (weak
 * `const` array + weak `const` count, read from elsewhere in the SDK)
 * needs its weak default split out exactly like this -- the pattern is
 * the bug, not this one instance.
 */

#include <stddef.h>

#include <alp/chips/cc3501e.h>

__attribute__((weak)) const cc3501e_gpio_route_t cc3501e_gpio_routes[]    = { 0 };
__attribute__((weak)) const size_t               cc3501e_gpio_route_count = 0u;

/*
 * Same reasoning as the route table above, for the unrouted list added by
 * #1854: defining a weak `const` array in the SAME translation unit that
 * reads it lets the compiler fold the count to 0 at -Os and drop the check
 * entirely, so a board's strong override becomes dead code (#1860).  It
 * lives here for exactly that reason -- do not move it back beside its
 * reader.
 */
__attribute__((weak)) const uint32_t cc3501e_gpio_unrouted[]     = { 0 };
__attribute__((weak)) const size_t   cc3501e_gpio_unrouted_count = 0u;
