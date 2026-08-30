# 0017. alp-sdk rides over the vendor SDK — no rewritten vendor drivers

Status: Accepted (amended 2026-06-15 — adds **Tier 1.5**; reclassifies the four AEN drivers; amended 2026-08-30 — corrects the E1M-AEN801 BRD_I2C routing; see "Amendment" below)
Date: 2026-06-15

alp-sdk's value is the portable `<alp/*>` unification layer plus the SoM/carrier
integration (board files, pin maps, provisioning, the multi-core orchestrator).
It is **not** a place to re-implement or fork silicon-vendor peripheral drivers.
This ADR makes that explicit after AEN (Alif Ensemble) bring-up drifted into
vendoring several Alif-specific Zephyr drivers, and sets the policy + the
migration back.

## Context

alp-sdk targets **one upstream Zephyr base** (v4.4.0) across all SoM families
(Alif / Renesas / NXP) plus each vendor's Apache-2.0 HAL module (`hal_alif`,
`hal_renesas`, `hal_nxp`); the full vendor Zephyr forks are opt-in `vendor-sdks`
manifest entries (see `west.yml`). Upstream Zephyr drives the **DesignWare**
(Synopsys) IP blocks the Alif Ensemble uses (GPIO, I2C, SPI, the eth_dwmac core)
directly.

During the AEN bring-up we filled the gap between "upstream + hal_alif" and the
Alif-specific peripherals upstream lacks by **vendoring/adapting Alif driver
code** into alp-sdk: the eth_dwmac platform glue (PR #149), the DWC-SSI SPI
driver (PR #150), the LPI2C TX-only driver (PR #148), and the UTIMER PWM/counter
drivers (PR #151). That is "rewriting the vendor" — a maintenance liability, a
licensing tangle (the Alif CMSIS DFP is under the Alif Software License
Agreement, not Apache-2.0), and a divergence risk against the vendor's own
updates. It also produced dead ends (e.g. an LPI2C "master" driver, when the
silicon's LPI2C0 is slave-only — see below).

## Decision

**alp-sdk sits over the vendor SDK. It does not ship rewritten vendor drivers.**
Every AEN peripheral falls into exactly one tier:

- **Tier 1 — upstream-native.** Blocks upstream Zephyr drives directly: GPIO
  (`gpio_dw`), edge I2C (`i2c_dw`), SPI core (`spi_dw`), Ethernet core
  (`eth_dwmac`). alp-sdk ships only the DT nodes (SoC overlay) + board overlays +
  the `<alp/*>` mapping. **No vendor code.** (GPIO and the edge DW I2C of PR #152
  are the reference examples.)

- **Tier 1.5 — in-tree thin driver over an Apache-2.0 HW library or an upstream
  core** *(added by the 2026-06-15 amendment)*. A small Zephyr-class driver
  alp-sdk keeps in-tree **only** where upstream **and** the opt-in fork ship no
  usable driver, and where the in-tree code links nothing but (a) an Apache-2.0
  vendor HW-register library (`hal_alif`) that exposes no Zephyr `struct device`,
  or (b) an upstream Zephyr core via its sanctioned platform-glue extension
  contract. It copies **no** fork driver logic and drags in **no** divergent
  fork core, so it keeps the one-upstream-base invariant intact. This tier exists
  because pure fork-consume (Tier 2) is *infeasible* for these peripherals —
  retiring them would drop real, build-verified AEN capability to `NOSUPPORT`
  with **no in-tree or fork replacement**, violating the "no removal before a
  proven replacement" rule below. Reference examples: the **UTIMER PWM/counter**
  drivers (thin shell over the Apache-2.0 `hal_alif` `alif_utimer_*` register
  library — flavor *a*; the fork ships UTIMER *bindings only*, no driver) and the
  **eth_dwmac platform glue** (upstream-shaped glue over the **upstream**
  `eth_dwmac` core via `dwmac_bus_init`/`dwmac_platform_init` — flavor *b*; the
  fork forked the DWMAC *core* itself, so consuming it would violate
  one-upstream-base). Each Tier-1.5 driver shipped marked *interim* until
  bench-verified on E8 silicon, then promotes to permanent; the UTIMER and
  eth_dwmac-glue drivers are now **bench-verified PASS on E8** (2026-06-17) and
  promoted.

- **Tier 2 — vendor-SDK-consumed.** Alif-specific peripherals upstream lacks
  **and** for which a genuine vendor *driver* exists to consume: the Alif LPI2C1
  master, the Alif PWM/ADC/DAC/ISP drivers, the **DWC-SSI SPI** flavor (a true
  fork-driver copy — `hal_alif` ships no SSI library). These come from the
  **opt-in Alif vendor SDK** (`sdk-alif` fork / the CMSIS DFP, in the
  `vendor-sdks` manifest group) that the customer adds. alp-sdk maps `<alp/*>`
  onto them when present and returns `sw_fallback`/`NOSUPPORT` otherwise.
  **alp-sdk does not vendor or rewrite them.**

- **Tier 3 — SE-mediated.** Housekeeping/security devices behind the Secure
  Enclave. On the E1M-AEN801 the on-module trio sits on the **slave-only LPI2C0**
  bus (the SoC is a slave at the hardcoded address 0x40; TRM §3.17.4), so the SE
  masters that bus. Such devices are read via **Alif SE services** (vendor SDK),
  surfaced through a portable board-info/manifest API — never an alp-sdk I2C
  master driver. *(SUPERSEDED for the E1M-AEN801's BRD_I2C trio — see
  "Amendment (2026-08-30)" below. The general Tier-3 category stands for any
  device genuinely behind the SE.)*

Bench-verification on real E8 silicon remains the acceptance gate for every tier.

## Consequences

- **The four interim AEN drivers are dispositioned per-tier, not lumped together**
  *(corrected by the 2026-06-15 amendment — the original "all four are Tier-2,
  migrate or hold" framing would have silently regressed three working
  peripherals; see "Amendment")*:
  - **#148 LPI2C-TX — retired.** Not interim: it is a master-TX-only copy against
    a bus the silicon can only be a **slave** on (LPI2C0 @0x40, TRM §3.17.4). It
    can never do a master read, has **zero** consumers, and the edge I2C master is
    already Tier-1 `i2c_dw`. Retiring loses no capability; the on-module
    RTC/TMP112 reads go **Tier-3 (SE)** (tasks #16/#17). The one driver whose
    removal ADR 0017-as-written genuinely justified.
  - **#149 eth glue — Tier-1.5, kept** (bench-verified PASS on E8, 2026-06-17 —
    end-to-end Ethernet with a DHCP lease and ARP-reachable; the GMAC DMA
    descriptor rings + net_buf pool must live in global SRAM0, not the DMA-invisible
    M55 DTCM). Fork-consume is a
    trap (the fork forked the DWMAC *core*); retiring it is an **unconditional**
    silent Ethernet loss on the upstream-only build (upstream `Kconfig.dwmac`
    offers only an STM32-gated platform and an `MMU`-gated path — the M55 has an
    MPU, not an MMU — and `hal_alif` ships no GMAC library).
  - **#150 SPI (DWC-SSI) — Tier-2 interim → retire onto the fork.** A genuine
    fork-driver copy (no `hal_alif` SSI library exists). The in-tree driver is
    **bench-verified PASS on E8** (2026-06-17, spi0 loopback), so it stays in place
    pending the fork migration; it is hard-deleted only once `spi0` is repointed to
    the fork compatible **and** that path is bench-verified — otherwise removing it
    now is a silent SPI-master regression. A pure-Tier-1
    end-state is one small upstream patch away (set `SSI_IS_MST`, CTRLR0[31],
    under `CONFIG_SPI_DW_HSSI`); filed as a follow-up, non-blocking.
  - **#151 UTIMER PWM/counter — merged as Tier-1.5** (bench-verified PASS on E8,
    2026-06-17 — PWM via UTIMER3 and the hardware counter via utimer0).
    Tier-2 is **infeasible**: the fork ships UTIMER bindings only (no `.c`), so
    there is nothing to migrate onto; retiring drops all AEN PWM (8 E1M pads) and
    the sole hardware counter to `NOSUPPORT` with no recovery (and AEN has no
    GD32-bridge fallback). The prior "held" stance is **reversed** under the
    Tier-1.5 amendment.

  The migration is deliberate (a planned PR per peripheral), not a revert-storm —
  working code is not removed before its replacement is proven + bench-verified.
  Tracked per-peripheral in this section (no single umbrella tracking issue).
- **New AEN peripheral work follows the tiers from day one** — but Tier 1.5 needs
  a HW library that actually covers the *data path*, verified per peripheral:
  - **NPU (task #19)** and **ISP (task #20)** are **Tier 1.5** — `hal_alif` ships
    real `ethos_u` and `isp` register libraries, so they follow the UTIMER pattern.
  - **ADC/DAC (task #18) is NOT Tier 1.5 — it is Tier 2 (fork-consumed).**
    `hal_alif`'s `analog` module (`analog_ctrl.{c,h}`) is only **analog
    reference/bias control** (VBAT rail, ADC vref buffer, DAC6/DAC12 vref scale) —
    a Tier-1.5 *helper* for the rails, but **not** the ADC sample/convert-FIFO or
    DAC output convert path, so a Tier-1.5 driver over `hal_alif` is not possible.
    The convert-path driver is the `sdk-alif` fork's `adc_alif`/`dac_alif` (Tier 2);
    `alp_adc` is **bench-verified PASS on E8** (single-shot) and the `alp_dac` code
    path holds (2026-06-17). Do not invent the convert registers from the TRM (per
    the pending-hw-configs policy).
  Peripherals with a genuine fork *driver* and no Apache HW library are Tier 2;
  the manifest/board-id read is Tier 3.
- **Pure-DesignWare stays.** Tier-1 nodes (`gpio_dw`, `i2c_dw`, …) are *not* vendor
  rewrites — they're upstream drivers we merely wire — and remain.
- **A hardware follow-up is recorded:** on the current E1M-AEN801 rev the
  housekeeping I2C devices are routed onto the slave-only LPI2C0, so the M55
  cannot master them. The next board revision must move them to a master-capable
  bus (a Shared-Peripheral I2C or LPI2C1); until then those reads are Tier-3
  (SE) or unavailable.

## Alternatives considered

- *Keep adapting vendor drivers onto the upstream base* (status quo): rejected —
  the maintenance/licensing/divergence cost is exactly what an "over the vendor
  SDK" product avoids.
- *Base the AEN target on the Alif Zephyr fork*: rejected — fragments the
  one-upstream-base invariant that serves Alif + Renesas + NXP; the fork stays
  opt-in for customers who want Alif's whole tree.

## Amendment (2026-06-15) — Tier 1.5

Executing the original three-tier policy literally would have **silently
regressed the upstream-only AEN build**: it enumerated the eth platform glue,
the DWC-SSI SPI flavor, and (by the "held" stance) the UTIMER PWM/counter drivers
as Tier-2 items that retire onto the opt-in fork — but for two of them the fork
has **nothing to retire onto**, contradicting this ADR's own rule that working
code is not removed before a proven replacement exists. Concretely:

- **The Context's licensing objection conflated two different things.** The "Alif
  Software License Agreement, not Apache-2.0" tangle is real for the **CMSIS
  DFP**, but **`hal_alif` is Apache-2.0** and already a pinned `west` manifest
  module. A thin Zephyr driver over `hal_alif`'s register library carries none of
  that licensing liability, and there is no competing vendor *driver* to diverge
  from (the library exposes no `struct device`).
- **The fork ships no UTIMER driver** (bindings only) and **no GMAC library
  anywhere**; `hal_alif` ships no SSI/GMAC library either. So "consume the fork
  driver" is infeasible for UTIMER and a one-upstream-base violation for eth (the
  fork forked the DWMAC *core*).

The amendment adds **Tier 1.5** (above) for exactly these in-tree-thin-driver
cases and reclassifies the four drivers (see Consequences). It is a deliberate
scope **correction**, not a reversal of the policy: rewritten fork-driver copies
(SPI, LPI2C) are still wrong and still go; in-tree drivers that link only an
Apache-2.0 HW library or an upstream core are recognised as legitimate and kept.
The one-upstream-base invariant and bench-verification gate are unchanged.

## Amendment (2026-08-30) — E1M-AEN801 BRD_I2C is I2C0, not slave-only LPI2C0

The Decision's Tier-3 paragraph and the LPI2C0-retirement rationale in
Consequences both state that the E1M-AEN801's on-module housekeeping trio
(RV-3028-C7 RTC / OPTIGA Trust M / TMP112) sits on the slave-only LPI2C0
(hardcoded slave address 0x40, TRM §3.17.4), so the M55 can never master it
and the SE must mediate every read. That routing claim is wrong. It is not
reversed here — the original text above is left as written, so a reader sees
what was decided and why it seemed right — but the underlying hardware fact
it depended on does not hold for this SoM, so the Tier-3 disposition it
produced for this specific trio does not either.

**What the evidence actually shows (#1848, #1814).** `BRD_I2C_SCL`/`BRD_I2C_SDA`
land on SoC pins `P7_1`/`P7_0` (the E1M-AEN-2626-R2 netlist, balls B3/B8).
Alif's `ADTS0013` v1.2 Table 3-16 gives those same pads the alternate function
`I2C0_SCL_C`/`I2C0_SDA_C` — **SoC I2C0**, not LPI2C0 (which lives on the
unrelated pad pair `P7_4`/`P7_5`). Per the HWRM §15.4.1, I2C0 is one of the
four shared-peripheral I2C modules and is **master-or-slave capable** — the
slave-only constraint is real, but it describes LPI2C0, a bus this trio was
never actually wired to.

**Effect on this ADR's tiering.** For the E1M-AEN801, the RTC/OPTIGA/TMP112
trio is **Tier 1 (upstream-native)**: ordinary upstream `i2c_dw` over the
already-Tier-1 `i2c0` node, the same as the edge I2C buses — no SE
mediation, no portable board-info/manifest API detour needed to reach it.
See `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` (the `i2c0` node and its
LPI2C0-note comment) and `metadata/e1m_modules/E1M-AEN801.yaml`
(`on_module.i2c_devices.brd_i2c`) for the corrected routing.

Getting a working bus needs more than "a pinctrl group + a board enable",
and two open facts bound it, not just the R2-vs-r1 netlist gap above:

- **No confirmed pull-up.** The R2 components CSV shows this net's pull-up
  jumpers (R93/R94) DNP -- unlike the I2C2/EEPROM bus, whose own pull-up
  path (R95/R96 into carrier resistors) IS stuffed. Whether that's a live
  problem depends on an unresolved document conflict: the datasheet marks
  `I2C0_SCL_C`/`I2C0_SDA_C` open-drain, requiring an external pull-up, while
  the HWRM's per-pin note for these ports says I2C "is operating properly
  with the push-pull (default) driver type" and that open-drain "must not
  be selected for I2C". Tri-state-high-phase-with-no-pull-up is a dead bus;
  push-pull makes the missing pull-up moot. Not resolved by paper alone --
  see `examples/aen/aen-secure-element-sign`'s board overlay, which wires
  the bus with NO internal bias pending a bench answer.
- **OPTIGA (IC1) is DNI** on the current bench population, independent of
  the bus question -- already documented in `docs/bring-up-aen.md`
  §5.1/§5.2 and unaffected by this amendment.

**What this amendment does not claim.** It is scoped to the E1M-AEN801's
BRD_I2C only — it does not revisit LPI2C0 itself (still genuinely slave-only;
the retired master-TX driver is still correctly retired), and it does not
reopen Tier 3 in general (a device genuinely behind the SE still belongs
there). It also does not claim bench verification: the routing evidence is
the E1M-AEN-2626-R2 netlist -- no R1 netlist is available, and the only
bench unit on hand is an r1 module — the physical bus needs an on-unit
probe before this routing is more than paper-correct for that unit.
