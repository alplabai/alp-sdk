/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-uart-ns16550-loopback -- scopeless on-silicon validation of the upstream
 * ns16550 UART driver (zephyr/drivers/serial/uart_ns16550.c) on the E1M-AEN801
 * (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * Target UART: Alif uart3 @ 0x4901b000 (IRQ 127), the spare instance -- NOT
 * uart5 (the E1M carrier console) and NOT uart2 (the DevKit console).  Pins
 * P1_2 = UART3_RX_A, P1_3 = UART3_TX_A (see app.overlay).
 *
 * We cannot see the UART pin on this bench (no scope; app UART not on USB), so
 * we validate two independent ways:
 *
 *   A. FUNCTIONAL round-trip.  The overlay sets `loopback;`, so the driver
 *      programs the 16550 MCR LOOP bit and TXD is internally fed back to RXD
 *      inside the UART.  We drive the PUBLIC Zephyr API -- uart_poll_out() to
 *      TX a known string, uart_poll_in() to RX it -- and byte-compare.  This
 *      exercises the real driver TX FIFO/THR + RX RDR path, not just register
 *      pokes.  (No jumper wire needed; see "unknowns" for the physical-wire
 *      variant.)
 *
 *   B. REGISTER readback.  We re-read the 16550 divisor latch (DLL/DLH) and the
 *      line-control register (LCR) to confirm the driver computed the correct
 *      115200 divisor and the 8N1 framing.  Reading DLL/DLH requires toggling
 *      LCR.DLAB; we set it, read, then RESTORE LCR exactly so the live UART is
 *      not left mis-configured.  The human re-reads the SAME absolute addresses
 *      over J-Link mem32 (see readbackPlan) -- so a driver that only *prints*
 *      the right thing is still caught.
 *
 * Clock / divisor derivation (stated so the expected values are auditable):
 *   uart3 has NO `clock-frequency` DT prop, so uart_ns16550.c takes the
 *   clock_control_get_rate() path on clockctrl with subsys ALIF_UART3_SYST_PCLK.
 *   That clock-id's parent is ALIF_PARENT_CLK_SYST_PCLK -> syst_pclk node ->
 *   DT_FREQ_M(100) = 100000000 Hz (ensemble_common.dtsi).
 *   The driver's divisor = ((pclk + (baud<<3)) / baud) >> 4
 *     = ((100000000 + 115200*8) / 115200) >> 4 = (100921600/115200)>>4
 *     = 876 >> 4 = 54 = 0x36.
 *   So DLL=0x36, DLH=0x00.  Actual baud = 100e6/(16*54) = 115740 (+0.47%, in tol).
 *   LCR for 8N1, DLAB clear = LCR_CS8(0x03) = 0x03.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#define UART_NODE DT_NODELABEL(uart3)

/* Absolute base straight from the dtsi reg cell, so it stays correct if the
 * node ever moves.  uart3: reg = <0x4901b000 0x1000>. */
#define UART_BASE ((uint32_t)DT_REG_ADDR(UART_NODE))

/* ns16550 register byte offsets.  reg-shift=2 -> reg_interval = 1<<2 = 4 bytes,
 * so logical register N lives at byte offset N*4 (uart_ns16550.c REG_* * 4):
 *   REG_BRDL/THR/RDR = 0 -> 0x00   (DLL when DLAB=1)
 *   REG_BRDH/IER     = 1 -> 0x04   (DLH when DLAB=1)
 *   REG_LCR          = 3 -> 0x0C
 *   REG_MDC (MCR)    = 4 -> 0x10
 *   REG_LSR          = 5 -> 0x14
 */
#define OFF_DLL 0x00U
#define OFF_DLH 0x04U
#define OFF_LCR 0x0CU
#define OFF_MCR 0x10U
#define OFF_LSR 0x14U

/* 16550 bit defs (mirror uart_ns16550.c). */
#define LCR_CS8  0x03U /* 8 data bits */
#define LCR_DLAB 0x80U /* divisor latch access */
#define MCR_LOOP 0x10U /* internal loopback */

/* Expected programmed values (see header derivation). */
#define EXP_DLL 0x36U /* 54 */
#define EXP_DLH 0x00U
#define EXP_LCR LCR_CS8 /* 0x03, 8N1, DLAB clear */

#define TEST_STR "ALP-UART3-LOOPBACK-115200"

static inline uint8_t rd8(uint32_t off)
{
	return *(volatile uint8_t *)(UART_BASE + off);
}

static inline void wr8(uint32_t off, uint8_t v)
{
	*(volatile uint8_t *)(UART_BASE + off) = v;
}

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(UART_NODE);

	printk("\n=== aen-uart-ns16550-loopback ===\n");
	printk("uart node      : %s\n", DT_NODE_FULL_NAME(UART_NODE));
	printk("uart_base      : 0x%08x\n", UART_BASE);
	printk("assumed pclk   : 100000000 Hz (syst_pclk, ALIF_UART3_SYST_PCLK parent)\n");
	printk("baud requested : 115200 8N1, internal loopback (MCR LOOP)\n");

	if (!device_is_ready(uart)) {
		printk("RESULT FAIL: uart device not ready (init/pinctrl/clock failed)\n");
		return 0;
	}

	/* ---- A. Functional loopback round-trip via the public API. ---- */
	const char *tx     = TEST_STR;
	size_t      n      = strlen(tx);
	char        rx[64] = { 0 };
	size_t      got    = 0;
	bool        rx_ok  = true;

	for (size_t i = 0; i < n; i++) {
		uart_poll_out(uart, (unsigned char)tx[i]);

		/* In MCR-LOOP the TX'd byte appears in RDR almost immediately; poll a
		 * bounded number of times so a broken RX path fails instead of hangs. */
		unsigned char c  = 0;
		int           rc = -1;
		for (int spin = 0; spin < 100000 && rc != 0; spin++) {
			rc = uart_poll_in(uart, &c);
		}
		if (rc != 0) {
			rx_ok = false;
			printk("loopback: no RX byte for tx[%u]='%c' (timeout)\n", (unsigned)i, tx[i]);
			break;
		}
		if (got < sizeof(rx) - 1) {
			rx[got++] = (char)c;
		}
	}

	bool match = rx_ok && (got == n) && (memcmp(tx, rx, n) == 0);
	printk("-- loopback --\n");
	printk("tx (%u): \"%s\"\n", (unsigned)n, tx);
	printk("rx (%u): \"%s\"\n", (unsigned)got, rx);
	printk("loopback match : %s\n", match ? "YES" : "NO");

	/* ---- B. Register readback: divisor latch + LCR. ---- */
	/* Snapshot LCR, set DLAB to expose DLL/DLH, read, then RESTORE LCR. */
	uint8_t lcr_live = rd8(OFF_LCR);
	wr8(OFF_LCR, lcr_live | LCR_DLAB);
	uint8_t dll = rd8(OFF_DLL);
	uint8_t dlh = rd8(OFF_DLH);
	wr8(OFF_LCR, lcr_live); /* restore exactly */

	uint8_t lcr_after = rd8(OFF_LCR);
	uint8_t mcr       = rd8(OFF_MCR);
	uint8_t lsr       = rd8(OFF_LSR);

	printk("-- regs --\n");
	printk("DLL  0x%08x = 0x%02x (exp 0x%02x)\n", UART_BASE + OFF_DLL, dll, EXP_DLL);
	printk("DLH  0x%08x = 0x%02x (exp 0x%02x)\n", UART_BASE + OFF_DLH, dlh, EXP_DLH);
	printk("LCR  0x%08x = 0x%02x (exp 0x%02x, 8N1 DLAB clear)\n",
	       UART_BASE + OFF_LCR,
	       lcr_after,
	       EXP_LCR);
	printk("MCR  0x%08x = 0x%02x (LOOP bit4 exp 1)\n", UART_BASE + OFF_MCR, mcr);
	printk("LSR  0x%08x = 0x%02x (no err bits 1..4 exp)\n", UART_BASE + OFF_LSR, lsr);

	/* ---- Verdict. ---- */
	bool regs_ok = (dll == EXP_DLL) && (dlh == EXP_DLH) &&
	               ((lcr_after & 0x3FU) == EXP_LCR); /* mask DLAB/break, check framing */

	if (match && regs_ok) {
		printk("RESULT PASS: ns16550 uart3 looped \"%s\" back; "
		       "DLL=0x%02x DLH=0x%02x LCR=0x%02x (115200 8N1)\n",
		       TEST_STR,
		       dll,
		       dlh,
		       lcr_after);
	} else if (regs_ok && !match) {
		/* Divisor/LCR correct but no byte came back: most likely MCR LOOP not
		 * set, or (in the physical-wire variant) the TX->RX jumper is missing. */
		printk("RESULT FAIL: divisor/LCR programmed OK but loopback round-trip "
		       "failed (MCR=0x%02x; check LOOP bit / jumper)\n",
		       mcr);
	} else {
		printk("RESULT FAIL: register readback mismatch "
		       "(DLL=0x%02x exp 0x%02x, DLH=0x%02x exp 0x%02x, LCR=0x%02x exp 0x%02x)\n",
		       dll,
		       EXP_DLL,
		       dlh,
		       EXP_DLH,
		       lcr_after,
		       EXP_LCR);
	}

	return 0;
}
