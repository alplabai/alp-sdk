/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_dsp dispatcher and per-backend
 * implementations.  The DSP class carries ONE stateful handle type
 * (alp_dsp_chain_t) -- a composable FIR / IIR / window / FFT pipeline
 * configured at chain_open() time and replayed against each input
 * buffer via apply_samples() / apply_bins().
 *
 * Mirrors the rtc / audio sibling shape: backend-owned state struct
 * carries the ops pointer + a backend-private context pointer
 * (state->be_data) the dispatcher does not interpret.  The portable
 * sw_fallback backend stashes its 8 KB CMSIS-DSP / portable-radix-2
 * working set behind state->be_data; a future HW-FFT backend (V2N
 * GD32G5 supervisor) would stash a stream-handle there instead.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_DSP_OPS_H
#define ALP_BACKENDS_DSP_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dsp.h>
#include <alp/peripheral.h>

typedef struct alp_dsp_ops alp_dsp_ops_t;

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

typedef struct alp_dsp_backend_state {
	void                *be_data; /* Backend-private CMSIS-DSP /
                                        radix-2 / HW-stream context;
                                        allocated by ops->open and
                                        torn down by ops->close. */
	const alp_dsp_ops_t *ops;
} alp_dsp_backend_state_t;

/* ------------------------------------------------------------------ */
/* Ops vtable -- one entry per public DSP primitive                    */
/* ------------------------------------------------------------------ */

struct alp_dsp_ops {
	alp_status_t (*open)(const alp_dsp_stage_t   *stages,
	                     size_t                   n_stages,
	                     alp_dsp_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*apply_samples)(alp_dsp_backend_state_t *state,
	                              const int16_t           *in_mv,
	                              size_t                   in_n,
	                              int16_t                 *out_mv,
	                              size_t                   out_cap,
	                              size_t                  *got);
	alp_status_t (*apply_samples_f32)(alp_dsp_backend_state_t *state,
	                                  const float             *in,
	                                  size_t                   in_n,
	                                  float                   *out,
	                                  size_t                   out_cap,
	                                  size_t                  *got);
	alp_status_t (*apply_bins)(alp_dsp_backend_state_t *state,
	                           const int16_t           *in_mv,
	                           size_t                   in_n,
	                           float                   *out_bins,
	                           size_t                   out_cap,
	                           size_t                  *got);
	alp_status_t (*apply_bins_f32)(alp_dsp_backend_state_t *state,
	                               const float             *in,
	                               size_t                   in_n,
	                               float                   *out_bins,
	                               size_t                   out_cap,
	                               size_t                  *got);
	void (*close)(alp_dsp_backend_state_t *state);
};

/* ------------------------------------------------------------------ */
/* Public handle layout -- owned by the dispatcher pool                */
/* ------------------------------------------------------------------ */

struct alp_dsp_chain {
	alp_dsp_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_DSP_OPS_H */
