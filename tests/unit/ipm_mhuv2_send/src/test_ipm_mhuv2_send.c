/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2983: mhuv2_send() wrote CH_SET without checking that the previous
 * word was consumed.  ipm_send() must return -EBUSY while CH_ST still shows
 * the word, leave CH_SET alone, and drop the access request again.
 */
/* CONFIG_ARM_MHUV2 is unset, so the ISR/init statics have no DT instance. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../../zephyr/drivers/ipm/ipm_arm_mhuv2.c"

#include <zephyr/ztest.h>

/* The register block marks status fields read-only; the model plays the
 * hardware side, so it writes them through this cast. */
#define HW(reg) (*(volatile uint32_t *)&(reg))

static struct MHUV2_SND           g_regs;
static struct mhuv2_device_config g_cfg;
static struct mhuv2_device_data   g_data = { .max_ch = 4 };
static const struct device g_dev = { .config = &g_cfg, .api = &mhuv2_driver_api, .data = &g_data };

static void before(void *fixture)
{
	(void)fixture;
	memset((void *)&g_regs, 0, sizeof(g_regs));
	HW(g_regs.ACCESS_READY)           = MHU_ACC_RDY;
	DEVICE_MMIO_ROM_PTR(&g_dev)->addr = (mm_reg_t)&g_regs;
}

ZTEST_SUITE(ipm_mhuv2_send, NULL, NULL, before, NULL, NULL);

ZTEST(ipm_mhuv2_send, test_send_busy_when_previous_word_unconsumed)
{
	uint32_t word = 0xA5A5A5A5u;

	HW(g_regs.CHANNEL[1].CH_ST) = MHU_CH_INT_ST_SET; /* receiver has not cleared it */
	g_regs.CHANNEL[1].CH_SET    = 0x11111111u;

	zassert_equal(ipm_send(&g_dev, 0, 1, &word, sizeof(word)), -EBUSY);
	zassert_equal(g_regs.CHANNEL[1].CH_SET, 0x11111111u, "pending word must not be overwritten");
	zassert_equal(g_regs.ACCESS_REQUEST, 0u, "access request dropped on -EBUSY");
}

ZTEST(ipm_mhuv2_send, test_send_ok_when_channel_free)
{
	uint32_t word = 0xA5A5A5A5u;

	zassert_equal(ipm_send(&g_dev, 0, 1, &word, sizeof(word)), 0);
	zassert_equal(g_regs.CHANNEL[1].CH_SET, word);
}
