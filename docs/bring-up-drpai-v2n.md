# DRP-AI3 bring-up on E1M-X V2N

How to get the RZ/V2N's on-die DRP-AI3 NPU running a real model through
`<alp/inference.h>` on an E1M-X V2N SoM.

> **Status: INFERENCE RUNS ON SILICON; THE BAKED IMAGE IS NOT YET CONFIRMED.**
> On 2026-09-28 an E1M-V2M103 (board #1, Alp SDK 0.7.0 image) ran YOLOX-S/VOC
> through `<alp/inference.h>` on the DRP-AI3 (#1268):
> - The `&drpai0` override was applied by hand to the board DTB, and
>   `drpai-rz 17000000.drpai` probed with the `0xD0000000` arena.
> - `src/yocto/inference_drpai.cpp` was cross-built against the real MERA2
>   runtime (RUHMI 2.7.0-hotfix2; `libmera_drpai_wrapper.so` compiled from
>   `apps/MeraDrpRuntimeWrapper.cpp`).
> - `alp_inference_open()` works under both `DRPAI` and `AUTO`, and `invoke()`
>   takes ~40 ms per 640x640 frame.
> - The outputs match the compiler's interpreter reference (correlation
>   0.996-0.998).
> - With a bundle calibrated on 100 VOC images, a real aeroplane photo is
>   detected as aeroplane (0.519, against 0.525 from ONNX Runtime CPU on the
>   same input).
>
> Not yet confirmed: an `alp-image-edge` baked from this tree producing the
> same result on its own. That needs the defaults in §4 (node on with
> meta-rz-drpai, backend on with `RUHMI_DRPAI_TVM_DIR`) and a real
> `mera2-drpai-tvm` BitBake run, which has not happened. `docs/test-plan.md`
> carries the verification rows this gates.

For the base V2N board bring-up see [bring-up-v2n.md](bring-up-v2n.md); for the
DEEPX DX-M1 delta on V2N-M1 see [bring-up-v2n-m1.md](bring-up-v2n-m1.md).
DRP-AI3 is on-die in the RZ/V2N SoC, so it is present on **every** V2N-family
SKU including V2M — DEEPX is an addition, not a replacement.

## 1. What DRP-AI needs, and where each piece comes from

Five inputs. They come from four different places, which is the main reason
this is fiddly.

| Input | Source | Notes |
| --- | --- | --- |
| DRP-AI kernel driver | `meta-rz-drpai`, patched into the kernel by `0002-enable-drpai-driver.patch` | Not a package — do not look for a `.ko` |
| `drpai0` DT node + label | `meta-rz-drpai`, `0001-add-drpai-property-to-devicetree.patch` | **Creates** the label; it does not exist in the pristine tree |
| `<linux/drpai.h>` UAPI header | `meta-rz-drpai` recipe `drpai` (1.4.0) | Headers only |
| `libtvm_runtime.so` | `meta-rz-drpai` recipe `lib-tvm` | No longer installed explicitly (it served a legacy TVM v2.5 path), but `libalp_sdk.so` still links `tvm_runtime`, so OE's shlibs pass pulls `lib-tvm` back in until the link fix on `fix/v2n-audit-yocto-sdk` lands |
| The MERA2 runtime closure: headers + **nine** staged libraries (a tenth, `libtvm_runtime.so`, comes from `lib-tvm` above) | `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb`, staged/compiled from a builder-supplied **`RUHMI_DRPAI_TVM_DIR`** checkout | The recipe vendors nothing — see §4. Note its `LICENSE = "CLOSED"`: the `rzv_drp-ai_tvm` **sources** are Apache-2.0, but the prebuilt MERA2 libraries staged alongside them are account-gated, so the package as a whole is not redistributable. Tracked as a licence-manifest gap. |

Baseline this was worked against: **AI SDK platform 7.1 on BSP v6.30**
(`RTK0EF0189F06300SJ`, linux-renesas `6.1.141-cip43`).

### The account-gated piece

Compiling a model additionally requires the **DRP-AI Translator**, which is a
separate download from the My Renesas portal and needs an account:

- RZ/V2H and RZ/V2N: `DRP-AI_Translator_i8` **v1.11 or later**
  (`DRP-AI_Translator_i8-v1.11-Linux-x86_64-Install`)
- <https://www.renesas.com/software-tool/drp-ai-translator-i8>

`tutorials/compile_onnx_model_quant.py` shells out to it; nothing in this repo
and no upstream build substitutes for it.

## 2. Host toolchain (RUHMI)

Clone and build `renesas-rz/rzv_drp-ai_tvm`, then export:

```sh
export ALP_DRPAI_TVM_HOME=<rzv_drp-ai_tvm checkout>
export ALP_DRPAI_TVM_APPS=$ALP_DRPAI_TVM_HOME/apps
```

Five things bite here:

- **Initialise the submodules.** A checkout can have its runtime libraries
  already built while `tvm/` is still empty. `MeraDrpRuntimeWrapper.h` hard-includes
  `<tvm/runtime/profiling.h>`, which lives in the `tvm` submodule, so without
  `git submodule update --init --recursive` any compile against the wrapper dies
  with `fatal error: tvm/runtime/profiling.h: No such file or directory`.
  `meta-rz-drpai`'s `lib-tvm` does not help — it ships `libtvm_runtime.so*` and a
  LICENSE, no headers.
- **Use the nested dlpack, not the top-level one.** The checkout carries two
  copies: the top-level `3rdparty/dlpack` (pinned v0.5) and the one the `tvm`
  submodule brings in, `tvm/3rdparty/dlpack`. Building against the top-level
  copy fails with `error: 'kDLCUDAManaged' was not declared in this scope`;
  the include path must point at the nested one.
- **Host TVM is not built either.** A fresh checkout has no `libtvm*.so*`
  anywhere — `import tvm` fails in `tvm/_ffi/libinfo.py:146 find_lib_path()`
  until it's built, which needs `llvm-14`.
- **RZ/V2N uses the V2H build.** The runtime libraries are
  `obj/build_runtime/v2h/lib/`, and the model compile takes `PRODUCT=V2N`
  (upstream `README.md` pairs "RZ/V2H and RZ/V2N" throughout;
  `scripts/alp_model/adapters/drpai.py` defaults to `PRODUCT=V2N`).
- **`obj/build_runtime/v2m/` is NOT ours.** That is Renesas **RZ/V2M**, an older,
  different SoC. It is unrelated to the E1M-V2M SKU, which is RZ/V2N + DEEPX and
  also uses the **v2h** libraries. Linking `v2m` would be the wrong silicon's NPU
  runtime.

Upstream states Ubuntu 22.04 / Python 3.10; a newer host may need a pinned venv.

## 3. Device tree — the node must be enabled

The carve-outs are declared in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`:

```
drp_reserved:        drp-ai@d0000000    reg = <0x0 0xd0000000 0x0 0x20000000>   /* 512 MiB */
shared_drp_reserved: shareddrp@afcff000 reg = <0x0 0xafcff000 0x0 0x00001000>   /* 4 KiB   */
```

Declaring them is **not sufficient** — something has to claim them.
`e1m-v2n-drpai.dtsi` carries the override, and the kernel bbappend installs it
when `meta-rz-drpai` is in `bblayers.conf` and `ALP_ENABLE_DRPAI` is `"1"`.
`ALP_ENABLE_DRPAI` **defaults to `"1"` whenever that layer is present**: it is
declared in all six V2N/V2M machine confs as on-with-the-layer, because every
V2N/V2M SKU carries the same on-die DRP-AI3.  An earlier revision defaulted it
to `"0"`, and shipped images then carried the driver, the arena and the
vendor runtime but no `/dev/drpai0`.  Set `ALP_ENABLE_DRPAI = "0"` in
`local.conf` to opt out; without the layer, or with the opt-out, the build
installs a comment-only stub and the node stays `disabled`.

The layer half of the gate exists because that layer creates the `drpai0`
label: referencing it without the layer fails in dtc, and the same SoM dtsi
is included by the V2M board dts, so it would take that dtb down too.

**Silicon confirms the node is not on by default.** The V2N bench unit's current
dtb, `/boot/r9a09g056n44-dev.dtb`, carries **zero** `drpai` nodes — the
enablement on that board comes from a different, already-loaded
`/boot/uio-683.dtb`, not from anything this repo builds. Our own dtb,
`e1m-v2n101-x-evk.dtb`, carries the node enabled whenever `ALP_ENABLE_DRPAI`
resolves to `"1"` for that build, which is now the default with
`meta-rz-drpai` present.  Images built before that default change carry
the stub, and the node stays `disabled` (E1M-V2M103 board #1 on the Alp SDK
0.7.0 image, 2026-09-28: `/soc/drpai@16800000` `status = "disabled"`, no
`/dev/drpai0`, although `CONFIG_DRPAI=y` and the `drp-ai@d0000000` arena are
present).  The overlay is what makes it present,
never the SoC by default. Separately: the **kernel** half of the stack is
already proven working on this silicon — `/dev/drpai0` exists on that board's
current image and the driver probes clean (`drpai-rz 17000000.drpai: DRP-AI
Driver version : 1.40 rel.3 V2N`, correct memory-region prints, zero errors).
The `17000000` there is the device name Linux derives from the node's FIRST
`reg` entry; the vendor names the node itself `drpai@16800000` after its
second. Both are right — see the note in `e1m-v2n-drpai.dtsi`.
What's missing there is the userspace (`ls /usr/lib/libdrpai*` finds nothing
on that board) — the gap this branch's packaging (§4) closes.

Both memory properties are mandatory. On V2N the driver defines
`ENABLE_DRP_SUPPORT_SHARED_MEMORY`, so a probe without
`memory-shared-for-drpai-ext-cont` hard-fails with `-ENOMEM` rather than
degrading to a single-region mode.

**Never point `memory-region` at `mmp_reserved` (`0x80000000`).** That is the
mmngr video buffer pool — now measured, not just reasoned: on
the V2N bench unit, `rgnmm_drv mmngr: assigned reserved memory node
linux,multimedia` reports that node's `reg` as base `0x80000000` size
`0x10000000`, exactly the deleted `kDrpAiMemStart` constant this driver used
to hard-code. The NPU DMAs against the DRP-AI base directly, so pointing it
at the mmngr pool instead corrupts the video pipeline silently rather than
failing.

The runtime does not hard-code the base: `_drpai_mem_start()` in
`src/yocto/inference_drpai.cpp` asks the driver via `DRPAI_GET_DRPAI_AREA` on a
fresh `/dev/drpai0` fd. A fresh fd is deliberate — the region cursor is per-fd
state and alternates once a second region exists — but it is not free:
`drpai_open()` takes a 1000 ms `down_timeout()`, and the matching `close()`
resets the DRP-AI when it is the sole opener. It runs once at open, never per
inference.

**Fresh-fd is confirmed safe here, but not proven load-bearing.**
The V2N bench unit has only one DRP-AI region, and two `DRPAI_GET_DRPAI_AREA`
calls on the *same* fd returned identical values on that board — but with a
single region a per-fd alternating cursor and no cursor at all look
identical from the outside. Whether the fresh-fd-per-call approach is
load-bearing on a two-region config remains unresolved; treat it as a safe
no-op on hardware seen so far, not as validated for that case.

## 4. Image

**The two-switch contract, authoritative here — every other mention in this
repo is a pointer to this paragraph, not a restatement of it.** Two
independent switches, both default OFF, deliberately not merged into one
(the released v0.15.0 contract, `CHANGELOG.md`: "Two independent switches,
both default OFF, deliberately not merged into one"):

- **`PACKAGECONFIG[drpai]`** on the `alp-sdk` recipe compiles the DRP-AI3
  backend into `libalp_sdk`.  It turns on by itself on an `rzv2n-family`
  MACHINE when `ALP_ENABLE_DRPAI` is `"1"` **and** `RUHMI_DRPAI_TVM_DIR`
  points at a RUHMI checkout (the MERA2 runtime cannot be built without
  one, so without it the backend stays off rather than failing the bake).
  `PACKAGECONFIG:append:pn-alp-sdk = " drpai"` in `local.conf` forces it.
- **`ALP_ENABLE_DRPAI`** (a MACHINE-conf variable, default `"1"` whenever
  `meta-rz-drpai` is present) does two things:
  enables the `&drpai0` devicetree node (§3), and — `alp-image-edge`
  only, and only on an `rzv2n-family` MACHINE (`'rzv2n-family' in
  (d.getVar('MACHINEOVERRIDES') or '').split(':')`) — gates installing
  `alp-drpai-inference` (only when `RUHMI_DRPAI_TVM_DIR` is set too, so
  the demo never lands without its backend); never
  `alp-image-prod`, and never on a non-RZ/V2N machine such as
  `e1m-nx9101-a55` or `e1m-aen801-a32` even with `ALP_ENABLE_DRPAI = "1"`
  set. It installs no userspace runtime package itself. The
  "opted in without `meta-rz-drpai`" `bb.fatal` guard lives only in
  `alp-image-edge.bb`; `alp-image-prod` has none, so a prod build with
  `ALP_ENABLE_DRPAI = "1"` and no `meta-rz-drpai` silently gets the
  comment-only `&drpai0` stub.
  `alp-image-common.inc`'s `ALP_RZ_DRPAI_INSTALL` is the
  single packaging authority for `kernel-module-mmngr` (`lib-tvm` is
  no longer installed explicitly), on every `alp-image-*` image (the three recipes that `require
  alp-image-common.inc` — `alp-image-base`/`-edge`/`-prod`), gated only
  on `rz-drpai` being in `BBFILE_COLLECTIONS` and `v2n` being in
  `MACHINE_FEATURES` (issue #1176), independent of `ALP_ENABLE_DRPAI`. A
  non-`alp-image-*` build (a bare `core-image-*`) with
  `ALP_ENABLE_DRPAI = "1"` does NOT get that package from alp-sdk's tree at
  all — it would need its own install, or the vendor layer's own
  `core-image` bbappend (no such bbappend exists in this tree).

**Both are required and neither implies the other.** Omitting the
`PACKAGECONFIG` half leaves `ALP_SDK_USE_DRPAI_V2N=OFF`, and
`alp_inference_open()` returns `NULL` with `ALP_ERR_NOSUPPORT` even though
the node and userspace payload are present. Omitting `ALP_ENABLE_DRPAI`
also fails, not idles: with a compiled backend but no `&drpai0` node,
`/dev/drpai0` does not exist, so `open()`'s `ENOENT` collapses to
`ALP_ERR_IO` (`src/yocto/inference_drpai.cpp`'s `_drpai_mem_start()`,
called first from `alp_inference_drpai_open()`, via
`_drpai_errno_to_status()`'s default case). A driver that IS present
but contended returns a different code from that same mapping instead
— `ALP_ERR_TIMEOUT` for `ETIMEDOUT`, `ALP_ERR_BUSY` for `EINPROGRESS`/
`EADDRNOTAVAIL` — so absent (`ALP_ERR_IO`) and busy
(`ALP_ERR_TIMEOUT`/`ALP_ERR_BUSY`) are distinguishable, not the same
code path. Neither switch silently half-works, and neither missing
switch is silent either.

Enable the backend through the SDK recipe's PACKAGECONFIG:

```
PACKAGECONFIG:append:pn-alp-sdk = " drpai"
```

That switch (whose DEPENDS names `mera2-drpai-tvm`, `drpai` and `lib-tvm`)
flips `-DALP_SDK_USE_DRPAI_V2N=ON` and
`-DALP_SDK_DRPAI_REQUIRED=ON`, and adds the `drpai` and `lib-tvm` build deps
together.

**The RUHMI libraries and wrapper header are now packaged**, closing the gap
the earlier revision of this doc left as a manual staging step.
`meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb` stages
headers plus the closure — **nine** libraries, not three: eight copied verbatim
out of a builder-supplied, already-built `rzv_drp-ai_tvm` checkout's
`obj/build_runtime/v2h/lib` (libmera2_runtime, libmera2_plan_io, libdrp_tvm_rt,
libdrp_rt, libacl_rt, libarm_compute, libarm_compute_core,
libarm_compute_graph), and a ninth the recipe **compiles itself**,
`libmera_drpai_wrapper.so`, from the checkout's `apps/MeraDrpRuntimeWrapper.cpp`
— that class ships as application-side glue source with no prebuilt library at
all, so the recipe compiles it once rather than leaving every consumer to
duplicate the vendor glue. It also `RDEPENDS` on mmngr-user-module /
mmngrbuf-user-module for `libmmngr.so.1` / `libmmngrbuf.so.1`. The recipe
fetches and vendors nothing: point the single variable **`RUHMI_DRPAI_TVM_DIR`**
(in `local.conf` or the environment) at a built checkout before enabling
`drpai` — an unset or incomplete checkout fails `do_compile`/`do_install`
loudly, naming the exact missing path.

**Checked at the symbol level, against the compiled objects rather than a
completed link:** with `RUHMI_DRPAI_TVM_DIR` pointed at a real
checkout, the previously-undefined `MeraDrpRuntimeWrapper::*` symbols alp-sdk
needs all match what the compiled wrapper exports — 26 symbols exported, 9
referenced by alp-sdk, all 9 match, 0 unresolved. **Not yet verified:** the
compile command later encoded in the recipe's `do_compile` was run only by
hand on an x86_64 dev host against a real RUHMI checkout's headers (system
spdlog/asio standing in for meta-oe's). That host-side probe compiled the
wrapper source, but could not link against the real aarch64
`obj/build_runtime/v2h` libraries; no
`bitbake` run of this recipe — with or without `do_compile` — has happened at
all. A full `alp-image-edge` bake has completed on this host (12118 tasks,
producing a 716 MB `.wic.gz`, the first ever here) but with `drpai` OFF (the
base image); a `drpai`-enabled bake on the real aarch64 Yocto cross-toolchain
is the step that would confirm the link and packaging end to end.

**`meta-rz-drpai` on `bblayers.conf` is necessary but not sufficient for the
image.** That layer ships its payload through a `core-image-%.bbappend`, and
that wildcard does not match `alp-image-edge`, so the bbappend never fires and
the image comes out with no DRP-AI userspace at all — silently.
`alp-image-common.inc` therefore installs `kernel-module-mmngr`
explicitly, gated on the layer being present. See issue #1176; the same trap
applies to the other `meta-rz-*` feature layers.

### Access control

`/dev/drpai0` is `0660 root:drpai` (udev rule in `alp-drpai-udev`, which also
creates the `drpai` system group). The recipe is pulled in by alp-sdk's
`PACKAGECONFIG[drpai]`, so the rule and the group exist only in images that
carry the SDK DRP-AI backend. `/run/alp` (the one-process-per-board lock) uses
the same group.

Nothing in the layer adds a user to `drpai`. A product's app user must opt in,
e.g. `EXTRA_USERS_PARAMS += "usermod -a -G drpai <user>;"` in the image or
`local.conf`; before this change `video` membership was enough.

- **Register ioctls are privileged.** Kernel patch
  `0018-drpai-require-CAP_SYS_RAWIO-for-the-register-ioctls.patch` makes the
  vendor driver's ioctls 64-69 (`DRPAI_READ/WRITE_DRP_REG`,
  `DRPAI_READ/WRITE_DRPAI_REG`, `DRPAI_READ/WRITE_CPG_REG`) return `-EPERM`
  without `CAP_SYS_RAWIO`. The runtime does not use them (confirm with the
  strace step below). It is installed only with `meta-rz-drpai`, which adds
  `drivers/drpai/`.
- **Residual risk: DMA.** The patch does not bound the descriptors passed to
  `DRPAI_ASSIGN` / `DRPAI_START`, so a process that can open the node can still
  make the NPU DMA to or from any physical address. Treat membership of
  `drpai` as a privileged grant (root-equivalent for memory), not as an
  ordinary device group.

## DRP1 (OpenCVA + codec)

`e1m-v2n-drp1.dtsi` enables `&drp1` (`memory-region = <&drp_codec>`,
`memory-oca-region = <&opencva_reserved>`,
`memory-shared-for-drpai-ext-cont = <&shared_drp_reserved>`), the same three
properties as the vendor EVK. The `drp1` label is created by
`meta-rz-opencva` / `meta-rz-codecs`, so the bbappend installs the real file
only when one of them is in `bblayers.conf` and a comment-only stub otherwise.
`drp1` and `drpai0` share the `0x17000000` register window, as on the vendor EVK.
The vendor OpenCVA U-Boot change targets `rzv2n-evk.h`, not the
`rzv2n-dev_defconfig` this build uses, so it may not take effect here; bench
step 5 below is the gate.

### Bench steps (not yet run)

1. Run an inference under `strace -f -e trace=ioctl` as root and confirm no
   ioctl with request number 64-69 on `/dev/drpai0` (`DRPAI_*_REG`).
2. As a user outside the `drpai` group, `open("/dev/drpai0")` must fail with
   `EACCES`; as a `drpai` member an inference must succeed.
3. As a `drpai` member without `CAP_SYS_RAWIO`, issue ioctl 64: expect `EPERM`.
4. `ls /dev/drp*` / `dmesg | grep -i drp`: the DRP1 device is present on an
   image with `meta-rz-opencva` or `meta-rz-codecs`.
5. Run an OpenCV `cv::resize` through OpenCVA and confirm it executes on the DRP.

## 5. Model compile

```sh
export ALP_DRPAI_TVM_HOME=<rzv_drp-ai_tvm checkout>
tan model build --sdk-root <alp-sdk> --board <path>/board.yaml
```

`tan` is a standalone binary — it must resolve an alp-sdk checkout before it
can compile anything, and `--sdk-root` is how you bind one explicitly rather
than relying on discovery (cwd-inside-the-checkout, a project pin, or a
global default) picking the right one.

`tan` is the whole command surface (ADR-0020 end-state B); `scripts/alp_cli`'s
former `model` command (and the rest of its command-line wrappers) retired
once `tan model` shipped a native port (alp-sdk#1368). There is no `alp`
console script, and no `python -m alp_cli <verb>` front door either any
more.

There is no `--target`/`--product` flag and no positional `<model.onnx>`
argument. `tan model build` compiles every `models:` entry declared in
`board.yaml` for every backend the SoM resolves to; `PRODUCT` for DRP-AI
comes from `models[].compile.drpai.product` (falling back to
`accel_config`, then `"V2N"`), not a CLI flag.

**`board.yaml`'s schema does not describe this config yet — use
`tan model build` above anyway; it does not run schema validation.**
`metadata/schemas/board.schema.json`'s `models[].compile.drpai` block only
declares a `spec:` key (`additionalProperties: false`, `required: ["spec"]`)
— a leftover from a design where an external spec file carried the model
geometry. `scripts/alp_model/adapters/drpai.py` never reads `spec`; it reads
`input_shape`, `input_name`, `images` and `product` straight out of the
`compile.drpai` block, so `tan validate` rejects a `board.yaml` written this
way. That does not block the command in step 5 above: `tan model build`
reads `board.yaml` with a plain `yaml.safe_load` and never calls the schema
validator itself — only the separate `tan validate` command does — so
`compile.drpai.input_shape` / `input_name` / `images` / `product` reach the
adapter unchanged through the documented CLI today. Until the schema is
reconciled with what the adapter actually reads, `tan validate` cannot be
used against a `board.yaml` with a `compile.drpai` block; `tan model build`
can.

`scripts/alp_model/adapters/drpai.py` drives
`$ALP_DRPAI_TVM_HOME/tutorials/compile_onnx_model_quant.py` with `PRODUCT` in the
environment. It needs an input shape and name, and calibration images, and
always forwards the images through the tutorial's `--images` flag.

**The `--images` calibration path only works for 224x224 ImageNet-style
classifiers.** The tutorial's `--images` handling always runs each calibration
image through `pre_process_imagenet_pytorch()`, which ignores the `dims`
argument it accepts and hard-codes `resize(256)` + `center_crop(224)`
regardless of the model's declared geometry. For any other input shape —
including every object detector, e.g. YOLOX at `1,3,640,640` — the adapter now
rejects the compile up front with a clear error instead of running the
(multi-minute) DRP-AI Translator only to abort deep inside the vendor tutorial
with a shape-broadcast error. There is no random-frame (`-n`) fallback wired
into the adapter: the tutorial's own `-n` path compiles and runs but leaves
post-training INT8 quantisation calibrated against noise rather than real
data, so it is not something to route detectors through silently. Owning the
calibration feed instead of delegating to the tutorial's classifier-shaped
helper — so a detector's real preprocessing (e.g. YOLOX letterbox padding)
matches what the on-device DRP preprocessing chain does — needs a real
calibration image set and a board to validate the result's accuracy; that is
tracked in alp-sdk#1271 and not done here.

### Detector bundle: the route that works today (YOLOX-S/VOC, #2236)

Until #1271 lands, compile a detector by hand with RUHMI's own tutorial,
patched exactly as `how-to/sample_app_v2h/app_yolox_cam/README.md` says.
The result below is the one proven on silicon: E1M-V2M103 board #1,
2026-09-28 (#1268). A VOC aeroplane photo came back as aeroplane at 0.519,
against 0.525 from ONNX Runtime CPU on the same input.

```sh
cd <rzv_drp-ai_tvm>/tutorials            # work on a copy if you keep the checkout clean
sed -i -e 's/256/640/g' -e 's/ 224/ 640/g' -e 's/to_tensor/pil_to_tensor/g'        -e '/std = stdev/d' -e '/F.normalize/d' -e 's/FORMAT.BGR/FORMAT.YUYV_422/g'        -e '/cof_add/d' -e '/cof_mul/d' -e 's/480, 640, 3/1920, 1920, 2/g'        compile_onnx_model_quant.py
export PATH=<drp-ai venv>/bin:$PATH      # the quantizer shells out to a bare `python3`
export LD_LIBRARY_PATH=<dir holding libLLVM-14.so.1>
export PRODUCT=V2N TVM_ROOT=<rzv_drp-ai_tvm> SDK=<RZ/V SDK>
export TRANSLATOR=<DRP-AI_Translator_i8>/translator/ QUANTIZER=<DRP-AI_Translator_i8>/drpAI_Quantizer/
python3 compile_onnx_model_quant.py <DRP-AI_Translator_i8>/onnx_models/YoloX-S_VOC_sparse70.onnx     -o yolox-s-voc -t $SDK -d $TRANSLATOR -c $QUANTIZER -s 1,3,640,640     --images <dir of ~100 real VOC JPEGs>
tar cf yolox-s-voc.tar -C yolox-s-voc --exclude=interpreter_out --exclude=input_0.bin .
```

Three things each broke a real run:
- **Calibration data.** Without `--images` the tutorial calibrates on random
  frames. That bundle runs fine but detects nothing: a flat ~0.32 on every
  anchor.
- **The quantizer's Python.** It spawns a bare `python3`. If that resolves to
  anything but the venv's (3.10 here), it fails with
  `onnx_optimizer.so: undefined symbol: _PyUnicode_Ready`.
- **Environment.** `PRODUCT` and LLVM 14 must be set, or the script exits
  before compiling.

The input the bundle expects is the `app_yolox_cam` preprocessing
`examples/v2n/v2n-drpai-inference/README.md` documents: an RGB-114 letterbox
with the image at the top, bilinear to 640, raw 0-255, NCHW float32.

**RUHMI ships the ONNX source, not a compiled bundle.** A fresh checkout has
`how-to/sample_app_v2h/app_yolox_cam/yolox-S_VOC.onnx` (35 MB, YOLOX-S on
VOC), but no pre-compiled `drpai_dir` output. A search for `drp_desc.bin`,
`weight.bin`, `addr_map.txt` and `deploy.json` finds none. Every bundle comes
from running the compile above yourself, as the #2236 detector bundle did. `tutorials/README.md` documents the alternative
public source instead: `wget` a public ONNX
(`resnet18-v1-7.onnx` from the `onnx/models` repo) and run
`compile_onnx_model.py` against it. Either way, compiling still requires the
account-gated DRP-AI Translator (§1) —
`tutorials/compile_onnx_model_quant.py:314` shells out to it via
`opts["drp_compiler_dir"]` / `drp_compiler_version`, not optionally.

The output is a **directory tar**, not a flat buffer — `blob_format` is
`drpai_dir`, containing `drp_desc.bin`, `weight.bin`, `addr_map.txt`,
`deploy.json`, `deploy.so` and `preprocess/`. `alp_inference_open()` extracts it
to a private temporary directory before calling `LoadModel()`.

## 6. Deploy

With the EVK strapped for **xSPI boot**, BL2 and the FIP come from `mtd0`/`mtd1`
and the microSD supplies only the kernel and rootfs — so this path never writes
xSPI, and cannot brick the boot chain.

Write the `.wic.gz` to a microSD card and insert it. The patched U-Boot tries
microSD first (`if mmc dev 1`) and falls back to eMMC. microSD is also, in
practice, the *only* deployment path right now: the bench board has no IP —
`end0` is DOWN and `end1` shows NO-CARRIER (errata E1 plus the switch's
Auto-MDIX behaviour; see [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md)) — so
serial at 115200 is the only channel and there is no `scp` route for an image
this size.

Two things had to be fixed for a self-built image to boot this way. Both are
issue #1175, and **the fix is not part of this change** — it lives on `dev`
already, via the `CONFIG_BOOTCOMMAND` override in
`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0002-rzv2n-dev-ALP-E1M-production-boot.patch`
(#1186):

- The vendor env loads `boot/r9a09g056n44-dev.dtb`, a filename no ALP image
  builds, on **both** the SD and eMMC paths. `CONFIG_BOOTCOMMAND` re-loads the
  correct dtb after the leading `env default -a` on both branches.
- The microSD root device was wrong. **Confirmed on hardware:** `mmcblk2`
  does not exist on this silicon at all — the board has exactly two SDHI
  controllers, `15c00000.mmc` -> `mmc0` -> eMMC (with `boot0`/`boot1`/`rpmb`
  partitions) and `15c10000.mmc` -> `mmc1` -> the SDHC slot. So eMMC is always
  `mmcblk0` and microSD is always `mmcblk1`; the vendor env's
  `alp_root=/dev/mmcblk2p2` names a device that cannot exist.

> **Neither fix has booted a board.** #1175 is closed and `dev` carries the fix
> (#1182, with the eMMC half completed in #1186), but it was settled by
> inspection of the patch, not by a boot. The second, independent
> implementation once carried on `feat/1145-drpai-v2n-bringup` -- per-MACHINE
> Kconfig strings for the dtb filename and the SD root -- was deliberately
> **not** landed: two mechanisms both setting the boot dtb is worse than
> either (reasons recorded on #1175 and #1238). Its one advantage, the V2M
> gap, has since been closed in-tree by a different mechanism --
> `CONFIG_ALP_E1M_FDTFILE`, defaulting to the V2N basename and overridden per
> MACHINE by `meta-alp-sdk/recipes-bsp/u-boot/u-boot/fdtfile-v2m.cfg` (#1252,
> itself bench-gated). Treat that branch as recoverable history, not a live
> option.

> **Operational trap.** The manual FIP flow has no `merge_config.sh` step, so it
> builds from the Kconfig defaults — the vendor values — and will boot the
> wrong dtb. Build the FIP through the Yocto path, or check the resulting
> `.config` before flashing. Recoverable by reflashing a known-good FIP, but it
> costs a bench session.

> **Flash-plan warning.** Don't assume the dtb your image built is the one a
> board will actually boot. On
> the V2N bench unit, the running kernel's `bootargs` carry
> `uio_pdrv_genirq.of_id=generic-uio` — a string that appears nowhere in the
> `bootcmd` currently stored in `mtd1`. That means the live kernel/dtb/cmdline
> did **not** come from that stored bootcmd, and reading `mtd1` alone cannot
> tell you what will actually load on the next power cycle. Establishing the
> real load path needs catching the U-Boot prompt over serial (i.e. a reboot)
> before trusting any flash plan built from the stored env.

## 7. Verify on the board

In order:

1. `ls /dev/drpai0` — absent means one of three things, in the order worth
   checking: `meta-rz-drpai` was not in `bblayers.conf`; the build set
   `ALP_ENABLE_DRPAI = "0"`; the image predates the default-on change; or the
   DT override otherwise did not land. Nothing else will work. (This
   node already exists on the V2N bench unit's current, non-ALP-built image, so
   its presence alone doesn't prove *this* image's DT override worked — check
   the dtb in use, per §3.)
2. `dmesg | grep -i drpai` — a probe failing `-ENOMEM` means
   `memory-shared-for-drpai-ext-cont` is missing. Confirmed good on
   the V2N bench unit: `drpai-rz 17000000.drpai: DRP-AI Driver version : 1.40
   rel.3 V2N`, correct region prints, zero errors.
3. Confirm the memory-base ioctl resolves to the DT region, not to
   `mmp_reserved` (§3). Needs only `python3`:
   ```python
   import fcntl, struct
   DRPAI_GET_DRPAI_AREA = 0x80102e0b  # _IOR(46, 11, drpai_data_t): two uint64
   with open("/dev/drpai0", "rb") as f:
       buf = bytearray(16)
       fcntl.ioctl(f, DRPAI_GET_DRPAI_AREA, buf)
       addr, size = struct.unpack("QQ", buf)
       print(f"ADDR=0x{addr:016x} SIZE=0x{size:016x}")
   ```
   On the V2N bench unit this returns `ADDR=0x00000000d0000000
   SIZE=0x0000000020000000`, matching the driver's own boot print, the DT
   `reg`, and `/proc/iomem` (`d0000000-efffffff : reserved`).
4. `ls /usr/lib/libmera2_runtime.so*` (and `ls /usr/lib/libtvm_runtime.so*`,
   present only while `libalp_sdk.so` still links it) —
   absent means the image did not get the vendor payload (§4); on
   the V2N bench unit's current image neither exists yet (`ls
   /usr/lib/libdrpai*` also finds nothing) — that userspace gap is what this
   branch's packaging is meant to close, once run through a `drpai`-enabled
   bake (§4).
5. Run the model. `alp_inference_open(.backend = ALP_INFERENCE_BACKEND_DRPAI)`
   returning `NULL` with `ALP_ERR_NOSUPPORT` means the backend was not compiled
   in; `ALP_ERR_TIMEOUT` means the driver semaphore expired; `ALP_ERR_BUSY` means
   the shared-memory exclusion lock is contended. **Not yet reachable: no model
   has been compiled (§5).**

## 8. Running more than one model

DRP-AI3 is **time-shared**: the hardware runs one job at a time and the
driver has no queue (a second `DRPAI_START` while a job runs returns
`-EBUSY`). What the SDK does about it, in one process:

- **Each open handle gets its own range of the arena.** Without that,
  every handle loaded at the arena base (`0xd0000000`) and the second
  model silently overwrote the first. The SDK now places each model after
  the highest live one, using the runtime's `GetLastAddress()` (the
  absolute end address of the model just loaded; `0` for a CPU-only model
  that uses no DRP-AI memory), aligned to 16 MiB as Renesas' own tutorial
  does, and keeps the last 32 MiB of the 512 MiB region free for DRP-AI
  pre-processing (an estimate -- no compiled bundle exists to measure it,
  #2236). A model that does not fit makes `alp_inference_open()` fail
  with `ALP_ERR_NOMEM`. Closing a handle gives its range back: the next
  model starts after the highest range still open (the arena base if none),
  so closing a model and loading another reuses the space. A hole below a
  still-open higher model is not reused.
- **Jobs are serialised.** One process-wide mutex covers `SetInput` +
  `Run` in `alp_inference_invoke()`, and also the model load in `open()`
  and the runtime teardown in `close()`, so threads on different handles
  wait their turn instead of colliding on the driver. Two models on DRP-AI
  therefore cost the sum of their latencies.
- **A failed job is not reported.** The runtime's `Run()` returns void,
  so a rejected or timed-out job still returns `ALP_OK`. A driver status
  query is not a cheap fix (it needs a second open of `/dev/drpai0`,
  which takes the driver semaphore for up to 1000 ms and the
  shared-memory lock), so the SDK makes none; sanity-check the outputs.
- **DRP-AI is one process per board.** The arena placement above is per
  process, so two processes would both load at the arena base and corrupt
  each other. The first DRP-AI handle in a process therefore takes an
  exclusive, non-blocking `flock()` on `/run/alp/drpai.lock` (the image
  creates `/run/alp` at boot, `root:drpai 0775`, via a systemd tmpfiles.d
  snippet in the `alp-drpai-udev` recipe) and keeps it until the last DRP-AI handle
  in that process closes. A second process gets `ALP_ERR_BUSY` from
  `alp_inference_open()`. Several handles inside one process stay allowed.
  There is no fallback path: a root and a non-root process must lock the
  same file, so if the lock file cannot be opened `alp_inference_open()`
  fails with `ALP_ERR_IO`. The DX-M1 has no such limit: a second
  process uses only the DX-M1, through `dxrtd`.

### One model per NPU (V2M)

An E1M-V2M has two NPUs, and they are independent hardware (own drivers,
IRQs and DMA engines), so one model on each can run at the same time:
open one handle with `.backend = ALP_INFERENCE_BACKEND_DRPAI` and one
with `.backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1`, and invoke each from
its own thread. Do not use `ALP_INFERENCE_BACKEND_AUTO` -- it resolves to
the same backend every time. `alp_inference_config_t.accel_unit_mask`
chooses the unit(s) inside a backend (bit `n` = unit `n`, `0` = default):
the DX-M1 handle can name NPU cores (`0x7` is all three), while DRP-AI3 is
one unit and takes only `0` or `0x1`; any other mask fails the open with
`ALP_ERR_NOSUPPORT`. `examples/v2n/v2n-two-models/` does exactly
this and prints per-NPU latency and combined FPS.

> **Not bench-verified.** The two backends have never run together. The
> V2M machine configs already enable both stacks when their layers are
> present (DEEPX with `meta-deepx-m1`, DRP-AI with `meta-rz-drpai`), so the
> gap is RUHMI, not a switch: the SDK's DRP-AI backend is only compiled when
> `RUHMI_DRPAI_TVM_DIR` points at an account-gated Renesas RUHMI checkout
> (section 4). Without it the image carries the DRP-AI kernel driver and TVM
> runtime but `libalp_sdk` has no DRP-AI backend, and
> `.backend = ALP_INFERENCE_BACKEND_DRPAI` fails with `ALP_ERR_NOSUPPORT`
> (the `alp-sdk` recipe warns at parse time when it sees this combination).
> On a build with both backends, `ALP_INFERENCE_BACKEND_AUTO` resolves to
> the DX-M1, so the DRP-AI handle must name `ALP_INFERENCE_BACKEND_DRPAI`
> explicitly. Shared DDR bandwidth, CPU pre/post-processing, power and
> thermal under both NPUs at full load are unmeasured.
>
> **CPU threads.** The DRP-AI TVM runtime starts a CPU worker pool
> (`TVM_NUM_THREADS`, `TVM_BIND_THREADS`) and, by upstream TVM's default,
> pins one worker per core (not confirmed for this build). The 4 A55 cores
> are also used by dx-rt's worker threads, `dxrtd`, and both models'
> pre/post-processing, so the example sets `TVM_NUM_THREADS=2` and
> `TVM_BIND_THREADS=0` unless you already set them. The effect on latency
> is unmeasured.

## Related

- [bring-up-v2n.md](bring-up-v2n.md) — base V2N bring-up
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — the DEEPX delta
- [build-yocto-v2n.md](build-yocto-v2n.md) — kernel + rootfs build and deploy
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — carrier errata, including the
  Ethernet MDI mirror that makes microSD the only deployment path
- `docs/test-plan.md` — the verification rows this bring-up gates
