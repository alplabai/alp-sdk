/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sdcard-file -- write, read back and list a file on the E1M-EVK microSD
 * card (slot J7) with FatFs, from the Alif Ensemble E8 M55-HE.
 *
 * ==== THE PATH FROM main() TO THE CARD ==============================
 *
 *   SoC SD host (sdhc0, SD "B" pads P13_0..3 / P14_0 / P14_1)
 *     -> carrier SDIO mux U38/U39 (select strapped to the microSD slot by
 *        header P18 open; enable /E = E1M IO20, active low)
 *     -> microSD card in J7
 *
 * The e1m_evk_microsd shield (added in CMakeLists.txt) turns sdhc0 on and
 * declares the card as the Zephyr disk "SD". `libraries: [fatfs]` in
 * board.yaml selects FatFs. So the only thing this app must do by hand is
 * open the mux.
 *
 * ==== WHY THE CC3501E IS INVOLVED ===================================
 *
 * On the E1M-AEN SoMs the E1M IO20 pad is wired to the on-module CC3501E,
 * not to an Alif pin. With CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y a plain
 * alp_gpio_open(ALP_E1M_GPIO_IO20) reaches it over the inter-chip SPI
 * bridge -- but only once the bridge is up. So the order is:
 *
 *   1. cc3501e_bridge_bringup()   power + reset the CC3501E, PING it
 *   2. IO20 low                   mux on: the card is now wired to sdhc0
 *   3. fs_mount("/SD:")           first time the SD stack talks to the card
 *
 * The SD host driver never touches the card before step 3, but that only
 * orders the SD *traffic*; it does not make the carrier safe (see below).
 *
 * ==== CARRIER REQUIREMENT: REWORKED U38/U39 =========================
 *
 * Run this ONLY on a carrier whose U38/U39 are the reworked 74LV3257 bus
 * switches. The stock 74LVC157 is unidirectional and always drives its Y
 * outputs (the selected input when /E is low, forced LOW when /E is high),
 * and those Y pins sit on the same nets as the SoC's own SD pads. So the
 * stock part fights the SoC's SD drivers on EITHER /E state -- leaving the
 * mux disabled is NOT safe (see include/alp/boards/alp_e1m_evk.h, "SD ...
 * CAN NEVER WORK through the stock 74LVC157"). This example has no build
 * or runtime gate for that: knowing your carrier is your responsibility.
 *
 * ==== DMA AND DTCM ==================================================
 *
 * The buffers below are ordinary statics, so they land in the M55's DTCM.
 * The SD host's DMA engine cannot reach DTCM; the driver notices and copies
 * through a buffer in SRAM0 (CONFIG_SDHC_DWC_BOUNCE_BUFFER), so nothing here
 * needs a special section.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <ff.h>

#include <alp/e1m_pinout.h>
#include <alp/peripheral.h>

#include "cc3501e_bridge.h"

#define MNT       "/SD:"
#define FILE_PATH MNT "/alp_sdcard_file.txt"

static FATFS             fat;
static struct fs_mount_t mnt = {
	.type      = FS_FATFS,
	.mnt_point = MNT,
	.fs_data   = &fat,
};

static char wbuf[4096];
static char rbuf[sizeof(wbuf)];

/* Steps 1 and 2: bridge up, then drive the SDIO mux enable low. */
static int sd_mux_enable(void)
{
	static cc3501e_t fw;
	alp_status_t     s = cc3501e_bridge_bringup(&fw);

	printf("[sd] CC3501E bridge bring-up -> %d\n", (int)s);
	if (s != ALP_OK) {
		return -1;
	}

	alp_gpio_t *en = alp_gpio_open(ALP_E1M_GPIO_IO20);

	if (en == NULL) {
		printf("[sd] IO20 open failed\n");
		return -1;
	}
	if (alp_gpio_configure(en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) != ALP_OK ||
	    alp_gpio_write(en, false) != ALP_OK) {
		printf("[sd] IO20 drive low failed\n");
		return -1;
	}
	printf("[sd] SDIO mux enabled (IO20 low)\n");
	return 0;
}

/* Write a file, sync it to the card, read it back and compare. */
static int write_read_back(void)
{
	struct fs_file_t f;
	ssize_t          n;
	int              rc;

	for (size_t i = 0; i < sizeof(wbuf); i++) {
		wbuf[i] = (char)('a' + i % 26);
	}

	fs_file_t_init(&f);
	rc = fs_open(&f, FILE_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		printf("[fs] open for write -> %d\n", rc);
		return rc;
	}
	n = fs_write(&f, wbuf, sizeof(wbuf));
	/* fs_sync() is what makes the data survive a power cut: without it the
	 * last cluster and the directory entry may still sit in FatFs's cache.
	 */
	rc = fs_sync(&f);
	fs_close(&f);
	printf("[fs] wrote %d B, sync -> %d\n", (int)n, rc);
	if (n != (ssize_t)sizeof(wbuf) || rc != 0) {
		return -1;
	}

	fs_file_t_init(&f);
	rc = fs_open(&f, FILE_PATH, FS_O_READ);
	if (rc != 0) {
		printf("[fs] open for read -> %d\n", rc);
		return rc;
	}
	n = fs_read(&f, rbuf, sizeof(rbuf));
	fs_close(&f);

	bool same = n == (ssize_t)sizeof(rbuf) && memcmp(wbuf, rbuf, sizeof(rbuf)) == 0;

	printf("[fs] read %d B, match=%d\n", (int)n, same);
	return same ? 0 : -1;
}

static void list_root(void)
{
	struct fs_dir_t  dir;
	struct fs_dirent ent;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, MNT) != 0) {
		printf("[fs] opendir failed\n");
		return;
	}
	printf("[fs] %s contents:\n", MNT);
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		printf("  %s %-32s %u\n",
		       ent.type == FS_DIR_ENTRY_DIR ? "d" : "f",
		       ent.name,
		       (unsigned int)ent.size);
	}
	fs_closedir(&dir);
}

static void report_space(void)
{
	struct fs_statvfs sv;

	if (fs_statvfs(MNT, &sv) == 0) {
		uint64_t total = (uint64_t)sv.f_frsize * sv.f_blocks;
		uint64_t free  = (uint64_t)sv.f_frsize * sv.f_bfree;

		printf("[fs] %u MiB free of %u MiB\n",
		       (unsigned int)(free >> 20),
		       (unsigned int)(total >> 20));
	}
}

int main(void)
{
	int rc;

	if (sd_mux_enable() != 0) {
		printf("RESULT FAIL: mux\n");
		return 0;
	}

	rc = fs_mount(&mnt);
	printf("[fs] mount %s -> %d\n", MNT, rc);
	if (rc != 0) {
		printf("RESULT FAIL: mount\n");
		return 0;
	}

	rc = write_read_back();
	list_root();
	report_space();

	/* Leave the card clean: the example's file is not user data. */
	fs_unlink(FILE_PATH);
	fs_unmount(&mnt);

	printf("RESULT %s: file\n", rc == 0 ? "PASS" : "FAIL");
	return 0;
}
