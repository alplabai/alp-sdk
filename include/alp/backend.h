/**
 * @file backend.h
 * @brief Backend registration + selection API.
 *
 * Each peripheral subsystem (ADC, SPI, ...) carries a class name.
 * Backends register themselves into a per-class linker section via
 * the ALP_BACKEND_REGISTER macro.  alp_<class>_open() walks the
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
 *      "*" -- wildcard.  Considered only when no exact-match backend
 *             exists.  Used by software fallback backends.
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
 * Expands to a static const struct in section ".alp_backends_<class>".
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
 * ld auto-emits __start_alp_backends_<class> and __stop_<class>
 * bound symbols.  `retain` keeps the entry through --gc-sections. */
#define ALP_BACKEND_REGISTER(class, name, ...)                                                     \
	static const alp_backend_t _alp_be_##class##_##name                                            \
	    __attribute__((used, retain, aligned(__alignof__(alp_backend_t)),                          \
	                   section("alp_backends_" #class))) = __VA_ARGS__

/**
 * @brief Define the class-range table entry for a per-class section.
 *
 * Each class dispatcher (typically the file that implements
 * alp_<class>_open) instantiates this once.  Tells the selector how
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
	    used, retain, aligned(__alignof__(alp_backend_class_range_t)),                             \
	    section("alp_backend_classes"))) = {                                                       \
		.class_name = #class,                                                                      \
		.start      = __start_alp_backends_##class,                                                \
		.stop       = __stop_alp_backends_##class,                                                 \
	}

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
 * @brief Count backends registered for a class (any silicon).
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @return  Number of entries in the .alp_backends_<class> section.
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
