/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADR-0017-ADJACENT, BENCH-UNVERIFIED (see display_sn65dsi83.c): the I2C half of the
 * SN65DSI83 init sequence (datasheet Table 7-2, steps 5-10) and the health pass built
 * on it.  Shared by the driver (init, and its own poll where it owns the bus) and the
 * recovery agent (the core that owns the bus after a handover), so the init is written
 * once and a recovery is, by construction, the init again.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sn65dsi83_recovery.h"

LOG_MODULE_REGISTER(sn65dsi83_rec, LOG_LEVEL_INF);

/*
 * Datasheet Table 7-4: "Addresses 0x08 - 0x00 = {0x01, 0x20, 0x20, 0x20,
 * 0x44, 0x53, 0x49, 0x38, 0x35}" -- the list is high-address-first, so 0x08
 * holds 0x01 and 0x00 holds 0x35.  A 9-byte burst read ascending from 0x00
 * therefore returns the list REVERSED: "58ISD   " + 0x01, i.e. "DSI85" read
 * backwards.  BENCH-UNVERIFIED -- the first real read-back settles it.
 */
static const uint8_t sn65dsi83_expected_id[SN65_REG_ID_LEN] = {
	0x35U, 0x38U, 0x49U, 0x53U, 0x44U, 0x20U, 0x20U, 0x20U, 0x01U,
};

int sn65dsi83_check_id(const struct i2c_dt_spec *i2c)
{
	uint8_t id[SN65_REG_ID_LEN];
	int     ret;

	ret = i2c_burst_read_dt(i2c, SN65_REG_ID_BASE, id, sizeof(id));
	if (ret != 0) {
		LOG_ERR("ID read failed (%d) -- EN high but the bridge did not answer I2C", ret);
		return ret;
	}

	if (memcmp(id, sn65dsi83_expected_id, sizeof(id)) != 0) {
		LOG_ERR("Unexpected ID: got %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
		        "want %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		        id[0],
		        id[1],
		        id[2],
		        id[3],
		        id[4],
		        id[5],
		        id[6],
		        id[7],
		        id[8],
		        sn65dsi83_expected_id[0],
		        sn65dsi83_expected_id[1],
		        sn65dsi83_expected_id[2],
		        sn65dsi83_expected_id[3],
		        sn65dsi83_expected_id[4],
		        sn65dsi83_expected_id[5],
		        sn65dsi83_expected_id[6],
		        sn65dsi83_expected_id[7],
		        sn65dsi83_expected_id[8]);
		return -ENODEV;
	}

	return 0;
}

int sn65dsi83_csr_write(const struct i2c_dt_spec *i2c, const struct sn65dsi83_csr *csr, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int ret = i2c_reg_write_byte_dt(i2c, csr[i].reg, csr[i].val);

		if (ret != 0) {
			LOG_ERR("CSR 0x%02x write failed (%d)", csr[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

int sn65dsi83_pll_start(const struct i2c_dt_spec *i2c)
{
	int ret;

	/* Datasheet init seq 6: set PLL_EN.  The input clock (the DSI clock lane,
	 * already HS since step 3 of sn65dsi83_init()) must already be stable. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_PLL_EN, SN65_PLL_EN_BIT);
	if (ret != 0) {
		LOG_ERR("PLL_EN write failed (%d)", ret);
		return ret;
	}

	/*
	 * CSR 0x0A.7 is named PLL_EN_STAT, not a "PLL locked" bit -- poll it
	 * instead of a fixed sleep so a PLL that never comes up at all is
	 * reported as such, distinct from a PLL that is merely still settling.
	 * Table 7-2's own init sequence (step 6) asks for a flat 10 ms wait
	 * after PLL_EN regardless of PLL_EN_STAT; give that margin here too
	 * rather than the smaller 3 ms the CSR 0x0A bit-field note alone would
	 * suggest.
	 */
	for (int i = 0; i < 20; i++) {
		uint8_t clk_src;

		ret = i2c_reg_read_byte_dt(i2c, SN65_REG_CLK_SRC, &clk_src);
		if (ret != 0) {
			LOG_ERR("PLL_EN_STAT poll read failed (%d)", ret);
			return ret;
		}
		if (clk_src & SN65_CLK_SRC_PLL_EN_STAT) {
			break;
		}
		k_msleep(1);
		if (i == 19) {
			LOG_ERR("PLL_EN_STAT did not assert within 20 ms (CSR 0x0A=0x%02x)", clk_src);
			return -ETIMEDOUT;
		}
	}
	/* Table 7-2 init sequence's own margin after PLL_EN (see the comment above). */
	k_msleep(10);

	/* Datasheet init seq 7: SOFT_RESET, then wait 10 ms. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_SOFT_RESET, SN65_SOFT_RESET_BIT);
	if (ret != 0) {
		LOG_ERR("SOFT_RESET write failed (%d)", ret);
		return ret;
	}
	k_msleep(10);

	/* Datasheet init seq 10: no IRQ pin wired on this adapter, so IRQ_EN stays
	 * off; clear whatever the reset/PLL-lock sequence latched into 0xE5. */
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_IRQ_EN, 0x00U);
	if (ret != 0) {
		LOG_ERR("IRQ_EN write failed (%d)", ret);
		return ret;
	}
	ret = i2c_reg_write_byte_dt(i2c, SN65_REG_ERR_STAT, 0xFFU);
	if (ret != 0) {
		LOG_ERR("Error-register clear failed (%d)", ret);
		return ret;
	}

	return 0;
}

int sn65dsi83_health_poll(const struct i2c_dt_spec   *i2c,
                          const struct sn65dsi83_csr *csr,
                          size_t                      n,
                          struct sn65dsi83_stats     *st,
                          struct sn65dsi83_report    *rep)
{
	int ret;

	memset(rep, 0, sizeof(*rep));

	ret = i2c_reg_read_byte_dt(i2c, SN65_REG_PLL_EN, &rep->pll_en);
	if (ret == 0) {
		ret = i2c_reg_read_byte_dt(i2c, SN65_REG_CLK_SRC, &rep->clk_src);
	}
	if (ret == 0) {
		ret = i2c_reg_read_byte_dt(i2c, SN65_REG_ERR_STAT, &rep->err_stat);
	}
	if (ret != 0) {
		return ret;
	}

	rep->health = sn65dsi83_health_decide(rep->pll_en, rep->clk_src, rep->err_stat);

	if (rep->health == SN65_HEALTH_CLEAR_ERRORS) {
		/* Latched flags are write-1-to-clear: write back what was read. */
		rep->err = i2c_reg_write_byte_dt(i2c, SN65_REG_ERR_STAT, rep->err_stat);
		if (rep->err == 0) {
			st->errors_cleared++;
		}
	} else if (rep->health == SN65_HEALTH_REINIT) {
		int64_t now = k_uptime_get();

		if (!sn65dsi83_reinit_allowed(st->have_last, st->last_reinit_ms, now)) {
			rep->suppressed = true;
			return 0;
		}
		st->have_last      = true;
		st->last_reinit_ms = now;

		/* The init again, from the table the boot-time init wrote.  The ID check
		 * first: never write a CSR bank into something that is not the bridge. */
		rep->err = sn65dsi83_check_id(i2c);
		if (rep->err == 0) {
			rep->err = sn65dsi83_csr_write(i2c, csr, n);
		}
		if (rep->err == 0) {
			rep->err = sn65dsi83_pll_start(i2c);
		}
		if (rep->err == 0) {
			st->recoveries++;
		} else {
			st->failures++;
		}
	}

	return 0;
}
