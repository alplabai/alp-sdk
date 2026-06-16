/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_storage dispatcher and per-backend
 * implementations.  NOT a public header.
 *
 * Storage has two Zephyr-side backends (raw flash_area, littlefs)
 * plus a stateless SW fallback.  Vendor extensions for inline AES
 * (Alif OSPI SecAES, NXP FlexSPI OTFAD) plug in via additional
 * registrations once their vendor packs land; the
 * configure_inline_aes op is part of every backend's vtable so the
 * portable code path treats it as a first-class operation.
 *
 * Zephyr leakage: the backend state types `dev` as void * so the
 * dispatcher TU stays free of <zephyr/device.h>.  Each backend
 * casts in/out of its own TU.
 */

#ifndef ALP_BACKENDS_STORAGE_OPS_H
#define ALP_BACKENDS_STORAGE_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/storage.h>

typedef struct alp_storage_ops alp_storage_ops_t;

typedef struct alp_storage_backend_state {
	void                    *dev; /* opaque backend device pointer */
	uint32_t                 instance_id;
	alp_storage_kind_t       kind;
	bool                     read_only;
	void                    *be_data; /* backend-private state */
	const alp_storage_ops_t *ops;
} alp_storage_backend_state_t;

struct alp_storage_ops {
	alp_status_t (*open)(const alp_storage_config_t *cfg, alp_storage_backend_state_t *state,
	                     alp_capabilities_t *caps_out);
	alp_status_t (*get_info)(alp_storage_backend_state_t *state, alp_storage_info_t *info);
	alp_status_t (*read)(alp_storage_backend_state_t *state, uint64_t offset, void *data,
	                     size_t len);
	alp_status_t (*write)(alp_storage_backend_state_t *state, uint64_t offset, const void *data,
	                      size_t len);
	alp_status_t (*erase)(alp_storage_backend_state_t *state, uint64_t offset, uint64_t len);
	alp_status_t (*sync)(alp_storage_backend_state_t *state);
	alp_status_t (*configure_inline_aes)(alp_storage_backend_state_t    *state,
	                                     const alp_storage_aes_config_t *cfg);
	void (*close)(alp_storage_backend_state_t *state);
};

struct alp_storage {
	alp_storage_backend_state_t state;
	const alp_backend_t        *backend;
	alp_capabilities_t          cached_caps;
	bool                        in_use;
};

#endif /* ALP_BACKENDS_STORAGE_OPS_H */
