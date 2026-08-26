# DRP-AI3 bring-up on E1M-X V2N

How to get the RZ/V2N's on-die DRP-AI3 NPU running a real model through
`<alp/inference.h>` on an E1M-X V2N SoM.

> **Status: KERNEL DRIVER PROVEN ON SILICON, PACKAGING WRITTEN BUT NEVER BAKED,
> INFERENCE NOT YET RUN.** A full `alp-image-edge` bake now completes on this
> host (12118 tasks,
> all succeeded, a 716 MB `.wic.gz`) — the first ever; previously nothing had
> baked. That run had `drpai` OFF (the base image); see §4 for what is and
> isn't proven about the `drpai`-enabled path. `PACKAGECONFIG[drpai]` now
> resolves the whole MERA2 runtime closure, and the `MeraDrpRuntimeWrapper::*`
> symbols alp-sdk needs all match what the wrapper exports (26 exported, 9
> referenced, 0 unresolved) — confirmed at the compile/symbol level, not yet
> through a `drpai`-enabled bake on the real aarch64 Yocto cross-toolchain. On
> real E1M-X V2N-M1 silicon the DRP-AI **kernel** driver stack is proven
> working: `/dev/drpai0` probes clean and the memory-base ioctl returns the
> correct arena (§3, §7) — but that silicon runs its own current image, not
> one built from this branch. No model has been compiled and no inference has
> run. Treat this as the procedure to execute and verify to completion, not a
> report of a working system. `docs/test-plan.md` carries the verification
> rows this gates.

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
| `libtvm_runtime.so` | `meta-rz-drpai` recipe `lib-tvm` | |
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
only when **both** `meta-rz-drpai` is in `bblayers.conf` **and**
`ALP_ENABLE_DRPAI = "1"` is set (default `"0"`).  The layer alone is
deliberately not enough: it ships bundled in the AI SDK BSP, so keying off its
presence would turn the NPU on for every V2N/V2M image.  It is declared
`ALP_ENABLE_DRPAI ?= "0"` in all four V2N/V2M machine confs; set it to
`"1"` in `local.conf` to opt in. Without it the build installs a comment-only stub and the node
stays `disabled`.

The layer half of the gate exists because that layer creates the `drpai0`
label: referencing it without the layer fails in dtc, and the same SoM dtsi
is included by the V2M board dts, so it would take that dtb down too.

**Silicon confirms the node is not on by default.** `e1mx-v2n-m1-01`'s current
dtb, `/boot/r9a09g056n44-dev.dtb`, carries **zero** `drpai` nodes — the
enablement on that board comes from a different, already-loaded
`/boot/uio-683.dtb`, not from anything this repo builds. Our own dtb,
`e1m-v2n101-x-evk.dtb`, carries the node enabled **only when
`ALP_ENABLE_DRPAI = "1"` was set for that build**; by default it carries the
stub and the node stays `disabled`.  The overlay is what makes it present,
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
`e1mx-v2n-m1-01`, `rgnmm_drv mmngr: assigned reserved memory node
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
`e1mx-v2n-m1-01` has only one DRP-AI region, and two `DRPAI_GET_DRPAI_AREA`
calls on the *same* fd returned identical values on that board — but with a
single region a per-fd alternating cursor and no cursor at all look
identical from the outside. Whether the fresh-fd-per-call approach is
load-bearing on a two-region config remains unresolved; treat it as a safe
no-op on hardware seen so far, not as validated for that case.

## 4. Image

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
recipe's own `do_compile` has been proven only compiling the wrapper source
cleanly against a real RUHMI checkout's headers on an x86_64 dev host (system
spdlog/asio standing in for meta-oe's); the final link against the real
aarch64 `obj/build_runtime/v2h` libraries has not been exercised, and no
`bitbake` run of this recipe — with or without `do_compile` — has happened at
all. A full `alp-image-edge` bake has completed on this host (12118 tasks,
producing a 716 MB `.wic.gz`, the first ever here) but with `drpai` OFF (the
base image); a `drpai`-enabled bake on the real aarch64 Yocto cross-toolchain
is the step that would confirm the link and packaging end to end.

**`meta-rz-drpai` on `bblayers.conf` is necessary but not sufficient for the
image.** That layer ships its payload through a `core-image-%.bbappend`, and
that wildcard does not match `alp-image-edge`, so the bbappend never fires and
the image comes out with no DRP-AI userspace at all — silently.
`alp-image-common.inc` therefore installs `lib-tvm` and `kernel-module-mmngr`
explicitly, gated on the layer being present. See issue #1176; the same trap
applies to the other `meta-rz-*` feature layers.

## 5. Model compile

```sh
export ALP_DRPAI_TVM_HOME=<rzv_drp-ai_tvm checkout>
tan model build --board <path>/board.yaml
```

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

**No compiled model exists yet — an ONNX source does.** RUHMI ships a real
model, `how-to/sample_app_v2h/app_yolox_cam/yolox-S_VOC.onnx` (35 MB,
YOLOX-S on VOC), but there is no pre-compiled `drpai_dir` output anywhere in a
fresh checkout — searching for `drp_desc.bin`, `weight.bin`, `addr_map.txt`
and `deploy.json` finds none. `tutorials/README.md` documents the alternative
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

> **Neither fix has booted a board.** `dev`'s version is code-complete and its
> own CHANGELOG says so; a second, independent implementation exists unmerged
> on `feat/1145-drpai-v2n-bringup` using per-MACHINE `CONFIG_ALP_FDT_FILE` /
> `CONFIG_ALP_SD_ROOT` Kconfig strings, which would also close the V2M gap
> `dev`'s hardcoded filename leaves open. See #1175 before relying on either.

> **Operational trap.** The manual FIP flow has no `merge_config.sh` step, so it
> builds from the Kconfig defaults — the vendor values — and will boot the
> wrong dtb. Build the FIP through the Yocto path, or check the resulting
> `.config` before flashing. Recoverable by reflashing a known-good FIP, but it
> costs a bench session.

> **Flash-plan warning.** Don't assume the dtb your image built is the one a
> board will actually boot. On
> `e1mx-v2n-m1-01`, the running kernel's `bootargs` carry
> `uio_pdrv_genirq.of_id=generic-uio` — a string that appears nowhere in the
> `bootcmd` currently stored in `mtd1`. That means the live kernel/dtb/cmdline
> did **not** come from that stored bootcmd, and reading `mtd1` alone cannot
> tell you what will actually load on the next power cycle. Establishing the
> real load path needs catching the U-Boot prompt over serial (i.e. a reboot)
> before trusting any flash plan built from the stored env.

## 7. Verify on the board

In order:

1. `ls /dev/drpai0` — absent means one of three things, in the order worth
   checking: `ALP_ENABLE_DRPAI` was not set to `"1"` (the default, and now the
   most likely cause); `meta-rz-drpai` was not in `bblayers.conf`; or the DT
   override otherwise did not land. Nothing else will work. (This
   node already exists on `e1mx-v2n-m1-01`'s current, non-ALP-built image, so
   its presence alone doesn't prove *this* image's DT override worked — check
   the dtb in use, per §3.)
2. `dmesg | grep -i drpai` — a probe failing `-ENOMEM` means
   `memory-shared-for-drpai-ext-cont` is missing. Confirmed good on
   `e1mx-v2n-m1-01`: `drpai-rz 17000000.drpai: DRP-AI Driver version : 1.40
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
   On `e1mx-v2n-m1-01` this returns `ADDR=0x00000000d0000000
   SIZE=0x0000000020000000`, matching the driver's own boot print, the DT
   `reg`, and `/proc/iomem` (`d0000000-efffffff : reserved`).
4. `ls /usr/lib/libtvm_runtime.so*` and `ls /usr/lib/libmera2_runtime.so*` —
   absent means the image did not get the vendor payload (§4); on
   `e1mx-v2n-m1-01`'s current image neither exists yet (`ls
   /usr/lib/libdrpai*` also finds nothing) — that userspace gap is what this
   branch's packaging is meant to close, once run through a `drpai`-enabled
   bake (§4).
5. Run the model. `alp_inference_open(.backend = ALP_INFERENCE_BACKEND_DRPAI)`
   returning `NULL` with `ALP_ERR_NOSUPPORT` means the backend was not compiled
   in; `ALP_ERR_TIMEOUT` means the driver semaphore expired; `ALP_ERR_BUSY` means
   the shared-memory exclusion lock is contended. **Not yet reachable: no model
   has been compiled (§5).**

## Related

- [bring-up-v2n.md](bring-up-v2n.md) — base V2N bring-up
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — the DEEPX delta
- [build-yocto-v2n.md](build-yocto-v2n.md) — kernel + rootfs build and deploy
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — carrier errata, including the
  Ethernet MDI mirror that makes microSD the only deployment path
- `docs/test-plan.md` — the verification rows this bring-up gates
