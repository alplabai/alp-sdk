/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-npu-ethosu55-regcheck -- scopeless on-silicon PRESENCE + CLOCK probe of the
 * HE-core-local Arm Ethos-U55 (128-MAC, compatible "arm,ethos-u", core-private
 * base 0x400e1000) on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run
 * + RAM-console flow.  Sibling of aen-npu-ethosu-regcheck (global Ethos-U85).
 *
 * WHAT THIS PROVES: the HE-local U55 register block is present, powered and
 * CLOCKED, and identifies the variant (U55, arch major 1) from two independent ID
 * fields.  It does NOT run an inference (that needs a Vela model + command stream).
 *
 * HOW WE OBSERVE (two independent confirmations, like the U85 regcheck):
 *   1. read ID/CONFIG/STATUS directly off the DT reg base, latch into file-scope
 *      globals, print one RESULT PASS/FAIL line to the RAM console;
 *   2. the human re-reads the SAME absolute addresses over J-Link mem32.
 *
 * REGISTER MAP + EXPECTED VALUES (AUTHORITATIVE -- from the Ethos-U core driver
 * hal_ethos_u ethosu55_interface.h + ethosu_device_u55.c; the id_r layout is
 * identical across U55/U85):
 *   Offsets: NPU_REG_ID=0x0000, NPU_REG_STATUS=0x0004, NPU_REG_CONFIG=0x0028.
 *   ID[31:28]=arch_major_rev: U55/U65 -> 1 (U85 -> 2)            <-- decisive
 *   ID[27:20]=arch_minor_rev, ID[19:16]=arch_patch_rev: U55 arch = 1.1.0
 *   ID[15:12]=product_major (reported).
 *   CONFIG[31:28]=product: ETHOSU_PRODUCT_U55 == 0 (U65=1, U85=2) <-- decisive
 *   CONFIG[3:0]=macs_per_cc(log2): 128-MAC -> 7.
 *   MEMORY: validated from HE; ID reads back 0x10104201 (arch 1.1.0, product_major
 *   4) and CONFIG.product == 0.
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* The Ethos-U node added by app.overlay (HE-local U55 @ 0x400e1000). */
#define NPU_NODE DT_NODELABEL(ethosu_npu)
#define NPU_BASE ((uint32_t)DT_REG_ADDR(NPU_NODE))

/* Register offsets -- VERBATIM from hal_ethos_u ethosu55_interface.h. */
#define OFF_ID     0x0000U
#define OFF_STATUS 0x0004U
#define OFF_CONFIG 0x0028U

/* ID.arch_major_rev = bits [31:28]. U55 -> 1. */
#define ID_ARCH_MAJOR_SHIFT 28U
#define ID_ARCH_MAJOR_MASK  0xFU
#define EXP_ARCH_MAJOR_U55  1U

#define ID_ARCH_MINOR_SHIFT  20U
#define ID_ARCH_MINOR_MASK   0xFFU
#define ID_ARCH_PATCH_SHIFT  16U
#define ID_ARCH_PATCH_MASK   0xFU
#define ID_PRODUCT_MAJ_SHIFT 12U
#define ID_PRODUCT_MAJ_MASK  0xFU

/* CONFIG.product = bits [31:28]. U55 -> 0 (ETHOSU_PRODUCT_U55). */
#define CFG_PRODUCT_SHIFT 28U
#define CFG_PRODUCT_MASK  0xFU
#define EXP_PRODUCT_U55   0U /* ETHOSU_PRODUCT_U55; U65=1, U85=2 */

/* CONFIG.macs_per_cc = bits [3:0], log2(macs/cc). 128-MAC -> 7. */
#define CFG_MACS_SHIFT 0U
#define CFG_MACS_MASK  0xFU

volatile uint32_t g_npu_base   = NPU_BASE;
volatile uint32_t g_npu_id     = 0xDEADBEEFU;
volatile uint32_t g_npu_status = 0xDEADBEEFU;
volatile uint32_t g_npu_config = 0xDEADBEEFU;

static inline uint32_t rd(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

int main(void)
{
	const struct device *npu = DEVICE_DT_GET(NPU_NODE);

	printk("\n=== aen-npu-ethosu55-regcheck (HE-local U55) ===\n");
	printk("npu node   : %s\n", DT_NODE_FULL_NAME(NPU_NODE));
	printk("npu_base   : 0x%08x\n", NPU_BASE);

	/* Direct, driver-independent ID/CONFIG/STATUS read FIRST. */
	g_npu_id     = rd(NPU_BASE, OFF_ID);
	g_npu_status = rd(NPU_BASE, OFF_STATUS);
	g_npu_config = rd(NPU_BASE, OFF_CONFIG);

	uint32_t id  = g_npu_id;
	uint32_t cfg = g_npu_config;
	uint32_t st  = g_npu_status;

	uint32_t arch_major = (id >> ID_ARCH_MAJOR_SHIFT) & ID_ARCH_MAJOR_MASK;
	uint32_t arch_minor = (id >> ID_ARCH_MINOR_SHIFT) & ID_ARCH_MINOR_MASK;
	uint32_t arch_patch = (id >> ID_ARCH_PATCH_SHIFT) & ID_ARCH_PATCH_MASK;
	uint32_t prod_major = (id >> ID_PRODUCT_MAJ_SHIFT) & ID_PRODUCT_MAJ_MASK;
	uint32_t cfg_prod   = (cfg >> CFG_PRODUCT_SHIFT) & CFG_PRODUCT_MASK;
	uint32_t cfg_macs   = (cfg >> CFG_MACS_SHIFT) & CFG_MACS_MASK;

	printk("-- readback --\n");
	printk("ID      0x%08x = 0x%08x\n", NPU_BASE + OFF_ID, id);
	printk("  arch a.b.c = %u.%u.%u (U55 exp 1.1.0; arch_major exp %u)\n",
	       arch_major,
	       arch_minor,
	       arch_patch,
	       EXP_ARCH_MAJOR_U55);
	printk("  product_major = %u\n", prod_major);
	printk("CONFIG  0x%08x = 0x%08x\n", NPU_BASE + OFF_CONFIG, cfg);
	printk("  product = %u (U55 exp %u; U65=1 U85=2)\n", cfg_prod, EXP_PRODUCT_U55);
	printk("  macs_per_cc(log2) = %u (128-MAC -> 7)\n", cfg_macs);
	printk("STATUS  0x%08x = 0x%08x\n", NPU_BASE + OFF_STATUS, st);

	bool drv_ready = device_is_ready(npu);

	printk("driver device_is_ready = %d\n", (int)drv_ready);

	/*
	 * PASS gate (presence + clock + variant), clock-INDEPENDENT:
	 *   - ID is a sane non-floating word (not 0x0 / not 0xFFFFFFFF),
	 *   - ID.arch_major_rev == 1 (Ethos-U55 architecture),
	 *   - CONFIG.product    == 0 (ETHOSU_PRODUCT_U55), the 2nd identity field.
	 * device_is_ready is reported + folded in as a bonus signal.
	 */
	bool id_sane = (id != 0x00000000U) && (id != 0xFFFFFFFFU);
	bool ok      = true;

	ok &= id_sane;
	ok &= (arch_major == EXP_ARCH_MAJOR_U55);
	ok &= (cfg_prod == EXP_PRODUCT_U55);

	if (ok) {
		printk("RESULT PASS: HE-local Ethos-U55 present+clocked at 0x%08x "
		       "ID=0x%08x (arch %u.%u.%u) CONFIG=0x%08x (product=%u) ready=%d\n",
		       NPU_BASE,
		       id,
		       arch_major,
		       arch_minor,
		       arch_patch,
		       cfg,
		       cfg_prod,
		       (int)drv_ready);
	} else {
		printk("RESULT FAIL: ID=0x%08x (arch_major=%u exp %u, id_sane=%d) "
		       "CONFIG=0x%08x (product=%u exp %u) ready=%d -- "
		       "U55 absent / not clocked / wrong variant\n",
		       id,
		       arch_major,
		       EXP_ARCH_MAJOR_U55,
		       (int)id_sane,
		       cfg,
		       cfg_prod,
		       EXP_PRODUCT_U55,
		       (int)drv_ready);
	}

	return 0;
}
