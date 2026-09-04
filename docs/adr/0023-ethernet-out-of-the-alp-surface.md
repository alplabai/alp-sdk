# 0023. Ethernet stays out of the `<alp/*>` surface

Status: Proposed
Date: 2026-08-05

## Context

There is no `include/alp/net.h`. Issue #1144 raises Ethernet as the
largest peripheral class with no portable surface in this SDK.

`docs/adr/0003-peripheral-coverage.md:44` deferred Ethernet at v0.2
with the plan "folds into `<alp/iot.h>` networking". That plan did
not happen: `include/alp/iot.h:8` scopes the header as "Wi-Fi-station
+ MQTT in v0.1" and it never grew Ethernet. **This ADR supersedes
that one row of ADR 0003 and nothing else** — the other 11 peripheral
classes and their disposition in ADR 0003 stand unchanged.

The per-family hardware, tree-verified:

- **V2N / V2M**: `metadata/socs/renesas/rzv2n/n44.json:62` declares
  `"ethernet_1g": 2`. All four SKU presets declare
  `ethernet_phy: rtl8211fdi` and `ethernet_phy_count: 2`
  (`metadata/e1m_modules/E1M-V2N101.yaml:27-28`, same in
  `E1M-V2N102.yaml`, `E1M-V2M101.yaml`, `E1M-V2M102.yaml`). On the
  Linux side, dual PHYs each sit at MDIO address 2, on separate buses
  `&mdio0` / `&mdio1`
  (`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi:198,203,211`).
- **AEN**: `"ethernet": 1` in every Ensemble SoC JSON (e.g.
  `metadata/socs/alif/ensemble/e8.json:152`). The on-module PHY is the
  TI DP83825 (`metadata/e1m_modules/aen/CHANGELOG.md:16`); RMII,
  100/full (`zephyr/dts/alif/ensemble_e8_peripherals.dtsi:467`). The
  MDIO address was contested in-tree when this ADR was written — the
  Zephyr DT recorded a managed-MDIO PHY at address 1, flagged "fork
  reference … confirm this address before relying on it", while the
  bench log from real E8 silicon read
  `[eth] MDIO PHY@0 id=2000a140 (DP83825I=2000a140)`
  (`examples/aen/aen-ethernet-link/README.md:39`) — address 0, not 1
  — and the binding doc's own example agreed with the bench log:
  "e.g. ti,dp83825 at addr 0"
  (`zephyr/dts/bindings/ethernet/alif,ethernet.yaml:47`).
  **Settled (#1244):** a Flow C ITCM RAM-run of the `mdio-managed`
  variant read the PHY over the managed controller itself —
  `[eth] MDIO PHY@0 id=2000a140 (DP83825=2000a140)` — so the DT node
  is now `ethernet-phy@0` with `reg = <0>`
  (`zephyr/dts/alif/ensemble_e8_peripherals.dtsi:538-560`). The
  order-code suffix stays TBD: `id=2000a140` is die/OUI identity only.
- **NX91**: `ethernet_phy: TBD`
  (`metadata/e1m_modules/E1M-NX9101.yaml:28`);
  `metadata/socs/nxp/imx9/imx93.json:55-56` states per-peripheral
  counts are pending reference-manual ingestion and must not be
  inferred from their absence. NX91 cannot be counted for or against
  Ethernet portability from this tree — its port count is unknown,
  not zero.

The decisive evidence that no SDK surface is needed here:
`examples/aen/aen-ethernet-link/README.md:35-47` records an
end-to-end **RESULT PASS on E8 silicon** — link up, DHCP lease
acquired, confirmed on the server's ARP table too — using plain
Zephyr `net_if` + DHCP. The working example needed no `<alp/*>` net
API.

## Decision

Ethernet's data plane **and** its generic control plane stay outside
`<alp/*>`. What the SDK already provides, and what this ADR ratifies
as the contract, is three layers:

1. **Compile-time capability** — `ALP_SOC_ETHERNET_COUNT` /
   `ALP_CAP_HW_ETHERNET` (`include/alp/soc_caps.h:48,455`, mirrored
   per SoC block). **Researching this ADR found that layer broken on
   the 2-port family the ADR is about**: `gen_soc_caps.py` read only
   the `ethernet` key while `n44.json` declares `ethernet_1g`, so
   `ALP_SOC_ETHERNET_COUNT` was 0 on V2N/V2M and `ALP_CAP_HW_ETHERNET`
   reported false on silicon with two 1GbE MACs. Fixed separately in
   **#1240**; `include/alp/soc_caps.h:373` now reads 2.

   That is worth recording rather than quietly repairing, because it
   is the failure mode this whole ADR guards against: a surface that
   exists, is documented and generated, and reports the opposite of
   the hardware. Ratifying a layer is not the same as the layer
   working, and nothing in the tree had noticed for as long as the
   `ethernet_1g` spelling had existed.

   One case remains false and is **not** a defect: NX91
   (`include/alp/soc_caps.h:333`), because `imx93.json` declares its
   peripheral counts pending reference-manual ingestion, so there is
   no key to sum and none may be inferred from the absence.
2. **Form-factor port identity** — `ALP_E1M_ETH0` with
   `ALP_E1M_ETH_COUNT 1u` (`include/alp/e1m_pinout.h:100,204`), and
   `ALP_E1M_X_ETH0` / `ALP_E1M_X_ETH1` with `ALP_E1M_X_ETH_COUNT 2u`
   (`include/alp/e1m_x_pinout.h:98-99,208`). This is **not** what a
   customer moving between a 2-port and a 1-port SoM keys off — the
   only 1-port SoM is E1M (AEN) and the only 2-port is E1M-X
   (V2N/V2M), so that swap is a cross-form-factor move, and
   `docs/adr/0011-intra-family-portability.md:52` states
   "Cross-form-factor (E1M ↔ E1M-X) is intentionally not supported
   by source-level portability"; `include/alp/e1m_x_pinout.h:14`
   likewise says the two namespaces are "intentionally NOT compatible
   by include". The use case issue #1144 raises is answered by ADR
   0011, not by this layer — a reader following #1144 here should
   stop at ADR 0011. What this layer actually delivers is
   **intra-family** portability: `ALP_E1M_X_ETH0`/`ALP_E1M_X_ETH1`
   are the same symbols across V2N and V2M, so a V2N101 → V2M102 swap
   (both E1M-X, per ADR 0011) needs no source change.
3. **Ring 2 PHY chip drivers under their natural names** — this layer
   exists for V2N/V2M: `include/alp/chips/rtl8211fdi.h`, which
   already carries a raw clause-22/page register escape hatch
   (`rtl8211fdi_read_reg`/`rtl8211fdi_write_reg`/
   `rtl8211fdi_read_page_reg`/`rtl8211fdi_write_page_reg`,
   `include/alp/chips/rtl8211fdi.h:232-244`). AEN — the only family
   with silicon-proven Ethernet in this tree — has no layer-3 chip
   driver: there is no `chips/dp83825/`. Every AEN preset
   (`metadata/e1m_modules/E1M-AEN301.yaml`..`E1M-AEN801.yaml`) declares
   `ethernet_phy: dp83825` (`metadata/chips/dp83825.yaml`,
   `driver_status: none`) — the metadata side of **#1241**; the C
   driver itself remains unwritten and the exact order-code suffix
   stays TBD pending the netlist/BOM, so the issue stays open.

The refusal list, with the reason each operation is refused:

| Refused operation | Why |
|---|---|
| PHY read/write | Linux side is kernel-owned. The tree already refuses it: `metadata/chips/rtl8211fdi.yaml:63` says `yocto: n/a # Linux uses upstream kernel phy_device`, and the PHYs are bound by the kernel DT nodes at `e1m-v2n-som.dtsi:204,212`. One-side-only is not portable. Zephyr callers on V2N/V2M already reach it through the chip header; AEN has no layer-3 chip driver (#1241), so there the operation is refused for lack of a header, not just by policy. |
| MAC provisioning | Neither backend has a runtime set-MAC seam. Zephyr is build/boot-time DT only — `zephyr/drivers/ethernet/eth_dwmac_alif_ensemble.c:305-311` `BUILD_ASSERT`s on DT `local-mac-address` or `zephyr,random-mac-address`, and pinning a per-unit MAC means editing an overlay (`examples/aen/aen-ethernet-link/README.md:49-54`). The identity source is also unresolved — see the open question below. |
| Link state | Both OSes *can* express it, so it fails no portability test — it is refused because it would be the header's only surviving function, and both sides already expose it natively (Zephyr `net_if`, per the silicon-proven example). A one-function header earns nothing. **This is the operation most likely to return**, additively, in an existing header, if a consumer requirement appears. |
| Data plane | BSD sockets exist on both Linux and Zephyr. Already portable; the SDK would only be a passthrough. |

Reversal condition, stated explicitly: if alp-studio needs a portable
"cable connected?" block (the ADR 0005 boundary), link state comes
back as one additive function in an existing header — not as a new
`<alp/net.h>`.

## Alternatives

**(b) — the narrow `<alp/net.h>` proposed by #1144**, covering link
state, PHY read/write, and MAC provisioning. Rejected: two of its
three candidate operations fail the refusal table above, and the lone
survivor is one function — not enough to justify a new header. The
failure mode is the one this repo already has a name for: a surface
that exists but cannot do anything real, shipping as
`ALP_ERR_NOSUPPORT` stubs on at least one OS. The house rule against
that is explicit — `include/alp/i3c.h:37-40` says not to freeze ABI
on hardware paths that are not bench-proven, and the relevant PHY
paths are not: `include/alp/chips/rtl8211fdi.h:10-13` marks the V2N
PHY driver `[UNTESTED]` on silicon, and
`zephyr/kconfigs/vendor-alif-peripherals.kconfig:140,157-158` records
the AEN managed-MDIO path as BUILD-ONLY, with no MDIO-managed PHY
exercised on real E8 silicon.

> **Superseded in part, 2026-08-12 (#1244).** The last clause is no longer
> true for AEN: a Flow C ITCM RAM-run on real E8 silicon performed a live MDIO
> register read — `[eth] MDIO PHY@0 id=2000a140 (DP83825=2000a140)`, with
> `ANAR=01e1 ANLPAR=0000 PHYSTS=0002 RCSR=00e1` — so `mdio_dwmac_alif` binds,
> `phy_ti_dp83825` binds on the bus, and the upstream `eth_dwmac` core's
> `phy_link` path runs. That run also corrected the PHY's MDIO address from
> the devicetree's `reg = <1>` to the measured `0`.
>
> **The decision this ADR records still stands.** One bench-proven read is not
> a bench-proven data plane, the V2N `rtl8211fdi` path remains `[UNTESTED]`,
> and the argument against freezing an `<alp/net.h>` ABI on unproven hardware
> is unchanged. Annotated rather than rewritten, because an ADR is a record of
> what was decided and why — editing the premise silently would leave the
> decision looking better-founded than it was at the time.

**A full `<alp/net.h>` with a data plane.** Rejected for the same
reasoning ADR 0003's Alternative B already gave for Ethernet, QSPI,
and MIPI CSI/DSI as a group (USB was given a separate reason there —
"its own header deserves a v0.3 milestone" —
`docs/adr/0003-peripheral-coverage.md:80-81`): each folds more
naturally into a higher-level header (or, here, needs no header at
all) than living in peripheral-primitive territory, and bundling it
in balloons scope without adding capability a customer cannot already
reach.

## Consequences

Good:
- No new ABI frozen on hardware paths that are not bench-proven.
- The three layers the SDK already ships — capability macro, port
  identity, chip driver — are named explicitly and become citable,
  instead of Ethernet portability being an implicit, undocumented
  fact.
- The stale ADR 0003 row stops pointing readers at a header
  (`<alp/iot.h>`) that never grew Ethernet.

Bad / costs:
- A customer looking for `<alp/net.h>` finds nothing under `<alp/*>`
  and must be pointed at the OS-native path instead: Zephyr `net_if`
  (as `examples/aen/aen-ethernet-link` demonstrates end-to-end) or the
  Linux kernel's own network stack on Yocto. That pointer needs to
  live somewhere a reader will find it — flagged as a follow-up doc
  task, not resolved by this ADR.

Open questions, left open rather than papered over:

1. **MAC provisioning source is unresolved.** The 128-byte EEPROM
   manifest (`include/alp/hw_info.h:88-113`) carries `family`, `sku`,
   `hw_rev`, and `serial` fields and has no MAC field. Today AEN uses
   a per-boot random locally-administered `02:01:56:xx:xx:xx`
   (`examples/aen/aen-ethernet-link/README.md:50-53`). Whether
   production units get factory-assigned MACs, and from what source,
   is a business and factory-provisioning decision outside this ADR's
   scope; until it is settled, an `alp_net_get_mac()` would have
   nothing authoritative to read.
2. Whether alp-studio needs a portable Ethernet block at all — the
   only consumer pull that would revive link state under `<alp/*>`.
3. V2N M33-side Ethernet ownership is undesigned: the CM33 targets do
   not own the MACs today, and M33 bring-up is blocked upstream.
   Nothing in this tree supports designing for it now.

Found while researching this ADR, fixed separately rather than here
(and forward-referenced from the Decision section above, layer 1):
`scripts/gen_soc_caps.py` matched only the `ethernet` key while
`n44.json` declares `ethernet_1g`, so V2N emitted
`ALP_SOC_ETHERNET_COUNT 0` despite having two ports — **#1240**. Its
guard is worth knowing about: the test that stops a third key spelling
recurring probes the real `ETHERNET_COUNT` lambda rather than a copy
of the key list, because the first version hardcoded that copy and
stayed green when the generator was reverted.

Also filed from this ADR's research: **#1241**, still open (every AEN
preset omitted `ethernet_phy`, and no `metadata/chips/dp83825`
manifest existed; this change adds both across the family, but the
exact order code stays TBD pending the netlist/BOM, and the C driver
itself remains unwritten) and **#1244**, now fixed (the devicetree put
the PHY at MDIO address 1 while the E8 bench log read it at address 0;
a managed-path MDIO read on silicon confirmed 0 and the node was
corrected).

## See also

- `docs/adr/0003-peripheral-coverage.md` — the original deferral this
  ADR corrects one row of.
- `docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md` — the boundary
  that governs the reversal condition for link state.
- `docs/adr/0011-intra-family-portability.md` — why layer 2's port
  identity covers V2N ↔ V2M and not E1M ↔ E1M-X.
- `include/alp/chips/rtl8211fdi.h`,
  `metadata/chips/rtl8211fdi.yaml` — the V2N/V2M PHY chip driver.
- `metadata/chips/dp83825.yaml` — the AEN PHY manifest (no chip
  driver yet, `driver_status: none`, #1241).
- `examples/aen/aen-ethernet-link/` — the silicon-proven Zephyr
  `net_if` reference this decision rests on.
- `docs/adr/0024-v2n-analog-and-counter-classes-stay-on-the-gd32-bridge.md`
  — opposite polarity: this ADR removes a class from `<alp/*>`; 0024
  keeps five classes in `<alp/*>` while fixing which die serves
  them.
