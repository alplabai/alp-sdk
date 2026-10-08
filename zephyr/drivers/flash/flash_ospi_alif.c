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
 * hardware batch, so there is nothing on the bus to silicon-verify there:
 * this driver's init reads its `struct ospi_init` straight out of the
 * devicetree node (reg/aes-reg/cs-pin/rx-ds-delay/ddr-drive-edge/bus-speed)
 * and calls alif_hal_ospi_initialize() ONCE at POST_KERNEL -- exercising the
 * controller-side register program (expected to complete now that the OSPI0
 * clock-enable below has landed and its gating mechanism is bench-measured,
 * see that block) and proving that hal_alif entry point compiles + links.
 * It does NOT call alif_hal_ospi_xip_enable() -- see the FOURTH section
 * below, that call bus-faults on AE822 and init must not fault regardless of
 * whether an app wants XiP.  See examples/aen/aen-ospi-regcheck, which
 * exercises alif_hal_ospi_initialize() directly from application code as an
 * independent compile+link+reachability proof.
 *
 * flash_driver_api (#915): E1M-AEN803 DOES fit an OSPI0 NOR (ISSI
 * IS25WX256-JHLE on CS1) and the read-side framing matches what issue #915's
 * own bench capture measured against it (CS1 @ 20 MHz) -- not yet re-run
 * from this tree, see the changelog fragment's "Not run here" paragraph.
 * `read_jedec_id`, `sfdp_read` and 4-byte-address `read` all use the DW-SSI
 * EEPROM-read transfer mode in 1-1-1 (standard single-lane) SPI, the frame
 * format the part answers in coming out of reset.
 *
 * `write`/`erase` (#915, this pass): bench attempts (issue #915 thread) show
 * WREN (06h) never sets WEL in 1-1-1 SPI on this part -- RDSR read 0x00
 * before and after WREN, from both TX-only and EEPROM-read mode, and a
 * follow-up ERASE (12h/21h) + PROGRAM sequence on a verified-blank sector
 * left it unchanged. The Alif DFP's own IS25WX256 driver
 * (components/Source/IS25WX256.c) never programs in 1-1-1 either -- it
 * first switches the part to Octal DDR (81h write volatile config, IO mode
 * 0xE7) and only then uses 84h program / 21h erase / 70h flag-status
 * polling; its own comment says the status register only reads correctly
 * after that switch. This pass follows that same sequence: `write()` and
 * `erase()` lazily switch the part (and this driver's own framing) to Octal
 * DDR on first use (`ospi_alif_ensure_octal_ddr()`), then use the DW-SSI
 * "FRF-defined" enhanced-SPI mode (SPI_CTRLR0.SPI_FRF = Octal,
 * OSPI_SPI_CTRLR0's INST_L/ADDR_L/WAIT_CYCLES/DDR_EN fields) to drive 84h
 * page-program (256 B pages) and 21h sector-erase (4 KiB, matching the
 * SFDP-measured erase type 1 / IS25WX256.h's FLASH_ISSI_SECTOR_SIZE),
 * polling 70h flag status to completion after each, with the same
 * fail-closed recovery (`ospi_alif_recover_transfer()`) as the existing read
 * path on any timeout. Octal DDR frames are 16 bits wide (2 bytes/frame),
 * matching the vendor driver's own uint16_t-typed data pointers in that
 * mode -- `write()`/`erase()`/the switched `read()` path all require even
 * lengths and reject odd ones with -EINVAL rather than silently padding.
 *
 * READ AFTER THE SWITCH: once Octal DDR is active the part no longer
 * answers 1-1-1 SPI at all (that is the whole point of the switch), so
 * `read()` checks `data->octal_ddr_active` and follows the part into Octal
 * DDR (opcode 7Ch, the vendor driver's CMD_READ_DATA) rather than switching
 * back to 1-1-1 per call -- switching back would need its own 81h write
 * every time and a failure mid-switch would leave read() unable to tell
 * which framing the part is actually in. `read_jedec_id()`/`sfdp_read()`
 * are UNCHANGED and stay 1-1-1-only: they are diagnostic reads this driver
 * never calls itself, and nothing here calls them after a write/erase.
 *
 * BENCH-VERIFIED (#915) on E1M-AEN803 serial 2026W36-0001: erase, program,
 * byte-for-byte readback and restore of one 4 KiB sector through
 * aen-ospi-regcheck's self-test. It is authored against the DFP's
 * documented IS25WX256 sequence and hal_alif's own SPI_CTRLR0 field
 * encoding (modules/hal/alif drivers/ospi/include/ospi.h,
 * drivers/ospi/include/ospi_hal.h) with no offset/bitfield open-coded
 * outside those two headers' own names -- same discipline as the existing
 * read path. See `ospi_alif_write()` / `ospi_alif_erase()` /
 * `ospi_alif_octal_switch_locked()` below.
 *
 * core_clk: PREVIOUSLY a placeholder that fell back to the node's `bus-speed`
 * (100 MHz) when `clock-frequency` was unset. This value feeds
 * alif_hal_ospi_initialize()'s whole-register write to OSPI_BAUDR (hal_alif
 * v2.3.0 modules/hal/alif drivers/ospi/include/ospi.h:507:
 * `ospi->OSPI_BAUDR = (clk / speed);`) -- core_clk == bus_speed wrote
 * OSPI_BAUDR = 1. HWRM AHRM0012NDA v0.3 S16.1.5.3.5 defines OSPI_BAUDR's
 * SCKDV field as bits 15:1 (bit 0 is RESERVED and forced to 0 on every
 * write, so the effective divider is the written value with its low bit
 * cleared): "If this field is set to all 0s, the serial output clock
 * (OSPI_SCLK) is disabled." A written value of 1 leaves SCKDV all zero --
 * the old fallback disabled OSPI_SCLK entirely. It was a DEAD BUS, not an
 * overclock. (An earlier revision of this fix claimed the old fallback
 * would overclock an external device past its rating; that claim was
 * wrong and is corrected here. HWRM S16.1.4.2 also makes SCLK <=
 * OSPI_CLK/2 a hardware property of Master mode, which makes an overclock
 * past the core clock structurally impossible regardless of what
 * core_clk / bus_speed evaluates to.)
 *
 * Fixed here: the true OSPI core-clock source is NOT an unavailable HW
 * fact -- HWRM Table 16-2 sources HEXSPI0's internal core clock
 * (OSPI0_CLK) from "SYST_ACLK or 266M_CLK", selected by
 * MISC_CLK_CTRL[SEL_OSPI_CLK]; HWRM 8.3.2.3.7 gives that bit's reset value
 * 0x0 as "400 MHz (SYST_ACLK)" (AHRM0012NDA v0.3). The ospi0 node now
 * declares `clock-frequency = <400000000>` (see
 * zephyr/dts/alif/ensemble_e8_peripherals.dtsi) and the fallback to
 * `bus-speed` below is REMOVED -- a board/SoM that omits `clock-frequency`
 * on its ospi0 node now fails to build instead of silently programming a
 * disabled clock. Worked through: 400000000 / 100000000 = 4, SCKDV = 2,
 * BAUDR = SCKDV x 2 = 4, SCLK = 400 / 4 = 100 MHz -- exactly the node's
 * bus-speed, and half the S10.5 200 MHz HEXSPI controller cap.
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
 *
 * DIVIDER INVARIANT: `clock-frequency` and `bus-speed` are both free DT
 * integers, and the HAL truncates their ratio into OSPI_BAUDR without
 * checking the result -- see OSPI_ALIF_CHECK_SCLK() below, which
 * BUILD_ASSERTs the same S16.1.5.3.5 semantics at compile time instead of
 * leaving a second silent-wrong-value path open for a plausible-looking
 * but wrong ratio.
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
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_ospi_alif.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_PINCTRL
#include <zephyr/drivers/pinctrl.h>
#endif

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
#ifdef CONFIG_PINCTRL
	const struct pinctrl_dev_config *pcfg;
#endif
};

struct ospi_alif_data {
	HAL_OSPI_Handle_T handle;
	/* Serializes flash_driver_api calls onto the shared OSPI controller --
	 * the cmd-then-read helper below owns the register file for the
	 * duration of one transfer and must not race a second caller. */
	struct k_mutex lock;
	/* Set once ospi_alif_ensure_octal_ddr() has switched the part (and
	 * this driver's own framing) to Octal DDR on the first write()/
	 * erase() call. See the file-header "READ AFTER THE SWITCH" note --
	 * read() branches on this to follow the part into Octal DDR instead
	 * of continuing to speak 1-1-1 SPI to a part that no longer answers
	 * it. Never cleared: this driver has no reason to switch back. */
	bool octal_ddr_active;
};

/* This driver polls hal_alif's IRQ pump because the current OSPI node does
 * not connect IRQ 96 to an NVIC handler. Keep the loop bounded so a
 * controller or bus regression reports an error instead of hanging the
 * caller. */
#define OSPI_ALIF_XFER_POLL_ITERATIONS 100000

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
		        base_regs,
		        ALIF_OSPI0_BASE,
		        ALIF_OSPI1_BASE);
		return -ENOTSUP;
	}

	sys_set_bit(ALIF_CLKCTL_OSPI_CTRL, bit);
	return 0;
}

/* alif_hal_ospi_cs_enable() intentionally refuses to touch SER while the
 * controller reports busy. That is correct during a healthy transfer, but
 * it cannot recover a timed-out one: returning with CS asserted would
 * poison the shared bus. Use the vendor register library's disable/mask/SS
 * helpers to force the controller idle without open-coding register
 * offsets or fields. Disabling the controller resets its FIFOs;
 * ospi_control_ss() leaves it enabled again for a later retry. */
static void ospi_alif_recover_transfer(const struct device *dev)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_regs              *regs   = (struct ospi_regs *)config->base_regs;

	ospi_disable(regs);
	ospi_mask_interrupts(regs);
	ospi_control_ss(regs, config->cs_pin, SPI_SS_STATE_DISABLE);
}

static int ospi_alif_init(const struct device *dev)
{
	const struct ospi_alif_config *config   = dev->config;
	struct ospi_alif_data         *data     = dev->data;
	struct ospi_init               init_cfg = {
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
#ifdef CONFIG_PINCTRL
	int pinctrl_rc;

	/* Mux the OSPI pads (SCLK/SS/DQ) before any OSPI register touch or
	 * clock-enable below -- this driver previously had no pinctrl support
	 * at all (issue #2041), leaving every consumer to mux the pads itself
	 * or silently not work. Pattern matched from spi_dw_alif.c's
	 * spi_dw_init(), which applies PINCTRL_STATE_DEFAULT as the first thing
	 * in its own init. The DT binding declares pinctrl-0/pinctrl-names
	 * required (zephyr/dts/bindings/ospi/snps,designware-ospi.yaml), so
	 * config->pcfg is never NULL here -- no NULL guard needed, unlike
	 * adc_alif.c's fixed-function-pad case. */
	pinctrl_rc = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (pinctrl_rc != 0) {
		LOG_ERR("pinctrl_apply_state failed: %d", pinctrl_rc);
		return pinctrl_rc;
	}
#endif

	k_mutex_init(&data->lock);

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

/* #915: program/erase, over the Octal DDR mode switch -- see the
 * file-header flash_driver_api note. Opcodes and register field values below
 * are the DFP's IS25WX256 sequence (components/Source/IS25WX256.c) expressed
 * through hal_alif's own SPI_CTRLR0 field names (ospi.h / ospi_hal.h). */

/* Opcodes used only once the part has left 1-1-1 SPI for Octal DDR. */
#define OSPI_ALIF_CMD_WREN             0x06U
#define OSPI_ALIF_CMD_WRITE_VOL_CFG    0x81U
#define OSPI_ALIF_CMD_PAGE_PROGRAM     0x84U
#define OSPI_ALIF_CMD_SECTOR_ERASE     0x21U
#define OSPI_ALIF_CMD_READ_FLAG_STATUS 0x70U
#define OSPI_ALIF_CMD_OCTAL_READ       0x7CU /* vendor CMD_READ_DATA, Octal DDR fast-read */

/* IS25WX256 volatile-config addresses (IS25WX256.c IO_MODE_ADDRESS /
 * WAIT_CYCLE_ADDRESS) and the Octal DDR IO-mode value (OCTAL_DDR). */
#define OSPI_ALIF_VOLCFG_ADDR_IO_MODE  0x00000000U
#define OSPI_ALIF_VOLCFG_ADDR_WAIT_CYC 0x00000001U
#define OSPI_ALIF_OCTAL_DDR_IO_MODE    0xE7U

/* Octal DDR array-read dummy cycles, programmed into the part's volatile
 * wait-cycle config at the switch and used for every 7Ch read afterwards --
 * the DFP's RTE_ISSI_FLASH_WAIT_CYCLES for this SoC (16). Deliberately NOT
 * the DT `xip-wait-cycles`: that property is the controller's XiP knob
 * (255 in ensemble_e8_peripherals.dtsi), which overflowed the 5-bit
 * SPI_CTRLR0.WAIT_CYCLES field and wrote 0xFF wait cycles into the part, so
 * every array read came back 0xFF and a successful program looked like a
 * no-op (bench, #915). */
#define OSPI_ALIF_OCTAL_READ_WAIT_CYCLES 16U

/* Flag-status register (70h) bits (IS25WX256.c FLAG_STATUS_BUSY/_ERROR).
 * Counter-intuitively, bit 7 set means the program/erase controller is
 * READY, not busy -- kept exactly as the vendor driver tests it
 * (`(val & FLAG_STATUS_BUSY) != 0` clears its own `busy` flag). */
#define OSPI_ALIF_FLAG_STATUS_READY 0x80U
#define OSPI_ALIF_FLAG_STATUS_ERROR 0x30U

#define OSPI_ALIF_PAGE_SIZE   256U
#define OSPI_ALIF_SECTOR_SIZE 4096U /* SFDP erase type 1, opcode 21h (4-byte) */

/* Same rationale as OSPI_ALIF_XFER_POLL_ITERATIONS above: bound every poll
 * so a wedged part reports an error instead of hanging the caller. Flag
 * status is itself a bus transaction (opcode + dummy cycles + read), so this
 * bounds "poll attempts", not raw cycles. */
#define OSPI_ALIF_READY_POLL_ATTEMPTS      100000
#define OSPI_ALIF_OCTAL_STATUS_WAIT_CYCLES 8U /* IS25WX256.c ReadStatusReg() */

/*
 * Low-level Octal DDR ("FRF-defined" enhanced SPI) command helper. Pushes
 * the opcode and, if `has_addr`, the 32-bit address as their own DR0 entries
 * -- the DW-SSI enhanced-SPI FIFO protocol takes instruction and address as
 * one push each regardless of the data frame size, only `data`/`out`
 * afterward are chunked at the programmed data-frame size (16 bits here, 2
 * bytes/frame, matching the vendor driver's uint16_t-typed buffers in this
 * mode) -- then either sends `data_len` bytes (write direction) or receives
 * `out_len` bytes (read direction); never both in one call. Mirrors
 * `ospi_alif_cmd_read_locked()`'s polling/recovery shape but for the octal
 * frame format instead of standard 1-1-1.
 */
static int ospi_alif_octal_xfer_locked(const struct device *dev,
                                       uint8_t              opcode,
                                       bool                 has_addr,
                                       uint32_t             addr,
                                       uint32_t             wait_cycles,
                                       const uint8_t       *data,
                                       size_t               data_len,
                                       uint8_t             *out,
                                       size_t               out_len)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_regs              *regs   = (struct ospi_regs *)config->base_regs;
	uint32_t                       ctrlr0;
	size_t                         frames;
	size_t                         got = 0U;
	int                            spin;
	int                            rc = 0;

	if (data_len != 0U && out_len != 0U) {
		return -EINVAL; /* one direction per call */
	}
	if ((data_len % 2U) != 0U || (out_len % 2U) != 0U) {
		return -EINVAL; /* Octal DDR frames are 2 bytes; caller pads */
	}
	if (ospi_busy(regs)) {
		ospi_alif_recover_transfer(dev);
		if (ospi_busy(regs)) {
			return -EBUSY;
		}
	}

	frames = (out_len != 0U) ? (out_len / 2U) : (data_len / 2U);

	ospi_disable(regs);
	ctrlr0 = regs->OSPI_CTRLR0;
	ctrlr0 &= ~(SPI_CTRLR0_DFS_MASK | SPI_CTRLR0_FRF_MASK | SPI_CTRLR0_TMOD_MASK |
	            SPI_CTRLR0_SPI_FRF_MASK | SPI_CTRLR0_SPI_HYPERBUS_EN_SSTE_MASK);
	ctrlr0 |= SPI_CTRLR0_DFS_16bit | SPI_CTRLR0_FRF_MOTOROLA | SPI_CTRLR0_SPI_FRF_OCTAL |
	          ((out_len != 0U) ? SPI_CTRLR0_TMOD_EEPROM_READ_ONLY : SPI_CTRLR0_TMOD_SEND_ONLY);
	regs->OSPI_CTRLR0     = ctrlr0;
	regs->OSPI_CTRLR1     = (out_len != 0U) ? (uint32_t)(frames - 1U) : 0U;
	regs->OSPI_SPI_CTRLR0 = SPI_TRANS_TYPE_FRF_DEFINED |
	                        (SPI_CTRLR0_SPI_RXDS_ENABLE << SPI_CTRLR0_SPI_RXDS_EN_OFFSET) |
	                        (OSPI_DDR_ENABLE << SPI_CTRLR0_SPI_DDR_EN_OFFSET) |
	                        (OSPI_INST_LENGTH_8_BITS << SPI_CTRLR0_INST_L_OFFSET) |
	                        ((has_addr ? OSPI_ADDR_LENGTH_32_BITS : OSPI_ADDR_LENGTH_0_BITS)
	                         << SPI_CTRLR0_ADDR_L_OFFSET) |
	                        (wait_cycles << SPI_CTRLR0_WAIT_CYCLES_OFFSET);
	regs->OSPI_IMR        = 0U;
	regs->OSPI_SER        = 0U;
	ospi_enable(regs);

	regs->OSPI_DR0 = opcode;
	if (has_addr) {
		regs->OSPI_DR0 = addr;
	}
	if (out_len == 0U) {
		const uint16_t *tx16 = (const uint16_t *)data;

		for (size_t i = 0U; i < frames; i++) {
			regs->OSPI_DR0 = tx16[i];
		}
	}
	regs->OSPI_SER = BIT(config->cs_pin); /* starts the transfer */

	if (out_len != 0U) {
		uint16_t *rx16 = (uint16_t *)out;

		for (spin = 0; got < frames && spin < OSPI_ALIF_XFER_POLL_ITERATIONS; spin++) {
			while (got < frames && regs->OSPI_RXFLR != 0U) {
				rx16[got++] = (uint16_t)regs->OSPI_DR0;
				spin        = 0;
			}
		}
		if (got < frames) {
			LOG_ERR("octal ddr opcode 0x%02x read stalled at %u/%u frames",
			        opcode,
			        (unsigned int)got,
			        (unsigned int)frames);
			rc = -ETIMEDOUT;
		}
	} else {
		for (spin = 0; ospi_busy(regs) && spin < OSPI_ALIF_XFER_POLL_ITERATIONS; spin++) {
			/* drain until the controller reports idle */
		}
		if (ospi_busy(regs)) {
			LOG_ERR("octal ddr opcode 0x%02x send stalled", opcode);
			rc = -ETIMEDOUT;
		}
	}

	ospi_alif_recover_transfer(dev);
	return rc;
}

/* 1-1-1 standard-SPI TX-only send, used only by the Octal DDR switch itself
 * (the part is still in 1-1-1 SPI at that point). Mirrors
 * `ospi_alif_cmd_read_locked()`'s register program but TMOD_SEND_ONLY with
 * no RX phase. */
static int ospi_alif_std_send_locked(const struct device *dev, const uint8_t *cmd, size_t cmd_len)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_regs              *regs   = (struct ospi_regs *)config->base_regs;
	uint32_t                       ctrlr0;
	int                            spin;

	if (cmd_len == 0U || cmd_len > OSPI_TX_FIFO_DEPTH) {
		return -EINVAL;
	}
	if (ospi_busy(regs)) {
		ospi_alif_recover_transfer(dev);
		if (ospi_busy(regs)) {
			return -EBUSY;
		}
	}

	ospi_disable(regs);
	ctrlr0 = regs->OSPI_CTRLR0;
	ctrlr0 &= ~(SPI_CTRLR0_DFS_MASK | SPI_CTRLR0_FRF_MASK | SPI_CTRLR0_TMOD_MASK |
	            SPI_CTRLR0_SPI_FRF_MASK | SPI_CTRLR0_SPI_HYPERBUS_EN_SSTE_MASK);
	ctrlr0 |= SPI_CTRLR0_DFS_8bit | SPI_CTRLR0_FRF_MOTOROLA | SPI_CTRLR0_TMOD_SEND_ONLY |
	          SPI_CTRLR0_SPI_FRF_STANDRAD;
	regs->OSPI_CTRLR0 = ctrlr0;
	regs->OSPI_CTRLR1 = 0U;
	regs->OSPI_IMR    = 0U;
	regs->OSPI_SER    = 0U;
	ospi_enable(regs);

	for (size_t i = 0U; i < cmd_len; i++) {
		regs->OSPI_DR0 = cmd[i];
	}
	regs->OSPI_SER = BIT(config->cs_pin);

	for (spin = 0; ospi_busy(regs) && spin < OSPI_ALIF_XFER_POLL_ITERATIONS; spin++) {
		/* drain until the controller reports idle */
	}
	if (ospi_busy(regs)) {
		LOG_ERR("std spi send stalled (opcode 0x%02x)", cmd[0]);
		ospi_alif_recover_transfer(dev);
		return -ETIMEDOUT;
	}

	ospi_alif_recover_transfer(dev);
	return 0;
}

static int ospi_alif_write_enable_locked(const struct device *dev, bool octal)
{
	static const uint8_t wren = OSPI_ALIF_CMD_WREN;

	if (octal) {
		return ospi_alif_octal_xfer_locked(dev, OSPI_ALIF_CMD_WREN, false, 0, 0, NULL, 0, NULL, 0);
	}
	return ospi_alif_std_send_locked(dev, &wren, sizeof(wren));
}

/* Bounded poll of the Octal DDR flag-status register (70h) until the part
 * reports ready or ERASE/PROGRAM error. See OSPI_ALIF_FLAG_STATUS_READY's
 * comment for the (counter-intuitive, vendor-matched) polarity. */
static int ospi_alif_poll_ready_locked(const struct device *dev)
{
	uint16_t frame  = 0;
	uint8_t  status = 0;

	for (int i = 0; i < OSPI_ALIF_READY_POLL_ATTEMPTS; i++) {
		int rc = ospi_alif_octal_xfer_locked(dev,
		                                     OSPI_ALIF_CMD_READ_FLAG_STATUS,
		                                     false,
		                                     0,
		                                     OSPI_ALIF_OCTAL_STATUS_WAIT_CYCLES,
		                                     NULL,
		                                     0,
		                                     (uint8_t *)&frame,
		                                     sizeof(frame));

		if (rc != 0) {
			return rc;
		}
		status = (uint8_t)frame;
		if ((status & OSPI_ALIF_FLAG_STATUS_READY) != 0U) {
			return (status & OSPI_ALIF_FLAG_STATUS_ERROR) != 0U ? -EIO : 0;
		}
	}
	LOG_ERR("flag status (70h) never reported ready (last=0x%02x)", status);
	return -ETIMEDOUT;
}

/*
 * Switches the part -- and this driver's own controller framing -- from
 * 1-1-1 SPI to Octal DDR, mirroring the DFP's IS25WX256_PowerControl()
 * sequence: WREN + write volatile config[IO_MODE_ADDRESS] = OCTAL_DDR while
 * still in 1-1-1 SPI (the part's post-reset framing), then WREN + write
 * volatile config[WAIT_CYCLE_ADDRESS] a second time, now in Octal DDR
 * framing on both sides. Bench evidence (#915) is that RDSR always reads
 * 0x00 in 1-1-1 on this part, so -- like the DFP's own driver -- this does
 * NOT check WEL before either write; the first real correctness signal is
 * the caller's subsequent `ospi_alif_poll_ready_locked()` succeeding in the
 * new framing. Called with `data->lock` already held.
 */
static int ospi_alif_octal_switch_locked(const struct device *dev)
{
	uint8_t  io_mode_cmd[5];
	uint16_t wait_cyc;
	int      rc;

	rc = ospi_alif_write_enable_locked(dev, false);
	if (rc != 0) {
		return rc;
	}

	io_mode_cmd[0] = OSPI_ALIF_CMD_WRITE_VOL_CFG;
	io_mode_cmd[1] = (uint8_t)(OSPI_ALIF_VOLCFG_ADDR_IO_MODE >> 16);
	io_mode_cmd[2] = (uint8_t)(OSPI_ALIF_VOLCFG_ADDR_IO_MODE >> 8);
	io_mode_cmd[3] = (uint8_t)OSPI_ALIF_VOLCFG_ADDR_IO_MODE;
	io_mode_cmd[4] = OSPI_ALIF_OCTAL_DDR_IO_MODE;
	rc             = ospi_alif_std_send_locked(dev, io_mode_cmd, sizeof(io_mode_cmd));
	if (rc != 0) {
		return rc;
	}

	/* The part is now Octal DDR; switch this driver's framing to match and
	 * program the array-read wait cycles the 7Ch path below uses. */
	rc = ospi_alif_write_enable_locked(dev, true);
	if (rc != 0) {
		return rc;
	}
	wait_cyc =
	    (uint16_t)((OSPI_ALIF_OCTAL_READ_WAIT_CYCLES << 8) | OSPI_ALIF_OCTAL_READ_WAIT_CYCLES);
	return ospi_alif_octal_xfer_locked(dev,
	                                   OSPI_ALIF_CMD_WRITE_VOL_CFG,
	                                   true,
	                                   OSPI_ALIF_VOLCFG_ADDR_WAIT_CYC,
	                                   0,
	                                   (const uint8_t *)&wait_cyc,
	                                   sizeof(wait_cyc),
	                                   NULL,
	                                   0);
}

static int ospi_alif_ensure_octal_ddr(const struct device *dev)
{
	struct ospi_alif_data *data = dev->data;
	int                    rc   = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->octal_ddr_active) {
		rc = ospi_alif_octal_switch_locked(dev);
		if (rc == 0) {
			rc = ospi_alif_poll_ready_locked(dev);
		}
		if (rc == 0) {
			data->octal_ddr_active = true;
		} else {
			LOG_ERR("octal ddr switch failed: %d -- write/erase unavailable", rc);
		}
	}
	k_mutex_unlock(&data->lock);
	return rc;
}

int flash_ospi_alif_reset_notify(const struct device *dev)
{
	struct ospi_alif_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->octal_ddr_active = false;
	k_mutex_unlock(&data->lock);
	return 0;
}

static int ospi_alif_write(const struct device *dev, off_t offset, const void *buffer, size_t len)
{
	struct ospi_alif_data *data = dev->data;
	const uint8_t         *src  = buffer;
	int                    rc;

	if (len == 0U) {
		return 0;
	}
	if (offset < 0 || buffer == NULL || (len % 2U) != 0U) {
		/* Octal DDR data frames are 2 bytes wide; an odd length has no
		 * representation on this bus and this driver does not invent a
		 * padding byte on the caller's behalf. */
		return -EINVAL;
	}

	rc = ospi_alif_ensure_octal_ddr(dev);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	while (len > 0U) {
		size_t page_off = (size_t)offset % OSPI_ALIF_PAGE_SIZE;
		size_t chunk    = MIN(len, OSPI_ALIF_PAGE_SIZE - page_off);

		chunk &= ~(size_t)1U;
		if (chunk == 0U) {
			rc = -EINVAL;
			break;
		}

		rc = ospi_alif_write_enable_locked(dev, true);
		if (rc == 0) {
			rc = ospi_alif_octal_xfer_locked(
			    dev, OSPI_ALIF_CMD_PAGE_PROGRAM, true, (uint32_t)offset, 0, src, chunk, NULL, 0);
		}
		if (rc == 0) {
			rc = ospi_alif_poll_ready_locked(dev);
		}
		if (rc != 0) {
			ospi_alif_recover_transfer(dev);
			break;
		}

		src += chunk;
		offset += (off_t)chunk;
		len -= chunk;
	}
	k_mutex_unlock(&data->lock);
	return rc;
}

static int ospi_alif_erase(const struct device *dev, off_t offset, size_t size)
{
	struct ospi_alif_data *data = dev->data;
	int                    rc;

	if (size == 0U) {
		return 0;
	}
	if (offset < 0 || ((uint32_t)offset % OSPI_ALIF_SECTOR_SIZE) != 0U ||
	    (size % OSPI_ALIF_SECTOR_SIZE) != 0U) {
		return -EINVAL;
	}

	rc = ospi_alif_ensure_octal_ddr(dev);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	while (size > 0U) {
		rc = ospi_alif_write_enable_locked(dev, true);
		if (rc == 0) {
			rc = ospi_alif_octal_xfer_locked(
			    dev, OSPI_ALIF_CMD_SECTOR_ERASE, true, (uint32_t)offset, 0, NULL, 0, NULL, 0);
		}
		if (rc == 0) {
			rc = ospi_alif_poll_ready_locked(dev);
		}
		if (rc != 0) {
			ospi_alif_recover_transfer(dev);
			break;
		}

		offset += (off_t)OSPI_ALIF_SECTOR_SIZE;
		size -= OSPI_ALIF_SECTOR_SIZE;
	}
	k_mutex_unlock(&data->lock);
	return rc;
}

/*
 * write_block_size = 2, not 1: Octal DDR page-program only has a 2-byte
 * (16-bit-frame) granularity on this part/controller (see
 * `ospi_alif_write()`'s even-length requirement above) -- 1 would claim a
 * byte-granular program this driver cannot actually perform.
 *
 * page_layout (CONFIG_FLASH_PAGE_LAYOUT) is deliberately NOT implemented:
 * the erase unit here is 4 KiB (OSPI_ALIF_SECTOR_SIZE, SFDP erase type 1,
 * opcode 21h 4-byte) and `ospi_alif_erase()` enforces that alignment, but a
 * `flash_pages_layout` callback also needs the part's total array size,
 * which -- per the existing `read()` ponytail note a few hundred lines up
 * -- this driver deliberately does not hardcode from the one bench-known
 * IS25WX256; the DT binding carries no `size` property to add without
 * diverging from the vendored-verbatim binding. A real page_layout needs
 * SFDP BFPT density parsing at init, which #915 does not ask for.
 */
static const struct flash_parameters ospi_alif_parameters = {
	.write_block_size = 2,
	.erase_value      = 0xFF,
};

static const struct flash_parameters *ospi_alif_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &ospi_alif_parameters;
}

/*
 * Command-then-read over standard single-lane SPI, in the DW-SSI EEPROM-read
 * transfer mode (TMOD = 0b11): the controller shifts out what is in the TX
 * FIFO (opcode, address, dummy bytes) and then clocks in CTRLR1 + 1 frames.
 *
 * Not alif_hal_ospi_transfer(): that entry programs TMOD = RECEIVE_ONLY and
 * relies on the SPI_CTRLR0 instruction phase, which only exists in the
 * enhanced (dual/quad/octal) frame formats. In standard format a
 * receive-only transfer never shifts the opcode out, so the part never
 * answers and the HAL's completion callback never fires -- on silicon that
 * was a -ETIMEDOUT with an all-zero JEDEC ID, 3 of 3 (#915). The part sits in
 * 1-1-1 SPI after reset, so standard format is the right one; hal_alif just
 * has no command-then-read entry for it. Fields come from hal_alif's ospi.h.
 *
 * The TX FIFO is filled while no slave is selected, then SER starts the
 * transfer, so a multi-byte command cannot underflow the FIFO mid-command.
 * RX is drained as it arrives; one transfer is at most 65536 frames (the
 * CTRLR1 NDF field), so longer reads are split, each with its own command.
 */
#define OSPI_ALIF_MAX_FRAMES 65536U

static int ospi_alif_cmd_read_locked(const struct device *dev,
                                     const uint8_t       *cmd,
                                     size_t               cmd_len,
                                     uint8_t             *out,
                                     size_t               len)
{
	const struct ospi_alif_config *config = dev->config;
	struct ospi_regs              *regs   = (struct ospi_regs *)config->base_regs;
	uint32_t                       ctrlr0;
	size_t                         got = 0U;
	int                            spin;
	int                            rc = 0;

	if (len == 0U || len > OSPI_ALIF_MAX_FRAMES || cmd_len == 0U || cmd_len > OSPI_TX_FIFO_DEPTH) {
		return -EINVAL;
	}
	if (ospi_busy(regs)) {
		/* A prior caller's transfer may have stalled without this one's
		 * cmd_read_locked() being reached to run the usual post-transfer
		 * recovery below (e.g. a timeout mid-transfer left BUSY set). Force
		 * the controller idle once and retry the check instead of wedging
		 * every later caller behind a state nothing else can clear. */
		ospi_alif_recover_transfer(dev);
		if (ospi_busy(regs)) {
			return -EBUSY;
		}
	}

	ospi_disable(regs);
	ctrlr0 = regs->OSPI_CTRLR0;
	ctrlr0 &= ~(SPI_CTRLR0_DFS_MASK | SPI_CTRLR0_FRF_MASK | SPI_CTRLR0_TMOD_MASK |
	            SPI_CTRLR0_SPI_FRF_MASK | SPI_CTRLR0_SPI_HYPERBUS_EN_SSTE_MASK);
	ctrlr0 |= SPI_CTRLR0_DFS_8bit | SPI_CTRLR0_FRF_MOTOROLA | SPI_CTRLR0_TMOD_EEPROM_READ_ONLY |
	          SPI_CTRLR0_SPI_FRF_STANDRAD;
	regs->OSPI_CTRLR0 = ctrlr0;
	regs->OSPI_CTRLR1 = (uint32_t)(len - 1U);
	regs->OSPI_IMR    = 0U;
	regs->OSPI_SER    = 0U;
	ospi_enable(regs);

	for (size_t i = 0U; i < cmd_len; i++) {
		regs->OSPI_DR0 = cmd[i];
	}
	regs->OSPI_SER = BIT(config->cs_pin); /* starts the transfer */

	for (spin = 0; got < len && spin < OSPI_ALIF_XFER_POLL_ITERATIONS; spin++) {
		while (got < len && regs->OSPI_RXFLR != 0U) {
			out[got++] = (uint8_t)regs->OSPI_DR0;
			spin       = 0;
		}
	}
	if (got < len) {
		LOG_ERR("command 0x%02x read stalled at %u/%u bytes",
		        cmd[0],
		        (unsigned int)got,
		        (unsigned int)len);
		rc = -ETIMEDOUT;
	}

	/* Deassert CS, leave the controller idle and enabled; the same forced
	 * recovery covers a stalled read. */
	ospi_alif_recover_transfer(dev);
	return rc;
}

static int ospi_alif_cmd_read(const struct device *dev,
                              const uint8_t       *cmd,
                              size_t               cmd_len,
                              uint8_t             *out,
                              size_t               len)
{
	struct ospi_alif_data *data = dev->data;
	int                    rc;

	k_mutex_lock(&data->lock, K_FOREVER);
	rc = ospi_alif_cmd_read_locked(dev, cmd, cmd_len, out, len);
	k_mutex_unlock(&data->lock);
	return rc;
}

/* 1-1-1 READ with a 4-byte address (13h). The fitted IS25WX256 advertises it
 * in its SFDP 4-byte address instruction table (DWORD1 bit 0, read on
 * silicon as 43 0e ff ff), and a 4-byte opcode works whatever address mode
 * the part was left in. No dummy cycles at this bus rate.
 * ponytail: no per-part array-size bound -- the DT binding is vendored
 * VERBATIM from the fork (no `size` property to add without diverging from
 * it) and this driver serves whatever part is on the node, not just the
 * bench-known IS25WX256, so a hardcoded density would be wrong for a
 * different part; add a real bound from SFDP BFPT DWORD2 (parsed at init)
 * when a consumer needs it. What IS bounded below: the 4-byte address
 * itself, so an offset the command can't even encode fails loudly instead
 * of silently wrapping into a valid-looking, wrong address. */
#define OSPI_ALIF_READ4 0x13U

/*
 * Octal DDR read path (7Ch, the vendor CMD_READ_DATA), used once
 * write()/erase() has switched the part -- see the file-header "READ AFTER
 * THE SWITCH" note for why this driver follows the part into Octal DDR
 * rather than switching it back to 1-1-1 per call.
 */
static int ospi_alif_read_octal(const struct device *dev, off_t offset, uint8_t *out, size_t len)
{
	struct ospi_alif_data *data = dev->data;
	int                    rc   = 0;

	if ((len % 2U) != 0U) {
		return -EINVAL; /* odd length has no Octal-DDR-frame form */
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	while (len > 0U) {
		/* One RX FIFO's worth of 16-bit frames per chip-select: at Octal
		 * DDR rates the CPU poll loop cannot drain the FIFO as fast as
		 * the bus fills it, so a longer transfer overflows RX and stalls
		 * (bench, #915). The DFP caps its reads the same way
		 * (IS25WX256.c OSPI_MAX_RX_COUNT). */
		size_t chunk = MIN(len, (size_t)OSPI_RX_FIFO_DEPTH * 2U) & ~(size_t)1U;

		rc = ospi_alif_octal_xfer_locked(dev,
		                                 OSPI_ALIF_CMD_OCTAL_READ,
		                                 true,
		                                 (uint32_t)offset,
		                                 OSPI_ALIF_OCTAL_READ_WAIT_CYCLES,
		                                 NULL,
		                                 0,
		                                 out,
		                                 chunk);
		if (rc != 0) {
			break;
		}
		out += chunk;
		offset += (off_t)chunk;
		len -= chunk;
	}
	k_mutex_unlock(&data->lock);
	return rc;
}

static int ospi_alif_read(const struct device *dev, off_t offset, void *buffer, size_t len)
{
	struct ospi_alif_data *data = dev->data;
	uint8_t               *out  = buffer;

	if (offset < 0 || (uint64_t)offset > UINT32_MAX || (buffer == NULL && len != 0U)) {
		return -EINVAL;
	}
	if (len > 0U && (uint64_t)offset + (len - 1U) > UINT32_MAX) {
		return -EINVAL;
	}
	if (data->octal_ddr_active) {
		return ospi_alif_read_octal(dev, offset, out, len);
	}
	while (len > 0U) {
		size_t  chunk = MIN(len, (size_t)OSPI_ALIF_MAX_FRAMES);
		uint8_t cmd[] = {
			OSPI_ALIF_READ4,
			(uint8_t)((uint32_t)offset >> 24),
			(uint8_t)((uint32_t)offset >> 16),
			(uint8_t)((uint32_t)offset >> 8),
			(uint8_t)offset,
		};
		int rc = ospi_alif_cmd_read(dev, cmd, sizeof(cmd), out, chunk);

		if (rc != 0) {
			return rc;
		}
		out += chunk;
		offset += (off_t)chunk;
		len -= chunk;
	}
	return 0;
}

#if defined(CONFIG_FLASH_JESD216_API)
/* Standard JEDEC-ID read opcode (9Fh); the fitted IS25WX256 answers 9d 5b 19
 * (bench-captured, #915 / #2041). */
#define OSPI_ALIF_JEDEC_RDID   0x9FU
#define OSPI_ALIF_JEDEC_ID_LEN 3U

static int ospi_alif_read_jedec_id(const struct device *dev, uint8_t *id)
{
	static const uint8_t cmd[] = { OSPI_ALIF_JEDEC_RDID };

	if (id == NULL) {
		return -EINVAL;
	}
	return ospi_alif_cmd_read(dev, cmd, sizeof(cmd), id, OSPI_ALIF_JEDEC_ID_LEN);
}

/* JESD216 Read SFDP: 0x5A, 24-bit address, 8 dummy clocks (one dummy byte in
 * 1-1-1), then data. */
#define OSPI_ALIF_SFDP_READ 0x5AU

static int ospi_alif_sfdp_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	uint8_t *out = data;

	if (offset < 0 || (data == NULL && len != 0U)) {
		return -EINVAL;
	}
	while (len > 0U) {
		size_t  chunk = MIN(len, (size_t)OSPI_ALIF_MAX_FRAMES);
		uint8_t cmd[] = {
			OSPI_ALIF_SFDP_READ,
			(uint8_t)(offset >> 16),
			(uint8_t)(offset >> 8),
			(uint8_t)offset,
			0x00U, /* 8 dummy clocks */
		};
		int rc = ospi_alif_cmd_read(dev, cmd, sizeof(cmd), out, chunk);

		if (rc != 0) {
			return rc;
		}
		out += chunk;
		offset += (off_t)chunk;
		len -= chunk;
	}
	return 0;
}
#endif /* CONFIG_FLASH_JESD216_API */

static const struct flash_driver_api ospi_alif_api = {
	.read           = ospi_alif_read,
	.write          = ospi_alif_write,
	.erase          = ospi_alif_erase,
	.get_parameters = ospi_alif_get_parameters,
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read     = ospi_alif_sfdp_read,
	.read_jedec_id = ospi_alif_read_jedec_id,
#endif
};

/* (clock-frequency / bus-speed): see the file-header DIVIDER INVARIANT note. */
#define OSPI_ALIF_SCLK_DIV(inst) \
	(DT_INST_PROP(inst, clock_frequency) / DT_INST_PROP(inst, bus_speed))

/*
 * OSPI_BAUDR[SCKDV] is bits 15:1 of the register the HAL writes whole from
 * `core_clk / bus_speed`; bit 0 is RESERVED and hardware-clears on every
 * write, so the effective divider is the written value with its low bit
 * forced off, not OSPI_ALIF_SCLK_DIV(inst) itself (HWRM AHRM0012NDA v0.3
 * S16.1.5.3.5). A written value of 0 or 1 leaves SCKDV all zero, which
 * S16.1.5.3.5 defines as "the serial output clock (OSPI_SCLK) is
 * disabled" -- the exact dead-bus failure this file fixed for a MISSING
 * `clock-frequency`; these three catch it for a present but WRONG ratio
 * instead, at build time rather than on a populated part nobody scoped.
 */
#define OSPI_ALIF_CHECK_SCLK(inst) \
	BUILD_ASSERT(OSPI_ALIF_SCLK_DIV(inst) >= 2, \
	             "ospi" STRINGIFY( \
	                 inst) ": clock-frequency / bus-speed < 2 leaves " \
	                       "OSPI_BAUDR[SCKDV] all zero -- OSPI_SCLK disabled (HWRM S16.1.5.3.5)"); \
	BUILD_ASSERT(OSPI_ALIF_SCLK_DIV(inst) % 2 == 0, \
	             "ospi" STRINGIFY( \
	                 inst) ": clock-frequency / bus-speed is odd -- " \
	                       "OSPI_BAUDR[SCKDV]'s reserved bit 0 rounds the divider DOWN, so SCLK " \
	                       "silently OVERSHOOTS bus-speed"); \
	BUILD_ASSERT(DT_INST_PROP(inst, clock_frequency) / \
	                     (OSPI_ALIF_SCLK_DIV(inst) & ~1 ? OSPI_ALIF_SCLK_DIV(inst) & ~1 : 1) <= \
	                 200000000, \
	             "ospi" STRINGIFY(inst) ": resulting OSPI_SCLK exceeds the 200 MHz HEXSPI " \
	                                    "controller cap (HWRM S10.5)")

#define OSPI_ALIF_INIT(inst) \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(inst);)) \
	OSPI_ALIF_CHECK_SCLK(inst); \
	static struct ospi_alif_data         ospi_alif_data_##inst; \
	static const struct ospi_alif_config ospi_alif_config_##inst = { \
		.base_regs       = (uint32_t *)DT_INST_REG_ADDR(inst), \
		.aes_regs        = (uint32_t *)DT_INST_PROP_BY_IDX(inst, aes_reg, 0), \
		.bus_speed       = DT_INST_PROP(inst, bus_speed), \
		.core_clk        = DT_INST_PROP(inst, clock_frequency), \
		.cs_pin          = DT_INST_PROP(inst, cs_pin), \
		.rx_ds_delay     = DT_INST_PROP(inst, rx_ds_delay), \
		.ddr_drive_edge  = DT_INST_PROP(inst, ddr_drive_edge), \
		.xip_wait_cycles = DT_INST_PROP(inst, xip_wait_cycles), \
		IF_ENABLED(CONFIG_PINCTRL, (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), )) \
	}; \
	DEVICE_DT_INST_DEFINE(inst, \
	                      ospi_alif_init, \
	                      NULL, \
	                      &ospi_alif_data_##inst, \
	                      &ospi_alif_config_##inst, \
	                      POST_KERNEL, \
	                      CONFIG_FLASH_INIT_PRIORITY, \
	                      &ospi_alif_api);

DT_INST_FOREACH_STATUS_OKAY(OSPI_ALIF_INIT)
