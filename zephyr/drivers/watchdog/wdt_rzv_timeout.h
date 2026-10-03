/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure timeout selection for the RZ/V2N WDT (no Zephyr or FSP dependency, so tests/unit can run it
 * on native_sim).  Register encodings are from the RZ/V2N hardware manual R01UH1071EJ0120 Rev.1.20,
 * 5.4.2.2.2 "WDT Control Register (WDTm_WDTCR)", Table 5.4-3:
 *   CKS[3:0]   0000b /1   0010b /16   0011b /32   0100b /64   1111b /128   0101b /256   others prohibited
 *   TOPS[1:0]  00b 1024   01b 4096   10b 8192   11b 16384 counts of the divided clock
 * (/4, /512, /2048 and /8192 exist in the generic FSP enum for other parts but are prohibited here.)
 */

#ifndef WDT_RZV_TIMEOUT_H_
#define WDT_RZV_TIMEOUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wdt_rzv_cks {
	uint8_t  cks;
	uint16_t divider;
};

static const struct wdt_rzv_cks wdt_rzv_cks_table[] = {
	{ 0x0, 1 }, { 0x2, 16 }, { 0x3, 32 }, { 0x4, 64 }, { 0xF, 128 }, { 0x5, 256 },
};

/* Counts per timeout, indexed by the TOPS[1:0] encoding. */
static const uint16_t wdt_rzv_tops_counts[] = { 1024, 4096, 8192, 16384 };

/* Longest encodable period (/256 x 16384 counts) rounded UP to whole ms: 175 at 24 MHz (174.76 ms).
 * A request above this is rejected by the driver, never clamped; the picked period itself is always
 * <= the request, so a request equal to the ceiling selects the longest period.
 */
static inline uint32_t wdt_rzv_ceiling_ms(uint32_t clock_freq)
{
	const uint64_t counts = 256ULL * 16384ULL;

	return clock_freq == 0 ? 0 : (uint32_t)((counts * 1000ULL + clock_freq - 1) / clock_freq);
}

/* True when a request of ms is at or below the longest encodable period (else the driver rejects it). */
static inline bool wdt_rzv_timeout_in_range(uint32_t clock_freq, uint32_t ms)
{
	return ms <= wdt_rzv_ceiling_ms(clock_freq);
}

/* Longest period not exceeding max_ms; false when even the shortest is longer. */
static inline bool wdt_rzv_pick(uint32_t clock_freq, uint32_t max_ms, uint8_t *tops, uint8_t *cks)
{
	uint64_t best_us = 0;

	if (clock_freq == 0) {
		return false;
	}

	for (size_t d = 0; d < sizeof(wdt_rzv_cks_table) / sizeof(wdt_rzv_cks_table[0]); d++) {
		for (size_t t = 0; t < sizeof(wdt_rzv_tops_counts) / sizeof(wdt_rzv_tops_counts[0]); t++) {
			uint64_t us = (uint64_t)wdt_rzv_cks_table[d].divider * wdt_rzv_tops_counts[t] *
			              1000000ULL / clock_freq;

			if (us <= (uint64_t)max_ms * 1000ULL && us > best_us) {
				best_us = us;
				*tops   = (uint8_t)t;
				*cks    = wdt_rzv_cks_table[d].cks;
			}
		}
	}

	return best_us != 0;
}

#endif /* WDT_RZV_TIMEOUT_H_ */
