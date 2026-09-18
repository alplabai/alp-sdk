### Added — `v2n-drpai-inference` example + the `ALP_ENABLE_DRPAI` opt-in that wires DRP-AI3 into `alp-image-edge` (#1145, #1268)

Salvaged from `feat/1145-drpai-v2n-bringup` (`1cadada9bb12b3039db35642c29bbef592b832e0`)
as a fresh PR after #1693 was found to overclaim what had actually run;
`docs/bring-up-drpai-v2n.md` (already on `dev`) carries the honest
BENCH-UNVERIFIED status this entry keeps.

**🟡 Code-complete, bench-unverified. No `drpai`-enabled `alp-image-edge`
bake has ever completed, on any host, and nothing has run on DRP-AI
silicon.** Bench sign-off is tracked as #1268.

- **`examples/v2n/v2n-drpai-inference/`** — the exhibition-booth demo: reads
  one or more raw pre-processed 640x640x3 float32 NHWC frames given on the
  command line, runs each through `<alp/inference.h>` with
  `backend = ALP_INFERENCE_BACKEND_DRPAI`, and prints per-image top-5 raw
  scores + timing. `[UNTESTED on silicon]`. `src/top_scores.c` (the top-N
  selection helper) is unit-tested by `tests/unit/top_scores/`.
- **`ALP_ENABLE_DRPAI`** (new, default `"0"`) on all four RZ/V2N-family
  machine confs (`e1m-v2n101-a55.conf`, `e1m-v2n102-a55.conf`,
  `e1m-v2m101-a55.conf`, `e1m-v2m102-a55.conf`) is a single opt-in driving
  three things together: `IMAGE_INSTALL:append` (`lib-tvm
  kernel-module-mmngr alp-drpai-inference`), `PACKAGECONFIG:append:pn-alp-sdk
  = " drpai"`, and the `&drpai0` devicetree node — so the demo binary, the
  compiled-in SDK backend, and the kernel claim of the NPU can't drift out of
  sync. `alp-image-edge.bb` gates a `bb.fatal()` guard on the
  `rzv2n-family` `MACHINEOVERRIDES` override so opting in without
  `meta-rz-drpai` in `bblayers.conf` fails loudly at parse time instead of
  an obscure missing-recipe error.
- **`meta-alp-sdk/recipes-examples/alp-drpai-inference/alp-drpai-inference_0.6.bb`**
  packages the example the same way `alp-edgeai_0.6.bb` /
  `alp-lvgl-dashboard_0.6.bb` do.
- **`alp-sdk_0.6.bb`'s `PACKAGECONFIG[drpai]`** (added on `dev` separately by
  `cbea0e29`) gains the genuine delta this branch needed: the OFF branch now
  also sets `-DALP_SDK_DRPAI_REQUIRED=OFF`, and the DEPENDS/RDEPENDS field
  adds `mera2-drpai-tvm` as a runtime dependency — plus a `do_configure`
  prefunc (`do_generate_toolchain_file`) that keeps `${WORKDIR}/toolchain.cmake`
  from going stale under `externalsrc` bakes.
- **`src/yocto/inference_drpai.cpp`**'s header comment is corrected, not
  copied verbatim from the source branch: the branch claimed a
  `drpai`-enabled bake resolved all 9 `MeraDrpRuntimeWrapper` symbols with
  "0 unresolved" — that never happened. What is actually established (see
  `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/mera2-drpai-tvm_2.7.0.bb`):
  the wrapper source cross-compiles to a valid `.o` with every symbol
  defined, confirmed by hand with `nm` on an x86_64 dev host, NOT by a bake;
  the final link against the real aarch64 `obj/build_runtime/v2h`
  libraries has never been exercised (that same host stops at "skipping
  incompatible ... when searching for -lmera2_runtime", an architecture
  mismatch, not proof of symbol resolution).

**Open reviewer question, not resolved by this change:** `alp-image-edge.bb`'s
`ALP_ENABLE_DRPAI`-gated `IMAGE_INSTALL:append` overlaps `dev`'s existing
unconditional `ALP_RZ_DRPAI_INSTALL` in `alp-image-common.inc` (#1176) — both
install the `meta-rz-drpai` userspace payload, one unconditionally and one
opt-in. `bitbake` dedupes the repeated package names, so this is not a build
break, but which mechanism should own the userspace install long-term is an
open question, left to review rather than picked here.

`docs/board-config-schema.md`'s example preset count is corrected to match:
101 examples target a preset today (75 on `e1m-evk`, 26 on `e1m-x-evk`,
counted directly off `examples/**/board.yaml`, not carried over from either
side of the merge).
