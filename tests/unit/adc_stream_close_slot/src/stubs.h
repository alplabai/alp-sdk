/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared state between src/stubs.c (the doubles) and the test body.
 */

#ifndef ADC_STREAM_CLOSE_SLOT_STUBS_H_
#define ADC_STREAM_CLOSE_SLOT_STUBS_H_

#include <stdbool.h>
#include <stdint.h>

#include "alp/peripheral.h"

#define STUB_SCRIPT_MAX 8

struct stub_state {
	/* Scripted alp_z_v2n_supervisor_acquire() results, consumed in order;
	 * once exhausted it returns ALP_OK, or ALP_ERR_BUSY when always_busy. */
	alp_status_t script[STUB_SCRIPT_MAX];
	int          script_len;
	int          script_pos;
	bool         always_busy;

	bool     gd32_stream_active[2]; /* the modelled GD32's per-slot state */
	unsigned acquire_calls;
	unsigned release_calls;
	unsigned end_calls;

	alp_status_t last_error;
};

extern struct stub_state g_stub;

#endif /* ADC_STREAM_CLOSE_SLOT_STUBS_H_ */
