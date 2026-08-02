# DRP-AI3 bring-up on E1M-X V2N

How to get the RZ/V2N's on-die DRP-AI3 NPU running a real model through
`<alp/inference.h>` on an E1M-X V2N SoM.

> **Status: NOT YET RUN ON SILICON.** Every step below is derived from the
> recipes, the vendor layer and the driver source, and the SDK-side changes pass
> the local gate set — but no full `alp-image-edge` bake has ever completed and
> there is no DRP-AI bench result on record. Treat this as the procedure to
> execute, not a report of a working system. `docs/test-plan.md` carries the
> verification rows this gates.

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
| `mera2_runtime`, `mera2_plan_io`, `drp_tvm_rt`, `MeraDrpRuntimeWrapper.h` | a **built** `rzv_drp-ai_tvm` (RUHMI) checkout | Apache-2.0, not NDA-gated |

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

Three things bite here:

- **Initialise the submodules.** A checkout can have its runtime libraries
  already built while `tvm/` is still empty. `MeraDrpRuntimeWrapper.h` hard-includes
  `<tvm/runtime/profiling.h>`, which lives in the `tvm` submodule, so without
  `git submodule update --init --recursive` any compile against the wrapper dies
  with `fatal error: tvm/runtime/profiling.h: No such file or directory`.
  `meta-rz-drpai`'s `lib-tvm` does not help — it ships `libtvm_runtime.so*` and a
  LICENSE, no headers.
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

Both memory properties are mandatory. On V2N the driver defines
`ENABLE_DRP_SUPPORT_SHARED_MEMORY`, so a probe without
`memory-shared-for-drpai-ext-cont` hard-fails with `-ENOMEM` rather than
degrading to a single-region mode.

**Never point `memory-region` at `mmp_reserved` (`0x80000000`).** That is the
mmngr video buffer pool. The NPU DMAs against this base directly, so a wrong
value corrupts the video pipeline silently instead of failing.

The runtime does not hard-code the base: `_drpai_mem_start()` in
`src/yocto/inference_drpai.cpp` asks the driver via `DRPAI_GET_DRPAI_AREA` on a
fresh `/dev/drpai0` fd. A fresh fd is deliberate — the region cursor is per-fd
state and alternates once a second region exists — but it is not free:
`drpai_open()` takes a 1000 ms `down_timeout()`, and the matching `close()`
resets the DRP-AI when it is the sole opener. It runs once at open, never per
inference.

## 4. Image

Enable the backend through the SDK recipe's PACKAGECONFIG:

```
PACKAGECONFIG:append:pn-alp-sdk = " drpai"
```

That switch flips `-DALP_SDK_USE_DRPAI_V2N=ON` and adds the `drpai` and
`lib-tvm` build deps together. The three RUHMI libraries and the wrapper header
are not packaged by any recipe by default — stage them into the sysroot, or
point `ALP_DRPAI_TVM_APPS` + `CMAKE_LIBRARY_PATH` at the checkout, before
enabling it. A missing input is reported at configure time with the exact name.

**`meta-rz-drpai` on `bblayers.conf` is necessary but not sufficient for the
image.** That layer ships its payload through a `core-image-%.bbappend`, and
that wildcard does not match `alp-image-edge`, so the bbappend never fires and
the image comes out with no DRP-AI userspace at all — silently.
`alp-image-edge.bb` therefore installs `lib-tvm` and `kernel-module-mmngr`
explicitly, gated on the layer being present. See issue #1176; the same trap
applies to the other `meta-rz-*` feature layers.

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

The output is a **directory tar**, not a flat buffer — `blob_format` is
`drpai_dir`, containing `drp_desc.bin`, `weight.bin`, `addr_map.txt`,
`deploy.json`, `deploy.so` and `preprocess/`. `alp_inference_open()` extracts it
to a private temporary directory before calling `LoadModel()`.

## 6. Deploy

With the EVK strapped for **xSPI boot**, BL2 and the FIP come from `mtd0`/`mtd1`
and the microSD supplies only the kernel and rootfs — so this path never writes
xSPI, and cannot brick the boot chain.

Write the `.wic.gz` to a microSD card and insert it. The patched U-Boot tries
microSD first (`if mmc dev 1`) and falls back to eMMC.

Two things had to be fixed for a self-built image to boot this way, and both
live in `meta-alp-sdk/recipes-bsp/u-boot/`:

- The vendor env loads `boot/r9a09g056n44-dev.dtb`, a filename no ALP image
  builds, on **both** the SD and eMMC paths. `CONFIG_ALP_FDT_FILE` supplies the
  per-SKU name and `CONFIG_BOOTCOMMAND` reloads it after the leading
  `env default -a`. A miss now refuses to boot rather than pressing on with a
  stale devicetree — hush does not abort a `;` list on a failed builtin, so
  without the guard `booti` would run against whatever sat at `0x48000000`.
- `CONFIG_ALP_SD_ROOT` sets the microSD root device. **Not bench-confirmed:**
  the DT says `mmcblk1p2` and the vendor env said `mmcblk2p2`; one console boot
  settles it.

> **Operational trap.** The manual FIP flow has no `merge_config.sh` step, so it
> builds from the Kconfig defaults — which are the vendor values. With the guard
> in place, a FIP built that way will hard-stop instead of booting. Set
> `CONFIG_ALP_FDT_FILE` and `CONFIG_ALP_SD_ROOT` in that flow's `.config` before
> the next FIP build. Recoverable by reflashing a known-good FIP, but it costs a
> bench session.

## 7. Verify on the board

In order:

1. `ls /dev/drpai0` — absent means the DT override did not land or
   `meta-rz-drpai` was not in `bblayers.conf`. Nothing else will work.
2. `dmesg | grep -i drpai` — a probe failing `-ENOMEM` means
   `memory-shared-for-drpai-ext-cont` is missing.
3. `ls /usr/lib/libtvm_runtime.so*` — absent means the image did not get the
   vendor payload (§4).
4. Run the model. `alp_inference_open(.backend = ALP_INFERENCE_BACKEND_DRPAI)`
   returning `NULL` with `ALP_ERR_NOSUPPORT` means the backend was not compiled
   in; `ALP_ERR_TIMEOUT` means the driver semaphore expired; `ALP_ERR_BUSY` means
   the shared-memory exclusion lock is contended.

## Related

- [bring-up-v2n.md](bring-up-v2n.md) — base V2N bring-up
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — the DEEPX delta
- [build-yocto-v2n.md](build-yocto-v2n.md) — kernel + rootfs build and deploy
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — carrier errata, including the
  Ethernet MDI mirror that makes microSD the only deployment path
- `docs/test-plan.md` — the verification rows this bring-up gates
