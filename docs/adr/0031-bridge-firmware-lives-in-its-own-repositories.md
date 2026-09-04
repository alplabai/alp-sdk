# 0031. Bridge firmware lives in its own repositories

Status: Accepted
Date: 2026-08-28
Deciders: alpCaner

Supersedes [0015](0015-cc3501e-firmware-embedded.md) ("CC3501E bridge firmware
is embedded in alp-sdk").

## Context

ADR 0015 decided, in June 2026, that the CC3501E bridge firmware should live
inside alp-sdk. Its reasoning was sound for the state of the project then, and
two of its three pillars have since stopped holding:

- **"Single-source protocol + atomic cross-side changes."** This was the strong
  argument, and it is *preserved*, not abandoned — see the Decision. The
  firmware still compiles the canonical `<alp/protocol/cc3501e.h>` from an
  alp-sdk checkout rather than a mirrored copy, so the wire contract still
  cannot fork.
- **"99% never rebuild it."** Already amended in 0015 itself (2026-08-27) for
  naming a `firmware/gd32-bridge/prebuilt/` path that never existed. The
  premise also aged badly in practice: this firmware was rebuilt and reflashed
  many times over the 2026-08 bring-up.
- **"Keeping it here costs little."** It costs a public repo that ships a
  binary customers flash, with **no CI of its own, no security policy, and no
  branch protection** — because those all lived at alp-sdk's granularity and
  none of them covered the firmware trees specifically. The firmware also
  carried a build that reached two directories up for its headers, which works
  only while it is nested.

Two concrete failures made the cost visible rather than theoretical:

- `SECURITY.md` declared `firmware/cc3501e/` and `firmware/gd32-bridge/` in
  scope for vulnerability reports at alp-sdk's granularity. Once the trees
  moved, that line sent reporters to the wrong repository.
- A fuzz harness built against `firmware/gd32-bridge/` sources silently stopped
  linking, and nothing noticed, because no CI built the firmware at all.

## Decision

The two bridge firmwares live in their own repositories:

- `alplabai/cc3501e-bridge-firmware`
- `alplabai/gd32-bridge-firmware`

Each is public, each carries its own CI, `SECURITY.md`, `CONTRIBUTING.md`,
`CODEOWNERS` and protected `main`.

**The wire contract stays single-sourced.** This is the part of 0015 that was
right and is deliberately kept: neither firmware mirrors the protocol header.
Each compiles alp-sdk's canonical header via an explicit `ALP_SDK_ROOT`, and CI
in the firmware repo fails when the two drift — cc3501e compares
`protocol-version.txt` against `ALP_CC3501E_PROTOCOL_VERSION`, and gd32
regenerates `tests/protocol_vectors.txt` and diffs it. A split without those
gates would have traded 0015's real benefit for tidiness, which is not a trade
worth making.

**alp-sdk keeps the host halves** (`chips/cc3501e/`, `chips/gd32g553/`), the
metadata that pins a companion image, and the `board.yaml`-emitted
configuration. Security scope follows the artifact: a defect in how the host
*parses* a reply is alp-sdk's; the same defect in how the companion *builds* it
belongs to the firmware repo.

## Consequences

- A wire change is now two PRs in two repositories, and the firmware CI is what
  keeps them honest. `CONTRIBUTING.md` in each firmware repo says so explicitly,
  because "CI here is green" no longer means the contract is whole.
- `firmware/cc3501e/` and `firmware/gd32-bridge/` still exist in alp-sdk at the
  time of writing. The deletion is sequenced separately (#1370): west projects,
  the fuzz harness's `ALP_GD32_BRIDGE_FIRMWARE_DIR`, metadata `helper_firmware`
  pins, and ~16 doc links all have to move first. **This ADR records the
  decision, not the completion.**
- The firmware build scripts no longer guess their dependency's location. Both
  take an explicit root (`-AlpSdkRoot` / `ALP_SDK_ROOT` / `-DALP_SDK_ROOT`) and
  refuse loudly when the header is absent — the relative `../..` guess was
  correct only while nested, and after extraction it silently resolved to
  whatever sibling checkout happened to be present, on whatever branch.

## Alternatives considered

- **Keep both trees in alp-sdk and add firmware-specific CI here.** Workable,
  and it would have fixed the fuzz-harness gap. It does not fix the artifact
  boundary: a customer who wants the companion source still clones the whole
  SDK, and the security scope stays ambiguous because one policy covers two
  very different trust surfaces.
- **Vendor the protocol header into each firmware repo.** Would make each repo
  self-contained and is the obvious way to split. Rejected: it re-creates
  exactly the forkable wire contract 0015 existed to prevent, and the drift
  would be silent — a mirrored header that is merely *stale* still compiles.
