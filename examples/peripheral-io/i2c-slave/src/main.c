/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-slave -- make this MCU answer on the bus as a register-mapped
 * I2C target (slave) via alp_i2c_target_open.
 *
 * Pattern: the canonical "pretend to be a sensor / EEPROM" idiom.
 * The external controller (master) writes <reg_addr> then either
 * streams data bytes to set registers, or issues a repeated-START
 * read to query them.  Three byte-granular callbacks implement it:
 *
 *   on_write  -- first byte after a (re)START latches the register
 *                pointer; subsequent bytes store with auto-increment.
 *   on_read   -- serves register bytes with auto-increment.
 *   on_stop   -- re-arms the "next write byte is the pointer" latch.
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
 * target_register).  See <alp/peripheral.h> "I2C -- target (slave)
 * mode" for the full contract.
 */

#include <stdio.h>

#include "alp/peripheral.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* ------------------------------------------------------------------
 * Register file.
 *
 * Eight 8-bit registers behind target address 0x42.  `volatile`
 * because the callbacks run in the I2C peripheral's ISR context;
 * the main thread polling these bytes must not have the compiler
 * cache stale copies.
 * ------------------------------------------------------------------ */

#define SLAVE_OWN_ADDR_7BIT 0x42u
#define SLAVE_REG_COUNT     8u

static volatile uint8_t g_regs[SLAVE_REG_COUNT];

/* Register pointer state machine.  expecting_reg_addr latches at
 * STOP so the first written byte of the NEXT transaction is decoded
 * as the register pointer -- the standard register-mapped protocol. */
static volatile uint8_t g_reg_ptr;
static volatile bool    g_expecting_reg_addr = true;

/* Counter so an operator watching the console can SEE traffic. */
static volatile uint32_t g_writes_seen;

/* Byte received from the controller.  ISR context: keep it to the
 * state-machine update; defer side-effects ("register 0x00 wrote a
 * new mode -> reconfigure DSP") to a thread / workqueue. */
static void on_write(uint8_t byte, void *user)
{
	(void)user;
	if (g_expecting_reg_addr) {
		g_reg_ptr            = byte;
		g_expecting_reg_addr = false;
		return;
	}
	g_writes_seen++;
	if (g_reg_ptr < SLAVE_REG_COUNT) {
		g_regs[g_reg_ptr++] = byte; /* auto-increment; ignore overflow writes */
	}
}

/* Byte requested by the controller.  Serve the register file from
 * the latched pointer with auto-increment; reads past the end wrap
 * to 0xFF so the controller can tell "ran off the register file"
 * from a legitimate 0x00 value. */
static alp_status_t on_read(uint8_t *byte, void *user)
{
	(void)user;
	*byte = (g_reg_ptr < SLAVE_REG_COUNT) ? g_regs[g_reg_ptr++] : 0xFFu;
	return ALP_OK;
}

/* STOP condition: transaction over -- the next written byte is a
 * fresh register pointer. */
static void on_stop(void *user)
{
	(void)user;
	g_expecting_reg_addr = true;
}

int main(void)
{
	/* Bring up the SDK runtime before the first open() -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[i2c-slave] listening @ 0x%02x on BOARD_I2C_SENSORS\n", SLAVE_OWN_ADDR_7BIT);

	/* Prime the register file BEFORE registering -- callbacks start
	 * firing as soon as open() returns.  Real firmware would expose
	 * device state here (ID register, status, sensor reading). */
	for (uint8_t i = 0; i < SLAVE_REG_COUNT; i++) {
		g_regs[i] = (uint8_t)(0xA0 + i);
	}

	alp_i2c_target_t *tgt = alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = BOARD_I2C_SENSORS, /* E1M EVK: E1M_I2C0; E1M-X EVK: E1M_X_I2C0 */
	    .own_addr_7bit = SLAVE_OWN_ADDR_7BIT,
	    .on_write      = on_write,
	    .on_read       = on_read,
	    .on_stop       = on_stop,
	    .user          = NULL,
	});
	if (tgt == NULL) {
		/* Common causes:
		 *   * ALP_ERR_NOSUPPORT -- controller driver has no target
		 *     mode (CONFIG_I2C_TARGET off, or the driver never
		 *     implemented target_register).
		 *   * ALP_ERR_NOT_READY -- alp-i2c0 alias unset / device
		 *     not ready on this board. */
		printf("[i2c-slave] target open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[i2c-slave]   I2C target mode is unavailable on this build\n");
		printf("[i2c-slave]   (check CONFIG_I2C_TARGET + controller-driver support)\n");
		printf("[i2c-slave] done\n");
		return 0;
	}

	/* Idle loop -- the callbacks do all the work in ISR context
	 * whenever the external controller addresses us.  Print the
	 * write count once a second so an operator can SEE traffic. */
	for (int i = 0; i < 5; i++) {
		printf("[i2c-slave] tick %d writes_seen=%u "
		       "regs={0x%02x,0x%02x,0x%02x,0x%02x,...}\n",
		       i,
		       (unsigned)g_writes_seen,
		       g_regs[0],
		       g_regs[1],
		       g_regs[2],
		       g_regs[3]);
		alp_delay_ms(1000);
	}

	alp_i2c_target_close(tgt);
	printf("[i2c-slave] done\n");
	return 0;
}
