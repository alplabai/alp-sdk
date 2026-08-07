/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-mcuboot-smoke -- bench proof of the SES -> MCUboot -> slot0 -> app boot
 * chain on the E1M-AEN801 (Ensemble E8, M55-HE).  Built with --sysbuild against
 * zephyr/sysbuild/aen/sysbuild.conf; MCUboot signs this image into slot0
 * (MRAM 0x80010000).  Flashed as: MCUboot bootloader -> ATOC (SES launches it),
 * this signed image -> slot0.  If this app's banner appears in the RAM console,
 * MCUboot verified + chain-loaded it from slot0 -- the chain works.
 *
 * VTOR (SCB Vector Table Offset, 0xE000ED08) is reported to characterise WHERE
 * the app runs from after the chain-load (the open bench question vs the proven
 * SES->app-direct/ITCM path): slot0 XIP -> VTOR in [0x80010000, 0x802b0000);
 * copied-to-TCM -> VTOR low (ITCM 0x0 region).  Either way, reaching main()
 * means MCUboot booted slot0.
 *
 * The upper bound (0x802b0000) is HE's own slot0 window top, unchanged by
 * #1069's disjoint-slot0 fix -- it is NOT "slot1"/OTA any more (that concept
 * is gone on this SKU; 0x802b0000 is now where HP's OWN slot0 starts, a
 * separate physical window this HE-only build never touches).  See
 * metadata/e1m_modules/E1M-AEN801.yaml `memory_map:`.
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08U)

#define SLOT0_BASE 0x80010000U
#define HE_SLOT0_TOP 0x802b0000U /* == HP's own slot0 base since #1069, not "slot1" */

/* Latched in a known symbol so the human can also read it over J-Link mem32. */
volatile uint32_t g_vtor = 0xDEADBEEFU;

int main(void)
{
	g_vtor = SCB_VTOR;

	printk("\n=== aen-mcuboot-smoke ===\n");
	printk("VTOR = 0x%08x\n", g_vtor);

	bool slot0_xip = (g_vtor >= SLOT0_BASE) && (g_vtor < HE_SLOT0_TOP);

	printk("exec model: %s\n",
	       slot0_xip ? "XIP from slot0 (MRAM)" : "not slot0 (TCM/SRAM copy or direct)");
	printk("RESULT PASS: app reached main() -> SES->MCUboot->slot0->app chain OK "
	       "(VTOR=0x%08x, %s)\n",
	       g_vtor,
	       slot0_xip ? "slot0-XIP" : "relocated");

	return 0;
}
