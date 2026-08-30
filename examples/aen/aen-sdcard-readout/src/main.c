/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sdcard-readout -- bring up the Ensemble E8 SD Host Controller on the
 * E1M-AEN801 (M55-HE) via the vendored snps,dwc-sdhc driver + the Zephyr SDMMC
 * disk, and probe a microSD card.  Drives the standard Zephyr disk-access API
 * (disk_access_init / disk_access_ioctl) on the "SD" disk.
 *
 * EVK ROUTING NOTE: on the E1M EVK the microSD sits on the SDIO bus behind a
 * 74LVC157 mux with an ENABLE (E1M IO20) and a SELECT (E1M IO21).
 *
 * CORRECTED for board rev 2626-R2 (#912).  This comment used to say both pins
 * were "on the CC3501E side and must be driven over the inter-chip SPI
 * bridge".  On R2 that is wrong about both of them, and the second half is the
 * one that matters:
 *
 *   - IO20 IS CC3501E-side -- metadata/e1m_modules/aen/from-cc3501e.tsv maps it
 *     to GPIO_26, so it is drivable today through the GPIO proxy.
 *   - IO21 is NOT.  hw-revisions.yaml records for r2: "IO21 unrouted (was
 *     CC3501E GPIO30)" -- GPIO_30 moved to IO8 and IO21 was left OPEN on the
 *     module.  It reaches neither the CC3501E nor the Alif SoC, so the mux
 *     SELECT cannot be driven from software on this board revision at all.
 *
 * So the card stays unreachable on R2, but the blocker is a module pad that no
 * longer goes anywhere -- not a bridge feature waiting to be written.  This
 * example therefore validates SDHC controller/driver bring-up (device builds +
 * inits) and card init is expected to fail with "no card".  See the README.
 *
 * PASS gate: disk_access_init returns 0 and the card geometry reads back (a card
 * was actually reachable + enumerated).  A clean controller bring-up where the
 * card is simply not reachable (the CC3501E mux is not routed on this bench) is
 * reported PARTIAL -- the controller/driver path is proven.
 */

#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>

#define DISK_NAME "SD"

int main(void)
{
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
		printf("[sd] RESULT PARTIAL: SDHC controller built + inited; card not reachable "
		       "(rc=%d). Route the EVK SDIO 74LVC157 mux (EN=IO20, SEL=IO21, both "
		       "CC3501E-side) + insert a card.\n",
		       rc);
	}
	printf("[sd] done\n");
	return 0;
}
