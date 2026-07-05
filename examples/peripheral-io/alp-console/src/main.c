/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-console -- the E1M-AEN801 "everything" demo: interactive console +
 * CC3501E Wi-Fi/BLE + a live RGB status LED, in one shippable slot0 app.
 *
 * The console itself is SDK infrastructure: enabling CONFIG_ALP_SDK_CONSOLE
 * registers the whole `alp` command tree on the Zephyr shell at link time,
 * so an app gets board / gpio / i2c / adc / pwm / mem / clk / reboot
 * diagnostics for free -- main() does NOT register any commands.  On the
 * AEN801 that tree includes `alp companion wifi scan|connect`,
 * `alp companion ble enable|scan`, and `alp companion sock tcp-get <ip> <port>
 * <path>` (a full TCP client -- open/connect/send/recv/close over the CC3501E IP
 * stack), which reach the on-module CC3501E over the inter-chip bridge bound
 * below.  A background thread breathes the on-board RGB LED so the board is
 * visibly alive while the shell stays fully responsive.
 *
 * Boot sequence:
 *   1. Zephyr shell starts (via CONFIG_SHELL=y in prj.conf).
 *   2. alp_banner.c (linked because CONFIG_ALP_SDK_CONSOLE=y pulls the
 *      banner module) prints the board identity and Alp SDK version.
 *   3. The shell prompt `uart:~$` appears.
 *   4. Type `alp` then Tab to explore all sub-commands.
 *
 * The ONE thing an app does here is bind the companion chip so
 * `alp companion` can reach it.  On V2N that is automatic (the GD32
 * supervisor is a singleton managed inside the SDK).  On Alif there is
 * no companion singleton, so we open the CC3501E and hand the handle to
 * the console.  On boards without a CC3501E (including native_sim) this
 * whole block compiles out via the CONFIG_ALP_SDK_CHIP_CC3501E guard.
 */

#include <zephyr/kernel.h>

/*
 * Companion binding -- Alif (AEN801) path only.
 *
 * cc3501e_bridge_bringup() is the one-call SoM bring-up helper from the
 * aen-cc3501e-bringup example (cc3501e_bridge.{c,h}).  It:
 *   - opens the inter-chip SPI bus (bus_id 1, 1 MHz, no chip-select)
 *   - opens the WIFI_EN + nRESET Alif LP-GPIO control pins
 *   - binds them to the cc3501e_t handle
 *   - attaches the GPIO proxy (when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y)
 *   - runs the TI SWRU626 power + reset sequence (blocks ~900 ms)
 *
 * After a successful bring-up, alp_console_companion_set() hands the
 * handle to the console so `alp companion ver` / `alp companion ping`
 * can reach the CC3501E without the app having to do anything further.
 *
 * The cc3501e_t handle MUST outlive the console (program lifetime), so
 * it is declared static at file scope.
 */
#if defined(CONFIG_ALP_SDK_CHIP_CC3501E)
#include <alp/console.h>
#include <alp/chips/cc3501e.h>

/*
 * cc3501e_bridge_bringup() -- the one-call SoM bring-up template copied
 * from examples/aen/aen-cc3501e-bringup/src/cc3501e_bridge.{c,h}.
 * That file is the authoritative pattern for opening the inter-chip SPI
 * bridge; the alp-console example ships its own copy here so it is
 * self-contained and does not depend on the bringup example's src/.
 */
#include "cc3501e_bridge.h"

/* Static storage: the console borrows this handle for the program's life. */
static cc3501e_t companion_ctx;

/*
 * bind_companion() -- best-effort.  If the CC3501E is absent or the SPI
 * bus is not in the overlay the bring-up returns an error and we leave
 * companion_cc3501e NULL.  `alp companion` then reports "not registered"
 * instead of crashing -- the console stays fully usable for every other
 * sub-command (board / gpio / i2c / adc / pwm / mem / clk).
 */
static void bind_companion(void)
{
	/*
	 * cc3501e_bridge_bringup() is silicon-validated on the E1M-AEN801
	 * (see memory/reference-v2n-mali-kbase-rebuild.md and the
	 * aen-cc3501e-bringup bench log).  It opens the inter-chip SPI
	 * (no-CS lockstep, mode 0, 1 MHz) + the WIFI_EN / nRESET LP-GPIO
	 * pads, then runs the Puya cold-boot hard-reset + TI SWRU626
	 * power sequence.  The bus_id, pin indices, and SPI frequency are
	 * the E1M-AEN SoM defaults from cc3501e_bridge.h -- override them
	 * per board variant without touching this file.
	 */
	alp_status_t s = cc3501e_bridge_bringup(&companion_ctx);

	if (s == ALP_OK) {
		/* Hand the ready handle to the console once; it borrows it
		 * for the program's life.  The companion_ctx storage above
		 * must NOT go out of scope (file-static, so it never does). */
		alp_console_companion_set(&companion_ctx);
	}
	/* On any error the companion simply reports "not registered" in
	 * `alp companion ver` -- no panic, no printk noise (the shell
	 * prompt follows immediately after main() returns). */
}
#else  /* !CONFIG_ALP_SDK_CHIP_CC3501E */

/*
 * No companion on this target (V2N singleton auto-bound inside the SDK;
 * native_sim has no companion at all).  bind_companion() is a no-op so
 * main() is identical across all targets.
 */
static void bind_companion(void)
{
}
#endif /* CONFIG_ALP_SDK_CHIP_CC3501E */

/* ---- RGB "board alive" status LED (E1M-AEN801 EVK) ---------------------- */
/*
 * A low-priority background thread breathes the on-module RGB LED through a
 * rainbow so the demo is visibly alive while the `alp` shell stays fully
 * responsive on the console.  Wiring + the breathe maths are lifted from the
 * aen-rgb-led-fade example (RED=PWM3/P2_4, GREEN=PWM0/P12_7, BLUE=PWM1/P12_6 --
 * the board overlay's pwm-leds children).  Device-tree-guarded: on a board
 * without the led_red pwm-leds child (native_sim, V2N) the whole block compiles
 * out and no thread is spawned.
 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(led_red), okay)
#include <zephyr/drivers/pwm.h>

#define RGB_PERIOD_CYCLES 1000u

/* 8-bit phase -> 0..255 triangle, smoothed by an integer smoothstep (also a
 * perceptual gamma) so the breathe looks linear to the eye.  No float / libm. */
static uint32_t rgb_breathe(uint32_t phase)
{
	uint32_t p = phase & 0xFFu;
	uint32_t t = (p < 128u) ? (p * 2u) : ((255u - p) * 2u);
	return (uint32_t)(((uint64_t)t * t * (765u - 2u * t)) / 65025u);
}

/* pulse = period * brightness / 255, clamped to >= 1 cycle (a 0 pulse takes the
 * driver's full-off path, which perturbs the shared timer -> visible flicker). */
static void rgb_set(const struct pwm_dt_spec *ch, uint32_t b8)
{
	uint32_t pulse = (RGB_PERIOD_CYCLES * b8) / 255u;

	if (pulse == 0u) {
		pulse = 1u;
	}
	(void)pwm_set_cycles(ch->dev, ch->channel, RGB_PERIOD_CYCLES, pulse, ch->flags);
}

static void rgb_status_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	const struct pwm_dt_spec red   = PWM_DT_SPEC_GET(DT_NODELABEL(led_red));
	const struct pwm_dt_spec green = PWM_DT_SPEC_GET(DT_NODELABEL(led_green));
	const struct pwm_dt_spec blue  = PWM_DT_SPEC_GET(DT_NODELABEL(led_blue));

	if (!pwm_is_ready_dt(&red) || !pwm_is_ready_dt(&green) || !pwm_is_ready_dt(&blue)) {
		return; /* controllers not ready -- leave the LED dark, console unaffected */
	}

	uint32_t phase = 0u;

	for (;;) {
		rgb_set(&red, rgb_breathe(phase));         /*   0 deg */
		rgb_set(&green, rgb_breathe(phase + 85u)); /* +120 deg around the wheel */
		rgb_set(&blue, rgb_breathe(phase + 170u)); /* +240 deg */
		phase = (phase + 1u) & 0xFFu;
		k_msleep(15);
	}
}

/* Spawn at boot (priority 7 -- below the shell + main).  The shell + companion
 * bring-up run independently; the LED just breathes in the background. */
K_THREAD_DEFINE(rgb_status_tid, 1024, rgb_status_thread, NULL, NULL, NULL, 7, 0, 0);
#endif /* led_red okay */

int main(void)
{
	/*
	 * The boot banner (src/zephyr/alp_banner.c, auto-linked when
	 * CONFIG_ALP_SDK_CONSOLE=y) has already printed at this point.
	 *
	 * bind_companion() is a no-op on targets without a CC3501E and
	 * takes ~900 ms on the AEN801 (CC3501E power + reset sequence).
	 * The shell prompt `uart:~$` appears immediately after main()
	 * returns -- type `alp` then Tab to explore.
	 *
	 * Bench tip: CONFIG_ALP_SDK_CONSOLE_UNSAFE=y (prj.conf) unlocks
	 * the write-capable commands (mem wr, gpio write, companion gpio
	 * write).  Leave it off in field builds.
	 */
	bind_companion();
	return 0;
}
