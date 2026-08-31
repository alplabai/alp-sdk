@page docs_index Documentation index

# alp-sdk documentation index

Navigation hub for everything under `docs/`.  Start here, then drill
into the topic-specific docs.

## Start here

- [getting-started.md](getting-started.md) — install + first build,
  cross-platform.
- [cross-platform-setup.md](cross-platform-setup.md) — per-OS
  quickstart (Linux + macOS + Windows native + WSL2; macOS
  real-silicon Zephyr builds and `tan build` are Apple Silicon
  only, `native_sim` works on Intel too).  *NEW — Codified in
  [ADR 0012](adr/0012-cross-platform-developer-host.md).*
- [cli.md](cli.md) — the `tan` CLI verb reference
  (init / build / flash / generate / validate / doctor / size /
  image / clean) + when to use `tan` vs `west alp-*`.
- [firmware-quickstart.md](firmware-quickstart.md) — minimal "what
  does a board.yaml + main.c look like" walkthrough.
- [troubleshooting.md](troubleshooting.md) — common failure modes
  + fixes.

## Portability — the SDK's load-bearing customer promise

- [portability.md](portability.md) — **cookbook**.  How to swap
  SoMs within a family with no source change.  Worked examples,
  capability validation, the dual-namespace decision.
- [portability-matrix.md](portability-matrix.md) — the generated
  swap-test matrix for the pinned E1M and E1M-X examples, including
  any cells that fail because an example does not claim a compatible
  board/pinout path.
- [peripheral-support-matrix.md](peripheral-support-matrix.md) —
  auto-generated SoM × peripheral-class presence matrix, projected
  from the single-source SoC metadata (presence only; driver
  maturity lives in the OS support matrix).
- [ADR 0011 — intra-family portability](adr/0011-intra-family-portability.md)
  — architectural decision: portability is INTRA-family;
  cross-form-factor is intentionally a separate product-line choice.
- [porting-new-som.md](porting-new-som.md) — 30-minute guide to
  adding a new SKU.

## Architecture & design

- [architecture.md](architecture.md) — SDK overview, repository
  layout, per-core slice fan-out, sparse capabilities flow,
  on_module: auto-enable, generators inventory.
- [board-config.md](board-config.md) — `board.yaml` v2 reference
  (landing page: quick start, single-source-of-truth model, file
  location, cross-field validation, versioning).
- [board-config-schema.md](board-config-schema.md) — `board.yaml`
  field-by-field schema reference.
- [board-config-emit.md](board-config-emit.md) — how the loader
  compiles `board.yaml` into per-backend build artefacts.
- [board-config-hardware.md](board-config-hardware.md) — hardware
  revision tracking + modular SoM chip populations.
- [board-config-features.md](board-config-features.md) —
  `board.yaml` build-system integration knobs (`boot:`, `ota:`,
  `storage:`, `security.psa:`, ...).
- [e1m-pinout.md](e1m-pinout.md) — E1M form-factor pinout reference.
- [board-id.md](board-id.md) — boot-time board identification: the
  on-module EEPROM manifest is the sole authoritative source of SoM
  hardware revision (no SoM-side ADC cross-check); a carrier board's
  own BOARD_ID resistor-divider ADC, where wired, is a separate,
  independent board-side revision path.
- [aen-accelerator-backends-design.md](aen-accelerator-backends-design.md)
  — integration design for the Alif Ensemble accelerator surfaces
  (GPU2D / VeriSilicon ISP Pico (vsi,isp-pico) / SecAES / aiPM), each
  silicon-gated; GPU2D and SecAES are also HAL-pack gated, ISP Pico's
  pack has already landed and is blocked by other reasons (see that
  doc's *Silicon scope — which E-part has what* section).
- [glossary.md](glossary.md) — terms.
- [adr/README.md](adr/README.md) — Architecture Decision Records
  index (31 ADRs; recount with `ls docs/adr/[0-9]*.md | wc -l`).

## Per-SoM bring-up

- [bring-up-aen.md](bring-up-aen.md) — Alif Ensemble family.
- [aen-bench-bringup.md](aen-bench-bringup.md) — E1M-AEN801 (E8)
  on-silicon bench bring-up: Flow A/C/D flashing, the validated
  peripheral matrix, and the NPU-from-MRAM path.
- [aen-provisioning.md](aen-provisioning.md) — provisioning an
  E1M-AEN SoM (SES → MCUboot → slot0 chain, bench-proven at
  `0da1f1b4`, + the SoM-maker policy).
- [aen-se-services.md](aen-se-services.md) — runtime `se_service_*`
  API (device/LCS/power queries + the gated DVFS / STOC-update path).
- [debugging-aen.md](debugging-aen.md) — attaching a debugger to
  E1M-AEN801: `west debug`, the generic-vs-Alif-part J-Link device
  trap, what J-Link is (and isn't) for, and the idle-core DAP-gating
  symptom + recovery.
- [bring-up-v2n.md](bring-up-v2n.md) — Renesas RZ/V2N.
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — V2N + DEEPX.
- [bring-up-drpai-v2n.md](bring-up-drpai-v2n.md) — the RZ/V2N on-die
  DRP-AI3 NPU: host toolchain, the DT override the driver needs,
  image wiring, model compile and microSD deploy. Kernel driver
  proven on silicon; userspace packaging is written but has never been
  baked, no model compiled and no inference run yet.
- [bring-up-imx93.md](bring-up-imx93.md) — NXP i.MX 93.
- [e1m-x-v2n-sdk-integration.md](e1m-x-v2n-sdk-integration.md) —
  landing the bench-validated V2N-M1 / E1M-X-EVK carrier bring-up
  into alp-sdk as the single source of truth.
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — hardware findings
  from E1M-X-EVK + V2N-M1 bench bring-up (with software workarounds).
- [rzv2n-m33-swd-debug.md](rzv2n-m33-swd-debug.md) — attaching to
  the V2N CM33 over the DAP (J-Link), status-block reads, gotchas.
- [rzv2n-m33-secure-boot.md](rzv2n-m33-secure-boot.md) — the CM33
  image's place in the V2N secure-boot chain.

## Build & integration

- [heterogeneous-builds.md](heterogeneous-builds.md) —
  multi-core / multi-OS build orchestration.
- [gd32-bridge.md](gd32-bridge.md) +
  [gd32-bridge-protocol.md](gd32-bridge-protocol.md) — V2N's
  on-module IO MCU.
- [gd32-link-sci7-next-rev.md](gd32-link-sci7-next-rev.md) —
  SCI7 is the bridge link's permanent transport (RSPI reroute
  rejected); hardening plan: DMAC data path, sequence echo, GD32
  ADC/rising-path latency.
- [cc3501e-bridge.md](cc3501e-bridge.md) — AEN's on-module Wi-Fi
  coprocessor.
- [cc3501e-production.md](cc3501e-production.md) — building,
  signing, and provisioning a shippable CC3501E coprocessor image.
- [cc3501e-gpio-bench.md](cc3501e-gpio-bench.md) — warm-boot bench
  validation of the CC3501E GPIO proxy (machine-checkable contract).
- [cc3501e-companion-commands.md](cc3501e-companion-commands.md) —
  reference for the `alp companion` console command surface
  (wifi / ble / sock / diag) + the host-driver OTA + GPIO-proxy APIs.
- [console.md](console.md) — the interactive `alp` command tree on the
  Zephyr shell (safety tiers, command list, companion binding, banner).
- [build-yocto-v2n.md](build-yocto-v2n.md) — building + deploying
  the V2N Linux kernel + rootfs (Yocto) for E1M-V2N101/102.
- [provisioning.md](provisioning.md) — provisioning a SoM from a
  versioned release bundle (the `provision_som.py` orchestrator + runbook).
- [os-support-matrix.md](os-support-matrix.md) — which OS runs
  on which core, per SoM.
- [recommended-libraries.md](recommended-libraries.md) — curated
  third-party libraries (integrated / recommended / deferred) for
  what the SDK deliberately leaves out of `<alp/...>`.
- [bench/model-perf-capture.md](bench/model-perf-capture.md) — recipe
  for a tier-2 bench-measured model-perf point
  (`metadata/model_perf/<SKU>/<hash>.yaml`); the contract ships in
  #1520, `metadata/model_perf/` stays empty until a real capture runs.

## Security & release

- [secure-boot.md](secure-boot.md) — MCUboot + OPTIGA flow; the
  MCUboot ECDSA-P256 chain is bench-proven on E1M-AEN801.
- [som-release-signing.md](som-release-signing.md) — verifying SoM-release
  bundle provenance (ECDSA-P256; a separate concern from device secure-boot).
- [threat-model.md](threat-model.md) — adversary classes, asset
  classes, per-header threat catalogue.
- [security-advisories.md](security-advisories.md),
  [security-audit-plan.md](security-audit-plan.md) — coordinated
  disclosure + external audit engagement.
- [ota.md](ota.md), [ota-device-contract.md](ota-device-contract.md)
  — Mender-based OTA.
- [release-policy.md](release-policy.md) — versioning + ABI
  policy.
- [branching-and-merge-policy.md](branching-and-merge-policy.md) —
  branch / PR / merge / push / tag rules (pairs with
  release-policy).
- [zephyr-version-policy.md](zephyr-version-policy.md) — Zephyr
  LTS pin.

## Contributing

- [contribution.md](contribution.md) — canonical contributor
  guide: reporting bugs, code style, PR flow.
- [contributing-tier-2.md](contributing-tier-2.md) — contributing
  chip drivers / libraries to the Tier-2 `alp-sdk-community` repo.

## Testing & verification

- [test-plan.md](test-plan.md) — the SDK's primary verification
  ledger; every claimed feature's silicon-evidence status lives
  here, hand-maintained.  Three other hand-maintained views of "is X
  verified" exist -- [os-support-matrix.md](os-support-matrix.md)'s
  GA labels, `metadata/chips/<name>.yaml` `verification:` blocks,
  and `@par Verification status` Doxygen tags on public headers --
  none is generated from this file or gated against it, so any can
  still disagree with it; this ledger wins when they do.
- [testing.md](testing.md) — test harness overview.
- [test-coverage-audit.md](test-coverage-audit.md) — gap audit.
- [verification-status.md](verification-status.md) — per-feature
  verification matrix, GENERATED from test-plan.md
  (`scripts/gen_verification_status.py`); do not hand-edit.
- [local-ci.md](local-ci.md) — running CI checks locally.

## Tutorials

- [tutorials/](tutorials/) — 16 walkthroughs covering first build,
  peripherals, security, OTA, inference.  Tutorial-04 covers
  intra-family portability with the swap-test recipe.

## Reference apps

- See [examples/README.md](../examples/README.md) — 50+ example
  apps, with the canonical portable trio (`i2c-scanner`,
  `gpio-button-led`, `pwm-led-fade`) + 8 vendor-SDK-style tutorial
  examples added 2026-05-18 (`hello-world`, `uart-hello-world`,
  `i2c-master`, `i2c-slave`, `spi-master`, `spi-slave`,
  `dac-waveform`, `timer-periodic-interrupt`).

## V1.0 readiness

- [v1.0-readiness.md](v1.0-readiness.md) — a 2026-05-14 execution-plan
  snapshot toward the v1.0.0 tag.  Not maintained current past the
  session that wrote it (the SDK has since shipped through v0.16.0) —
  cross-check any status claim against `VERSIONS.md` and
  `CHANGELOG.md`, which are.
- [v0.6-tbd-and-assumptions.md](v0.6-tbd-and-assumptions.md) —
  historical: the TBDs + implementation-assumptions log from the
  v0.6 heterogeneous-OS-orchestration wave (v0.6.0 released
  2026-06-06), kept for context on the calls made at the time, not
  as a current tracker.
- [vendor-partnerships.md](vendor-partnerships.md) — tracker for
  the vendor relationships gating Pillar 9 (ecosystem) of v1.0.

## ABI

- [abi-markers.md](abi-markers.md) — per-header ABI status.
- [abi/](abi/) — point-in-time ABI snapshots.
