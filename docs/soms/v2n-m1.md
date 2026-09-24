# E1M-X V2N-M1 family

> V2N + on-module **DEEPX DX-M1** NPU.  AI-accelerator variant.

## SKUs

| SKU            | Memory                                | Status     |
|----------------|---------------------------------------|------------|
| `E1M-V2M101`   | 32 Gbit LPDDR4X + 32 Gbit eMMC + DX-M1| production |
| `E1M-V2M102`   | 64 Gbit LPDDR4X + 128 Gbit eMMC + DX-M1| production |
| `E1M-V2M103`   | 32 Gbit LPDDR4X + 128 Gbit eMMC + DX-M1| production |

## What's different from V2N base

V2N-M1 inherits the full V2N base module (see [`v2n.md`](v2n.md))
and adds:

| Component                | Where + how                                                |
|--------------------------|------------------------------------------------------------|
| **DEEPX DX-M1 NPU**      | On-module, PCIe                                            |
| `M1_RESET`               | Renesas-side GPIO controlling DX-M1 reset (active-low)     |
| 2 × PI3DBS12212A muxes   | Switch PCIe routing between DEEPX and the E1M edge         |
| 0.75 V DEEPX rail        | DA9292 CH2 (disabled on V2N base; brought up by U-Boot on M1, over RIIC8/BRD_I2C -- Cortex-A55/Linux-exclusive) |
| 3 × TPS628640 bucks      | DDR5/LPDDR rails for DEEPX (`0x44` / `0x4F` / `0x48`, all bench-confirmed, see below) |

## DEEPX bring-up

Four-step sequence, run **after** the Renesas side boots and
**before** the Linux kernel attempts to open the PCIe device.  Steps
1, 3, and 4 run in **U-Boot's `board_late_init()`**
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/
0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch` for step 1;
`0001-rzv2n-dev-EEPROM-gated-DEEPX-DX-M1-PCIe-bring-up.patch` for
steps 3-4) -- no application firmware (CM33 or Linux) writes or
re-runs them.  Step 2 (below) is not implemented by either patch; it
remains a bench-diagnostic check, not an automated bring-up step:

1. **Enable the 0.75 V DEEPX rail** via the secondary PMIC's CH2, over
   RIIC8/BRD_I2C.  This bus is Cortex-A55/Linux-exclusive
   (`metadata/e1m_modules/v2n/core-ownership.yaml`); U-Boot runs on
   the A55 before Linux starts, so it -- not the CM33 -- is the sole
   writer.
2. **ACK-probe** the DEEPX TPS628640 instances at `0x44` / `0x4F` /
   `0x48` to confirm population (self-regulating).  `deepx_lpddr_0v85`
   (`0x48`) only ACKs after step 1 drives `P64` high -- see the strap
   note below.
3. **Route the PCIe muxes** to the DEEPX path with the PI3DBS12212A
   driver (PD pin on Renesas `P80`, SEL pin on `P95`).
4. **Release `M1_RESET`** (Renesas `PA6`; active-low) -- ONLY once
   step 1 confirms the rail is power-good.

### `deepx_lpddr_0v85` strap is resolved: `0x48` (#1163, #1845)

The third DEEPX buck (`tps628640`, role `deepx_lpddr_0v85`) is
`address_7bit: "0x48"` on the V2M pair --
`metadata/e1m_modules/E1M-V2M101.yaml` / `E1M-V2M102.yaml` /
`E1M-V2M103.yaml`.  **Bench-measured 2026-09-24 on E1M-V2M103:**
`0x48` ACKs on `BRD_I2C` only once `P64` (`DEEPX_CORE_0P75_EN`) is
driven high (step 1 above), and its VOUT register (`0x5A`) reads
0.85 V -- matching the role.  This is the same address the chip's
own default strap gives, but that was NOT sufficient on its own to
resolve the strap (see the now-superseded collision history below);
the bench measurement is what confirms it.

Superseded history: this address was `TBD` because of an apparent
collision with `tmp112` (also nominally strappable to `0x48`..`0x4B`)
-- that premise no longer held once `tmp112` was maintainer-confirmed
at `0x40` (all six V2N-family SKUs, one shared PCB, one ADD0 net; see
`metadata/chips/tmp112.yaml`), and the bench measurement above then
confirmed `0x48` directly rather than inferring it from the
non-collision. See
[#1163](https://github.com/alplabai/alp-sdk/issues/1163) and
[#1845](https://github.com/alplabai/alp-sdk/issues/1845) for the full
history.

The `chips/deepx_dxm1/` driver wraps steps 3-4 into a single
[`deepx_dxm1_bring_up(&ctx, DEEPX_DXM1_DEFAULT_BOOT_US)`](../../include/alp/chips/deepx_dxm1.h)
call, for platforms where a portable caller owns `M1_RESET`.  On
V2N-M1, U-Boot implements steps 3-4 itself with raw register pokes
(not this driver) so it can gate them on step 1's rail check; nothing
calls `deepx_dxm1_bring_up()` here.  Step 1 similarly has its own
driver API (`chips/da9292/`), which U-Boot does not call either (same
reason: U-Boot builds standalone against upstream sources, not
alp-sdk) -- see `docs/bring-up-v2n-m1.md` §2.

Walk-through with code: [`docs/bring-up-v2n-m1.md`](../bring-up-v2n-m1.md).

## DEEPX runtime

The DEEPX silicon's userland API (`libdxrt.so`) is upstream at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).
The Yocto layer that brings it into your image is wired in
`meta-alp-sdk/conf/machine/e1m-v2m101-a55.conf` and references
`github.com/DEEPX-AI/meta-deepx-m1`.

Integration cross-link: [`vendors/deepx-dxm1/README.md`](../../vendors/deepx-dxm1/README.md).

## Example apps targeting V2N-M1

All V2N examples apply.  DEEPX-specific examples land separately
as the NPU integration matures.

## Common gotchas

| Symptom                                              | Cause + fix                                                            |
|------------------------------------------------------|------------------------------------------------------------------------|
| U-Boot logs `ALP: DA9292 ...` abort / `DEEPX rail not enabled` / `DEEPX rail disabled` | 0.75 V plane shorted, or another check named in the printed register bytes failed; probe the rail directly.  See `docs/bring-up-v2n-m1.md` §2. |
| U-Boot logs `... DEEPX rail not enabled (CH2 may require EN2/P64 high before PG -- see bring-up doc)` | CH2_EN was written over I2C but PG never asserted -- possible EN2/P64 hardware gating (P64 only goes high after PG in the current sequence). See `docs/bring-up-v2n-m1.md` §2 and #2045. |
| DEEPX rails up but PCIe link never trains            | `M1_RESET` polarity wrong -- the driver default is active-low; board may need override via `deepx_dxm1_set_reset_polarity`. |
| PCIe link trains but kernel driver reports BAR errors| PCIe muxes on the wrong path -- check `PI3DBS_STATE_PATH_0` matches your board's silk-screen. |
| `dxrt_init()` returns an error                       | Check the DEEPX kernel driver (`dx_rt_npu_linux_driver`) is loaded.    |

## See also

* [`v2n.md`](v2n.md) -- the base SoM.
* [`../bring-up-v2n-m1.md`](../bring-up-v2n-m1.md) -- bench bring-up.
