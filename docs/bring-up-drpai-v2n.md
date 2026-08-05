# DRP-AI3 bring-up on E1M-X V2N

How to get the RZ/V2N's on-die DRP-AI3 NPU running a real model through
`<alp/inference.h>` on an E1M-X V2N SoM.

> **Status: KERNEL DRIVER PROVEN ON SILICON, PACKAGING FIXED, INFERENCE NOT YET
> RUN.** A full `alp-image-edge` bake now completes on this host (12118 tasks,
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
> one built from this branch. A model has now been compiled (§5) — YOLOX-S
> VOC, fully NPU-offloaded per its deploy graph — but it was quantised
> without the vendor calibration set, so its accuracy is unvalidated, and no
> inference has run on silicon. Treat this as the procedure to execute and
> verify to completion, not a report of a working system. `docs/test-plan.md`
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
| `libtvm_runtime.so` | `meta-rz-drpai` recipe `lib-tvm` | |
| The MERA2 runtime closure: headers + **ten** libraries, not three | `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb`, staged/compiled from a builder-supplied **`RUHMI_DRPAI_TVM_DIR`** checkout | Apache-2.0, not NDA-gated; the recipe vendors nothing — see §4 |

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
only when `meta-rz-drpai` is in `bblayers.conf` (that layer creates the `drpai0`
label, so referencing it without the layer fails in dtc — and the same SoM dtsi
is included by the V2M board dts, so it would take that dtb down too).

**Silicon confirms the node is not on by default.** `e1mx-v2n-m1-01`'s current
dtb, `/boot/r9a09g056n44-dev.dtb`, carries **zero** `drpai` nodes — the
enablement on that board comes from a different, already-loaded
`/boot/uio-683.dtb`, not from anything this repo builds. Our own dtb,
`e1m-v2n101-x-evk.dtb`, does carry the node enabled, via the `&drpai0`
overlay in `e1m-v2n-drpai.dtsi` above; that overlay is what makes it present,
not the SoC by default. Separately: the **kernel** half of the stack is
already proven working on this silicon — `/dev/drpai0` exists on that board's
current image and the driver probes clean (`drpai-rz 17000000.drpai: DRP-AI
Driver version : 1.40 rel.3 V2N`, correct memory-region prints, zero errors).
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

Enable the backend with the single opt-in switch, in `local.conf`:

```
ALP_ENABLE_DRPAI = "1"
```

The four rzv2n-family machine confs (`e1m-v2n101-a55.conf`,
`e1m-v2n102-a55.conf`, `e1m-v2m101-a55.conf`, `e1m-v2m102-a55.conf`) read that
one variable and drive both sides of the hardware fact from it, so they
cannot diverge:

- `IMAGE_INSTALL:append` adds `lib-tvm kernel-module-mmngr` (the userspace
  runtime payload).
- `PACKAGECONFIG:append:pn-alp-sdk` adds `drpai`, which flips
  `-DALP_SDK_USE_DRPAI_V2N=ON` and `-DALP_SDK_DRPAI_REQUIRED=ON` in
  `src/yocto/CMakeLists.txt` and adds the `drpai` and `lib-tvm` build deps.

It still needs `meta-rz-drpai` in `bblayers.conf` (`lib-tvm` doesn't exist
without it) — opting in without the layer is caught by an anonymous-Python
guard in `alp-image-edge.bb` that fails the parse loudly, naming the cause,
instead of an obscure missing-recipe error at build time.

**Status: `ALP_ENABLE_DRPAI` is parse-verified and expansion-verified, not yet
bake-verified.** In an isolated build dir (own `TMPDIR`/`SSTATE_DIR`,
`EXTERNALSRC:pn-alp-sdk` pointed at the checkout, the manual
`PACKAGECONFIG:append:pn-alp-sdk` override below commented out so
`ALP_ENABLE_DRPAI` was the only possible source of the enable), with
`meta-rz-drpai` in `bblayers.conf`:

```
Parsing of 5447 .bb files complete (0 cached, 5447 parsed). 8037 targets, 387 skipped, 70 masked, 0 errors.

bitbake -e alp-sdk        ->  PACKAGECONFIG="mqtt security audio drpai"
bitbake -e alp-image-edge ->  IMAGE_INSTALL contains lib-tvm and kernel-module-mmngr
```

Both halves of the opt-in resolve from `ALP_ENABLE_DRPAI` alone, and the
recipe's weak defaults (`mqtt security audio`) survive the append rather than
being replaced. With `meta-rz-drpai` removed from `bblayers.conf` (same
config otherwise), the parse halts (rc=1) and the guard fires with exactly
its intended text:

```
ERROR: meta-alp-sdk/recipes-images/alp-image-edge.bb: ALP_ENABLE_DRPAI = "1" but the rz-drpai layer (meta-rz-drpai) is not in bblayers.conf -- lib-tvm and kernel-module-mmngr do not exist without it. Add meta-rz-drpai to bblayers.conf or set ALP_ENABLE_DRPAI = "0".
```

**Not yet bake-verified: no image has been built through the
`ALP_ENABLE_DRPAI` path.** The bake this document's "confirmed"/"proven"
claims are measured against used the older, manual route instead, set
directly in `local.conf` alongside `RUHMI_DRPAI_TVM_DIR` pointed at a built
RUHMI checkout:

```
PACKAGECONFIG:append:pn-alp-sdk = " drpai"
```

That manual route is what actually produced a completed image (MACHINE
`e1m-v2m101-a55`, 12118 tasks attempted, all succeeded). No image has been
produced through `ALP_ENABLE_DRPAI` yet — re-verify with a real bake before
relying on it to the same degree.

**The RUHMI libraries and wrapper header are now packaged**, closing the gap
the earlier revision of this doc left as a manual staging step.
`meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb` stages
headers plus the closure — **ten** libraries, not three: eight copied verbatim
out of a builder-supplied, already-built `rzv_drp-ai_tvm` checkout's
`obj/build_runtime/v2h/lib` (libmera2_runtime, libmera2_plan_io, libdrp_tvm_rt,
libdrp_rt, libacl_rt, libarm_compute, libarm_compute_core,
libarm_compute_graph), `libtvm_runtime.so` separately via `meta-rz-drpai`'s
`lib-tvm`, and a ninth the recipe **compiles itself**,
`libmera_drpai_wrapper.so`, from the checkout's `apps/MeraDrpRuntimeWrapper.cpp`
— that class ships as application-side glue source with no prebuilt library at
all, so the recipe compiles it once rather than leaving every consumer to
duplicate the vendor glue. It also `RDEPENDS` on mmngr-user-module /
mmngrbuf-user-module for `libmmngr.so.1` / `libmmngrbuf.so.1`. The recipe
fetches and vendors nothing: point the single variable **`RUHMI_DRPAI_TVM_DIR`**
(in `local.conf` or the environment) at a built checkout before enabling
`drpai` — an unset or incomplete checkout fails `do_compile`/`do_install`
loudly, naming the exact missing path.

**Verified, at the symbol level:** with `RUHMI_DRPAI_TVM_DIR` pointed at a real
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
the image comes out with no DRP-AI userspace at all — silently. The fix is
`ALP_ENABLE_DRPAI = "1"` (above): the four rzv2n-family machine confs install
`lib-tvm` and `kernel-module-mmngr` explicitly, gated on that variable, not on
layer presence. `alp-image-edge.bb` itself installs neither — it only carries
the loud-failure guard for opting in with the layer absent. See issue #1176;
the same trap applies to the other `meta-rz-*` feature layers.

## 5. Model compile

```sh
export ALP_DRPAI_TVM_HOME=<rzv_drp-ai_tvm checkout>
python3 -m alp_model build --target drpai --product V2N <model.onnx>
```

`scripts/alp_model/adapters/drpai.py` drives
`$ALP_DRPAI_TVM_HOME/tutorials/compile_onnx_model_quant.py` with `PRODUCT` in the
environment. It needs an input shape and name, and calibration images; the
tutorial falls back to random calibration data, which is enough to prove the
pipeline but not enough for a demo's accuracy.

**A compiled model bundle now exists.** RUHMI's own
`how-to/sample_app_v2h/app_yolox_cam/yolox-S_VOC.onnx` sample (35 MB, YOLOX-S
on VOC) has been compiled with DRP-AI Translator i8 v1.12. Its
`sub_0000__CPU_DRP_TVM/deploy.json` has two nodes — a `null` input node named
`images`, and one fused `tvm_op` node,
`tvmgen_default_tvmgen_default_mera_drp_main_0` — so the whole graph is
offloaded to the NPU, not split with a CPU fallback. **Its accuracy is
unvalidated**: it was quantised without the vendor calibration set — all 200
images under the Translator's `drpAI_Quantizer/calibrate_images` are 129-byte
Git LFS pointer stubs, not real image data. No inference has been run on
silicon with this model yet. `tutorials/README.md` also documents an
alternative public source, `resnet18-v1-7.onnx` from the `onnx/models` repo
via `compile_onnx_model.py`, for anyone compiling a different model. Either
way, compiling still requires the account-gated DRP-AI Translator (§1) —
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

Two things had to be fixed for a self-built image to boot this way, and both
live in `meta-alp-sdk/recipes-bsp/u-boot/`:

- The vendor env loads `boot/r9a09g056n44-dev.dtb`, a filename no ALP image
  builds, on **both** the SD and eMMC paths. `CONFIG_ALP_FDT_FILE` supplies the
  per-SKU name and `CONFIG_BOOTCOMMAND` reloads it after the leading
  `env default -a`. A miss now refuses to boot rather than pressing on with a
  stale devicetree — hush does not abort a `;` list on a failed builtin, so
  without the guard `booti` would run against whatever sat at `0x48000000`.
- `CONFIG_ALP_SD_ROOT` sets the microSD root device. **Now confirmed:**
  `mmcblk2` doesn't exist on this silicon at all — the board has exactly two
  SDHI controllers, `15c00000.mmc` -> `mmc0` -> eMMC (with `boot0`/`boot1`/
  `rpmb` partitions) and `15c10000.mmc` -> `mmc1` -> the SDHC slot — so eMMC is
  always `mmcblk0` and microSD is always `mmcblk1`. The vendor env's
  `alp_root=/dev/mmcblk2p2` names a device that can't exist; `mmcblk1p2` (what
  `CONFIG_ALP_SD_ROOT` in `alp-boot-v2n.cfg` sets) is the fix.

> **Operational trap.** The manual FIP flow has no `merge_config.sh` step, so it
> builds from the Kconfig defaults — which are the vendor values. With the guard
> in place, a FIP built that way will hard-stop instead of booting. Set
> `CONFIG_ALP_FDT_FILE` and `CONFIG_ALP_SD_ROOT` in that flow's `.config` before
> the next FIP build. Recoverable by reflashing a known-good FIP, but it costs a
> bench session.

> **Flash-plan warning.** Don't assume a file written under the
> `CONFIG_ALP_FDT_FILE` name is what a board will actually boot. On
> `e1mx-v2n-m1-01`, the running kernel's `bootargs` carry
> `uio_pdrv_genirq.of_id=generic-uio` — a string that appears nowhere in the
> `bootcmd` currently stored in `mtd1`. That means the live kernel/dtb/cmdline
> did **not** come from that stored bootcmd, and reading `mtd1` alone cannot
> tell you what will actually load on the next power cycle. Establishing the
> real load path needs catching the U-Boot prompt over serial (i.e. a reboot)
> before trusting any flash plan built from the stored env.

## 7. Verify on the board

In order:

1. `ls /dev/drpai0` — absent means the DT override did not land or
   `meta-rz-drpai` was not in `bblayers.conf`. Nothing else will work. (This
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
   the shared-memory exclusion lock is contended. **Not yet run:** a compiled
   model now exists (§5) and the backend cross-links (the symbol measurement
   is recorded in
   `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb`),
   but no inference has been run on silicon and the model's accuracy is
   unvalidated (§5).

## Related

- [bring-up-v2n.md](bring-up-v2n.md) — base V2N bring-up
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — the DEEPX delta
- [build-yocto-v2n.md](build-yocto-v2n.md) — kernel + rootfs build and deploy
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — carrier errata, including the
  Ethernet MDI mirror that makes microSD the only deployment path
- `docs/test-plan.md` — the verification rows this bring-up gates
