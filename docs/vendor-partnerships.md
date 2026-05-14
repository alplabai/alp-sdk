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

**Open items**:

- 📋 **Public RZ/V2N N44 FSP release.**  We're currently
  building against a pre-release tarball Renesas shared
  under NDA in 2026-Q1.  v1.0 of the SDK needs the public
  FSP shipped + the metadata under `metadata/socs/renesas/
  rzv2n/n44.json` matches what Renesas publishes.
- 📋 **DA9292 + V2N PMIC pad-routing confirmation.**  The
  bring-up sequence in `chips/da9292/da9292.c::da9292_v2n_m1_enable_deepx_rail`
  encodes the maintainer's reading of the V2N schematic +
  the AROVx OTP variant trap.  Renesas' AE has confirmed
  the schematic; the OTP confirmation is open.
- 📋 **DRP-AI runtime license.**  v0.5 inference dispatcher
  exposes the DEEPX path; the RZ/V2N's on-die DRP-AI is a
  separate runtime we plan to integrate.  Licensing terms
  pending.

**Next action**: Q2 sync call with Renesas AE.

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
