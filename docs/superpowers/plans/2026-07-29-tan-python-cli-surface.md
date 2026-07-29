# Sub-project 2 — a real CLI on the Python executor

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development.

**Goal:** turn the sub-project-1 libraries into a working `tan` CLI — `tan build` end to end — without violating a single invariant in
[2026-07-29-tan-port-invariants.md](../specs/2026-07-29-tan-port-invariants.md).

**Working tree:** `E:\GitHub\tan-cli\.worktrees\python-executor`, branch `feat/python-executor-mvp`. Everything under `python/`.

## The mechanism question is CLOSED — subprocess, not import

Established by the west/Zephyr coupling map:

- **west core has 13 commands** (`west/app/main.py:1204-1225`): Init, Update, List, ManifestCommand, Compare, Diff, Status, ForAll, Grep, Help, Config, Topdir, SelfUpdate. **No Build, no Flash, no Debug.**
- **`west build`/`flash`/`debug` and all 47 runners live in the Zephyr checkout** — `<zephyr>/scripts/west_commands/`, 23 files, 6052 lines — wired by ONE line, `alp-sdk/west.yml:182` `west-commands: scripts/west-commands.yml`.
- **Importing west in-process is dead**: the frozen `tan.exe` bundles `python311.dll` against Zephyr's `PYTHON_MINIMUM_REQUIRED 3.12`; `west build` isn't in the pip package to freeze; and `build.py:681` bakes `-DWEST_PYTHON={sys.executable}` — i.e. `tan.exe` — which `python.cmake:16-18` adopts **with no version check** and shells ~68 times.

**So: `tan` spawns `west` as a subprocess, with argv from the planner.** That is what the Rust tan already does and what `python/tan/commands/build/execute.py` was built for.

## Global constraints

- Python 3.11+; deps `typer`, `rich`, `pytest`, `pyinstaller` only.
- **Nothing but JSON on stdout** in `--format json`. Logs/progress to stderr.
- Envelope `{command, ok, exitCode, project, sdk?, data, issues}`; `sdk` **omitted** when absent.
- Exit codes fixed: `0` Success, `1` RuntimeFailure, `2` ValidationFailure, `3` WriteFailure, `4` DoctorFailure, `5` InternalFailure.
- Issue codes come from `contract/issue-codes.json` — 27 codes, 5 frozen, `bootstrap.windows-unsupported` **retired** (never reuse that spelling).
- **Commands must be imported STATICALLY** — a `pkgutil`/`importlib` registry works in source and fails frozen. See `python/tan/commands/__init__.py`.
- SPDX header on every new file; no AI/Claude attribution.
- The suite must stay green: 117 passed / 1 skipped / 18 xfailed.

## Invariants this sub-project must not break

Cited from the invariants doc; read it before starting.

- **I-01/I-02** — the OS is derived from the core class and is never selectable. **No `--os`, no `--backend` flag, ever.** A `board.yaml` carrying top-level `os:` is rejected by schema.
- **I-04** — the planner fans out over the **SoC's** cores, not the customer's. A one-core `board.yaml` legitimately produces three slices including a Yocto one. The CLI must not "helpfully" filter.
- **I-06** — build-plan order is `sorted(coreId)` with `off` excluded; system-manifest is SoC **array order** with `off` **included**. **Do not unify.**
- **I-11** — a slice is never dropped: `command: null` plus a matching `warnings[]` entry.
- **I-20** — shared artefacts land on disk **before any slice runs**.

---

### Task 1: The CLI skeleton and `tan --version` / bare-invocation behaviour

**Files:** modify `python/tan/__main__.py`; create `python/tan/cli.py`; test `python/tests/test_cli_skeleton.py`

Fixes a real defect found by the parity harness: **bare `tan` exits 0 silently in Python; Rust exits 2 with help.**

- [ ] **Step 1: failing test**

```python
# python/tests/test_cli_skeleton.py
# SPDX-License-Identifier: Apache-2.0
import json, subprocess, sys


def run(*argv):
    return subprocess.run([sys.executable, "-m", "tan", *argv],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def test_bare_invocation_exits_2_with_help_on_stderr():
    p = run()
    assert p.returncode == 2
    assert p.stdout == ""          # stdout is the envelope channel; help is not an envelope
    assert p.stderr.strip() != ""


def test_version_first_line_matches_the_extension_probe():
    p = run("--version")
    assert p.returncode == 0
    assert p.stdout.splitlines()[0].startswith("tan ")


def test_unknown_command_exits_2_and_emits_an_envelope_in_json_mode():
    p = run("definitely-not-a-command", "--format", "json")
    assert p.returncode == 2
    env = json.loads(p.stdout)
    assert env["ok"] is False and env["exitCode"] == 2
    assert "sdk" not in env or env["sdk"] is not None
```

- [ ] **Step 2:** run it, watch `test_bare_invocation_exits_2_with_help_on_stderr` fail (currently exit 0, empty everything).
- [ ] **Step 3:** implement. Bare invocation prints help to **stderr** and exits 2. Keep `--version` exactly as it is.
- [ ] **Step 4:** rerun; all three pass; full suite still 117+.
- [ ] **Step 5:** commit.

---

### Task 2: `tan build` wired to the existing executor

**Files:** create `python/tan/commands/build_cmd.py`; modify `python/tan/cli.py`; test `python/tests/commands/test_build_command.py`

Wires the sub-project-1 libraries into a command: resolve project → emit/read plan → substitute tokens → materialise **all** artefacts → execute slices → envelope.

**Order is a contract (I-20):** every `sharedArtefacts` entry and every slice's `configArtefacts` are written **before the first slice runs**.

- [ ] **Step 1: failing test** — a fixture plan with two slices, one `command: null`; assert the envelope reports one executed and one skipped, that `warnings[]` survives, and that artefacts existed on disk before the first spawn (record order via a spy `on_output`).
- [ ] **Step 2:** run, watch it fail.
- [ ] **Step 3:** implement `build_cmd.py`. `--plan-from <file>` reads a captured plan; without it, invoke the planner. **No `--os`/`--backend` flag.**
- [ ] **Step 4:** rerun; assert exit code maps through `ExitCode`.
- [ ] **Step 5:** promote the parity case if it now runs; if it XPASSes, delete its `NOT_PORTED` entry (`strict=True` will force this).
- [ ] **Step 6:** commit.

---

### Task 3: `tan doctor` — and fix the Python-version gap that breaks a fresh customer

**Files:** create `python/tan/commands/doctor_cmd.py`; test `python/tests/commands/test_doctor_command.py`

**A real bug, found in the coupling map, on Target 1's exact path:** `metadata/bootstrap.json:16` declares `pythonMinVersion: "3.10"`, Zephyr's `cmake/modules/python.cmake:14` requires **3.12**, and the bootstrap POSIX branch *"cannot fail on version"*. On Ubuntu 22.04, bootstrap succeeds, doctor says Pass, and the **first build dies at Zephyr's CMake configure**.

`doctor` must check, with the frozen issue codes:

- [ ] host Python **>= 3.12** (Zephyr's floor, not the manifest's 3.10) → `bootstrap.python-too-old`
- [ ] a runnable interpreter → `bootstrap.python-not-runnable`
- [ ] `west` present and >= the manifest's floor
- [ ] `SETOOLS_DIR` + `SE_UART` + the `fdt` package for AEN flashing — **today neither doctor mentions them**, so a customer gets a clean bill of health then fails at flash with a raw `RuntimeError`. Name the Alif download in the message.
- [ ] J-Link presence for Flow D
- [ ] exit `4` (`DoctorFailure`) when unhealthy — never 0

- [ ] Steps: failing test → run → implement → rerun → commit.

---

### Task 4: `tan validate`

**Files:** create `python/tan/commands/validate_cmd.py`; test `python/tests/commands/test_validate_command.py`

Has contract fixtures already (`validate-offline-clean`, `validate-offline-schema-violation`). Those fixtures are the spec: matching them is the definition of done. Exit `2` on a schema violation.

- [ ] Steps: failing test → run → implement → rerun → **promote the two conformance fixtures** → commit.

---

## Verification before this sub-project is done

- [ ] `cd python && python -m pytest -q` green; conformance xfail count **decreased** by the number of promoted fixtures
- [ ] `cargo fmt --all --check && cargo clippy --all-targets -- -D warnings && cargo build --all-targets && cargo test` still green — the Rust workspace stays untouched
- [ ] `bash scripts/build_binary.sh` still yields ONE file under the 15 MB ceiling, `--version` inside 3 s
- [ ] `tan build --plan-from <captured>` runs a real plan end to end
- [ ] No `--os` or `--backend` flag exists anywhere (grep and prove it)
