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
 */

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <alp/backend.h>

/* The "alp_backend_classes" section's bounds.  The linker provides
 * these symbols when the section name is a valid C identifier. */
extern const alp_backend_class_range_t __start_alp_backend_classes[];
extern const alp_backend_class_range_t __stop_alp_backend_classes[];

static const alp_backend_t *select_in_range(const alp_backend_t *start,
                                            const alp_backend_t *stop,
                                            const char *silicon_ref)
{
    const alp_backend_t *best = NULL;
    for (const alp_backend_t *be = start; be < stop; ++be) {
        const bool wild = (be->silicon_ref != NULL
                           && be->silicon_ref[0] == '*'
                           && be->silicon_ref[1] == '\0');
        const bool exact = (be->silicon_ref != NULL
                            && silicon_ref != NULL
                            && strcmp(be->silicon_ref, silicon_ref) == 0);
        if (!wild && !exact) {
            continue;
        }
        if (best == NULL || be->priority > best->priority) {
            best = be;
        }
    }
    return best;
}

const alp_backend_t *alp_backend_select(const char *class_name,
                                        const char *silicon_ref)
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
