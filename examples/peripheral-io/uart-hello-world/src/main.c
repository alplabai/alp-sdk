/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * uart-hello-world -- the canonical "printf via UART" walkthrough.
 *
 * Open a UART port with alp_uart_open(), send a greeting + a
 * monotonically-increasing counter every second.  Distinct from
 * examples/peripheral-io/uart-echo (which is a bidirectional ping-pong) -- this
 * example is the producer-only variant most vendor SDKs ship as
 * their first tutorial.  It deliberately keeps the raw E1M_UART0
 * instance ID in view; examples/peripheral-io/uart-echo opens the same port by its
 * board-macro name (EVK_UART_PORT_DEBUG, from alp_e1m_evk_routes.h).
 * Both styles are first-class -- the macro is just a board-named
 * alias for the portable instance ID.
 *
 * Why does this exist when Zephyr already routes printf to its
 * console UART?  Two reasons:
 *
 *   1. printf goes to the *console* UART (a single, Kconfig-pinned
 *      device).  Many real apps need to drive a *different* UART
 *      -- one wired to a Bluetooth modem, a GPS, a stepper-motor
 *      driver, a debug pin on a custom board.  The
 *      alp_uart_*() surface gives you that without touching
 *      CONFIG_CONSOLE knobs.
 *   2. printf is a one-way text formatter.  alp_uart_write()
 *      takes a byte buffer with explicit length, which is what
 *      you want when shipping binary protocols (e.g. NMEA
 *      sentences, packed-binary RPC frames, line-protocol metrics).
 *
 * What success looks like:
 *
 *   [uart-hello] open E1M_UART0 @ 115200 8N1
 *   [uart-hello] greeting written
 *   [uart-hello] tick 0 written
 *   [uart-hello] tick 1 written
 *   ...
 *   [uart-hello] done
 *
 * On real silicon you'd also see the greeting + ticks on whatever
 * terminal is wired to E1M_UART0 (the FTDI USB-UART on the EVK,
 * the board's modem header on a custom board, ...).
 */

#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

/* Capped tick count keeps the native_sim run inside twister's
 * timeout.  Real on-silicon firmware would loop forever instead. */
#define HELLO_TICKS 5u

/* The wait between ticks.  alp_delay_ms yields the CPU on
 * scheduler-backed targets (busy-loops on baremetal). */
#define HELLO_TICK_PERIOD_MS 1000u

/* The greeting we ship out the UART.  Stored as a `static const`
 * array so the compiler can place it in flash (not on the stack)
 * and the size is fixed at build time.  Note the explicit "\r\n"
 * line-ending -- serial terminals expect CR+LF (most don't
 * implement bare-LF auto-CR like a stdout). */
static const uint8_t HELLO_GREETING[] = "Alp SDK uart-hello-world\r\n";

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[uart-hello] open E1M_UART0 @ 115200 8N1\n");

	/* Open UART0 at the lowest-common-denominator serial framing.
     *
     * 115200 8N1 is what every serial console + USB-UART bridge
     * supports out of the box.  Override these fields when talking
     * to a chip that demands different framing -- e.g. some
     * industrial PLCs use 7-E-1, RS-485 multidrop uses 9-bit
     * frames, some legacy GPS modules want 9600 baud.
     *
     * The E1M_UART0 instance ID is portable across every
     * E1M-conformant SoM (E1M-AEN, E1M-V2N, ...): the SDK's
     * loader resolves it to the right SoC USART node at build
     * time via the devicetree `alp-uart0` alias. */
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = E1M_UART0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	if (u == NULL) {
		/* Likely causes (in descending order of frequency):
         *
         *   * No `alp-uart0` alias on this board's overlay.  Add
         *     one in your board file or
         *     `boards/<board>.overlay` -- see the README for the
         *     binding shape.
         *   * The Zephyr UART driver returned -ENOTSUP because
         *     the controller doesn't accept the requested
         *     baudrate / framing combination.
         *   * On native_sim without the matching overlay the
         *     alias resolves to NULL spec and you land here. */
		printf("[uart-hello] open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[uart-hello] done\n");
		return 0;
	}

	/* Write the greeting.  alp_uart_write blocks until every byte
     * is queued in the controller's TX FIFO (or shifted out the
     * line on tiny SoCs with no FIFO).  The `sizeof - 1` trims
     * the trailing NUL in the string literal -- send the bytes
     * a human would see on the terminal, not the C-string
     * sentinel. */
	alp_status_t s = alp_uart_write(u, HELLO_GREETING, sizeof HELLO_GREETING - 1);
	if (s != ALP_OK) {
		/* Write failures are rare on UART (no slave to NACK like
         * I2C) -- they usually indicate the driver was torn down
         * mid-write, e.g. because of a runtime PM transition. */
		printf("[uart-hello] greeting write -> status=%d\n", (int)s);
		alp_uart_close(u);
		printf("[uart-hello] done\n");
		return 0;
	}
	printf("[uart-hello] greeting written\n");

	/* Periodic counter loop.  Format each tick into a local stack
     * buffer using snprintf (NUL-safe + bounded), then write the
     * formatted bytes (minus the NUL) over the UART.
     *
     * snprintf returns the number of bytes that *would have been*
     * written if the buffer were unbounded; on truncation that's
     * > sizeof(buf).  Clamp before passing to alp_uart_write so
     * we never read past the end of the stack buffer. */
	for (uint32_t tick = 0; tick < HELLO_TICKS; tick++) {
		char   buf[48];
		int    n  = snprintf(buf, sizeof buf, "tick %u\r\n", tick);
		size_t ln = (n < 0) ? 0u : (size_t)n;
		if (ln > sizeof buf - 1u) ln = sizeof buf - 1u;

		s = alp_uart_write(u, (const uint8_t *)buf, ln);
		if (s != ALP_OK) {
			printf("[uart-hello] tick %u write -> status=%d\n", tick, (int)s);
			break;
		}
		printf("[uart-hello] tick %u written\n", tick);
		alp_delay_ms(HELLO_TICK_PERIOD_MS);
	}

	/* Close releases the SDK handle but does NOT power down the
     * controller or drop the line -- on real hardware the UART
     * keeps running idle high.  If you truly need to shut down,
     * use the Zephyr PM API (pm_device_action_run) after close. */
	alp_uart_close(u);
	printf("[uart-hello] done\n");
	return 0;
}
