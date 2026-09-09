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
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/sys_io.h> /* sys_read32() -- raw diagnostic register reads, see below */

#include "alp/peripheral.h"    /* alp_gpio_*, alp_status_t */
#include "alp/e1m_pinout.h"    /* ALP_E1M_GPIO_IO20 */
#include "alp/chips/cc3501e.h" /* cc3501e_t */

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template,
                              * copied verbatim from examples/aen/aen-evk-demo. */

#define DISK_NAME "SD"

/*
 * ============================================================================
 * SD register-level diagnostics (#2035) -- SW_RST_CMD investigation
 * ============================================================================
 * The card still does not enumerate even with the bridge/mux proven driven
 * (see the file header) and `disk_access_init` returns -116: `SW_RST_R` at
 * 0x4810202F stays 0x02, i.e. SW_RST_CMD never self-clears while SW_RST_DAT
 * does. Everything read on the bench so far was sampled AFTER the driver's
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

/* Bounded PING retry after bridge bring-up, before the first proxied request
 * (#2035 GPIO-proxy race) -- same parameters as aen-evk-demo's phase 8
 * (examples/aen/aen-evk-demo/src/main.c: CC35_PING_RETRIES/CC35_PING_GAP_MS),
 * which measured needing 11 of 25 attempts, 200 ms apart, on this silicon. */
#define CC35_PING_RETRIES 25u
#define CC35_PING_GAP_MS  200u

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
	 * --- 3. Assert the SDIO mux ENABLE over the GPIO proxy -------------
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
	 * --- 4. Enumerate the card, read-only ------------------------------
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
		printf("[sd] RESULT PARTIAL: bridge up (rc=%d), mux ENABLE asserted (rc=%d), SDHC "
		       "controller built + inited; card still not reachable (disk_access_init "
		       "rc=%d). The mux is no longer the open question -- it is proven driven --"
		       " so look at the controller/card handshake itself, or at the SELECT "
		       "jumper on header P18 (see README.md)\n",
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
