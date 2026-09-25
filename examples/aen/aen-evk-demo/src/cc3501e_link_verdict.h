/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2035: this demo's CC3501E GET_VERSION verdict, pulled out of main.c into
 * its own header so it is unit-testable without a board -- same pattern as
 * bmp581_verdict.h in this directory.
 *
 * A bench session on a board running 3.1 firmware reported "MAJOR MISMATCH"
 * and an overall phase FAIL while every functional sub-check (ping, MAC,
 * caps, scan, BLE) passed. The host driver is deliberately BILINGUAL during
 * the v3->v4 migration window: it accepts ALP_CC3501E_PROTOCOL_MAJOR (4,
 * this host's own wire) AND ALP_CC3501E_PROTOCOL_MAJOR_LEGACY (3, the
 * pre-4.0 predecessor -- see the paragraph above ALP_CC3501E_PROTOCOL_MAJOR
 * in <alp/protocol/cc3501e.h>). A legacy-major peer is the migration
 * WORKING, not a mismatch, so this demo's old two-way ver_ok boolean was
 * wrong to fail it.
 *
 * This expresses the identical acceptance rule as
 * cc3501e_fw_major_is_acceptable() (chips/cc3501e/cc3501e_core.c) again,
 * against the same two public constants -- that function is declared in
 * chips/cc3501e/cc3501e_internal.h, `chips/`-internal and not part of this
 * demo's reach, so it is re-expressed here rather than widening that
 * internal header's surface just for a demo.
 */
#ifndef ALP_EVK_DEMO_CC3501E_LINK_VERDICT_H
#define ALP_EVK_DEMO_CC3501E_LINK_VERDICT_H

#include <stdbool.h>

#include "alp/peripheral.h"       /* alp_status_t, ALP_OK */
#include "alp/protocol/cc3501e.h" /* ALP_CC3501E_PROTOCOL_MAJOR[_LEGACY], _MINOR */

/* Four, and only four, outcomes GET_VERSION can settle into -- see this
 * header's file comment for why the third is not a failure. */
typedef enum {
	CC3501E_LINK_VERDICT_MATCH,       /* MAJOR and MINOR both equal this host's build. */
	CC3501E_LINK_VERDICT_MINOR_AHEAD, /* MAJOR matches; MINOR differs -- additive (ADR 0033). */
	CC3501E_LINK_VERDICT_LEGACY,      /* fw on ALP_CC3501E_PROTOCOL_MAJOR_LEGACY, host bilingual
	                                   * -- the v3->v4 migration working, not a mismatch. */
	CC3501E_LINK_VERDICT_MISMATCH,    /* ver_rc failed, or fw_major is neither accepted value. */
} cc3501e_link_verdict_t;

/* Classify a GET_VERSION reply. Mirrors cc3501e_fw_major_is_acceptable()'s
 * two-constant rule (see file comment) but distinguishes MATCH from
 * MINOR_AHEAD from LEGACY, which that boolean-returning function does not
 * need to. */
static inline cc3501e_link_verdict_t
cc3501e_classify_link(alp_status_t ver_rc, unsigned fw_major, unsigned fw_minor)
{
	if (ver_rc != ALP_OK) {
		return CC3501E_LINK_VERDICT_MISMATCH;
	}
	if (fw_major == (unsigned)ALP_CC3501E_PROTOCOL_MAJOR_LEGACY) {
		return CC3501E_LINK_VERDICT_LEGACY;
	}
	if (fw_major != (unsigned)ALP_CC3501E_PROTOCOL_MAJOR) {
		return CC3501E_LINK_VERDICT_MISMATCH;
	}
	return (fw_minor == (unsigned)ALP_CC3501E_PROTOCOL_MINOR) ? CC3501E_LINK_VERDICT_MATCH
	                                                          : CC3501E_LINK_VERDICT_MINOR_AHEAD;
}

/* True for every outcome the phase's overall PASS/FAIL gates on as "the
 * link is usable" -- everything except a genuine MISMATCH. */
static inline bool cc3501e_link_verdict_ok(cc3501e_link_verdict_t v)
{
	return v != CC3501E_LINK_VERDICT_MISMATCH;
}

#endif /* ALP_EVK_DEMO_CC3501E_LINK_VERDICT_H */
