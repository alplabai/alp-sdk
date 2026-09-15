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

/* examples/aen/aen-pdm-mic-alif's 4-channel map (register-level verified on
 * both pairs; acoustic capture at 48 kHz verified by speaker loopback on
 * mic ch0/ch1, PDM controller 0, ONLY -- issue #2133 round 4f, see
 * alif_pdm.c's STATUS banner. The D2 pair (HW 4/5) is register-level
 * verified only, never acoustically tested): D0 (pdm=0) carries channels
 * 0/1, D2 (pdm=2) carries channels 2/3 -- must enable HW channels 0, 1, 4,
 * 5 (the PDM_MASK_CHANNEL_0|1|4|5 the example used to pass by hand).
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
	              "must match the example's raw PDM_MASK_CHANNEL_0|1|4|5");
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

/* L/R is untestable by a map that enables both edges of the same pair (round
 * 2 review finding): a mutant that swaps LEFT/RIGHT in the (pdm*2)+lr
 * formula still passes test_backend_2channel_map_enables_hw_0_and_1 and
 * test_example_4channel_map_enables_hw_0_1_4_5, because both request LEFT
 * *and* RIGHT on every PDM controller they use, so the resulting mask is
 * identical either way. These three single-channel cases each request only
 * ONE edge, so a swapped formula flips the expected bit and fails them (red/
 * green proof against the `(pdm * 2U) + (1U - (uint8_t)lr)` mutant recorded
 * in the PR/commit description, not kept as a permanent test).
 */
ZTEST(alif_pdm_chanmap, test_single_channel_left_enables_hw_0_only)
{
	uint32_t map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	uint8_t  mask   = 0;
	int      rc     = alif_pdm_chanmap_translate(map_lo, 0, 1, &mask);

	zassert_equal(rc, 0, "a single LEFT channel on pdm0 must be expressible");
	zassert_equal(
	    mask, 0x01U, "LEFT@pdm0 (even/rising, HWRM 15.7.4.3.1) must be HW channel 0 only");
}

ZTEST(alif_pdm_chanmap, test_single_channel_right_enables_hw_1_only)
{
	uint32_t map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT);
	uint8_t  mask   = 0;
	int      rc     = alif_pdm_chanmap_translate(map_lo, 0, 1, &mask);

	zassert_equal(rc, 0, "a single RIGHT channel on pdm0 must be expressible");
	zassert_equal(
	    mask, 0x02U, "RIGHT@pdm0 (odd/falling, HWRM 15.7.4.3.1) must be HW channel 1 only");
}

ZTEST(alif_pdm_chanmap, test_single_channel_pdm2_right_enables_hw_5_only)
{
	uint32_t map_lo = dmic_build_channel_map(0, 2, PDM_CHAN_RIGHT);
	uint8_t  mask   = 0;
	int      rc     = alif_pdm_chanmap_translate(map_lo, 0, 1, &mask);

	zassert_equal(rc, 0, "a single RIGHT channel on pdm2 must be expressible");
	zassert_equal(mask, 0x20U, "RIGHT@pdm2 must be HW channel 5 (bit 5) only");
}

/* Two logical channels naming the SAME hw channel (both LEFT on pdm0) must
 * be rejected: alif_pdm.c stores num_channels = req_num_chan (2 here), but
 * the mask would only ever enable one hw channel, so the ISR would copy a
 * repeated/stale sample into the second logical slot every burst.
 */
ZTEST(alif_pdm_chanmap, test_duplicate_hw_channel_rejected)
{
	uint32_t map_lo =
	    dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) | dmic_build_channel_map(1, 0, PDM_CHAN_LEFT);
	uint8_t mask = 0x77U;
	int     rc   = alif_pdm_chanmap_translate(map_lo, 0, 2, &mask);

	zassert_equal(rc, -EINVAL, "two logical channels naming the same hw channel must be rejected");
	zassert_equal(mask, 0x77U, "mask_out must be left untouched on failure");
}

/* Logical channel 0 naming a HIGHER hw channel than logical channel 1 (RIGHT
 * then LEFT on the same pdm) must be rejected: the ISR always de-interleaves
 * in ascending hw-channel order, so this map would silently swap the two
 * logical channels' data rather than error.
 */
ZTEST(alif_pdm_chanmap, test_out_of_order_hw_channel_rejected)
{
	uint32_t map_lo =
	    dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT) | dmic_build_channel_map(1, 0, PDM_CHAN_LEFT);
	uint8_t mask = 0x77U;
	int     rc   = alif_pdm_chanmap_translate(map_lo, 0, 2, &mask);

	zassert_equal(rc, -EINVAL, "a non-ascending hw-channel map must be rejected");
	zassert_equal(mask, 0x77U, "mask_out must be left untouched on failure");
}

/* pdm_ch_gain_clamp() (alif_pdm_reg.h, issue #2133 round 5): PDM_CH_GAIN's
 * GAIN field is only 12 bits -- pdm_set_ch_gain() (alif_pdm.c) clamps every
 * write through this pure function instead of writing an unclamped value
 * that would truncate to bits [11:0] (0x1000 exactly -> 0 = mute). Host-
 * tested here (no MMIO/device involved) so this boundary is exercised
 * without a full driver build; mutation-checked: removing the clamp (an
 * unconditional `return ch_gain;`) fails exactly the two out-of-range
 * cases below.
 */
ZTEST(alif_pdm_chanmap, test_gain_clamp_max_value_passes_through)
{
	zassert_equal(pdm_ch_gain_clamp(PDM_CH_GAIN_MAX),
	              PDM_CH_GAIN_MAX,
	              "the largest valid 12-bit value must pass through unchanged");
}

ZTEST(alif_pdm_chanmap, test_gain_clamp_mute_boundary_is_clamped)
{
	zassert_equal(pdm_ch_gain_clamp(PDM_CH_GAIN_MAX + 1U),
	              PDM_CH_GAIN_MAX,
	              "0x1000 (the exact truncate-to-mute value) must be clamped to PDM_CH_GAIN_MAX, "
	              "not written unclamped");
}

ZTEST(alif_pdm_chanmap, test_gain_clamp_max_uint32_is_clamped)
{
	zassert_equal(pdm_ch_gain_clamp(0xFFFFFFFFU),
	              PDM_CH_GAIN_MAX,
	              "an arbitrary out-of-range caller value must clamp to PDM_CH_GAIN_MAX, "
	              "not truncate to whatever its low 12 bits happen to be");
}
