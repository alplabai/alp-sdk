/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2138: Phase 11's "is E1M IO8 -> CC3501E GPIO_30 safe to drive" gate,
 * pulled out of main.c so it is unit-testable without a board -- same
 * pattern as amp_fault_verdict.h / sound_verdict.h / cc3501e_link_verdict.h
 * in this directory.
 *
 * WHY THIS EXISTS. cc3501e_gpio_routes.c's E1M IO8 -> CC3501E GPIO_30 entry
 * (the I2S mux ENABLE phase 11 drives) is correct ONLY on hw_rev 2626-r2.
 * On 2626-r1, metadata/e1m_modules/aen/hw-revisions.yaml `pad_route_
 * overrides` moves IO8 to a direct Alif GPIO and re-routes IO21, not IO8,
 * onto CC3501E GPIO_30 -- which on an E1M-EVK 2626-R2 carrier is the SDIO
 * mux SELECT, tied to +3V3 by a fitted P18 jumper. Driving GPIO_30 low as
 * I2S_EN on an r1 module shorts a CC3501E output against +3V3. This gate is
 * the single place that "is it safe" decision is made, so it can be proven
 * with a unit test (tests/zephyr/chips/src/test_hw_rev_verdict.c) instead
 * of trusted by inspection -- a prior version of this gate was checked only
 * by grepping main.c for a token, which passed both a stub TODO in place of
 * the check and the check's own condition inverted (round-2 review of this
 * issue).
 *
 * TAKES alp_hw_info_read()'s OWN OUTPUT, not a second hand-rolled EEPROM
 * read. alp_hw_info_classify_manifest() (src/zephyr/hw_info_zephyr.c) --
 * the function alp_hw_info_read() itself calls -- already validates magic +
 * schema_version + CRC32 before it ever populates `som_hw_rev`, and that
 * validation is already exercised end-to-end (valid / blank-erased /
 * zeroed / bad-schema / bad-CRC manifests) by tests/zephyr/hw_info/src/
 * main.c. classify_manifest() itself is SDK-internal BY CONTRACT, not by
 * reach -- zephyr/CMakeLists.txt's zephyr_include_directories(src/common)
 * puts its header on every consuming app's include path app-wide, same as
 * tests/zephyr/hw_info/src/main.c uses it, so nothing stops app code from
 * calling it directly -- but only alp_hw_info_read()/alp_hw_info_t is the
 * PUBLIC <alp/hw_info.h> surface apps are meant to consume, and calling
 * classify_manifest() directly here would still require this app to
 * hand-roll its own raw EEPROM read first (classify_manifest() takes an
 * already-read manifest buffer, not a bus/address), reintroducing exactly
 * the second, app-owned read this file exists to avoid. So this gate calls
 * the public entry point instead, and only has to decide whether an
 * ALREADY-VALIDATED read names r2.
 */
#ifndef ALP_EVK_DEMO_HW_REV_VERDICT_H
#define ALP_EVK_DEMO_HW_REV_VERDICT_H

#include <stdbool.h>
#include <string.h>

#include "alp/hw_info.h"    /* ALP_HW_INFO_HW_REV_LEN */
#include "alp/peripheral.h" /* alp_status_t, ALP_OK */

/* The one hw_rev string cc3501e_gpio_routes.c's hand-written E1M IO8 ->
 * CC3501E GPIO_30 entry is correct for. */
#define AEN_EVKDEMO_HW_REV_IO8_SAFE "2626-r2"

/*
 * True iff `read_rc` is ALP_OK -- alp_hw_info_read() succeeded, which
 * itself required a magic + schema_version + CRC32-valid manifest -- AND
 * `som_hw_rev` names exactly AEN_EVKDEMO_HW_REV_IO8_SAFE. Anything else --
 * a read failure, an unprovisioned/corrupt manifest, r1, a same-prefix
 * string like "2626-r2X", or a future r3+ -- returns false, and the caller
 * must not open E1M IO8 (or drive CC3501E GPIO_30) when it does.
 */
static inline bool
aen_evkdemo_hw_rev_confirms_io8_safe(alp_status_t read_rc,
                                     const char   som_hw_rev[ALP_HW_INFO_HW_REV_LEN])
{
	if (read_rc != ALP_OK) return false;
	/* strncmp bounded by the field's own size, not strcmp: alp_hw_info_
	 * classify_manifest()'s copy_field() (src/zephyr/hw_info_zephyr.c)
	 * always NUL-terminates som_hw_rev on the ALP_OK path this function
	 * relies on -- so THAT specific caller could safely use strcmp() --
	 * but this function's own signature accepts a bare
	 * char[ALP_HW_INFO_HW_REV_LEN] with no documented terminator
	 * guarantee, and defending the bound here rather than trusting every
	 * possible caller costs nothing. An 8-byte field compared 8 bytes
	 * deep against an 8-byte literal (7 chars + NUL) still requires an
	 * exact match including the terminator, so a hypothetical
	 * full-width, unterminated buffer like "2626-r2X" correctly
	 * disagrees on that last byte instead of appearing to match a
	 * truncated prefix -- see
	 * test_hw_rev_verdict_refuses_unterminated_field's own comment for
	 * why that case has to call this function directly rather than
	 * through alp_hw_info_classify_manifest(), which would silently fix
	 * such an input before it ever reached here. */
	return strncmp(som_hw_rev, AEN_EVKDEMO_HW_REV_IO8_SAFE, ALP_HW_INFO_HW_REV_LEN) == 0;
}

#endif /* ALP_EVK_DEMO_HW_REV_VERDICT_H */
