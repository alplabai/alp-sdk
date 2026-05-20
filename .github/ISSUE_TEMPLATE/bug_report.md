---
name: Bug Report
about: Report a bug in ALP SDK or E1M modules
title: "[BUG] "
labels: bug
assignees: ''
---

## Description
Brief description of the bug.

## Steps to Reproduce
1. 
2. 
3. 

## Expected Behavior
What should happen.

## Actual Behavior
What actually happens.

## Environment
- **SoM SKU:** (e.g. `E1M-AEN701`, `E1M-V2N101`, `E1M-V2M102`, or `host` for native_sim builds)
- **SoC variant:** (e.g. `alif:ensemble:e7`, `renesas:rzv2n:n44` — the `ref` from the `metadata/socs/` JSON)
- **Board board:** (E1M EVK / custom board — paste the board ID)
- **SDK version / commit:** (`git describe --tags --always`)
- **OS / backend:** (Bare-metal / Zephyr / Yocto Linux)
- **Zephyr version:** (`west list zephyr`, if applicable)
- **Toolchain:** (e.g. zephyr-sdk 0.16.5, ARM GNU Toolchain 13.x, host gcc)
- **Build invocation:** (`west build` / `cmake -B build` / twister command)

## Logs / artefacts
- Twister `build.log` and `handler.log` if applicable.
- Serial capture for HW-in-loop bugs.
- `metadata/socs/.../<part>.json` snippet if the bug is metadata-shaped.
