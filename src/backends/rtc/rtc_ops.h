/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_rtc dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 */

#ifndef ALP_BACKENDS_RTC_OPS_H
#define ALP_BACKENDS_RTC_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rtc.h>

typedef struct alp_rtc_ops alp_rtc_ops_t;

typedef struct alp_rtc_backend_state {
	void                *dev; /* opaque backend device pointer
                                           * (const struct device * on Zephyr;
                                           * kept void* so the portable handle
                                           * does not pull in <zephyr/device.h>) */
	uint32_t             rtc_id;
	void                *be_data;
	const alp_rtc_ops_t *ops;
} alp_rtc_backend_state_t;

struct alp_rtc_ops {
	alp_status_t (*open)(uint32_t                 rtc_id,
	                     alp_rtc_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*set_time)(alp_rtc_backend_state_t *state, const alp_rtc_time_t *time);
	alp_status_t (*get_time)(alp_rtc_backend_state_t *state, alp_rtc_time_t *time);
	void (*close)(alp_rtc_backend_state_t *state);
};

struct alp_rtc {
	alp_rtc_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_RTC_OPS_H */
