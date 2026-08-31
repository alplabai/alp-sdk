/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ============================== STATUS ==============================
 * ADR 0017 Tier-1.5 (in-tree thin driver over the Apache-2.0 hal_alif OSPI
 * register library, modules/hal/alif drivers/ospi/{include,src}/ospi*.{c,h})
 * -- HW-BLOCKED, BUILD-ONLY this batch.  The fork ships no Zephyr OSPI
 * class driver either (only the DT binding), so this thin shell -- authored
 * here against the documented hal_alif API, no offset/bitfield open-coded --
 * is the only path to AEN OSPI.  See docs/adr/0017.
 *
 * The E1M-AEN801 (Ensemble E8) has no octal-NOR/HyperBus part populated this
 * hardware batch, so there is nothing on the bus to silicon-verify: this
 * driver's init reads its `struct ospi_init` straight out of the devicetree
 * node (reg/aes-reg/cs-pin/rx-ds-delay/ddr-drive-edge/bus-speed) and calls
 * alif_hal_ospi_initialize() ONCE at POST_KERNEL -- exercising the
 * controller-side register program (expected to complete now that the OSPI0
 * clock-enable below has landed and its gating mechanism is bench-measured,
 * see that block) and proving that hal_alif entry point compiles + links.
 * It does NOT call alif_hal_ospi_xip_enable() -- see the FOURTH section
 * below, that call bus-faults on AE822 and init must not fault regardless of
 * whether an app wants XiP -- and does NOT implement flash_driver_api
 * (read/write/erase/SFDP) -- that is a larger silicon-gated follow-up once a
 * part is populated, not this batch.  See examples/aen/aen-ospi-regcheck,
 * which exercises alif_hal_ospi_initialize() directly from application code
 * as an independent compile+link+reachability proof.
 *
 * core_clk: PREVIOUSLY a placeholder that fell back to the node's `bus-speed`
 * (100 MHz) when `clock-frequency` was unset -- WRONG, and hazardous: this
 * value feeds alif_hal_ospi_initialize()'s SCLK-divider derivation
 * (divider ~= core_clk / bus_speed), so feeding it core_clk == bus_speed
 * yields the minimum divider, i.e. SCLK programmed at or near the full core
 * clock. If the true core clock is higher than bus-speed, that is a
 * controller programmed to overclock whatever external device is on the
 * bus -- silently, on a fallback that looked like a degraded feature but
 * was not one. Fixed here: the true OSPI core-clock source is NOT an
 * unavailable HW fact -- HWRM Table 16-2 sources HEXSPI0's internal core
 * clock (OSPI0_CLK) from "SYST_ACLK or 266M_CLK", selected by
 * MISC_CLK_CTRL[SEL_OSPI_CLK]; HWRM 8.3.2.3.7 gives that bit's reset value
 * 0x0 as "400 MHz (SYST_ACLK)" (AHRM0012NDA v0.3). The ospi0 node now
 * declares `clock-frequency = <400000000>` (see
 * zephyr/dts/alif/ensemble_e8_peripherals.dtsi) and the fallback to
 * `bus-speed` below is REMOVED -- a board/SoM that omits `clock-frequency`
 * on its ospi0 node now fails to build instead of silently programming a
 * wrong divider; a wrong core_clk is a device-overclock, not a degraded
 * feature, so a build error is the correct failure mode.
 *
 * WHAT REMAINS UNADDRESSED: this driver still does not program
 * MISC_CLK_CTRL[SEL_OSPI_CLK] itself, so 400 MHz is correct only for as
 * long as whatever boot stage runs before it (ROM/SE) leaves that mux at
 * its documented silicon reset value. HWRM 8.3.2.3.7 also documents a 0x1
 * selection (266 MHz from 266M_CLK/PLL, additionally gated on
 * CLK_ENA[CLK266M]) -- if a future boot stage or SoM selects that path,
 * this driver's 400 MHz would again be wrong. Making the value
 * unconditionally correct means the driver programming SEL_OSPI_CLK itself
 * and deriving core_clk from the selection it just made -- a distinct
 * change that touches a shared system clock-mux register (MISC_CLK_CTRL is
 * not OSPI-instance-scoped) and needs bench confirmation of its placement
 * relative to the OSPI0 clock-enable sequence above; not done here.
 * ======================================================================
 *
 * ====== OSPI0 clock-enable (CLKCTL_PER_SLV->OSPI_CTRL) -- FIXES A
 * REPRODUCED BUS FAULT, ROOT CAUSE PER DFP, GATING SCOPE MEASURED ======
 * On AE822FA0E5597 (E8) the OSPI register window sits behind a per-instance
 * clock-enable gate that hal_alif's OSPI library never writes -- that library
 * targets parts without the gate.  Without it, alif_hal_ospi_initialize()'s
 * first register touch (ospi_set_tx_threshold() reading OSPI_TXFTLR, hal_alif
 * modules/hal/alif drivers/ospi/src/ospi.c:199 / ospi_hal.c:125) bus-faults:
 *
 *   ***** BUS FAULT ***** Precise data bus error, BFAR 0x83000018
 *
 * (base 0x83000000 + OSPI_TXFTLR offset 0x18 -- exactly BFAR; reproduced
 * identically on two bench runs of examples/aen/aen-ospi-regcheck).
 *
 * Per the DFP's own sequence, this write must happen BEFORE any OSPI
 * register touch:
 *   - AE822FA0E5597/include/soc_features.h:90 -- SOC_FEAT_OSPI_HAS_CLK_ENABLE (1)
 *     (AE722F80F55D5/soc_features.h:86 -- E7 has (0); the E7->E8 silicon delta)
 *   - ospi_xip/source/ospi/ospi_drv.c:305-307 -- vendor calls
 *     enable_ospi_clk(drv_instance) under that flag before any OSPI touch
 *   - drivers/include/sys_ctrl_ospi.h:45-48 -- the write itself:
 *     CLKCTL_PER_SLV->OSPI_CTRL |= (1 << drv_instance)
 *   - CLKCTL_PER_SLV_BASE 0x4902F000 (soc.h:3763), OSPI_CTRL offset 0x3C
 *     (soc.h:2584) -> OSPI0 = bit 0 of 0x4902F03C
 *   - OSPI0_BASE 0x83000000 (rtss_he/soc.h:3778; rtss_hp/soc.h:3784, same base)
 *
 * MEASURED 2026-07-28 (was UNVERIFIED -- the pending bench A/B below has now
 * run), READ HALF ONLY: every step below is a J-Link `mem32` READ -- no write
 * to the 0x8300_00xx OSPI register window has ever been tested from the
 * debugger, so the write-gating half of the APB-gating reading is UNMEASURED.
 * One held J-Link session on E1M-AEN801 / AE822 M55-HE, verbatim:
 *
 *   J-Link>mem32 0x4902F03C,1
 *   4902F03C = 00000000
 *   J-Link>mem32 0x83000018,1
 *   Could not read memory.
 *   J-Link>w4 0x4902F03C, 0x00000001
 *   Writing 00000001 -> 4902F03C
 *   J-Link>mem32 0x83000018,1
 *   83000018 = 00000000
 *   J-Link>mem32 0x83000000,1
 *   83000000 = 00C00407
 *
 * With CLKCTL_PER_SLV->OSPI_CTRL bit 0 clear, the debugger's own AHB-AP
 * *read* of the OSPI register window (0x83000018, OSPI_TXFTLR -- the same
 * offset that produced BFAR 0x83000018 above) aborts ("Could not read
 * memory"); with the bit set, the identical read succeeds, and a second
 * register read (0x83000000, CTRLR0) returns 0x00C00407 -- an alp-advisor
 * pass confirmed this equals the SVD's documented CTRLR0 resetValue
 * 0x00C00407 (alif-dfp-ref/Debug/SVD/AE822FA0E5597BS0_CM55_HE_View.svd:75097),
 * i.e. what the read exposes is the controller's power-on-reset value
 * becoming visible once the gate opens, not evidence any register was ever
 * written through it. This confirms the APB-gating reading for READS: bit 0
 * gates read access to the register interface, not merely the serial clock.
 * Whether it also gates WRITES is UNMEASURED -- no write to this window has
 * been attempted from the debugger.
 *
 * CORROBORATION: Alif_CMSIS/Source/Driver_OSPI.c:441 calls enable_ospi_clk()
 * in ARM_OSPI_PowerControl() immediately before ospi_set_tx_threshold(OSPI->regs,
 * ...) -- the identical first register touch whose OSPI_TXFTLR read produced
 * BFAR 0x83000018 here.  Tighter than the ospi_xip/ospi_drv.c:305-307 citation
 * above (same file, same call site, same register); kept alongside it.
 *
 * COUNTER-EVIDENCE CITATION (kept for context, does NOT carry over now that
 * OSPI_CTRL is independently measured above): zephyr/drivers/gpio/gpio_clk_alif.c:23-36
 * records that on this SAME SoC and this SAME CLKCTL_PER_SLV block,
 * GPIO_CTRL bit 16 (CKEN) was bench-measured NOT required for pad drive.
 * That bit was inert for pad drive; the OSPI0 A/B above demonstrates
 * OSPI_CTRL bit 0 is not inert -- it gates register access (AHB-AP read
 * aborts with it clear, succeeds with it set). Different bit, different
 * block function, different measured outcome -- the GPIO result was never
 * proof for OSPI and is retained only as the prior data point the OSPI
 * question was weighed against before its own bench A/B ran.
 *
 * aes-reg (0x83001000, ospi_hal.c:141) follows the same gate per the DFP
 * sequence -- no separate enable is written or expected.
 *
 * SECOND, DISTINCT FAULT (same 2026-07-28 bench session) -- ROOT-CAUSED
 * 2026-07-28 by alp-advisor, further into alif_hal_ospi_initialize() than
 * the CKEN fix above reaches:
 *
 *   ***** BUS FAULT *****
 *     Imprecise data bus error
 *   r0/a1:  0x83000000   r14/lr: 0x00006e17
 *   Faulting instruction address (r15/pc): 0x00006e20
 *
 * 0x00006e20 -> ospi_mode_master(), modules/hal/alif drivers/ospi/include/
 * ospi.h:477, inlined into alif_hal_ospi_initialize(), modules/hal/alif
 * drivers/ospi/src/ospi_hal.c:135.
 *
 * This CKEN fix is correct and complete with respect to everything the DFP
 * documents: an advisor pass exhausted the documented enable surface --
 * OSPI_CTRL has exactly ONE field, CKEN bit[0], resetMask 0x00000001 (SVD
 * lines 53038-53064); enable_ospi_clk() writes only (1 << drv_instance)
 * (alif-dfp-ref/drivers/include/sys_ctrl_ospi.h:45-48); and
 * SOC_FEAT_FORCE_ENABLE_SYSTEM_CLOCKS is (0) for this part
 * (AE822FA0E5597/include/soc_features.h:124). The second fault above is
 * therefore NOT a missing documented enable -- it is a separate, root-caused
 * issue.
 *
 * ROOT CAUSE: no MPU region in this build maps the OSPI register window
 * (0x83000000) as Device memory, so the CPU reaches it as NORMAL
 * write-through memory via the ARMv8-M PRIVDEFENA default map
 * (0x8000_0000-0x9FFF_FFFF = External RAM, Normal). ospi_mode_master()
 * (ospi.h:477, inlined here) brackets a protected register write with
 * ENR=0 ... write ... ENR=1 -- both ENR=0 and ENR=1 are the same word
 * (offset 0x08); under Normal attributes the M55 store buffer may merge or
 * reorder those stores before they drain, so the peripheral never sees the
 * disable and the protected write can land while ENR=1. MEASURED
 * 2026-07-28: a debugger CTRLR0 write while ENR=1 errors ("Failed to write
 * memory", read-back unchanged); the identical write with ENR=0 is
 * accepted -- an errored buffered write is exactly an imprecise BusFault,
 * taken wherever the pipeline had reached, which is also why the reported
 * PC wandered between bench runs. (TXFTLR and IMR are NOT in the protected
 * set measured this way -- narrower than the SVD's prose implies.) THE FIX
 * IS THE DT/MPU REGION, NOT THIS DRIVER: see `ospi_reg_region` in
 * zephyr/dts/alif/ensemble_e8_peripherals.dtsi, which maps this window (+
 * AES0/OSPI1/AES1) as ATTR_MPU_DEVICE.
 *
 * BENCH A/B RAN, MEASURED 2026-07-28: CONFIRMED for this fault -- this is a
 * real step forward, but it does NOT fully fix the OSPI path; see the THIRD,
 * DISTINCT FAULT section below. One held J-Link session, region in place:
 *
 *   RNR 3  RBAR 83000001  RLAR 83003FF7   OSPI_REG 0x83000000..0x83003FFF
 *                                         AttrIndx=3 -> MAIR0 Attr3=0x00 = Device-nGnRnE
 *   MPU_CTRL (0xE000ED94) = 00000005  (ENABLE | PRIVDEFENA)
 *   MPU_TYPE (0xE000ED90) = 00001000  -> DREGION = 16 regions, 4 in use
 *
 * The Device attribute is confirmed in force. The original fault above is
 * confirmed gone: alif_hal_ospi_initialize() now returns OSPI_ERR_NONE,
 * proven because execution reached the alif_hal_ospi_xip_enable() call this
 * driver made at the time (since removed, see FOURTH section), which was
 * only reachable past the `return -EIO` on a nonzero init rc. The abort also
 * changed character exactly as the Device attribute predicts:
 * from imprecise with a wandering PC (0x6e34, 0x6e20 across runs, as above)
 * to precise with a valid BFAR (see the THIRD, DISTINCT FAULT below) -- this
 * corroborates the store-buffer/ENR-ordering mechanism as this fault's real
 * cause. Do not read this as "OSPI is fixed": a second, distinct fault
 * remains further along, root cause open.
 *
 * Both reference SoC layers already carve this window out as Device
 * (zephyr_alif fork mpu_regions.c:10-11,105-106, region "OSPI_CTRL",
 * KB(16); Alif CMSIS mpu.c:97-99, MEMATTRIDX_DEVICE_nGnRE) -- the
 * upstream-Zephyr-based v4.4 port this repo builds against dropped it; this
 * is a port regression, not a silicon or driver defect.
 *
 * CORRECTED PREMISE: an earlier pass of this note argued no barrier was
 * needed because "ARMv8-M already orders Device-nGnRnE accesses to the same
 * peripheral" -- that premise is FALSE as stated here: the accesses at
 * fault time were NOT Device (that is the whole bug above). The conclusion
 * survives for a different reason -- once the region above maps this window
 * Device, ordering is architectural and no barrier is needed; a per-store
 * DSB would only mask the symptom (and the vendor's own AE822 sequences add
 * neither). Do not add a DSB/DMB/barrier or a speculative second enable
 * here.
 *
 * THIRD, DISTINCT FAULT (same MPU-region bench session, measured
 * 2026-07-28) -- further into alif_hal_ospi_xip_enable() than the region
 * fix above reaches. ROOT-CAUSED 2026-07-28 by an alp-advisor pass -- see the
 * FOURTH section below for the citation chain and the fix:
 *
 *   ***** BUS FAULT *****
 *     Precise data bus error
 *   BFAR Address: 0x8300010c
 *   r0/a1:  0x83000000   r1/a2:  0x83001000   r2/a3:  0x00000001
 *   Faulting instruction address (r15/pc): 0x00006d26
 *
 * 0x00006d26 -> ospi_control_xip_ss(), hal_alif modules/hal/alif drivers/
 * ospi/src/ospi.c:273; LR 0x00006e9f -> alif_hal_ospi_xip_enable(),
 * ospi_hal.c:412 (this driver called it here at POST_KERNEL at the time --
 * removed, see FOURTH section). The faulting instruction is a LOAD,
 * `ldr.w r4, [r0, #268]` (0x10C) -- the read half of the
 * `OSPI_XIP_SER &= ~(1 << slave)` read-modify-write; BFAR matches exactly.
 * OSPI_XIP_SER at offset 0x10C is cited from hal_alif's own ospi.c:273 (the
 * driver's own source), not the SVD.
 *
 * DECISIVE MEASUREMENT distinguishing this from the fault above: 0x8300010C
 * is unreadable from the debugger too (`mem32 0x8300010C,1` -> "Could not
 * read memory."), at BOTH ENR=0 and ENR=1, with the clock gate set -- and
 * it was already unreadable before anything was written. The ENR-bracket
 * registers at offsets 0xF0 / 0xF8 -- carried through as RX_SAMPLE_DELAY /
 * DDR_DRIVE_EDGE respectively, TBD pending SVD/DFP confirmation, unlike
 * OSPI_XIP_SER those two names are not independently cited here -- read
 * back fine at both ENR states in the same session (accepted with ENR=0,
 * "Failed to write memory" with ENR=1, value unchanged). So this fault is
 * NOT the store-buffer/ENR-ordering mechanism above: an address inside the
 * now-Device-mapped window simply does not respond to a read.
 *
 * PRACTICAL EFFECT AT THE TIME: examples/aen/aen-ospi-regcheck FAILED. The
 * fault was inside alif_hal_ospi_xip_enable(), called from this driver's
 * POST_KERNEL init (ospi_alif_init() below), so main() never ran -- the
 * fault was inside driver init, before application code started. See the
 * FOURTH section immediately below for the fix.
 *
 * FOURTH -- RESOLVED 2026-07-28: OSPI_XIP_SER (offset 0x10C) does not exist
 * on AE822. This is a real silicon delta, not an artifact of the missing
 * external part:
 *   - AE822FA0E5597/include/soc_features.h:89 -- SOC_FEAT_OSPI_HAS_XIP_SER
 *     (0). AE722F80F55D5/soc_features.h:85 -- E7 has (1) (this Zephyr port
 *     carries no E7 SoC layer today, so E7 is not reachable from this repo,
 *     but the flag is the E7->E8 silicon delta regardless).
 *   - Corroborating: soc_features.h:91 (AE822) --
 *     SOC_FEAT_OSPI_ADDR_IN_SINGLE_FIFO_LOCATION (1); the E7 header has it
 *     (0) at soc_features.h:87 -- exactly complementary to
 *     SOC_FEAT_OSPI_HAS_XIP_SER on both parts. AE822 addresses XiP through a
 *     single FIFO location instead of a per-slave XIP_SER register; there is
 *     nothing at offset 0x10C to program.
 *   - The AE822 SVD's OSPI0 register list goes OSPI_XIP_CTRL @ 0x108 ->
 *     OSPI_XIP_CNT_TIME_OUT @ 0x114 -- nothing at 0x10C or 0x110.
 *   - Alif's own driver wraps every OSPI_XIP_SER access in
 *     `#if SOC_FEAT_OSPI_HAS_XIP_SER` (drivers/source/ospi.c:185,
 *     ospi_xip/source/ospi/ospi_drv.c:244,283) -- hal_alif's ospi.c instead
 *     hardcodes the E7-era register map (include/ospi.h:85) and RMWs it
 *     unconditionally at ospi.c:273 (ospi_control_xip_ss(), reached from both
 *     ospi_xip_enable() and ospi_xip_disable()), guarded only by
 *     `#ifndef CONFIG_FLASH_ADDRESS_IN_SINGLE_FIFO_LOCATION` at ospi.c:810
 *     and :860 -- a symbol hal_alif itself never defines.
 *
 * FIX, two layers (a DT-only gate is not enough: a populated AE822 board
 * would fault identically, since the missing register is a property of the
 * die, not the BOM):
 *   1. THIS DRIVER no longer calls alif_hal_ospi_xip_enable() at
 *      POST_KERNEL at all (see ospi_alif_init() below) -- init must not
 *      fault regardless of what any given app wants. A devicetree-only gate
 *      was considered (DT_INST_NODE_HAS_PROP(inst, xip_base_address) --
 *      `xip-base-address` is optional per the vendored binding,
 *      snps,designware-ospi.yaml:86-88; a `jedec,spi-nor` child-node test was
 *      rejected because that binding declares no `bus:`, so a child flash
 *      node would not bind). Not used: the fork's own e1.dtsi sets
 *      xip-base-address unconditionally on ospi0 (a controller-side XiP
 *      window reservation, not a populated-part signal), so on THIS batch's
 *      dtsi the DT test would evaluate true regardless of what's on the bus
 *      and gate nothing. Removing the call outright is both simpler and
 *      actually correct for this hardware.
 *   2. hal_alif's own OSPI_XIP_SER touches are silenced for any Ensemble-E8
 *      build by defining CONFIG_FLASH_ADDRESS_IN_SINGLE_FIFO_LOCATION (see
 *      zephyr/CMakeLists.txt, gated on CONFIG_SOC_SERIES_E8) -- so a future
 *      caller of alif_hal_ospi_xip_enable()/_disable() on this part (this
 *      driver does not expose one yet) does not reintroduce the fault. Not
 *      done in hal_alif itself: it is an external module, not edited here.
 * Whether XiP then *functions* in single-FIFO-address mode on AE822 is TBD
 * pending a populated part -- out of scope here; the requirement met is only
 * "must not fault".
 *
 * INSTANCE DERIVATION: computed from the DT reg address, not hardcoded --
 * both instances are DFP-sourced and mapped to their enable_ospi_clk()
 * OSPI_INSTANCE bit (sys_ctrl_ospi.h:32-35, bit = drv_instance):
 *   - OSPI0_BASE 0x83000000 (rtss_he/soc.h:3778, rtss_hp/soc.h:3784) -> bit 0
 *   - OSPI1_BASE 0x83002000 (rtss_he/soc.h:3780, rtss_hp/soc.h:3786) -> bit 1
 * No ospi1 DT node exists in-tree today (only ospi0 is declared in the fork
 * e1.dtsi), so the second entry is currently unreachable -- kept anyway:
 * an unrecognized-base skip previously reproduced the identical bus fault it
 * was meant to avoid (see alif_ospi_clk_enable()), and unreachable-today is
 * exactly how that class of bug survives.
 *
 * Applies core-agnostically: OSPI_CTRL is a system-level CLKCTL_PER_SLV
 * register, not per-core -- the write covers both alp_e1m_aen801_m55_he and
 * ..._m55_hp, which also means concurrent init on both cores races on this
 * same register (sys_set_bit() is a plain non-atomic read-modify-write,
 * sys_bitops.h:24-29).  The DFP's own enable_ospi_clk() has the identical
 * hazard, so this mirrors the vendor's sequence rather than introducing a
 * new race -- not fixed here; deviating from that sequence is out of scope.
 * Written once in this driver's POST_KERNEL init, which also covers
 * aen-ospi-regcheck's direct alif_hal_ospi_initialize() call (both paths run
 * after this init has already executed).
 * ===================================================================
 */

#define DT_DRV_COMPAT snps_designware_ospi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <ospi_hal.h>

LOG_MODULE_REGISTER(flash_ospi_alif, CONFIG_FLASH_LOG_LEVEL);

struct ospi_alif_config {
	uint32_t *base_regs;
	uint32_t *aes_regs;
	uint32_t  bus_speed;
	uint32_t  core_clk;
	uint32_t  cs_pin;
	uint32_t  rx_ds_delay;
	uint32_t  ddr_drive_edge;
	uint16_t  xip_wait_cycles;
};

struct ospi_alif_data {
	HAL_OSPI_Handle_T handle;
};

/* CLKCTL_PER_SLV->OSPI_CTRL; see the file-header provenance block for the
 * full DFP citation chain (soc.h:3763 base + soc.h:2584 offset). */
#define ALIF_CLKCTL_OSPI_CTRL 0x4902F03Cu

/* OSPI reg bases, mapped to their CLKCTL_PER_SLV->OSPI_CTRL enable bit per
 * the DFP's enable_ospi_clk()/OSPI_INSTANCE enum (sys_ctrl_ospi.h:32-35,
 * bit = drv_instance); see the header note for the full citation chain and
 * why OSPI1 is currently unreachable (no ospi1 DT node in-tree). */
#define ALIF_OSPI0_BASE 0x83000000u /* rtss_he/soc.h:3778, rtss_hp/soc.h:3784 -- bit 0 */
#define ALIF_OSPI1_BASE 0x83002000u /* rtss_he/soc.h:3780, rtss_hp/soc.h:3786 -- bit 1 */

/*
 * Enable the CLKCTL_PER_SLV->OSPI_CTRL clock-gate bit for the OSPI instance
 * at base_regs, per the DFP's documented enable_ospi_clk()/sys_ctrl_ospi.h
 * sequence: CLKCTL_PER_SLV->OSPI_CTRL |= (1 << drv_instance).  Derived from
 * the reg address rather than hardcoded.  Returns -ENOTSUP on an
 * unrecognized base instead of silently skipping the write: skipping it
 * does not soften anything -- alif_hal_ospi_initialize() then walks
 * straight into the same first register touch that bus-faults without the
 * clock-enable, only now with the one explanatory LOG_WRN likely lost to
 * deferred-logging before the fault.  The caller turns this into a
 * device_is_ready()-visible init failure instead.
 */
static int alif_ospi_clk_enable(uint32_t base_regs)
{
	unsigned int bit;

	if (base_regs == ALIF_OSPI0_BASE) {
		bit = 0;
	} else if (base_regs == ALIF_OSPI1_BASE) {
		bit = 1;
	} else {
		LOG_WRN("ospi clk-enable: unrecognized OSPI base 0x%08x (only OSPI0 0x%08x / "
			"OSPI1 0x%08x are DFP-cited for this fix); refusing to touch OSPI "
			"registers -- they would bus-fault",
			base_regs, ALIF_OSPI0_BASE, ALIF_OSPI1_BASE);
		return -ENOTSUP;
	}

	sys_set_bit(ALIF_CLKCTL_OSPI_CTRL, bit);
	return 0;
}

static int ospi_alif_init(const struct device *dev)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_alif_data         *data   = dev->data;
	struct ospi_init init_cfg = {
		.bus_speed       = config->bus_speed,
		.core_clk        = config->core_clk,
		.cs_pin          = config->cs_pin,
		.rx_ds_delay     = config->rx_ds_delay,
		.ddr_drive_edge  = config->ddr_drive_edge,
		.baud2_delay     = OSPI_BAUD2_DELAY_AUTO,
		.base_regs       = config->base_regs,
		.aes_regs        = config->aes_regs,
		.xip_wait_cycles = config->xip_wait_cycles,
	};
	int32_t rc;
	int     clk_rc;

	/* Must happen before the first OSPI register touch inside
	 * alif_hal_ospi_initialize() -- see the file-header provenance block. */
	clk_rc = alif_ospi_clk_enable((uint32_t)config->base_regs);
	if (clk_rc != 0) {
		return clk_rc;
	}

	rc = alif_hal_ospi_initialize(&data->handle, &init_cfg);
	if (rc != OSPI_ERR_NONE) {
		LOG_ERR("alif_hal_ospi_initialize failed: %d", rc);
		return -EIO;
	}

	/*
	 * alif_hal_ospi_xip_enable() is deliberately NOT called here anymore --
	 * see the FOURTH section of the file-header provenance block. It
	 * bus-faults on AE822 (SOC_FEAT_OSPI_HAS_XIP_SER=0: the register is
	 * absent from the die, not merely unmapped), and POST_KERNEL runs before
	 * main(), so calling it here bricks every app that enables this node
	 * regardless of what the app itself needs. Init ends at a successful
	 * controller program; XiP setup is left to a future caller that actually
	 * has a populated part to program it for.
	 */

	return 0;
}

#define OSPI_ALIF_INIT(inst)                                                                      \
	static struct ospi_alif_data         ospi_alif_data_##inst;                                  \
	static const struct ospi_alif_config ospi_alif_config_##inst = {                             \
		.base_regs       = (uint32_t *)DT_INST_REG_ADDR(inst),                                   \
		.aes_regs        = (uint32_t *)DT_INST_PROP_BY_IDX(inst, aes_reg, 0),                    \
		.bus_speed       = DT_INST_PROP(inst, bus_speed),                                        \
		.core_clk        = DT_INST_PROP(inst, clock_frequency),                                  \
		.cs_pin          = DT_INST_PROP(inst, cs_pin),                                           \
		.rx_ds_delay     = DT_INST_PROP(inst, rx_ds_delay),                                       \
		.ddr_drive_edge  = DT_INST_PROP(inst, ddr_drive_edge),                                    \
		.xip_wait_cycles = DT_INST_PROP(inst, xip_wait_cycles),                                   \
	};                                                                                             \
	DEVICE_DT_INST_DEFINE(inst, ospi_alif_init, NULL, &ospi_alif_data_##inst,                    \
			       &ospi_alif_config_##inst, POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(OSPI_ALIF_INIT)
