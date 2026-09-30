/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble E8 flex-IO bank 1.8 V mode (CONFIG_ALIF_FLEX_IO_1V8).
 *
 * The flex-IO pads (GPIO7 pins 4..7 and the LPGPIO flex pins on AE822, per
 * the DFP's SOC_FEAT_GPIO7_FLEXIO_PIN_MASK / SOC_FEAT_LPGPIO_FLEXIO_PIN_MASK)
 * are powered from VDD_IO_FLEX and take their level mode from VBAT->GPIO_CTRL
 * bit 0 (0x1A609000; 0 = 3.3 V, the reset value; 1 = 1.8 V).  The Alif DFP
 * sets the bit in board_pins_config() when FLEX_IO_VOLTAGE_1V8 == 1; upstream
 * Zephyr's Alif SoC layer never touches it.
 *
 * Left in 3.3 V mode on a 1.8 V supply, a flex pin cannot read HIGH: measured
 * on E1M-AEN803 2026W36-0009, P7_5 read 0 at 0x49007050 while the M55 itself
 * drove it HIGH.  See #2434.
 */

#include <zephyr/init.h>
#include <zephyr/arch/cpu.h>
#include <soc_common.h>

#define VBAT_GPIO_CTRL_VOLT_1V8 BIT(0)

static int alif_flex_io_1v8(void)
{
	sys_set_bits(VBAT_GPIO_CTRL_EN, VBAT_GPIO_CTRL_VOLT_1V8);
	return 0;
}

SYS_INIT(alif_flex_io_1v8, PRE_KERNEL_1, 0);
