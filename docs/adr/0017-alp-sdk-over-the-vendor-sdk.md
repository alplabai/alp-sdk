# 0017. alp-sdk rides over the vendor SDK — no rewritten vendor drivers

Status: Accepted
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

- **Tier 2 — vendor-SDK-consumed.** Alif-specific peripherals upstream lacks: the
  Alif LPI2C1 master, the Alif PWM/ADC/DAC/ISP drivers, the DWC-SSI SPI flavor,
  the eth platform glue. These come from the **opt-in Alif vendor SDK**
  (`sdk-alif` fork / the CMSIS DFP, in the `vendor-sdks` manifest group) that the
  customer adds. alp-sdk maps `<alp/*>` onto them when present and returns
  `sw_fallback`/`NOSUPPORT` otherwise. **alp-sdk does not vendor or rewrite them.**

- **Tier 3 — SE-mediated.** Housekeeping/security devices behind the Secure
  Enclave. On the E1M-AEN801 the on-module trio sits on the **slave-only LPI2C0**
  bus (the SoC is a slave at the hardcoded address 0x40; TRM §3.17.4), so the SE
  masters that bus. Such devices are read via **Alif SE services** (vendor SDK),
  surfaced through a portable board-info/manifest API — never an alp-sdk I2C
  master driver.

Bench-verification on real E8 silicon remains the acceptance gate for every tier.

## Consequences

- **The interim vendored Alif drivers are migrated, not kept.** The merged ones
  (#148 LPI2C-TX, #149 eth glue, #150 SPI) stay as clearly-marked *interim*
  gap-fillers until their Tier-2 `<alp/*>`-over-vendor-SDK mapping lands; PR #151
  (PWM/counter) is **held** rather than merged. The migration is deliberate (a
  planned PR per peripheral), not a revert-storm — working code is not removed
  before its replacement is proven + bench-verified. Tracked as task #21.
- **New AEN peripheral work follows the tiers from day one** (e.g. ADC/DAC/NPU/ISP
  are Tier 2; the manifest/board-id read is Tier 3).
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
