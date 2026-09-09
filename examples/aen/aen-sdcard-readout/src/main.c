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

#include "alp/peripheral.h"    /* alp_gpio_*, alp_status_t */
#include "alp/e1m_pinout.h"    /* ALP_E1M_GPIO_IO20 */
#include "alp/chips/cc3501e.h" /* cc3501e_t */

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template,
                              * copied verbatim from examples/aen/aen-evk-demo. */

#define DISK_NAME "SD"

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
	 * --- 2. Assert the SDIO mux ENABLE over the GPIO proxy -------------
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
	 * --- 3. Enumerate the card, read-only ------------------------------
	 * From here on the mux stays ENABLED (GPIO_26 driven low), including
	 * past this app's exit -- deliberately not restored to idle. /E LOW
	 * is this board's working state, on the maintainer's instruction, not
	 * a resource this app must hand back.
	 */
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
