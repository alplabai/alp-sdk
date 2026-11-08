/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2983: mhuv2_send() wrote CH_SET without checking that the previous
 * word was consumed.  ipm_send() must return -EBUSY while CH_ST still shows
 * the word, leave CH_SET and ACCESS_REQUEST alone (the request may be held
 * high for the in-flight word).
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
	memset(&g_cfg, 0, sizeof(g_cfg));
	HW(g_regs.ACCESS_READY)           = MHU_ACC_RDY;
	DEVICE_MMIO_ROM_PTR(&g_dev)->addr = (mm_reg_t)&g_regs;
}

ZTEST_SUITE(ipm_mhuv2_send, NULL, NULL, before, NULL, NULL);

ZTEST(ipm_mhuv2_send, test_send_busy_when_previous_word_unconsumed)
{
	uint32_t word = 0xA5A5A5A5u;

	HW(g_regs.CHANNEL[1].CH_ST) = 0x80000000u; /* pending word, bit 0 clear */
	g_regs.CHANNEL[1].CH_SET    = 0x11111111u;
	g_regs.ACCESS_REQUEST       = MHU_ACC_REQ; /* held for the in-flight word */

	zassert_equal(ipm_send(&g_dev, 0, 1, &word, sizeof(word)), -EBUSY);
	zassert_equal(g_regs.CHANNEL[1].CH_SET, 0x11111111u, "pending word must not be overwritten");
	zassert_equal(g_regs.ACCESS_REQUEST, MHU_ACC_REQ, "access request untouched on -EBUSY");
}

ZTEST(ipm_mhuv2_send, test_send_ok_when_channel_free)
{
	uint32_t word = 0xA5A5A5A5u;

	zassert_equal(ipm_send(&g_dev, 0, 1, &word, sizeof(word)), 0);
	zassert_equal(g_regs.CHANNEL[1].CH_SET, word);
}

ZTEST(ipm_mhuv2_send, test_poll_out_busy_when_previous_word_unconsumed)
{
	uint32_t word = 0xA5A5A5A5u;

	HW(g_regs.CHANNEL[1].CH_ST) = 0x80000000u; /* pending word, bit 0 clear */
	g_regs.CHANNEL[1].CH_SET    = 0x11111111u;
	g_cfg.irq_type              = 1; /* poll_out is ENOTSUP without an IRQ type */
	g_regs.ACCESS_REQUEST       = MHU_ACC_REQ;

	zassert_equal(mhuv2_poll_out(&g_dev, 1, &word, sizeof(word), K_NO_WAIT), -EBUSY);
	zassert_equal(g_regs.CHANNEL[1].CH_SET, 0x11111111u, "pending word must not be overwritten");
	zassert_equal(g_regs.ACCESS_REQUEST, MHU_ACC_REQ, "access request untouched on -EBUSY");
}
