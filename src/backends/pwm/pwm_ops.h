/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_pwm dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_PWM_OPS_H
#define ALP_BACKENDS_PWM_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/pwm.h>

typedef struct alp_pwm_ops alp_pwm_ops_t;

typedef struct alp_pwm_backend_state {
	void                *dev;        /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t             channel_id; /* studio-resolved E1M index 0..7 */
	void                *be_data;
	const alp_pwm_ops_t *ops;
} alp_pwm_backend_state_t;

/*
 * Unified vtable: drive-side + capture-side ops live on a single
 * table so the capture handle can be allocated from the same pool
 * and dispatch through the same backend selection.  Backends that
 * lack a one-shot or input-capture primitive return
 * ALP_ERR_NOSUPPORT from the relevant entry.
 */
struct alp_pwm_ops {
	alp_status_t (*open)(const alp_pwm_config_t *cfg, alp_pwm_backend_state_t *state,
	                     alp_capabilities_t *caps_out);
	alp_status_t (*set_duty)(alp_pwm_backend_state_t *state, uint32_t pulse_ns);
	alp_status_t (*set_period)(alp_pwm_backend_state_t *state, uint32_t period_ns);
	alp_status_t (*configure)(alp_pwm_backend_state_t *state, alp_pwm_align_t align_mode,
	                          uint32_t dead_time_ns, uint8_t break_cfg);
	alp_status_t (*single_pulse)(alp_pwm_backend_state_t *state, uint32_t pulse_ns);
	alp_status_t (*capture_open)(const alp_pwm_capture_config_t *cfg,
	                             alp_pwm_backend_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*capture_read)(alp_pwm_backend_state_t *state, uint32_t *period_ns_out,
	                             uint32_t *pulse_ns_out);
	void (*capture_close)(alp_pwm_backend_state_t *state);
	void (*close)(alp_pwm_backend_state_t *state);
};

/*
 * Portable handle layout.  The period_ns / flags / channel fields
 * that previously lived in src/zephyr/handles.h are inlined here so
 * the backend can fill them at open time; the dispatcher reads
 * period_ns to bounds-check pulse_ns before forwarding to the
 * backend.
 */
struct alp_pwm {
	alp_pwm_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
	uint32_t                channel;   /* hardware channel within state->dev */
	uint32_t                period_ns; /* current configured period */
	uint32_t                flags;     /* backend-private polarity / DT flags */
};

/*
 * Capture handle shares the backend-state plumbing with the drive
 * handle but is allocated from a separate pool so the same channel
 * can be re-opened as output after capture_close without leaking
 * state.
 */
struct alp_pwm_capture {
	alp_pwm_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_PWM_OPS_H */
