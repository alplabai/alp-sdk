/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dma-regcheck -- on-silicon validation of the ARM PL330 DMA controller on
 * the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console
 * flow.  Unlike the bind-only *-regcheck siblings (CAN/camera/ISP, which are
 * HW-blocked on missing wiring), this app performs a REAL memory-to-memory copy:
 * the PL330 needs no external pins or transceiver, so it actually runs the DMA
 * and verifies the bytes landed.
 *
 * ===========================  WHAT THIS VALIDATES  ==========================
 *   1. The dma2 DT node binds to UPSTREAM Zephyr's ARM PL330 driver
 *      (drivers/dma/dma_pl330.c, compatible "arm,dma-pl330") at the E8's
 *      core-local secure DMA2 base 0x400C0000 -- pure ADR 0017 Tier-1
 *      (upstream-native): the standard Zephyr dma_* class device, no vendored
 *      or forked code.
 *   2. The alp portable alias alp_dma0 resolves to that same node.
 *   3. The driver INSTANTIATES (DEVICE_DT_GET resolves, device_is_ready()).
 *   4. A real M2M transfer COPIES the bytes: dma_config(MEMORY_TO_MEMORY) +
 *      dma_start(), then a memcmp() of source vs destination.
 *
 * The upstream PL330 driver is a POLLING engine: dma_pl330_transfer_start()
 * calls dma_pl330_submit() -> dma_pl330_xfer() -> dma_pl330_wait(), which spins
 * on the channel-status CS0 register until the channel goes idle.  So when
 * dma_start() RETURNS 0 the copy has already completed -- no interrupt and no
 * completion callback are needed (and the binding declares no `interrupts`).
 * That is why this app verifies synchronously right after dma_start().
 *
 * ====================  THE #1 BENCH RISK: DMA REACHABILITY  =================
 * The PL330 is an AXI bus master.  It fetches its generated channel microcode
 * AND reads/writes the source/destination buffers over its OWN AXI master, so
 * EVERY address it touches must be a GLOBAL (AXI-visible) address -- NOT a
 * core-local TCM alias.  The M55's default RAM (.bss/.noinit) on the generated
 * board lands in DTCM (local 0x20000000), which the DMA AXI master cannot see.
 *
 * Fix (mirrors the eth_dwmac DMA-buffers-in-SRAM0 + the NPU arena-in-SRAM0
 * findings): src_buf/dst_buf are tagged into the "SRAM0" linker region -- global
 * on-chip SRAM @0x02000000 -- and the board overlay points the PL330 `microcode`
 * scratch at a carve-out in that same bank.  Combined with CONFIG_DCACHE=n
 * (prj.conf) the CPU and the DMA share one coherent view with no cache
 * maintenance.  If the buffers were left in DTCM the copy would read/write
 * garbage or fault; placing them in global SRAM0 is what makes the transfer
 * real.  See the board overlay header for the carve-out addresses.
 * ============================================================================
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

/* The PL330 controller node + the alp portable alias that must point at it. */
#define DMA_NODE  DT_NODELABEL(dma2)
#define DMA_ALIAS DT_ALIAS(alp_dma0)

/* Channel 0 of the 8 PL330 execution channels (dma-channels = <8>). */
#define DMA_CHANNEL 0U

/* Transfer size.  Deliberately not a multiple of the burst length so the
 * driver's residual-tail path is exercised too. */
#define XFER_LEN 1000U

/*
 * Expected facts, transcribed from the SoC dtsi (which carries the Alif AE822
 * DFP addresses) so a binding that resolved to the wrong node is caught.
 *
 *   reg base 0x400C0000 -- DMA2_SEC_BASE (DFP rtss_he/soc.h)
 *   dma-channels 8      -- PL330 execution channels (DFP DMA_*_CHANNEL_*[8])
 */
#define DMA_BASE_EXPECTED     0x400C0000U
#define DMA_CHANNELS_EXPECTED 8U

/*
 * Source + destination buffers MUST be in global, AXI-visible SRAM0 (see the
 * file + overlay headers): the PL330 AXI master cannot reach the M55's DTCM.
 * The "SRAM0" section is the sram0 node's linker region (@0x02000000).
 */
static uint8_t src_buf[XFER_LEN] __attribute__((section("SRAM0")));
static uint8_t dst_buf[XFER_LEN] __attribute__((section("SRAM0")));

/*
 * Compile-time staging fact: the node exists, is enabled, and binds to the
 * upstream PL330 compatible.
 */
#define DMA_BOUND                                                                                  \
	(DT_NODE_HAS_STATUS(DMA_NODE, okay) && DT_NODE_HAS_COMPAT(DMA_NODE, arm_dma_pl330))

/* The alp_dma0 alias must resolve to the SAME node the PL330 driver binds. */
#define ALIAS_OK (DT_NODE_EXISTS(DMA_ALIAS) && (DT_DEP_ORD(DMA_ALIAS) == DT_DEP_ORD(DMA_NODE)))

int main(void)
{
	printk("\n=== aen-dma-regcheck ===\n");

	/*
	 * Alif E8 prelude: DMA2 is the M55-core-LOCAL DMA.  The upstream
	 * arm,dma-pl330 driver is SoC-agnostic and never touches the Alif system-
	 * control block, so before it can run we have to do here exactly what the
	 * Alif CMSIS DMA driver does in its power-up path (DFP Driver_DMA.c
	 * DMA_PowerControl ARM_POWER_FULL, case DMA_INSTANCE_LOCAL):
	 *
	 *   (1) dmalocal_enable_periph_clk()        CLK_ENA   |= BIT(4)
	 *   (2) dmalocal_set_boot_manager_secure()  DMA_CTRL  &= ~BIT(0)   (0 = secure)
	 *   (3) dmalocal_set_boot_irq_ns_mask(0)    DMA_IRQ    = 0
	 *   (4) dmalocal_set_boot_periph_ns_mask(0) DMA_PERIPH = 0
	 *   (5) dmalocal_reset()                    DMA_CTRL  |= BIT(16)   (SW_RST)
	 *
	 * These registers live in the per-core M55_CFG_Common block.  On the M55-HE
	 * that block IS M55HE_CFG (DFP rtss_he/core_defines.h:
	 * M55LOCAL_CFG == (M55_CFG_Common_Type *)M55HE_CFG_BASE).  Clean-room values
	 * transcribed from the DFP (proprietary; values only, not source):
	 *   M55HE_CFG_BASE        0x43007000  -- DFP rtss_he/soc.h
	 *   DMA_CTRL    @ +0x00 |   DMA_IRQ @ +0x04 |  DMA_PERIPH @ +0x08
	 *   DMA_SEL     @ +0x0C |   CLK_ENA @ +0x10   -- DFP M55_CFG_Common_Type
	 *                                                (rtss_he/core_defines.h)
	 *   CLK_ENA_DMA_CKEN      BIT(4)       -- DFP drivers/include/sys_ctrl_dma.h
	 *   DMA_CTRL_BOOT_MANAGER BIT(0)  0=Secure 1=Non-Secure -- DFP sys_ctrl_dma.h
	 *   DMA_CTRL_SW_RST       BIT(16)      -- DFP drivers/include/sys_ctrl_dma.h
	 * Boot IRQ / boot peripheral non-secure masks are 0 (RTE_DMA2_BOOT_IRQ_NS_STATE
	 * / RTE_DMA2_BOOT_PERIPH_NS_STATE == 0 for AE822, DFP soc/AE822.../rte/
	 * RTE_Device.h) -- everything stays in the secure domain, matching the secure
	 * DMA2 base 0x400C0000 (DMA2_SEC_BASE) the M55-HE uses on the secure RAM-run.
	 *
	 * WHY the M2M copy did not land before this fix (clock-only prelude):
	 * step (1) alone clears the bus fault and lets the driver bind, but the PL330
	 * manager samples its boot security pins (boot_manager_ns / boot_irq_ns[] /
	 * boot_peripheral_ns[]) ONLY out of reset, from DMA_CTRL[0] / DMA_IRQ /
	 * DMA_PERIPH.  The upstream driver launches channel 0 with a *secure* DMAGO
	 * (DBGINST0 ns bit clear, since ch_handle->nonsec_mode == 0).  Per the ARM
	 * DMA-330 TRM a DMAGO that requests a security state the manager has not
	 * booted into is treated as DMANOP -- accepted with no fault, but the channel
	 * thread never starts.  That is exactly the observed signature: DBGCMD goes
	 * idle, FSRD/FSRC/FTR0 == 0, yet CS0 == STOP and SA0/DA0/CPC0 == 0 (channel
	 * never programmed).  Writing the secure boot config in (2)-(4) and then
	 * pulsing SW_RST in (5) re-boots the manager into the secure domain so the
	 * secure DMAGO is honoured and the channel executes the generated microcode.
	 *
	 * TODO(#21): fold this into the Tier-1.5 clockctrl so the device is clocked
	 * and boot-configured by its DT `clocks` phandle instead of an app-level poke.
	 */
#define M55HE_CFG_DMA_CTRL    0x43007000U /* M55_CFG_Common.DMA_CTRL   (+0x00) */
#define M55HE_CFG_DMA_IRQ     0x43007004U /* M55_CFG_Common.DMA_IRQ    (+0x04) */
#define M55HE_CFG_DMA_PERIPH  0x43007008U /* M55_CFG_Common.DMA_PERIPH (+0x08) */
#define M55HE_CFG_CLK_ENA     0x43007010U /* M55_CFG_Common.CLK_ENA    (+0x10) */
#define CLK_ENA_DMA_CKEN      BIT(4)      /* DFP CLK_ENA_DMA_CKEN              */
#define DMA_CTRL_BOOT_MANAGER BIT(0)      /* DFP DMA_CTRL_BOOT_MANAGER 0=Sec   */
#define DMA_CTRL_SW_RST       BIT(16)     /* DFP DMA_CTRL_SW_RST               */

	/* (1) ungate the DMA2 local peripheral clock. */
	sys_write32(sys_read32(M55HE_CFG_CLK_ENA) | CLK_ENA_DMA_CKEN, M55HE_CFG_CLK_ENA);

	/* (2) boot the PL330 manager SECURE (clear BOOT_MANAGER), matching the
	 * secure DMA2 base and the driver's secure DMAGO. */
	sys_write32(sys_read32(M55HE_CFG_DMA_CTRL) & ~DMA_CTRL_BOOT_MANAGER, M55HE_CFG_DMA_CTRL);

	/* (3)+(4) all boot IRQs and peripheral request lines stay secure. */
	sys_write32(0U, M55HE_CFG_DMA_IRQ);
	sys_write32(0U, M55HE_CFG_DMA_PERIPH);

	/* (5) pulse SW_RST so the manager re-samples the secure boot config above. */
	sys_write32(sys_read32(M55HE_CFG_DMA_CTRL) | DMA_CTRL_SW_RST, M55HE_CFG_DMA_CTRL);

	printk("prelude: DMA2 local clock+secure-boot configured "
	       "(CLK_ENA|=BIT4, DMA_CTRL secure-mgr, DMA_IRQ=DMA_PERIPH=0, SW_RST)\n");

	/*
	 * Step 1+2: report the node's binding + reg base + channel count.  These are
	 * build-time constants pulled from the bound node; a mismatch vs the DFP
	 * address means the binding resolved to the wrong node.
	 */
	uint32_t dma_base     = (uint32_t)DT_REG_ADDR(DMA_NODE);
	uint32_t dma_channels = (uint32_t)DT_PROP(DMA_NODE, dma_channels);

	printk("dma2  : %s\n", DT_NODE_FULL_NAME(DMA_NODE));
	printk("        bound=%d compat=arm,dma-pl330 base=0x%08x (exp 0x%08x) channels=%u (exp %u)\n",
	       (int)DMA_BOUND,
	       dma_base,
	       DMA_BASE_EXPECTED,
	       dma_channels,
	       DMA_CHANNELS_EXPECTED);
	printk("alias : alp_dma0 -> %s (resolves_to_dma2=%d)\n",
	       DT_NODE_FULL_NAME(DMA_ALIAS),
	       (int)ALIAS_OK);

	/*
	 * Step 3: probe the instantiated driver.  DEVICE_DT_GET resolves at build
	 * time; device_is_ready() reports the PL330 init result (it only sets up the
	 * per-channel mcode_base + mutexes -- no bus access at init).
	 */
	const struct device *dma_dev = DEVICE_DT_GET(DMA_NODE);
	bool                 ready   = device_is_ready(dma_dev);

	printk("dma2  device : %s\n",
	       ready ? "READY (arm,dma-pl330 driver instantiated)" : "NOT ready (init failed)");

	/*
	 * Step 4: the real work -- a memory-to-memory copy through the PL330.
	 *
	 * Fill the source with a known ramp, clear the destination, then ask the
	 * DMA to copy.  The driver's M2M path reads only head_block's source/dest
	 * addresses, block_size, and the addr-adj modes (both INCREMENT for a
	 * straight copy); source/dest_data_size + burst_length are advisory (the
	 * driver computes its own burst geometry from the addresses + size).
	 */
	for (uint32_t i = 0; i < XFER_LEN; i++) {
		src_buf[i] = (uint8_t)(i + 1U);
	}
	memset(dst_buf, 0, sizeof(dst_buf));

	struct dma_block_config blk = {
		.source_address  = (uint32_t)(uintptr_t)src_buf,
		.dest_address    = (uint32_t)(uintptr_t)dst_buf,
		.block_size      = XFER_LEN,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj   = DMA_ADDR_ADJ_INCREMENT,
	};
	struct dma_config cfg = {
		.channel_direction   = MEMORY_TO_MEMORY,
		.source_data_size    = 1,
		.dest_data_size      = 1,
		.source_burst_length = 1,
		.dest_burst_length   = 1,
		.block_count         = 1,
		.head_block          = &blk,
	};

	bool xfer_ok  = false;
	int  rc_cfg   = -1;
	int  rc_start = -1;

	if (ready) {
		rc_cfg = dma_config(dma_dev, DMA_CHANNEL, &cfg);
		if (rc_cfg == 0) {
			/*
			 * Synchronous on the PL330 polling driver: when dma_start()
			 * returns 0 the channel has already gone idle (dma_pl330_wait
			 * polled CS0), so the copy is complete and we can verify now.
			 */
			rc_start = dma_start(dma_dev, DMA_CHANNEL);
			if (rc_start == 0) {
				xfer_ok = (memcmp(src_buf, dst_buf, XFER_LEN) == 0);
			}
		}
	}

	printk("xfer  : dma_config rc=%d dma_start rc=%d memcmp_match=%d (%u bytes M2M)\n",
	       rc_cfg,
	       rc_start,
	       (int)xfer_ok,
	       XFER_LEN);

	/*
	 * Diagnostic: dump the PL330 manager + channel-0 status after the transfer.
	 * Standard ARM PL330 (DMA-330) register offsets (ARM DDI 0424 TRM):
	 *   DSR  +0x000 manager status   DPC  +0x004 manager PC
	 *   FSRD +0x030 fault-status mgr  FSRC +0x034 fault-status chan (bit n = chan n)
	 *   FTRD +0x038 fault-type  mgr   FTR0 +0x040 fault-type  chan0
	 *   CS0  +0x100 chan0 status (bits[3:0] state: 0=STOP 1=EXEC 7=WFE Eh=FAULTING Fh=FLT_COMPLETING)
	 *   CPC0 +0x104 chan0 PC          SA0  +0x400 src addr     DA0 +0x404 dst addr
	 * A faulting channel (CS0[3:0]=Eh/Fh, FSRC bit0=1) explains a clean dma_start
	 * return with no bytes moved: the upstream polling driver treats "not running"
	 * as done. CPC0 + FTR0 localize WHERE/WHY it stopped.
	 */
	const uint32_t pl330 = DMA_BASE_EXPECTED;

	printk("pl330 : DSR=%08x DPC=%08x FSRD=%08x FSRC=%08x FTRD=%08x FTR0=%08x\n",
	       sys_read32(pl330 + 0x000), sys_read32(pl330 + 0x004), sys_read32(pl330 + 0x030),
	       sys_read32(pl330 + 0x034), sys_read32(pl330 + 0x038), sys_read32(pl330 + 0x040));
	printk("pl330 : CS0=%08x CPC0=%08x SA0=%08x DA0=%08x\n",
	       sys_read32(pl330 + 0x100), sys_read32(pl330 + 0x104), sys_read32(pl330 + 0x400),
	       sys_read32(pl330 + 0x404));

	/*
	 * PASS gate: the PL330 BINDS (dma2 -> arm,dma-pl330 at 0x400C0000 with 8
	 * channels), the alp_dma0 alias points at it, the driver is ready, AND a real
	 * memory-to-memory copy moved every byte (memcmp match).  This is a genuine
	 * end-to-end transfer, not a bind-only staging check.
	 */
	bool pass = DMA_BOUND && (dma_base == DMA_BASE_EXPECTED) &&
	            (dma_channels == DMA_CHANNELS_EXPECTED) && ALIAS_OK && ready && xfer_ok;

	if (pass) {
		printk("RESULT PASS: PL330 DMA WORKS -- dma2 binds to arm,dma-pl330 at 0x%08x "
		       "(%u channels), alp_dma0 alias points at it, and a %u-byte memory-to-memory "
		       "copy through channel %u verified byte-for-byte (src/dst in global SRAM0)\n",
		       DMA_BASE_EXPECTED,
		       DMA_CHANNELS_EXPECTED,
		       XFER_LEN,
		       DMA_CHANNEL);
	} else {
		printk("RESULT FAIL: PL330 DMA NOT validated "
		       "(bound=%d base=%d channels=%d alias=%d ready=%d xfer=%d -- a fact is "
		       "missing, the node bound to the wrong compatible/reg base, or the M2M "
		       "copy did not complete; if bind is OK but xfer failed, check that the "
		       "buffers + microcode are in global SRAM0, not DTCM)\n",
		       (int)DMA_BOUND,
		       (int)(dma_base == DMA_BASE_EXPECTED),
		       (int)(dma_channels == DMA_CHANNELS_EXPECTED),
		       (int)ALIAS_OK,
		       (int)ready,
		       (int)xfer_ok);
	}

	return 0;
}
