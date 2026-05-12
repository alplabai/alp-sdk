# E1M-X V2N-M1 hardware-revision change log

Per-rev summary of pin / component / configuration changes on the
V2N-M1 SoM family.  V2N-M1 inherits the V2N base ([`../v2n/CHANGELOG.md`](../v2n/CHANGELOG.md))
plus the deltas listed here.

Detailed schematic-level rev notes live in the private companion repo.

## r1 -- production (initial release, 2026-05-12)

* First V2N-M1 revision.  Adds DEEPX DX-M1 NPU + 2 × PI3DBS12212A
  PCIe muxes + 3 × TPS628640 buck instances on BRD_I2C
  (at `0x44` / `0x48` / `0x4F`) on top of the V2N r1 base.
* `M1_RESET` on Renesas `PA6` (active-low, confirmed 2026-05-12).
* `PCIe.MUX_PD` on `P80`, `PCIe.MUX_SEL` on `P95`.
* DEEPX bring-up sequence in `chips/deepx_dxm1/`:
  1. Enable DA9292 ch2 (0.75 V DEEPX rail).
  2. ACK-probe the three TPS628640 instances.
  3. Route PCIe muxes to DEEPX path.
  4. Release `M1_RESET`.

## r2..r8 -- reserved

See `../v2n/CHANGELOG.md` for the rev-tracking format.

## Inherited from V2N base

Every V2N base change carries over to V2N-M1 unless explicitly
overridden in this file.  See [`../v2n/CHANGELOG.md`](../v2n/CHANGELOG.md).
