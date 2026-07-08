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
3. OpenEmbedded layerindex registration for `meta-alp-sdk`.

None of these are SDK source-tree changes; they're meta-work
that gates the customer onramp.  This doc tracks the open
items + owner + next-action per vendor so the maintainer
doesn't lose state between weekly partnership-review cycles.

## Renesas (RZ/V2N family)

**Surface impact**: `chips/gd32g553/` host driver +
`src/zephyr/v2n_supervisor.c` + `src/zephyr/v2n_power_mgmt.c`
all assume an upstream Renesas FSP for the RZ/V2N N44 SoC.

**Status update 2026-05-14 (verified against upstream)**:

**Zephyr-integration status (§C.40 cross-check)**: Zephyr's
`hal_renesas` module already mirrors the RZ/V FSP under
`drivers/rz/fsp/src/rzv/bsp/mcu/rzv2n/` -- Zephyr v3.7 pins
revision `af77d7cd...`, which a customer `west update` pulls
automatically once `hal_renesas` is in the name-allowlist
(landed §C.40).  No extra customer-side setup needed for
Zephyr builds.  Bare-metal customers can still pull the
upstream `rzv-fsp` directly via our `vendor-sdks` group
(pinned to v3.1.0 for audit clarity).

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

**Status update 2026-05-14 (verified against upstream)**:

Alif Semiconductor publishes 59 repos at
[`github.com/alifsemi`](https://github.com/alifsemi).  The
relevant ones for the SDK split cleanly into two licensing
buckets.

**Alif Zephyr-integration story** (verified §C.40, refined §C.41 + §C.42):

The Alif story has **two pieces** -- HAL drivers and Zephyr
board files -- and they live in different upstreams.  Both
matter for AEN builds.

**Piece 1 -- HAL drivers (`hal_alif`)**: Apache-2.0,
standalone, at
[`github.com/alifsemi/hal_alif`](https://github.com/alifsemi/hal_alif).
Latest release v2.2.0 (2026-03-27); steady release cadence
(v2.1.0 Dec 2025, v2.0.0 Nov 2025).  Standard Zephyr-module
shape (`zephyr/module.yml` + `zephyr/Kconfig` + root
`CMakeLists.txt`) -- structurally indistinguishable from
`zephyrproject-rtos/hal_renesas` / `hal_nxp`.  Our `west.yml`
imports it **unconditionally** as a top-level project so the
HAL is on every workspace.

**Piece 2 -- Zephyr board files**: As of Zephyr v4.4 (the SDK's
current pin) upstream Zephyr ships three Alif Ensemble board
families directly under `boards/alif/`: `ensemble_e8_dk` (with
multiple silicon-variant qualifiers), `ensemble_e1c_dk`, and
`balletto_b1_dk`.  This is **new in v4.x** -- v3.7 LTS had no
Alif boards at all, and Alif's own
[`alifsemi/zephyr_alif`](https://github.com/alifsemi/zephyr_alif)
fork was the only path to stock board files.  With v4.4 the
upstream coverage is enough for SDK CI -- our example twister
scenarios target `ensemble_e8_dk/ae402fa0e5597le0/rtss_hp` as
the AEN proxy.  Customers wanting the **full** Alif EVK board
catalogue (the older 8-board set under `alif_e7_*` naming) still
fall back to `sdk-alif` (zas-v2.0.0-rc1) in the
**`vendor-sdks` opt-in group**.

**Decision tree** for AEN customers:

```
Building for an Alif Ensemble EVK?
├── ensemble_e8_dk / ensemble_e1c_dk / balletto_b1_dk
│         → our default workspace works.  Upstream Zephyr v4.4
│           ships these in tree; the top-level hal_alif import
│           gives you the HAL drivers.  west build -b <name>/<qualifier>.
├── Board-specific design (E1M-X SoM + custom board)
│         → keep the default workspace; ship your own board file
│           under alplabai/alp-zephyr-modules with the alp,pin-array
│           slot defines.  hal_alif via west update is sufficient.
└── Older Alif EVK variant not in upstream (alif_e7_dk_rtss_he etc.)
          → enable vendor-sdks + use sdk-alif as workspace topdir
            for the full 8-board set from Alif's zephyr_alif fork.
```

§C.40 had the wrong assumption that `hal_alif` had to come
through `sdk-alif`'s manifest -- it doesn't, the standalone
HAL is enough for custom-board customers.  §C.41 then went too
far and removed `sdk-alif` from `vendor-sdks` entirely -- which
broke the stock-EVK path.  §C.42 restores `sdk-alif` to
`vendor-sdks` with the two-path documentation above.

**Vendor-licensed Alif drivers** (`alif_dave2d-driver`,
`alif_image-processing-lib`): source-visible under the Alif
Semiconductor Software License Agreement, not Apache.  Stay
in the `vendor-sdks` opt-in group alongside `sdk-alif` so the
explicit opt-in matches the licence consent story.

**Genuinely open (Apache-2.0 / MIT, inherited from upstream
forks)**:

| Repo                       | License     | Purpose                                           |
|----------------------------|-------------|---------------------------------------------------|
| `zephyr_alif`              | Apache-2.0  | Alif's Zephyr fork                                |
| `hal_alif`                 | Apache-2.0  | HAL integration layer for Zephyr                  |
| `cmsis_alif`               | Apache-2.0  | CMSIS fork                                        |
| `mcuboot_alif`             | Apache-2.0  | MCUboot port for Ensemble                         |
| `matter_alif`              | Apache-2.0  | Matter port                                       |
| `tinyusb` (Alif's port)    | MIT         | TinyUSB stack                                     |
| `meta-alif`                | MIT         | Ensemble BSP Yocto layer                          |
| `meta-alif-ensemble`       | MIT         | Ensemble device recipes                           |
| `meta-alif-iot`            | MIT         | IoT meta layer                                    |
| `alif-sdk-containers`      | MIT         | Docker containers for building releases           |

**Vendor-licensed (Alif Semiconductor Software License
Agreement) for the differentiating drivers**:

| Repo                                | Purpose                                    |
|-------------------------------------|--------------------------------------------|
| `sdk-alif`                          | Zephyr SDK aggregate                        |
| `alif_dave2d-driver`                | D/AVE 2D graphics driver                   |
| `alif_lvgl-dave2d`                  | LVGL+D/AVE 2D integration                  |
| `alif_image-processing-lib`         | D/AVE 2D + Helium image-processing kernels |
| `alif_ml-embedded-evaluation-kit`   | Ethos-U eval-kit fork                      |
| `alif_ensemble-cmsis-dfp`           | CMSIS Device Family Pack                   |

Same pattern as NXP: Alif's forks of upstream OSS keep
upstream's permissive licensing; their proprietary
differentiating drivers (DAVE2D, Ethos-U eval kit, ISP
helpers) ride a vendor-specific licence.  Source-visible
but with Alif terms.

The `sdk-alif` aggregate Zephyr SDK has been **steadily
released** (v2.3.0-rc1 2026-05-09, v2.2.0 2026-03-27,
v2.1.0 2026-01-21) -- the tracker's "Alif HAL v1.6 ships
2026-Q2" line was reading the wrong release stream.  The
DAVE2D + Ethos-U + CSI driver repos are **already public**
(updated 2026-04-30 / 2026-05-13).

**Open items**:

- [x] **Public AEN Zephyr SDK availability.**  Public at
  `github.com/alifsemi/sdk-alif`, with steady releases.
  Tracker line item closed; what was open is just our own
  alignment cadence to specific tags.
- [x] **DAVE2D + Ethos-U evaluation kit availability.**
  `alif_dave2d-driver` + `alif_ml-embedded-evaluation-kit`
  both already public (vendor-licensed, source-visible).
- [x] **Alif-licence acknowledgement in v1.0 docs.**  Section 8
  of `docs/getting-started.md` now carries a per-vendor licence
  table covering Alif / Renesas / NXP / DEEPX (landed §C.36).
- 📋 **Dual-image build flow upstreaming.**  The §C.30
  HE-side peer image needs sysbuild glue that builds both
  HP + HE halves in one invocation.  This sits in
  `alplabai/alp-zephyr-modules` (not this repo); the
  upstream pattern from `sdk-alif` v2.x sysbuild can be
  the model.

**Next action**: Sync `metadata/socs/alif/ensemble/*.json`
to the v2.3.0-rc1 `sdk-alif` Zephyr-board manifests; ping
Alif about the §C.30 dual-image pattern once that lands.

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
- [x] **V2N-M1 DEEPX inference sample skeleton.**  Landed
  §C.37 at `examples/v2n/v2n-m1-deepx-inference/`.
  Build-only Twister scenario today; flips to a positive-
  path run once a V2N-M1 board file + the customer-side
  `dx_rt` integration are wired up on a HiL rig.

**Next action**: Ping DEEPX customer support about the
`meta-deepx-m1` LICENSE.

## NXP (i.MX 93 family)

**Surface impact**: `examples/imx93-*` (none today, planned)
+ Yocto layer + the `<alp/storage.h>` OTFAD backend that
ships when NXP's FlexSPI OTFAD driver stabilises.

**Status update 2026-05-14 (verified against upstream)**:

**Zephyr-integration status (§C.40 cross-check)**: Zephyr's
`hal_nxp` module mirrors MCUXpresso under
`mcux/mcux-sdk-ng/devices/i.MX/i.MX93/` -- 10 i.MX 93 SKU
device-headers (MIMX9301 .. MIMX9352) in tree.  Zephyr v3.7
pins revision `862e0015...`.  E1M-NX9101's target part
(MIMX9352) is covered.  No extra customer-side setup needed
for Zephyr builds once `hal_nxp` is in the name-allowlist
(landed §C.40).  Bare-metal MCU customers pull the
manifest-aware MCUXpresso directly via our `vendor-sdks`
group (pinned to v26.03.00 for audit clarity).

NXP publishes the MCUXpresso SDK on GitHub as a manifest
repo at
[`github.com/nxp-mcuxpresso/mcuxsdk-manifests`](https://github.com/nxp-mcuxpresso/mcuxsdk-manifests).
Latest stable tag **v26.03.00** (Q1 2026), prerelease tags
for v26.06.00 (Q2 2026) already in flight.  Layout:

- `west.yml` -- Zephyr-style west manifest aggregating the
  per-component repos (HAL, RTOS, examples).
- `boards/` -- 11 i.MX 9x board manifests: `mcimx93evk`,
  `mcimx93autoevk`, `mcimx93qsb`, `mcimx93wevk`,
  `frdmimx95`, `imx95verdinevk`, plus several i.MX 95 EVK
  variants.

**License is the NXP-specific `LA_OPT_Online Code Hosting
NXP_Software_License v1.4` (May 2025)** -- not Apache /
BSD.  Source-visible but with NXP terms (acceptance of the
licence implied by clone / install / use).  Customers
integrating against MCUXpresso are already familiar with
this; it's not a Yocto / Zephyr-LTS-style permissive
licence.

The mcuxsdk-manifests covers the **MCU-side** of i.MX 9x
(the Cortex-M33 cores running real-time workloads).  The
**Yocto / Linux-side** (Cortex-A55 application processor)
ships through `meta-imx` -- a separate release cycle.

**Open items**:

- [x] **i.MX 9x MCU-side SDK availability.**  Public at
  `github.com/nxp-mcuxpresso/mcuxsdk-manifests` v26.03.00
  with `mcimx93evk` + 3 other i.MX 93 board manifests
  in-tree.
- [x] **NXP-licence acknowledgement in v1.0 docs.**  Section 8
  of `docs/getting-started.md` now carries the per-vendor licence
  table covering all four vendors (landed §C.36).
- 📋 **i.MX 93 Yocto BSP confirmation.**  meta-imx releases
  cycle quarterly; v1.0 of the SDK aligns to whichever
  meta-imx release ships closest to our v1.0 tag.  No open
  technical issues; tracking the calendar.
- 📋 **OTFAD inline-AES driver upstreaming.**  The
  `<alp/storage.h>` inline-AES surface (`alp_storage_configure_inline_aes`)
  reaches NXP silicon through NXP's OTFAD driver; that
  driver is mainline in Linux but not yet in Zephyr.  No
  public Zephyr issue or PR exists as of 2026-07-08; Alp-side
  tracking remains in `alplabai/alp-sdk#456` until an upstream
  tracker is filed.

**Next action**: 2026-Q3 sync after the v26.06.00 MCUXpresso
release stabilises + meta-imx mickledore ships.

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

## OpenEmbedded layerindex (meta-alp-sdk)

**Surface impact**: Customer Yocto integration.  Today
customers add `meta-alp-sdk` via a layers.conf edit; once
registered, `bitbake-layers add-layer meta-alp-sdk` finds it
through the standard layer-fetcher path.

**Open items**:

- 📋 **Submit `meta-alp-sdk` to layerindex.**  One-time
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
