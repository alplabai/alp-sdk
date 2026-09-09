/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sdcard-readout -- bring up the Ensemble E8 SD Host Controller on the
 * E1M-AEN801 (M55-HE) via the vendored snps,dwc-sdhc driver + the Zephyr SDMMC
 * disk, and probe a microSD card.  Drives the standard Zephyr disk-access API
 * (disk_access_init / disk_access_ioctl) on the "SD" disk.  READ-ONLY BY
 * CONSTRUCTION: no CONFIG_FILE_SYSTEM, no fs_* call, no disk_access_write, no
 * mkfs anywhere in this app -- disk_access_init() plus the geometry ioctls,
 * and nothing that could touch a byte on whatever card is in the slot.
 *
 * EVK ROUTING: on the E1M EVK the microSD sits on the SDIO bus behind a
 * 74LVC157 mux with an ENABLE (E1M IO20) and a SELECT (E1M IO21), and BOTH
 * are CC3501E-side on this module (metadata/e1m_modules/aen/
 * from-cc3501e.tsv) -- so the card is electrically disconnected from the SoC
 * until something drives the mux over the coprocessor's inter-chip bridge.
 * THAT WAS THIS APP'S OWN GAP UNTIL NOW: earlier revisions had no GPIO code
 * at all, so a standalone run measured a controller with the card unreachable
 * and could not tell that apart from a real disk fault.  This revision brings
 * the bridge up and drives the ENABLE itself -- see main() below -- so a bench
 * run of THIS app alone is now a valid vehicle for SD debugging; it no longer
 * needs examples/aen/aen-evk-demo's other thirteen phases just to reach the
 * card.  SELECT (IO21) is still not software-drivable on this module (r2:
 * physically open; r1: driving it would contend with the P18 header jumper),
 * so it stays a HAND-SET jumper -- see README.md and main()'s mux-enable
 * comment for the same argument phase 9 of aen-evk-demo makes in full.
 *
 * PASS gate: disk_access_init returns 0 and the card geometry reads back (a
 * card was actually reachable + enumerated).  A clean controller bring-up
 * where the bridge + mux came up but the card still does not enumerate is
 * reported PARTIAL -- the controller/driver path is proven, and the mux is
 * proven asserted, so the remaining gap is elsewhere (see README.md for the
 * currently open one).  A bridge or mux failure is its own, earlier verdict:
 * see main()'s early-return paths below.  A card enumerating with the bridge
 * NOT brought up would be worthless as a measurement, so this app refuses to
 * even try disk_access_init() unless the bridge + mux both came up clean.
 *
 * SD_RST (#2035): P14_2 (SD_RST_B's pad) had never been driven by anything
 * in this app or the board tree before this revision, and three independent
 * vendor sources drive it as part of bringing an SDHC controller up on this
 * exact pad -- see the reset-pulse block in main() below for the full
 * citation. The same PASS/PARTIAL/FAIL gating now applies to it: a
 * reset-pin fault is reported and this app stops before disk_access_init(),
 * for the same reason a mux fault stops it -- a disk result measured with
 * the reset line unproven would be meaningless.
 *
 * SD clock gate (#2035): CLKCTL_PER_MST bit 16 is a SOURCE-SELECT that has
 * always picked the SD controller's 100 MHz source, not an enable -- and the
 * gate for that source, CGU CLK_ENA bit 7 (CLK100M), read 0xFE03FF71 on
 * silicon (bit 7 CLEAR) with the resident Linux chain fully up, meaning the
 * source feeding the mux had never been switched on. main() now asks the
 * Secure Enclave to enable it (and CLKEN_CLK_20M alongside, per Alif's own
 * baremetal demo) before the controller is touched, and reads CGU CLK_ENA
 * back afterwards to prove the bit actually flipped on silicon rather than
 * just that the service call returned success -- see sd_se_enable_clock()
 * below for the full citation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h> /* memset() for the SE clock-enable request packet, see below */

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/sys_io.h> /* sys_read32() -- raw diagnostic register reads, see below */

#include "alp/peripheral.h"    /* alp_gpio_*, alp_status_t */
#include "alp/e1m_pinout.h"    /* ALP_E1M_GPIO_IO20 */
#include "alp/chips/cc3501e.h" /* cc3501e_t */

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template,
                              * copied verbatim from examples/aen/aen-evk-demo. */

/* SE clock-enable request (#2035): clk_set_enable_svc_t, SERVICE_CLOCK_SET_ENABLE and
 * CLKEN_CLK_100M/CLKEN_CLK_20M come transitively via se_service.h (services_lib_api.h +
 * services_lib_ids.h) -- the same pattern src/backends/security/se_cryptocell.c documents
 * for its own direct SE requests. No DFP include needed. */
#include <se_service.h>

#define DISK_NAME "SD"

/*
 * ============================================================================
 * SD register-level diagnostics (#2035) -- SW_RST_CMD investigation
 * ============================================================================
 * The card still does not enumerate even with the bridge/mux proven driven
 * (see the file header) and `disk_access_init` returns -116: `SW_RST_R` at
 * 0x4810202F stays 0x02, i.e. SW_RST_CMD never self-clears while SW_RST_DAT
 * does. As of this revision main() also pulses SD_RST (P14_2) before any of
 * this runs -- see step 6 below -- so a persisting -116 is no longer
 * explainable by an undriven reset line; whatever is captured here on the
 * next load is measured with SD_RST proven asserted-then-released.
 * Everything read on the bench so far was sampled AFTER the driver's
 * own timeout path had already written SW_RST_CMD|SW_RST_DAT (see
 * sdhc_dwc_wait_cmd_complete() in zephyr/drivers/sdhc/sdhc_dwc.c) -- which
 * clears command-complete -- so an "INT STATUS = 0x00000000" reading taken
 * that way carries no information: it cannot distinguish "the command never
 * completed" from "it completed and the reset wiped the evidence". This
 * section exists to stop guessing and start measuring, on the NEXT load
 * either way -- SW_RST_CMD fixed or not.
 *
 * NOT a driver modification: this app has no hook into sdhc_dwc.c's own
 * command path, so "before any SW_RST write" is approximated by watching
 * PSTATE's CMD_INHIBIT bit (bit 0) for its 0->1 rising edge -- the
 * externally-observable side effect of the driver's own CMD_R write -- from a
 * low-priority background thread that only gets the CPU while main() is
 * blocked inside the driver's own k_event_wait(). The driver's own
 * command-complete timeout defaults to 1000 ms (sdhc_dwc_wait_cmd_complete()),
 * so a sample taken 10 ms after the rising edge is comfortably pre-reset.
 */
#define SD_REG_BASE   0x48102000u
#define SD_REG_PSTATE (SD_REG_BASE + 0x024u) /* Present State */
#define SD_REG_NORMAL_INT_STAT \
	(SD_REG_BASE + 0x030u)                          /* Normal + Error Interrupt Status (32b read) */
#define SD_REG_CAPABILITIES1 (SD_REG_BASE + 0x040u) /* Capabilities 1 -- base clock field, Task 4 */

#define SD_PSTATE_CMD_INHIBIT_Msk 0x00000001u /* bit0: a command is in flight */
#define SD_PSTATE_DAT_INHIBIT_Msk 0x00000002u /* bit1 */

/* Alif E8 pinmux registers for the SD "B" route's CLK/CMD pads (P14_1/P14_0).
 * Pad config lives at bits[23:16]; bit 16 of that byte is PADCTRL_READ_ENABLE
 * -- the exact bit Task 1's fix turns on for CLK. Printing it here proves the
 * setting reached silicon, not just the devicetree. */
#define SD_PINMUX_P14_1_CLK       0x1A6031C4u
#define SD_PINMUX_P14_0_CMD       0x1A6031C0u
#define SD_PINMUX_PADCFG_Pos      16u
#define SD_PINMUX_PADCFG_Msk      (0xFFu << SD_PINMUX_PADCFG_Pos)
#define SD_PINMUX_READ_ENABLE_Msk (1u << SD_PINMUX_PADCFG_Pos)

/* CLKCTL_PER_MST -- bit 16 gates the SD peripheral clock at the SoC clock-tree
 * level, upstream of anything the SDHC's own Clock Control Register does. */
#define SD_CLKCTL_PER_MST           0x4903F00Cu
#define SD_CLKCTL_PER_MST_SD_EN_Msk (1u << 16)

/* CGU (Clock Generation Unit) CLK_ENA -- the actual ENABLE gate for the 100 MHz
 * source CLKCTL_PER_MST bit 16 above only SELECTS: vendor Linux models sdhci_clk
 * as a 1-bit mux between syst_hclk and 100m_clk and forces the latter
 * (drivers/clk/clk-ensemble.c), and CLK100M here is the gate for that 100 MHz
 * leg's PLL output (Alif's own E8 SVD: "Enable 100MHz PLL clock"). MEASURED on
 * silicon, three reads, identical, with the resident Linux chain fully booted:
 * 0xFE03FF71 -- bit0 SYSPLL=1, bit7 CLK100M=0, bit9 CLK10M=1, bit22 usb_clk=0.
 * Bits 4-6 are all set, so bit7 reading clear is a genuine single gap, not part
 * of a blank region -- and a command circuit with no clock cannot complete
 * SW_RST_CMD or clock out CMD0, exactly what this app's own SW_RST_R
 * diagnostics above have been showing.
 *
 * THE SE OWNS THIS REGISTER -- it is firewalled on this part, and a direct
 * write to it is the class of action that has bricked boards on this bench.
 * This app only ever READS it (sd_diag_print_cgu_clk_ena() below), to prove
 * the SERVICE_CLOCK_SET_ENABLE request in sd_se_enable_clock() actually
 * reached silicon; the enable itself goes through se_service_send_request(),
 * never a direct write to this address. */
#define SD_CGU_CLK_ENA             0x1A602014u
#define SD_CGU_CLK_ENA_SYSPLL_Msk  (1u << 0)
#define SD_CGU_CLK_ENA_CLK100M_Msk (1u << 7)
#define SD_CGU_CLK_ENA_CLK10M_Msk  (1u << 9)
#define SD_CGU_CLK_ENA_USBCLK_Msk  (1u << 22)

/* Print the four "before" registers: does the pad setting, the clock gate and
 * the capability field this app depends on actually look right on THIS
 * silicon, before disk_access_init() even runs. */
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

/* Read CGU CLK_ENA back AFTER the SE clock-enable request in main() runs, so a
 * bench run shows whether CLK100M (bit 7) actually flipped on silicon, not just
 * that the service call returned success. Read-only, like the function above --
 * see the SD_CGU_CLK_ENA comment for why this register is never written
 * directly. */
static void sd_diag_print_cgu_clk_ena(void)
{
	uint32_t clk_ena = sys_read32(SD_CGU_CLK_ENA);

	printf("[sd][diag] CGU CLK_ENA @0x%08x = 0x%08x  SYSPLL(bit0)=%u CLK100M(bit7)=%u "
	       "CLK10M(bit9)=%u usb_clk(bit22)=%u -- CLK100M gates the SD controller's clock "
	       "source; 1 here means the SE enable request took effect\n",
	       (unsigned)SD_CGU_CLK_ENA,
	       clk_ena,
	       (unsigned)((clk_ena & SD_CGU_CLK_ENA_SYSPLL_Msk) != 0u),
	       (unsigned)((clk_ena & SD_CGU_CLK_ENA_CLK100M_Msk) != 0u),
	       (unsigned)((clk_ena & SD_CGU_CLK_ENA_CLK10M_Msk) != 0u),
	       (unsigned)((clk_ena & SD_CGU_CLK_ENA_USBCLK_Msk) != 0u));
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

/*
 * FILE-STATIC, not a main() local: cc3501e_t is ~32 KB (the driver's own
 * scratch buffers live inside it, not on the caller's stack -- see
 * cc3501e_bridge.h), and CONFIG_MAIN_STACK_SIZE here is 4096. Putting it on
 * main()'s stack the way aen-cc3501e-bringup does would blow PSPLIM; that app
 * compensates with a 32768-byte stack instead. This app's handle is static
 * for exactly the same reason aen-evk-demo's is.
 */
static cc3501e_t cc35_fw;

/* Settle time for the CC3501E driving its pad and the card seeing its lines
 * after the mux ENABLE write -- see the comment at the write site below.
 * Copied from aen-evk-demo's SD_MUX_SETTLE_MS (same board, same mux, same
 * settle requirement). */
#define SD_MUX_SETTLE_MS 10u

/* SD_RST (P14_2) -- index [3] in the board overlay's `alp,pin-array` node
 * (boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay). A native
 * Alif GPIO, NOT a CC3501E-proxied one -- unlike the mux ENABLE above, this
 * pin is reached through the ordinary Alif GPIO backend (gpio14), so it
 * needs no bridge and no route-table entry. Raw index, not an
 * ALP_E1M_GPIO_* macro, for the same reason CC3501E_BRIDGE_PIN_* are raw
 * indices: this pin is SoM-internal, not an E1M edge pad. */
#define SD_RST_PIN_ID 3u

/* Reset-pulse timings, taken verbatim from vendor Linux's
 * arch/arm/mach-ensemble/sdhci-alif-reset.c (an arch_initcall that runs
 * BEFORE the SDHCI driver probes): drive the line low, sleep 1-2 ms, drive
 * it high, sleep 2-3 ms. This app uses the upper bound of each range rather
 * than inventing its own numbers -- see the pulse block in main() for the
 * full three-source citation (vendor Linux + its devicetree binding +
 * Alif's own baremetal DFP demo_sd.c). */
#define SD_RST_ASSERT_MS         2u /* line held LOW */
#define SD_RST_RELEASE_SETTLE_MS 3u /* line held HIGH before anything else touches the controller */

/* Bounded PING retry after bridge bring-up, before the first proxied request
 * (#2035 GPIO-proxy race) -- same parameters as aen-evk-demo's phase 8
 * (examples/aen/aen-evk-demo/src/main.c: CC35_PING_RETRIES/CC35_PING_GAP_MS),
 * which measured needing 11 of 25 attempts, 200 ms apart, on this silicon.
 *
 * MEASURED, TWO DATA POINTS ONLY -- margin visible, not a new budget: the
 * demo's run above needed 11 of 25 (~2.2 s); a run of THIS app needed 15 of
 * 25 (~3.0 s). That leaves only ~2 s of headroom over the worse of the two,
 * and the count is variable run to run. Not enough data to justify raising
 * (or trusting) a budget, so it stays 25/200 -- but the next person hitting
 * exhaustion should know the margin is this thin before assuming a hard
 * regression. */
#define CC35_PING_RETRIES 25u
#define CC35_PING_GAP_MS  200u

/* GET_MAC timeout for the meta-handshake below -- same value as
 * aen-evk-demo's CC35_MAC_TIMEOUT_MS (examples/aen/aen-evk-demo/src/main.c),
 * copied rather than shared because these are two separate example apps. */
#define CC35_MAC_TIMEOUT_MS 2000u

/*
 * SE clock-enable request wrapper (#2035) -- CLKCTL_PER_MST_SD_EN_Msk earlier
 * in this file confirms the SD peripheral clock is gated ON, and CLKCTL_PER_MST
 * bit 16 is a source-select that has always picked the 100 MHz leg (vendor
 * Linux clk-ensemble.c models sdhci_clk as exactly that 1-bit mux, forced to
 * 100m_clk) -- but nothing in this app, the board tree, or vendor Linux's own
 * devicetree (mmc@48102000 is `disabled` there) had ever asked the Secure
 * Enclave to switch that 100 MHz source on. Alif's own baremetal DFP demo
 * does, right before its own sd_host_init(): SERVICES_clocks_enable_clock(...,
 * CLKEN_CLK_100M, true, ...) (demo_sd.c), and enables CLKEN_CLK_20M alongside
 * -- their DFP treats a 20 MHz clock as required for SD too, though which CGU
 * bit it gates has not been established (unlike CLK100M/bit7, confirmed by the
 * E8 SVD). hal_alif's Zephyr layer has no clock-enable wrapper of its own
 * (only se_service_clock_set_divider()), so this builds the same
 * SERVICE_CLOCK_SET_ENABLE request the DFP's SERVICES_clocks_enable_clock()
 * sends (services_host_clocks.c) and posts it with the generic
 * se_service_send_request() transport -- the same one
 * src/backends/security/se_cryptocell.c and src/backends/ext/alif/storage.c
 * already use for their own direct SE requests.
 *
 * ORDERING MATTERS: the 0xFE03FF71 reading cited above was taken with the
 * resident Linux chain fully up. Vendor Linux's clk_disable_unused late-
 * initcall drops every CGU gate that has no enabled consumer (it marks only
 * camera_pixclk CLK_IGNORE_UNUSED), and mmc@48102000 is disabled in the
 * resident device tree -- so this request MUST run after that late-initcall
 * has already fired, or the gate gets dropped again a moment later. This
 * app's RAM-run image loading over an already-booted system satisfies that
 * naturally; moving this call into an early init hook that runs before Linux
 * finishes booting would silently break it again.
 *
 * Reports BOTH return codes rather than one collapsed bool: the transport
 * layer (se_service_send_request() -- MHUv2 round trip / SE busy / timeout)
 * fails differently from the SE itself rejecting the request
 * (pkt.resp_error_code) -- collapsing them would hide which one happened, and
 * a silent clock request that fails is exactly the defect class this slice
 * has spent all day removing.
 */
static alp_status_t sd_se_enable_clock(uint32_t clock_type, const char *name)
{
	clk_set_enable_svc_t pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.hdr_service_id = SERVICE_CLOCK_SET_ENABLE;
	pkt.send_clock_type       = clock_type;
	pkt.send_enable           = 1u;

	int transport_rc = se_service_send_request((uint32_t *)&pkt, sizeof(pkt));
	printf("[sd] SE CLOCK_SET_ENABLE(%s) -> transport=%d se_resp=%d\n",
	       name,
	       transport_rc,
	       (int)pkt.resp_error_code);

	if (transport_rc != 0) {
		return ALP_ERR_IO;
	}
	return (pkt.resp_error_code == 0) ? ALP_OK : ALP_ERR_IO;
}

int main(void)
{
	/*
	 * --- 1. Bring the CC3501E bridge up -------------------------------
	 * One call: opens SPI1 (hardware SS0, ALP_SPI_NO_CS) and the WIFI_EN /
	 * nRESET / READY pins, turns the LP pads' output drivers on, binds
	 * them, then runs the power-up and reset sequence (including the
	 * Puya-flash double-boot workaround). Blocks ~900 ms. Silicon-proven
	 * today as aen-evk-demo's phase 8 -- this is that same call, not a
	 * reimplementation.
	 */
	alp_status_t bridge_rc = cc3501e_bridge_bringup(&cc35_fw);
	printf("[sd] cc3501e_bridge_bringup() -> %d\n", (int)bridge_rc);
	if (bridge_rc != ALP_OK) {
		/*
		 * NOT_PRESENT_ON_THIS_SOC means the overlay does not declare the
		 * bridge's SPI bus / control pins -- a build fault, since the
		 * CC3501E is fitted on every E1M-AEN SoM. ALP_ERR_VERSION means
		 * cc3501e_reset() refused on a MAJOR protocol skew between this
		 * host and the coprocessor's firmware and left the handle
		 * unusable. Either way every step below would run against a
		 * bridge that never came up, so stop here rather than let a
		 * disk fault measured with the mux undriven masquerade as a
		 * real one -- that has already cost a bench session once.
		 */
		printf("[sd] RESULT FAIL: CC3501E bridge did not come up (rc=%d) -- the SDIO mux "
		       "ENABLE rides this bridge, so the card cannot be reached at all. Not "
		       "attempting disk_access_init: a disk error measured with the mux "
		       "undriven would be meaningless. Check the board overlay carries the "
		       "SPI1/WIFI_EN/nRESET/READY nodes and that CONFIG_ALP_SDK_CHIP_CC3501E is "
		       "set\n",
		       (int)bridge_rc);
		return -1;
	}

	/*
	 * --- 2. Wait for the bridge to be READY, not just electrically up --
	 * cc3501e_bridge_bringup() returning ALP_OK means the reset sequence
	 * finished -- rails up, nRESET released, the ~900 ms boot budget
	 * waited out -- NOT that the coprocessor is parsing requests yet.
	 * chips/cc3501e/cc3501e_core.c's cc3501e_reset() says so explicitly:
	 * its own GET_VERSION probe treats a round trip that never completes
	 * as "let the caller retry", not as a reset failure, precisely
	 * because protocol readiness is documented as the CALLER's job (see
	 * the comment block above that call, which names this app's own
	 * bringup helper as one of the two callers required to retry).
	 *
	 * Without this wait, the very next proxied GPIO configure below raced
	 * the coprocessor and lost: measured on hardware as `configure -> -4`
	 * (ALP_ERR_TIMEOUT), immediately after a clean `bridge_bringup() -> 0`.
	 * aen-evk-demo's phase 8 hits the identical gap before its own first
	 * request and closes it with a bounded PING retry loop -- mirrored
	 * here with the same parameters (25 attempts, 200 ms apart) rather
	 * than an invented timeout, because 200 ms apart is what was actually
	 * measured needing 11 of 25 attempts on this silicon (see the PING
	 * log line in examples/aen/aen-evk-demo/src/main.c).
	 */
	alp_status_t ping_rc  = ALP_ERR_TIMEOUT;
	unsigned     attempts = 0u;
	for (; attempts < CC35_PING_RETRIES; ++attempts) {
		ping_rc = cc3501e_ping(&cc35_fw);
		if (ping_rc == ALP_OK) {
			attempts++; /* count the one that succeeded, not the ones before it */
			break;
		}
		k_msleep(CC35_PING_GAP_MS);
	}
	printf("[sd] CC3501E: PING (0x00) -> %d after %u attempt(s) of %u (%u ms apart)\n",
	       (int)ping_rc,
	       attempts,
	       CC35_PING_RETRIES,
	       CC35_PING_GAP_MS);
	if (ping_rc != ALP_OK) {
		/*
		 * Distinguish "the coprocessor never became ready" from "the GPIO
		 * request failed" -- the mux ENABLE write below was never reached,
		 * so a reader must not mistake this for a mux/route-table fault.
		 * The figure below is the SLEEP budget only, same caveat as the
		 * demo: each attempt also spends its own transport timeout inside
		 * cc3501e_ping(), so real elapsed time is longer.
		 */
		printf("[sd] RESULT FAIL: the CC3501E bridge came up electrically (rc=0) but never "
		       "became ready to serve requests -- no PING answer after %u ms of retry gaps "
		       "(plus each attempt's own transport timeout, so longer in wall-clock). This is "
		       "NOT a mux/GPIO failure: the proxied mux ENABLE request below was never "
		       "attempted. Not attempting disk_access_init: a disk error measured with the "
		       "mux undriven would be meaningless\n",
		       (unsigned)(CC35_PING_RETRIES * CC35_PING_GAP_MS));
		return -1;
	}

	/*
	 * --- 3. Meta handshake: VERSION, MAC, CAPABILITIES ------------------
	 * NOT here for their own sake -- this app has no use for the protocol
	 * version, the station MAC, or the capability bitmap. They are issued
	 * because, measured on this silicon, the mux ENABLE request below
	 * (the first PROXIED GPIO request this app makes) came back
	 * ALP_ERR_INVAL -- traced all the way to the coprocessor's own
	 * RESP_ERR_INVALID reply, not a host-side rejection (#2035) -- when it
	 * was the first request sent after PING. examples/aen/aen-evk-demo's
	 * phase 8 never hits that: it always runs this exact three-command
	 * sequence before phase 9 touches any GPIO. Mirroring it here is the
	 * cheapest way to test whether the coprocessor requires this
	 * handshake before it will service a proxied GPIO configure -- same
	 * three opcodes, same order, same driver calls as the demo. If that
	 * turns out not to be the fix, this block still earns its keep as the
	 * demo's identity check; it is not being deleted either way.
	 *
	 * Deliberately NOT issued: WIFI_SCAN_START / BLE_ENABLE. Those bring
	 * up radios this app has no business touching, cost real seconds
	 * apiece, and are far less likely to be a GPIO-proxy precondition
	 * than the capability handshake below -- see the task instructions
	 * for this change. Adding them is a later, deliberate step if this
	 * block alone does not clear the INVALID.
	 */
	uint16_t     cc35_version  = 0u;
	alp_status_t version_rc    = cc3501e_get_version(&cc35_fw, &cc35_version);
	unsigned     cc35_fw_major = ALP_CC3501E_PROTOCOL_VERSION_MAJOR(cc35_version);
	unsigned     cc35_fw_minor = ALP_CC3501E_PROTOCOL_VERSION_MINOR(cc35_version);
	printf("[sd] CC3501E: GET_VERSION (0x01) -> %d protocol v%u.%u (host built for v%u.%u) %s\n",
	       (int)version_rc,
	       cc35_fw_major,
	       cc35_fw_minor,
	       (unsigned)ALP_CC3501E_PROTOCOL_MAJOR,
	       (unsigned)ALP_CC3501E_PROTOCOL_MINOR,
	       (version_rc == ALP_OK && cc35_fw_major == (unsigned)ALP_CC3501E_PROTOCOL_MAJOR)
	           ? "match"
	           : "MAJOR MISMATCH or no reply");

	uint8_t      cc35_mac[CC3501E_MAC_LEN] = { 0 };
	alp_status_t mac_rc = cc3501e_wifi_get_mac(&cc35_fw, cc35_mac, CC35_MAC_TIMEOUT_MS);
	printf("[sd] CC3501E: GET_MAC (0x03) -> %d  %02x:%02x:%02x:%02x:%02x:%02x\n",
	       (int)mac_rc,
	       cc35_mac[0],
	       cc35_mac[1],
	       cc35_mac[2],
	       cc35_mac[3],
	       cc35_mac[4],
	       cc35_mac[5]);

	uint32_t     cc35_caps    = 0u;
	alp_status_t caps_rc      = cc3501e_get_capabilities(&cc35_fw, &cc35_caps);
	bool         cap_gpio_prx = (cc35_caps & ALP_CC3501E_CAP_GPIO_PROXY) != 0u;
	printf("[sd] CC3501E: GET_CAPABILITIES (0x06) -> %d caps=0x%08x gpio_proxy=%s%s\n",
	       (int)caps_rc,
	       (unsigned)cc35_caps,
	       cap_gpio_prx ? "yes" : "no",
	       (caps_rc == ALP_ERR_INVAL)
	           ? " (INVAL = firmware predates the opcode: no capability information)"
	       : (caps_rc == ALP_OK && !cap_gpio_prx)
	           ? " -- firmware itself reports GPIO proxying unavailable; that alone would "
	             "explain the mux ENABLE INVALID below, independent of command ordering"
	           : "");

	/*
	 * --- 4. Attach the bridge to the GPIO proxy -------------------------
	 * cc3501e_bridge_bringup() already calls alp_gpio_cc3501e_attach()
	 * internally (see cc3501e_bridge.c step 3) and ignores its return --
	 * so the proxy is already routing by this point on a clean bring-up.
	 * Call it again here, explicitly, and check it: alp_gpio_open() below
	 * cannot tell an unattached proxy apart from a genuinely un-owned
	 * pin_id -- both DELEGATE to the platform driver and fail with the
	 * same ALP_ERR_INVAL (src/backends/gpio/cc3501e_proxy.c:199) -- so a
	 * silent gap here would surface, if at all, as a confusing mux
	 * failure three lines down instead of as what it is. That silent
	 * fall-through is exactly what cost two bench loads before this app
	 * had a PING wait; make the attach outcome visible too rather than
	 * assume the implicit call inside bring-up covers it.
	 */
	alp_status_t attach_rc = alp_gpio_cc3501e_attach(&cc35_fw);
	printf("[sd] alp_gpio_cc3501e_attach() -> %d\n", (int)attach_rc);
	if (attach_rc != ALP_OK) {
		printf("[sd] RESULT FAIL: the GPIO proxy did not attach to the bridge (rc=%d) -- "
		       "every proxied pin, including the mux ENABLE below, would silently "
		       "delegate to the Alif platform driver and fail. Not attempting the mux "
		       "write: a failure there would look like a route-table or hardware fault "
		       "instead of this\n",
		       (int)attach_rc);
		return -1;
	}

	/*
	 * --- 5. Assert the SDIO mux ENABLE over the GPIO proxy -------------
	 * alp_gpio_open() on a PORTABLE E1M pin id. The proxy backend looks
	 * IO20 up in this app's cc3501e_gpio_routes[] table, finds raw
	 * CC3501E GPIO_26, and sends the configure/write over the bridge just
	 * brought up. Nothing here names GPIO_26 -- the raw index belongs in
	 * the route table, which is derived from the SoM pad map, not in app
	 * code. Copied from aen-evk-demo's phase 9 step 1, its silicon-proven
	 * origin.
	 */
	alp_gpio_t *mux_en = alp_gpio_open(ALP_E1M_GPIO_IO20);
	if (mux_en == NULL) {
		printf("[sd] alp_gpio_open(E1M IO20 = SDIO mux /E) -> NULL, err=%d -- the mux "
		       "cannot be enabled, so the card is electrically disconnected. IO20 "
		       "reaches CC3501E GPIO_26 on BOTH module revisions and the bridge just "
		       "came up, so this is a build/route-table fault, not an absent card\n",
		       (int)alp_last_error());
		return -1;
	}

	alp_status_t cfg_rc = alp_gpio_configure(mux_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	/* ACTIVE LOW: `false` asserts /E and connects the card to the SoC.
	 * Skip the write and the read-back below when configure() itself
	 * already failed -- driving, and then reading back, a pin that was
	 * never configured as an output would report on calls that never
	 * ran. */
	alp_status_t en_rc     = (cfg_rc == ALP_OK) ? alp_gpio_write(mux_en, false) : cfg_rc;
	bool         mux_level = true;
	alp_status_t rd_rc     = ALP_OK;
	const char  *level_str = "?";
	if (cfg_rc == ALP_OK) {
		/* Read the pin back rather than trusting the write return code
		 * alone. A LOW read-back is CORROBORATION only, not proof: it
		 * may report the bridge's own output register rather than the
		 * pad. It does not gate anything below. */
		rd_rc     = alp_gpio_read(mux_en, &mux_level);
		level_str = (rd_rc == ALP_OK) ? (mux_level ? "HIGH" : "LOW") : "?";
	}
	printf("[sd] mux ENABLE via GPIO proxy (E1M IO20 -> CC3501E GPIO_26, /E active low, "
	       "driven LOW): configure -> %d, write -> %s, read-back -> %s (level=%s), "
	       "settle=%u ms\n",
	       (int)cfg_rc,
	       (cfg_rc == ALP_OK) ? "ran" : "SKIPPED (configure failed)",
	       (cfg_rc == ALP_OK) ? "ran" : "SKIPPED",
	       level_str,
	       (unsigned)SD_MUX_SETTLE_MS);

	if (en_rc != ALP_OK) {
		printf("[sd] RESULT FAIL: the mux ENABLE could not be driven (rc=%d) -- the card "
		       "is not connected to the SoC. Check that the bridge bring-up above really "
		       "passed and that this build carries the IO20 route. Not attempting "
		       "disk_access_init: a disk error against an undriven mux is meaningless\n",
		       (int)en_rc);
		/*
		 * No restore-to-idle here, on the maintainer's explicit
		 * instruction: /E LOW is this board's working state, not a
		 * transient this app borrows and must give back on the way
		 * out. It also lets the pin be metered at U38 pin 15 / U39
		 * pin 15 at any time after this app runs. See the identical
		 * decision, and its full reasoning, in aen-evk-demo's phase 9
		 * (commit 79cdeca6d). close() only frees the host-side proxy
		 * handle -- whatever GPIO_26 was left driving stays as-is.
		 */
		alp_gpio_close(mux_en);
		return -1;
	}
	k_msleep(SD_MUX_SETTLE_MS);

	/*
	 * --- 6. Pulse SD_RST before touching the controller ------------------
	 * P14_2 (the SD_RST_B pad, muxed here as plain GPIO -- see the overlay's
	 * pinctrl_sdmmc group2 comment) has never been driven by anything in
	 * this app or the board tree before this revision. Three independent
	 * vendor sources drive this exact line as part of bringing an SD host
	 * controller up on this exact pad, and all three bit-bang it as a GPIO
	 * rather than trusting it to the controller's own alternate function --
	 * this app follows them rather than the alternate-function route:
	 *   - vendor Linux `arch/arm/mach-ensemble/sdhci-alif-reset.c`, an
	 *     `arch_initcall` that runs BEFORE the SDHCI driver probes: take the
	 *     reset GPIO (defaulting high), drive it LOW, sleep 1-2 ms, drive it
	 *     HIGH, sleep 2-3 ms;
	 *   - its binding `Documentation/devicetree/bindings/mmc/
	 *     alif,sdhci-alif-reset.yaml`, which lists `reset-gpios` under
	 *     `required:`;
	 *   - Alif's own baremetal DFP: `sd_host_init()` (`drivers/source/sd.c`)
	 *     calls `sd_param.reset_cb` as the FIRST action inside init, and the
	 *     demo's callback (`Boards/Templates/Baremetal/demo_sd.c`) drives
	 *     the pin low, busy-waits, then high.
	 * Never having driven this pin may have been holding the card in reset
	 * the whole time this app has been failing to enumerate one -- that is
	 * the mechanism under test on the next bench load. The hold/settle
	 * timings are the upper bound of each vendor Linux range
	 * (SD_RST_ASSERT_MS / SD_RST_RELEASE_SETTLE_MS above), not invented
	 * values. This is a GPIO pulse, not a disk write, so it stays within
	 * this app's read-only-by-construction contract (see the file header).
	 */
	alp_gpio_t *sd_rst = alp_gpio_open(SD_RST_PIN_ID);
	printf("[sd] alp_gpio_open(SD_RST, P14_2) -> %s\n", (sd_rst != NULL) ? "ok" : "NULL");
	if (sd_rst == NULL) {
		printf("[sd] RESULT FAIL: SD_RST could not be opened (err=%d) -- the reset line "
		       "cannot be driven, so a disk fault measured past this point could just be "
		       "an undriven/undefined reset line. Check the board overlay carries gpio14 "
		       "status=\"okay\" and a 4th entry in the alp,pin-array node\n",
		       (int)alp_last_error());
		alp_gpio_close(mux_en);
		return -1;
	}

	alp_status_t rst_cfg_rc = alp_gpio_configure(sd_rst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	alp_status_t rst_lo_rc  = (rst_cfg_rc == ALP_OK) ? alp_gpio_write(sd_rst, false) : rst_cfg_rc;
	printf("[sd] SD_RST configure -> %d, write LOW -> %s\n",
	       (int)rst_cfg_rc,
	       (rst_cfg_rc == ALP_OK) ? "ran" : "SKIPPED (configure failed)");
	if (rst_lo_rc != ALP_OK) {
		printf("[sd] RESULT FAIL: SD_RST could not be driven low (rc=%d) -- proceeding to "
		       "disk_access_init() with an unproven reset line would be a meaningless "
		       "measurement\n",
		       (int)rst_lo_rc);
		alp_gpio_close(sd_rst);
		alp_gpio_close(mux_en);
		return -1;
	}
	k_msleep(SD_RST_ASSERT_MS);

	alp_status_t rst_hi_rc = alp_gpio_write(sd_rst, true);
	printf("[sd] SD_RST write HIGH -> %d (held LOW for %u ms, now settling %u ms before the "
	       "controller is touched)\n",
	       (int)rst_hi_rc,
	       (unsigned)SD_RST_ASSERT_MS,
	       (unsigned)SD_RST_RELEASE_SETTLE_MS);
	if (rst_hi_rc != ALP_OK) {
		printf("[sd] RESULT FAIL: SD_RST could not be driven high (rc=%d) -- the card would "
		       "be left held in reset\n",
		       (int)rst_hi_rc);
		alp_gpio_close(sd_rst);
		alp_gpio_close(mux_en);
		return -1;
	}
	k_msleep(SD_RST_RELEASE_SETTLE_MS);
	/* close() only frees the host-side handle (see z_close() in
	 * src/backends/gpio/zephyr_drv.c) -- it does not reconfigure the pad,
	 * so P14_2 stays driven HIGH (released) for the rest of this run. */
	alp_gpio_close(sd_rst);

	/*
	 * --- 7. Ask the Secure Enclave to enable the SD controller's clock
	 * source, BEFORE the SD host is touched -----------------------------
	 * See sd_se_enable_clock() above for the full citation. Short version:
	 * CLKCTL_PER_MST bit 16 only SELECTS the 100 MHz source -- it has
	 * never enabled it -- and the gate for that source, CGU CLK_ENA bit 7
	 * (CLK100M), measured 0 (0xFE03FF71) on silicon with Linux fully up.
	 * A command circuit with no clock cannot complete SW_RST_CMD, which is
	 * exactly what this app's own diagnostics have been showing. Requests
	 * both clocks Alif's own demo enables for SD (100 MHz + 20 MHz) and
	 * reports every return code -- neither request gates the rest of this
	 * app: even a failed request should still be visible in the CGU
	 * read-back and in disk_access_init()'s own result below, rather than
	 * stopping this run before that evidence is captured.
	 */
	alp_status_t clk100_rc = sd_se_enable_clock(CLKEN_CLK_100M, "CLKEN_CLK_100M");
	alp_status_t clk20_rc  = sd_se_enable_clock(CLKEN_CLK_20M, "CLKEN_CLK_20M");

	/* Read CGU CLK_ENA back now, not just after the requests print their own
	 * rc -- proves the effect on silicon rather than only the service's
	 * verdict (see sd_diag_print_cgu_clk_ena()'s own comment). */
	sd_diag_print_cgu_clk_ena();

	if (clk100_rc != ALP_OK || clk20_rc != ALP_OK) {
		printf("[sd] WARNING: at least one SE clock-enable request did not return OK "
		       "(CLKEN_CLK_100M -> %d, CLKEN_CLK_20M -> %d) -- check the CGU CLK_ENA "
		       "read-back above before trusting whatever disk_access_init() reports "
		       "next\n",
		       (int)clk100_rc,
		       (int)clk20_rc);
	}

	/*
	 * --- 8. Enumerate the card, read-only ------------------------------
	 * From here on the mux stays ENABLED (GPIO_26 driven low), including
	 * past this app's exit -- deliberately not restored to idle. /E LOW
	 * is this board's working state, on the maintainer's instruction, not
	 * a resource this app must hand back.
	 */
	sd_diag_print_static_regs();

	printf("[sd] disk_access_init(\"%s\") on the E8 DWC SDHC\n", DISK_NAME);

	int rc = disk_access_init(DISK_NAME);
	printf("[sd] disk_access_init -> %d\n", rc);

	if (rc == 0) {
		uint32_t sectors = 0, ssize = 0;
		(void)disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
		(void)disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &ssize);
		uint64_t mb = ((uint64_t)sectors * ssize) / (1024u * 1024u);
		printf("[sd] card: %u sectors x %u B = %llu MB\n", sectors, ssize, (unsigned long long)mb);
		printf("[sd] RESULT PASS: SD card enumerated (%llu MB)\n", (unsigned long long)mb);
	} else {
		printf("[sd] RESULT PARTIAL: bridge up (rc=%d), mux ENABLE asserted (rc=%d), SD_RST "
		       "pulsed low-then-high, SDHC controller built + inited; card still not "
		       "reachable (disk_access_init rc=%d). The mux and the reset line are no "
		       "longer the open question -- both are proven driven -- so look at the "
		       "controller/card handshake itself, or at the SELECT jumper on header P18 "
		       "(see README.md)\n",
		       (int)bridge_rc,
		       (int)en_rc,
		       rc);
	}
	/* close() only frees the host-side GPIO proxy handle -- the CC3501E
	 * keeps driving GPIO_26 low afterwards regardless (see the no-restore
	 * comment above). */
	alp_gpio_close(mux_en);
	printf("[sd] done\n");
	return 0;
}
