# 0015. CC3501E bridge firmware is embedded in alp-sdk

Status: Accepted
Date: 2026-06-13

Supersedes the "separate `alplabai/cc3501e-firmware` repo" stance
previously described in `docs/cc3501e-bridge.md` ("Two-repo split") and
the original `firmware/cc3501e/README.md` bootstrap doc.

## Context

The E1M-AEN family carries a TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor.
The Alif host drives it over an inter-chip link (SPI default, SDIO
optional) using a small binary protocol; the firmware that runs on the
CC3501E is the slave-side parser fronting TI's SimpleLink Wi-Fi + BLE
stacks.

The scaffolding shipped earlier assumed that firmware would live in a
**separate** repository (`alplabai/cc3501e-firmware`), reasoning:
different vendor dependency (TI's SimpleLink SDK), different release
cadence, different audience (most consumers never rebuild it), different
toolchain (`ticlang`).

That reasoning does not survive contact with our own precedent.  The
[`gd32-bridge`](../../firmware/gd32-bridge/) firmware is the **same
class of artifact** -- an on-SoM helper-MCU bridge: custom binary
protocol over SPI/I2C, host driver in alp-sdk, prebuilt blob shipped in
alp-sdk, its own toolchain, its own version axis -- and it lives
**inside** alp-sdk at `firmware/gd32-bridge/`.  Every "separate"
argument is already answered there:

| "Separate" argument | What gd32-bridge does |
|---------------------|-----------------------|
| Different/heavy toolchain | ARM-GCC, built **outside** the Zephyr gate; the odd toolchain is a separate CI *job*, not a separate repo |
| Different release cadence | own `firmware-version.txt` + protocol version, released independently -- inside alp-sdk |
| Vendors a 3rd-party lib | already vendors the GigaDevice library; TI SimpleLink is BSD-3, same shape |
| 99% never rebuild it | true for gd32-bridge too; the prebuilt blob ships from `firmware/<bridge>/prebuilt/` in alp-sdk **regardless of where source lives** |

Separately, the firmware README cited "[ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md)'s
dual-use acid test" as the basis for a separate repo.  ADR 0005 governs
**alp-sdk vs alp-studio**, not firmware sub-repos.  Applied honestly --
*"would a hand-written-firmware author ever directly use this?"* -- the
answer is yes (rebuild is "optional and open ... public, like the GD32
bridge"), which resolves to **alp-sdk**.

## Decision

The CC3501E bridge firmware lives **in alp-sdk**, at
`firmware/cc3501e/`, exactly as `firmware/gd32-bridge/` does.

- The firmware `#include`s the canonical
  `include/alp/protocol/cc3501e.h` **directly** -- no mirrored header,
  no CI diff-check, no drift.  An opcode change moves the host driver,
  the firmware parser, and the wire-vector tests in **one commit**.
- The firmware build runs **outside** the Zephyr/twister gate.  A
  dedicated workflow (`.github/workflows/pr-cc3501e-bridge-build.yml`)
  builds the silicon-free `stub` backend; the production `ti` backend
  (TI `ticlang` + the SimpleLink CC33xx SDK as an optional submodule) is
  built on the bench and is never on the per-PR critical path.
- The prebuilt, signed binary ships from `firmware/cc3501e/prebuilt/`
  for consumers who only flash.

## Alternatives

**A. Separate `alplabai/cc3501e-firmware` repo.**  Rejected: every
rationale is a CI-*job* concern already solved by the gd32-bridge
precedent, and it forces the header-mirror-plus-diff-check dance --
duplicated truth, which the project's single-source principle treats as
a bug.  The empty scaffold repo is reduced to a pointer to alp-sdk.

**B. Link the firmware into the Zephyr build.**  Rejected: it is a
different target (CC3501E Cortex-M33) and toolchain; it must stay a
separate compile artifact, just inside the same repo (as gd32-bridge
is).

## Consequences

**Good:**
- Single-source wire protocol; atomic cross-side changes.
- One mental model for both on-SoM bridges (gd32-bridge, cc3501e-bridge).
- Consumers get firmware + protocol + host driver + wiring + docs in one
  clone; the prebuilt blob ships from alp-sdk either way.

**Bad / costs:**
- The TI SimpleLink SDK rides as an optional, non-default submodule
  under `firmware/cc3501e/vendor/` (not recursed by default; only the
  bench `ti` build needs it).
- alp-sdk gains a second non-Zephyr CI job (the cc3501e stub build),
  mirroring the gd32-bridge one.

## See also

- [`docs/cc3501e-bridge.md`](../cc3501e-bridge.md) -- bridge architecture
  + selectable transport + the (now embedded) firmware layout.
- [`firmware/cc3501e/README.md`](../../firmware/cc3501e/README.md),
  `firmware/cc3501e/DESIGN.md`.
- [ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md) -- the
  alp-sdk/alp-studio boundary (distinct from this firmware-home question).
