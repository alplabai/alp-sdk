# Unified Linux build management for alp-sdk

**Status:** Design approved 2026-05-25 (brainstorm). Awaiting spec review → implementation plan.
**Branch context:** raised on `feat/e1m-x-v2n-carrier-bringup`; this design is broader than the carrier bring-up and may warrant its own branch at implementation time.
**Affects:** `docs/build-yocto-v2n.md`, `kas/e1m-v2n.yml`, `meta-alp-sdk/README.md` (BSP-version reconciliation), `scripts/alp_cli/`, `scripts/alp_orchestrate.py`.

## 1. Problem

alp-sdk is Zephyr/west-first; the Linux/Yocto side is more built-out than "forgotten" — `meta-alp-sdk` ships `libalp_sdk.so` (the portable `<alp/*>` API on A55 Linux), `alp-chips`, heterogeneous IPC (`alp-remoteproc` + `alp-dts-reservations`), a reference image (`alp-image-edge`: ROS 2 + DEEPX + Mender OTA), machines for V2N/V2M/i.MX93, and per-MACHINE inference-runtime wiring — and `alp_orchestrate.py` already resolves `board.yaml` into per-core slices and emits per-MACHINE bitbake invocations. But three gaps put it below the Variscite/Toradex "one proven, turnkey path" norm:

1. **Unproven.** `meta-alp-sdk/README.md` is self-described `[UNTESTED]` — "v0.6 paper-correct, no full image bake executed."
2. **Inconsistent build narratives.** The README targets **AI SDK 7.10 / Scarthgap 5.0.11 / `bitbake-layers` / free download**; this branch's `build-yocto-v2n.md` + bbappends target **AI SDK 6.30 / linux-renesas 6.1-cip43 / kas / gated**. They disagree on version, Yocto release, orchestration tool, and gating. `kas/e1m-v2n.yml` is effectively a third orchestration layer.
3. **Not unified / not easy.** No single front door; a customer faces kas-vs-bitbake-layers-vs-west and per-vendor BSP mechanics.

## 2. Goals

- **Primary — converge + prove one path:** one canonical, documented, CI/HiL-validated Linux build; resolve the README↔build-doc contradiction.
- **Secondary — easy + unified:** one command, driven by `board.yaml`, that builds any supported SoM and both the Zephyr (M-core) and Linux (A-core) sides — the customer never touches kas/bitbake-layers/TEMPLATECONF/west.

## 3. Decisions (locked in brainstorm)

- **Canonical BSP = AI SDK 7.10 / Scarthgap 5.0.11** (the README's target; free download, package `RTK0EF0045Z94001AZJ-v1.0.3`). The 6.30 / 6.1-cip43 target is the bench's *old* target and is retired (no legacy-compat — there are no active customers): `build-yocto-v2n.md` is rewritten to 7.10, `kas/e1m-v2n.yml` is dropped, and the bbappend SRCREVs + DT patches retarget to 7.10's kernel.
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
1. Reconcile the narrative → 7.10 canonical: make `meta-alp-sdk/README.md`'s 7.10 `bitbake-layers` flow the single source of truth — validating/correcting it as the bake reveals issues (it is itself currently `[UNTESTED]`) — fold/retire `build-yocto-v2n.md` into it, and drop `kas/e1m-v2n.yml`.
2. Retarget kernel artifacts to 7.10: regenerate DT patches 0006–0013 against 7.10's rzv2n kernel (deltas carry; line-context + SRCREV change); retarget the `linux-renesas` bbappend SRCREV + the `tas2563-audio.cfg` wiring.
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

- **7.10's exact kernel version** (for the DT-patch regen) is unknown until the BSP is in hand (bench) — the patch deltas carry, but line-context + SRCREV must be re-derived.
- **V2N silicon support** may live only in the 7.10 tarball, not upstream `meta-renesas` (per the README) — the provider must use the tarball, not a git pin, for `meta-renesas`.
- **TEMPLATECONF replication risk** — the provider *applies* Renesas's TEMPLATECONF (vlp-v4-conf) rather than re-encoding it, to stay robust across BSP updates.
