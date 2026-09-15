/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sdhc-probe -- register-level bring-up probes for the Ensemble E8
 * SD Host Controller (`snps,dwc-sdhc`) on the E1M-AEN801 (M55-HE).
 *
 * ============================================================================
 * BENCH-TEST BRANCH (test/2051-sdhc-enable-on-reworked-mux) -- READ FIRST
 * ============================================================================
 * SD stays disabled entirely on a STOCK E1M-EVK 2626-R2 (#2051): the
 * 74LVC157 mux (U38/U39) has NO high-impedance state, so its SoC-facing Y
 * outputs are forced LOW -- never released -- whenever `/E` is HIGH,
 * REGARDLESS of ENABLE. That reasoning is UNCHANGED and still correct for
 * every un-reworked carrier, which is most of them.
 *
 * This build runs on bench board `e1m-aen-evk-03` ONLY, where the
 * maintainer has PHYSICALLY REPLACED U38, U39 and U46 with 74LV3257 bus
 * switches -- bidirectional, with a TRUE high-impedance state when `/E` is
 * deasserted. That rework removes the forced-low premise above, so this
 * branch's board overlay re-enables `sdhc0` and drives the mux ENABLE, to
 * actually exercise the microSD path and prove or disprove the rework on
 * silicon. This is a BENCH TEST for that one board, NOT a rescoping of
 * #2051 or #2122 -- do not read an enabled `sdhc0` here as "the mux defect
 * is fixed everywhere". See the board overlay's own header for the same
 * caveat in the devicetree.
 * ============================================================================
 *
 * WHAT THIS APP DOES: `#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay)`
 * below selects between two builds, decided entirely by the board overlay's
 * `sdhc0` status -- this source file does not otherwise know which board it
 * is on:
 *
 *   - `sdhc0` DISABLED (any stock/un-reworked board): compiles out to a
 *     CLOCK-GATE PROOF -- no SD pin is opened, configured, or driven; no
 *     CC3501E bridge is brought up; no `disk_access_init()` is attempted.
 *     `CLKCTL_PER_MST` (the peripheral clock gate) and `CAPABILITIES1` (the
 *     SDHC block's own read-only capability word) are both plain
 *     memory-mapped registers -- reading and gating them touches no pad and
 *     needs no pinctrl. The proof: read both registers, call
 *     `clock_control_on()` for the gate, read both again, and print whether
 *     the gate went clear->set and `CAPABILITIES1` went zero->non-zero --
 *     see `sd_diag_prove_clock_gate()` below. This proves the controller
 *     becomes addressable; it does NOT prove a card enumerates.
 *
 *   - `sdhc0` ENABLED (this branch's board overlay, the reworked bench
 *     board only): drives the CC3501E-proxied SDIO mux ENABLE first (see
 *     `sd_mux_enable()` below), then runs the full controller-level probe
 *     set -- CMD0, `sdhc_hw_reset()`, register reads -- AND a full
 *     `disk_access_init()` enumeration (PROBE 3): PROBE 1/2 alone cannot
 *     answer "does the card actually work through the reworked mux", only
 *     "is the controller alive". The full register-probe logic stays in
 *     the source either way, guarded, so a future board whose overlay
 *     re-enables `sdhc0` (a working mux, or a carrier with none at all)
 *     gets the same test this app has always been for, without a second
 *     app to maintain.
 *
 * THE PROBES (only compiled/run when `sdhc0` is enabled):
 *   PROBE 1 -- a bare CMD0 (GO_IDLE_STATE) with NO reset call in front of
 *     it: does the SD controller's card clock exist at all, independent of
 *     `SW_RST`? Touches no clock register.
 *   PROBE 2 -- a READ-ONLY check that the SD peripheral clock gate
 *     (`CLKCTL_PER_MST` bit 16, `SDC_CKEN`) is set, then a real
 *     `sdhc_hw_reset()` + CMD0 exercise.
 *   PROBE 3 -- `disk_access_init()` + geometry/CID ioctls + a one-block
 *     `disk_access_read()`, through the `sdmmc` child node the board
 *     overlay adds under `&sdhc0`. This is the only probe that actually
 *     moves card data; PROBE 1/2 stay at the bare register level on
 *     purpose (see `sd_probe3_full_enumeration()` below) so the three
 *     probes isolate clock/controller/card layers from each other in the
 *     final verdict line.
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
#include <zephyr/sys/sys_io.h> /* sys_read32() -- raw diagnostic register reads, both branches below */

/*
 * ALP-SDK DELTA (#2051), not upstream: registers used by BOTH branches
 * below -- the full sdhc0-enabled probe set (further down) and the
 * disabled-board clock-gate proof (the #else branch) both need
 * CAPABILITIES1 and CLKCTL_PER_MST, so these four are defined once, here,
 * rather than twice inside two mutually-exclusive #if/#else arms (which
 * would compile fine either way but could silently drift apart).
 * CLKCTL_PER_MST is a SoC-level system-control register, NOT part of the
 * SDHC register block itself (it sits at a different base address
 * entirely) -- gating its bit 16 clocks the DWC SDHC block internally and
 * routes to no pad; SD pinctrl is applied only by the SDHC driver's own
 * init, which is never built or run while sdhc0 is disabled.
 */
#define SD_REG_BASE 0x48102000u
#define SD_REG_CAPABILITIES1 \
	(SD_REG_BASE + 0x040u)            /* Capabilities 1 -- base clock field, read-only */
#define SD_CLKCTL_PER_MST 0x4903F00Cu /* CLKCTL_PER_MST.PERIPH_CLK_ENA -- routes to no pad */
#define SD_CLKCTL_PER_MST_SD_EN_Msk (1u << 16) /* bit 16, SDC_CKEN */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay)

#include <zephyr/drivers/sdhc.h> /* sdhc_request(), sdhc_hw_reset() -- the register-level probes
                                   * below */
#include <zephyr/kernel.h>

#include <alp/e1m_pinout.h> /* ALP_E1M_GPIO_IO20 -- the SDIO mux ENABLE */
#include <alp/peripheral.h> /* alp_gpio_open()/configure()/write()/read() */
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

#include <errno.h> /* -EIO -- the "geometry not usable, read skipped" default in PROBE 3 */
#include <zephyr/storage/disk_access.h> /* PROBE 3 -- disk_access_init()/ioctl()/read(), and the
                                          * DISK_IOCTL_* / DISK_STATUS_* codes (zephyr/drivers/disk.h)
                                          */

/* sdhc0 -- the node label the board overlay gives the DWC SDHC controller. */
#define SDHC_DEV DEVICE_DT_GET(DT_NODELABEL(sdhc0))

#define SD_REG_PSTATE (SD_REG_BASE + 0x024u) /* Present State */
#define SD_REG_NORMAL_INT_STAT \
	(SD_REG_BASE + 0x030u) /* Normal + Error Interrupt Status (32b read) */
#define SD_REG_NORMAL_INT_STAT_EN \
	(SD_REG_BASE + 0x034u) /* Normal + Error Int Status Enable (32b read) -- #2035, see below */
#define SD_REG_NORMAL_INT_SIGNAL_EN \
	(SD_REG_BASE + 0x038u) /* Normal + Error Int Signal Enable (32b read) */

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

/* SD_CLKCTL_PER_MST / SD_CLKCTL_PER_MST_SD_EN_Msk -- bit 16 gates the SD
 * peripheral clock at the SoC clock-tree level, upstream of anything the
 * SDHC's own Clock Control Register does; defined once, above the #if, so
 * the disabled-board clock-gate proof (the #else branch) shares it. */

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

/* CMD8 (SEND_IF_COND) timeout/retries for the card-presence probe below --
 * a few retries, unlike CMD0's zero above, because a card that just saw two
 * CMD0s (PROBE 1/2) can need a beat before it answers CMD8; still nowhere
 * near CONFIG_SD_RETRY_COUNT (subsys/sd's own, much longer, retry budget). */
#define SD_DIAG_CMD8_TIMEOUT_MS 100
#define SD_DIAG_CMD8_RETRIES    2u

/*
 * BENCH-TEST fix (this branch): does a card physically answer CMD8, checked
 * BEFORE disk_access_init() and independent of it? Bench evidence (see the
 * file header) is that -134/-ENOTSUP from disk_access_init() is ambiguous by
 * itself -- subsys/sd/sd.c:239 returns the identical code for "the bus is
 * dead, CMD8 never got a response" and "the card fully identified (ACMD41,
 * CMD2/3/9/7/55 all cc:1) then failed on the FIRST data-bearing command,
 * deep inside sdmmc_card_init()". This probe answers the CMD8 half on its
 * own, directly through sdhc_request() -- same pattern as sd_diag_send_cmd0()
 * above, same command subsys/sd/sd.c's sd_send_interface_condition() sends
 * (CMD0 then CMD8, SD_IF_COND_VHS_3V3 | SD_IF_COND_CHECK, R7) -- so the
 * verdict below can tell "no card answered" apart from "a card answered but
 * something past CMD8 (e.g. ADMA/DMA translation) failed".
 */
static bool sd_diag_probe_card_present(void)
{
	(void)sd_diag_send_cmd0();

	struct sdhc_command cmd = {
		.opcode        = SD_SEND_IF_COND,
		.arg           = SD_IF_COND_VHS_3V3 | SD_IF_COND_CHECK,
		.response_type = SD_RSP_TYPE_R7,
		.retries       = SD_DIAG_CMD8_RETRIES,
		.timeout_ms    = SD_DIAG_CMD8_TIMEOUT_MS,
	};

	int rc = sdhc_request(SDHC_DEV, &cmd, NULL);

	printf("[sd][probe3] card-presence CMD8 (SEND_IF_COND) -> %d%s\n",
	       rc,
	       (rc == 0) ? ((cmd.response[0] & 0xFFu) == SD_IF_COND_CHECK
	                        ? " (check pattern echoed -- a card is present and answering)"
	                        : " (no check-pattern echo -- treating as no card)")
	                 : " (no response -- no card, or the bus/mux is dead)");

	return (rc == 0) && ((cmd.response[0] & 0xFFu) == SD_IF_COND_CHECK);
}

/*
 * Settle time for the CC3501E driving its pad and the card seeing its
 * lines -- the mux itself is a 74LV3257/74LVC157, combinational, ns-scale.
 * Same value and reasoning as aen-evk-demo's SD_MUX_SETTLE_MS.
 */
#define SD_MUX_SETTLE_MS 10u

/*
 * BENCH-TEST (test/2051-sdhc-enable-on-reworked-mux): assert the SDIO mux
 * ENABLE (E1M IO20 -> CC3501E GPIO_26, active low) over the CC3501E GPIO
 * proxy, BEFORE any SD pin is touched below -- the mux sits between the
 * controller and the card, so without this the probes that follow would be
 * driving pads that never reach a card at all, reworked mux or not.
 *
 * Brings the bridge up first (cc3501e_bridge_bringup(), the same one-call
 * SoM template every other CC3501E example uses), then drives IO20 low
 * through the portable alp_gpio_* API -- the proxy backend (built via
 * CONFIG_ALP_SDK_GPIO_CC3501E_PROXY + this app's one-entry
 * src/cc3501e_gpio_routes.c) routes it over the bridge to raw CC3501E
 * GPIO_26. `alp_gpio_read()` afterwards is CORROBORATION ONLY, not proof:
 * depending on bridge firmware this may report the far-side output
 * register rather than the pad itself (examples/aen/aen-evk-demo/src/
 * main.c documents the same caveat for this exact pin). It does not gate
 * anything below -- a read disagreement is printed and the probes still
 * run, so the log shows what the bridge reports either way.
 *
 * `E1M_GPIO_IO21` (the mux SELECT) is deliberately NOT touched here --
 * `dispatch: unrouted` on 2626-R2, and `alp_gpio_open()` refuses it
 * unconditionally. The select is set by the P18 jumper (a hardware strap:
 * fitted pulls MUX_SEL.SDIO high through R198, open lets R27 pull it to
 * 0V -- LOW selects the microSD slot per the netlist), not firmware.
 *
 * Unlike aen-cc3501e-gpio's demo, this does NOT run a PING liveness poll
 * before driving the pin -- cc3501e_bridge_bringup()'s own cc3501e_reset()
 * already retries a stalled first boot internally (the Puya cold-boot
 * workaround). If the bridge is genuinely not answering, the configure/
 * write calls below simply return non-ALP_OK and this function reports it;
 * that is a coarser signal than aen-cc3501e-gpio's PING-gated retry loop,
 * not a stronger one -- see the report for this as a known bench risk.
 *
 * SD_RST (P14_2) IS DELIBERATELY NOT DRIVEN ANYWHERE IN THIS APP -- traced
 * against the 2626-R2 netlist (E1M-EVK-2626-R2_pinmap.csv) and confirmed by
 * the maintainer: with the mux SELECT strapped LOW (microSD selected, the
 * bench default), U39 channel 2 routes P14_2 to `M2E_SDIO_RSTn` -- the M.2
 * E-key connector's reset, NOT the microSD socket. J7 (the microSD slot)
 * exposes only DAT2/CD-DAT3/CMD/CLK/DAT0/DAT1 -- SD cards have no reset
 * pin, full stop. Driving P14_2 cannot reset the card and would assert
 * reset on an M.2 module if one were fitted. Do NOT re-add an SD_RST pulse
 * here on the "three vendor sources bit-bang it" reasoning that applied to
 * the OLD aen-sdcard-readout app -- that reasoning assumed the pin reaches
 * the card, and on THIS mux topology it does not.
 *
 * Returns true iff the mux ENABLE write itself reported ALP_OK (i.e. the
 * bridge accepted the write) -- callers use this only to shape the final
 * verdict text, not to gate whether PROBE 3 below runs.
 */
static bool sd_mux_enable(void)
{
	static cc3501e_t fw; /* static: ~32 KB, would blow PSPLIM as a main() stack local */

	alp_status_t s = cc3501e_bridge_bringup(&fw);
	printf("[sd][mux] cc3501e_bridge_bringup() -> %d%s\n",
	       (int)s,
	       (s == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) ? " (SPI1/WIFI_EN/nRESET absent -- check "
	                                                "the board overlay)"
	                                              : "");
	if (s != ALP_OK) {
		printf("[sd][mux] SDIO mux ENABLE NOT attempted -- bridge bring-up failed, so the "
		       "card is almost certainly electrically disconnected from the controller "
		       "below\n");
		return false;
	}

	alp_gpio_t *mux_en = alp_gpio_open(ALP_E1M_GPIO_IO20);
	if (mux_en == NULL) {
		printf("[sd][mux] alp_gpio_open(E1M IO20 = SDIO mux /E) -> NULL, err=%d -- the mux "
		       "cannot be enabled; check CONFIG_ALP_SDK_GPIO_CC3501E_PROXY and this app's "
		       "cc3501e_gpio_routes[] carry the IO20 entry\n",
		       (int)alp_last_error());
		return false;
	}

	alp_status_t cfg_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	/* ACTIVE LOW: `false` asserts /E and connects the card-side nets to the SoC. */
	alp_status_t en_rc = (cfg_rc == ALP_OK) ? alp_gpio_write(mux_en, false) : cfg_rc;

	bool         mux_level = true;
	alp_status_t rd_rc     = ALP_ERR_NOT_READY;
	if (en_rc == ALP_OK) {
		rd_rc = alp_gpio_read(mux_en, &mux_level);
	}
	printf("[sd][mux] SDIO mux ENABLE (E1M IO20 -> CC3501E GPIO_26, /E active low, driven "
	       "LOW): configure -> %d, write -> %d, read-back -> %d (level=%s, CORROBORATION "
	       "ONLY -- may reflect the bridge's output register rather than the pad, not proof "
	       "the line moved), settle=%u ms\n",
	       (int)cfg_rc,
	       (int)en_rc,
	       (int)rd_rc,
	       (rd_rc == ALP_OK) ? (mux_level ? "HIGH" : "LOW") : "?",
	       (unsigned)SD_MUX_SETTLE_MS);

	if (en_rc != ALP_OK) {
		printf("[sd][mux] the mux ENABLE could not be driven -- every SDHC probe below runs "
		       "against a card that is not connected to the controller\n");
		alp_gpio_close(mux_en);
		return false;
	}
	k_msleep(SD_MUX_SETTLE_MS);
	/* mux_en is intentionally left open and /E left asserted for the rest of
	 * the run, matching aen-evk-demo's phase 9 -- /E LOW is this bench's
	 * working state for the duration of the probes below, not a resource to
	 * restore-on-exit. */
	return true;
}

/*
 * PROBE 3 -- the maintainer's real question ("does the card work through
 * the reworked mux") cannot be answered by PROBE 1/2 alone: both stay at
 * the register/`sdhc_request()` level and never move a byte of card data.
 * PROBE 3 goes all the way through Zephyr's disk-access layer (the
 * `sdmmc` child node the board overlay now adds under &sdhc0, backed by
 * CONFIG_DISK_DRIVER_SDMMC + CONFIG_SDMMC_STACK -- see prj.conf):
 * `disk_access_init()`, geometry + CID over `disk_access_ioctl()`, and a
 * single `disk_access_read()` of sector 0.
 *
 * PROBE 1/2 are left exactly as they were -- this function runs AFTER
 * them, using their evidence (via `controller_alive`) only to word the
 * final verdict, not to skip itself: even a "controller looks dead"
 * reading is worth confirming disk_access_init() also fails, rather than
 * assuming it.
 */
#define SD_DISK_NAME          "SD"
#define SD_PROBE3_BLOCK_BYTES 512u /* SD/SDHC sector size; the geometry ioctl below confirms it */
#define SD_PROBE3_HEXDUMP_BYTES \
	64u /* a SHORT hex dump -- a prefix of the block, not the whole 512 B */
#define SD_PROBE3_HEXDUMP_PERLINE 16u

typedef enum {
	SD_VERDICT_CONTROLLER_DEAD,       /* clock gate or CAPABILITIES1 wrong -- PROBE 1/2's layer */
	SD_VERDICT_NO_CARD,               /* CMD8 itself got no response -- mux or card layer */
	SD_VERDICT_CARD_SEEN_INIT_FAILED, /* CMD8 answered, but disk_access_init() still failed --
					    * a card IS there; the failure is past CMD8 (see
					    * sd_probe3_full_enumeration()'s printf for detail) */
	SD_VERDICT_PARTIAL,               /* card enumerated but geometry/read did not fully succeed */
	SD_VERDICT_CARD_OK,               /* enumerated AND a block read back -- the rework works */
} sd_verdict_t;

/* `total_len` is the geometry ioctl's reported sector size; only the first
 * SD_PROBE3_HEXDUMP_BYTES of it are ever printed -- see the macro comment. */
static void sd_diag_hexdump_prefix(const uint8_t *buf, uint32_t total_len)
{
	uint32_t n = (total_len < SD_PROBE3_HEXDUMP_BYTES) ? total_len : SD_PROBE3_HEXDUMP_BYTES;

	for (uint32_t off = 0u; off < n; off += SD_PROBE3_HEXDUMP_PERLINE) {
		printf("[sd][probe3]   %04x:", off);
		for (uint32_t i = off; i < off + SD_PROBE3_HEXDUMP_PERLINE && i < n; i++) {
			printf(" %02x", buf[i]);
		}
		printf("\n");
	}
}

static sd_verdict_t sd_probe3_full_enumeration(bool controller_alive, bool mux_ok)
{
	printf("[sd] -- PROBE 3: full enumeration (disk_access) --------------------\n");

	if (!controller_alive) {
		printf("[sd][probe3] SKIPPED -- PROBE 2 already found the controller unclocked or "
		       "CAPABILITIES1 still zero; disk_access_init() would only time out against a "
		       "dead controller, telling us nothing PROBE 1/2 have not already shown. This "
		       "is a CONTROLLER-layer failure, not a card or mux one\n");
		return SD_VERDICT_CONTROLLER_DEAD;
	}

	/*
	 * Card-presence probe, INDEPENDENT of disk_access_init() and run
	 * before it: disk_access_init()'s own -ENOTSUP (-134) is ambiguous by
	 * itself between "no card ever answered" and "a card answered CMD8
	 * fully then failed later" (see sd_diag_probe_card_present()'s header
	 * comment) -- so the verdict below asks the CMD8 question directly
	 * rather than inferring it from disk_access_init()'s return code.
	 */
	bool card_present = sd_diag_probe_card_present();

	int drc = disk_access_init(SD_DISK_NAME);
	printf("[sd][probe3] disk_access_init(\"%s\") -> %d\n", SD_DISK_NAME, drc);
	if (drc != 0) {
		if (!card_present) {
			printf("[sd][probe3] NO CARD -- the controller is alive (PROBE 1/2 above) "
			       "and CMD8 itself got no response. mux ENABLE write %s (see "
			       "sd_mux_enable() above); if it reported OK, check the P18 jumper is "
			       "OPEN (required to select the microSD slot -- S must read LOW) and "
			       "that a card is actually seated in J7. This is a MUX-or-CARD-layer "
			       "failure, not a controller one\n",
			       mux_ok ? "reported OK" : "did NOT report OK");
			return SD_VERDICT_NO_CARD;
		}
		printf("[sd][probe3] CARD SEEN, INIT FAILED -- CMD8 got a response (a card IS "
		       "present and answering) but disk_access_init() still returned %d. This is "
		       "NOT a missing-card verdict: the failure is downstream of CMD8, inside "
		       "sdmmc_card_init() -- enable CONFIG_SD_LOG_LEVEL_DBG/CONFIG_SDHC_LOG_LEVEL_DBG "
		       "(see prj.conf) and read the debug log for the deciding command (e.g. an "
		       "ADMA_ERR on the first data-bearing command past enumeration)\n",
		       drc);
		return SD_VERDICT_CARD_SEEN_INIT_FAILED;
	}

	uint32_t sector_count = 0u, sector_size = 0u;
	int      sc_rc = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sector_count);
	int      ss_rc = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	printf("[sd][probe3] geometry: %u sectors x %u B = %llu MiB (ioctl rc %d / %d)\n",
	       (unsigned)sector_count,
	       (unsigned)sector_size,
	       (unsigned long long)(((uint64_t)sector_count * sector_size) / (1024u * 1024u)),
	       sc_rc,
	       ss_rc);

	static uint8_t block[SD_PROBE3_BLOCK_BYTES] __aligned(SD_PROBE3_BLOCK_BYTES);
	int            rd_rc = -EIO;
	bool geometry_ok = (sc_rc == 0) && (ss_rc == 0) && (sector_count > 0u) && (sector_size > 0u) &&
	                   (sector_size <= SD_PROBE3_BLOCK_BYTES);
	if (geometry_ok) {
		rd_rc = disk_access_read(SD_DISK_NAME, block, 0u, 1u);
	}
	printf("[sd][probe3] disk_access_read(sector 0, 1 block) -> %d%s\n",
	       rd_rc,
	       geometry_ok ? ""
	                   : " (SKIPPED -- geometry ioctl above did not return a usable "
	                     "sector size)");
	if (rd_rc == 0) {
		printf("[sd][probe3] first %u of %u B of sector 0:\n",
		       (unsigned)((sector_size < SD_PROBE3_HEXDUMP_BYTES) ? sector_size
		                                                          : SD_PROBE3_HEXDUMP_BYTES),
		       (unsigned)sector_size);
		sd_diag_hexdump_prefix(block, sector_size);
		/* The MBR / boot-sector signature sits past the hex-dump prefix;
		 * print it so a real card read is provable from the log alone. */
		if (sector_size >= 0x200u) {
			bool sig = (block[0x1FE] == 0x55u) && (block[0x1FF] == 0xAAu);
			printf("[sd][probe3] sector 0 bytes 0x1FE/0x1FF = %02x %02x (%s)\n",
			       block[0x1FE],
			       block[0x1FF],
			       sig ? "55 AA boot signature present" : "no 55 AA boot signature");
		}
	}

	/* DISK_IOCTL_GET_CARD_CID -- deliberately AFTER the sector read. Zephyr
	 * serves this ioctl with CMD2 ALL_SEND_CID (zephyr/subsys/sd/sd_ops.c:295,
	 * via :888), which a card only answers in the identification state.
	 * After disk_access_init() the card is in transfer state, so CMD2 times
	 * out and sdhc_dwc's error path soft-resets CMD|DAT -- and on the bench
	 * (evk-03, run 4) that reset landed immediately before CMD17, confounding
	 * the first 4-bit read. Running it last keeps it out of the read path;
	 * a failure here is expected and says nothing about the card. */
	uint32_t cid[4] = { 0 };
	int      cid_rc = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_GET_CARD_CID, cid);
	if (cid_rc == 0) {
		printf("[sd][probe3] CID = %08x %08x %08x %08x\n", cid[0], cid[1], cid[2], cid[3]);
	} else {
		printf("[sd][probe3] DISK_IOCTL_GET_CARD_CID -> %d (expected in transfer state: "
		       "Zephyr sends CMD2, see comment)\n",
		       cid_rc);
	}

	if (geometry_ok && rd_rc == 0) {
		printf("[sd][probe3] CARD OK -- enumerated AND a block read back successfully: the "
		       "card path through the reworked mux works\n");
		return SD_VERDICT_CARD_OK;
	}
	printf("[sd][probe3] PARTIAL -- disk_access_init() succeeded (the card answered) but "
	       "geometry and/or the block read did not (sc_rc=%d ss_rc=%d rd_rc=%d) -- a card "
	       "layer problem short of full data movement, not a clean PASS\n",
	       sc_rc,
	       ss_rc,
	       rd_rc);
	return SD_VERDICT_PARTIAL;
}

int main(void)
{
	bool mux_ok = sd_mux_enable();

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

	printf("[sd] RESULT (PROBE 1/2, controller-level): CMD0 no-reset -> %d, CMD0 post-reset -> "
	       "%d, sdhc_hw_reset -> %d. This is a REWORKED-MUX bench build "
	       "(test/2051-sdhc-enable-on-reworked-mux) -- see sd_mux_enable() above for whether "
	       "the mux ENABLE actually drove\n",
	       cmd0a_rc,
	       cmd0b_rc,
	       hwreset2_rc);

	/*
	 * Verdict for PROBE 3 below: the controller is judged "alive" only if
	 * BOTH the clock gate is set AND CAPABILITIES1 is non-zero -- the same
	 * pair PROBE 2 already prints, re-read fresh here (not reused from
	 * `clkctl_now` above) so this check reflects state AFTER PROBE 2's own
	 * sdhc_hw_reset()/CMD0, not before it.
	 */
	uint32_t clkctl_final = sys_read32(SD_CLKCTL_PER_MST);
	uint32_t caps_final   = sys_read32(SD_REG_CAPABILITIES1);
	bool     controller_alive =
	    ((clkctl_final & SD_CLKCTL_PER_MST_SD_EN_Msk) != 0u) && (caps_final != 0u);

	sd_verdict_t verdict = sd_probe3_full_enumeration(controller_alive, mux_ok);

	const char *verdict_str = "?";
	switch (verdict) {
	case SD_VERDICT_CONTROLLER_DEAD:
		verdict_str = "CONTROLLER DEAD -- clock gate or CAPABILITIES1 wrong (PROBE 1/2's "
		              "layer, upstream of the mux entirely)";
		break;
	case SD_VERDICT_NO_CARD:
		verdict_str = "CONTROLLER ALIVE, NO CARD -- CMD8 itself got no response (mux or "
		              "card layer; check mux ENABLE result, the P18 jumper, and that a "
		              "card is seated in J7)";
		break;
	case SD_VERDICT_CARD_SEEN_INIT_FAILED:
		verdict_str = "CARD SEEN, INIT FAILED -- CMD8 answered (a card IS present) but "
		              "disk_access_init() failed past that point; read the debug log for "
		              "the deciding command, this is not a missing-card result";
		break;
	case SD_VERDICT_PARTIAL:
		verdict_str = "PARTIAL -- card answered disk_access_init() but geometry/read did "
		              "not fully succeed";
		break;
	case SD_VERDICT_CARD_OK:
		verdict_str = "CARD ENUMERATED AND READ -- the rework works";
		break;
	}
	printf("[sd] RESULT (PROBE 3, final verdict): %s\n", verdict_str);
	printf("[sd] done\n");
	return 0;
}

#else /* !DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay) */

#include <errno.h> /* -ENODEV, the CLOCKCTRL_DEV-not-ready fallback below */
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/alif-ensemble-clocks-ext.h> /* ALIF_SDC_CLK */
#include <zephyr/kernel.h> /* pulls in the arch-specific sys_read32() implementation --
                              * zephyr/sys/sys_io.h above is doxygen-only declarations */

/*
 * clockctrl -- the SoC-wide clock-controller node ("alif,clockctrl",
 * zephyr/dts/arm/alif/ensemble/common/ensemble_common.dtsi, upstream
 * Zephyr), present and "okay" on every Alif Ensemble board REGARDLESS of
 * sdhc0's own status: it is the parent clock-gate block, not the SD
 * controller itself. CONFIG_CLOCK_CONTROL=y in prj.conf pulls its driver
 * in explicitly, since CONFIG_SDHC_DWC's own `select CLOCK_CONTROL` never
 * fires while sdhc0 is disabled (see prj.conf's comment).
 */
#define CLOCKCTRL_DEV DEVICE_DT_GET(DT_NODELABEL(clockctrl))

/*
 * ALP-SDK DELTA (#2051), not upstream: CLOCK-GATE PROOF. sdhc0 stays
 * disabled while this runs -- no pinctrl is ever applied and no SD pad is
 * ever touched by anything below. CLKCTL_PER_MST and CAPABILITIES1 are
 * both plain memory-mapped registers, reachable with a bare
 * sys_read32()/clock_control_on() regardless of whether the SDHC
 * *device* (which exists only when sdhc0 is "okay") is instantiated.
 * CLKCTL_PER_MST is a SoC-level clock gate, not the SD controller
 * itself: setting its bit 16 clocks the DWC SDHC block internally and
 * routes to NO pad. This is the read-only-before / write-the-gate /
 * read-only-after sequence that proves the gate, not a reset defect, is
 * what leaves the controller inert -- see the PR discussion for #2051
 * for why this is the only proof that can run without enabling SD
 * pinctrl or driving a single SD pad.
 */
static void sd_diag_prove_clock_gate(void)
{
	uint32_t clkctl_before = sys_read32(SD_CLKCTL_PER_MST);
	printf("[sd][gate] CLKCTL_PER_MST @0x%08x = 0x%08x  bit16(SDC_CKEN)=%u (before)\n",
	       (unsigned)SD_CLKCTL_PER_MST,
	       clkctl_before,
	       (unsigned)((clkctl_before & SD_CLKCTL_PER_MST_SD_EN_Msk) != 0u));

	uint32_t caps_before = sys_read32(SD_REG_CAPABILITIES1);
	printf("[sd][gate] CAPABILITIES1  @0x%08x = 0x%08x  (before -- read-only; 0x00000000 "
	       "expected while the gate above is clear)\n",
	       (unsigned)SD_REG_CAPABILITIES1,
	       caps_before);

	int ret = -ENODEV;
	if (device_is_ready(CLOCKCTRL_DEV)) {
		ret = clock_control_on(CLOCKCTRL_DEV, (clock_control_subsys_t)ALIF_SDC_CLK);
	}
	printf("[sd][gate] clock_control_on(clockctrl, ALIF_SDC_CLK) -> %d\n", ret);

	uint32_t clkctl_after = sys_read32(SD_CLKCTL_PER_MST);
	printf("[sd][gate] CLKCTL_PER_MST @0x%08x = 0x%08x  bit16(SDC_CKEN)=%u (after)\n",
	       (unsigned)SD_CLKCTL_PER_MST,
	       clkctl_after,
	       (unsigned)((clkctl_after & SD_CLKCTL_PER_MST_SD_EN_Msk) != 0u));

	uint32_t caps_after = sys_read32(SD_REG_CAPABILITIES1);
	printf("[sd][gate] CAPABILITIES1  @0x%08x = 0x%08x  (after)\n",
	       (unsigned)SD_REG_CAPABILITIES1,
	       caps_after);

	/*
	 * The verdict below checks TRANSITIONS (clear->set, zero->non-zero),
	 * never a specific expected word -- CAPABILITIES1's exact value is
	 * silicon fact this app has never had a bench-verified constant for
	 * (see the file header), and asserting a guessed one here would turn
	 * a real measurement into a self-fulfilling one. Whatever the
	 * silicon returns stands as the evidence, printed above.
	 */
	bool gate_went_clear_to_set = ((clkctl_before & SD_CLKCTL_PER_MST_SD_EN_Msk) == 0u) &&
	                              ((clkctl_after & SD_CLKCTL_PER_MST_SD_EN_Msk) != 0u);
	bool caps_went_zero_to_live = (caps_before == 0u) && (caps_after != 0u);

	printf("[sd][gate] RESULT %s: bit16 clear->set=%s CAPABILITIES1 0x00000000->non-zero=%s "
	       "-- %s\n",
	       (gate_went_clear_to_set && caps_went_zero_to_live) ? "PASS" : "INCONCLUSIVE",
	       gate_went_clear_to_set ? "yes" : "no",
	       caps_went_zero_to_live ? "yes" : "no",
	       (gate_went_clear_to_set && caps_went_zero_to_live)
	           ? "the peripheral clock gate was what left the controller inert, and "
	             "clock_control_on() is what clears it (#2051)"
	           : "does not confirm the gate was the (only) problem on this run -- see "
	             "the raw register values above");
	printf("[sd] This proves the controller becomes ADDRESSABLE -- it does NOT prove a "
	       "card enumerates. No SD pin was opened, configured, or driven above; sdhc0 "
	       "stays disabled and no pinctrl was ever applied.\n");
}

int main(void)
{
	printf("[sd] SD host controller (sdhc0) is DISABLED on this board -- the E1M-EVK "
	       "2626-R2 SDIO mux (74LVC157 U38/U39) has no high-impedance state, so its "
	       "SoC-facing outputs are actively held low whenever the mux is powered, "
	       "regardless of its ENABLE input. Enabling the controller would start SD "
	       "pinctrl and the identification clock and fight those held-low pads. This is "
	       "a hardware defect pending a component change, not a firmware gap -- see "
	       "README.md. Nothing in this build opens, configures, or drives an SD pin.\n");

	sd_diag_prove_clock_gate();

	printf("[sd] RESULT SKIPPED: card path not exercised on this board (hardware defect, "
	       "rework pending) -- see the clock-gate proof above\n");
	return 0;
}

#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay) */
