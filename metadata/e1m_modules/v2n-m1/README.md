# E1M-X V2N-M1 module pinout

Pin-to-function mapping for the E1M-X V2N-M1 family of SoMs
(`E1M-V2M101`, `E1M-V2M102` -- Renesas RZ/V2N + DEEPX DX-M1 NPU).

The V2N-M1 module reuses the [base V2N pinout](../v2n/) in full
plus the small overlay listed in `m1-additions.tsv`.

## Files

| File                       | Schema                                   |
|----------------------------|------------------------------------------|
| `m1-additions.tsv`         | `peripheral \t renesas_pad`              |
| `m1-additions.csv`         | `row, peripheral, renesas_pad`           |
| `hw-revisions.yaml`        | Per-rev SDK-version compatibility window |

## What V2N-M1 adds over base V2N

- `M1_RESET` -- Renesas-side GPIO driving the DEEPX silicon's reset
  pin (active-low; see [`<alp/chips/deepx_dxm1.h>`](../../../include/alp/chips/deepx_dxm1.h)).
- `PCIe.MUX_PD` / `PCIe.MUX_SEL` -- Renesas-side GPIOs driving the
  two passive PCIe muxes (PI3DBS12212A) that switch PCIe routing
  between on-module DEEPX and the E1M edge connector.

The V2N-M1 also adds the three DEEPX-specific TPS628640 buck
instances on BRD_I2C (at addresses 0x44, 0x48, 0x4F); those are
populated parts on this module but the BRD_I2C pin pair is the
same as on V2N base.

## Bring-up flow

The DEEPX silicon requires firmware-side bring-up: bring the DEEPX
rails up, route the PCIe muxes to the DEEPX path, then release
`M1_RESET`.  See
[`../../../docs/bring-up-v2n-m1.md`](../../../docs/bring-up-v2n-m1.md).
