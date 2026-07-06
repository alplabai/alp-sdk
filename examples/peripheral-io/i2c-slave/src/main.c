/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-slave -- make this MCU answer on the bus as a register-mapped
 * I2C target (slave) via the <alp/i2c_regfile.h> helper.
 *
 * Pattern: the canonical "pretend to be a sensor / EEPROM" idiom.
 * The external controller (master) writes <reg_addr> then either
 * streams data bytes to set registers, or issues a repeated-START
 * read to query them.  alp_i2c_regfile_open ships that state machine
 * ready-made: hand it a buffer and it runs the classic protocol --
 *
 *   controller write, byte 0   -> latches the register pointer
 *   controller write, byte 1.. -> stores into the buffer (wraps)
 *   controller read            -> streams the buffer from the pointer
 *   STOP                       -> re-arms the pointer latch
 *
 * Rolling your own instead: <alp/peripheral.h>'s raw byte-granular
 * callbacks (alp_i2c_target_open with on_write / on_read / on_stop)
 * remain the escape hatch when the register-file shape doesn't fit --
 * command/response protocols, FIFOs, clock-stretching tricks.  This
 * example USED to hand-roll exactly the state machine above on those
 * callbacks; `git log -- examples/peripheral-io/i2c-slave` shows that
 * raw-callback version if you need a starting point.
 *
 * Test setup: wire SDA, SCL, and GND to a second board running
 * examples/peripheral-io/i2c-master (pointed at address 0x42), or
 * probe from a USB-I2C adapter:
 *
 *   i2ctransfer -y 0 w1@0x42 0x00 r4   ->  0xa0 0xa1 0xa2 0xa3
 *
 * What success looks like (real hardware, controller polling us):
 *
 *   [i2c-slave] listening @ 0x42 on BOARD_I2C_SENSORS
 *   [i2c-slave] tick 0 writes_seen=3 regs={0xa0,0x55,0xa2,0xa3,...}
 *   ...
 *   [i2c-slave] done
 *
 * On native_sim (CI lane) the emulated controller ACCEPTS the
 * target registration (CONFIG_I2C_TARGET), but no external
 * controller ever drives the emulated bus, so the ticks show
 * writes_seen=0.  On drivers WITHOUT target support open() fails
 * with ALP_ERR_NOSUPPORT and the diagnostic prints instead.
 * Either way the [i2c-slave] done marker latches the harness.
 *
 * Availability note: target mode needs controller-driver support
 * (Zephyr: CONFIG_I2C_TARGET plus a driver implementing
 * target_register).  The helper is a pure layer over the portable
 * target API, so it degrades with exactly the same status codes --
 * see <alp/peripheral.h> "I2C -- target (slave) mode".
 */

#include <stdio.h>

#include "alp/i2c_regfile.h"
#include "alp/peripheral.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> ALP_E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> ALP_E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* ------------------------------------------------------------------
 * Register file.
 *
 * Eight 8-bit registers behind target address 0x42.  The buffer is
 * caller-owned: the helper's callbacks store into it from the I2C
 * peripheral's ISR context, and the main thread polls it -- hence
 * `volatile`, so the compiler never caches stale copies.  Publishing
 * fresh device state to the controller is just a store into g_regs.
 * ------------------------------------------------------------------ */

#define SLAVE_OWN_ADDR_7BIT 0x42u
#define SLAVE_REG_COUNT     8u

static volatile uint8_t g_regs[SLAVE_REG_COUNT];

int main(void)
{
	/* Bring up the SDK runtime before the first open() -- thin today,
	 * but future backends (bridge links, vendor HAL bring-up) rely on
	 * it, and THEY can fail.  Check the return like any other call --
	 * an app that ignores it would run against a half-initialised
	 * SDK. */
	alp_status_t init_rc = alp_init();
	if (init_rc != ALP_OK) {
		printf("[i2c-slave] alp_init failed: %d\n", (int)init_rc);
		printf("[i2c-slave] done\n");
		return 1;
	}

	printf("[i2c-slave] listening @ 0x%02x on BOARD_I2C_SENSORS\n", SLAVE_OWN_ADDR_7BIT);

	/* Prime the register file BEFORE opening -- the controller can
	 * address us the moment open() returns.  Real firmware would
	 * expose device state here (ID register, status, sensor reading). */
	for (uint8_t i = 0; i < SLAVE_REG_COUNT; i++) {
		g_regs[i] = (uint8_t)(0xA0 + i);
	}

	/* One call replaces the whole hand-rolled pointer state machine:
	 * pointer latch, auto-increment with wraparound, STOP re-arm. */
	alp_i2c_regfile_t *rf;
	alp_status_t       rc = alp_i2c_regfile_open(
	    BOARD_I2C_SENSORS, /* E1M EVK: ALP_E1M_I2C0; E1M-X EVK: ALP_E1M_X_I2C0 */
	    SLAVE_OWN_ADDR_7BIT,
	    g_regs,
	    SLAVE_REG_COUNT,
	    &rf);
	if (rc != ALP_OK) {
		/* Common causes:
		 *   * ALP_ERR_NOSUPPORT -- controller driver has no target
		 *     mode (CONFIG_I2C_TARGET off, or the driver never
		 *     implemented target_register).
		 *   * ALP_ERR_NOT_READY -- alp-i2c0 alias unset / device
		 *     not ready on this board. */
		printf("[i2c-slave] regfile open failed: %d\n", (int)rc);
		printf("[i2c-slave]   I2C target mode is unavailable on this build\n");
		printf("[i2c-slave]   (check CONFIG_I2C_TARGET + controller-driver support)\n");
		printf("[i2c-slave] done\n");
		return 0;
	}

	/* Optional: make registers 0..1 read-only (device-ID style) by
	 * shrinking the controller-writable window to 2..7.  Out-of-window
	 * writes are dropped but the pointer still advances, mirroring
	 * real silicon.  Set the window BEFORE controller traffic. */
	(void)alp_i2c_regfile_set_write_window(rf, 2u, SLAVE_REG_COUNT - 2u);

	/* Idle loop -- the helper does all the work in ISR context
	 * whenever the external controller addresses us.  The stats
	 * counters give bench observability: print them once a second
	 * so an operator can SEE traffic without a logic analyzer. */
	for (int i = 0; i < 5; i++) {
		alp_i2c_regfile_stats_t st = { 0 };
		(void)alp_i2c_regfile_stats(rf, &st);
		printf("[i2c-slave] tick %d writes_seen=%u "
		       "regs={0x%02x,0x%02x,0x%02x,0x%02x,...}\n",
		       i,
		       (unsigned)st.writes_seen,
		       g_regs[0],
		       g_regs[1],
		       g_regs[2],
		       g_regs[3]);
		alp_delay_ms(1000);
	}

	alp_i2c_regfile_close(rf);
	printf("[i2c-slave] done\n");
	return 0;
}
