/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Alif pinctrl word for "this pad is a GPIO input" (issue #2808).  Pure
 * arithmetic with no Zephyr or Alif header, so a native_sim test can assert the
 * exact word; gpio_pad_alif.c BUILD_ASSERTs these constants against the real
 * pinctrl_soc.h macros so they cannot drift.  NOT a public header.
 *
 * Pad-config byte (pinctrl_soc.h), word bits:
 *   16 REN  input buffer       17 SMT  Schmitt trigger     18 SR  slew rate
 *   19:20 DSC bias (1 up, 2 down)   21:22 ODS drive (0 2mA, 1 4mA)   23 DRV
 *
 * Choice made explicit: an input gets REN + ODS 4 mA and nothing else.  That is
 * exactly the word the aen-trace-runner pinctrl workaround produced
 * (input-enable on the binding defaults: drive-strength 4, slew slow, no
 * Schmitt), bench-proven on EVK-03 as register 0x00210000, so moving the
 * workaround into the SDK changes no pad bit.  SMT is NOT set: the reset state
 * has it on (0x00220000) but that is not what the proven word did, and the
 * encoder / switch have external RC debounce.  Writing REN alone would also have
 * dropped ODS to 2 mA (bit 21), which is the difference the bench caught.
 * The pad register is written whole by pinctrl, so every bit is chosen here.
 */

#ifndef ALP_BACKENDS_GPIO_PAD_ALIF_WORD_H
#define ALP_BACKENDS_GPIO_PAD_ALIF_WORD_H

#include <stdint.h>

#include <alp/peripheral.h>

#define ALP_PAD_REN        (1u << 16)
#define ALP_PAD_DSC_SHIFT  19
#define ALP_PAD_DSC_UP     1u
#define ALP_PAD_DSC_DOWN   2u
#define ALP_PAD_ODS_4MA    (1u << 21)
#define ALP_PAD_PORT_SHIFT 6
#define ALP_PAD_PIN_SHIFT  3

static inline uint32_t alp_alif_pad_input_word(uint32_t port, uint32_t pin, alp_gpio_pull_t pull)
{
	uint32_t dsc = (pull == ALP_GPIO_PULL_UP)     ? ALP_PAD_DSC_UP
	               : (pull == ALP_GPIO_PULL_DOWN) ? ALP_PAD_DSC_DOWN
	                                              : 0u;
	/* AF 0 = GPIO. */
	return (port << ALP_PAD_PORT_SHIFT) | (pin << ALP_PAD_PIN_SHIFT) | ALP_PAD_REN |
	       ALP_PAD_ODS_4MA | (dsc << ALP_PAD_DSC_SHIFT);
}

#endif /* ALP_BACKENDS_GPIO_PAD_ALIF_WORD_H */
