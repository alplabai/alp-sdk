/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_inference dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Zephyr/M-class registry backends:
 *
 *   - tflm           (priority 50, "*")  -- portable TFLM CPU executor.
 *   - ethos_u_aen    (priority 100, alif:ensemble:e3/e4/e5/e6/e7/e8)
 *                                       -- TFLM + Ethos-U op resolver on AEN.
 *   - ethos_u_n93    (priority 100, nxp:imx9:imx93)
 *                                       -- TFLM + Ethos-U op resolver on i.MX 93.
 *   - sw_fallback    (priority 0,   "*") -- stateless NOSUPPORT stub.
 *
 * There is deliberately NO M-class DRP-AI or DEEPX DX-M1 backend:
 * both engines are A55/Linux-side only (MERA runtime / PCIe libdxrt;
 * issues #58/#59) and live in src/yocto/inference_{drpai,deepx}.cpp
 * behind the Yocto dispatcher, not this registry.
 *
 * Vendor extensions plug in via the alp_<vendor>_inference_*
 * surface defined in <alp/ext/<vendor>/inference.h>; each
 * function gates on the handle's backend->vendor string before
 * touching hardware.
 *
 * Zephyr leakage: backends type `dev` as void * so the dispatcher
 * TU stays free of <zephyr/device.h>.  Each backend casts in/out
 * of its own TU.
 */

#ifndef ALP_BACKENDS_INFERENCE_OPS_H
#define ALP_BACKENDS_INFERENCE_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

typedef struct alp_inference_ops alp_inference_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_inference_backend_state {
	void                      *dev;        /* opaque backend device pointer */
	alp_inference_backend_t    backend_id; /* AUTO-resolved or caller-pinned */
	void                      *be_data;    /* backend-private state */
	const alp_inference_ops_t *ops;
} alp_inference_backend_state_t;

/** Vtable each inference backend implements. */
struct alp_inference_ops {
	alp_status_t (*open)(const alp_inference_config_t  *cfg,
	                     alp_inference_backend_state_t *state,
	                     alp_capabilities_t            *caps_out);
	size_t (*num_inputs)(alp_inference_backend_state_t *state);
	size_t (*num_outputs)(alp_inference_backend_state_t *state);
	alp_status_t (*get_input)(alp_inference_backend_state_t *state,
	                          size_t                         index,
	                          alp_inference_tensor_t        *out);
	alp_status_t (*get_output)(alp_inference_backend_state_t *state,
	                           size_t                         index,
	                           alp_inference_tensor_t        *out);
	alp_status_t (*invoke)(alp_inference_backend_state_t *state);
	void (*close)(alp_inference_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_inference alp_inference_t;` forward
 * declaration in <alp/inference.h>.  Defined here so the
 * dispatcher (src/inference_dispatch.c) and any per-backend .c /
 * .cpp files can access the fields without duplicating the
 * layout.
 */
struct alp_inference {
	alp_inference_backend_state_t state;
	const alp_backend_t          *backend;
	alp_capabilities_t            cached_caps;
	/* Last successful alp_inference_invoke()'s wall-clock duration, in
	 * microseconds -- written by the dispatcher itself (it brackets
	 * ops->invoke), never by a backend.  uint32_t, NOT uint64_t
	 * (issue: 64-bit atomics don't link on Cortex-M -- M-profile has
	 * no 64-bit atomic instruction, GCC lowers __atomic_{load,store}_n
	 * on a uint64_t to __atomic_load_8/__atomic_store_8 libatomic
	 * calls, and the Zephyr SDK's arm-zephyr-eabi toolchain ships no
	 * libatomic, so every Cortex-M app failed to link the moment this
	 * dispatcher -- which zephyr/CMakeLists.txt compiles
	 * unconditionally -- pulled one in). A naturally-aligned 32-bit
	 * load/store IS lock-free on every Cortex-M this SDK targets (a
	 * single LDR/STR, exactly like active_ops/lifecycle below), so
	 * __atomic_store_n/__atomic_load_n on this field compiles to plain
	 * instructions with no libcall. The tradeoff is range: values
	 * saturate at UINT32_MAX - 1 us (~4294.967294 s, just under the
	 * ~71.58-minute field ceiling) -- see
	 * alp_inference_last_invoke_latency_us()'s header doc
	 * (<alp/inference.h>) for the ceiling and the saturate-not-wrap
	 * behaviour. UINT32_MAX itself is RESERVED as the "no successful
	 * invoke yet" sentinel and deliberately never written by a real
	 * measurement: src/inference_dispatch.c's alp_inference_invoke()
	 * clamps any duration that reaches the raw UINT32_MAX ceiling down
	 * to UINT32_MAX - 1, precisely so a genuinely saturated reading
	 * stays distinguishable from "never invoked" instead of colliding
	 * with it (an earlier version of this field clamped to UINT32_MAX
	 * and reasoned the exact-all-ones coincidence "wasn't worth
	 * guarding" -- wrong: the clamp itself made the WHOLE tail
	 * [UINT32_MAX, inf) of measured durations land on that value, not
	 * a 1-in-2^32 coincidence, so every one of them silently read back
	 * as ALP_ERR_NOT_READY with *out_us never written). With the
	 * UINT32_MAX - 1 clamp, alp_inference_last_invoke_latency_us()
	 * (src/inference_dispatch.c) can treat UINT32_MAX as an
	 * unambiguous sentinel and translate it to ALP_ERR_NOT_READY at
	 * the public API boundary without ever misreading a real
	 * (saturated) measurement that way. The claim-time memset in
	 * _alloc() zeroes this field along with everything else before
	 * in_use, so alp_inference_open() must explicitly (re)set it to
	 * UINT32_MAX once the handle is claimed -- a fresh handle must
	 * read NOT_READY, not "0 us". active_ops permits concurrent ops on one
	 * handle (it is a drain counter, not a mutex -- see
	 * alp_slot_claim.h), so a concurrent invoke()-writer and
	 * latency-reader is a real interleaving: always access this field
	 * via __atomic_store_n/__atomic_load_n, never a plain read/write
	 * (issue #629 discipline). The public accessor's out_us stays
	 * uint64_t (see <alp/inference.h>) -- only the internal storage
	 * narrowed, so this is not an ABI change to the new (still
	 * ABI-EXPERIMENTAL) accessor's signature. */
	uint32_t last_invoke_latency_us;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_INFERENCE_OPS_H */
