/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_audio dispatcher and per-backend
 * implementations.  The audio class is unusual: it carries TWO
 * stateful handle types behind a single header (<alp/audio.h>):
 *
 *   - alp_audio_in_t   -- microphone capture (PDM / DMIC / line-in)
 *   - alp_audio_out_t  -- speaker playback   (I2S DAC / line-out)
 *
 * Per design spec Section 4: ONE class registry covers both
 * directions because a backend that wires up the active SoC's audio
 * path always implements both (the Zephyr backend lifts the DMIC
 * input AND the alp_i2s_*-delegated output from the legacy
 * src/zephyr/audio_zephyr.c file).  The ops vtable carries function
 * pointers for both surfaces; the dispatcher owns two separate
 * handle pools (in + out).
 *
 * Mirrors the multi-handle shape of security_ops.h (hash + AEAD)
 * but every op carries the customer's alp_audio_config_t on the
 * state struct so backends can specialise without an extra
 * parameter on every I/O op.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_AUDIO_OPS_H
#define ALP_BACKENDS_AUDIO_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_audio_ops alp_audio_ops_t;

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

typedef struct alp_audio_in_backend_state {
	alp_audio_config_t     cfg;
	void                  *be_data;
	const alp_audio_ops_t *ops;
} alp_audio_in_backend_state_t;

typedef struct alp_audio_out_backend_state {
	alp_audio_config_t     cfg;
	uint8_t                volume; /* current 0..255 linear scale */
	void                  *be_data;
	const alp_audio_ops_t *ops;
} alp_audio_out_backend_state_t;

/* ------------------------------------------------------------------ */
/* Combined ops vtable -- one entry per primitive op                   */
/* ------------------------------------------------------------------ */

struct alp_audio_ops {
	/* ---- Input (microphone) ---- */
	alp_status_t (*in_open)(const alp_audio_config_t     *cfg,
	                        alp_audio_in_backend_state_t *state,
	                        alp_capabilities_t           *caps_out);
	alp_status_t (*in_start)(alp_audio_in_backend_state_t *state);
	alp_status_t (*in_stop)(alp_audio_in_backend_state_t *state);
	alp_status_t (*in_read)(alp_audio_in_backend_state_t *state,
	                        void                         *buf,
	                        size_t                        frames,
	                        size_t                       *out_frames,
	                        uint32_t                      timeout_ms);
	void (*in_close)(alp_audio_in_backend_state_t *state);

	/* ---- Output (speaker) ---- */
	alp_status_t (*out_open)(const alp_audio_config_t      *cfg,
	                         alp_audio_out_backend_state_t *state,
	                         alp_capabilities_t            *caps_out);
	alp_status_t (*out_start)(alp_audio_out_backend_state_t *state);
	alp_status_t (*out_stop)(alp_audio_out_backend_state_t *state);
	alp_status_t (*out_write)(alp_audio_out_backend_state_t *state,
	                          const void                    *buf,
	                          size_t                         frames,
	                          size_t                        *out_frames,
	                          uint32_t                       timeout_ms);
	alp_status_t (*out_set_volume)(alp_audio_out_backend_state_t *state, uint8_t vol);
	void (*out_close)(alp_audio_out_backend_state_t *state);
};

/* ------------------------------------------------------------------ */
/* Public handle layouts -- owned by the dispatcher pools              */
/* ------------------------------------------------------------------ */

struct alp_audio_in {
	alp_audio_in_backend_state_t state;
	const alp_backend_t         *backend;
	alp_capabilities_t           cached_caps;
	bool                         in_use;
};

struct alp_audio_out {
	alp_audio_out_backend_state_t state;
	const alp_backend_t          *backend;
	alp_capabilities_t            cached_caps;
	bool                          in_use;
};

#endif /* ALP_BACKENDS_AUDIO_OPS_H */
