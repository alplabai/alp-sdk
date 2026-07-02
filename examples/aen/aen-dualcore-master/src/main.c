/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dualcore-master -- B1 Option B: the SES-booted core starts the OTHER M55
 * at runtime via se_service_boot_cpu() over the seservice0 MHU, instead of a
 * dual-boot ATOC (which boots only one core, #200). On this E8 the SES boots the
 * HP entry from a dual ATOC, so the HP build is the master that releases HE.
 *
 * Board-aware: the HP build calls boot_cpu(EXTSYS_1=HE, 0x58000000); the HE build
 * calls boot_cpu(EXTSYS_0=HP, 0x50000000). Each stamps its own global-SRAM0
 * beacon + the boot rc. The partner is the aen-dualcore-probe build for the other
 * core, packaged ["load"]-only (SES loads it but does not auto-boot it).
 *
 * cpu_id enum (hal_alif services_lib_api.h:248-251): EXTSYS_0=2 M55-HP, EXTSYS_1=3 M55-HE.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <se_service.h>

/* Core-role selection (HP vs HE build of this app), not a peripheral-presence gate. */
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define ROLE        "HP"
#define TARGET_CPU  3U          /* EXTSYS_1 = M55-HE */
#define TARGET_ADDR 0x58000000U /* HE ITCM global alias = HE-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02000010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02000018U)
#define SELF_MAGIC  0xB1B10090U
#else
#define ROLE        "HE"
#define TARGET_CPU  2U          /* EXTSYS_0 = M55-HP */
#define TARGET_ADDR 0x50000000U /* HP ITCM global alias = HP-APP loadAddress */
#define SELF_BEACON ((volatile uint32_t *)0x02001010U)
#define BOOT_SLOT   ((volatile uint32_t *)0x02001018U)
#define SELF_MAGIC  0xB1B100E0U
#endif

int main(void)
{
	printk("\n=== aen-dualcore-master (%s) -- start the other M55 via se_service_boot_cpu ===\n",
	       ROLE);

	SELF_BEACON[0] = SELF_MAGIC;
	SELF_BEACON[1] = 0U;

	int rc = se_service_boot_cpu(TARGET_CPU, TARGET_ADDR);

	BOOT_SLOT[0] = 0xB007C0DEU;
	BOOT_SLOT[1] = (uint32_t)rc;
	printk("se_service_boot_cpu(cpu=%u, 0x%08x) rc=%d\n", TARGET_CPU, TARGET_ADDR, rc);
	printk("RESULT: boot_cpu rc=%d -- read the OTHER core's beacon to confirm it runs\n", rc);

	for (uint32_t hb = 1U;; hb++) {
		SELF_BEACON[1] = hb;
		for (volatile uint32_t d = 0U; d < 200000U; d++) {
		}
	}
	return 0;
}
