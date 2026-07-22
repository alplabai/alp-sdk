<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0020 parity gate

`tan` (this repo) is the sole executor for the Alp Lab build (ADR-0020, end-state
B). alp-sdk's planner is the fast-moving half of that split, so a planner
change that emits fine but builds wrong must be caught before it reaches a
release, not discovered on a bench. This directory seeds the gate ADR-0020's
2026-07-20 Amendment (alp-sdk#855) says is release-blocking: a **two-seam
parity gate** plus a **cross-repo trigger** so alp-sdk CI can drive it on
every planner change.

## The two seams

| Seam | Checks | Status |
|---|---|---|
| **1 — plan shape** | Does a live `--emit build-plan` still match a frozen, hand-verified oracle's command / env / appDir / skip-fail-decision *shape*, field for field, over the SoM matrix? Deliberately does NOT re-diff the materialised config-artefact content (alp.conf/local.conf/cmake-args.txt/sysbuild-conf bytes) — see "Seam-1 scope" below. Toolchain-free; runs on any `ubuntu-latest` runner. | **Implemented here**: `seam1_field_diff.py` + `.github/workflows/parity.yml`'s `seam1-plan-shape` job. |
| **2 — real build** | Materialise byte-check, an actual `west`/Zephyr build off the plan, and a Renode smoke test — the thing seam 1 can't catch (a plan that *looks* right but doesn't build). | **Follow-up, not seeded here.** Needs a Linux runner with the Zephyr SDK / toolchain installed (`west`, the AEN/E1M-X Zephyr modules, Renode). Placeholder `seam2` job in `.github/workflows/parity.yml` documents this — it does not run a fake check and does not report success for work it didn't do. |

Yocto/A-core artefact parity is explicitly **out of scope** for both seams —
no bitbake-capable runner infra exists, and bitbake output isn't
byte-reproducible (ADR-0020, "Phase-3 parity gate").

## Why the oracle is frozen at `97ad481b`

`tests/parity/oracle/*.build-plan.json` were captured at alp-sdk commit
`df312cec^` == `97ad481b` ("feat(build-plan): publish envAppendPath +
executionPolicy (ADR-0020 Phase 1, additive)", #847).

That specific SHA matters: it is the **last** alp-sdk commit that carries
*both*

- `Orchestrator.fan_out()` — the in-repo SDK-side executor, which until
  `df312cec` (#848, "retire the SDK executor, tan is sole executor") was
  still alive and usable as a real, in-repo build oracle; and
- the Phase-1 contract fields `tan` depends on today — per-slice
  `envAppendPath` and the top-level `executionPolicy`.

`df312cec` retires `fan_out` and every SDK-side executor outright (no legacy
shim survives that migration — ADR-0020 is explicit that there is no
rollback after Phase 4). After that commit there is no SDK-side executor left
to diff a live emit against, ever again. `97ad481b` is therefore the last
frame in which "does the live emit still match what the last real in-repo
oracle produced" is even an answerable question — freezing it now is
reconstructing that oracle retroactively, per the Amendment's remediation
step, not a routine fixture update.

## Seam-1 scope: shape only, not config-artefact content (2026-07-22 retune)

Through #874, seam-1 also byte-diffed each slice's materialised
config-artefact `contents` (the rendered `alp.conf`/`local.conf`/
`cmake-args.txt`) and `sharedArtefacts[*].contents` (DTS overlays,
`system_ipc.h`, sysbuild/TF-M conf) against the frozen oracle. That turned
*every* intentional emitter content change (a Kconfig dependency-gating fix,
a new peripheral default) into a seam-1 failure that could only be
"resolved" by adding another hand-reviewed strip to `normalize_plan`
(`_strip_863_additions`'s `_strip_lib_loader_block` half) — a per-change
treadmill that eroded the gate instead of doing its stated job (plan
*shape* parity). #874's follow-up retuned the comparator: `normalize_plan`
now drops every artefact's `contents` (`_drop_artefact_contents`), keeping
only its `path` in the shape check. Command, env, appDir, skip/fail
decision, and `debug.probe` are unaffected — see the sections below.

**Content coverage does not go away — it moves.** Every artefact's rendered
bytes are covered byte-for-byte by `tests/fixtures/emit-snapshots/*.
{build-plan,zephyr-conf}.snap` (`scripts/check_emit_snapshots.py`) instead,
and eventually by seam-2's real build. The table below maps every oracle
fixture to the emit-snapshot case that covers its config-artefact content —
confirmed complete as of the retune (two boards, `audio_i2s-tone` and
`connectivity_iot-fleet-ota`, had no prior emit-snapshot case; both were
added as `audio-i2s-tone.build-plan` / `iot-fleet-ota.build-plan` in
`scripts/check_emit_snapshots.py` rather than leave the retune opening an
uncovered content-check hole):

| Oracle fixture (`tests/parity/oracle/*.build-plan.json`) | `boardYaml` | Emit-snapshot golden (`tests/fixtures/emit-snapshots/`) |
|---|---|---|
| `audio_i2s-tone` | `examples/audio/i2s-tone/board.yaml` | `audio-i2s-tone.build-plan.snap` |
| `connectivity_iot-fleet-ota` | `examples/connectivity/iot-fleet-ota/board.yaml` | `iot-fleet-ota.build-plan.snap` (also covers its sysbuild/TF-M `sharedArtefacts`) |
| `multicore_heterogeneous-offload` | `examples/multicore/heterogeneous-offload/board.yaml` | `hetero-offload.build-plan.snap` |
| `multicore_rpmsg-aen` | `examples/multicore/rpmsg-aen/board.yaml` | `rpmsg-aen.build-plan.snap` |
| `multicore_rpmsg-imx93` | `examples/multicore/rpmsg-imx93/board.yaml` | `rpmsg-imx93.build-plan.snap` |
| `multicore_rpmsg-v2n` | `examples/multicore/rpmsg-v2n/board.yaml` | `rpmsg-v2n.build-plan.snap` |

A future oracle fixture MUST get a matching emit-snapshot `build-plan` case
in the same change, or this coverage table (and the content-check it
promises) silently rots.

> **Contract note:** this retune narrows seam-1's ADR-0020 scope (it no
> longer asserts config-artefact content parity at all — that assertion
> moved to the emit-snapshot goldens). Like the #863/#865 additive deltas
> above, this needs Hakan's ratification on review before it's load-bearing
> for a release gate the same way ADR-0020's Amendment is.

## Why the comparator normalizes three fields

A `build-plan.json` is **not hermetic** — it embeds facts about the checkout
and host that emitted it, not just the board it plans for:

- **the checkout-root absolute path**, in `env.ALP_SDK_ROOT`, every
  `envAppendPath.*` entry, each slice's `appDir`, and (for sysbuild slices)
  embedded mid-string inside `command.args` (`-DSB_CONF_FILE=<root>/a;<root>/b`,
  which isn't a root-prefixed string outright — the comparator does a global
  substring replace, not a prefix check, to catch this);
- **`sdkCommit`**, the emitting commit's short SHA;
- **the emitting host's Python interpreter path**, in each cmake/sysbuild
  slice's `command.args` entry `-DPython3_EXECUTABLE=<path>` — at the
  frozen oracle's `97ad481b`, `orchestrator.py` pinned this to
  `sys.executable`, so it was inherently host-specific (a Homebrew path on
  the oracle's capture machine, `/usr/bin/python3` on a WSL runner, a
  hosted-toolcache path on `ubuntu-latest`). Issue #865 later replaced that
  with a literal `${PYTHON}` token on a live plan (see below) — the
  comparator's `-DPython3_EXECUTABLE=` regex match tokenizes either shape
  the same way, so this stays true for both the frozen oracle and a live
  `planPathMode: tokened` plan.

None of the three is a real parity break — they differ by construction
between the oracle's `97ad481b` capture checkout/host and whatever checkout/
host is emitting the live plan under test. `seam1_field_diff.py` normalizes
all three before diffing: the checkout root (discovered from the plan's own
`slices[0].env.ALP_SDK_ROOT` — nothing is hardcoded) is replaced everywhere
with the literal token `__SDKROOT__`, `sdkCommit` is replaced with `__SHA__`,
and any `-DPython3_EXECUTABLE=<path>` arg has its path replaced with
`<PYEXE>` (the arg itself still must be present — only the host-specific
value is tokenized, so the shape check survives).

## Issue #865: this checks the token-COLLAPSED shape, not token choice

A live plan is now `planPathMode: tokened` (`${SDK_ROOT}`/`${PROJECT_ROOT}`/
`${PYTHON}` literal tokens, not baked-in absolute paths — see
`docs/adr/0020-sdk-owns-build-execution.md`'s Amendment item 5). Every
harness fixture board.yaml nests under the SDK checkout, so `normalize_plan`
maps `${PROJECT_ROOT}` to `__SDKROOT__/<boardYaml's own directory>` — the
**same** collapsed string `${SDK_ROOT}` maps to. This seam therefore proves
the plan *shape* is unchanged; it cannot distinguish a bug that swapped
`${SDK_ROOT}` and `${PROJECT_ROOT}` at some call site from a correct emit.
Token *choice* is guarded elsewhere: the emit-snapshot goldens
(`tests/fixtures/emit-snapshots/*.build-plan.snap`, which keep the two
tokens' literal shapes) and tan-cli #24's own substitution tests.

## The one allowed delta: `debug.probe`

After normalization, the **only** field allowed to differ between the oracle
and a live emit is `slices[*].debug.probe`, and only in the direction
`"openocd"` (oracle, captured at `97ad481b`) `->` `null` (`df312cec` and
later). This is `#848`'s intentional, hand-reviewed change: the SDK-side
executor named a concrete debug-probe runner because it drove `west`/OpenOCD
itself; post-ADR-0020 the SDK doesn't own flashing at all (`tan` does), so
asserting `probe: "openocd"` would be a claim the SDK can no longer honestly
make. `null` means "the SDK isn't naming a probe" — a downgrade to
not-claiming, not a hidden capability loss. ADR-0020's Amendment states this
explicitly: "the only `97ad481b`<->`df312cec` emit delta is `debug.probe`
`"openocd"->null`, hand-reviewed."

Any other diff in the plan's SHAPE — a changed command, a changed `env`
value, a changed slice count, a `probe` change to anything other than that
exact transition — **fails** the gate. Config-artefact `contents` is dropped
before the diff runs at all (see "Seam-1 scope" above), so a content-only
change never reaches this allow-list in the first place. See
`seam1_field_diff.py`'s module docstring for the exact rule the comparator
implements.

## Running it locally

```
python3 tests/parity/seam1_field_diff.py \
  --sdk /path/to/an/alp-sdk/checkout \
  --oracle tests/parity/oracle
```

`--boards` restricts the check to specific oracle fixtures (filename minus
`.build-plan.json`, e.g. `--boards audio_i2s-tone multicore_rpmsg-v2n`);
omitted, it checks every fixture in `--oracle`. Exit code is `0` iff every
board's only diffs (if any) are the allowed `debug.probe` delta.

## CI wiring

`.github/workflows/parity.yml` runs `seam1-plan-shape` on every pull request
(against a pinned alp-sdk tag — see the workflow's `PINNED_SDK_TAG` comment)
and on a `repository_dispatch` of type `alp-sdk-planner-change` (the
cross-repo trigger ADR-0020's Amendment requires: alp-sdk CI fires this on
every planner change so a drifting emit surfaces on the *alp-sdk* PR, not
discovered later against a stale checkout). The dispatch payload's
`client_payload.sdk_ref` picks the exact SDK ref under test.
