/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * alp-console -- bring up the Alp SoM interactive console.
 *
 * The console itself is SDK infrastructure: enabling CONFIG_ALP_SDK_CONSOLE
 * registers the whole `alp` command tree on the Zephyr shell at link time,
 * so an app gets board / gpio / i2c / adc / pwm / mem / clk / reboot
 * diagnostics for free -- main() does NOT register any commands.
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
