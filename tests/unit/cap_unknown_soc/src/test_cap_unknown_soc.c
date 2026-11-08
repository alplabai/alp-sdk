/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * With no CONFIG_ALP_SOC_* selected, every ALP_SOC_* macro defaults to
 * UINT16_MAX.  soc_caps.h promises that ALP_CAP_* collapse to exactly
 * 0 or 1, so the flag caps must not pass that raw value through.
 */

#include <zephyr/ztest.h>

#include <alp/soc_caps.h>

ZTEST_SUITE(alp_cap_unknown_soc, NULL, NULL, NULL, NULL, NULL);

#define CAP_IS_BOOL(cap) ((ALP_CAP_##cap) == 0 || (ALP_CAP_##cap) == 1)

ZTEST(alp_cap_unknown_soc, test_every_cap_is_exactly_0_or_1)
{
	zassert_true(CAP_IS_BOOL(HW_I2C));
	zassert_true(CAP_IS_BOOL(HW_CAN_FD));
	zassert_true(CAP_IS_BOOL(XSPI_DMA));
	zassert_true(CAP_IS_BOOL(HEXSPI_DMA));
	zassert_true(CAP_IS_BOOL(EMMC_DMA));
	zassert_true(CAP_IS_BOOL(QUADSPI_DMA));
	zassert_true(CAP_IS_BOOL(NPU_DRPAI));
	zassert_true(CAP_IS_BOOL(HELIUM_MVE));
	zassert_true(CAP_IS_BOOL(NEON));
	zassert_true(CAP_IS_BOOL(GPU2D));
	zassert_true(CAP_IS_BOOL(DAVE2D));
	zassert_true(CAP_IS_BOOL(CRYPTOCELL));
	zassert_true(CAP_IS_BOOL(INLINE_AES));
	zassert_true(CAP_IS_BOOL(CAU));
	zassert_true(CAP_IS_BOOL(DMA2D));
}
