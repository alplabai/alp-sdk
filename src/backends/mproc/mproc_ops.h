/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_mproc dispatcher and per-backend
 * implementations.  Covers ALL THREE handle types -- alp_shmem_t,
 * alp_mbox_t, alp_hwsem_t -- which share a single 'mproc' class
 * registry per the design spec Section 4 decision: the three IPC
 * primitives live behind one header (<alp/mproc.h>) and the
 * backend that implements one always implements the other two on
 * the same SoC.  The ops vtable carries function pointers for all
 * three surfaces and the dispatcher owns three separate handle
 * pools (shmem + mbox + hwsem) keyed off the same backend.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_MPROC_OPS_H
#define ALP_BACKENDS_MPROC_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>

typedef struct alp_mproc_ops alp_mproc_ops_t;

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

typedef struct alp_shmem_backend_state {
	void                  *be_data;
	const alp_mproc_ops_t *ops;
} alp_shmem_backend_state_t;

typedef struct alp_mbox_backend_state {
	void                  *be_data;
	const alp_mproc_ops_t *ops;
} alp_mbox_backend_state_t;

typedef struct alp_hwsem_backend_state {
	void                  *be_data;
	const alp_mproc_ops_t *ops;
} alp_hwsem_backend_state_t;

/* ------------------------------------------------------------------ */
/* Combined ops vtable -- one entry per primitive op                   */
/* ------------------------------------------------------------------ */

struct alp_mproc_ops {
	/* ---- Shared memory ---- */
	alp_status_t (*shmem_open)(const alp_shmem_config_t  *cfg,
	                           alp_shmem_backend_state_t *state,
	                           alp_capabilities_t        *caps_out);
	alp_status_t (*shmem_view)(alp_shmem_backend_state_t *state, void **base_out, size_t *size_out);
	void (*shmem_close)(alp_shmem_backend_state_t *state);

	/* ---- Mailbox ---- */
	alp_status_t (*mbox_open)(const alp_mbox_config_t  *cfg,
	                          alp_mbox_backend_state_t *state,
	                          alp_capabilities_t       *caps_out);
	alp_status_t (*mbox_send)(alp_mbox_backend_state_t *state,
	                          const void               *data,
	                          size_t                    len,
	                          uint32_t                  timeout_ms);
	alp_status_t (*mbox_set_callback)(alp_mbox_backend_state_t *state,
	                                  alp_mbox_msg_cb_t         cb,
	                                  void                     *user);
	void (*mbox_close)(alp_mbox_backend_state_t *state);

	/* ---- Hardware semaphore ---- */
	alp_status_t (*hwsem_open)(uint32_t                   hwsem_id,
	                           alp_hwsem_backend_state_t *state,
	                           alp_capabilities_t        *caps_out);
	alp_status_t (*hwsem_try_lock)(alp_hwsem_backend_state_t *state);
	alp_status_t (*hwsem_lock)(alp_hwsem_backend_state_t *state, uint32_t timeout_ms);
	alp_status_t (*hwsem_unlock)(alp_hwsem_backend_state_t *state);
	void (*hwsem_close)(alp_hwsem_backend_state_t *state);
};

/* ------------------------------------------------------------------ */
/* Peer-core boot vtable (class "mproc_boot", handle-less)             */
/*                                                                     */
/* A SEPARATE registry class from 'mproc': releasing a peer core is a  */
/* boot-authority concern (SoC secure / system-controller firmware),   */
/* not an IPC-primitive concern, and its silicon-specific backend must */
/* not displace the portable zephyr_drv winner of the mproc class.     */
/* ------------------------------------------------------------------ */

typedef struct alp_mproc_boot_ops {
	/** Ask the boot authority to start @p core at @p entry_addr
	 *  (global address map).  Bounded; returns rather than hangs. */
	alp_status_t (*boot_core)(alp_core_id_t core, uintptr_t entry_addr);
} alp_mproc_boot_ops_t;

/* ------------------------------------------------------------------ */
/* Public handle layouts -- owned by the dispatcher pools              */
/* ------------------------------------------------------------------ */

struct alp_shmem {
	alp_shmem_backend_state_t state;
	const alp_backend_t      *backend;
	alp_capabilities_t        cached_caps;
	bool                      in_use;
};

struct alp_mbox {
	alp_mbox_backend_state_t state;
	const alp_backend_t     *backend;
	alp_capabilities_t       cached_caps;
	bool                     in_use;
};

struct alp_hwsem {
	alp_hwsem_backend_state_t state;
	const alp_backend_t      *backend;
	alp_capabilities_t        cached_caps;
	bool                      in_use;
};

#endif /* ALP_BACKENDS_MPROC_OPS_H */
