### Fixed — GBETH PHY address no longer needs per-unit config (#2582)

The RTL8211F PHY address on each GBETH is strap-latched and differs per unit
(bench unit V2M103 0008: end0 at MDIO address 2, end1 at 3), so the fixed
`reg = <2>` in the Linux DT left end1 failing with `phy_poll_reset failed: -110`.

Changes:
- U-Boot patch 0017 scans MDIO addresses 1..31 on both GBETH buses at boot by
  reading the DWC EQoS MDIO registers directly (bounded wait, stops on the
  first read error) and, when exactly one RTL8211F PHY answers at an address
  other than the DT's `reg`, rewrites the booted DT's phy `reg` and node name
  before `booti` (`ALP: GBETH<n> PHY at MDIO addr <a> (DT had <b>) - fixed`);
  with no PHY found it warns and leaves the DT unchanged.
- `docs/errata-e1m-x-v2n.md` E2 records the 0008 measurement and the fixup.
