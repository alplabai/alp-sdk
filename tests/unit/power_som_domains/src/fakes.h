/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FAKES_H
#define FAKES_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

#include <alp/chips/cc3501e.h>

/* Call counters and recorded state of the faked CC3501E driver. */
struct fake_cc {
	unsigned int hard_reset_calls;
	unsigned int reset_calls; /* must stay 0: cc3501e_reset() is the supply-cycling path */
	unsigned int power_off_calls;
	bool         nrst_level; /* last level written to reset_pin */
	bool         nrst_written;
	bool         en_level;
	bool         en_written;
	unsigned int ping_calls;
	bool         ping_always_fail;
};

extern struct fake_cc    g_cc;
extern cc3501e_t         g_fw;
extern alp_gpio_t *const g_nrst_pin;
extern alp_gpio_t *const g_en_pin;

/* Register file of an emulated I2C device (one byte per register). */
uint8_t *fake_regs(const struct emul *e);

/* Bus fake the TMP112 chip driver is pointed at. */
extern uint8_t g_chip_regs[256];

extern unsigned int g_clkout_calls;
extern int          g_clkout_src;

/* alp_som_power_pads_apply() stand-in: call count and the status it returns. */
extern unsigned int g_pads_calls;
extern alp_status_t g_pads_rc;

/* Pad index (>= 0) that reads back the opposite of what was driven; -1 = none. */
extern int g_pad_stuck_pin;

/* STOP_MODE_STAT word the boot restore reads (bit 4 = STOP wake). */
extern uint32_t g_stop_mode;

void fakes_reset(void);

#endif /* FAKES_H */
