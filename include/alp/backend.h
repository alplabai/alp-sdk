/**
 * @file backend.h
 * @brief Backend registration + selection API.
 *
 * Each peripheral subsystem (ADC, SPI, ...) carries a class name.
 * Backends register themselves into a per-class linker section via
 * the ALP_BACKEND_REGISTER macro.  alp_&lt;class&gt;_open() walks the
 * section once, picks a backend by silicon_ref + priority, caches
 * the choice, and dispatches through ops thereafter.
 *
 * See docs/architecture/backend-registry.md for the design
 * narrative.
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 new.  Promoted to [ABI-STABLE] after three vendor
 *      families exercise the registry.
 */

#ifndef ALP_BACKEND_H
#define ALP_BACKEND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One row in a per-class linker section.
 *
 * @par Special silicon_ref:
 *      "*" -- wildcard.  Matches any SoC.  A wildcard row does NOT
 *             lose automatically to an exact-match row: the selector
 *             ranks by `priority` first and only prefers exact over
 *             wildcard as a tiebreaker at equal priority (see
 *             alp_backend_select()).  A high-priority wildcard can
 *             therefore win over a lower-priority exact backend.
 *             Commonly used by software fallback backends.
 */
typedef struct alp_backend {
	const char *silicon_ref;
	const char *vendor;
	uint32_t    base_caps;
	uint8_t     priority;
	const void *ops;
	int (*probe)(uint32_t instance_id, uint32_t *refined_caps);
} alp_backend_t;

/**
 * @brief Per-class section range descriptor.
 *
 * Populated by ALP_BACKEND_DEFINE_CLASS in each class dispatcher.
 * The selector walks the alp_backend_classes section to find the
 * range for the requested class_name.
 */
typedef struct alp_backend_class_range {
	const char          *class_name;
	const alp_backend_t *start;
	const alp_backend_t *stop;
} alp_backend_class_range_t;

/**
 * @brief Register a backend into the per-class section.
 *
 * Expands to a static const struct in section ".alp_backends_&lt;class&gt;".
 * The linker collects every such entry into a contiguous array.
 *
 * @param class  Class name (e.g. adc, spi, inference).  Becomes part of
 *               the section name; must be a valid C identifier.
 * @param name   Backend identifier (e.g. alif_e7, sw_fallback).  Must be
 *               unique within a class; appears in the symbol name.
 * @param ...    Brace-enclosed initializer for alp_backend_t.
 */
/* The `used` attribute keeps the symbol through compile-time stripping;
 * `retain` (GCC 11+) keeps it through linker `--gc-sections`.  Zephyr's
 * native_sim builds enable --gc-sections aggressively and drop the
 * registration without `retain`. */
/* Section name MUST be a valid C identifier (no leading dot) so GNU
 * ld auto-emits __start_alp_backends_&lt;class&gt; and __stop_&lt;class&gt;
 * bound symbols.  `retain` keeps the entry through --gc-sections. */
#define ALP_BACKEND_REGISTER(class, name, ...)                                                     \
	static const alp_backend_t _alp_be_##class##_##name __attribute__((                            \
	    used, retain, aligned(__alignof__(alp_backend_t)), section("alp_backends_" #class))) =     \
	    __VA_ARGS__

/**
 * @brief Define the class-range table entry for a per-class section.
 *
 * Each class dispatcher (typically the file that implements
 * alp_&lt;class&gt;_open) instantiates this once.  Tells the selector how
 * to find the section for a class name.
 */
#define ALP_BACKEND_DEFINE_CLASS(class)                                                            \
	extern const alp_backend_t __start_alp_backends_##class[];                                     \
	extern const alp_backend_t __stop_alp_backends_##class[];                                      \
	/* aligned(__alignof__(struct alp_backend_class_range)) forces the
     * entries to pack contiguously at the struct's natural alignment
     * (8 bytes on LP64).  Without this the section min-alignment can
     * exceed sizeof(struct), the linker inserts trailing pad between
     * entries, and the walker `++c` reads garbage from the pad.  */                             \
	static const alp_backend_class_range_t _alp_class_range_##class __attribute__((                \
	    used,                                                                                      \
	    retain,                                                                                    \
	    aligned(__alignof__(alp_backend_class_range_t)),                                           \
	    section("alp_backend_classes"))) = {                                                       \
		.class_name = #class,                                                                      \
		.start      = __start_alp_backends_##class,                                                \
		.stop       = __stop_alp_backends_##class,                                                 \
	}

/**
 * @brief Static-archive link anchor for a per-class registry section.
 *
 * On a plain-CMake STATIC libalp_sdk.a the archive member that carries
 * a class's ALP_BACKEND_REGISTER entry (typically its sw_fallback TU)
 * is nothing but data in the `alp_backends_&lt;class&gt;` linker section -- no
 * code references it.  A static-library link only pulls a member when
 * an already-included object needs one of its symbols, so that member
 * never joins the link; the section is then absent and the
 * __start_/__stop_alp_backends_&lt;class&gt; bounds ALP_BACKEND_DEFINE_CLASS
 * reads from the (always-linked) dispatcher go undefined -- the link
 * fails with `undefined reference to __start_alp_backends_&lt;class&gt;`
 * (issue #368).
 *
 * The anchor closes that gap.  The section-carrying backend TU exports
 * one global symbol via ALP_BACKEND_ANCHOR_DEFINE(class); the
 * dispatcher TU takes its address via ALP_BACKEND_ANCHOR(class).  The
 * dispatcher is always pulled (it owns alp_&lt;class&gt;_open), so the
 * address-of forces the backend member -- and with it the section and
 * its bound symbols -- into the link.
 *
 * Only plain-CMake static links need this, so the two macros are inert
 * unless the top-level CMake defines ALP_BACKEND_STATIC_ANCHORS (the
 * non-Zephyr build).  Zephyr links whole objects (zephyr_library), so
 * the member is always present and the anchors compile to nothing.
 *
 * Adding a NEW registry class that ships a portable catch-all backend
 * reachable from the plain-CMake path: call ALP_BACKEND_ANCHOR_DEFINE
 * once in that backend's TU and ALP_BACKEND_ANCHOR once in the
 * dispatcher.  Both are no-ops on Zephyr, so they can sit
 * unconditionally next to the ALP_BACKEND_REGISTER / DEFINE_CLASS they
 * pair with.
 */
#ifdef ALP_BACKEND_STATIC_ANCHORS
#define ALP_BACKEND_ANCHOR_DEFINE(class)                                                           \
	const int _alp_backend_anchor_##class __attribute__((used, retain)) = 0
#define ALP_BACKEND_ANCHOR(class)                                                                  \
	extern const int         _alp_backend_anchor_##class;                                          \
	static const void *const _alp_backend_anchor_ref_##class __attribute__((used)) =               \
	    (const void *)&_alp_backend_anchor_##class
#else
/* Whole-archive (Zephyr) links never need the anchor: expand to a bare
 * declaration so the call sites still take a trailing semicolon while
 * emitting no code or symbols. */
#define ALP_BACKEND_ANCHOR_DEFINE(class) extern const int _alp_backend_anchor_decl_##class
#define ALP_BACKEND_ANCHOR(class)        extern const int _alp_backend_anchor_decl_##class
#endif

/**
 * @brief Find the best backend registered for a class on the active SoC.
 *
 * Walks the per-class section, filters by silicon_ref exact match or
 * "*" wildcard, then picks the winner using the following tiebreaker
 * (applied in order, see issue #30):
 *
 *   1. Higher `priority` wins.
 *   2. At equal priority, an exact silicon_ref match beats "*" wildcard.
 *   3. At equal priority and same match-type, the lower `vendor`
 *      string (strcmp) wins.  Pins the choice deterministically
 *      regardless of linker object order.
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @param silicon_ref  Active SoC reference (e.g. "alif:ensemble:e7").
 *                     Pass ALP_SOC_REF_STR from <alp/soc_caps.h>.
 * @return  Pointer to the chosen backend, or NULL if none match.
 */
const alp_backend_t *alp_backend_select(const char *class_name, const char *silicon_ref);

/**
 * @brief Find the next-best backend AFTER a previously selected one.
 *
 * Walks the same per-class section as alp_backend_select() with the
 * same silicon_ref filter and tiebreaker, but only considers entries
 * that rank strictly below @p prev in the selection order.  Lets a
 * dispatcher fall through to the next candidate when the currently
 * selected backend declines an open() with ALP_ERR_NOSUPPORT (e.g.
 * an algorithm the hardware path does not implement) instead of
 * surfacing the decline as a hard application error (issue #239).
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @param silicon_ref  Active SoC reference (e.g. "alif:ensemble:e7").
 *                     Pass ALP_SOC_REF_STR from <alp/soc_caps.h>.
 * @param prev         Backend returned by a previous alp_backend_select()
 *                     or alp_backend_select_next() call for this class.
 *                     NULL behaves exactly like alp_backend_select().
 * @return  The best-ranked matching backend below @p prev, or NULL when
 *          no lower-ranked candidate remains.
 */
const alp_backend_t *
alp_backend_select_next(const char *class_name, const char *silicon_ref, const alp_backend_t *prev);

/**
 * @brief Count backends registered for a class (any silicon).
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @return  Number of entries in the .alp_backends_&lt;class&gt; section.
 */
size_t alp_backend_count(const char *class_name);

/**
 * @brief Runtime check: is any backend linked in for this class?
 *
 * Resolves at runtime via alp_backend_count.  Use in `if(...)` —
 * NOT preprocessor `#if`, since the count is a function call, not
 * a constant expression.  For compile-time pruning, customer code
 * should use ALP_HAS(<CAP>) from <alp/cap.h> which queries the
 * SoC-level capability table.
 */
#define ALP_BACKEND_AVAILABLE(class) (alp_backend_count(#class) > 0u)

#ifdef __cplusplus
}
#endif

#endif /* ALP_BACKEND_H */
