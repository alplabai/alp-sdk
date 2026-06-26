/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon I2C2 + EEPROM validation for the E1M-AEN801 (Alif Ensemble E8) over
 * the UPSTREAM DesignWare i2c_dw driver (ADR 0017 Tier-1, "snps,designware-i2c").
 *
 * What it proves
 * --------------
 * The SoC I2C2 master bus works end to end on silicon: bus arbitration, 7-bit
 * addressing, ACK detect, and a combined write-then-read (repeated-START)
 * transaction against the on-module 24C128 EEPROM.  Three steps:
 *
 *   1. SCAN the 7-bit address space 0x08..0x77 -- probe each address with a
 *      1-byte read and record which ones ACK.  Prints the populated addresses.
 *   2. confirm 0x50 (the 24C128's standard 7-bit address) is among them.
 *   3. read 16 bytes from EEPROM offset 0 via i2c_write_read(dev, 0x50,
 *      {0x00,0x00} (big-endian 2-byte word address), 2, buf, 16) and hexdump.
 *
 * The EEPROM reaches I2C2 (not the slave-only LPI2C0) via bridge/DNP resistors on
 * P5_6 SCL_C / P5_7 SDA_C -- the established board finding; see the overlay.
 *
 * PASS criteria: device ready AND 0x50 found in the scan AND i2c_write_read()
 * returns 0.  Content is NOT checked: a BLANK 24C128 reads all-0xff (coherent),
 * which is a PASS -- this test proves the bus + addressing + read PATH, not the
 * stored manifest.  (aen-eeprom-manifest decodes the content.)
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

/* i2c2 == i2c@49012000 (snps,designware-i2c), okay'd + pinctrl'd by the overlay. */
#define I2C2_NODE DT_NODELABEL(i2c2)

/* 24C128 standard 7-bit address. */
#define EEPROM_ADDR 0x50U

/* General-call (0x00..0x07) and reserved-high (0x78..0x7f) addresses are skipped
 * by the conventional scan window, matching Zephyr's i2c_scanner sample. */
#define SCAN_LO 0x08U
#define SCAN_HI 0x77U

static const struct device *const i2c2 = DEVICE_DT_GET(I2C2_NODE);

/* xxd-style 16-byte hexdump (single line here; len is 16). */
static void hex_dump(const uint8_t *buf, size_t len)
{
	printk("  0000  ");
	for (size_t i = 0; i < len; i++) {
		printk("%02x ", buf[i]);
	}
	printk(" |");
	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];

		printk("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
	}
	printk("|\n");
}

int main(void)
{
	int          rc;
	uint8_t      dummy;
	bool         found_0x50 = false;
	unsigned int n_found    = 0U;

	printk("\n=== AEN801 I2C2 + EEPROM bench (i2c_dw / i2c2 @ 0x49012000) ===\n");

	/* 1. device readiness. */
	if (!device_is_ready(i2c2)) {
		printk("RESULT FAIL: i2c2 device not ready\n");
		return 0;
	}
	printk("i2c2 device ready\n");

	/* 2. bus scan: probe each 7-bit address with a 1-byte read; an ACK (rc==0)
	 *    means a device sits there.  A 1-byte read is the most portable probe
	 *    for i2c_dw -- a zero-length write is not universally honoured. */
	printk("scanning 0x%02x..0x%02x ...\n", SCAN_LO, SCAN_HI);
	for (uint16_t addr = SCAN_LO; addr <= SCAN_HI; addr++) {
		rc = i2c_read(i2c2, &dummy, 1U, addr);
		if (rc == 0) {
			printk("  ACK @ 0x%02x\n", addr);
			n_found++;
			if (addr == EEPROM_ADDR) {
				found_0x50 = true;
			}
		}
	}
	printk("scan done: %u device(s) responded; 0x50 %s\n",
	       n_found,
	       found_0x50 ? "PRESENT" : "MISSING");

	if (!found_0x50) {
		printk("RESULT FAIL: 24C128 @ 0x50 not found on I2C2 "
		       "(EEPROM populated?  bridge/DNP selecting I2C2?  pinctrl?)\n");
		return 0;
	}

	/* 3. read 16 bytes from EEPROM offset 0.  The 24C128 takes a 2-byte
	 *    BIG-ENDIAN word address; i2c_write_read issues write(addr)+repeated-
	 *    START+read in one transaction. */
	uint8_t word_addr[2] = { 0x00U, 0x00U };
	uint8_t buf[16];

	rc = i2c_write_read(i2c2, EEPROM_ADDR, word_addr, sizeof(word_addr), buf, sizeof(buf));
	printk("i2c_write_read(0x50, off=0, 16B) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: i2c_write_read rc=%d\n", rc);
		return 0;
	}

	printk("EEPROM[0..15]:\n");
	hex_dump(buf, sizeof(buf));

	/* 4. verdict: bus + addressing + read path all worked.  All-0xff (blank
	 *    EEPROM) is still a PASS -- the test proves the PATH, not the content. */
	printk("RESULT PASS: I2C2 master scan found 0x50 and read 16B "
	       "(bus + addressing + repeated-START read OK; content not checked)\n");

	return 0;
}
