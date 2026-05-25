# Unified Linux build management for alp-sdk

**Status:** Design approved 2026-05-25 (brainstorm). Awaiting spec review → implementation plan.
**Branch context:** raised on `feat/e1m-x-v2n-carrier-bringup`; this design is broader than the carrier bring-up and may warrant its own branch at implementation time.
**Affects:** `docs/build-yocto-v2n.md`, `kas/e1m-v2n.yml`, `meta-alp-sdk/README.md` (BSP-version reconciliation), `scripts/alp_cli/`, `scripts/alp_orchestrate.py`.

## 1. Problem

alp-sdk is Zephyr/west-first; the Linux/Yocto side is more built-out than "forgotten" — `meta-alp-sdk` ships `libalp_sdk.so` (the portable `<alp/*>` API on A55 Linux), `alp-chips`, heterogeneous IPC (`alp-remoteproc` + `alp-dts-reservations`), a reference image (`alp-image-edge`: ROS 2 + DEEPX + Mender OTA), machines for V2N/V2M/i.MX93, and per-MACHINE inference-runtime wiring — and `alp_orchestrate.py` already resolves `board.yaml` into per-core slices and emits per-MACHINE bitbake invocations. But three gaps put it below the Variscite/Toradex "one proven, turnkey path" norm:

1. **Unproven.** `meta-alp-sdk/README.md` is self-described `[UNTESTED]` — "v0.6 paper-correct, no full image bake executed."
2. **Doc version confusion (not a real conflict).** The README names **AI SDK 7.10** + `bitbake-layers`; the branch's `build-yocto-v2n.md` + bbappends name **BSP 6.3 (`RTK0EF0189F06300SJ` / linux-renesas 6.1-cip43)** + kas. These are **two version axes** — the AI-SDK *platform* (7.1) vs the Yocto *BSP* (6.3) — that each doc named in isolation, reading as a conflict. The fix is to state *both* everywhere; the orchestration choice (kas vs `bitbake-layers`) still needs converging.
3. **Not unified / not easy.** No single front door; a customer faces kas-vs-bitbake-layers-vs-west and per-vendor BSP mechanics.

## 2. Goals

- **Primary — converge + prove one path:** one canonical, documented, CI/HiL-validated Linux build; resolve the README↔build-doc contradiction.
- **Secondary — easy + unified:** one command, driven by `board.yaml`, that builds any supported SoM and both the Zephyr (M-core) and Linux (A-core) sides — the customer never touches kas/bitbake-layers/TEMPLATECONF/west.

## 3. Decisions (locked in brainstorm)

- **Canonical target = AI SDK platform 7.1 on BSP 6.3** (`RTK0EF0189F06300SJ`, linux-renesas 6.1-cip43, kernel SHA 6717c06c). These are two axes, **not** competing BSPs — 7.1 is the AI-SDK/platform umbrella, 6.3 is the Yocto BSP beneath it — so the branch's **original BSP target was already correct**, and its DT patches (generated at 6.3 / 6717c06c) **stay as-is, no regen**. Reconciliation = make the docs name *both* axes (platform 7.1 / BSP 6.3) consistently, and drop `kas/e1m-v2n.yml` in favour of the README's `bitbake-layers` flow (no legacy-compat — no active customers).
- **Approach = a unified `alp build` front-end with a per-vendor BSP-provider seam.** Chosen over kas-everywhere (fights Renesas's tarball/TEMPLATECONF model; unifies Linux only) and over raw bitbake-layers (not unified).
- **Phased:** prove the native 7.10 flow first (no new tooling), then wrap it.

## 4. Architecture

Unified front, vendor-specific back. `alp build` resolves `board.yaml` (reusing `alp_orchestrate.py`, which already maps `cortex-a→yocto` / `M→zephyr` and emits per-MACHINE invocations) into a build graph, then dispatches each core slice to the **west builder** (Zephyr/M) or a **BSP provider** (Yocto/A). Each SoC vendor's build mechanics are isolated behind the BSP-provider interface, so the customer-facing command is identical across SoMs.

```
board.yaml ──► alp_orchestrate.py (resolve) ──► build graph (per-core slices)
                                                      │
                                          ┌───────────┴───────────┐
                                   os: zephyr (M)          os: yocto (A)
                                        │                        │
                                   west builder            BSP provider (per family)
                                                                 │
                                       ┌─────────────────────────┼──────────────┐
                                 renesas_rzv2n              nxp_imx93        (future)
                                 (AI SDK 7.10 tarball,      (meta-imx +
                                  TEMPLATECONF/vlp-v4,       meta-freescale)
                                  bitbake-layers)
                                                       ──► bitbake alp-image-edge
```

- **BSP stays un-vendored** (gated/large): the provider expects the customer's downloaded BSP via `--bsp` / `ALP_RZV2N_BSP` / `~/.alp/bsp.toml`, validates presence + version, and on a miss prints exactly what to download + where to point it.
- **Reproducibility:** each provider reads a per-family manifest (`metadata/bsp/<family>.yaml`) pinning the BSP package ID + version, the canonical layer list + SHAs, the TEMPLATECONF path, and the MACHINE pattern — so an alp-sdk tag → a reproducible image, and updating a BSP edits YAML, not code.

## 5. Components (Phase 2, under `scripts/alp_cli/`)

Each unit is independently testable; only `bake()` needs real Linux + BSP.

- **`alp build` subcommand** — customer entry: `alp build [-C <project>] [--core ID] [--machine M]`. Resolves `board.yaml`, builds the dispatch graph, runs builders, prints artifact paths. *Depends on:* orchestrator + dispatcher.
- **Dispatcher** — pure routing: slice → builder by `slice.os`. *Depends on:* builders + provider registry. Testable with zero bitbake.
- **`BspProvider` interface + registry** — the vendor seam: `validate(bsp_root)`, `prepare(bsp_root, manifest) → BuildEnv`, `bake(env, machine, image) → Artifacts`. Registry maps SoM-preset `family:` → provider. A new SoC family = one new provider.
- **`RenesasRzv2nProvider`** — encodes the Phase-1-proven 7.10 flow (extract BSP, apply TEMPLATECONF, `bitbake-layers add-layer` the canonical set, `bitbake <image>`). Covers V2N; V2M adds `meta-deepx-m1`.
- **`NxpImx93Provider`** — meta-imx / meta-freescale flow for `e1m-nx9101`.
- **`ZephyrBuilder`** — wraps `west build` for M-core slices (orchestrator already emits the board/invocation).
- **BSP locator** — resolves + validates the external BSP; actionable missing-BSP error.
- **Reproducibility manifest** — `metadata/bsp/<family>.yaml` (data; the provider is a generic engine).

## 6. Data flow

```
alp build → load board.yaml → orchestrator.resolve() → slices[]
  for slice (os != off):
     zephyr → ZephyrBuilder.build(slice)              # west
     yocto  → provider = registry[som.family]
              bsp = locate(family); provider.validate(bsp)
              manifest = metadata/bsp/<family>.yaml
              env  = provider.prepare(bsp, manifest)
              arts = provider.bake(env, slice.machine, slice.image)
  → write system-manifest.yaml (orchestrator) → report artifact paths
```

## 7. Phasing

**Phase 1 — Prove one path (no new tooling).**
1. Reconcile docs: make the README's `bitbake-layers` flow the single source of truth — naming **both** axes (platform 7.1 / BSP 6.3) — fold/retire `build-yocto-v2n.md` into it, drop `kas/e1m-v2n.yml`. (The README is itself `[UNTESTED]`; validate/correct it as the bake reveals issues.)
2. Verify (don't regen) the DT artifacts: patches 0006–0013 are already at BSP 6.3 (6.1-cip43 / SHA 6717c06c), so just confirm they still apply against the downloaded 6.3 BSP's rzv2n kernel + the `linux-renesas` bbappend SRCREV matches.
3. Green bake: `MACHINE=e1m-v2n101-a55 bitbake alp-image-edge` on 7.10 (WSL + downloaded BSP); fix recipe/layer/bbappend breakage. Audio stays off (patch 0010) → still green.
- **Exit:** one documented command boots `alp-image-edge` on V2N101/7.10 (HiL-validated); the README↔doc contradiction is gone.

**Phase 2 — Easy + unified (`alp build`).**
1. `alp build` CLI + dispatcher (extends `alp_orchestrate.py`).
2. `renesas_rzv2n` provider — automates Phase 1's manual steps.
3. BSP locator + validation + the reproducibility manifest.
4. `nxp_imx93` provider + a green bake for `e1m-nx9101`.
5. Heterogeneous unification — one `alp build` drives M-core Zephyr (west) + A-core Yocto from one `board.yaml`.
- **Exit:** `alp build` (any supported SoM) builds Zephyr + Yocto from `board.yaml`; V2N + i.MX93 green; reproducible.

**Sequencing:** Phase 1 strictly precedes Phase 2 (prove, then automate). V2M (DEEPX) rides the `renesas_rzv2n` provider + `meta-deepx-m1` as a small Phase-2 add. Phase 1 needs the 7.10 BSP + WSL/bench.

## 8. Error handling — fail fast + actionable, never silent

- **BSP missing / wrong version** → fail before any bake; print what to download (`RTK0EF0045Z94001AZJ-v1.0.3.zip`, free signup), how to extract, where to point `ALP_RZV2N_BSP`.
- **Host not Linux** → Yocto needs Linux/WSL2 (Zephyr slices still build on Windows). **Host deps missing** → preflight names them.
- **bitbake / layer failure** → surface the exit + log path, never swallow; dispatcher reports which slice failed (partial success allowed: e.g. Zephyr built, Yocto failed).
- **Layer / TEMPLATECONF mismatch** → after `prepare()`, validate the manifest's canonical layer set is present; name any missing layer + its source.
- **Reproducibility drift** → BSP actual version/SHA ≠ manifest pin → warn (`--strict` → fail).
- **board.yaml errors** → existing `OrchestratorError`, surfaced cleanly (no stack dump).

## 9. Testing — layered to the isolation

1. **Unit (Windows/WSL, no BSP, no bake)** — the bulk, in the existing pytest suite: dispatcher routing (board.yaml fixtures → right builder/MACHINE), provider command-construction (mock subprocess → assert the `bitbake-layers`/`TEMPLATECONF`/`bitbake` invocations), locator resolution + the missing-BSP message, manifest parse/validate.
2. **Recipe-parse gate (Linux, no full bake)** — `bitbake-layers parse-recipes` / `bitbake -n` against the assembled layers; catches recipe/bbappend breakage in seconds, not a multi-GB bake.
3. **Integration / HiL (bench)** — the real `bitbake alp-image-edge` green bake + boot, on the `hil-yocto` runner (`docs/ci/HW-IN-LOOP.md`); per-SoM (V2N, then i.MX93).

Unit + parse-gate run everywhere cheaply; the expensive bake gates on HiL — matching the local-first split.

## 10. Scope / non-goals

**In scope:** the `alp build` front-end + BSP-provider seam; V2N (Phase 1) then i.MX93 + V2M (Phase 2); the 7.10 doc reconciliation; the reproducibility manifest.

**Non-goals (explicit):**
- The **Mender server** side (separate repo/owner) — device-side only.
- The **TAS2563 audio DT** patch — a separate bench task on the new PCB order (carrier bring-up workstream, not this spec).
- **Generating the Yocto machine confs + carrier DT from metadata** — they stay hand-authored in `meta-alp-sdk` (normal BSP practice); a metadata-driven Yocto-machine generator is a possible *future* spec, not this one.
- **AEN A32-class Linux** (deferred to v0.7) and the **Ubuntu backend** (deferred past v1.0).
- Re-architecting the existing `meta-alp-sdk` recipes beyond the 7.10 retarget.

## 11. Open questions / risks

- **Resolved:** the BSP is **6.3** (linux-renesas 6.1-cip43, SHA 6717c06c); the branch's DT patches are already against it → no regen, just verify they apply against the downloaded 6.3 BSP. (The AI-SDK *platform* is 7.1 — a separate axis.) Optional add-on packages (ISP/camera, ROS2, Multi-OS/OpenAMP) come as separate Renesas downloads layered on top.
- **V2N silicon support** may live only in the 7.10 tarball, not upstream `meta-renesas` (per the README) — the provider must use the tarball, not a git pin, for `meta-renesas`.
- **TEMPLATECONF replication risk** — the provider *applies* Renesas's TEMPLATECONF (vlp-v4-conf) rather than re-encoding it, to stay robust across BSP updates.
