/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-spi-regcheck -- scopeless on-silicon validation of the Alif DWC_ssi SPI
 * driver (zephyr/drivers/spi/spi_dw_alif.c, compatible "alif,dwc-ssi-spi",
 * spi0 @ 0x48103000) on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench
 * RAM-run + RAM-console flow.
 *
 * We cannot see the SPI pins on this bench (no scope, app UART not on USB). So
 * we validate by driving the standard Zephyr spi_transceive() API and then doing
 * a REGISTER READBACK of the controller's CTRLR0 / SSIENR / BAUDR / SR.
 *
 * The transfer is configured as an INTERNAL LOOPBACK (SPI_MODE_LOOP -> the
 * DWC_ssi SRL bit, CTRLR0[13]): the controller ties its own TX shift-register to
 * its RX shift-register, so a full-duplex transceive echoes TX bytes back into
 * the RX buffer with NO external wire. That gives us a real data check on this
 * scopeless bench. (For a PIN-level loopback instead, jumper P5_1 MOSI -> P5_0
 * MISO and clear SPI_MODE_LOOP -- see the readback/unknowns notes.)
 *
 * Two independent confirmations, exactly like aen-pwm-utimer-regcheck:
 *   1. This firmware drives the API, prints the tx/rx bytes + the four control
 *      registers it reads back, plus a single RESULT PASS/FAIL line, to the RAM
 *      console (read 'ram_console_buf' over J-Link mem8, ASCII-decode).
 *   2. The human re-reads the SAME absolute addresses over J-Link mem32 (see the
 *      readback plan) -- so a driver that only PRINTS the right thing is caught.
 *
 * CTRLR0 expected value derivation (clock-INDEPENDENT, the strict PASS gate):
 * word size 8, SPI mode 0, master, loopback, dwc_ssi layout. From
 * spi_dw_configure() + transceive() in spi_dw_alif.c:
 *   - DFS (max_xfer_size default 16 -> DW_SPI_CTRLR0_DFS_16(8) = 8-1) = 0x7  (bits[3:0])
 *   - master + dwc_ssi -> DWC_SSI_SPI_IS_MST_BIT = BIT(31)                   = 0x80000000
 *   - mode 0 -> no SCPOL(bit9)/SCPH(bit8)
 *   - SPI_MODE_LOOP -> DWC_SSI_SPI_CTRLR0_SRL = BIT(13)                      = 0x00002000
 *   - TMOD: tx+rx bufs both present -> DW_SPI_CTRLR0_TMOD_TX_RX = 0 (bits[11:10]=00)
 *   - SSTE not set (slv-slct-toggle absent)
 *   => CTRLR0 == 0x80002007
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>

#define SPI_NODE DT_NODELABEL(spi0)

/* Absolute reg base straight from the dtsi reg = <0x48103000 0x1000>, pulled
 * from devicetree so this stays correct if the node ever moves. */
#define SPI_BASE ((uint32_t)DT_REG_ADDR(SPI_NODE))

/* DWC_ssi register offsets -- VERBATIM from spi_dw_alif_regs.h. */
#define OFF_CTRLR0 0x00U
#define OFF_CTRLR1 0x04U
#define OFF_SSIENR 0x08U
#define OFF_BAUDR  0x14U
#define OFF_SR     0x28U

/* Expected CTRLR0 after configure for 8-bit, mode-0, master, loopback (see the
 * file header derivation). Clock-independent. */
#define EXP_CTRLR0 0x80002007U

/* SR bits (spi_dw_alif.h): bit0 BUSY, bit1 TFNF (tx fifo not full),
 * bit3 RFNE (rx fifo not empty). After completed() the controller is disabled
 * and idle, so we expect BUSY=0. */
#define SR_BUSY_BIT 0U

/* BAUDR is clk_freq / requested-freq. With the overlay's clock-frequency=100MHz
 * and a 1 MHz transfer, expect 100 (0x64). Clock-rate is a bench unknown, so
 * BAUDR is REPORTED, not part of the strict PASS gate. */
#define TEST_FREQ_HZ      1000000U
#define EXP_BAUDR_AT_100M 100U /* 100e6 / 1e6 */

static inline uint32_t rd(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

int main(void)
{
	const struct device *spi = DEVICE_DT_GET(SPI_NODE);

	printk("\n=== aen-spi-regcheck ===\n");
	printk("spi node   : %s\n", DT_NODE_FULL_NAME(SPI_NODE));
	printk("spi_base   : 0x%08x\n", SPI_BASE);

	if (!device_is_ready(spi)) {
		printk("RESULT FAIL: spi device not ready (init/pinctrl/clock failed)\n");
		return 0;
	}

	/* 8-bit, MSB-first, SPI mode 0, master, INTERNAL loopback (SRL). */
	struct spi_config cfg = {
		.frequency = TEST_FREQ_HZ,
		.operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8) | SPI_MODE_LOOP,
		.slave     = 0,     /* HW SS0 (SS0_B on P5_2) */
		.cs        = { 0 }, /* no cs-gpios; controller SER drives SS */
	};

	uint8_t tx[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
	uint8_t rx[4] = { 0, 0, 0, 0 };

	const struct spi_buf     txb    = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf     rxb    = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set tx_set = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rxb, .count = 1 };

	printk("tx bytes   : %02x %02x %02x %02x\n", tx[0], tx[1], tx[2], tx[3]);
	printk("freq=%u Hz, ws=8, mode=0, master, loopback (SRL)\n", TEST_FREQ_HZ);

	int rc = spi_transceive(spi, &cfg, &tx_set, &rx_set);

	printk("spi_transceive rc = %d\n", rc);
	printk("rx bytes   : %02x %02x %02x %02x\n", rx[0], rx[1], rx[2], rx[3]);

	/*
	 * Register readback. spi_dw_alif's completed() disables the controller
	 * (clears SSIENR) at the end of a transfer, so:
	 *   - CTRLR0 retains the last-programmed value (the configure value) -- the
	 *     decisive, clock-independent evidence the driver programmed the IP.
	 *   - SSIENR reads 0 (controller disabled / idle after completion) -- expected.
	 *   - SR.BUSY reads 0 (idle).
	 *   - BAUDR retains the computed divider (reported, clock-dependent).
	 */
	uint32_t ctrlr0 = rd(SPI_BASE, OFF_CTRLR0);
	uint32_t ctrlr1 = rd(SPI_BASE, OFF_CTRLR1);
	uint32_t ssienr = rd(SPI_BASE, OFF_SSIENR);
	uint32_t baudr  = rd(SPI_BASE, OFF_BAUDR);
	uint32_t sr     = rd(SPI_BASE, OFF_SR);

	printk("-- readback --\n");
	printk("CTRLR0  0x%08x = 0x%08x (exp 0x%08x)\n", SPI_BASE + OFF_CTRLR0, ctrlr0, EXP_CTRLR0);
	printk("CTRLR1  0x%08x = 0x%08x (NDF; full-duplex path writes 0)\n",
	       SPI_BASE + OFF_CTRLR1,
	       ctrlr1);
	printk("SSIENR  0x%08x = 0x%08x (exp 0x0 after completion: ctrl disabled)\n",
	       SPI_BASE + OFF_SSIENR,
	       ssienr);
	printk("BAUDR   0x%08x = 0x%08x (exp %u @100MHz src; clk-rate is a bench unknown)\n",
	       SPI_BASE + OFF_BAUDR,
	       baudr,
	       EXP_BAUDR_AT_100M);
	printk("SR      0x%08x = 0x%08x (bit0 BUSY exp 0)\n", SPI_BASE + OFF_SR, sr);

	/* Loopback data check: with SRL set, rx must echo tx exactly. */
	bool data_ok = (rx[0] == tx[0]) && (rx[1] == tx[1]) && (rx[2] == tx[2]) && (rx[3] == tx[3]);

	/*
	 * PASS gate:
	 *   - spi_transceive returned 0 (no controller/clock error),
	 *   - CTRLR0 programmed exactly as derived (clock-independent),
	 *   - controller is idle (SR.BUSY clear),
	 *   - internal-loopback echo matched (real data path through the IP).
	 * BAUDR is reported only (clock-rate is a bench unknown), and BAUDR != 0 is
	 * folded in as a sanity check that the divider was actually written.
	 */
	bool ok = true;

	ok &= (rc == 0);
	ok &= (ctrlr0 == EXP_CTRLR0);
	ok &= ((sr & (1U << SR_BUSY_BIT)) == 0U);
	ok &= (baudr != 0U);
	ok &= data_ok;

	if (ok) {
		printk("RESULT PASS: spi0 CTRLR0=0x%08x baudr=%u, transceive rc=0, "
		       "loopback echo OK (rx==tx)\n",
		       ctrlr0,
		       baudr);
	} else {
		printk("RESULT FAIL: rc=%d ctrlr0=0x%08x(exp 0x%08x) "
		       "busy=%u baudr=%u loopback=%s\n",
		       rc,
		       ctrlr0,
		       EXP_CTRLR0,
		       (unsigned)(sr & 0x1U),
		       baudr,
		       data_ok ? "OK" : "MISMATCH");
	}

	return 0;
}
