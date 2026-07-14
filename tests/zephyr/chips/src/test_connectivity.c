/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Networking / RF chip smokes: rtl8211fdi (Ethernet PHY),
 * murata_lbee5hy2fy (Wi-Fi + BLE module GPIO surface),
 * clk_5l35023b (audio-rate clock generator, feeds the connectivity
 * codecs), and the v0.5 §D.iot batch cellular/LoRa/GNSS modules.
 */

#include <zephyr/ztest.h>

#include "alp/chips/atgm336h.h"
#include "alp/chips/clk_5l35023b.h"
#include "alp/chips/murata_lbee5hy2fy.h"
#include "alp/chips/quectel_bg77.h"
#include "alp/chips/quectel_bg95.h"
#include "alp/chips/rtl8211fdi.h"
#include "alp/chips/semtech_sx1262.h"
#include "alp/chips/semtech_sx1276.h"
#include "alp/chips/ublox_max_m10s.h"
#include "alp/chips/ublox_neo_m9n.h"
#include "alp/chips/ublox_sara_r5.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* rtl8211fdi -- Realtek PHY driver, NULL-arg validation              */
/* ------------------------------------------------------------------ */

static int test_dummy_mdio_read(uint8_t phy, uint8_t reg, uint16_t *val, void *user)
{
	(void)phy;
	(void)reg;
	(void)user;
	*val = 0u;
	return 0;
}
static int test_dummy_mdio_write(uint8_t phy, uint8_t reg, uint16_t val, void *user)
{
	(void)phy;
	(void)reg;
	(void)val;
	(void)user;
	return 0;
}

ZTEST(alp_chips, test_rtl8211fdi_init_null_args)
{
	rtl8211fdi_t ctx;
	zassert_equal(rtl8211fdi_init(NULL, 0u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(rtl8211fdi_init(&ctx, 0u, NULL, test_dummy_mdio_write, NULL), ALP_ERR_INVAL);
	zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read, NULL, NULL), ALP_ERR_INVAL);
	/* PHY address > 31 (5-bit address space) must be rejected. */
	zassert_equal(rtl8211fdi_init(&ctx, 32u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
	              ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_rtl8211fdi_init_oui_check_rejects_zero)
{
	/* Dummy callbacks read 0x0000 for every register -- PHYID1
     * OUI check should reject (Realtek OUI is 0x001C). */
	rtl8211fdi_t ctx;
	zassert_equal(rtl8211fdi_init(&ctx, 0u, test_dummy_mdio_read, test_dummy_mdio_write, NULL),
	              ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_rtl8211fdi_post_init_rejects_uninitialised)
{
	rtl8211fdi_t ctx = { 0 };

	bool               up;
	rtl8211fdi_speed_t speed;
	bool               fd;
	zassert_equal(rtl8211fdi_get_link(&ctx, &up, &speed, &fd), ALP_ERR_NOT_READY);
	zassert_equal(rtl8211fdi_soft_reset(&ctx, 1000u), ALP_ERR_NOT_READY);
	zassert_equal(rtl8211fdi_restart_autoneg(&ctx), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* clk_5l35023b -- Renesas/IDT audio-rate clock generator stub        */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_clk_5l35023b_init_null_args)
{
	clk_5l35023b_t ctx;
	alp_i2c_t     *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 100000,
	});
	zassert_not_null(bus);

	zassert_equal(clk_5l35023b_init(NULL, bus, CLK_5L35023B_I2C_ADDR_DEFAULT),
	              ALP_ERR_INVAL,
	              "NULL ctx must be rejected");
	zassert_equal(clk_5l35023b_init(&ctx, NULL, CLK_5L35023B_I2C_ADDR_DEFAULT),
	              ALP_ERR_INVAL,
	              "NULL bus must be rejected");
	/* 0x80 is out of 7-bit range. */
	zassert_equal(
	    clk_5l35023b_init(&ctx, bus, 0x80u), ALP_ERR_INVAL, "addr > 0x7F must be rejected");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_clk_5l35023b_raw_rw_rejects_uninitialised)
{
	/* Without a real chip behind the emul controller, the I2C ACK
     * probe in clk_5l35023b_init will fail and the driver stays in
     * its zero state.  All subsequent register accesses must report
     * NOT_READY rather than IO. */
	clk_5l35023b_t ctx = { 0 };

	uint8_t v;
	zassert_equal(clk_5l35023b_read_reg(&ctx, 0u, &v), ALP_ERR_NOT_READY);
	zassert_equal(clk_5l35023b_write_reg(&ctx, 0u, 0xFFu), ALP_ERR_NOT_READY);

	uint8_t dump[8];
	zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, dump, sizeof dump), ALP_ERR_NOT_READY);

	/* deinit on a zero context is a no-op (idempotent). */
	clk_5l35023b_deinit(&ctx);
	clk_5l35023b_deinit(NULL);
}

ZTEST(alp_chips, test_clk_5l35023b_register_dump_rejects_invalid)
{
	/* Force the .initialised flag so the function passes the
     * NOT_READY gate and reaches its INVAL argument-validation
     * branch.  This is the same trick used by the gd32g553 tests. */
	clk_5l35023b_t ctx = { .initialised = true };

	/* count == 0 -> INVAL even with a non-NULL out. */
	uint8_t out[4];
	zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, out, 0u), ALP_ERR_INVAL);

	/* NULL out -> INVAL even with a positive count. */
	zassert_equal(clk_5l35023b_register_dump(&ctx, 0u, NULL, 1u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_clk_5l35023b_typed_helpers_reject_uninitialised)
{
	/* New typed surface added with the datasheet integration --
     * Dash-Code-ID read, strap-address decode, soft power-down.
     * Each must report NOT_READY on a zeroed context. */
	clk_5l35023b_t            ctx = { 0 };
	uint8_t                   dashcode;
	clk_5l35023b_strap_addr_t strap;

	zassert_equal(clk_5l35023b_read_dashcode_id(&ctx, &dashcode), ALP_ERR_NOT_READY);
	zassert_equal(clk_5l35023b_get_strap_addr(&ctx, &strap), ALP_ERR_NOT_READY);
	zassert_equal(clk_5l35023b_set_power_down(&ctx, true), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_clk_5l35023b_typed_helpers_validate_args)
{
	/* .initialised forced so the function reaches the NULL-out
     * check before any bus access. */
	clk_5l35023b_t ctx = { .initialised = true };

	zassert_equal(clk_5l35023b_read_dashcode_id(&ctx, NULL), ALP_ERR_INVAL);
	zassert_equal(clk_5l35023b_get_strap_addr(&ctx, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_clk_5l35023b_get_strap_addr_decodes_general_ctrl)
{
	/* The strap address lives in Byte 0x00 bits[6:5].  Drive the
     * cached general_ctrl byte through each of the four strap
     * encodings and confirm the decoded enum value matches. */
	clk_5l35023b_t ctx = { .initialised = true };

	static const struct {
		uint8_t                   gc_byte;
		clk_5l35023b_strap_addr_t expected;
	} cases[] = {
		{ .gc_byte = 0x00u, .expected = CLK_5L35023B_STRAP_ADDR_0X68 },
		{ .gc_byte = 0x20u, .expected = CLK_5L35023B_STRAP_ADDR_0X69 },
		{ .gc_byte = 0x40u, .expected = CLK_5L35023B_STRAP_ADDR_0X6A },
		{ .gc_byte = 0x60u, .expected = CLK_5L35023B_STRAP_ADDR_0X6B },
	};

	for (size_t i = 0u; i < ARRAY_SIZE(cases); ++i) {
		ctx.general_ctrl              = cases[i].gc_byte;
		clk_5l35023b_strap_addr_t got = (clk_5l35023b_strap_addr_t)0xFFu;
		zassert_equal(clk_5l35023b_get_strap_addr(&ctx, &got), ALP_OK);
		zassert_equal((unsigned)got,
		              (unsigned)cases[i].expected,
		              "gc_byte=0x%02X: expected strap %u, got %u",
		              cases[i].gc_byte,
		              (unsigned)cases[i].expected,
		              (unsigned)got);
	}
}

/* ------------------------------------------------------------------ */
/* murata_lbee5hy2fy -- Wi-Fi 6 + BLE 5.4 module GPIO surface         */
/*                                                                    */
/* The driver delegates the REG_ON outputs to caller-supplied         */
/* callbacks (because on V2N those lines live on the GD32 supervisor  */
/* MCU and aren't reachable through Zephyr's GPIO API).  The fake     */
/* callbacks below capture every set / get into module-local arrays   */
/* so the tests can observe what the driver wrote.                    */
/* ------------------------------------------------------------------ */

static bool fake_murata_reg_state[2];
static int  fake_murata_set_calls;

static int fake_murata_reg_set(murata_reg_t which, bool enable, void *user)
{
	(void)user;
	fake_murata_reg_state[(int)which] = enable;
	++fake_murata_set_calls;
	return 0;
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_init_null_args)
{
	murata_lbee5hy2fy_t ctx;
	/* NULL ctx -> INVAL. */
	zassert_equal(murata_lbee5hy2fy_init(NULL, fake_murata_reg_set, NULL, NULL, NULL, NULL, NULL),
	              ALP_ERR_INVAL);
	/* NULL reg_set callback -> INVAL.  reg_get is optional so it stays
     * NULL here. */
	zassert_equal(murata_lbee5hy2fy_init(&ctx, NULL, NULL, NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_power_calls_reject_uninitialised)
{
	/* Zero-init the ctx; driver sees .initialised == false and must
     * report NOT_READY rather than IO from the power helpers. */
	murata_lbee5hy2fy_t ctx = { 0 };

	zassert_equal(murata_lbee5hy2fy_bt_power(&ctx, true), ALP_ERR_NOT_READY);
	zassert_equal(murata_lbee5hy2fy_wl_power(&ctx, true), ALP_ERR_NOT_READY);

	bool level;
	zassert_equal(murata_lbee5hy2fy_bt_host_wake_level(&ctx, &level), ALP_ERR_NOT_READY);
	zassert_equal(murata_lbee5hy2fy_wl_host_wake_level(&ctx, &level), ALP_ERR_NOT_READY);

	/* deinit on uninitialised must be a safe no-op. */
	murata_lbee5hy2fy_deinit(&ctx);
	murata_lbee5hy2fy_deinit(NULL);
}

ZTEST(alp_chips, test_murata_lbee5hy2fy_bt_wake_returns_nosupport_when_pin_null)
{
	/* Init the ctx with a working reg_set callback and NULL
     * bt_dev_wake (the V2N convention -- the line is not routed).
     * bt_wake_device() must report NOSUPPORT (not NOT_READY). */
	fake_murata_set_calls    = 0;
	fake_murata_reg_state[0] = true; /* seed non-zero to verify init drives low. */
	fake_murata_reg_state[1] = true;

	murata_lbee5hy2fy_t ctx;
	alp_status_t        s =
	    murata_lbee5hy2fy_init(&ctx, fake_murata_reg_set, NULL, NULL, NULL, NULL, NULL);
	zassert_equal(s, ALP_OK, "init with NULL bt_dev_wake must succeed (V2N path)");
	zassert_equal(fake_murata_set_calls, 2, "init must drive BOTH regulators low");
	zassert_false(fake_murata_reg_state[(int)MURATA_REG_BT], "init must drive BT_REG_ON low");
	zassert_false(fake_murata_reg_state[(int)MURATA_REG_WL], "init must drive WL_REG_ON low");

	/* bt_dev_wake handle is NULL by construction. */
	zassert_equal(murata_lbee5hy2fy_bt_wake_device(&ctx), ALP_ERR_NOSUPPORT);

	murata_lbee5hy2fy_deinit(&ctx);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.iot batch -- cellular / LoRa / GNSS modules                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_quectel_bg95_init_null_args)
{
	quectel_bg95_t dev;
	zassert_equal(quectel_bg95_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(quectel_bg95_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_quectel_bg77_init_null_args)
{
	quectel_bg77_t dev;
	zassert_equal(quectel_bg77_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(quectel_bg77_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_sara_r5_init_null_args)
{
	ublox_sara_r5_t dev;
	zassert_equal(ublox_sara_r5_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ublox_sara_r5_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_semtech_sx1262_init_null_args)
{
	semtech_sx1262_t dev;
	zassert_equal(semtech_sx1262_init(NULL, NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(semtech_sx1262_init(&dev, NULL, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_semtech_sx1276_init_null_args)
{
	semtech_sx1276_t dev;
	zassert_equal(semtech_sx1276_init(NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(semtech_sx1276_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_neo_m9n_init_null_args)
{
	ublox_neo_m9n_t dev;
	zassert_equal(ublox_neo_m9n_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ublox_neo_m9n_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_ublox_max_m10s_init_null_args)
{
	ublox_max_m10s_t dev;
	zassert_equal(ublox_max_m10s_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(ublox_max_m10s_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_atgm336h_init_null_args)
{
	atgm336h_t dev;
	zassert_equal(atgm336h_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(atgm336h_init(&dev, NULL), ALP_ERR_INVAL);
}
