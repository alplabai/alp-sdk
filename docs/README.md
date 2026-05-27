@page docs_index Documentation index

# alp-sdk documentation index

Navigation hub for everything under `docs/`.  Start here, then drill
into the topic-specific docs.

## Start here

- [getting-started.md](getting-started.md) — install + first build,
  cross-platform.
- [cross-platform-setup.md](cross-platform-setup.md) — per-OS
  quickstart (Linux + macOS + Windows native + WSL2).  *NEW —
  Codified in [ADR 0012](adr/0012-cross-platform-developer-host.md).*
- [firmware-quickstart.md](firmware-quickstart.md) — minimal "what
  does a board.yaml + main.c look like" walkthrough.
- [troubleshooting.md](troubleshooting.md) — common failure modes
  + fixes.

## Portability — the SDK's load-bearing customer promise

- [portability.md](portability.md) — **cookbook**.  How to swap
  SoMs within a family with no source change.  Worked examples,
  capability validation, the dual-namespace decision.
- [portability-matrix.md](portability-matrix.md) — the empirical
  guarantee: 21/21 E1M + 12/12 E1M-X cells green, all Phase B
  gaps resolved.
- [ADR 0011 — intra-family portability](adr/0011-intra-family-portability.md)
  — architectural decision: portability is INTRA-family;
  cross-form-factor is intentionally a separate product-line choice.
- [porting-new-som.md](porting-new-som.md) — 30-minute guide to
  adding a new SKU.

## Architecture & design

- [architecture.md](architecture.md) — SDK overview, repository
  layout, per-core slice fan-out, sparse capabilities flow,
  on_module: auto-enable, generators inventory.
- [board-config.md](board-config.md) — `board.yaml` v2 reference.
- [e1m-pinout.md](e1m-pinout.md) — E1M form-factor pinout reference.
- [board-id.md](board-id.md) — boot-time board identification:
  SoM EEPROM manifest + BOARD_ID ADC cross-check.
- [glossary.md](glossary.md) — terms.
- [adr/README.md](adr/README.md) — Architecture Decision Records
  index (12 ADRs as of 2026-05-18).

## Per-SoM bring-up

- [bring-up-aen.md](bring-up-aen.md) — Alif Ensemble family.
- [bring-up-v2n.md](bring-up-v2n.md) — Renesas RZ/V2N.
- [bring-up-v2n-m1.md](bring-up-v2n-m1.md) — V2N + DEEPX.
- [bring-up-imx93.md](bring-up-imx93.md) — NXP i.MX 93.
- [e1m-x-v2n-sdk-integration.md](e1m-x-v2n-sdk-integration.md) —
  landing the bench-validated V2N-M1 / E1M-X-EVK carrier bring-up
  into alp-sdk as the single source of truth.
- [errata-e1m-x-v2n.md](errata-e1m-x-v2n.md) — hardware findings
  from E1M-X-EVK + V2N-M1 bench bring-up (with software workarounds).

## Build & integration

- [heterogeneous-builds.md](heterogeneous-builds.md) —
  multi-core / multi-OS build orchestration.
- [gd32-bridge.md](gd32-bridge.md) +
  [gd32-bridge-protocol.md](gd32-bridge-protocol.md) — V2N's
  on-module IO MCU.
- [cc3501e-bridge.md](cc3501e-bridge.md) — AEN's on-module Wi-Fi
  coprocessor.
- [cc3501e-integration-plan.md](cc3501e-integration-plan.md) —
  SWRU626 deep-dive informing the CC3501E host + firmware wire
  protocol (research-only).
- [build-yocto-v2n.md](build-yocto-v2n.md) — building + deploying
  the V2N Linux kernel + rootfs (Yocto) for E1M-V2N101/102.
- [os-support-matrix.md](os-support-matrix.md) — which OS runs
  on which core, per SoM.
- [recommended-libraries.md](recommended-libraries.md) — curated
  third-party libraries (integrated / recommended / deferred) for
  what the SDK deliberately leaves out of `<alp/...>`.

## Security & release

- [secure-boot.md](secure-boot.md) — MCUboot + OPTIGA flow.
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

- [test-plan.md](test-plan.md) — authoritative status of every
  ABI claim.
- [testing.md](testing.md) — test harness overview.
- [test-coverage-audit.md](test-coverage-audit.md) — gap audit.
- [verification-status.md](verification-status.md) — per-feature
  verification matrix.
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

- [v1.0-readiness.md](v1.0-readiness.md) — living checklist of
  everything between today and the v1.0.0 tag.
- [v0.6-tbd-and-assumptions.md](v0.6-tbd-and-assumptions.md) —
  in-flight v0.6 caveats.
- [vendor-partnerships.md](vendor-partnerships.md) — tracker for
  the vendor relationships gating Pillar 9 (ecosystem) of v1.0.

## ABI

- [abi-markers.md](abi-markers.md) — per-header ABI status.
- [abi/](abi/) — point-in-time ABI snapshots.
