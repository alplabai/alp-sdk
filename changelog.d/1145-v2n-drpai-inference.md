### Added — `v2n-drpai-inference` example + the `ALP_ENABLE_DRPAI` opt-in that wires DRP-AI3 into `alp-image-edge` (#1145, #1268)

Salvaged from `feat/1145-drpai-v2n-bringup` (`1cadada9bb12b3039db35642c29bbef592b832e0`)
as a fresh PR after #1693 was found to overclaim what had actually run;
`docs/bring-up-drpai-v2n.md` (already on `dev`) carries the honest
BENCH-UNVERIFIED status this entry keeps.

**🟡 Code-complete, bench-unverified. No `drpai`-enabled `alp-image-edge`
bake has ever completed, on any host, and nothing has run on DRP-AI
silicon.** Bench sign-off is tracked as #1268.

- **`examples/v2n/v2n-drpai-inference/`** — the exhibition-booth demo: reads
  one or more raw pre-processed 640x640x3 float32 NCHW (planar) frames given on the
  command line, runs each through `<alp/inference.h>` with
  `backend = ALP_INFERENCE_BACKEND_DRPAI`, and prints per-image top-5 raw
  scores + timing. `[UNTESTED on silicon]`. `src/top_scores.c` (the top-N
  selection helper) is unit-tested by `tests/unit/top_scores/`.
- **`ALP_ENABLE_DRPAI`** (default `"0"`; the flag itself predates this change
  — added by `cbea0e29`) is declared on all six RZ/V2N-family machine confs
  (`e1m-v2n101-a55.conf`, `e1m-v2n102-a55.conf`, `e1m-v2n103-a55.conf`,
  `e1m-v2m101-a55.conf`, `e1m-v2m102-a55.conf`, `e1m-v2m103-a55.conf`) and
  does two things: it gates the `&drpai0` devicetree node, and -- on
  `alp-image-edge` only -- gates installing the demo binary below. It
  installs no userspace runtime package itself. An earlier revision of this
  change also had the four v2n101/102 + v2m101/102 confs gate a machine-conf
  `IMAGE_INSTALL:append` (`lib-tvm kernel-module-mmngr`); review caught that
  this duplicated `alp-image-common.inc`'s existing `ALP_RZ_DRPAI_INSTALL`
  (#1176) pair-for-pair for every `alp-image-*` image, so it was always a
  no-op there, never a drift guard -- the four appends are dropped from this
  change, and `alp-image-common.inc` is the userspace pair's single
  packaging authority for `alp-image-*` builds. The demo binary, **`alp-drpai-inference`**,
  rides the same `ALP_ENABLE_DRPAI` opt-in but from a separate, IMAGE-level
  append in `alp-image-edge.bb` (not the machine confs), gated on the same
  `rzv2n-family` `MACHINEOVERRIDES` check as the `bb.fatal()` guard below,
  so this booth demo (#1268) reaches `alp-image-edge` only -- and only on
  an `rzv2n-family` MACHINE -- never `alp-image-prod`. The
  alp-sdk backend's own `PACKAGECONFIG[drpai]` stays the released,
  independently-set second
  switch ("Two independent switches, both default OFF, deliberately not
  merged into one" — `CHANGELOG.md`'s v0.15.0 entry). An earlier revision of
  this branch coupled the two via `PACKAGECONFIG:append:pn-alp-sdk`; that
  coupling is dropped from this change. `alp-image-edge.bb` gates a
  `bb.fatal()` guard on the `rzv2n-family` `MACHINEOVERRIDES` override so
  opting in without `meta-rz-drpai` in `bblayers.conf` fails loudly at
  parse time instead of an obscure missing-recipe error.
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

**Reviewer question, resolved in this round:** the four RZ/V2N-family
machine confs' own `ALP_ENABLE_DRPAI`-gated `IMAGE_INSTALL:append`
(`lib-tvm kernel-module-mmngr`) overlapped `dev`'s existing
`ALP_RZ_DRPAI_INSTALL` in `alp-image-common.inc` (#1176) — both installed
the same `lib-tvm kernel-module-mmngr` pair on every `alp-image-*` image
(the three recipes that `require alp-image-common.inc`), one
unconditionally there and one opt-in. `bitbake` dedupes the repeated
package names, so it was never a build break, but it was also never a
drift guard for `alp-image-*` builds: `ALP_ENABLE_DRPAI = "0"` still left
the pair installed via `alp-image-common.inc` whenever the layer +
MACHINE_FEATURES matched, so the two mechanisms couldn't drift relative to
each other there and the four appends bought nothing. Removed; the
corresponding comments in each machine conf and in `alp-image-edge.bb` are
corrected to name `alp-image-common.inc` as the single packaging authority
for `alp-image-*` builds.

**Behavioural change, stated honestly, not glossed over:**
`alp-image-common.inc` is required only by `alp-image-base`/`-edge`/`-prod`
(the three `alp-image-*` recipes), whereas the removed machine-conf
`IMAGE_INSTALL:append` reached EVERY image built for those four MACHINEs --
including a bare `core-image-*` or any other non-`alp-image-*` recipe. A
non-`alp-image-*` build that relied on `ALP_ENABLE_DRPAI = "1"` alone to
pull in `lib-tvm` + `kernel-module-mmngr` must now add that pair itself
(its own `IMAGE_INSTALL`), or rely on the vendor layer's own `core-image`
bbappend for it -- no such bbappend exists anywhere in this tree today, so
there is currently no fallback install path for a non-alp image here.

**Reviewer finding, resolved in this round:** the example's README and
`main.c` taught `python3 -m alp_model build --target drpai --product V2N
<model.onnx>` as the way to produce the `drpai_dir` bundle `argv[1]` loads,
but `scripts/alp_model` has no `__main__`, so the command fails outright —
and even a working invocation could not compile this model, since the
adapter's calibration path rejects a `1,3,640,640` detector shape. The dead
command is dropped everywhere (README.md, `main.c`'s teaching comment and
`usage()` string) and replaced with an honest statement: there is no
supported path in this SDK today to produce this bundle; producing one is
tracked in [alplabai/alp-sdk#2236](https://github.com/alplabai/alp-sdk/issues/2236);
until then the bundle has to be compiled outside the SDK, directly with the
Renesas DRP-AI TVM (RUHMI) toolchain, and tarred the same way
`adapters/drpai.py` would.

`docs/board-config-schema.md`'s example preset count is corrected to match:
after merging `dev` (which added its own new examples in parallel), 101
examples target a preset today (76 on `e1m-evk`, 25 on `e1m-x-evk`), counted
directly off `examples/**/board.yaml` post-merge, not carried over from
either side's pre-merge count.
