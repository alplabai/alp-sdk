/* SPDX-License-Identifier: Apache-2.0
 *
 * Fictional "demo" peripheral class used only to exercise the
 * backend registry end-to-end without bringing real silicon into
 * the unit test.
 */

#ifndef DEMO_CLASS_H
#define DEMO_CLASS_H

#include <stdint.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>

typedef struct demo_handle {
	const alp_backend_t *backend;
	alp_capabilities_t   caps;
} demo_handle_t;

typedef struct demo_ops {
	int (*open)(demo_handle_t *h, uint32_t instance_id);
	int (*read)(demo_handle_t *h, uint32_t *out);
} demo_ops_t;

/* Returns 0 on success, negative ALP_ERR_* on failure. */
int                       demo_open(demo_handle_t *h, uint32_t instance_id);
int                       demo_read(demo_handle_t *h, uint32_t *out);
const alp_capabilities_t *demo_capabilities(const demo_handle_t *h);

#endif /* DEMO_CLASS_H */
