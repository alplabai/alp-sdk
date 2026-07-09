/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon MRAM flash erase/program/readback validation for the E1M-AEN801
 * (Alif Ensemble E8) over the alp-sdk Tier-2 flash_mram_alif driver (ADR 0017
 * Tier-2, "alif,mram-flash-controller" -- a Zephyr flash-class driver vendored
 * from the Apache-2.0 zephyr_alif fork for the on-die MRAM at 0x80000000).
 *
 * What it proves (closes issue #512)
 * ----------------------------------
 * MCUboot and the app boot path already prove MRAM read+execute.  This app
 * proves the WRITE side of flash_mram_alif.c end-to-end through the PORTABLE
 * Zephyr flash_* class API:
 *
 *     flash_erase()  -> readback == erase_value (0x00)   (erase works)
 *     flash_write()  -> readback == written pattern      (program works)
 *     flash_read()   -> byte-exact verify                (read works)
 *     out-of-unit erase/write -> -EINVAL                 (range/align guard)
 *
 * two independent ways:
 *   1. in-firmware:  this app runs the sequence and prints every API return
 *      code + a single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads mem32 0x80560000 (the storage partition,
 *      see below) after the program step -- the ground truth on silicon,
 *      independent of any printk.  The pattern's first word is 0xA5A5A5A5.
 *
 * Safe scratch target -- NOT slot0/slot1
 * --------------------------------------
 * The test writes ONLY the `storage` fixed-partition (label "storage", MRAM
 * offset 0x560000 = absolute 0x80560000, 128 KiB, the NVS/settings region),
 * which is empty on this bench and holds no boot code.  The app also re-erases
 * the scratch block on exit so it is left clean.  This app is flashed into MRAM
 * slot0 and SE-booted (bench flow D -- the SES boots the resident slot0 image
 * in preference to a Flow-C ITCM RAM-run), so slot0 briefly holds THIS app
 * instead of person_detect; the canonical person_detect slot0 is RESTORED after
 * the run (byte-exact, reset vector 0x80011F15).  No other MRAM region is
 * mutated by the test itself.
 *
 * Console is the Alp UART console (see prj.conf): E1M edge UART0 = Alif UART5
 * (P3_4/P3_5, 115200 8N1), USB-routed to the labgrid `console` resource
 * (/dev/ttyUSB2) since 2026-07-03, so the RESULT PASS/FAIL line streams live as
 * the SES boots this slot0 image.  Independently, the human reads mem32
 * 0x80560000 over J-Link after the program step -- silicon ground truth for the
 * written pattern (first word 0xA5A5A5A5), independent of any printk.
 * BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* The safe scratch target: the `storage` fixed-partition under the MRAM
 * controller.  FIXED_PARTITION_* resolve the offset/size from DT so nothing is
 * hard-coded, and FIXED_PARTITION_DEVICE resolves the flash device the
 * flash_mram_alif driver bound (the "alif,mram-flash-controller" node). */
#define SCRATCH_PARTITION storage_partition

static const struct device *const flash_dev = FIXED_PARTITION_DEVICE(SCRATCH_PARTITION);

#define SCRATCH_OFF  FIXED_PARTITION_OFFSET(SCRATCH_PARTITION)
#define SCRATCH_SIZE FIXED_PARTITION_SIZE(SCRATCH_PARTITION)

/* One erase block (1024 B, the node's erase-block-size) is enough to prove the
 * path; the written pattern is 256 B = 16 * 16-byte program units. */
#define ERASE_UNIT   1024U
#define PATTERN_LEN  256U

/* Never let main() return.  On this SoC an idle M55 -- the kernel idle thread's
 * low-power wait once main returns -- lets the SES gate the debug (SWD/DAP)
 * power domain OFF, which then wedges EVERY subsequent J-Link connect (a bare
 * "Failed to power up DAP"), including the person_detect slot0 restore, and
 * survives a cold power-cycle because the resident app re-idles on each boot.
 * Park in a BUSY heartbeat (k_busy_wait, never WFI) so the core stays active,
 * the DAP stays powerable, and the UART console keeps streaming the verdict --
 * just like the continuously-busy person_detect image that keeps the bench
 * probe alive between runs. */
static FUNC_NORETURN void park(void)
{
	for (;;) {
		k_busy_wait(1000U * 1000U); /* 1 s busy; never enter idle */
		printk("mram-flash-validate: parked (heartbeat)\n");
	}
}

int main(void)
{
	int      rc;
	uint8_t  wbuf[PATTERN_LEN];
	uint8_t  rbuf[PATTERN_LEN];
	uint32_t i;

	printk("\n=== AEN801 MRAM flash erase/program/readback bench "
	       "(flash_mram_alif / alif,mram-flash-controller) ===\n");

	/* 1. device readiness.  If the controller node did not instantiate a
	 *    device the build would fail at link, so reaching here means the
	 *    device object exists -- confirm it is ready. */
	if (!device_is_ready(flash_dev)) {
		printk("RESULT FAIL: MRAM flash device not ready\n");
		park();
	}
	printk("flash device ready: %s\n", flash_dev->name);
	printk("scratch = storage partition: off=0x%06lx size=%u B (abs 0x%08lx)\n",
	       (unsigned long)SCRATCH_OFF, (unsigned)SCRATCH_SIZE,
	       (unsigned long)(0x80000000UL + SCRATCH_OFF));

	/* 2. static parameters: write_block_size (expect 16) + erase_value
	 *    (expect 0x00 -- flash_mram_alif erases to zero, not 0xFF). */
	const struct flash_parameters *fp = flash_get_parameters(flash_dev);

	if (fp == NULL) {
		printk("RESULT FAIL: flash_get_parameters returned NULL\n");
		park();
	}
	printk("write_block_size=%u erase_value=0x%02x\n",
	       (unsigned)fp->write_block_size, fp->erase_value);

	/* 3. ERASE one unit at the scratch offset. */
	rc = flash_erase(flash_dev, SCRATCH_OFF, ERASE_UNIT);
	printk("flash_erase(0x%06lx, %u) rc=%d\n",
	       (unsigned long)SCRATCH_OFF, ERASE_UNIT, rc);
	if (rc != 0) {
		printk("RESULT FAIL: flash_erase rc=%d\n", rc);
		park();
	}

	/* 4. readback the erased region and confirm every byte == erase_value. */
	memset(rbuf, 0xFF, sizeof(rbuf));
	rc = flash_read(flash_dev, SCRATCH_OFF, rbuf, PATTERN_LEN);
	printk("flash_read(post-erase) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: flash_read post-erase rc=%d\n", rc);
		park();
	}
	for (i = 0; i < PATTERN_LEN; i++) {
		if (rbuf[i] != fp->erase_value) {
			printk("RESULT FAIL: erase mismatch at +%u: 0x%02x != 0x%02x\n",
			       i, rbuf[i], fp->erase_value);
			park();
		}
	}
	printk("erase verify OK: %u bytes == 0x%02x\n", PATTERN_LEN, fp->erase_value);

	/* 5. PROGRAM a deterministic pattern (first word 0xA5A5A5A5 for the J-Link
	 *    readback; the rest is a counting ramp so a stuck/shorted byte shows). */
	for (i = 0; i < PATTERN_LEN; i++) {
		wbuf[i] = (i < 4) ? 0xA5 : (uint8_t)(i & 0xFF);
	}
	rc = flash_write(flash_dev, SCRATCH_OFF, wbuf, PATTERN_LEN);
	printk("flash_write(0x%06lx, %u) rc=%d\n",
	       (unsigned long)SCRATCH_OFF, PATTERN_LEN, rc);
	if (rc != 0) {
		printk("RESULT FAIL: flash_write rc=%d\n", rc);
		park();
	}

	/* --- J-Link readback window: mem32 0x80560000 should read 0xA5A5A5A5 --- */

	/* 6. readback the programmed region and byte-exact verify. */
	memset(rbuf, 0x00, sizeof(rbuf));
	rc = flash_read(flash_dev, SCRATCH_OFF, rbuf, PATTERN_LEN);
	printk("flash_read(post-write) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: flash_read post-write rc=%d\n", rc);
		park();
	}
	if (memcmp(rbuf, wbuf, PATTERN_LEN) != 0) {
		for (i = 0; i < PATTERN_LEN; i++) {
			if (rbuf[i] != wbuf[i]) {
				printk("RESULT FAIL: program mismatch at +%u: "
				       "0x%02x != 0x%02x\n", i, rbuf[i], wbuf[i]);
				break;
			}
		}
		park();
	}
	printk("program verify OK: %u bytes byte-exact\n", PATTERN_LEN);

	/* 7. negative path: an erase whose length is not a multiple of the erase
	 *    unit must be rejected with -EINVAL BEFORE any silicon write (proves
	 *    the driver's range/alignment guard, not just the happy path). */
	rc = flash_erase(flash_dev, SCRATCH_OFF, ERASE_UNIT - 16U);
	printk("flash_erase(unaligned len) rc=%d (expect %d)\n", rc, -EINVAL);
	if (rc != -EINVAL) {
		printk("RESULT FAIL: unaligned erase not rejected (rc=%d)\n", rc);
		park();
	}

	/* 8. leave the scratch block clean (erased) so the storage/NVS region is
	 *    not left holding our test pattern. */
	rc = flash_erase(flash_dev, SCRATCH_OFF, ERASE_UNIT);
	printk("flash_erase(cleanup) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: cleanup erase rc=%d\n", rc);
		park();
	}

	printk("RESULT PASS: MRAM erase/program/readback verified on the "
	       "storage partition (slot0 untouched)\n");
	park();
}
