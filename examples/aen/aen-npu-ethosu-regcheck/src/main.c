/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-npu-ethosu-regcheck -- scopeless on-silicon PRESENCE + CLOCK probe of the
 * Arm Ethos-U85 NPU (compatible "arm,ethos-u", ethosu85 @ 0x49042000) on the
 * E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * WHAT THIS PROVES (and what it does NOT):
 *   This confirms the NPU register block is present, powered, and CLOCKED -- a
 *   reachable, sensibly-decoded ID register is the canonical "the IP is alive"
 *   evidence. It identifies the variant (U85 vs the E8's other Ethos-U55) from
 *   two independent ID fields. It does NOT run an inference -- that needs the
 *   Ethos-U core driver lib (present, hal_ethos_u) PLUS a Vela-compiled model +
 *   command stream, which is the documented follow-up.
 *
 * HOW WE OBSERVE (two independent confirmations, like the SPI/PWM regchecks):
 *   1. This firmware reads the NPU ID + CONFIG + STATUS registers DIRECTLY off
 *      the DT reg base, decodes them, latches the raw words into file-scope
 *      globals (g_npu_id / g_npu_config / g_npu_status), and prints one RESULT
 *      PASS/FAIL line to the RAM console (read 'ram_console_buf' over J-Link
 *      mem8, ASCII-decode). The globals are also readable by symbol over SWD,
 *      so the evidence survives even if a later step were to fault.
 *   2. The human re-reads the SAME absolute addresses over J-Link mem32 (see the
 *      readback plan) -- so a driver/firmware that only PRINTS the right thing is
 *      caught.
 *
 * WHY WE READ THE ID DIRECTLY (not via ethosu_get_hw_info):
 *   The arm,ethos-u node is status="okay", so drivers/misc/ethos_u/ethos_u_arm.c
 *   auto-instantiates the device at POST_KERNEL and calls ethosu_init(), which
 *   does a NPU SOFT RESET and spins waiting for the NPU to come ready. If the NPU
 *   clock/power is NOT up (the ethosu85 node carries no `clocks` phandle -- see
 *   the overlay + unknowns), that init can hang BEFORE main() ever runs. To make
 *   this a robust presence probe regardless, main() reads the ID register itself
 *   from the raw base. (If init hangs, main() won't print -- but a direct J-Link
 *   mem32 read of the ID address per the readback plan still tells you whether the
 *   block is clocked. That dual path is the point.)
 *
 * REGISTER MAP + EXPECTED VALUES (AUTHORITATIVE -- derived from the Ethos-U core
 * driver, hal_ethos_u/src/ethosu85_interface.h + ethosu_device_u85.c; nothing
 * invented):
 *
 *   Offsets (NPU_REG_*, ethosu85_interface.h):
 *       NPU_REG_ID     = 0x0000   (the architecture / product / version ID word)
 *       NPU_REG_STATUS = 0x0004
 *       NPU_REG_CONFIG = 0x0028
 *
 *   ID register bitfields (struct id_r, ethosu85/ethosu55_interface.h -- identical
 *   layout across variants), LSB..MSB:
 *       [ 3: 0] version_status   (silicon RnPn -- NOT gated)
 *       [ 7: 4] version_minor    (silicon RnPn -- NOT gated)
 *       [11: 8] version_major    (silicon RnPn -- NOT gated)
 *       [15:12] product_major    (unique per base product -- reported)
 *       [19:16] arch_patch_rev   (U85 arch a.b.c patch -> 0)
 *       [27:20] arch_minor_rev   (U85 -> 0)
 *       [31:28] arch_major_rev   (U85 -> 2 ; U55/U65 -> 1)   <-- decisive
 *   The arch a.b.c constants are NNX_ARCH_VERSION_{MAJOR,MINOR,PATCH}:
 *       U85 = 2.0.0   U55 = 1.1.0   U65 = 1.0.6
 *   => ID[31:28] (arch_major_rev) == 2 IFF this is an Ethos-U85.
 *
 *   CONFIG register `product` field (struct config_r, ethosu85_interface.h):
 *       product = bits [31:28] (top nibble). The U85 device driver gates exactly
 *       on this: ethosu_device_u85.c -> `if (dev->reg->CONFIG.product !=
 *       ETHOSU_PRODUCT_U85)`, with ETHOSU_PRODUCT_U85 == 2 (and U55==0, U65==1).
 *   => CONFIG[31:28] (product) == 2 IFF this is an Ethos-U85.
 *
 * So we have TWO independent silicon-identity checks (ID.arch_major_rev and
 * CONFIG.product), both expected == 2 for the U85. A non-clocked / absent block
 * typically reads back 0x00000000 or 0xFFFFFFFF on this fabric -- either fails
 * both gates, which is the FAIL signal we want.
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* The Ethos-U node added by app.overlay. */
#define NPU_NODE DT_NODELABEL(ethosu_npu)

/* Absolute reg base straight from the overlay reg = <0x49042000 0x1000>, pulled
 * from devicetree so this stays correct if the node ever moves. */
#define NPU_BASE ((uint32_t)DT_REG_ADDR(NPU_NODE))

/* Register offsets -- VERBATIM from hal_ethos_u/src/ethosu85_interface.h. */
#define OFF_ID     0x0000U /* NPU_REG_ID     */
#define OFF_STATUS 0x0004U /* NPU_REG_STATUS */
#define OFF_CONFIG 0x0028U /* NPU_REG_CONFIG */

/* ID.arch_major_rev = bits [31:28]. U85 -> 2 (NNX_ARCH_VERSION_MAJOR). */
#define ID_ARCH_MAJOR_SHIFT 28U
#define ID_ARCH_MAJOR_MASK  0xFU
#define EXP_ARCH_MAJOR_U85  2U

/* ID sub-fields for reporting. */
#define ID_ARCH_MINOR_SHIFT  20U
#define ID_ARCH_MINOR_MASK   0xFFU
#define ID_ARCH_PATCH_SHIFT  16U
#define ID_ARCH_PATCH_MASK   0xFU
#define ID_PRODUCT_MAJ_SHIFT 12U
#define ID_PRODUCT_MAJ_MASK  0xFU

/* CONFIG.product = bits [31:28]. U85 -> 2 (ETHOSU_PRODUCT_U85). */
#define CFG_PRODUCT_SHIFT 28U
#define CFG_PRODUCT_MASK  0xFU
#define EXP_PRODUCT_U85   2U /* ETHOSU_PRODUCT_U85; U55=0, U65=1 */

/* CONFIG.macs_per_cc = bits [3:0], log2(macs/cc) -- reported (sanity). */
#define CFG_MACS_SHIFT 0U
#define CFG_MACS_MASK  0xFU

/*
 * Latched raw register words, in a known file-scope symbol the human can read by
 * name over J-Link (independent of the RAM-console decode). volatile so the
 * stores are not optimised away.
 */
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

	printk("\n=== aen-npu-ethosu-regcheck ===\n");
	printk("npu node   : %s\n", DT_NODE_FULL_NAME(NPU_NODE));
	printk("npu_base   : 0x%08x\n", NPU_BASE);

	/*
	 * Direct, driver-independent ID/CONFIG/STATUS read FIRST (see file header:
	 * survives even if the driver's ethosu_init() path misbehaves). Latch raw
	 * words into the globals immediately.
	 */
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
	printk("  arch a.b.c = %u.%u.%u (U85 exp 2.0.0; arch_major exp %u)\n",
	       arch_major,
	       arch_minor,
	       arch_patch,
	       EXP_ARCH_MAJOR_U85);
	printk("  product_major = %u\n", prod_major);
	printk("CONFIG  0x%08x = 0x%08x\n", NPU_BASE + OFF_CONFIG, cfg);
	printk("  product = %u (U85 exp %u; U55=0 U65=1)\n", cfg_prod, EXP_PRODUCT_U85);
	printk("  macs_per_cc(log2) = %u (256-MAC -> 8)\n", cfg_macs);
	printk("STATUS  0x%08x = 0x%08x\n", NPU_BASE + OFF_STATUS, st);

	/*
	 * The driver auto-inits the device at POST_KERNEL (status=okay). Report
	 * whether it came up READY -- a secondary signal. If ethosu_init() had
	 * hung, we'd never reach main(); reaching here with device_is_ready()==1
	 * means the full init (incl. soft reset) completed.
	 */
	bool drv_ready = device_is_ready(npu);

	printk("driver device_is_ready = %d\n", (int)drv_ready);

	/*
	 * PASS gate (presence + clock + variant), clock-INDEPENDENT:
	 *   - ID is a sane, non-floating word (not 0x00000000 / not 0xFFFFFFFF),
	 *   - ID.arch_major_rev == 2  (Ethos-U85 architecture),
	 *   - CONFIG.product   == 2  (ETHOSU_PRODUCT_U85), the second independent
	 *     identity field.
	 * device_is_ready is REPORTED and folded in as a bonus signal, but the
	 * decisive evidence is the two silicon-identity register fields -- so a
	 * present-but-init-quirky NPU still PASSES the presence/clock check.
	 */
	bool id_sane = (id != 0x00000000U) && (id != 0xFFFFFFFFU);
	bool ok      = true;

	ok &= id_sane;
	ok &= (arch_major == EXP_ARCH_MAJOR_U85);
	ok &= (cfg_prod == EXP_PRODUCT_U85);

	if (ok) {
		printk("RESULT PASS: Ethos-U85 present+clocked at 0x%08x "
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
		       "NPU absent / not clocked / wrong variant\n",
		       id,
		       arch_major,
		       EXP_ARCH_MAJOR_U85,
		       (int)id_sane,
		       cfg,
		       cfg_prod,
		       EXP_PRODUCT_U85,
		       (int)drv_ready);
	}

	return 0;
}
