# Vendor-partnership tracker

> **Status:** open — pending vendor confirmation calls.
> Tracks the four vendor relationships that gate Pillar 9 of
> [`docs/v1.0-readiness.md`](v1.0-readiness.md).

## Why this doc exists

Pillar 9 ("ecosystem") of the v1.0 readiness plan depends
on three external relationships landing:

1. SoC-vendor SDK / HAL pack confirmations (Renesas FSP,
   Alif Ensemble HAL, NXP MCUXpresso).
2. `alplabai/alp-zephyr-modules` repo public release with the
   board-file definitions for every E1M-* SoM in the matrix.
3. OpenEmbedded layerindex registration for `meta-alp`.

None of these are SDK source-tree changes; they're meta-work
that gates the customer onramp.  This doc tracks the open
items + owner + next-action per vendor so the maintainer
doesn't lose state between weekly partnership-review cycles.

## Renesas (RZ/V2N family)

**Surface impact**: `chips/gd32g553/` host driver +
`src/zephyr/v2n_supervisor.c` + `src/zephyr/v2n_power_mgmt.c`
all assume an upstream Renesas FSP for the RZ/V2N N44 SoC.

**Status update 2026-05-14 (verified against upstream)**:

The RZ/V FSP **is already public** at
[`github.com/renesas/rzv-fsp`](https://github.com/renesas/rzv-fsp)
under **BSD-3-Clause** for the MPU BSP / Board BSP / HAL
drivers / generic middleware (the parts the SDK actually
consumes).  Latest release **v3.1.0** (2025-03-11).  Board
support for **`rzv2n_evk`** is already in-tree at
`rzv/board/rzv2n_evk/` -- no NDA tarball needed.

So the "public FSP release" line item is **already closed**;
the SDK just needs to sync its metadata + build instructions
to point at the public release rather than the prerelease
tarball notation that survived in the tracker.

The `renesas/fsp` repo (no rz prefix) is the **RA MCU family**
FSP -- a different SoC line (Cortex-M MCU vs the RZ/V
application-processor line).  Don't confuse the two.

**Open items**:

- [x] **Public RZ/V2N N44 FSP release.**  Public at
  `github.com/renesas/rzv-fsp` since v2.0 (the v3.1.0
  release was March 2025).  SDK builds should sync to the
  tagged release rather than the older prerelease tarball.
- 📋 **`metadata/socs/renesas/rzv2n/n44.json` cross-check.**
  Confirm the SoC metadata in this repo matches what
  rzv-fsp's `rzv/fsp/inc/api/` headers declare.  Mechanical
  diff against the public FSP; lands when the maintainer
  has bandwidth.
- 📋 **DA9292 + V2N PMIC pad-routing confirmation.**  The
  bring-up sequence in `chips/da9292/da9292.c::da9292_v2n_m1_enable_deepx_rail`
  encodes the maintainer's reading of the V2N schematic +
  the AROVx OTP variant trap.  Renesas' AE has confirmed
  the schematic; the OTP confirmation is open.
- 📋 **DRP-AI runtime license.**  v0.5 inference dispatcher
  exposes the DEEPX path; the RZ/V2N's on-die DRP-AI is a
  separate runtime we plan to integrate.  Licensing terms
  for the DRP-AI translator toolchain pending (the on-die
  driver ships as BSD-3-Clause via rzv-fsp; the model
  compiler is the licensing question).

**Next action**: Q2 sync call with Renesas AE re: DRP-AI
compiler licence + DA9292 OTP confirmation.

## Alif Semiconductor (AEN family)

**Surface impact**: `examples/aen/` flagships +
`chips/aen-isp` /`<alp/gpu2d.h>` AEN backends + the dual-
image build flow for `mproc-mailbox`.

**Open items**:

- 📋 **Public AEN HAL pack.**  We're building against
  the Alif Ensemble HAL v1.5 which is publicly available;
  v1.6 ships in 2026-Q2 with the DAVE2D + Ethos-U + CSI
  surfaces that <alp/gpu2d.h> / <alp/inference.h> /
  <alp/camera.h> consume.  v1.0 of the SDK syncs to v1.6
  once it ships.
- 📋 **Dual-image build flow upstreaming.**  The §C.30
  HE-side peer image needs sysbuild glue that builds both
  HP + HE halves in one invocation.  This sits in
  `alplabai/alp-zephyr-modules` (not this repo); Alif is
  reviewing our proposed pattern.

**Next action**: 2026-Q2 sync at the Alif partner summit.

## DEEPX (DX-M1 NPU for V2N-M1)

**Surface impact**: `chips/deepx_dxm1/` host driver + the
`<alp/inference.h>` DEEPX backend dispatch +
`examples/v2n/` flagships that target DX-M1 inference.

**Status update 2026-05-14 (verified against upstream)**:

The DEEPX-AI GitHub org (`github.com/DEEPX-AI`) hosts 42
public repos.  The relevant ones for the SDK:

| Repo                              | Description                                         | License                                                   |
|-----------------------------------|-----------------------------------------------------|-----------------------------------------------------------|
| `dx_fw`                           | NPU firmware images                                  | **Apache-2.0** (genuinely open)                            |
| `dx_rt`                           | Inference runtime                                    | **Customer-only** ("supplied with DEEPX NPU... unauthorized sharing prohibited") |
| `dx_app`                          | Runtime app + templates                              | **Customer-only** (same wording)                            |
| `dx_rt_npu_linux_driver`          | Linux PCIe driver                                   | **Customer-only** (same wording)                            |
| `meta-deepx-m1`                   | Yocto recipes for DX-M1                              | No LICENSE file                                            |
| `dx_rt_windows`                   | Windows runtime + drivers                            | Customer-only                                              |
| `dx-modelzoo`                     | Reference models                                     | MIT (genuinely open)                                       |
| `ultralytics-deepx`               | YOLO fork for DEEPX NPU                              | AGPL-3.0                                                   |

The "open source" framing in several repo descriptions is
misleading -- the LICENSE files explicitly restrict to "customers
who are supplied with DEEPX NPU".  Source-visible ≠ Apache /
BSD redistributable.  The firmware images (`dx_fw`) are the
exception -- those are real Apache-2.0.

**Implications for the SDK**:

- `chips/deepx_dxm1/` is a thin host driver (PCIe + GPIO
  bring-up + reset polarity) -- doesn't redistribute DEEPX
  runtime code, so no licence-encumbered dependency in this
  repo.  Already in-tree under our usual Apache-2.0.
- `<alp/inference.h>`'s DEEPX backend dispatch is a header-
  level seam.  When a customer builds the SDK against the
  DX-M1 path, they pull `dx_rt` / `dx_rt_npu_linux_driver`
  themselves (as a DEEPX NPU customer) and the SDK links
  against headers.  We don't redistribute DEEPX code.
- `meta-deepx-m1`'s missing LICENSE file is the one open
  question -- customers integrating V2N-M1 into a Yocto
  image want clarity on whether they can redistribute the
  layer.  Carry as an open item.

**Open items**:

- 📋 **`meta-deepx-m1` LICENSE clarification.**  The Yocto
  layer that customers consume to integrate DX-M1 into
  V2N-M1 BSP images has no LICENSE file in upstream.  Ask
  DEEPX whether the layer is intended to be redistributable
  (typical for Yocto layers, which are mostly recipes that
  fetch the actual binaries at build time) or whether the
  customer-only restriction extends to it.
- 📋 **DX-M1 PCIe driver upstreaming.**  `dx_rt_npu_linux_driver`
  is a customer-only PCIe driver today.  Whether DEEPX
  plans to upstream into mainline Linux is open; meanwhile
  customers vendor the driver from the GitHub repo at
  integration time.
- 📋 **Yocto BSP integration sample.**  No
  `examples/v2n/v2n-m1-deepx-inference/` flagship exists
  yet; lands once a customer-side `meta-deepx-m1` build
  is reproducible on our reference V2N-M1 hardware.

**Next action**: Ping DEEPX customer support about the
`meta-deepx-m1` LICENSE.

## NXP (i.MX 93 family)

**Surface impact**: `examples/imx93-*` (none today, planned)
+ Yocto layer + the `<alp/storage.h>` OTFAD backend that
ships when NXP's FlexSPI OTFAD driver stabilises.

**Open items**:

- 📋 **i.MX 93 Yocto BSP confirmation.**  meta-imx releases
  cycle quarterly; v1.0 of the SDK aligns to whichever
  meta-imx release ships closest to our v1.0 tag.  No open
  technical issues; tracking the calendar.
- 📋 **OTFAD inline-AES driver upstreaming.**  The
  `<alp/storage.h>` inline-AES surface (`alp_storage_configure_inline_aes`)
  reaches NXP silicon through NXP's OTFAD driver; that
  driver is mainline in Linux but not yet in Zephyr.  Open
  upstreaming work tracked at zephyrproject-rtos/zephyr#TBD.

**Next action**: 2026-Q3 sync after meta-imx mickledore
ships.

## alp-zephyr-modules repo

**Surface impact**: Customer board-file consumption.
Without this repo public, customers can't build for any
E1M-* SoM out of the box.

**Open items**:

- 📋 **Public release.**  The repo exists private; the
  public release ships after `alp-sdk` v1.0 cuts so the
  two version contracts line up.
- 📋 **v0.4 dual-image build flow.**  The `mproc-mailbox`
  HE-side peer (§C.30) builds via single-target invocation
  today; the dual-image sysbuild glue is in
  `alplabai/alp-zephyr-modules` waiting on the public
  release.
- 📋 **Per-SoM HW baselines.**  Pillar 5 wants per-(SoM, OS)
  bench baselines under `tests/bench/baselines/`.  Those
  files have to be captured against real silicon, which
  needs the Zephyr board files this repo publishes.

**Next action**: Coordinate public release with the v1.0
SDK tag.

## OpenEmbedded layerindex (meta-alp)

**Surface impact**: Customer Yocto integration.  Today
customers add `meta-alp` via a layers.conf edit; once
registered, `bitbake-layers add-layer meta-alp` finds it
through the standard layer-fetcher path.

**Open items**:

- 📋 **Submit `meta-alp` to layerindex.**  One-time
  submission; gates on the public-release version of the
  layer being stable (i.e. after v1.0).

**Next action**: After v1.0 tag.

## How this doc evolves

Each weekly partnership-review cycle:

1. Maintainer updates the per-vendor status above.
2. Closed items get the date stamp + cross-ref to the
   commit / Issue that closed them.
3. New blocking items get added with an owner + next action.

When all four vendors clear, Pillar 9 flips to ✅ in the
readiness doc and this file gets archived under
`docs/archive/2026-vendor-partnerships.md` as a record of
what the engagement looked like at v1.0.
