/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/i2c_regfile.h> -- register-file I2C target helper.
 *
 * Pure layer over the portable target API (alp_i2c_target_open in
 * <alp/peripheral.h>): generalises the register-pointer state machine
 * from examples/peripheral-io/i2c-slave to a caller-owned buffer of
 * arbitrary length.  OS-agnostic -- compiles into every backend build
 * (Zephyr module, Yocto, baremetal) and degrades exactly as the
 * wrapped target open does (ALP_ERR_NOSUPPORT on backends without
 * target mode), so it adds no porting surface of its own.
 *
 * Concurrency model: the three callbacks run in the I2C peripheral's
 * ISR context (see <alp/peripheral.h>).  All state they touch is
 * `volatile`, and each transaction only ever runs one callback at a
 * time, so plain loads/stores are sufficient -- no locking.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/i2c_regfile.h>
#include <alp/peripheral.h>

/* Helper handles are 1:1 over target handles, so bound the pool by the
 * same knob the I2C dispatcher uses for its target pool (Kconfig on
 * Zephyr; the dispatcher's fallback default of 2 elsewhere). */
#ifndef CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES
#define CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES 2
#endif

struct alp_i2c_regfile {
	alp_i2c_target_t *tgt;  /* wrapped portable target handle */
	volatile uint8_t *regs; /* caller-owned backing buffer */
	size_t            len;  /* register count in regs */

	/* Write window [wr_first, wr_first + wr_count): controller
	 * writes outside it are dropped (read-only registers). */
	volatile size_t wr_first;
	volatile size_t wr_count;

	/* Register-pointer state machine.  expecting_reg_addr re-arms
	 * at STOP so the first written byte of the NEXT transaction is
	 * decoded as the register pointer. */
	volatile size_t reg_ptr;
	volatile bool   expecting_reg_addr;

	volatile uint32_t writes_seen;
	volatile uint32_t reads_seen;

	bool in_use;
};

static struct alp_i2c_regfile _pool[CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES];

static struct alp_i2c_regfile *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I2C_TARGET_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset((void *)&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

/* Byte received from the controller (ISR context).  First byte after
 * a (re)START latches the register pointer; subsequent bytes store
 * with auto-increment.  Both wrap modulo len, EEPROM-style, so the
 * controller can never index outside the caller's buffer. */
static void rf_on_write(uint8_t byte, void *user)
{
	struct alp_i2c_regfile *rf = user;

	if (rf->expecting_reg_addr) {
		rf->reg_ptr            = (size_t)byte % rf->len;
		rf->expecting_reg_addr = false;
		return;
	}
	rf->writes_seen++;
	size_t p = rf->reg_ptr;
	if (p >= rf->wr_first && (p - rf->wr_first) < rf->wr_count) {
		rf->regs[p] = byte;
	}
	rf->reg_ptr = (p + 1u) % rf->len;
}

/* Byte requested by the controller (ISR context): stream the register
 * file from the latched pointer with wraparound auto-increment. */
static alp_status_t rf_on_read(uint8_t *byte, void *user)
{
	struct alp_i2c_regfile *rf = user;

	size_t p    = rf->reg_ptr;
	*byte       = rf->regs[p];
	rf->reg_ptr = (p + 1u) % rf->len;
	rf->reads_seen++;
	return ALP_OK;
}

/* STOP condition: transaction over -- the next written byte is a
 * fresh register pointer. */
static void rf_on_stop(void *user)
{
	struct alp_i2c_regfile *rf = user;

	rf->expecting_reg_addr = true;
}

alp_status_t alp_i2c_regfile_open(uint32_t            bus_id,
                                  uint8_t             own_addr_7bit,
                                  volatile uint8_t   *regs,
                                  size_t              len,
                                  alp_i2c_regfile_t **out)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	*out = NULL;
	/* 0x00..0x07 and 0x78..0x7F are I2C-reserved; the documented
	 * valid range for own_addr_7bit is 0x08..0x77. */
	if (regs == NULL || len == 0u || own_addr_7bit < 0x08u || own_addr_7bit > 0x77u) {
		return ALP_ERR_INVAL;
	}

	struct alp_i2c_regfile *rf = _alloc();
	if (rf == NULL) {
		return ALP_ERR_NOMEM;
	}
	rf->regs               = regs;
	rf->len                = len;
	rf->wr_first           = 0u;
	rf->wr_count           = len; /* whole file writable by default */
	rf->reg_ptr            = 0u;
	rf->expecting_reg_addr = true;

	/* State is primed -- callbacks may fire the moment this returns. */
	rf->tgt = alp_i2c_target_open(&(alp_i2c_target_config_t){
	    .bus_id        = bus_id,
	    .own_addr_7bit = own_addr_7bit,
	    .on_write      = rf_on_write,
	    .on_read       = rf_on_read,
	    .on_stop       = rf_on_stop,
	    .user          = rf,
	});
	if (rf->tgt == NULL) {
		/* Propagate the wrapped open's failure code (NOSUPPORT /
		 * NOT_READY / NOMEM ...) so the helper degrades exactly
		 * like the raw target API. */
		alp_status_t rc = alp_last_error();
		rf->in_use      = false;
		return (rc == ALP_OK) ? ALP_ERR_IO : rc;
	}
	*out = rf;
	return ALP_OK;
}

alp_status_t alp_i2c_regfile_set_write_window(alp_i2c_regfile_t *rf, size_t first, size_t count)
{
	if (rf == NULL || !rf->in_use) {
		return ALP_ERR_INVAL;
	}
	/* Window must fit the file: first may equal len only when the
	 * window is empty (count == 0 -> fully read-only). */
	if (first > rf->len || count > rf->len - first) {
		return ALP_ERR_INVAL;
	}
	rf->wr_first = first;
	rf->wr_count = count;
	return ALP_OK;
}

alp_status_t alp_i2c_regfile_stats(const alp_i2c_regfile_t *rf, alp_i2c_regfile_stats_t *out)
{
	if (rf == NULL || !rf->in_use || out == NULL) {
		return ALP_ERR_INVAL;
	}
	out->writes_seen = rf->writes_seen;
	out->reads_seen  = rf->reads_seen;
	return ALP_OK;
}

void alp_i2c_regfile_close(alp_i2c_regfile_t *rf)
{
	if (rf == NULL || !rf->in_use) {
		return;
	}
	alp_i2c_target_close(rf->tgt); /* no callback fires after this */
	rf->tgt    = NULL;
	rf->regs   = NULL;
	rf->in_use = false;
}
