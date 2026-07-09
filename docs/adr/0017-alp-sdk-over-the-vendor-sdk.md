# 0017. alp-sdk rides over the vendor SDK — no rewritten vendor drivers

Status: Accepted (amended 2026-06-15 — adds **Tier 1.5**; reclassifies the four AEN drivers; see "Amendment" below)
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
  master driver.

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
