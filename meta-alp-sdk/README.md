@page meta_alp_sdk_index Yocto meta-layer (meta-alp-sdk)

# meta-alp-sdk

> **Build-validated (partial), 2026-05-26.** The BSP v6.30
> `bitbake-layers` flow below was exercised on WSL: the carrier DT
> patches apply to linux-renesas 6.1.141-cip43 and `core-image-minimal`
> produces the kernel `Image` + carrier dtb + `.wic.gz`.  A full
> `alp-image-edge` bake and on-bench boot are the remaining gates; the
> i.MX 93 path is still paper-correct (gates on v0.7 HiL).

Yocto layer that packages the **Alp SDK** runtime, on-board chip
drivers, edge-AI examples, and reference ROS 2 nodes for the
V2N / V2N-M1 / i.MX 93 Linux side of every supported E1M SoM.

The orchestrator (`scripts/alp_orchestrate/`) emits per-MACHINE
build invocations against this layer; customers who hand-write
firmware skip the orchestrator and consume the layer directly.

## Layout

```
meta-alp-sdk/
├── conf/
│   ├── layer.conf                       # Yocto layer metadata.
│   ├── distro/
│   │   ├── alp.conf                     # Alp distro identity (rebrands Renesas rz-vlp).
│   │   └── include/
│   │       └── mender.inc               # Opt-in Mender OTA distro config.
│   └── machine/
│       ├── e1m-v2n101-a55.conf          # V2N base SoM, A55 Linux cluster.
│       ├── e1m-v2n102-a55.conf          # V2N variant.
│       ├── e1m-v2m101-a55.conf          # V2N + DEEPX DX-M1.
│       ├── e1m-v2m102-a55.conf          # V2N + DEEPX variant.
│       └── e1m-nx9101-a55.conf          # NXP i.MX 93.
├── recipes-core/
│   ├── alp-sdk/
│   │   └── alp-sdk_0.6.bb               # libalp_sdk.so + headers.
│   ├── alp-chips/
│   │   └── alp-chips_0.6.bb             # libalp_chips.a + per-chip PACKAGECONFIG.
│   └── alp-system/
│       ├── alp-dts-reservations_0.6.bb  # Orchestrator-emitted DT reservations.
│       ├── alp-network-defaults_0.7.bb  # Wired-DHCP networkd story pinned in the layer.
│       ├── alp-remoteproc_0.6.bb        # systemd unit for the M-side firmware lifecycle.
│       ├── alp-remoteproc.service
│       ├── alp-ssh-hardening_0.7.bb     # Prod key-only SSH (sshd_config.d drop-in).
│       ├── alp-watchdog-policy_0.7.bb   # CA55-cluster systemd HW-watchdog supervision.
│       └── files/
│           ├── 10-alp-ssh-hardening.conf
│           ├── 10-alp-watchdog.conf
│           ├── 80-alp-wired-dhcp.network
│           └── alp-remoteproc-start.sh
├── recipes-examples/
│   └── alp-edgeai/
│       └── alp-edgeai_0.6.bb            # End-to-end EdgeAI demo (camera → NPU → display).
├── recipes-deepx/
│   └── dx-rt/
│       └── dx-rt_2.4.bb                 # Pins the DEEPX runtime (vendor-licensed).
├── recipes-images/
│   ├── alp-image-common.inc            # Shared runtime for both images below.
│   ├── alp-image-edge.bb                # Dev image: common + debug-tweaks + bench tooling.
│   └── alp-image-prod.bb               # Production image: hardened, key-only SSH (DISTRO=alp).
├── recipes-ros/
│   └── alp-perception/
│       └── alp-perception_0.6.bb        # examples/v2n/v2n-m1-ros-perception node.
└── README.md                            # this file
```

## Naming convention

MACHINE names follow the per-cluster pattern `e1m-<sku>-<cluster>`:

- `<sku>` is the lowercase SoM SKU (`v2n101`, `v2m101`, `nx9101`, ...).
- `<cluster>` is the cluster identifier from
  `metadata/e1m_modules/<SKU>.yaml`'s `topology:` block (`a55` for
  the Linux cluster on V2N / iMX93; the M33 system core builds via
  Zephyr, not Yocto).

This matches what `scripts/alp_orchestrate/` writes into the
emitted `system-manifest.yaml` per the heterogeneous-OS spec at
`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`.

The AEN A32-class MACHINEs (`e1m-aen801-a32`, `e1m-aen701-a32`) carry
carrier scaffolding in-tree, but neither builds today, for related but
distinct reasons: `e1m-aen801-a32.conf`'s active `require` names an
upstream `devkit-e8.conf` that exists in no branch of the public
`meta-alif-ensemble` (#1968), while `e1m-aen701-a32.conf`'s `require`
on `devkit-e7.conf` (which DOES exist upstream, unlike `devkit-e8.conf`)
is commented out pending that layer being vendored at all. Both are
unbuildable regardless: `meta-alif-ensemble` is Yocto-series-incompatible
with this repo's Scarthgap baseline (#1971).  The orchestrator refuses
to emit a `bitbake` command for either MACHINE for the same reason
(#1982).  See the
"Alif Ensemble E8" section below and issue #264 for the rebuild.

## How customers consume it

### V2N / V2N-M1 — via the Renesas RZ/V2N AI SDK (platform 7.1 / BSP v6.30)

Renesas distributes the **RZ/V2N AI SDK** through their own portal
(start at the public [RZ/V2N product
page](https://www.renesas.com/en/products/rz-v2n) under *Software &
Tools*).  Mind the two version axes: the **AI SDK platform is 7.1**,
while the **BSP it rides on is v6.30** (= linux-renesas
`6.1.141-cip43`) -- v6.30 is the revision this carrier was
bring-up-tested against.

The AI SDK comes as two downloads -- an apps/binary package and a
**Source Code** package.  To *build* an image you need the Source
Code package, because that is the one carrying the
`rzv2n_ai-sdk_yocto_recipe_*.tar.gz` tarball.  Extracting it gives
the pre-arranged set of meta-layers below (each a git checkout
pinned to the BSP v6.30 release; the tarball model is canonical
because V2N silicon support may not yet be on the corresponding
`meta-renesas` upstream branch):

> **alp-sdk does not redistribute the Renesas BSP or AI SDK.** Fetch
> them from Renesas under your own account and licence; this repo
> ships only the `meta-alp-sdk` overlay that layers on top.

| Layer                                          | Source repo                                                                    | Role                                                       |
|------------------------------------------------|--------------------------------------------------------------------------------|------------------------------------------------------------|
| `poky`                                         | <https://git.yoctoproject.org/poky>                                            | Yocto base.                                                |
| `meta-arm`                                     | <https://git.yoctoproject.org/meta-arm>                                        | ARM-specific recipes.                                      |
| `meta-openembedded`                            | <https://github.com/openembedded/meta-openembedded>                            | Standard OE recipe collection.                             |
| `meta-renesas`                                 | <https://github.com/renesas-rz/meta-renesas>                                   | Renesas RZ base BSP — provides `rzv2n-evk` MACHINE.        |
| `meta-rz-features/meta-rz-graphics`            | (bundled in `meta-rz-features` under Renesas)                                  | Mali GPU drivers + Weston compositor wiring.               |
| `meta-rz-features/meta-rz-drpai`               | (bundled in `meta-rz-features`)                                                | **DRP-AI userspace runtime + headers.**                    |
| `meta-rz-features/meta-rz-opencva`             | (bundled in `meta-rz-features`)                                                | OpenCV acceleration via DRP.                               |
| `meta-rz-features/meta-rz-codecs`              | (bundled in `meta-rz-features`)                                                | Hardware video codec recipes.                              |
| `meta-econsys`                                 | (bundled; vendored from e-con Systems)                                         | Camera drivers.  Contact e-con Systems for `e-CAM22_CURZH` patch. |

DRP-AI is fully covered by `meta-rz-drpai` — there's no separate
post-build tarball install for the runtime headers, and no NDA gate
on it.  Only the e-con Systems MIPI camera patch requires a
manufacturer contact, and it's optional (only needed if you
populate `e-CAM22_CURZH` on the board).

`meta-rz-graphics` does **not** have the #1176 defect the three layers
above did (or the `alp-image-edge`-only fix): it carries no
`core-image-%.bbappend` at all. Its `conf/layer.conf` `include`s
`include/rz-graphics.inc` → `include/mali-graphics.inc`, which sets
`IMAGE_INSTALL:append:mali-family` — a conf-level override, not a
recipe-name-matched bbappend, so it reaches `alp-image-*` (or any other
image recipe) normally regardless of what the image is called. Its
Mali/Weston wiring was never affected; issue #1176's "Impact" list
naming it alongside the other three was inaccurate, not merely unfixed
— see the closing comment on #1176.

Yocto release: **Scarthgap (5.0.11)**.  GCC 13.  Toolchain SDK:
`bitbake core-image-weston -c populate_sdk` against the matching
MACHINE.

### Build steps

```bash
# 1. Obtain the AI SDK *Source Code* package from Renesas (under your
#    own Renesas account + licence -- alp-sdk does not redistribute
#    it).  Choose the Source Code download, NOT the apps/binary one.
unzip <rzv2n-ai-sdk-source-code>.zip
cd <extracted_dir>

# 2. Extract the recipe tarball; produces poky/, meta-arm/,
#    meta-openembedded/, meta-renesas/, meta-rz-features/, meta-econsys/.
tar zxvf src_setup/rzv2n_ai-sdk_yocto_recipe_*.tar.gz

# 3. Init the Yocto env (the template ships with vlp-v4-conf):
TEMPLATECONF=$PWD/meta-renesas/meta-rz-distro/conf/templates/vlp-v4-conf/ \
    source poky/oe-init-build-env build

# 4. Add the Renesas feature sublayers:
bitbake-layers add-layer ../meta-rz-features/meta-rz-graphics
bitbake-layers add-layer ../meta-rz-features/meta-rz-drpai
bitbake-layers add-layer ../meta-rz-features/meta-rz-opencva
bitbake-layers add-layer ../meta-rz-features/meta-rz-codecs
bitbake-layers add-layer ../meta-econsys

# 4b. ROS 2 layer -- ONLY for images that ship the alp-sdk ROS nodes
#     (e.g. alp-image-edge).  meta-ros2-humble is a LAYERRECOMMENDS, not
#     a hard dep: for a lean image (e.g. core-image-minimal) skip this
#     step and BBMASK the ROS recipes.  It is not in the BSP tarball, so
#     clone it from upstream meta-ros first:
git clone -b scarthgap https://github.com/ros/meta-ros ../meta-ros
bitbake-layers add-layer ../meta-ros/meta-ros2-humble

# 5. Add meta-alp-sdk:
git clone https://github.com/alplabai/alp-sdk ../alp-sdk
bitbake-layers add-layer ../alp-sdk/meta-alp-sdk

# 6. For V2N-M1, also add meta-deepx-m1 (DEEPX's M1 recipes):
git clone https://github.com/DEEPX-AI/meta-deepx-m1 ../meta-deepx-m1
bitbake-layers add-layer ../meta-deepx-m1

# 7. Pick the MACHINE in conf/local.conf:
MACHINE = "e1m-v2n101-a55"     # plain V2N
# or
MACHINE = "e1m-v2m101-a55"     # V2N + DEEPX

# 8. Build the image:
bitbake alp-image-edge                 # dev image (passwordless root, bench tooling)
# or the hardened production image, against the Alp distro identity:
DISTRO=alp bitbake alp-image-prod      # key-only SSH, no debug tooling, "Alp SDK" branding
```

See the edge-vs-prod posture table + `DISTRO=alp` notes in
[`../docs/build-yocto-v2n.md`](../docs/build-yocto-v2n.md#edge-vs-production-image).

The resulting `alp-image-edge-<machine>.wic[.gz]` is the kernel +
rootfs (the bootloader is production-flashed by Alp).  See
[`../docs/build-yocto-v2n.md`](../docs/build-yocto-v2n.md) for the
deploy + on-board verification steps.

### i.MX 93 — via meta-imx

The NX9101 path tracks NXP's
[`meta-imx`](https://github.com/nxp-imx/meta-imx) for the i.MX 93
base BSP plus
[`meta-freescale`](https://git.yoctoproject.org/meta-freescale) for
the broader i.MX userspace stack.  The `e1m-nx9101-a55.conf`
MACHINE ships today; board DTB + full image-bake gate on v0.7
HW-in-loop.

```bash
MACHINE = "e1m-nx9101-a55"
bitbake alp-image-edge
```

### Alif Ensemble E8 — via meta-alif-ensemble (BROKEN today — see #264)

> **This path does not build. Do not follow the steps below as
> written; they are kept only to document what does not work and
> why.** `e1m-aen801-a32` fails at BitBake's own `require` step (its
> `require conf/machine/devkit-e8.conf` names a file absent from every
> branch of the public upstream, issue #1968); `e1m-aen701-a32` never
> reaches that step because its own `require` is commented out pending
> the layer being vendored, so it parses with no base tune / kernel
> provider / TF-A platform set at all. The orchestrator refuses to emit
> a `bitbake` command for either MACHINE (`YOCTO_MACHINE_UNBUILDABLE`
> in `scripts/alp_orchestrate/orchestrator.py`, issue #1982). PR
> **#264** is rebuilding this path on a real base; this section will be
> rewritten once that lands.

The AEN801 (E8) A32 path was intended to ride on Alif's
[`meta-alif-ensemble`](https://github.com/alifsemi/meta-alif-ensemble)
BSP.  Three independent things are wrong with that plan as documented
here previously:

- **There is no `scarthgap` branch.** Verified 2026-09-05: the public
  repo has exactly one branch, `devkit-ex-b0` (also `origin/HEAD`) —
  see issue #1967.
- **Even on `devkit-ex-b0`, `conf/machine/devkit-e8.conf` does not
  exist.** `conf/machine/` there carries only `appkit-e7.conf`,
  `devkit-e5.conf`, `devkit-e7.conf`.  `e1m-aen801-a32.conf`'s active
  `require` names that missing file — see issue #1968. (`devkit-e7.conf`
  DOES exist there; `e1m-aen701-a32.conf`'s `require` on it is
  commented out in-tree pending the layer being vendored at all, a
  separate gap from #1968.) Whether a `scarthgap`-series layer with
  `devkit-e8.conf` exists behind Alif's login-gated support portal is
  unconfirmed; that question needs asking Alif directly, not assuming
  either answer.
- **The layer is structurally incompatible with this repo's Yocto
  baseline regardless.** `devkit-ex-b0`'s `layer.conf` declares
  `LAYERSERIES_COMPAT = "warrior zeus"` (Yocto 3.0, 2019) against
  meta-alp-sdk's Scarthgap (5.0.11) baseline, uses pre-honister
  `_append`/`_prepend` override syntax removed in Yocto 4.0+, pins a
  stale `linux-alif_5.4.bb` kernel (the E8 kernel work that actually
  exists upstream is on `linux_alif` branch `v6.12-dev`, Linux
  6.12.6), and its machine confs pass TF-A build knobs
  (`UART`/`HYPRAM_EN`/`FLASH_EN`/`MODEM_SRAM`/`RAM_PRELOADED_DTB_BASE`)
  that current `trusted-firmware-a_alif` no longer defines — see issue
  #1971.

None of this affects the **M55 side**: the E8 platform builds on
upstream Zephyr's `ensemble_e8_dk` board today, so the heterogeneous
E8 story is real for the M55 HP/HE cores (see
[`../docs/bring-up-aen.md`](../docs/bring-up-aen.md)) — it is only the
A32 Linux cluster's Yocto path that is unbuilt.  alp-sdk does **not**
redistribute or fork the Alif BSP.

```bash
# BROKEN -- kept for documentation only, see the callout above.
# 1. Clone the Alif Ensemble BSP under your own licence (there is no
#    `scarthgap` branch; this clones the only branch that exists):
git clone -b devkit-ex-b0 https://github.com/alifsemi/meta-alif-ensemble ../meta-alif-ensemble
bitbake-layers add-layer ../meta-alif-ensemble

# 2. Add meta-alp-sdk (if not already) and pick the MACHINE:
MACHINE = "e1m-aen801-a32"
bitbake alp-image-edge   # fails: MACHINE parse error, `require
                          # conf/machine/devkit-e8.conf` -- see #1968
```

The `e1m-aen801-a32.conf` / `e1m-aen701-a32.conf` MACHINEs carry carrier
scaffolding in-tree, but neither builds today for the reasons above
(one fails BitBake's `require` parse outright, the other never wires up
a base at all); their carrier DTB, TF-A memory map, and boot-media
routing were also pending maintainer-supplied AEN HW-config inputs (marked
`# TBD(alif-hw-config)`) independent of this defect (E8 silicon is
also flagged `status.preliminary` in
`metadata/e1m_modules/E1M-AEN801.yaml`).  Track the rebuild at #264.

## Per-machine inference runtime

The SDK's `<alp/inference.h>` compiles in the dispatcher for every
backend the SoM preset's `capabilities:` block declares
(silicon-determined), but the **vendor NPU runtimes are not build-time
dependencies of the `alp-sdk` library** — the Yocto build links only
the dispatcher + portable stubs.  Where a runtime userspace package
exists, the **image** recipe installs it (e.g. `alp-image-edge`'s
`IMAGE_INSTALL:append:e1m-v2m101 = "dx-rt"`); DRP-AI3 is driven through
the in-kernel driver + UAPI headers from `meta-rz-drpai` (see below).

| MACHINE              | NPU backend            | Runtime source                              |
|----------------------|------------------------|---------------------------------------------|
| `e1m-v2n101-a55`     | DRP-AI3                | in-kernel driver + `meta-rz-drpai` headers  |
| `e1m-v2n102-a55`     | DRP-AI3                | Same as V2N101 (memory variant)             |
| `e1m-v2m101-a55`     | DRP-AI3 + DEEPX DX-M1  | DRP-AI3 as above; `dx-rt` via the image     |
| `e1m-v2m102-a55`     | Same as V2M101         | Same as V2M101 (memory variant)             |
| `e1m-nx9101-a55`     | Ethos-U65              | NXP i.MX 93 Ethos-U userspace via the image |
| `e1m-aen801-a32`     | Ethos-U85 + 2x U55     | Ethos-U path inside the alp-sdk library     |
| `e1m-aen701-a32`     | 2x Ethos-U55           | Ethos-U path inside the alp-sdk library     |

Customer apps pick the active backend per-handle at runtime via
`alp_inference_open(.backend = ALP_INFERENCE_BACKEND_AUTO)` (or
an explicit `ETHOS_U / DRPAI / DEEPX_DXM1` value for benchmarking).
There is NO build-time pin -- silicon is the source of truth.

### DRP-AI userspace headers

Adding `meta-rz-drpai` (or `meta-rz-codecs` / `meta-rz-opencva`) to
`bblayers.conf` is **not**, by itself, enough to get their payload —
this was issue #1176: each of these vendor layers ships its runtime
packages and its `TOOLCHAIN_TARGET_TASK` SDK-sysroot entries through
its own `recipes-core/images/core-image-%.bbappend`, and a
`core-image-%` bbappend filename does not match any `alp-image-*`
recipe name (bitbake matches a `.bbappend` to its exact target recipe
base name; `%` only wildcards the version suffix). Both halves of the
payload have to be ported explicitly on the `meta-alp-sdk` side —
which is what `alp-image-common.inc` / `packagegroup-alp-camera.bb` do,
gated on the layer's `BBFILE_COLLECTIONS` name (not on `MACHINE`, so
builds that legitimately drop the RZ/V feature layers still parse):

- **Runtime (target rootfs):** `alp-image-common.inc` installs
  `lib-tvm` + `kernel-module-mmngr` into every `alp-image-*` build —
  the DRP-AI3 userspace runtime the `<alp/inference.h>` Yocto backend
  dispatches into at runtime.
- **SDK sysroot headers (`populate_sdk`):** `alp-image-common.inc`
  also ports the vendor bbappends' `TOOLCHAIN_TARGET_TASK:append`
  entries (`drpai` from `meta-rz-drpai`, `drp` — shared — from
  `meta-rz-codecs` / `meta-rz-opencva`), so `bitbake alp-image-* -c
  populate_sdk` actually produces `<linux/drpai.h>` / `<linux/drp.h>`
  (the `drpai_*` / `drp_*` ioctls) at standard sysroot paths. Without
  that port, `populate_sdk` silently produces an SDK missing both
  headers even with the layer present and the image built cleanly.

### Model compilation toolchain (RUHMI / DRP-AI TVM)

Models for DRP-AI compile through Renesas's RUHMI (formerly
DRP-AI TVM) toolchain on the build host — not at image build
time.  It's a separate Apache-2.0 project at
<https://github.com/renesas-rz/rzv_drp-ai_tvm>; model authors
install it on their workstation and ship the compiled output
as a model asset.

The image build needs more than `meta-rz-drpai` alone.  That layer
supplies `<linux/drpai.h>` (recipe `drpai`) and `libtvm_runtime.so`
(recipe `lib-tvm`), but the rest of the MERA2 runtime closure is
staged by `recipes-renesas/mera2-drpai-tvm`, which reads it out of a
BUILT `rzv_drp-ai_tvm` (RUHMI) checkout the builder points at with
`RUHMI_DRPAI_TVM_DIR`.  That recipe fetches and vendors nothing.  All
three are pulled in together by the `alp-sdk` recipe's
`PACKAGECONFIG[drpai]`; see `docs/bring-up-drpai-v2n.md` section 4 for
the full procedure and section 3 for the separate `ALP_ENABLE_DRPAI`
switch that enables the kernel-side node.

## OTA via Mender (opt-in)

`meta-alp-sdk` ships an opt-in Mender integration at
[`conf/distro/include/mender.inc`](conf/distro/include/mender.inc).
When enabled, every reference image gains:

- A `.mender` artefact next to the standard `.wic` / `.tar.bz2`
  outputs.
- An A/B rootfs partition layout (1 GiB per slot by default;
  override via `MENDER_STORAGE_TOTAL_SIZE_MB`).
- The on-target Mender client + `mender-connect` daemon.
- Atomic image swap with bootloader-assisted rollback on failed
  health check.

The integration is **opt-in** — builds that don't ship OTA can
ignore it entirely, and `bitbake-layers parse-recipes` stays
clean without `meta-mender-core` on `bblayers.conf`.

### Enabling Mender on a build

```bash
# 1. Add meta-mender-core to bblayers.conf:
git clone -b scarthgap https://github.com/mendersoftware/meta-mender \
    ../meta-mender
bitbake-layers add-layer ../meta-mender/meta-mender-core

# 2. Uncomment the `require conf/distro/include/mender.inc` line
#    in the machine .conf for your target, OR add it to local.conf.

# 3. Production fleets: override the server + tenant token in
#    local.conf BEFORE the first image build:
echo 'MENDER_SERVER_URL = "https://your-mender-instance"' >> conf/local.conf
echo 'MENDER_TENANT_TOKEN = "your-tenant-token"'          >> conf/local.conf

# 4. Build the artefact:
bitbake alp-image-edge
# Produces:
#   tmp/deploy/images/${MACHINE}/alp-image-edge-${MACHINE}.mender
#   tmp/deploy/images/${MACHINE}/alp-image-edge-${MACHINE}.wic.gz
```

`flash` the `.wic.gz` for first-boot provisioning; subsequent
updates ride the `.mender` artefact through the Mender server.

### Mender status + scope

- Recipe wiring lands in v0.6 (this revision).
- Real artefact generation + on-device install + rollback test
  parked behind an explicit Yocto bench run -- there is no automated
  HIL runner -- per
  [`docs/ci/HW-IN-LOOP.md`](../docs/ci/HW-IN-LOOP.md).
- The Mender-server side (deployment orchestration, fleet
  monitoring) is out of scope for `meta-alp-sdk`; consumers stand
  up a hosted or self-hosted Mender server independently (per
  the project memory note "OTA server owned by Hakan, separate repo").
- Reference rollout: [`docs/ota.md`](../docs/ota.md).

## Licence

Apache-2.0 (umbrella).  Vendor-licensed components (`dx-rt`,
`drp-ai-tvm`) follow their upstream licences and are flagged as
such in the matching recipes' `LICENSE` field.

## What's deferred

- `dx-rt_*.bb` is a skeleton — the DEEPX SDK signed-licence
  acknowledgement closes the legal review per
  [`docs/vendor-partnerships.md`](../docs/vendor-partnerships.md)
  §C.31.
- AEN A32-class MACHINE carrier scaffolding ships for five SKUs
  (`e1m-aen{501,601,701,801,803}-a32`), but NONE of the five build
  today -- `e1m-aen801-a32` / `e1m-aen701-a32` carry a broken or
  commented-out `require` on a real-but-unbuildable meta-alif-ensemble
  base (issues #1968 / #1971); `e1m-aen501-a32` / `e1m-aen601-a32` /
  `e1m-aen803-a32` ship no conf at all. The orchestrator refuses to
  emit a `bitbake` command for any of the five
  (`YOCTO_MACHINE_UNBUILDABLE` in
  `scripts/alp_orchestrate/orchestrator.py`, issue #1982). See the
  "Alif Ensemble E8" section above and issue #264 for the rebuild;
  the carrier DTB + TF-A memory map + full image-bake for whichever
  SKU #264 lands first also await the maintainer's AEN HW config (the
  `# TBD(alif-hw-config)` overrides in the machine confs).
- `alp-image-edge.bb`'s minimal package set is documentary; the
  v1.0 sysbuild matrix in `docs/test-plan.md` adds the BLE
  provisioning layer + the certificate-pinning post-install hook.

## Verification status

**Partial.** `core-image-minimal` baked on the BSP v6.30 flow (WSL,
2026-05-26): the carrier DT patches apply and the kernel + carrier dtb
+ image build.  Still pending: a full `alp-image-edge` bake (ROS 2 +
DEEPX + Mender recipes) and on-bench boot — the v0.7 V2N HiL gate.  The
i.MX 93 path remains unbaked.

## See also

- [*RZ/V2N Group Handbook*](https://www.renesas.com/en/document/oth/rzv2n-group-handbook)
  — Renesas's master index of V2N collateral.
- [RZ/V2N product page (AI SDK + BSP downloads)](https://www.renesas.com/en/products/rz-v2n)
  — Software overview + getting-started + how-to-build.
- [`vendors/deepx-dxm1/README.md`](../vendors/deepx-dxm1/README.md)
  — DEEPX DX-M1 integration notes (covers V2M101 / V2M102).
- `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`
  — the orchestrator spec this layer is wired to.
