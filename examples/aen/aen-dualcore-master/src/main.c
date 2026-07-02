/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-master -- B1 Option B: the SES-booted core starts the OTHER M55
 * at runtime via the portable alp_mproc_boot_core() (<alp/mproc.h>), instead of
 * a dual-boot ATOC (which boots only one core, #200). On this E8 the SES boots
 * the HP entry from a dual ATOC, so the HP build is the master that releases HE.
 *
 * The portable call resolves through the backend registry to the SoM's boot
 * authority -- on AEN that is the Secure Enclave's boot service over the
 * seservice0 MHU (the bench-proven route this example originally validated with
 * a direct vendor call).  Application code carries no vendor include.
 *
 * Board-aware: the HP build boots ALP_CORE_M55_HE @ 0x58000000; the HE build
 * boots ALP_CORE_M55_HP @ 0x50000000. Each stamps its own global-SRAM0 beacon +
 * the boot rc. The partner is the aen-dualcore-probe build for the other core,
 * packaged ["load"]-only (SES loads it but does not auto-boot it).
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/mproc.h>

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define TARGET_CORE ALP_CORE_M55_HE
#define TARGET_NAME "M55-HE"
#define TARGET_ADDR 0x58000000U /* HE ITCM global alias = HE-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02000018U)
#define SELF_MAGIC  0xB1B10090U
#else
#define ROLE        "HE"
#define TARGET_CORE ALP_CORE_M55_HP
#define TARGET_NAME "M55-HP"
#define TARGET_ADDR 0x50000000U /* HP ITCM global alias = HP-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02001018U)
#define SELF_MAGIC  0xB1B100E0U
#endif

int main(void)
{
	printk("\n=== aen-dualcore-master (%s) -- start the other M55 via alp_mproc_boot_core ===\n",
	       ROLE);

	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;

	alp_status_t rc = alp_mproc_boot_core(TARGET_CORE, TARGET_ADDR);

	BOOT_SLOT[0] = 0xB007C0DEU;
	BOOT_SLOT[1] = (uint32_t)rc;
	printk("alp_mproc_boot_core(%s, 0x%08x) rc=%d\n", TARGET_NAME, TARGET_ADDR, (int)rc);
	printk("RESULT: boot_core rc=%d -- read the OTHER core's beacon to confirm it runs\n", (int)rc);

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
		}
	}
	return 0;
}
