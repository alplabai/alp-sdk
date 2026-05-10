# Overnight summary — 2026-05-11

## Commits landed

- `20cc1d4` feat(inference): DEEPX DX-M1 backend hook + Yocto inference dispatcher
- `a61e4a2` feat(inference): Ethos-U65 on i.MX 93 wired alongside Ethos-U55 (AEN)
- `e4d5d17` feat(bench): scaffolding for v1.0 microbench suite
- `19278c1` feat(fuzz): scaffolding for v1.0 libFuzzer harness suite
- `bdc1a8e` ci: Coverity workflow stub + ABI freeze gate vs tagged v0.1
- `c327e24` feat(libs): LwRB + nanopb integration anchors for v0.4 prep

Closes tasks #13 (DEEPX DX-M1), #14 (i.MX 93 Ethos-U65), #15 (v1.0 hardening prep), #18 (LwRB + nanopb integration pass).

## Open questions for review

- **Coverity secrets.**  `.github/workflows/coverity.yml` no-ops until `COVERITY_TOKEN` and `COVERITY_EMAIL` are provisioned at *Settings -> Secrets and variables -> Actions*.  Drop them in and the weekly cron starts publishing to <https://scan.coverity.com/projects/alplabai-alp-sdk>.
- **west.yml pins for the new libs.**  v0.4 needs concrete revisions for `MaJerle/lwrb` (recommend `v3.2.0`) and `nanopb/nanopb` (recommend `0.4.9`).  Both directories ship stub headers today so the SDK compiles without them; the real link only fires when `CONFIG_ALP_SDK_USE_LWRB=y` / `CONFIG_ALP_SDK_USE_NANOPB=y` AND the modules are on the include path.
- **DEEPX SDK provenance.**  `vendors/deepx-dxm1/README.md` documents two install paths (DEEPX developer portal direct, or `meta-deepx` Yocto layer via `deepx-dxm1-host-sdk` recipe).  Pick one for the v0.4 bring-up plan; the SDK is proprietary and not redistributable.
- **i.MX 93 SoC variant.**  Per the memory note "pending exact hardware configurations," the production E1M-i.MX93 SoM variant (9301 / 9302 / 9311 / 9312 / 9352) is still TBD.  Affects the `metadata/socs/nxp/imx9/imx93.json` flesh-out and the i.MX 93 Ethos-U65 driver attach in `src/zephyr/inference_ethosu_n93.c` (only the variant-name string today).
- **alp_last_error unification.**  `src/yocto/inference_yocto.c` has a TODO(v0.4) to fold its last-error stamping into the same static that `src/common/stub_backend.c` reads.  Today the yocto inference dispatcher and the peripheral stubs have separate static slots; `alp_last_error()` returns whichever wrote most recently.  Not a v0.3 blocker but worth landing alongside the v0.4 Yocto implementation pass.
- **EVK wiring gaps.**  Grep'd `include/alp/boards/alp_e1m_evk.h` for "TBD" -- zero matches.  No hand-fleshing applicable from inference work; the file is already authoritative.
- **Schematic PDF.**  Untouched.  `pdftoppm` not installed and the PDF lives outside the repo; parked per overnight constraint.  Any pin-level question that needs the schematic is deferred until you confirm.

## CI status

- main @ `c327e24`: **green** -- pr-static-analysis, pr-plain-cmake, pr-generated-files, pr-metadata-validate, pr-twister all completed successfully.

## Files / paths added overnight

```
.github/workflows/coverity.yml                         (new)
.github/workflows/pr-generated-files.yml               (extended -- ABI freeze gate)
bench/                                                 (new -- 6 files)
fuzz/                                                  (new -- 4 files + 2 corpus seeds)
metadata/protos/alp_mproc.proto                        (new)
src/yocto/inference_yocto.c                            (new -- Yocto dispatcher)
src/yocto/inference_deepx.cpp                          (new)
src/zephyr/inference_ethosu_n93.c                      (new)
vendors/deepx-dxm1/                                    (new -- 3 files)
vendors/lwrb/                                          (new -- 3 files)
vendors/nanopb/                                        (new -- 6 files)
```

Public `<alp/...>` headers untouched -- no ABI delta against v0.1.
