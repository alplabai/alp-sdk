/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2133: dmic_alif_pdm_configure() read req_chan_map_lo's low byte
 * VERBATIM as the hardware channel-enable mask instead of decoding the
 * standard Zephyr dmic_build_channel_map() nibble encoding -- for the
 * portable backend's exact 2-channel request (channel 0 = LEFT on PDM
 * controller 0, channel 1 = RIGHT on PDM controller 0), that verbatim read
 * produced 0x10 (hardware channel 4 only; the EVK's mics are on hardware
 * channels 0 and 1) instead of 0x03. Exercised here against the exact
 * translation dmic_alif_pdm_configure() now calls, on the host, with no
 * MMIO/devicetree/PDM-instance involved.
 */
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/ztest.h>

#include "alif_pdm_chanmap.h"
#include "alif_pdm_reg.h" /* PDM_CHANNEL_0/1/4/5 */

ZTEST_SUITE(alif_pdm_chanmap, NULL, NULL, NULL, NULL, NULL);

/* The exact repro from issue #2133: src/backends/audio/zephyr_drv.c's
 * 2-channel request (z_in_open(), zephyr_drv.c:286-291) must enable
 * hardware channels 0 and 1 -- not hardware channel 4.
 */
ZTEST(alif_pdm_chanmap, test_backend_2channel_map_enables_hw_0_and_1)
{
	uint32_t map_lo =
	    dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) | dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
	uint8_t mask = 0xFFU; /* poison so a no-op translate is caught */
	int     rc;

	/* The old, wrong interpretation this issue fixes: reading the raw
	 * map byte VERBATIM as the hardware mask. Ground the red/green claim
	 * in the test itself -- the correct mask below must differ from it.
	 */
	uint8_t old_verbatim_mask = (uint8_t)(map_lo & 0xFFU);

	zassert_equal(map_lo, 0x10U, "sanity: this is the exact issue #2133 evidence value");
	zassert_equal(
	    old_verbatim_mask, 0x10U, "the old driver enabled HW channel 4 only, not 0 and 1");

	rc = alif_pdm_chanmap_translate(map_lo, 0, 2, &mask);

	zassert_equal(rc, 0, "a 2-channel map on PDM controller 0 must be expressible");
	zassert_equal(mask, 0x03U, "LEFT@pdm0 + RIGHT@pdm0 must enable HW channels 0 and 1");
	zassert_not_equal(
	    mask, old_verbatim_mask, "fixed translation must differ from the old buggy verbatim read");
}

/* The bench-proven examples/aen/aen-pdm-mic-alif 4-channel map: D0 (pdm=0)
 * carries channels 0/1, D2 (pdm=2) carries channels 2/3 -- must enable HW
 * channels 0, 1, 4, 5 (the PDM_MASK_CHANNEL_0|1|4|5 the working example used
 * to pass by hand).
 */
ZTEST(alif_pdm_chanmap, test_example_4channel_map_enables_hw_0_1_4_5)
{
	uint32_t map_lo =
	    dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) | dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT) |
	    dmic_build_channel_map(2, 2, PDM_CHAN_LEFT) | dmic_build_channel_map(3, 2, PDM_CHAN_RIGHT);
	uint8_t mask = 0;
	int     rc   = alif_pdm_chanmap_translate(map_lo, 0, 4, &mask);

	zassert_equal(rc, 0, "a 4-channel map spanning PDM controllers 0 and 2 must be expressible");
	zassert_equal(mask,
	              (PDM_CHANNEL_0 | PDM_CHANNEL_1 | PDM_CHANNEL_4 | PDM_CHANNEL_5),
	              "must match the working example's raw PDM_MASK_CHANNEL_0|1|4|5");
}

/* A PDM controller index the 8-hw-channel part cannot express
 * (ALIF_PDM_MAX_CONTROLLERS = 4, so pdm=4 would need HW channels 8/9) must
 * be rejected outright, never silently truncated/masked.
 */
ZTEST(alif_pdm_chanmap, test_unexpressible_pdm_controller_rejected)
{
	uint32_t map_lo = dmic_build_channel_map(0, 4, PDM_CHAN_LEFT);
	uint8_t  mask   = 0xAAU;
	int      rc     = alif_pdm_chanmap_translate(map_lo, 0, 1, &mask);

	zassert_equal(rc, -EINVAL, "pdm=4 exceeds ALIF_PDM_MAX_CONTROLLERS and must be rejected");
	zassert_equal(mask, 0xAAU, "mask_out must be left untouched on failure");
}

/* req_num_chan out of range (0, or above the 8 hardware channels) must also
 * fail closed rather than decode a partial/garbage map.
 */
ZTEST(alif_pdm_chanmap, test_req_num_chan_out_of_range_rejected)
{
	uint8_t mask = 0x55U;

	zassert_equal(
	    alif_pdm_chanmap_translate(0, 0, 0, &mask), -EINVAL, "req_num_chan == 0 must be rejected");
	zassert_equal(alif_pdm_chanmap_translate(0, 0, 9, &mask),
	              -EINVAL,
	              "req_num_chan > MAX_NUM_CHANNELS (8) must be rejected");
	zassert_equal(mask, 0x55U, "mask_out must be left untouched on failure");
}
