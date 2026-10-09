/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADR-0017-ADJACENT (see display_sn65dsi83.c).  Private to the SN65DSI83 driver
 * and its recovery agent: the register facts both share, the pure health
 * decision, and the replay entry points.
 *
 * WHY THIS EXISTS: the bridge can reset itself to its power-on defaults (an ESD
 * or supply glitch) while EN stays high and the DSI clock keeps running.  Bench
 * (E1M-AEN803, RVT121): CSR 0x0D 0x00 (PLL_EN), 0x0A 0x0A (PLL_EN_STAT and
 * HS_CLK_SRC clear), 0xE5 0x3D -- the panel black, the SoC side healthy.  Init
 * runs once at boot, so nothing set the bridge up again.
 *
 * WHO RUNS THE CHECK: whichever core owns the I2C controller at run time, and
 * never two.  Alone on the bus (the aen-lvds-display example) the driver's own
 * delayable work item does.  Where the display core hands the controller to
 * another core after init (the Trace Runner: the HE configures the bridge, then
 * alp,i2c-handover gives I2C1 to the HP for the camera), the driver publishes
 * its CSR table in a shared-SRAM recipe (recovery-recipe-address) and the core
 * that owns the bus replays it (sn65dsi83_recovery_agent.c).  The display core
 * must not touch the bus after the handover.
 */
#ifndef ZEPHYR_DRIVERS_SN65DSI83_RECOVERY_H_
#define ZEPHYR_DRIVERS_SN65DSI83_RECOVERY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/toolchain.h>
#include <zephyr/sys/util.h>

/* CSR addresses shared by the driver and the replay (datasheet Tables 7-4..7-9). */
#define SN65_REG_ID_BASE    0x00U /* 9-byte burst: CSR 0x00..0x08. */
#define SN65_REG_ID_LEN     9U
#define SN65_REG_SOFT_RESET 0x09U
#define SN65_REG_CLK_SRC    0x0AU /* LVDS_CLK_RANGE[3:1], HS_CLK_SRC[0]; PLL_EN_STAT[7] (R/O). */
#define SN65_REG_PLL_EN     0x0DU
#define SN65_REG_IRQ_EN     0xE0U
#define SN65_REG_ERR_STAT   0xE5U

/* CSR 0x0A.7: PLL_EN_STAT (not itself named "PLL locked" -- Table 7-2's own
 * init sequence wants >= 10 ms after PLL_EN regardless, see sn65dsi83_pll_start()). */
#define SN65_CLK_SRC_PLL_EN_STAT BIT(7)
/* CSR 0x0A.0: HS_CLK_SRC -- the driver always writes 1; a power-on reset reads 0. */
#define SN65_CLK_SRC_HS_CLK_SRC BIT(0)

/* CSR 0x0D / 0x09. */
#define SN65_PLL_EN_BIT     BIT(0)
#define SN65_SOFT_RESET_BIT BIT(0)

/* One CSR write of the init table. */
struct sn65dsi83_csr {
	uint8_t reg;
	uint8_t val;
};

enum sn65dsi83_health {
	SN65_HEALTH_OK,           /* PLL enabled, locked, config kept, no latched error. */
	SN65_HEALTH_CLEAR_ERRORS, /* PLL fine, CSR 0xE5 latched something: clear and count only. */
	SN65_HEALTH_REINIT,       /* PLL off / not locked / config lost / errors persist: replay. */
};

/* Latched errors on this many consecutive polls (PLL fine) are not transient: re-init. */
#define SN65_ERR_POLLS_REINIT 3U

/*
 * The whole recovery decision, from the three registers read: CSR 0x0D, 0x0A, 0xE5.
 *
 * hs_clk_src: the HS_CLK_SRC bit (0x0A bit 0) the driver's own table writes -- a bridge
 * that reads back anything else lost the table.  err_polls: consecutive polls (this one
 * included) with CSR 0xE5 non-zero.  after_recovery: this is the first poll after a
 * re-init.  A single transient sync error with the PLL fine is cleared and counted; the
 * same errors on SN65_ERR_POLLS_REINIT polls in a row, or on the first poll after a
 * re-init, mean the link does not hold and the sequence is replayed.
 */
static inline enum sn65dsi83_health sn65dsi83_health_decide(uint8_t  pll_en,
                                                            uint8_t  clk_src,
                                                            uint8_t  err_stat,
                                                            uint8_t  hs_clk_src,
                                                            uint32_t err_polls,
                                                            bool     after_recovery)
{
	if (!(pll_en & SN65_PLL_EN_BIT) || !(clk_src & SN65_CLK_SRC_PLL_EN_STAT) ||
	    (clk_src & SN65_CLK_SRC_HS_CLK_SRC) != (hs_clk_src & SN65_CLK_SRC_HS_CLK_SRC)) {
		return SN65_HEALTH_REINIT;
	}
	if (err_stat == 0U) {
		return SN65_HEALTH_OK;
	}
	return (after_recovery || err_polls >= SN65_ERR_POLLS_REINIT) ? SN65_HEALTH_REINIT
	                                                              : SN65_HEALTH_CLEAR_ERRORS;
}

/* A bridge that will not stay up is re-initialised at most this often. */
#define SN65_REINIT_MIN_GAP_MS 5000

static inline bool sn65dsi83_reinit_allowed(bool have_last, int64_t last_ms, int64_t now_ms)
{
	return !have_last || (now_ms - last_ms) >= SN65_REINIT_MIN_GAP_MS;
}

/*
 * What the display core's boot init may touch.  A cold boot runs the whole datasheet sequence.  A
 * WARM boot of the display core alone (the bus owner keeps running, alp,i2c-handover with an
 * alive-address) must leave the shared I2C controller and the bridge's EN pin alone: EN low would
 * reset a bridge the bus owner is watching, and any bus access would collide with the owner's
 * transfers.  It still re-initialises the DSI host and republishes the recipe (compile-time
 * constant, so the owner's latch stays valid); the owner replays the CSRs if the host's clock-lane
 * restart cost the bridge its PLL lock.  Only a core that hands its bus away (has a recipe) can
 * boot warm.
 */
struct sn65dsi83_boot_plan {
	bool toggle_en;      /* EN low for >= 10 ms, then high (datasheet init seq 3-4) */
	bool touch_bus;      /* ID check, CSR writes, PLL start over I2C */
	bool clear_recipe;   /* zero the recipe magic and the owner's counters before re-publishing */
	bool publish_recipe; /* (re)publish the CSR table for the bus owner */
};

static inline struct sn65dsi83_boot_plan sn65dsi83_boot_plan_for(bool warm, bool have_recipe)
{
	bool w = warm && have_recipe;

	return (struct sn65dsi83_boot_plan){
		.toggle_en      = !w,
		.touch_bus      = !w,
		.clear_recipe   = !w && have_recipe,
		.publish_recipe = have_recipe,
	};
}

/* The bus owner's I2C access failed (not a bridge verdict) on this many polls in a row: worth one
 * report.  Nothing else is done about it: the next interval retries, and the I2C driver already
 * aborts a timed-out transfer by itself. */
#define SN65_IO_FAIL_POLLS 3U

/* Count one poll's I2C result in *streak.  True exactly once per episode: when the streak reaches
 * SN65_IO_FAIL_POLLS.  A successful poll ends the episode (the next one reports again). */
static inline bool sn65dsi83_io_fail_step(uint32_t *streak, int io_result)
{
	if (io_result == 0) {
		*streak = 0U;
		return false;
	}
	if (*streak < SN65_IO_FAIL_POLLS) {
		(*streak)++;
		return *streak == SN65_IO_FAIL_POLLS;
	}
	return false;
}

struct sn65dsi83_stats {
	uint32_t recoveries;     /* successful re-inits */
	uint32_t failures;       /* re-inits that failed (ID mismatch, I2C error, PLL never locked) */
	uint32_t errors_cleared; /* CSR 0xE5 cleared with the PLL fine */
	uint32_t err_polls;      /* consecutive polls with CSR 0xE5 non-zero */
	int64_t  last_reinit_ms;
	bool     have_last;
	bool     after_recovery; /* the next poll is the first after a successful re-init */
};

struct sn65dsi83_report {
	enum sn65dsi83_health health;
	bool                  suppressed; /* wanted a re-init, rate limit said not yet */
	int                   err;        /* re-init result (0 when none was attempted) */
	uint8_t               pll_en;
	uint8_t               clk_src;
	uint8_t               err_stat;
};

/* Shared-SRAM recipe: the display core publishes its CSR table, the bus owner replays it. */
#define SN65_RECIPE_MAGIC 0x33385341u /* 'AS83' */
#define SN65_RECIPE_MAX   32U

struct sn65dsi83_recipe {
	volatile uint32_t magic; /* written LAST by the publisher; cleared first on every COLD boot */
	/* Bus owner's counters (struct sn65dsi83_stats), readable by the display core. */
	volatile uint32_t    recoveries;
	volatile uint32_t    failures;
	volatile uint32_t    errors_cleared;
	uint8_t              n;
	uint8_t              pad[3];
	struct sn65dsi83_csr csr[SN65_RECIPE_MAX];
};

/* The Trace Runner's tr_memmap.h places this struct in a fixed page: keep the size pinned. */
#define SN65_RECIPE_SIZE 0x54U
BUILD_ASSERT(sizeof(struct sn65dsi83_recipe) == SN65_RECIPE_SIZE,
             "struct sn65dsi83_recipe changed size: update TR_MEM_SN65_RECIPE's reserved size");

/*
 * The bus owner's own copy of the recipe.  The display core clears the recipe magic every time it
 * re-initialises (a warm reboot of that core), and its re-init can then lose a race for the bus
 * it has already handed away and never publish again: so the first valid recipe is LATCHED, a
 * later magic of 0 means "no newer recipe", and the latched table stays in use.
 */
struct sn65dsi83_latch {
	uint8_t              n; /* 0: nothing latched yet */
	struct sn65dsi83_csr csr[SN65_RECIPE_MAX];
};

/* Copy a valid recipe into *l (re-reading the magic after the copy: a copy that overlapped a
 * rewrite is dropped).  Returns true if *l was refreshed; with no valid magic *l is untouched. */
bool sn65dsi83_recipe_latch(const struct sn65dsi83_recipe *r, struct sn65dsi83_latch *l);

/* Identity check (CSR 0x00..0x08): 0 on a match, -ENODEV on a mismatch, <0 on an I2C error. */
int sn65dsi83_check_id(const struct i2c_dt_spec *i2c);

/* Datasheet init seq 5: write the CSR table. */
int sn65dsi83_csr_write(const struct i2c_dt_spec *i2c, const struct sn65dsi83_csr *csr, size_t n);

/* Datasheet init seq 6-10: PLL_EN, wait for PLL_EN_STAT, SOFT_RESET, clear CSR 0xE5. */
int sn65dsi83_pll_start(const struct i2c_dt_spec *i2c);

/*
 * One health pass over the bus: read CSR 0x0D/0x0A/0xE5, decide, act.  The full init
 * (PLL off, ID check, sn65dsi83_csr_write(), sn65dsi83_pll_start()) is replayed from `csr`,
 * the same table the driver's init wrote; a failure part-way writes CSR 0x0D = 0 (best
 * effort) so the next pass sees a bridge that needs a replay.  `now_ms` is the caller's
 * uptime (injected so the rate limit is testable).  Returns the I2C error if the three
 * reads failed (bridge absent or powered down: nothing to repair from here), else 0 --
 * the outcome is in `rep` and `st`.  This file is silent; sn65dsi83_report_print() reports.
 */
int sn65dsi83_health_poll(const struct i2c_dt_spec   *i2c,
                          const struct sn65dsi83_csr *csr,
                          size_t                      n,
                          struct sn65dsi83_stats     *st,
                          struct sn65dsi83_report    *rep,
                          int64_t                     now_ms);

/* One printk line for a pass that did something (nothing while healthy). */
void sn65dsi83_report_print(const struct sn65dsi83_report *rep, const struct sn65dsi83_stats *st);

#endif /* ZEPHYR_DRIVERS_SN65DSI83_RECOVERY_H_ */
