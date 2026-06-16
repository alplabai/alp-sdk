/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend selector: walks the per-class linker section, picks the
 * best match for the active silicon_ref by priority.
 *
 * The per-class sections are populated by ALP_BACKEND_REGISTER in
 * <alp/backend.h>.  The linker provides __start_/__stop_ symbols
 * automatically for sections whose names are valid C identifiers
 * after the leading dot is stripped (GCC/Clang behaviour).
 *
 * Each class dispatcher (alp_adc_open, etc.) instantiates an
 * alp_backend_class_range_t via ALP_BACKEND_DEFINE_CLASS, which
 * lands in the "alp_backend_classes" section.  The selector walks
 * that section to map class_name -> (start, stop), then walks the
 * per-class entries to find the best match.
 *
 * Selector tiebreaker (applied in order, see issue #30):
 *
 *   1. Higher priority wins.  Backends register with a uint8_t
 *      priority; the dispatcher prefers the larger value.
 *
 *   2. At equal priority, an exact silicon_ref match beats the
 *      "*" wildcard.  Rationale: a backend that named a specific
 *      silicon clearly intended to override generic catch-alls
 *      that happen to advertise the same priority.
 *
 *   3. At equal priority AND same match-type (both exact, or both
 *      wildcard), the lower vendor string (strcmp) wins.  This
 *      pins the choice deterministically regardless of linker
 *      object order.  Real codebases should not register two
 *      backends with the same {vendor, class, priority} -- this
 *      rule exists so the registry never depends on link order.
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <alp/backend.h>

/* The "alp_backend_classes" section's bounds.  The linker provides
 * these symbols when the section name is a valid C identifier. */
/* Strong refs so GNU ld emits the auto-generated section-bound
 * symbols (auto-emit triggers on a strong undefined reference to
 * __start_<id> / __stop_<id> when the section name is a valid C
 * identifier).  Each dispatcher's ALP_BACKEND_DEFINE_CLASS() drops
 * a retained entry into "alp_backend_classes", so the section
 * always has at least one entry on any non-trivial build. */
extern const alp_backend_class_range_t __start_alp_backend_classes[];
extern const alp_backend_class_range_t __stop_alp_backend_classes[];

static bool                            is_wildcard(const alp_backend_t *be)
{
	return (be->silicon_ref != NULL && be->silicon_ref[0] == '*' && be->silicon_ref[1] == '\0');
}

/* Returns true if `cand` should replace `best` under the tiebreaker
 * documented at the top of this file.  Caller guarantees both
 * candidates already passed the silicon_ref filter. */
static bool candidate_beats_best(const alp_backend_t *cand, const alp_backend_t *best)
{
	/* Tier 1: priority. */
	if (cand->priority != best->priority) {
		return cand->priority > best->priority;
	}
	/* Tier 2: exact silicon_ref beats "*" wildcard at equal priority. */
	const bool cand_wild = is_wildcard(cand);
	const bool best_wild = is_wildcard(best);
	if (cand_wild != best_wild) {
		return !cand_wild; /* cand is exact, best is wildcard -> swap */
	}
	/* Tier 3: alphabetic on vendor (deterministic, link-order independent).
     * Defensive: treat NULL vendor as the largest string so it loses. */
	if (cand->vendor == NULL) {
		return false;
	}
	if (best->vendor == NULL) {
		return true;
	}
	return strcmp(cand->vendor, best->vendor) < 0;
}

static const alp_backend_t *select_in_range(const alp_backend_t *start, const alp_backend_t *stop,
                                            const char *silicon_ref)
{
	const alp_backend_t *best = NULL;
	for (const alp_backend_t *be = start; be < stop; ++be) {
		const bool wild  = is_wildcard(be);
		const bool exact = (be->silicon_ref != NULL && silicon_ref != NULL &&
		                    strcmp(be->silicon_ref, silicon_ref) == 0);
		if (!wild && !exact) {
			continue;
		}
		if (best == NULL || candidate_beats_best(be, best)) {
			best = be;
		}
	}
	return best;
}

const alp_backend_t *alp_backend_select(const char *class_name, const char *silicon_ref)
{
	if (class_name == NULL) {
		return NULL;
	}
	if (silicon_ref == NULL) {
		return NULL;
	}
	for (const alp_backend_class_range_t *c = __start_alp_backend_classes;
	     c < __stop_alp_backend_classes; ++c) {
		if (strcmp(c->class_name, class_name) == 0) {
			return select_in_range(c->start, c->stop, silicon_ref);
		}
	}
	return NULL;
}

size_t alp_backend_count(const char *class_name)
{
	if (class_name == NULL) {
		return 0u;
	}
	for (const alp_backend_class_range_t *c = __start_alp_backend_classes;
	     c < __stop_alp_backend_classes; ++c) {
		if (strcmp(c->class_name, class_name) == 0) {
			return (size_t)(c->stop - c->start);
		}
	}
	return 0u;
}
