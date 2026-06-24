/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-slave -- demonstrate the SHAPE of I2C slave-mode application
 * code on the Alp SDK.
 *
 * ============================================================
 *   SDK GAP NOTICE -- READ THIS BEFORE COPYING THE EXAMPLE
 * ============================================================
 *
 * As of v0.6 the Alp SDK's <alp/peripheral.h> I2C surface is
 * MASTER-ONLY.  There is no `alp_i2c_open_slave()` /
 * `alp_i2c_slave_register_callbacks()` / equivalent yet.  This
 * example exists to:
 *
 *   1. Document the gap so customers don't waste time hunting for
 *      a non-existent header (issue: track at v1.0 ABI freeze).
 *   2. Stake out the proposed API shape so when the slave-mode
 *      surface lands, the example is the migration template.
 *   3. Show the recommended fake-register-file pattern -- the
 *      idiom every embedded engineer expects for "make this MCU
 *      look like a sensor / EEPROM / register-mapped peripheral".
 *
 * The code below uses a small `alp_i2c_slave_*` shim defined at
 * the top of this file.  Today every shim function returns
 * ALP_ERR_NOSUPPORT; the example prints the diagnostic and exits.
 * When the real surface lands (planned for v0.7), the shim block
 * deletes and the calls bind to the upstream API unchanged.
 *
 * For master-side I2C (which the SDK DOES support), see:
 *   * examples/peripheral-io/i2c-scanner   -- discovery
 *   * examples/peripheral-io/i2c-master    -- known-address read
 *
 * To test the slave-mode example on real hardware once the API
 * lands, wire two boards together (SDA, SCL, GND) and run the
 * master-side counterpart from your application.
 *
 * ============================================================
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

/* ------------------------------------------------------------------
 * Local shim for the not-yet-shipped slave-mode API.
 *
 * Every function returns ALP_ERR_NOSUPPORT so the example runs to
 * completion without hardware effects.  When the real
 * <alp/peripheral.h> surface lands, delete this block and the
 * downstream code keeps compiling against the upstream names.
 * ------------------------------------------------------------------ */

/** Proposed: opaque slave handle, parallel to alp_i2c_t. */
typedef struct alp_i2c_slave_shim alp_i2c_slave_t;

/** Proposed: slave configuration. */
typedef struct {
	uint32_t bus_id;        /**< E1M_I2Cn bus to claim. */
	uint8_t  own_addr_7bit; /**< This slave's 7-bit address. */
} alp_i2c_slave_config_t;

/** Proposed: callback signature for incoming write-from-master. */
typedef void (*alp_i2c_slave_write_cb_t)(uint8_t        reg_addr,
                                         const uint8_t *data,
                                         size_t         len,
                                         void          *user);

/** Proposed: callback signature for incoming read-from-master. */
typedef alp_status_t (*alp_i2c_slave_read_cb_t)(
    uint8_t reg_addr, uint8_t *data, size_t max_len, size_t *out_len, void *user);

/* TODO(api-gap): replace these stubs once <alp/peripheral.h> grows
 * slave-mode support.  The downstream main() is already coded
 * against the proposed names. */
static alp_i2c_slave_t *alp_i2c_slave_open(const alp_i2c_slave_config_t *cfg)
{
	(void)cfg;
	/* Real impl will Zephyr-dispatch to the controller's slave
     * registration call (i2c_slave_register on the Zephyr API). */
	return NULL;
}

static alp_status_t alp_i2c_slave_set_callbacks(alp_i2c_slave_t         *slave,
                                                alp_i2c_slave_write_cb_t on_write,
                                                alp_i2c_slave_read_cb_t  on_read,
                                                void                    *user)
{
	(void)slave;
	(void)on_write;
	(void)on_read;
	(void)user;
	return ALP_ERR_NOSUPPORT;
}

static void alp_i2c_slave_close(alp_i2c_slave_t *slave)
{
	(void)slave;
}

/* ------------------------------------------------------------------
 * Fake register file.
 *
 * The canonical I2C-slave idiom: pretend to be a register-mapped
 * peripheral.  The master writes <reg_addr><value> to set a register
 * and writes <reg_addr> + reads to query.  This snippet would back
 * an 8-register slave at address 0x42.
 * ------------------------------------------------------------------ */

#define SLAVE_OWN_ADDR_7BIT 0x42u
#define SLAVE_REG_COUNT     8u

/* `volatile` because callbacks run from interrupt context on the
 * I2C peripheral's ISR; main thread polling the bytes must not
 * have the compiler cache stale copies. */
static volatile uint8_t g_regs[SLAVE_REG_COUNT];

/* Counter so the example prints something interesting even when
 * the API is unavailable.  Demonstrates how a real callback would
 * pull data off the wire into the register file. */
static volatile uint32_t g_writes_seen = 0;

/* Write-from-master callback.  Real implementations would:
 *   1. Sanity-check reg_addr is within g_regs[].
 *   2. memcpy len bytes from data into g_regs starting at reg_addr.
 *   3. Optionally trigger a deferred work item to handle side-effects
 *      (e.g. "register 0x00 wrote a new mode -> reconfigure DSP").
 * Keep ISR-context work minimal -- defer everything else. */
static void on_master_write(uint8_t reg_addr, const uint8_t *data, size_t len, void *user)
{
	(void)user;
	g_writes_seen++;
	if (reg_addr >= SLAVE_REG_COUNT) {
		return; /* Master wrote to a non-existent register. */
	}
	/* Clamp the copy to the register file end -- protects against a
     * master that streams more bytes than we have registers to hold. */
	size_t to_copy = len;
	if (reg_addr + to_copy > SLAVE_REG_COUNT) {
		to_copy = SLAVE_REG_COUNT - reg_addr;
	}
	for (size_t i = 0; i < to_copy; i++) {
		g_regs[reg_addr + i] = data[i];
	}
}

/* Read-from-master callback.  Master sends our 7-bit address with the
 * R/W bit set, then clocks out bytes.  Return however many bytes the
 * register file holds starting at reg_addr.  out_len is what we
 * actually filled in. */
static alp_status_t
on_master_read(uint8_t reg_addr, uint8_t *data, size_t max_len, size_t *out_len, void *user)
{
	(void)user;
	if (reg_addr >= SLAVE_REG_COUNT) {
		*out_len = 0;
		return ALP_ERR_OUT_OF_RANGE;
	}
	size_t available = SLAVE_REG_COUNT - reg_addr;
	size_t n         = available < max_len ? available : max_len;
	for (size_t i = 0; i < n; i++) {
		data[i] = g_regs[reg_addr + i];
	}
	*out_len = n;
	return ALP_OK;
}

int main(void)
{
	printf("[i2c-slave] open as slave @ 0x%02x on BOARD_I2C_SENSORS\n", SLAVE_OWN_ADDR_7BIT);

	/* Prime the register file so a master reading from address 0
     * sees recognisable bytes.  Real firmware would expose device
     * state here (ID register, status, sensor reading). */
	for (uint8_t i = 0; i < SLAVE_REG_COUNT; i++) {
		g_regs[i] = (uint8_t)(0xA0 + i);
	}

	alp_i2c_slave_t *s = alp_i2c_slave_open(&(alp_i2c_slave_config_t){
	    .bus_id        = BOARD_I2C_SENSORS, /* E1M EVK: E1M_I2C0; E1M-X EVK: E1M_X_I2C0 */
	    .own_addr_7bit = SLAVE_OWN_ADDR_7BIT,
	});
	if (s == NULL) {
		/* Today this branch ALWAYS fires because the shim returns
         * NULL.  The diagnostic line documents that the SDK lacks
         * slave-mode support so customers reading the console
         * output understand why their `west build` succeeded but
         * the device doesn't respond to a master probe. */
		printf("[i2c-slave] Alp SDK v0.6 does NOT support I2C slave mode\n");
		printf("[i2c-slave]   <alp/peripheral.h> is master-only today\n");
		printf("[i2c-slave]   tracking: v0.7 API surface addition\n");
		printf("[i2c-slave] done\n");
		return 0;
	}

	/* This block is unreachable today but stays compiled-in so the
     * proposed API shape is exercised by the type-checker.  When
     * the shim deletes, this is the only production-path code. */
	alp_status_t st = alp_i2c_slave_set_callbacks(s,
	                                              on_master_write,
	                                              on_master_read,
	                                              /* user */ NULL);
	if (st != ALP_OK) {
		printf("[i2c-slave] set_callbacks -> %d (expected -6 NOSUPPORT today)\n", (int)st);
		alp_i2c_slave_close(s);
		printf("[i2c-slave] done\n");
		return 0;
	}

	/* Idle loop -- on real hardware the callbacks fire whenever a
     * master addresses us.  Print the write count once a second so
     * an operator running the example can SEE incoming traffic. */
	for (int i = 0; i < 5; i++) {
		printf("[i2c-slave] tick %d writes_seen=%u "
		       "regs={0x%02x,0x%02x,0x%02x,0x%02x,...}\n",
		       i,
		       (unsigned)g_writes_seen,
		       g_regs[0],
		       g_regs[1],
		       g_regs[2],
		       g_regs[3]);
		k_msleep(1000);
	}

	alp_i2c_slave_close(s);
	printf("[i2c-slave] done\n");
	return 0;
}
