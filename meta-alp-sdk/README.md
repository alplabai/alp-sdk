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
├── recipes-renesas/
│   └── mera2-drpai-tvm/
│       └── mera2-drpai-tvm_2.7.0.bb     # Stages the MERA2/TVM runtime from a builder-supplied RUHMI checkout.
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

The AEN A32-class MACHINEs (`e1m-aen801-a32`, `e1m-aen701-a32`)
ship the carrier scaffolding today -- they `require` the upstream
Alif `devkit-e8` base and override the carrier specifics; the carrier
DTB + TF-A memory map + full image-bake gate on the maintainer's AEN
HW config (marked `# TBD(alif-hw-config)` in the machine confs).

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
`drp_tvm_rt` — is packaged by `recipes-renesas/mera2-drpai-tvm`, a
staging-only recipe in **this** layer: it fetches and vendors nothing,
it only stages those headers and libraries out of a built RUHMI /
`rzv_drp-ai_tvm` checkout that the builder points it at (see [Model
compilation toolchain (RUHMI)](#model-compilation-toolchain-ruhmi--drp-ai-tvm)
and [Making the RUHMI checkout visible to the
bake](#making-the-ruhmi-checkout-visible-to-the-bake) below).  There is
no NDA gate on any of it (the `rzv_drp-ai_tvm` sources are Apache-2.0),
but the prebuilt MERA2 libraries and the Translator are
Renesas/EdgeCortix account-gated and are not vendored here or anywhere
else in this public repo.

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

# 7b. OPTIONAL: compile the DRP-AI3 NPU backend into libalp_sdk.so.
#     Default OFF.  Only do this once a built RUHMI checkout's headers +
#     libs are STAGED INTO THE RECIPE SYSROOT (ALP_DRPAI_TVM_APPS +
#     CMAKE_LIBRARY_PATH are a plain-CMake-only hint; they do nothing
#     under BitBake -- see "Making the RUHMI checkout visible to the
#     bake" below).  BENCH-UNVERIFIED: never run on DRP-AI silicon.
PACKAGECONFIG:append:pn-alp-sdk = " drpai"

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

### Alif Ensemble E8 — via meta-alif-ensemble

The AEN801 (E8) A32 path rides on Alif's
[`meta-alif-ensemble`](https://github.com/alifsemi/meta-alif-ensemble)
BSP, branch **scarthgap** (matching alp-sdk's Yocto series).  That
layer ships the upstream-complete `devkit-e8` MACHINE (+ `appkit-e8`)
— linux-alif, the TF-A platform, and `devkit-e8.dtb` — which the
`e1m-aen801-a32.conf` carrier `require`s and then overrides.  On the
M55 side the same E8 platform builds on upstream Zephyr's
`ensemble_e8_dk` board, so the heterogeneous E8 stack is
upstream-native top to bottom; alp-sdk only adds the thin carrier
overlay (ADR-0017).  alp-sdk does **not** redistribute or fork the
Alif BSP.

```bash
# 1. Clone the Alif Ensemble BSP (scarthgap) under your own licence:
git clone -b scarthgap https://github.com/alifsemi/meta-alif-ensemble ../meta-alif-ensemble
bitbake-layers add-layer ../meta-alif-ensemble

# 2. Add meta-alp-sdk (if not already) and pick the MACHINE:
MACHINE = "e1m-aen801-a32"
bitbake alp-image-edge
```

The `e1m-aen801-a32.conf` MACHINE ships the carrier scaffolding today;
the carrier DTB, TF-A memory map, and boot-media routing are
maintainer-supplied AEN HW-config inputs and are marked
`# TBD(alif-hw-config)` until that config lands (E8 silicon is also
flagged `status.preliminary` in `metadata/e1m_modules/E1M-AEN801.yaml`).
The `e1m-aen701-a32.conf` (E7) MACHINE follows the same pattern but is
deprioritised both ways — Alp Lab leads with AEN801/E8, and upstream
Alif demotes E7 on scarthgap (only `devkit-e7.conf.orig` remains).

## Per-machine inference runtime

The SDK's `<alp/inference.h>` always compiles in the dispatcher plus the
portable stubs.  For most backends the **vendor NPU runtimes are not
build-time dependencies of the `alp-sdk` library** — the Yocto build
links the dispatcher only, and where a runtime userspace package exists
the **image** recipe installs it (e.g. `alp-image-edge`'s
`IMAGE_INSTALL:append:e1m-v2m101 = "dx-rt"`).  DEEPX DX-M1 keeps that
shape: `ALP_SDK_USE_DEEPX_DXM1` compiles against an in-tree stub header,
so it stays dep-free.

**DRP-AI3 is the exception.**  Its backend
(`src/yocto/inference_drpai.cpp`) is real `MeraDrpRuntimeWrapper` code;
when it is compiled in, the MERA2 / TVM runtime *is* a build-time
dependency of `libalp_sdk.so`, and `<linux/drpai.h>` is a build-time
dependency of the recipe.

| MACHINE              | NPU backend                          | Runtime source                                                        |
|----------------------|--------------------------------------|-----------------------------------------------------------------------|
| `e1m-v2n101-a55`     | DRP-AI3 — opt-in, BENCH-UNVERIFIED   | kernel driver + `<linux/drpai.h>` + `libtvm_runtime.so` from `meta-rz-drpai`; `mera2_runtime` / `mera2_plan_io` / `drp_tvm_rt` + `MeraDrpRuntimeWrapper.h` from a built RUHMI checkout |
| `e1m-v2n102-a55`     | DRP-AI3 — opt-in, BENCH-UNVERIFIED   | Same as V2N101 (memory variant)                                       |
| `e1m-v2m101-a55`     | DRP-AI3 + DEEPX DX-M1                | DRP-AI3 as above; `dx-rt` via the image                               |
| `e1m-v2m102-a55`     | Same as V2M101                       | Same as V2M101 (memory variant)                                       |
| `e1m-nx9101-a55`     | Ethos-U65                            | NXP i.MX 93 Ethos-U userspace via the image                           |
| `e1m-aen801-a32`     | Ethos-U85 + 2x U55                   | Ethos-U path inside the alp-sdk library                               |
| `e1m-aen701-a32`     | 2x Ethos-U55                         | Ethos-U path inside the alp-sdk library                               |

Customer apps still pick the active backend per-handle at runtime via
`alp_inference_open(.backend = ALP_INFERENCE_BACKEND_AUTO)` (or an
explicit `ETHOS_U / DRPAI / DEEPX_DXM1` value for benchmarking) — the
image does not pin one backend.  The one thing decided at build time is
whether the DRP-AI3 backend is *present in the library at all*; when it
is not, a `DRPAI`-requesting `alp_inference_open` returns `NULL` with
`ALP_ERR_NOSUPPORT` (and on a plain V2N, `AUTO` does the same) rather
than silently routing elsewhere.

### Compiling the DRP-AI3 backend in

The compile gate is the CMake option `ALP_SDK_USE_DRPAI_V2N`
(`src/yocto/CMakeLists.txt`, default OFF), paired with a second option,
`ALP_SDK_DRPAI_REQUIRED` (also default OFF), that decides what a missing
runtime does to the configure step.  **Two different ways set
`ALP_SDK_USE_DRPAI_V2N`, and only one of them is live today:**

- **`PACKAGECONFIG[drpai]` in `alp-sdk_0.6.bb` — an explicit opt-in,
  default OFF.**  This is the mechanism that actually works today.
  Enabling it passes `-DALP_SDK_USE_DRPAI_V2N=ON
  -DALP_SDK_DRPAI_REQUIRED=ON` *and* adds `drpai lib-tvm` to `DEPENDS`
  in the same switch, so the recipe can never ask CMake for a backend
  whose header it did not stage:

  ```
  PACKAGECONFIG:append:pn-alp-sdk = " drpai"
  ```

  `drpai` + `lib-tvm` only cover `<linux/drpai.h>` and
  `libtvm_runtime.so`.  `MeraDrpRuntimeWrapper.h` and the TVM header
  tree it hard-includes (`tvm/runtime/profiling.h`, `dlpack/dlpack.h`,
  `dmlc/logging.h`), plus the three libraries `mera2_runtime`,
  `mera2_plan_io`, `drp_tvm_rt`, are in no Yocto layer — see [Making the
  RUHMI checkout visible to the
  bake](#making-the-ruhmi-checkout-visible-to-the-bake) below for what
  actually works to supply them, since the env-var hint this section
  used to recommend does not reach a BitBake configure at all.
- **`scripts/alp_orchestrate/kconfig.py`'s `capabilities.drp_ai`
  auto-emit** produces the same `-DALP_SDK_USE_DRPAI_V2N=ON` in
  `_slice_cmake_args()`, but it does **not** reach this option for any
  Yocto build today: `alp_project.py --emit cmake-args` refuses any
  `--core` whose slice has `os: yocto` (the emit mode only supports
  `baremetal`/`zephyr`), `--emit yocto-conf` — the emit mode a `yocto`
  slice *does* use — never includes a DRP-AI flag, and the one core
  class the auto-emit can reach through `cmake-args`, the M33 Zephyr
  slice, never runs `src/yocto/CMakeLists.txt` at all (it `return()`s
  immediately on `DEFINED ZEPHYR_BASE`).  Treat this path as dormant
  wiring for a future `os: baremetal` DRP-AI slice, not a live A55
  enable path — do not rely on it, and do not cite it to explain why
  `ALP_SDK_DRPAI_REQUIRED` defaults OFF (see next paragraph).

`ALP_SDK_DRPAI_REQUIRED` decides what a missing runtime does to
configure, and it does not default OFF because of the (dormant)
`kconfig.py` path above — it defaults OFF because that is the safe
choice for a builder who passes `-DALP_SDK_USE_DRPAI_V2N=ON` to a
direct plain-CMake configure by hand, outside any recipe: an incomplete
host degrades cleanly instead of hard-failing.  `alp-sdk_0.6.bb`'s
`PACKAGECONFIG[drpai]` overrides it to `ON` in the same switch that sets
`ALP_SDK_USE_DRPAI_V2N=ON`, because there the builder asked for the
backend by name and a silently DRP-AI-less `libalp_sdk.so` would be the
wrong answer.

With `ALP_SDK_USE_DRPAI_V2N=ON`, CMake probes **nine** inputs before
compiling the backend in: the `MeraDrpRuntimeWrapper.h` header, the
`<linux/drpai.h>` UAPI header, the three TVM headers
`tvm/runtime/profiling.h` / `dlpack/dlpack.h` / `dmlc/logging.h`
(the wrapper's transitive include tree), and the four libraries
`mera2_runtime`, `mera2_plan_io`, `drp_tvm_rt`, `tvm_runtime`.  If
**any** of the nine is missing, it names every missing one and either
warns and drops the backend (`ALP_SDK_DRPAI_REQUIRED=OFF` — the direct
plain-CMake case) or fails configure (`ALP_SDK_DRPAI_REQUIRED=ON` — the
recipe's case, so an enabled `PACKAGECONFIG[drpai]` bake can never go
green with the backend silently absent).  On the `OFF` path,
`inference_drpai.cpp` is left out of the build, `ALP_SDK_USE_DRPAI_V2N`
is not defined, and the dispatcher falls back to its other backends —
**read the configure log** on that path rather than assuming the flag
took effect.

Runtime side, `/dev/drpai0` only appears when `meta-rz-drpai` is in
`bblayers.conf` (it provides both the driver and the `drpai0` DT label
that `e1m-v2n-drpai.dtsi` overrides to `status = "okay"`).  Compiling
the backend in without that layer produces a library that opens nothing.

> **BENCH-UNVERIFIED.** The DRP-AI3 backend has never run on silicon and
> no full `alp-image-edge` bake has completed with it enabled.

### Model compilation toolchain (RUHMI / DRP-AI TVM)

Models for DRP-AI compile through Renesas's RUHMI (formerly
DRP-AI TVM) toolchain on the build host — not at image build
time.  It's a separate Apache-2.0 project at
<https://github.com/renesas-rz/rzv_drp-ai_tvm>; model authors
install it on their workstation and ship the compiled output
as a model asset.

A built RUHMI checkout is **also** where the runtime libraries come
from, so it is not purely a model-author concern: `meta-rz-drpai`
supplies only `libtvm_runtime.so`, while `mera2_runtime`,
`mera2_plan_io`, `drp_tvm_rt`, `MeraDrpRuntimeWrapper.h`, and the TVM
header tree it hard-includes (`tvm/include`, `tvm/3rdparty/dlpack`,
`tvm/3rdparty/dmlc-core`) come out of `rzv_drp-ai_tvm` itself
(aarch64 objects under `obj/build_runtime/<soc>/lib`, headers under
`apps/` and `tvm/`).  RZ/V2N uses the **v2h** runtime build;
`obj/build_runtime/v2m` is the older Renesas RZ/V2M SoC and is not this
one.  `tvm/` is a submodule of `rzv_drp-ai_tvm` and ships uninitialised
on a bare clone — run `git submodule update --init --recursive` before
pointing anything at the checkout, or the TVM headers will be missing
even though the checkout "looks" built.

### Making the RUHMI checkout visible to the bake

Any image build that compiles the DRP-AI3 backend in needs that
checkout's headers and libraries where `src/yocto/CMakeLists.txt`'s
`find_path()` / `find_library()` probes can see them — and **the
mechanism that does that differs by build path; do not mix them up:**

- **Plain-CMake (the maintainer header-check, or any direct,
  non-BitBake consumer of this repo's CMake).**  Point
  `ALP_DRPAI_TVM_APPS` at the checkout's `apps/` dir (the probes also
  derive the TVM header tree from `$ALP_DRPAI_TVM_APPS/../tvm/...`) and
  put `obj/build_runtime/<soc>/lib` on `CMAKE_LIBRARY_PATH` before
  configuring.  This works exactly as CMake's plain `HINTS`/
  `CMAKE_LIBRARY_PATH` semantics promise.
- **BitBake (`alp-sdk_0.6.bb`'s `PACKAGECONFIG[drpai]`).**  The
  env-var hints above do **not** work here.  Poky's
  `meta/classes-recipe/cmake.bbclass` sets
  `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY` and
  `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE` to `ONLY`, which restricts every
  `find_path()`/`find_library()` call — including ones with explicit
  `HINTS`/`PATHS` — to paths re-rooted under the recipe's own sysroot
  (`STAGING_DIR_HOST`); a `HINTS` value pointing outside it is silently
  never tried, so `ALP_DRPAI_TVM_APPS` + `CMAKE_LIBRARY_PATH` have no
  effect on a bake and the probes just report the inputs missing.  What
  actually works, and what `recipes-renesas/mera2-drpai-tvm` now does:
  **stage the checkout's headers and libraries into the recipe's own
  sysroot** before `do_configure` runs — `apps/MeraDrpRuntimeWrapper.h`,
  `tvm/include`, `tvm/3rdparty/{dlpack,dmlc-core}/include`, and all
  eight `.so` files under `obj/build_runtime/v2h/lib/`
  (`libmera2_runtime.so`, `libmera2_plan_io.so`, `libdrp_tvm_rt.so`,
  `libdrp_rt.so`, `libacl_rt.so`, `libarm_compute.so`,
  `libarm_compute_core.so`, `libarm_compute_graph.so`;
  `libtvm_runtime.so` is `lib-tvm`'s, separately) installed under
  `${STAGING_INCDIR}` / `${STAGING_LIBDIR}` — since that is the root
  the unmodified probes already search by default. The three libraries
  `src/yocto/CMakeLists.txt` links directly DT_NEED the other five;
  omitting them (an earlier cut of this recipe did) passes configure
  but fails a real `bitbake -c package_qa` with `file-rdeps` errors.
  `libmmngr.so.1` / `libmmngrbuf.so.1`, DT_NEEDED by two of the eight,
  are not RUHMI's to stage at all — they come from meta-rz-drpai's
  `mmngr-user-module` / `mmngrbuf-user-module` recipes, which
  `mera2-drpai-tvm` RDEPENDS on explicitly instead.
  Point the recipe's `RUHMI_DRPAI_TVM_DIR` variable at the checkout
  root (its own SRC_URI is empty; it fetches and vendors nothing, only
  stages) and add it to `alp-sdk_0.6.bb`'s `PACKAGECONFIG[drpai]`
  build deps, which is already done.  Adding the checkout as an extra
  `CMAKE_FIND_ROOT_PATH` entry (via `EXTRA_OECMAKE:append`) is the
  other named escape hatch for this restriction, but only helps if the
  checkout's on-disk layout lines up with what each probe searches for
  once re-rooted; staging is the unambiguous option, and the one this
  note recommends.  Static-checked only, and still not bake-verified —
  no `alp-image-edge` bake has ever completed with `drpai` enabled.

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
  parked behind the `hil-yocto` HIL runner per
  [`docs/ci/HW-IN-LOOP.md`](../docs/ci/HW-IN-LOOP.md).
- The Mender-server side (deployment orchestration, fleet
  monitoring) is out of scope for `meta-alp-sdk`; consumers stand
  up a hosted or self-hosted Mender server independently (per
  the project memory note "OTA server owned by Hakan, separate repo").
- Reference rollout: [`docs/ota.md`](../docs/ota.md).

## Licence

Apache-2.0 (umbrella).  Vendor-licensed components follow their
upstream licences and are flagged as such in the matching recipes'
`LICENSE` field: `dx-rt` is proprietary (DEEPX EULA); the
`rzv_drp-ai_tvm` sources are Apache-2.0 but the prebuilt MERA2
libraries and the Translator are Renesas/EdgeCortix account-gated
(`mera2-drpai-tvm`'s `LICENSE = "CLOSED"` reflects that gap, not an
assertion of a license this recipe could grant).  None of it is
vendored in this repo — `mera2-drpai-tvm` only stages a builder-local
checkout, it fetches nothing.

## What's deferred

- `dx-rt_*.bb` is a skeleton — the DEEPX SDK signed-licence
  acknowledgement closes the legal review per
  [`docs/vendor-partnerships.md`](../docs/vendor-partnerships.md)
  §C.31.
- AEN A32-class MACHINE carrier scaffolding (`e1m-aen801-a32`,
  `e1m-aen701-a32`) ships; the carrier DTB + TF-A memory map + full
  image-bake await the maintainer's AEN HW config (the
  `# TBD(alif-hw-config)` overrides in the machine confs).
- The DRP-AI3 backend (`PACKAGECONFIG[drpai]`) ships OFF and
  BENCH-UNVERIFIED: never run on DRP-AI silicon, never carried through
  a completed `alp-image-edge` bake with `drpai` enabled (a `drpai`-off
  bake of `alp-image-edge` has completed on this host).  Its eight
  MERA2/TVM libraries and `MeraDrpRuntimeWrapper.h` are packaged by
  `mera2-drpai-tvm` (staged from a builder-supplied RUHMI checkout,
  nothing vendored), which also `RDEPENDS` on meta-rz-drpai's
  `mmngr-user-module` / `mmngrbuf-user-module` / `kernel-module-mmngr`
  for the two libraries the RUHMI checkout doesn't carry. A
  `do_package_qa` run against this PACKAGECONFIG did fail once, on
  three libraries the recipe originally staged (missing five more
  DT_NEEDED libraries and both mmngr RDEPENDS, plus landing what it did
  stage in the wrong package); that fix is code-complete but a green
  `drpai`-enabled bake through it is still open work.
- `alp-image-edge.bb`'s minimal package set is documentary; the
  v1.0 sysbuild matrix in `docs/test-plan.md` adds the BLE
  provisioning layer + the certificate-pinning post-install hook.

## Verification status

**Partial.** `core-image-minimal` baked on the BSP v6.30 flow (WSL,
2026-05-26): the carrier DT patches apply and the kernel + carrier dtb
+ image build.  Still pending: a full `alp-image-edge` bake (ROS 2 +
DEEPX + Mender recipes) and on-bench boot — the v0.7 V2N HiL gate.  **No
full `alp-image-edge` bake has ever completed on any host.**  The
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
  — DEEPX DX-M1 integration notes (covers V2M101 / V2M102).
- `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`
  — the orchestrator spec this layer is wired to.
