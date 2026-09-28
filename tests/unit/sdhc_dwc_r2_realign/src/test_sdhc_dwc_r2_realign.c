/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-testable coverage for sdhc_dwc_realign_r2_response()
 * (sdhc_dwc.h) -- the R2 (CID/CSD) realignment sdhc_dwc_read_response()
 * (sdhc_dwc.c) applies when CONFIG_SDHC_RSP_136_HAS_CRC is enabled.
 *
 * The vector below is the 4 RESP words a DWC_mshc controller would present
 * (before realignment) for a real CSD v2 register with CSD_STRUCTURE = 1,
 * READ_BL_LEN = 9, and C_SIZE = 0x3BAFF -- all other CSD fields zero. That
 * is CSD bits [127:8] (the 120-bit content the RESPxx registers hold
 * without the CRC byte at bits [7:0]) right-justified across response[0]
 * (least significant, RESP01) .. response[3] (most significant, RESP67):
 *
 *   response[0] = 0x00000000
 *   response[1] = 0x03baff15   (C_SIZE's low bits + READ_BL_LEN)
 *   response[2] = 0x00000900   (C_SIZE's high bits)
 *   response[3] = 0x00400000   (CSD_STRUCTURE)
 *
 * decoded the same way zephyr/subsys/sd/sd_ops.c:145-146 decodes a v2
 * C_SIZE: raw_csd[2] contributes the top 6 bits, raw_csd[1] the low 16.
 */
#include <zephyr/ztest.h>

#include "sdhc_dwc.h"

#define RESP0_PRE 0x00000000U
#define RESP1_PRE 0x03baff15U
#define RESP2_PRE 0x00000900U
#define RESP3_PRE 0x00400000U

/* zephyr/subsys/sd/sd_ops.c:98 -- CSD_STRUCTURE, raw_csd[3] bits [31:30]. */
static uint8_t decode_csd_structure(const uint32_t raw_csd[4])
{
	return (uint8_t)((raw_csd[3] & 0xC0000000U) >> 30U);
}

/* zephyr/subsys/sd/sd_ops.c:103 -- READ_BL_LEN, raw_csd[2] bits [19:16]. */
static uint8_t decode_read_bl_len(const uint32_t raw_csd[4])
{
	return (uint8_t)((raw_csd[2] & 0xF0000U) >> 16U);
}

/* zephyr/subsys/sd/sd_ops.c:145-146 -- v2 C_SIZE/device_size: raw_csd[2]
 * bits [5:0] as the top 6 bits, raw_csd[1] bits [31:16] as the low 16.
 */
static uint32_t decode_c_size(const uint32_t raw_csd[4])
{
	uint32_t device_size = (raw_csd[2] & 0x3FU) << 16U;

	device_size |= (raw_csd[1] & 0xFFFF0000U) >> 16U;
	return device_size;
}

ZTEST_SUITE(sdhc_dwc_r2_realign, NULL, NULL, NULL, NULL, NULL);

/* Green: the current (index 3->0, carry from response[i - 1]) loop must
 * decode the CSD fields correctly, including C_SIZE = 0x3BAFF -- the field
 * that was truncated to 0xBAFF by the old (index 0->3, carry from
 * response[i + 1]) loop, and reported as 524800 sectors with no
 * realignment at all (#2131).
 */
ZTEST(sdhc_dwc_r2_realign, test_new_loop_decodes_csd_v2_correctly)
{
	uint32_t response[4] = { RESP0_PRE, RESP1_PRE, RESP2_PRE, RESP3_PRE };

	sdhc_dwc_realign_r2_response(response);

	zassert_equal(decode_csd_structure(response), 1, "CSD_STRUCTURE must decode to v2 (1)");
	zassert_equal(decode_read_bl_len(response), 9, "READ_BL_LEN must decode to 9");
	zassert_equal(decode_c_size(response),
	              0x3BAFFU,
	              "C_SIZE must decode to 0x3BAFF, got 0x%x",
	              decode_c_size(response));

	/* A field that lives entirely in the LOW words: response[0] (the
	 * least significant word, RESP01) has no word below index 0 to carry
	 * from, so its new low byte must be zero-filled, not polluted with
	 * data pulled from the wrong neighbour.
	 */
	zassert_equal(response[0],
	              0x00000000U,
	              "response[0]'s low byte must be zero-filled, got 0x%08x",
	              response[0]);
}

/* Red (documents the bug, does not call the helper): the OLD carry
 * direction -- index 0->3, carrying from response[i + 1] -- truncates
 * C_SIZE to 0xBAFF (49020928 sectors instead of 250347520, E1M-AEN803 serial 2026W36-0002)
 * and pollutes response[0]'s low byte with response[1]'s top byte instead
 * of leaving it zero. This proves the fix is the carry direction, not
 * merely that the shift ran.
 */
ZTEST(sdhc_dwc_r2_realign, test_old_loop_would_truncate_c_size)
{
	uint32_t response[4] = { RESP0_PRE, RESP1_PRE, RESP2_PRE, RESP3_PRE };

	for (int i = 0; i < 4; i++) {
		response[i] <<= 8;
		if (i != 3) {
			response[i] |= response[i + 1] >> 24;
		}
	}

	zassert_equal(decode_c_size(response),
	              0xBAFFU,
	              "old carry direction truncates C_SIZE to 0xBAFF, got 0x%x",
	              decode_c_size(response));
	zassert_not_equal(
	    decode_c_size(response), 0x3BAFFU, "old carry direction must NOT recover the true C_SIZE");
	zassert_equal(response[0],
	              0x00000003U,
	              "old carry direction pollutes response[0]'s low byte, got 0x%08x",
	              response[0]);
}

/* No realignment at all (the dead CONFIG_SDHC_RSP_136_HAS_CRC symbol,
 * before this fix): every field is read 8 bits off, including
 * CSD_STRUCTURE itself -- a v2 card gets parsed as v1.
 */
ZTEST(sdhc_dwc_r2_realign, test_no_realignment_misreads_every_field)
{
	uint32_t response[4] = { RESP0_PRE, RESP1_PRE, RESP2_PRE, RESP3_PRE };

	zassert_not_equal(decode_csd_structure(response),
	                  1,
	                  "unrealigned response must NOT decode CSD_STRUCTURE as v2");
	zassert_not_equal(
	    decode_read_bl_len(response), 9, "unrealigned response must NOT decode READ_BL_LEN as 9");
}
