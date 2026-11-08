/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the production SWD host driver in
 * `chips/gd32_swd/gd32_swd.c`.
 *
 * The harness links the real driver and fakes only its two seams: the
 * platform pad opener (`gd32_swd_platform()`, a weak symbol in the driver)
 * and the portable GPIO calls it makes (`alp_gpio_*`).  SWDIO reads return
 * the fuzz bytes as a bit stream (LSB first), so the target's side of the
 * wire -- ACK, 32-bit data word, parity bit -- is attacker-controlled and
 * the driver's own parity / ACK / retry / halt-poll logic runs unmodified
 * through gd32_swd_init / connect / halt / flash_write / reset_and_run.
 *
 *   byte[0]    bit 0 = level SWDIO reads once the stream is exhausted
 *   byte[1..]  the SWDIO read bit stream
 *
 * Every loop in the driver is bounded, so a hang is a driver bug and
 * libFuzzer's -timeout reports it.
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=baremetal \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_swd_packet
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/chips/gd32g553.h"
#include "alp/chips/gd32_swd.h"
#include "alp/peripheral.h"
#include "gd32_swd_platform.h"

/* Fuzz-fed SWDIO bit stream. */
static const uint8_t *g_bits;
static size_t         g_nbits;
static size_t         g_pos;
static bool           g_tail_level;

/* Pads are opaque to the driver; three distinct tokens are enough. */
static int g_pad[3];

static alp_gpio_t *fake_open_pad(uint32_t pad_id)
{
	switch (pad_id) {
	case GD32G553_PAD_ID_SWDIO:
		return (alp_gpio_t *)&g_pad[0];
	case GD32G553_PAD_ID_SWCLK:
		return (alp_gpio_t *)&g_pad[1];
	case GD32G553_PAD_ID_NRST:
		return (alp_gpio_t *)&g_pad[2];
	default:
		return NULL;
	}
}

static alp_status_t fake_output_low(alp_gpio_t *pad)
{
	(void)pad;
	return ALP_OK;
}

static void fake_close_pad(alp_gpio_t *pad)
{
	(void)pad;
}

static const gd32_swd_platform_t g_plat = {
	.open_pad             = fake_open_pad,
	.configure_output_low = fake_output_low,
	.close_pad            = fake_close_pad,
};

/* Overrides the driver's weak default. */
const gd32_swd_platform_t *gd32_swd_platform(void)
{
	return &g_plat;
}

alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	(void)pin;
	(void)dir;
	(void)pull;
	return ALP_OK;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_OK;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	if (g_pos < g_nbits) {
		*level = ((g_bits[g_pos >> 3] >> (g_pos & 7u)) & 1u) != 0u;
		g_pos++;
	} else {
		*level = g_tail_level;
	}
	return ALP_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2u) return 0;
	g_tail_level = (data[0] & 1u) != 0u;
	g_bits       = data + 1;
	g_nbits      = (size - 1u) * 8u;
	g_pos        = 0u;

	gd32_swd_t ctx;
	if (gd32_swd_init(&ctx) != ALP_OK) __builtin_trap();
	(void)gd32_swd_set_clock_delay(&ctx, 0u);

	if (gd32_swd_connect(&ctx) == ALP_OK) {
		static const uint8_t blob[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
		(void)gd32_swd_halt(&ctx);
		(void)gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, blob, sizeof(blob));
		(void)gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, blob, sizeof(blob));
		(void)gd32_swd_reset_and_run(&ctx);
	}
	gd32_swd_deinit(&ctx);
	return 0;
}
