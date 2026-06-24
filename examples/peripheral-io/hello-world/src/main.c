/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * hello-world -- the canonical "first program" for the Alp SDK.
 *
 * No peripherals, no chips, no board-specific wiring.  Just the
 * boot path + a periodic printf loop so customers can confirm
 * their toolchain + flash flow + log console are wired correctly
 * before chasing harder bugs.
 *
 * What success looks like:
 *
 *   [hello] Alp SDK hello-world starting
 *   [hello] tick 0
 *   [hello] tick 1
 *   ...
 *   [hello] tick 4
 *   [hello] done
 *
 * Where the output lands depends on the SoM:
 *
 *   * E1M-AEN (Alif Ensemble) -- the SoC's first USART, surfaced
 *     on the EVK board as the FTDI USB-UART (open at 115200 8N1).
 *   * E1M-V2N / V2N-M1        -- the Renesas SCIF console; same FTDI
 *     bridge on the E1M-X-EVK at 115200 8N1.
 *   * native_sim host build   -- stdout of the host binary.
 *
 * The SDK doesn't ship its own alp_log API yet (planned post-1.0);
 * use printf or Zephyr's LOG_* macros directly.  This example uses
 * printf for clarity -- it lands on whatever Zephyr has picked as
 * its console (CONFIG_LOG_BACKEND_UART under the hood).
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include <alp/cap.h>
#include <alp/soc_caps.h>

/* Capped tick count keeps the native_sim run inside twister's
 * timeout.  Real on-silicon firmware would loop forever instead --
 * see TICKS_ON_REAL_SILICON below.  We expose the cap as a macro
 * so customers see exactly what to change. */
#define HELLO_TICKS 5u

/* The wait between ticks.  k_msleep yields the CPU to other
 * threads + the Zephyr power-management subsystem during the wait
 * (unlike alp_delay_ms which falls through to a busy-loop on
 * baremetal targets where there's no scheduler). */
#define HELLO_TICK_PERIOD_MS 1000u

int main(void)
{
	/* The first line a bring-up engineer wants to see -- it
     * confirms the boot path made it to main() AND the printf
     * console is decoded correctly.  If THIS line is missing,
     * the toolchain / flash flow / console-cable is the suspect,
     * not the SDK. */
	printf("[hello] Alp SDK hello-world starting\n");

	/* Periodic loop -- prints a monotonically-increasing tick every
     * HELLO_TICK_PERIOD_MS so the customer can see the app is alive
     * (not just printed once + crashed).  Capped at HELLO_TICKS so
     * twister's console-harness one_line regex can latch the `done`
     * marker; real firmware would loop forever. */
	for (uint32_t tick = 0; tick < HELLO_TICKS; tick++) {
		printf("[hello] tick %u\n", tick);
		k_msleep(HELLO_TICK_PERIOD_MS);
	}

	/* TICKS_ON_REAL_SILICON: in production firmware you'd swap the
     * bounded for-loop above for:
     *
     *     for (uint32_t tick = 0; ; tick++) {
     *         printf("[hello] tick %u\n", tick);
     *         k_msleep(HELLO_TICK_PERIOD_MS);
     *     }
     *
     * Returning from main() on Zephyr is technically legal but the
     * kernel idles afterwards -- nothing keeps the heartbeat going. */

	/* Capability-API teaching block.
     *
     * `ALP_HAS()` is a compile-time constant expression.  Use it for
     * #if / static_assert -- the unused branch disappears entirely
     * from the binary, so this is zero-cost on parts that lack the
     * feature. */
#if ALP_HAS(HELIUM_MVE)
	printf("[hello] this build targets a Helium-capable SoC\n");
#else
	printf("[hello] no Helium MVE on this SoC -- scalar path\n");
#endif

	/* `alp_has()` is the runtime equivalent.  Useful when the same
     * binary may run on different SoCs (rare on Zephyr, common in
     * board-bringup tooling) or when the branch only matters for
     * logging rather than codegen. */
	if (alp_has(ALP_CAP_ID_HW_I2C)) {
		printf("[hello] HW I2C available (could probe sensors here)\n");
	} else {
		printf("[hello] no HW I2C on this SoC\n");
	}

	printf("[hello] done\n");
	return 0;
}
