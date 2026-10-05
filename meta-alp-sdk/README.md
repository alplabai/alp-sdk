@page meta_alp_sdk_index Yocto meta-layer (meta-alp-sdk)

# meta-alp-sdk

> **Build-validated (partial), 2026-05-26.** The BSP v6.30
> `bitbake-layers` flow below was exercised on WSL: the carrier DT
> patches apply to linux-renesas 6.1.141-cip43 and `core-image-minimal`
> produces the kernel `Image` + carrier dtb + `.wic.gz`.  A `drpai`-OFF
> `alp-image-edge` bake has since completed too -- see
> [`docs/bring-up-drpai-v2n.md`](../docs/bring-up-drpai-v2n.md)'s status
> banner for the task count and artefact.  On-bench boot and a
> `drpai`-enabled bake are the remaining gates; the i.MX 93 path is
> still paper-correct (gates on v0.7 HiL).

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
│       ├── e1m-v2n103-a55.conf          # V2N variant (4 GB / 16 GB).
│       ├── e1m-v2m101-a55.conf          # V2N + DEEPX DX-M1.
│       ├── e1m-v2m102-a55.conf          # V2N + DEEPX variant.
│       ├── e1m-v2m103-a55.conf          # V2N + DEEPX variant (4 GB / 16 GB).
│       ├── e1m-nx9101-a55.conf          # NXP i.MX 93.
│       └── include/
│           └── e1m-v2m-deepx.inc        # Shared DEEPX block `require`d by the three V2M confs above.
├── dynamic-layers/
│   ├── meta-alif-ensemble/
│   │   └── recipes-kernel/linux/
│   │       └── linux-alif_%.bbappend    # E1M-AEN console routing (parsed only when meta-alif-ensemble is in bblayers.conf).
│   └── meta-deepx-m1/
│       └── recipes-runtime/
│           ├── dx-driver/
│           │   └── dx-driver_%.bbappend # Tightens the 99-dx-dma.rules udev MODE (parsed only when meta-deepx-m1 is in bblayers.conf).
│           └── dx-rt/
│               ├── dx-rt_%.bbappend     # dxrtd service mode, dxrt-cli sub-package fix, and a stderr warning before dxrt-cli -u / -w / -C.
│               └── dx-rt/alp_fw_warning.cpp  # The warning text (Alp code; the bbappend inserts one call per command).
├── recipes-core/
│   ├── alp-sdk/
│   │   └── alp-sdk_0.6.bb               # libalp_sdk.so + headers.
│   ├── alp-chips/
│   │   └── alp-chips_0.6.bb             # libalp_chips.a + per-chip PACKAGECONFIG.
│   ├── alp-hostname/
│   │   └── alp-hostname_0.1.bb          # Hostname from the SoM SKU (/chosen/alp,sku).
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
│   ├── alp-edgeai/
│   │   └── alp-edgeai_0.6.bb            # End-to-end EdgeAI demo (camera → NPU → display).
│   ├── alp-lvgl-dashboard/
│   │   └── alp-lvgl-dashboard_0.6.bb    # LVGL dashboard on the X-EVK MIPI-DSI panel.
│   └── alp-drpai-inference/
│       └── alp-drpai-inference_0.6.bb   # DRP-AI3 still-frame inference exhibition demo.
├── recipes-renesas/
│   └── mera2-drpai-tvm/
│       └── mera2-drpai-tvm_2.7.0.bb     # Stages + compiles the MERA2/TVM runtime from a builder-supplied RUHMI checkout.
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
| `meta-rz-features/meta-rz-drpai`               | (bundled in `meta-rz-features`)                                                | **DRP-AI kernel driver + `drpai0` DT label + `<linux/drpai.h>` + `libtvm_runtime.so`** (NOT the whole runtime — see below). |
| `meta-rz-features/meta-rz-opencva`             | (bundled in `meta-rz-features`)                                                | OpenCV acceleration via DRP.                               |
| `meta-rz-features/meta-rz-codecs`              | (bundled in `meta-rz-features`)                                                | Hardware video codec recipes.                              |
| `meta-econsys`                                 | (bundled; vendored from e-con Systems)                                         | Camera drivers.  Contact e-con Systems for `e-CAM22_CURZH` patch. |

`meta-rz-drpai` does **not** cover all of DRP-AI.  It supplies four
things:

1. the DRP-AI kernel driver (its `0002-*` patch),
2. the `drpai0` DT node + label in `r9a09g056.dtsi` (its
   `0001-add-drpai-property-to-devicetree.patch`) — the label does
   **not** exist in the pristine linux-renesas tree,
3. the `<linux/drpai.h>` UAPI header (recipe `drpai`, 1.4.0), and
4. `libtvm_runtime.so` (recipe `lib-tvm`).

Everything else the alp-sdk DRP-AI3 backend compiles and links against
— `MeraDrpRuntimeWrapper.h`, `mera2_runtime`, `mera2_plan_io`,
`drp_tvm_rt`, and `mera_drpai_wrapper` — is packaged by
`recipes-renesas/mera2-drpai-tvm`, a recipe in **this** layer: it
fetches and vendors nothing, it only stages/compiles those headers and
libraries out of a built RUHMI / `rzv_drp-ai_tvm` checkout that the
builder points it at (see the "Model compilation toolchain
(RUHMI / DRP-AI TVM)" section below, which also covers making the
checkout visible to the bake).
`mera_drpai_wrapper` is the one exception to "staging-only": RUHMI
ships no prebuilt library for `MeraDrpRuntimeWrapper`'s own symbols
(ctor, `Run`, `SetInput`, `GetInputInfo`, …) at all — they are
application-side glue *source*
(`apps/MeraDrpRuntimeWrapper.cpp`) every RUHMI sample app compiles for
itself — so this recipe compiles that one file into
`libmera_drpai_wrapper.so` and packages it alongside the other eight.
There is no NDA gate on any of it (the `rzv_drp-ai_tvm` sources,
including that glue source, are Apache-2.0), but the prebuilt MERA2
libraries and the Translator are Renesas/EdgeCortix account-gated and
are not vendored here or anywhere else in this public repo.

`meta-rz-drpai` is a **soft** dep of this layer
(`LAYERRECOMMENDS_alp-sdk`, not `LAYERDEPENDS_alp-sdk`) — the AEN and
NX91 machines have no DRP-AI silicon and must not be forced to carry an
RZ/V-only vendor layer.  The `linux-renesas` bbappend therefore gates
the `&drpai0` overlay on the layer being in `bblayers.conf`
(`ALP_DRPAI_LAYER`): present → the real override in
`recipes-kernel/linux/linux-renesas/e1m-v2n-drpai.dtsi` is installed;
absent → a comment-only stub of the same filename, so the board dtb
still compiles and the NPU is simply left unclaimed.  **Without the
layer there is no `/dev/drpai0`**, and every
`alp_inference_open(.backend = DRPAI)` fails regardless of how the SDK
was built.

Only the e-con Systems MIPI camera patch requires a manufacturer
contact, and it's optional (only needed if you populate
`e-CAM22_CURZH` on the board).

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

# 6. For V2N-M1 / V2M, also add DEEPX's own official meta-deepx-m1
#    layer, pinned to the verified commit:
git clone -b scarthgap https://github.com/DEEPX-AI/meta-deepx-m1.git ../meta-deepx-m1
git -C ../meta-deepx-m1 checkout 8d09b25f20f81104c16c7de90928ff8920eb482d
bitbake-layers add-layer ../meta-deepx-m1

# 7. Pick the MACHINE in conf/local.conf:
MACHINE = "e1m-v2n101-a55"     # plain V2N
# or
MACHINE = "e1m-v2m101-a55"     # V2N + DEEPX

# 7b. DRP-AI3 NPU.  With meta-rz-drpai in bblayers.conf the &drpai0
#     node (/dev/drpai0) is ON by default on every V2N/V2M MACHINE
#     (ALP_ENABLE_DRPAI = "0" opts out).  The SDK backend in
#     libalp_sdk.so additionally needs the MERA2 runtime, built from a
#     RUHMI checkout; point at one and PACKAGECONFIG[drpai] turns on by
#     itself (see "Model compilation toolchain (RUHMI / DRP-AI TVM)"
#     below and docs/bring-up-drpai-v2n.md section 4).  BENCH-UNVERIFIED.
RUHMI_DRPAI_TVM_DIR = "/path/to/built/rzv_drp-ai_tvm"

# 8. The DEEPX runtime (dx-driver + dx-rt + dx-rt-cli) is installed
#    automatically on the V2M MACHINEs once step 6's layer is present;
#    set ALP_ENABLE_DEEPX_DXM1 = "0" in local.conf to leave it out.

# 8b. The ONNX Runtime CPU floor (`PACKAGECONFIG[ort]`, own onnxruntime
#     recipe) is on by default on V2N101/V2N102/V2N103 and, when the DEEPX
#     runtime is off, V2M101/V2M102/V2M103 (`ALP_ENABLE_ORT_CPU`, "0" opts
#     out).  With DEEPX on, V2M defaults it off: dx-rt brings its own
#     libonnxruntime and the two packages collide.  AUTO never picks it.

# 9. Build the image:
bitbake alp-image-edge                 # dev image (passwordless root, bench tooling)
# or the hardened production image, against the Alp distro identity:
DISTRO=alp bitbake alp-image-prod      # key-only SSH, no debug tooling, "Alp SDK" branding
```

See the edge-vs-prod posture table + `DISTRO=alp` notes in
[`../docs/build-yocto-v2n.md`](../docs/build-yocto-v2n.md#edge-vs-production-image).

#### Verifying the ROS 2 payload lands (#372)

The one supported command for a ROS 2-carrying image is steps 4b + 7 + 8
above: `MACHINE = "e1m-v2n101-a55"` (or `e1m-v2m101-a55`), then
`bitbake alp-image-edge`, with `meta-ros2-humble` added to `bblayers.conf`.
`alp-image-edge.bb` turns on `IMAGE_FEATURES += "alp-ros"`, which
`alp-image-common.inc`'s `FEATURE_PACKAGES_alp-ros` maps to
`packagegroup-alp-ros` -- whose `RDEPENDS:${PN}` names both `rclcpp` and
`alp-perception` (`recipes-core/packagegroups/packagegroup-alp-ros.bb`).
Without a Yocto CI build lane in alp-sdk CI, that dependency chain --
not a finished image manifest -- is the grounded proof this command puts
both packages on the rootfs; `tests/scripts/test_library_layer.py`
(`test_ros2_edge_image_pulls_rclcpp_and_alp_perception`) pins the chain so
it can't silently drift. That gap (no real Yocto CI build) is also why
`ros2.yaml` stays Tier B: ADR 0018 Tier A requires "built in CI for at
least one board", which a doc-only proof cannot satisfy.

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

Once this path builds, TF-A's BL32 console needs a carrier-specific
UART base + pinmux, not the Alif DevKit's UART2 default — see
`recipes-bsp/trusted-firmware-a/trusted-firmware-a/alif-console-uart-build-knobs.patch`
and the `:e1m-aen801`/`:e1m-aen701` knobs in
`trusted-firmware-a_%.bbappend` (#1979). That patch is inert today for
the same reason this whole section is broken.

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
exists, the **image** recipe installs it (e.g.
`conf/machine/include/e1m-v2m-deepx.inc` appending `dx-driver dx-rt
dx-rt-cli` when `ALP_ENABLE_DEEPX_DXM1 = "1"` (the default once meta-deepx-m1 is in bblayers.conf) -- `dxrt-cli`, `run_model`
and the other tools ship in the `dx-rt-cli` sub-package);
DEEPX DX-M1's `deepx-dxm1` PACKAGECONFIG pulls only the `dx-rt`
build dependency (headers + libdxrt), not the runtime install.

**DRP-AI3 is the exception.**  Its backend
(`src/yocto/inference_drpai.cpp`) is real `MeraDrpRuntimeWrapper` code;
when it is compiled in, the MERA2 / TVM runtime *is* a build-time
dependency of `libalp_sdk.so`, and `<linux/drpai.h>` is a build-time
dependency of the recipe.

| MACHINE              | NPU backend                          | Runtime source                                                        |
|----------------------|--------------------------------------|-----------------------------------------------------------------------|
| `e1m-v2n101-a55`     | DRP-AI3 — node on by default with `meta-rz-drpai`; backend (`PACKAGECONFIG[drpai]`) on when `RUHMI_DRPAI_TVM_DIR` is set; BENCH-UNVERIFIED | kernel driver + `<linux/drpai.h>` + `libtvm_runtime.so` from `meta-rz-drpai`; `mera2_runtime` / `mera2_plan_io` / `drp_tvm_rt` (staged) + `mera_drpai_wrapper` (compiled from `apps/MeraDrpRuntimeWrapper.cpp`) from a built RUHMI checkout |
| `e1m-v2n102-a55`     | DRP-AI3 — node on by default with `meta-rz-drpai`; backend (`PACKAGECONFIG[drpai]`) on when `RUHMI_DRPAI_TVM_DIR` is set; BENCH-UNVERIFIED | Same as V2N101 (memory variant)                                       |
| `e1m-v2n103-a55`     | DRP-AI3 — node on by default with `meta-rz-drpai`; backend (`PACKAGECONFIG[drpai]`) on when `RUHMI_DRPAI_TVM_DIR` is set; BENCH-UNVERIFIED | Same as V2N101 (memory variant)                                       |
| `e1m-v2m101-a55`     | DRP-AI3 — node on by default with `meta-rz-drpai`; backend (`PACKAGECONFIG[drpai]`) on when `RUHMI_DRPAI_TVM_DIR` is set; BENCH-UNVERIFIED + DEEPX DX-M1 — opt-in (`ALP_ENABLE_DEEPX_DXM1`) | DRP-AI3 as above; `dx-driver`/`dx-rt` via `meta-deepx-m1` (`ALP_ENABLE_DEEPX_DXM1`) |
| `e1m-v2m102-a55`     | Same as V2M101                       | Same as V2M101 (memory variant)                                       |
| `e1m-v2m103-a55`     | Same as V2M101                       | Same as V2M101 (memory variant)                                       |
| `e1m-nx9101-a55`     | Ethos-U65                            | NXP i.MX 93 Ethos-U userspace via the image                           |
| `e1m-aen801-a32`     | Ethos-U85 + 2x U55                   | Ethos-U path inside the alp-sdk library                               |
| `e1m-aen701-a32`     | 2x Ethos-U55                         | Ethos-U path inside the alp-sdk library                               |

See `docs/bring-up-drpai-v2n.md` section 4 for the full DRP-AI3 two-switch
contract (what each of `ALP_ENABLE_DRPAI` / `PACKAGECONFIG[drpai]` actually
controls -- `ALP_ENABLE_DRPAI` gates the `&drpai0` devicetree node and,
`alp-image-edge` only, the demo install; `PACKAGECONFIG[drpai]` compiles the
SDK backend; neither installs the `lib-tvm` + `kernel-module-mmngr`
userspace pair, which is `alp-image-common.inc`'s job -- and what omitting
either switch does).

Customer apps still pick the active backend per-handle at runtime via
`alp_inference_open(.backend = ALP_INFERENCE_BACKEND_AUTO)` (or an
explicit `ETHOS_U / DRPAI / DEEPX_DXM1` value for benchmarking) — the
image does not pin one backend.  The one thing decided at build time is
whether the DRP-AI3 backend is *present in the library at all*; when it
is not, a `DRPAI`-requesting `alp_inference_open` returns `NULL` with
`ALP_ERR_NOSUPPORT` (and on a plain V2N, `AUTO` does the same) rather
than silently routing elsewhere.

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

Apache-2.0 (umbrella).  Vendor-licensed components follow their
upstream licences and are flagged as such in the matching recipes'
`LICENSE` field: the `rzv_drp-ai_tvm` sources are Apache-2.0 but the
prebuilt MERA2 libraries and the Translator are Renesas/EdgeCortix
account-gated (`mera2-drpai-tvm`'s `LICENSE = "CLOSED"` reflects
that gap, not an assertion of a license this recipe could grant);
`mera2-drpai-tvm` only stages a builder-local checkout, it fetches
nothing.  The DEEPX DX-M1 driver + runtime
(`dx-driver`, `dx-rt`) come from DEEPX's own
`meta-deepx-m1` layer (github.com/DEEPX-AI/meta-deepx-m1); those
recipes declare `LICENSE = "Proprietary"`, and the `dx_rt` /
`dx_rt_npu_linux_driver` source they fetch carries DEEPX's own
customer-only licence terms ("provided exclusively to customers who
are supplied with DEEPX NPU" — the driver source also carries SPDX
GPL-2.0 headers in places, an ambiguity in DEEPX's own upstream that
this repo does not attempt to resolve).  `meta-deepx-m1` itself ships
no LICENSE file.  This public `meta-alp-sdk` layer ships **no DEEPX
code** — it only references DEEPX's own public repos by URL, and a
DEEPX NPU customer fetches them themselves at build time by adding
the layer to their own bblayers.conf.  See
[`docs/vendor-partnerships.md`](../docs/vendor-partnerships.md)'s
DEEPX section for the full licensing detail.

## What's deferred

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
- The DRP-AI3 backend (`PACKAGECONFIG[drpai]`) is auto-enabled by
  `alp-sdk_0.6.bb` on an `rzv2n-family` MACHINE when `ALP_ENABLE_DRPAI` is
  `"1"` and `RUHMI_DRPAI_TVM_DIR` is set;
  `mera2-drpai-tvm_2.7.0.bb`'s `do_compile` and packaging have run in a
  `drpai`-enabled `alp-image-edge` bake (#2400, which found and fixed the
  missing `-lfmt` link gap there).  See
  [`docs/bring-up-drpai-v2n.md`](../docs/bring-up-drpai-v2n.md) section 4
  for exactly what IS established and what is still UNTESTED (everything
  downstream of the bake — including on-silicon inference from a baked
  image; no compiled YOLOX-S/VOC bundle exists yet
  either, since the documented compile path can't calibrate a
  1,3,640,640 detector against real images (RUHMI's 200 calibration
  images ship as 129-byte Git LFS pointer stubs in this checkout, and
  there is no random-frame fallback) -- tracked in alp-sdk#2236).  Its
  nine MERA2/TVM libraries and
  `MeraDrpRuntimeWrapper.h` are packaged by `mera2-drpai-tvm`: eight
  staged verbatim from a builder-supplied RUHMI checkout, nothing
  vendored, plus a ninth (`libmera_drpai_wrapper.so`) that recipe
  COMPILES from that checkout's `apps/MeraDrpRuntimeWrapper.cpp` (RUHMI
  ships no prebuilt for those symbols).  The recipe also `RDEPENDS` on
  meta-rz-drpai's `mmngr-user-module` / `mmngrbuf-user-module` /
  `kernel-module-mmngr` for the two libraries the RUHMI checkout doesn't
  carry.  Treat the whole backend as BENCH-UNVERIFIED.
- `alp-image-edge.bb`'s minimal package set is documentary; the
  v1.0 sysbuild matrix in `docs/test-plan.md` adds the BLE
  provisioning layer + the certificate-pinning post-install hook.

## Verification status

**Partial.** `core-image-minimal` baked on the BSP v6.30 flow (WSL,
2026-05-26): the carrier DT patches apply and the kernel + carrier dtb
+ image build.  A `drpai`-OFF `alp-image-edge` bake has since completed
too — see `docs/bring-up-drpai-v2n.md` for the task count and artefact.
Still pending: a `drpai`-enabled bake, the ROS 2 + DEEPX + Mender
feature set together, and on-bench boot — the v0.7 V2N HiL gate.  The
i.MX 93 path remains unbaked.

DRP-AI3 specifically: **never run on silicon.**  The `&drpai0` overlay,
the `drpai` PACKAGECONFIG and `src/yocto/inference_drpai.cpp` are
code-complete and compile-gated; nothing in this layer has been observed
to probe `/dev/drpai0`, load a model, or run an inference on DRP-AI
hardware.

## See also

- [*RZ/V2N Group Handbook*](https://www.renesas.com/en/document/oth/rzv2n-group-handbook)
  — Renesas's master index of V2N collateral.
- [RZ/V2N product page (AI SDK + BSP downloads)](https://www.renesas.com/en/products/rz-v2n)
  — Software overview + getting-started + how-to-build.
- [`vendors/deepx-dxm1/README.md`](../vendors/deepx-dxm1/README.md)
  — DEEPX DX-M1 integration notes (covers V2M101 / V2M102 / V2M103).
- `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`
  — the orchestrator spec this layer is wired to.
