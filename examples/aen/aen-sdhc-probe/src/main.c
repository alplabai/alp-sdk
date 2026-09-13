/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sdhc-probe -- register-level bring-up probes for the Ensemble E8
 * SD Host Controller (`snps,dwc-sdhc`) on the E1M-AEN801 (M55-HE).
 *
 * SD IS DISABLED ENTIRELY ON THE E1M-EVK 2626-R2 (#2051). The maintainer
 * decided to keep `sdhc0` (`status = "disabled"`, the SoC dtsi's own
 * default) rather than merely leave the SDIO mux ENABLE undriven, because
 * the 74LVC157 mux (U38/U39) has NO high-impedance state: with its `/E`
 * ENABLE input HIGH, its Y outputs are forced LOW, not released. U38/U39's
 * outputs are the SoC-facing nets (E1M_CLK, E1M_CMD, E1M_D3..D0,
 * E1M_SDIO_RST), so those nets are actively held low by the mux whenever
 * it is powered, REGARDLESS of ENABLE. Enabling `sdhc0` would apply SD
 * pinctrl and let `sdhc_dwc_init()` start the 400 kHz identification
 * clock on P14_1 at `POST_KERNEL`, before this app's own `main()` even
 * runs -- putting the SoC's own 8 mA CLK/CMD drivers in a fight with the
 * mux's held-low outputs on every SD-routed pad. This is a hardware
 * defect pending a component change, not a firmware workaround -- see
 * README.md and docs/boards/e1m-evk.md.
 *
 * WHAT THIS APP DOES NOW: with `sdhc0` disabled, `#if
 * DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay)` below compiles out to
 * nothing but a one-line SKIPPED message and an early exit -- no SD pin is
 * opened, configured, or driven; no CC3501E bridge is brought up (nothing
 * downstream needs it any more, since the SD mux is never touched); no
 * `disk_access_init()` is attempted. The full register-probe logic stays
 * in the source, guarded, so a future board whose overlay re-enables
 * `sdhc0` (a working mux, or a carrier with none at all) gets the same
 * controller-level bring-up test this app has always been for, without a
 * second app to maintain.
 *
 * THE PROBES (only compiled/run when `sdhc0` is enabled):
 *   PROBE 1 -- a bare CMD0 (GO_IDLE_STATE) with NO reset call in front of
 *     it: does the SD controller's card clock exist at all, independent of
 *     `SW_RST`? Touches no clock register.
 *   PROBE 2 -- a READ-ONLY check that the SD peripheral clock gate
 *     (`CLKCTL_PER_MST` bit 16, `SDC_CKEN`) is set, then a real
 *     `sdhc_hw_reset()` + CMD0 exercise.
 *
 * SD PERIPHERAL CLOCK GATE, RESOLVED (#2035, #2051): `CLKCTL_PER_MST` bit
 * 16 is `SDC_CKEN`, a plain peripheral clock ENABLE (Alif's own DFP
 * header, `sys_ctrl_sd.h:30`, "Enable clock supply for SDMMC", set with
 * `|=` at `:40`) -- NOT a source-select/mux, which an earlier revision of
 * this comment (following a vendor Linux `clk-ensemble.c` reading) used to
 * claim. Bench evidence settled it: with the bit clear, EVERY SDHC
 * register -- including read-only `CAPABILITIES1` -- read `0x00000000`,
 * which a source-select could never produce. `sdhc_dwc_init()` now sets
 * this bit via `clocks = <&clockctrl ALIF_SDC_CLK>` (declared once, in the
 * shared SoC dtsi -- zephyr/dts/alif/ensemble_e8_peripherals.dtsi) --
 * PROBE 2 above is the read-only confirmation.
 *
 * WHAT SDC_CKEN DOES NOT EXPLAIN: this all-zero-register reading is a
 * DIFFERENT symptom from the original `SW_RST_CMD`=1-stuck-on-a-CLOCKED-
 * block reading this app's own diagnostics recorded before SDC_CKEN was
 * identified -- `CAPABILITIES1[5:0]=0x0a`, `SW_RST_R=0x02` (not `0x00`),
 * `NORMAL_INT_STAT_EN=0x7eff`/`ERROR_INT_STAT_EN=0xffff` are all values an
 * entirely unclocked block cannot give back. That reading, on a block that
 * WAS clocked, stays open; SDC_CKEN is a real, separate, now-fixed bug
 * found while investigating it, not its resolution. NONE of this has been
 * run on real hardware as of this head -- see the BENCH-UNVERIFIED note in
 * alif-ensemble-clocks-ext.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay)

#include <zephyr/drivers/sdhc.h> /* sdhc_request(), sdhc_hw_reset() -- the register-level probes
                                   * below */
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h> /* sys_read32() -- raw diagnostic register reads, see below */

/* sdhc0 -- the node label the board overlay gives the DWC SDHC controller. */
#define SDHC_DEV DEVICE_DT_GET(DT_NODELABEL(sdhc0))

#define SD_REG_BASE   0x48102000u
#define SD_REG_PSTATE (SD_REG_BASE + 0x024u) /* Present State */
#define SD_REG_NORMAL_INT_STAT \
	(SD_REG_BASE + 0x030u) /* Normal + Error Interrupt Status (32b read) */
#define SD_REG_NORMAL_INT_STAT_EN \
	(SD_REG_BASE + 0x034u) /* Normal + Error Int Status Enable (32b read) -- #2035, see below */
#define SD_REG_NORMAL_INT_SIGNAL_EN \
	(SD_REG_BASE + 0x038u) /* Normal + Error Int Signal Enable (32b read) */
#define SD_REG_CAPABILITIES1 (SD_REG_BASE + 0x040u) /* Capabilities 1 -- base clock field, Task 4 */

#define SD_PSTATE_CMD_INHIBIT_Msk 0x00000001u /* bit0: a command is in flight */
#define SD_PSTATE_DAT_INHIBIT_Msk 0x00000002u /* bit1 */

/* CLK_CTRL_R (uint16_t @0x02C) and SW_RST_R (uint8_t @0x02F) sit in the same
 * 32-bit-aligned word (sdhc_dwc.h:37-39: ..._CLK_CTRL_R @0x02C,
 * ..._TOUT_CTRL_R @0x02E, ..._SW_RST_R @0x02F), so one sys_read32() at the
 * word's base covers both -- little-endian, so SW_RST_R lands in byte 3
 * (bits[31:24]) and CLK_CTRL_R in bytes 0-1 (bits[15:0]). */
#define SD_REG_CLK_SWRST_WORD (SD_REG_BASE + 0x02Cu)
#define SD_CLK_CTRL_Msk       0x0000FFFFu
#define SD_SW_RST_R_Pos       24u
#define SD_SW_RST_ALL_Msk     0x01u /* bit0 of the SW_RST_R byte: register-file reset */
#define SD_SW_RST_CMD_Msk     0x02u /* bit1: command-circuit reset */
#define SD_SW_RST_DAT_Msk     0x04u /* bit2: data-circuit reset */

/* CLK_CTRL_R bit0 -- believed to ARM a reset handshake into the CMD/DAT
 * circuits that runs on the card clock rather than the host clock. */
#define SD_CLK_CTRL_INTERNAL_CLK_EN_Msk 0x0001u

/* Alif E8 pinmux registers for the SD "B" route's CLK/CMD pads (P14_1/P14_0).
 * Pad config lives at bits[23:16]; bit 16 of that byte is PADCTRL_READ_ENABLE.
 * Printing it here proves the setting reached silicon, not just the
 * devicetree -- inert while sdhc0 (and its pinctrl) is disabled. */
#define SD_PINMUX_P14_1_CLK       0x1A6031C4u
#define SD_PINMUX_P14_0_CMD       0x1A6031C0u
#define SD_PINMUX_PADCFG_Pos      16u
#define SD_PINMUX_PADCFG_Msk      (0xFFu << SD_PINMUX_PADCFG_Pos)
#define SD_PINMUX_READ_ENABLE_Msk (1u << SD_PINMUX_PADCFG_Pos)

/* CLKCTL_PER_MST -- bit 16 gates the SD peripheral clock at the SoC clock-tree
 * level, upstream of anything the SDHC's own Clock Control Register does. */
#define SD_CLKCTL_PER_MST           0x4903F00Cu
#define SD_CLKCTL_PER_MST_SD_EN_Msk (1u << 16)

/* Print the four "before" registers: does the pad setting, the clock gate and
 * the capability field this app depends on actually look right on THIS
 * silicon. */
static void sd_diag_print_static_regs(void)
{
	uint32_t p14_1  = sys_read32(SD_PINMUX_P14_1_CLK);
	uint32_t p14_0  = sys_read32(SD_PINMUX_P14_0_CMD);
	uint32_t clkctl = sys_read32(SD_CLKCTL_PER_MST);
	uint32_t caps   = sys_read32(SD_REG_CAPABILITIES1);

	printf("[sd][diag] P14_1 pinmux (CLK) @0x%08x = 0x%08x  padcfg=0x%02x read-enable=%u\n",
	       (unsigned)SD_PINMUX_P14_1_CLK,
	       p14_1,
	       (unsigned)((p14_1 & SD_PINMUX_PADCFG_Msk) >> SD_PINMUX_PADCFG_Pos),
	       (unsigned)((p14_1 & SD_PINMUX_READ_ENABLE_Msk) != 0u));
	printf("[sd][diag] P14_0 pinmux (CMD) @0x%08x = 0x%08x  padcfg=0x%02x read-enable=%u\n",
	       (unsigned)SD_PINMUX_P14_0_CMD,
	       p14_0,
	       (unsigned)((p14_0 & SD_PINMUX_PADCFG_Msk) >> SD_PINMUX_PADCFG_Pos),
	       (unsigned)((p14_0 & SD_PINMUX_READ_ENABLE_Msk) != 0u));
	printf("[sd][diag] CLKCTL_PER_MST     @0x%08x = 0x%08x  SD_CLK_EN(bit16)=%u\n",
	       (unsigned)SD_CLKCTL_PER_MST,
	       clkctl,
	       (unsigned)((clkctl & SD_CLKCTL_PER_MST_SD_EN_Msk) != 0u));
	printf("[sd][diag] CAPABILITIES1      @0x%08x = 0x%08x  (base-clock field: see Task 4's fix in "
	       "zephyr/drivers/sdhc/sdhc_dwc.c)\n",
	       (unsigned)SD_REG_CAPABILITIES1,
	       caps);
}

/* Read SW_RST_R and CLK_CTRL_R together (one aligned 32-bit read, see
 * SD_REG_CLK_SWRST_WORD above) and decode SW_RST_R's three reset bits plus
 * CLK_CTRL_R's INTERNAL_CLK_EN individually. `label` tags each line so a
 * bench log can tell them apart. */
static void sd_diag_print_clk_swrst(const char *label)
{
	uint32_t word     = sys_read32(SD_REG_CLK_SWRST_WORD);
	uint32_t clk_ctrl = word & SD_CLK_CTRL_Msk;
	uint32_t sw_rst   = (word >> SD_SW_RST_R_Pos) & 0xFFu;

	printf("[sd][diag] SW_RST_R(%s) @0x%08x = 0x%02x  ALL(bit0)=%u CMD(bit1)=%u DAT(bit2)=%u -- "
	       "CLK_CTRL_R @0x%08x = 0x%04x INTERNAL_CLK_EN(bit0)=%u\n",
	       label,
	       (unsigned)(SD_REG_BASE + 0x02Fu),
	       sw_rst,
	       (unsigned)((sw_rst & SD_SW_RST_ALL_Msk) != 0u),
	       (unsigned)((sw_rst & SD_SW_RST_CMD_Msk) != 0u),
	       (unsigned)((sw_rst & SD_SW_RST_DAT_Msk) != 0u),
	       (unsigned)(SD_REG_BASE + 0x02Cu),
	       clk_ctrl,
	       (unsigned)((clk_ctrl & SD_CLK_CTRL_INTERNAL_CLK_EN_Msk) != 0u));
}

/* Read NORMAL_INT_STAT_EN / ERROR_INT_STAT_EN, plus the two SIGNAL_EN
 * registers next to them -- READ ONLY, at several points below. `label`
 * tags each line so a bench log can tell them apart. */
static void sd_diag_print_int_enables(const char *label)
{
	uint32_t stat_en   = sys_read32(SD_REG_NORMAL_INT_STAT_EN);
	uint32_t signal_en = sys_read32(SD_REG_NORMAL_INT_SIGNAL_EN);

	printf("[sd][diag] INT enables (%s): NORMAL_INT_STAT_EN@0x%08x=0x%04x "
	       "ERROR_INT_STAT_EN@0x%08x=0x%04x NORMAL_INT_SIGNAL_EN@0x%08x=0x%04x "
	       "ERROR_INT_SIGNAL_EN@0x%08x=0x%04x -- both STAT_EN zero means command-complete "
	       "cannot latch, full stop\n",
	       label,
	       (unsigned)SD_REG_NORMAL_INT_STAT_EN,
	       (unsigned)(stat_en & 0xFFFFu),
	       (unsigned)(SD_REG_NORMAL_INT_STAT_EN + 2u),
	       (unsigned)(stat_en >> 16),
	       (unsigned)SD_REG_NORMAL_INT_SIGNAL_EN,
	       (unsigned)(signal_en & 0xFFFFu),
	       (unsigned)(SD_REG_NORMAL_INT_SIGNAL_EN + 2u),
	       (unsigned)(signal_en >> 16));
}

#define SD_DIAG_MAX_CMD_SAMPLES   4u  /* covers CMD0, CMD8, ACMD41 (or its first retry), CMD2/3 */
#define SD_DIAG_POST_CMD_DELAY_MS 10u /* well inside the driver's >=1000 ms own timeout */
#define SD_DIAG_POLL_INTERVAL_MS  1u  /* k_msleep, not k_busy_wait -- see thread note below */
#define SD_DIAG_RISE_TIMEOUT_MS \
	3000u /* give up watching for a further command after this long idle */

/*
 * Background watcher, NOT the driver's own thread. Runs at a LOWER priority
 * than main() (a higher numeric value) so main always preempts it -- it only
 * gets the CPU while main is blocked inside the driver's k_event_wait(),
 * which is exactly when there is something worth polling for. k_msleep(),
 * never k_busy_wait(), for the same reason: a non-yielding poll loop at a
 * priority that could starve main would stop the very command it is trying
 * to observe from ever completing.
 */
static void sd_diag_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (unsigned sample = 0; sample < SD_DIAG_MAX_CMD_SAMPLES; sample++) {
		uint32_t waited_ms = 0u;
		bool     saw_idle  = false;

		/* Wait for CMD_INHIBIT's 0->1 edge: idle observed first, THEN
		 * inhibited, so a sample thread starting mid-command never
		 * mistakes an already-in-flight command for a fresh one. */
		while (waited_ms < SD_DIAG_RISE_TIMEOUT_MS) {
			uint32_t pstate    = sys_read32(SD_REG_PSTATE);
			bool     inhibited = (pstate & SD_PSTATE_CMD_INHIBIT_Msk) != 0u;

			if (!saw_idle) {
				saw_idle = !inhibited;
			} else if (inhibited) {
				break;
			}
			k_msleep(SD_DIAG_POLL_INTERVAL_MS);
			waited_ms += SD_DIAG_POLL_INTERVAL_MS;
		}
		if (waited_ms >= SD_DIAG_RISE_TIMEOUT_MS) {
			printf("[sd][diag] cmd#%u: no further command register write observed within "
			       "%u ms -- stopping\n",
			       sample,
			       (unsigned)SD_DIAG_RISE_TIMEOUT_MS);
			return;
		}

		k_msleep(SD_DIAG_POST_CMD_DELAY_MS);

		uint32_t int_stat_raw = sys_read32(SD_REG_NORMAL_INT_STAT);
		uint32_t pstate       = sys_read32(SD_REG_PSTATE);
		printf("[sd][diag] cmd#%u +%u ms (BEFORE any SW_RST write): NORMAL_INT_STAT=0x%04x "
		       "ERROR_INT_STAT=0x%04x PSTATE=0x%08x CMD_INHIBIT=%u DAT_INHIBIT=%u\n",
		       sample,
		       (unsigned)SD_DIAG_POST_CMD_DELAY_MS,
		       (unsigned)(int_stat_raw & 0xFFFFu),
		       (unsigned)(int_stat_raw >> 16),
		       pstate,
		       (unsigned)((pstate & SD_PSTATE_CMD_INHIBIT_Msk) != 0u),
		       (unsigned)((pstate & SD_PSTATE_DAT_INHIBIT_Msk) != 0u));
	}
}

K_THREAD_DEFINE(sd_diag_tid, 1024, sd_diag_thread_fn, NULL, NULL, NULL, 7, 0, 0);

/* CMD0 timeout for the probes below -- short on purpose: this is a probe of
 * whether the command clocks out AT ALL, not a real enumeration attempt, so
 * there is no reason to wait anywhere near the driver's own 1000 ms default
 * (sdhc_dwc_wait_cmd_complete()). */
#define SD_DIAG_CMD0_TIMEOUT_MS 100

/* Issue CMD0 (GO_IDLE_STATE) directly through the public sdhc_request() API,
 * with NO reset call in front of it -- the entire point of PROBE 1 below.
 * SD_RSP_TYPE_NONE / no data phase: CMD0 carries no response and moves no
 * bytes, so this is a bare command-clock test, nothing else. */
static int sd_diag_send_cmd0(void)
{
	struct sdhc_command cmd = {
		.opcode        = SD_GO_IDLE_STATE,
		.arg           = 0u,
		.response_type = SD_RSP_TYPE_NONE,
		.retries       = 0u,
		.timeout_ms    = SD_DIAG_CMD0_TIMEOUT_MS,
	};

	return sdhc_request(SDHC_DEV, &cmd, NULL);
}

int main(void)
{
	sd_diag_print_int_enables("main() entry");
	sd_diag_print_int_enables("before PROBE 1 (CMD0, no reset)");

	/*
	 * --- PROBE 1: CMD0 with NO reset call in front of it ---------------
	 * Does the card clock exist at all? A completing CMD0 disproves a
	 * dead-clock theory outright; a timeout with SW_RST_R settling back
	 * to 0x02 corroborates it. Touches no clock register.
	 */
	sd_diag_print_clk_swrst("PROBE 1, before CMD0");

	int cmd0a_rc = sd_diag_send_cmd0();
	printf("[sd] PROBE 1: CMD0 (GO_IDLE_STATE) with no reset in front of it -> %d "
	       "(0 = clocked out and completed; -116 = timed out -- see SW_RST_R/CLK_CTRL_R "
	       "below and PSTATE/NORMAL_INT_STAT for what the controller saw)\n",
	       cmd0a_rc);

	uint32_t probe1_int_stat = sys_read32(SD_REG_NORMAL_INT_STAT);
	uint32_t probe1_pstate   = sys_read32(SD_REG_PSTATE);
	printf("[sd] PROBE 1: NORMAL_INT_STAT @0x%08x = 0x%04x  PSTATE @0x%08x = 0x%08x "
	       "CMD_INHIBIT(bit0)=%u\n",
	       (unsigned)SD_REG_NORMAL_INT_STAT,
	       (unsigned)(probe1_int_stat & 0xFFFFu),
	       (unsigned)SD_REG_PSTATE,
	       probe1_pstate,
	       (unsigned)((probe1_pstate & SD_PSTATE_CMD_INHIBIT_Msk) != 0u));
	sd_diag_print_clk_swrst("PROBE 1, after CMD0");
	sd_diag_print_int_enables("after PROBE 1 (CMD0, no reset)");

	/*
	 * --- PROBE 2: confirm the SD peripheral clock gate, READ-ONLY ------
	 * An earlier revision of this probe WROTE CLKCTL_PER_MST bit 16 to
	 * test a (now-settled) mux-vs-enable theory -- see the file header.
	 * sdhc_dwc_init() now sets this bit itself before main() ever runs,
	 * so this only reads the gate back rather than writing it.
	 */
	uint32_t clkctl_now = sys_read32(SD_CLKCTL_PER_MST);
	printf("[sd] PROBE 2: CLKCTL_PER_MST @0x%08x = 0x%08x -- bit 16 (SDC_CKEN) %s\n",
	       (unsigned)SD_CLKCTL_PER_MST,
	       clkctl_now,
	       (clkctl_now & SD_CLKCTL_PER_MST_SD_EN_Msk)
	           ? "SET (clocked)"
	           : "CLEAR (unclocked -- the driver's clock enable did not take)");

	int hwreset2_rc = sdhc_hw_reset(SDHC_DEV);
	sd_diag_print_clk_swrst("PROBE 2, after sdhc_hw_reset()");
	printf("[sd] PROBE 2: sdhc_hw_reset(sdhc0) -> %d\n", hwreset2_rc);

	int cmd0b_rc = sd_diag_send_cmd0();
	sd_diag_print_clk_swrst("PROBE 2, after CMD0");
	printf("[sd] PROBE 2: CMD0 (GO_IDLE_STATE) -> %d\n", cmd0b_rc);

	sd_diag_print_static_regs();
	sd_diag_print_int_enables("before exit");

	printf("[sd] RESULT: controller-level probes complete (CMD0 no-reset -> %d, "
	       "CMD0 post-reset -> %d, sdhc_hw_reset -> %d). This board does not enable the "
	       "card path -- see the file header\n",
	       cmd0a_rc,
	       cmd0b_rc,
	       hwreset2_rc);
	printf("[sd] done\n");
	return 0;
}

#else /* !DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay) */

int main(void)
{
	printf("[sd] SD host controller (sdhc0) is DISABLED on this board -- the E1M-EVK "
	       "2626-R2 SDIO mux (74LVC157 U38/U39) has no high-impedance state, so its "
	       "SoC-facing outputs are actively held low whenever the mux is powered, "
	       "regardless of its ENABLE input. Enabling the controller would start SD "
	       "pinctrl and the identification clock and fight those held-low pads. This is "
	       "a hardware defect pending a component change, not a firmware gap -- see "
	       "README.md. Nothing in this build opens, configures, or drives an SD pin.\n");
	printf("[sd] RESULT SKIPPED: SD host controller disabled on this board (hardware "
	       "defect, rework pending)\n");
	return 0;
}

#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay) */
