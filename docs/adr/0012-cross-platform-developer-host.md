# 0012. alp-sdk developer host is cross-platform; Linux required ONLY for Yocto

Status: Accepted
Date: 2026-05-18
Deciders: alpCaner

## Context

The SDK has two backend lines and they have wildly different
host-OS requirements:

- **Zephyr-on-M-class** (the entry-point line) — fully cross-platform
  by construction.  `west` is Python.  `cmake` + `ninja` are
  cross-platform.  The Arm GNU Toolchain ships for Win + Mac +
  Linux from arm.com.  `native_sim` builds use the host's own GCC
  / Clang.  Every script under `scripts/` that customers invoke
  (`validate_metadata.py`, `validate_board_yaml.py`,
  `alp_project.py`, `check_pin_conflicts.py`, …) is Python 3.10+
  and has no Linux-specific syscalls.
- **Yocto-on-A-class** (the heterogeneous-orchestration line, see
  [ADR 0010](0010-heterogeneous-os-orchestration.md)) — Linux-only
  by upstream constraint.  `bitbake` + OE-core + the per-vendor
  layers (`meta-renesas`, `meta-alif`, `meta-imx`,
  `meta-deepx-m1`) all require a Linux userland; the sstate / build
  pipeline assumes case-sensitive filesystems, ext4-class semantics,
  and POSIX-shell host tooling that Windows + macOS do not provide
  natively.

Most customers will start with Zephyr-on-M (lower entry barrier,
immediate hardware availability via the Alif EVK + the V2N EVK,
shorter iteration loop, no `bitbake` cold-build wait).  A
material fraction of those customers run Windows or macOS as
their primary development host.

If the SDK *appears* to require Linux for any workflow — even
just "you should be on Ubuntu to evaluate alp-sdk" prose in the
README — adoption drops.  The user (alpCaner, 2026-05-18) stated
this explicitly:

> "We will need Linux. Alp-sdk should make it development
> possible in cross-platform. Otherwise it will be difficult to
> convince users for Linux."

The constraint is honest: Linux *is* required for the Yocto
half.  But the Zephyr-on-M workflow is genuinely cross-platform
today and the SDK has so far failed to **say so explicitly** in
its docs, in CI, and in its repo lints.  That communication gap
is the gap this ADR closes.

Existing state on 2026-05-18:

- `docs/getting-started.md` already lists per-platform install
  one-liners for macOS / Linux / Windows / WSL2.
- Most Python scripts work cross-platform; tests (`tests/scripts/`)
  pass on Windows already (the user develops on Windows daily).
- `west build`, `cmake`, `ninja`, `arm-none-eabi-gcc` are all
  cross-platform.
- `scripts/bootstrap.sh` is `#!/usr/bin/env bash` — it works on
  Mac and on WSL but not on native PowerShell.  This is fine if
  the doc surrounding it makes clear it is one entry point among
  several, not the only one.
- The shipped CI (`.github/workflows/pr-twister.yml`,
  `pr-metadata-validate.yml`, etc.) runs Linux-only today.  This
  is a coverage gap — nothing in CI exercises the SDK on a Mac or
  Windows runner.
- Memory `[[ubuntu-deferred-to-v2]]` records that an Ubuntu /
  Linux *backend* (running Alp user-space binaries on Linux) is
  deferred indefinitely past v1.0.  That is orthogonal to this
  ADR, which is about the *host* developers use to build firmware,
  not about the runtime target.

## Decision

**Three load-bearing claims, made explicit:**

1. **The Zephyr-on-M-class developer workflow is first-class on
   Win + Mac + Linux.**  `board.yaml` → `alp.conf` →
   `west build` → `west flash` works end-to-end on all three host
   OSes with the same source tree, the same `west.yml`, the same
   metadata, and identical artefacts.  This is the load-bearing
   onboarding path for ~the entire E1M product line, the M-cluster
   half of every E1M-X SoM, and every standalone-firmware customer.
2. **The Yocto-on-A-class developer workflow remains Linux-only
   by upstream constraint.**  Win + Mac users targeting Yocto have
   two supported options: (a) WSL2 on Windows 10/11, (b) a Linux
   VM or container.  The SDK does not pretend the alternative
   (native Yocto on Windows / macOS) is on the table — bitbake
   simply does not run there.  This is honest scoping, not a
   limitation that the SDK can hide behind better tooling.
3. **CI runs at least one Win + one Mac runner for the
   Zephyr-on-M path.**  Linux remains the canonical baseline (the
   shipped Twister gate stays on `ubuntu-latest`), but Mac and
   Windows runners run a slimmer scaffold — metadata validation,
   the cross-platform lint, and a single-board `native_sim` build
   — so a Linux-only assumption that creeps into the SDK gets
   caught at PR time on the platform where it would actually
   break a customer.  Yocto CI (`pr-bitbake.yml`) stays
   Linux-only.

**Operational consequences of the three claims:**

- Every customer-facing doc, script, and tutorial avoids
  Linux-only idioms.  Concretely: no hard-coded `/dev/ttyUSB0`
  outside of a documented "this is a Linux path; the macOS
  equivalent is `/dev/cu.usbserial-*`, the Windows equivalent is
  `COMx`" aside; no `~/.bashrc` advice (Mac defaults to
  `~/.zshrc` since macOS Catalina; Windows has no equivalent);
  no `make` invocations in tutorials where `west build` or
  `cmake --build` would be the cross-platform alternative; no
  forward-slash absolute paths in code examples.
- Customer-invoked scripts under `scripts/` that are intentionally
  bash-only (because they wrap Linux-side tooling — `west`
  wrapper, Yocto setup, EEPROM programmer for a Linux-side test
  rig) carry a header note explaining which OSes they work on and
  the cross-platform equivalent for Win + Mac users.
- `scripts/check_cross_platform.py` (new in this slice) is the
  mechanical enforcement of the above.  Soft warnings initially;
  flipped to fail-on-warning in CI once the docs are cleaned (a
  future cleanup task, scoped separately).
- The `standalone-from-alp-studio` promise
  ([[alp-sdk-standalone-studio-is-consumer]]) extends here: a
  Win / Mac user with `python` + `west` +
  `arm-none-eabi-gcc` can produce a flashable Zephyr image
  without ever touching a Linux machine, an alp-studio install,
  or a WSL shell.

## Alternatives considered

**A. Linux-first, Win/Mac via WSL2 / Docker.**  Ship a
recommended Docker image; document WSL2 as the canonical
Windows entry point; treat native Win / Mac as best-effort.
Rejected because:

- This is the status quo of most embedded SDKs and it is exactly
  the friction the user wants to remove.  "Install WSL2 + a
  Docker daemon + a Linux VM before you can evaluate alp-sdk"
  is a multi-hour onboarding hurdle; the customer who is
  evaluating against three competing SDKs picks one of the
  other two before they finish.
- It hides cross-platform gaps in the toolchain layer behind a
  Linux emulation layer that the customer has to opt into.  The
  metadata-driven design ([[simplification-unification-principle]])
  is cross-platform by construction; abdicating that and
  retreating to "run a Linux container" gives back exactly the
  property the design earned.
- WSL2 + Docker are not free — they require Windows Pro /
  Enterprise (for older Windows builds), a working virtualization
  layer (which BIOS settings sometimes disable on locked-down
  corporate laptops), and a non-trivial RAM allocation.

**B. Linux-only.**  Document that alp-sdk requires Linux,
period; Mac and Windows customers are responsible for setting
up their own Linux environment.  Rejected because:

- The actual cross-platform property already exists in the code;
  this option throws it away for no engineering benefit.
- Embedded developers use a mix of host OSes — survey data from
  the Eclipse / Zephyr communities consistently shows Linux is
  a plurality, not a majority.  Restricting the SDK to a
  plurality means leaving the rest of the market unaddressed.
- The user's product positioning is explicit: "make it
  development possible in cross-platform … otherwise it will be
  difficult to convince users."  This option directly contradicts
  the product direction.

**C. Per-OS forks.**  Ship a `alp-sdk-windows`, `alp-sdk-mac`,
`alp-sdk-linux` set of repos with OS-specific scripts and docs.
Rejected because:

- It multiplies maintenance cost by 3× for zero earned
  abstraction — the underlying Python + CMake + west surface is
  already cross-platform, so forking would just be making the
  same code live in three places.
- It defeats the unified-metadata principle: a SoC JSON or
  board.yaml schema bump would have to be replicated across
  three repos.  That is exactly the duplicated-truth pattern the
  `simplification-unification-principle` memory entry calls out
  as a bug.
- Customers who switch host OS (e.g. from a Mac laptop to a
  Linux desktop CI host) would have to switch which fork they
  consume — a needless migration cost.

**D. Pure documentation fix.**  Just clean up the existing docs
to avoid Linux-only idioms, skip the ADR + lint + CI matrix
scaffolding.  Rejected because:

- A doc-only fix has no enforcement.  The next contributor
  writes another `~/.bashrc` line and the assumption creeps
  back in — the same way the README ended up with
  `inference: { backend: cpu }` before commit a3cd4fd removed it.
  Without a lint + a CI gate the cleanup is one-shot, not
  durable.
- The ADR is the artefact that next month's contributor reads
  to understand *why* the rules are what they are — without it
  the lint looks like an arbitrary stylistic preference that the
  next reviewer overrides.

## Consequences

### Positive

- **Adoption uplift on Win + Mac.**  Customers evaluating
  alp-sdk on a MacBook or a Windows workstation can complete
  the getting-started walkthrough without installing a Linux
  layer.  The "convince users" objection the user raised goes
  away by construction.
- **Honest scoping of the Linux-only constraint.**  The Yocto
  half is explicitly Linux-only and that scoping is documented
  + reflected in CI, not implied or hand-wavily mentioned in a
  README footnote.  Customers who genuinely need Yocto reach
  for WSL2 / a VM with their eyes open; customers who only
  need Zephyr-on-M never have to.
- **Mechanical enforcement of the property.**  The lint
  (`scripts/check_cross_platform.py`) catches Linux-only
  idioms in docs and scripts at PR time, and the CI matrix
  catches the same at build time on actual Win + Mac runners.
  Drift is impossible to land silently.
- **The standalone-from-alp-studio promise extends to host
  OS.**  Per [[alp-sdk-standalone-studio-is-consumer]] the
  SDK works without alp-studio; this ADR adds that it works
  without Linux too for the Zephyr-on-M path.  Customers can
  pick alp-sdk + Zephyr-on-M as a hand-written-firmware
  toolchain on the host OS they already use.

### Negative

- **CI cost increases.**  Adding a Win runner + a Mac runner
  to every PR cycle costs runner-minutes.  GitHub Actions Mac
  runners are ~10× more expensive than Linux per minute; Win
  runners are ~2×.  Mitigated by (a) running a slimmer scaffold
  on the non-Linux runners (metadata validate + the
  cross-platform lint + one example build, not the full
  Twister suite) and (b) marking the Mac + Win jobs
  `continue-on-error: true` initially while the runners prove
  out — they surface drift without blocking PRs while flakes
  are still being characterised.
- **Cross-platform discipline becomes a code-review concern.**
  Reviewers have to read shell snippets in docs with an eye
  for Linux-only idioms.  Mitigated by the lint, which surfaces
  the common cases automatically.
- **Bash-only scripts under `scripts/` get a header comment +
  cross-platform equivalent note.**  `bootstrap.sh` and
  `test-all.sh` are the two big ones today.  This is a small
  doc burden but a permanent one.

### Neutral

- The SDK already does most of the work — Python scripts, west,
  cmake, Arm GNU Toolchain, `<alp/*>` headers — all
  cross-platform.  This ADR is mostly **ratifying** existing
  cross-platform reality and **closing** a small set of
  documentation + CI gaps, not building cross-platform support
  from scratch.
- The `docs/getting-started.md` per-platform install one-liners
  already exist; the new `docs/cross-platform-setup.md` (this
  slice) is the deep dive, getting-started stays as the
  quickstart.
- Linux remains the canonical CI baseline.  The Mac + Win
  matrix entries are *additional* coverage, not a replacement
  for the existing Linux gates.  Twister stays on `ubuntu-latest`;
  bitbake stays on `ubuntu-latest`; the Mac + Win runners catch
  cross-platform drift but they are not load-bearing for
  shipping a release.

## References

- [`docs/cross-platform-setup.md`](../cross-platform-setup.md) —
  per-OS quickstart guide that operationalises this ADR.
- [`scripts/check_cross_platform.py`](../../scripts/check_cross_platform.py)
  — the mechanical lint that catches Linux-only idioms in docs
  + scripts.
- [`.github/workflows/cross-platform-zephyr.yml`](../../.github/workflows/cross-platform-zephyr.yml)
  — the Win + Mac CI matrix scaffolding.
- [ADR 0001](0001-wrapper-on-top-of-zephyr.md) — the wrapper-on-
  Zephyr framing makes the SDK's portable surface meaningful;
  this ADR extends that portability claim from "across vendors"
  to "across host OSes".
- [ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md) — alp-sdk
  is standalone; alp-studio is a consumer.  This ADR adds: the
  standalone path is cross-platform without Linux too.
- [ADR 0010](0010-heterogeneous-os-orchestration.md) — Zephyr +
  Yocto as peers, not alternatives.  This ADR scopes the
  host-OS implication of that decision: Yocto host stays
  Linux-only, Zephyr host is cross-platform.
- [ADR 0011](0011-intra-family-portability.md) — intra-family
  source portability across SoMs.  This ADR is the **host-OS**
  analogue: the same source builds the same artefact on
  Win + Mac + Linux.
- Memory `[[ubuntu-deferred-to-v2]]` — Ubuntu / Linux as a
  *backend* (running Alp user-space on Linux) is deferred past
  v1.0.  Orthogonal to this ADR: this ADR is about the **host**,
  not the target runtime.
- Memory `[[alp-sdk-standalone-studio-is-consumer]]` — alp-sdk
  is first-class without alp-studio.  This ADR extends that
  framing to host OS.
- Memory `[[no-nordic-branded-tooling]]` — toolchain
  recommendations stay vendor-neutral (Alif / Renesas / NXP),
  no Nordic-branded helpers.
