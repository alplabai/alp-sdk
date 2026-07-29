# Python `tan` Core Executor MVP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Python `tan` that executes an alp-sdk build plan end-to-end and is byte-compatible with the shipped Rust `tan`, proven against the committed contract goldens and a live Rust oracle.

**Architecture:** A new `python/` package inside the tan-cli repo, mirroring the Rust module split file-for-file so parity review is a side-by-side read. Pure decision logic (env assembly, policy, token substitution, stamp) goes in `tan/core/` with no IO, exactly as `tan-core` does; the executor owns spawning and path resolution. The Rust workspace is **not touched** — it stays shipped and is the oracle.

**Tech Stack:** Python 3.11+, Typer (CLI surface), stdlib `subprocess` (execution), `rich` (progress), pytest, PyInstaller `--onefile` (packaging).

**Spec:** [2026-07-29-tan-python-executor-mvp-design.md](../specs/2026-07-29-tan-python-executor-mvp-design.md)

**Working tree:** `E:\GitHub\tan-cli\.worktrees\python-executor` (branch `feat/python-executor-mvp`, off `dev`). All paths below are relative to it unless stated.

## Global Constraints

- **Python 3.11+.** No dependency beyond `typer`, `rich`, `pytest`, `pyinstaller`.
  **Scope note:** this four-dependency ceiling binds **sub-project 1 only**. The
  executor genuinely needs nothing more. From sub-project 2 on — when the
  planner moves in — `tan` additionally requires `PyYAML`, `jsonschema`,
  `click`, `cryptography`, `cbor2`, `questionary`, `colorama` and `pyserial`
  (already alp-sdk's declared deps), plus `kconfiglib` loaded at runtime from
  `$ZEPHYR_BASE/scripts/kconfig`. Do not carry this ceiling into a later
  sub-project as if it were permanent. Note `alp_cli` is already built on
  `click` and Typer is a layer over click, so the two CLI surfaces converge.
- **Nothing but JSON on stdout** in `--format json` mode. Logs/progress go to stderr. A stray `print()` silently breaks the extension.
- **`--version` first line must match `/^tan \d+\.\d+\.\d+/`** — the extension rejects the binary otherwise (`alp-sdk-vscode/src/alpCli/service.ts:107-121`).
- **Envelope key set is `{command, ok, exitCode, project, sdk?, data, issues}`.** `sdk` is **omitted entirely when absent — never `null`** (that is what keeps the goldens byte-identical).
- **Exit codes are fixed:** `0` Success, `1` RuntimeFailure, `2` ValidationFailure, `3` WriteFailure, `4` DoctorFailure, `5` InternalFailure.
- **SDK-contract strings verbatim** — `alp-sdk`, `alp_orchestrate`, `alp_project.py`, `board.yaml`, `alp.conf`, `alp.overlay`, `.alp/`, `.tan-sdk-root`. Never rename.
- **Issue codes are matched by exact string** by the extension and the goldens. Copy them character-for-character.
- **Apache-2.0** — every new file starts with `# SPDX-License-Identifier: Apache-2.0`.
- **No AI/Claude attribution** in code, comments, or commit messages.
- **Keep files small.** Pure logic in `tan/core/`, never in the executor IO file.

---

## File Structure

| Path | Responsibility |
|---|---|
| `python/pyproject.toml` | Package metadata, deps, `tan` entrypoint |
| `python/tan/__main__.py` | Typer app, global args, `--format json` |
| `python/tan/exit_codes.py` | The 6 fixed exit codes |
| `python/tan/envelope.py` | Envelope + Issue + Project + SdkInfo, `to_json` |
| `python/tan/core/plan_exec.py` | Pure: env append/assemble, policy, stamp action |
| `python/tan/core/build_plan.py` | Plan parse + version-skew guard |
| `python/tan/core/plan_tokens.py` | Pure token substitution + demotion |
| `python/tan/commands/build/token_substitution.py` | IO half of token substitution (git HEAD, resolution) |
| `python/tan/commands/build/materialise.py` | Write shared + config artefacts |
| `python/tan/commands/build/execute.py` | Spawn slices, stream, cancel |
| `python/tests/` | pytest unit tests, mirroring each module |
| `python/tests/conformance/test_contract_envelopes.py` | Runs the committed `contract/envelopes/` goldens |
| `python/tests/parity/oracle.py` | Diffs Python `tan` against the Rust binary |

---

### Task 1: Package skeleton, exit codes, envelope

**Files:**
- Create: `python/pyproject.toml`, `python/tan/__init__.py`, `python/tan/__main__.py`, `python/tan/exit_codes.py`, `python/tan/envelope.py`
- Test: `python/tests/test_envelope.py`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `ExitCode` (IntEnum: `SUCCESS=0`, `RUNTIME_FAILURE=1`, `VALIDATION_FAILURE=2`, `WRITE_FAILURE=3`, `DOCTOR_FAILURE=4`, `INTERNAL_FAILURE=5`); `Issue(code: str, severity: str, message: str)`; `Project(root: str | None, board_yaml: str | None)`; `SdkInfo(root: str, source_tier: str)`; `Envelope(command, project, data, issues, exit_code, sdk=None)` with `.to_json() -> str`.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/test_envelope.py
# SPDX-License-Identifier: Apache-2.0
import json

from tan.envelope import Envelope, Issue, Project, SdkInfo
from tan.exit_codes import ExitCode


def test_sdk_key_is_absent_when_none_not_null():
    """Absent, never null -- this is what keeps the contract goldens byte-identical."""
    env = Envelope("test", Project(root="/p", board_yaml=None), 1, [], ExitCode.SUCCESS)
    parsed = json.loads(env.to_json())
    assert "sdk" not in parsed, f"sdk must be absent, not null: {parsed}"


def test_sdk_key_serialises_camel_case_member_set():
    env = Envelope(
        "test", Project(root="/p", board_yaml=None), 1, [], ExitCode.SUCCESS,
        sdk=SdkInfo(root="/resolved/sdk", source_tier="discovery"),
    )
    parsed = json.loads(env.to_json())
    assert parsed["sdk"] == {"root": "/resolved/sdk", "sourceTier": "discovery"}


def test_ok_is_derived_from_exit_code_and_keys_are_camel_case():
    env = Envelope(
        "test", Project(root=None, board_yaml="/p/board.yaml"), 42,
        [Issue("x.y", "error", "m")], ExitCode.VALIDATION_FAILURE,
    )
    parsed = json.loads(env.to_json())
    assert parsed["ok"] is False
    assert parsed["exitCode"] == 2
    assert parsed["project"] == {"root": None, "boardYaml": "/p/board.yaml"}
    assert parsed["data"] == 42
    assert parsed["issues"] == [{"code": "x.y", "severity": "error", "message": "m"}]


def test_to_json_never_raises_on_unserialisable_payload():
    """Rust contract: a payload that cannot serialise must still emit ONE parseable
    envelope with ok:false and an envelope.serialize-failed issue -- never a crash
    with zero bytes on stdout."""
    env = Envelope("test", Project(None, None), {(1, 2): 3}, [], ExitCode.SUCCESS)
    parsed = json.loads(env.to_json())
    assert parsed["ok"] is False
    assert parsed["exitCode"] == 5
    assert parsed["issues"][0]["code"] == "envelope.serialize-failed"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd python && python -m pytest tests/test_envelope.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tan'`

- [ ] **Step 3: Write minimal implementation**

```python
# python/tan/exit_codes.py
# SPDX-License-Identifier: Apache-2.0
"""Stable process exit codes, fixed by the CLI output contract."""
from enum import IntEnum


class ExitCode(IntEnum):
    SUCCESS = 0
    RUNTIME_FAILURE = 1
    VALIDATION_FAILURE = 2
    WRITE_FAILURE = 3
    DOCTOR_FAILURE = 4
    INTERNAL_FAILURE = 5
```

```python
# python/tan/envelope.py
# SPDX-License-Identifier: Apache-2.0
"""Machine-readable result envelope. JSON mode writes exactly one to stdout."""
import json
from dataclasses import dataclass
from typing import Any

from tan.exit_codes import ExitCode


@dataclass(frozen=True)
class Issue:
    code: str
    severity: str
    message: str

    def as_dict(self) -> dict[str, str]:
        return {"code": self.code, "severity": self.severity, "message": self.message}


@dataclass(frozen=True)
class Project:
    root: str | None
    board_yaml: str | None

    def as_dict(self) -> dict[str, Any]:
        return {"root": self.root, "boardYaml": self.board_yaml}


@dataclass(frozen=True)
class SdkInfo:
    root: str
    source_tier: str

    def as_dict(self) -> dict[str, str]:
        return {"root": self.root, "sourceTier": self.source_tier}


class Envelope:
    def __init__(self, command, project, data, issues, exit_code, sdk=None):
        self.command = command
        self.project = project
        self.data = data
        self.issues = issues
        self.exit_code = int(exit_code)
        self.sdk = sdk

    def _as_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "command": self.command,
            "ok": self.exit_code == 0,
            "exitCode": self.exit_code,
            "project": self.project.as_dict(),
        }
        # Absent, not null -- see test_sdk_key_is_absent_when_none_not_null.
        if self.sdk is not None:
            out["sdk"] = self.sdk.as_dict()
        out["data"] = self.data
        out["issues"] = [i.as_dict() for i in self.issues]
        return out

    def to_json(self) -> str:
        try:
            return json.dumps(self._as_dict(), separators=(",", ":"))
        except (TypeError, ValueError) as err:
            fallback = {
                "command": self.command,
                "ok": False,
                "exitCode": int(ExitCode.INTERNAL_FAILURE),
                "project": self.project.as_dict(),
            }
            if self.sdk is not None:
                fallback["sdk"] = self.sdk.as_dict()
            fallback["data"] = None
            fallback["issues"] = [
                Issue(
                    "envelope.serialize-failed",
                    "error",
                    f"failed to serialize command output: {err}",
                ).as_dict()
            ]
            return json.dumps(fallback, separators=(",", ":"))
```

```toml
# python/pyproject.toml
[project]
name = "tan"
version = "0.4.0"
requires-python = ">=3.11"
dependencies = ["typer>=0.12", "rich>=13"]

[project.scripts]
tan = "tan.__main__:main"

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"
```

```python
# python/tan/__main__.py
# SPDX-License-Identifier: Apache-2.0
"""The `tan` entrypoint."""
import typer

from tan.version import TAN_VERSION

app = typer.Typer(add_completion=False)


@app.callback(invoke_without_command=True)
def cli(version: bool = typer.Option(False, "--version")) -> None:
    if version:
        # MUST match /^tan \d+\.\d+\.\d+/ -- the extension rejects the binary
        # otherwise (alp-sdk-vscode/src/alpCli/service.ts:107-121).
        typer.echo(f"tan {TAN_VERSION}")


def main() -> None:
    app()
```

```python
# python/tan/version.py
# SPDX-License-Identifier: Apache-2.0
TAN_VERSION = "0.4.0"
```

Also create an empty `python/tan/__init__.py`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd python && python -m pytest tests/test_envelope.py -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Verify the version line matches the extension's regex**

Run: `cd python && python -m tan --version`
Expected: exactly `tan 0.4.0`

- [ ] **Step 6: Commit**

```bash
git add python/
git commit -m "feat(python): package skeleton, exit codes and the result envelope"
```

---

### Task 2: Pure env assembly and execution policy

**Files:**
- Create: `python/tan/core/__init__.py`, `python/tan/core/plan_exec.py`
- Test: `python/tests/core/test_plan_exec.py`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `sep_for_key(var: str) -> str`; `apply_env_append(base: list[tuple[str, str]], append: dict[str, list[str]]) -> None` (mutates in place); `assemble_slice_env(slice_env: dict[str, str], env_append_path: dict[str, list[str]], inherited: Callable[[str], str | None], gap_fillers: Sequence[tuple[str, str]]) -> list[tuple[str, str]]`; `PolicyAction` (StrEnum: `SKIP="skip"`, `FAIL="fail"`); `resolve_action(policy: ExecutionPolicy | None, key: str, default: PolicyAction) -> PolicyAction`; `SdkStampAction` (StrEnum: `KEEP`, `PRISTINE`); `sdk_stamp_action(cached, current, cache_configured, build_dir_overridden, cwd_under_build_root) -> SdkStampAction`.

**Port note — this is the highest-value trap in the whole port.** `EXTRA_ZEPHYR_MODULES` is a **CMake list**, joined with `;` on **every** platform. Joining it with `os.pathsep` (`:` on Linux/WSL) makes `west build` configure fail with `is not a valid zephyr module`. Every other variable is a real OS path list and uses `os.pathsep`.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/core/test_plan_exec.py
# SPDX-License-Identifier: Apache-2.0
import os

from tan.core.plan_exec import (
    ExecutionPolicy,
    PolicyAction,
    SdkStampAction,
    apply_env_append,
    assemble_slice_env,
    resolve_action,
    sdk_stamp_action,
)

SEP = os.pathsep


def test_apply_env_append_appends_and_dedups():
    base = [("PYTHONPATH", f"/a{SEP}/b")]
    apply_env_append(base, {"PYTHONPATH": ["/b", "/c"]})
    assert base == [("PYTHONPATH", f"/a{SEP}/b{SEP}/c")]


def test_extra_zephyr_modules_always_uses_semicolon():
    """CMake list -- ';' on EVERY platform. ':' on Linux makes west build fail
    configure with 'is not a valid zephyr module'."""
    base = [("EXTRA_ZEPHYR_MODULES", "/a")]
    apply_env_append(base, {"EXTRA_ZEPHYR_MODULES": ["/b"]})
    assert base == [("EXTRA_ZEPHYR_MODULES", "/a;/b")]


def test_apply_env_append_seeds_absent_var():
    base = []
    apply_env_append(base, {"EXTRA_ZEPHYR_MODULES": ["/sdk"]})
    assert base == [("EXTRA_ZEPHYR_MODULES", "/sdk")]


def test_assemble_slice_env_appends_dedups_seeds_and_fills_gaps():
    env = assemble_slice_env(
        slice_env={"ALP_SDK_ROOT": "/sdk"},
        env_append_path={
            "PYTHONPATH": ["/sdk/scripts"],
            "EXTRA_ZEPHYR_MODULES": ["/plan/sdk"],
        },
        inherited=lambda k: "/inh" if k == "PYTHONPATH" else None,
        gap_fillers=[("ZEPHYR_BASE", "/ws/zephyr")],
    )
    got = dict(env)
    assert got["ALP_SDK_ROOT"] == "/sdk"
    # Inherited PYTHONPATH is EXTENDED, never replaced.
    assert got["PYTHONPATH"] == f"/inh{SEP}/sdk/scripts"
    assert got["EXTRA_ZEPHYR_MODULES"] == "/plan/sdk"
    assert got["ZEPHYR_BASE"] == "/ws/zephyr"


def test_resolve_action_honors_policy_with_default_fallback():
    policy = ExecutionPolicy(unknown_backend=None, missing_tool=PolicyAction.FAIL,
                             null_command=PolicyAction.SKIP)
    assert resolve_action(policy, "missing_tool", PolicyAction.SKIP) is PolicyAction.FAIL
    assert resolve_action(policy, "null_command", PolicyAction.FAIL) is PolicyAction.SKIP
    # Absent entry -> default; None policy (older plan) -> default.
    assert resolve_action(policy, "unknown_backend", PolicyAction.SKIP) is PolicyAction.SKIP
    assert resolve_action(None, "missing_tool", PolicyAction.SKIP) is PolicyAction.SKIP


def test_sdk_stamp_action_matrix():
    keep, pristine = SdkStampAction.KEEP, SdkStampAction.PRISTINE
    # Matching stamp -> keep.
    assert sdk_stamp_action("/sdk/v0.13.0", "/sdk/v0.13.0", True, False, True) is keep
    # Mismatched stamp -> wipe (the reported v0.11.0 -> v0.13.0 case).
    assert sdk_stamp_action("/sdk/v0.11.0", "/sdk/v0.13.0", True, False, True) is pristine
    # Missing stamp on an already-configured dir -> wipe (the ONLY self-heal path).
    assert sdk_stamp_action(None, "/sdk/v0.13.0", True, False, True) is pristine
    # Never configured -> nothing to go stale.
    assert sdk_stamp_action(None, "/sdk/v0.13.0", False, False, True) is keep
    # -d/--build-dir override -> west wrote somewhere we cannot know.
    assert sdk_stamp_action("/sdk/v0.11.0", "/sdk/v0.13.0", True, True, True) is keep
    # cwd outside build root -> refuse to target <project>/src/build.
    assert sdk_stamp_action("/sdk/v0.11.0", "/sdk/v0.13.0", True, False, False) is keep
    # Unresolved current sdk root -> nothing to compare against.
    assert sdk_stamp_action("/sdk/v0.11.0", None, True, False, True) is keep
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd python && python -m pytest tests/core/test_plan_exec.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tan.core'`

- [ ] **Step 3: Write minimal implementation**

```python
# python/tan/core/plan_exec.py
# SPDX-License-Identifier: Apache-2.0
"""Pure build-plan execution decisions (ADR-0020): turn the plan's
envAppendPath and executionPolicy into concrete env values and skip/fail
dispositions. No IO, no spawning -- the executor calls these and owns the IO."""
import os
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from enum import StrEnum


class PolicyAction(StrEnum):
    SKIP = "skip"
    FAIL = "fail"


@dataclass(frozen=True)
class ExecutionPolicy:
    unknown_backend: PolicyAction | None = None
    missing_tool: PolicyAction | None = None
    null_command: PolicyAction | None = None


def sep_for_key(var: str) -> str:
    """The join separator for ONE envAppendPath var -- per-key, not uniformly
    os.pathsep. EXTRA_ZEPHYR_MODULES is a CMake list that Zephyr's
    zephyr_module.py splits on ';' on EVERY platform (never an OS path list);
    joining it with ':' on Linux/WSL fails `west build` configure with
    "is not a valid zephyr module"."""
    return ";" if var == "EXTRA_ZEPHYR_MODULES" else os.pathsep


def apply_env_append(base: list[tuple[str, str]], append: dict[str, list[str]]) -> None:
    """Append each value to its var using that var's separator, skipping a value
    already present segment-wise. A var absent from `base` is seeded from the
    appended values (the plan owns it). Mutates `base` in place."""
    for var, values in append.items():
        sep = sep_for_key(var)
        current = next((v for k, v in base if k == var), None)
        segments = current.split(sep) if current else []
        for val in values:
            if val not in segments:
                segments.append(val)
        joined = sep.join(segments)
        for i, (k, _) in enumerate(base):
            if k == var:
                base[i] = (var, joined)
                break
        else:
            base.append((var, joined))


def assemble_slice_env(
    slice_env: dict[str, str],
    env_append_path: dict[str, list[str]],
    inherited: Callable[[str], str | None],
    gap_fillers: Sequence[tuple[str, str]],
) -> list[tuple[str, str]]:
    """Start from the slice's verbatim env, seed any envAppendPath var the slice
    doesn't pin from `inherited` (so an append EXTENDS rather than replaces an
    inherited PYTHONPATH), apply the appends, then merge the CLI's
    consumer-mechanism gap fillers -- "plan wins / CLI fills gaps"."""
    env: list[tuple[str, str]] = list(slice_env.items())
    for key in env_append_path:
        if not any(k == key for k, _ in env):
            value = inherited(key)
            if value is not None:
                env.append((key, value))
    apply_env_append(env, env_append_path)
    for key, value in gap_fillers:
        for i, (k, _) in enumerate(env):
            if k == key:
                env[i] = (key, value)
                break
        else:
            env.append((key, value))
    return env


def resolve_action(
    policy: ExecutionPolicy | None, key: str, default: PolicyAction
) -> PolicyAction:
    """Honour executionPolicy's entry when present, else the CLI's built-in
    behaviour for an older plan that omits it."""
    if policy is None:
        return default
    picked = getattr(policy, key, None)
    return picked if picked is not None else default


class SdkStampAction(StrEnum):
    KEEP = "keep"
    PRISTINE = "pristine"


def sdk_stamp_action(
    cached: str | None,
    current: str | None,
    cache_configured: bool,
    build_dir_overridden: bool,
    cwd_under_build_root: bool,
) -> SdkStampAction:
    """Whether to wipe a slice's build dir because it was configured against a
    different SDK root. West refuses to reconfigure a build dir whose CMake cache
    is bound to another source tree ("FATAL ERROR: refusing to proceed without
    --force"), which reaches the user as a bare "terminated with exit code: 1".

    A missing stamp on an already-configured dir reads as stale DELIBERATELY --
    it is the only way a build dir predating this feature ever self-heals."""
    if build_dir_overridden or not cwd_under_build_root or not cache_configured:
        return SdkStampAction.KEEP
    if current is None:
        return SdkStampAction.KEEP
    return SdkStampAction.KEEP if cached == current else SdkStampAction.PRISTINE
```

Also create an empty `python/tan/core/__init__.py` and `python/tests/core/__init__.py`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd python && python -m pytest tests/core/test_plan_exec.py -v`
Expected: PASS (6 tests)

- [ ] **Step 5: Cross-check against the Rust unit tests**

Run: `cargo test -p tan-core plan_exec`
Expected: PASS. Read `crates/tan-core/src/plan_exec.rs` tests and confirm each Rust assertion has a Python twin above. Any Rust case with no Python twin is a gap — add it.

- [ ] **Step 6: Commit**

```bash
git add python/tan/core/plan_exec.py python/tan/core/__init__.py python/tests/core/
git commit -m "feat(python): port pure env-assembly, execution policy and SDK stamp decisions"
```

---

### Task 3: Build-plan parsing and the version-skew guard

**Files:**
- Create: `python/tan/core/build_plan.py`
- Test: `python/tests/core/test_build_plan.py`

**Interfaces:**
- Consumes: `ExecutionPolicy`, `PolicyAction` from `tan.core.plan_exec`.
- Produces: `SUPPORTED_SCHEMA_VERSION = 1`; `PlanParseError(Exception)` with `.code: str` and `.message: str`; `SliceCommand(tool: str, args: list[str], cwd: str | None)`; `Slice(core_id, backend, build_dir, app_dir, config_artefacts, toolchain, artifacts, debug, command, env, env_append_path)`; `BuildPlan(schema_version, generated_by, sdk_version, sdk_commit, plan_path_mode, board_yaml, sku, build_root, execution_policy, slices, shared_artefacts, warnings)`; `parse_build_plan(text: str) -> BuildPlan`.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/core/test_build_plan.py
# SPDX-License-Identifier: Apache-2.0
import pytest

from tan.core.build_plan import PlanParseError, parse_build_plan
from tan.core.plan_exec import PolicyAction

MINIMAL = """{
  "schemaVersion": 1, "generatedBy": "alp_orchestrate", "boardYaml": "/w/board.yaml",
  "sku": "E1M-AEN801", "buildRoot": "build", "slices": [], "sharedArtefacts": [], "warnings": []
}"""


def test_parses_a_minimal_plan():
    plan = parse_build_plan(MINIMAL)
    assert plan.schema_version == 1
    assert plan.sku == "E1M-AEN801"
    assert plan.slices == []
    # Optional-but-always-emitted fields default cleanly (tolerant consumer).
    assert plan.execution_policy is None
    assert plan.plan_path_mode is None


def test_rejects_an_unsupported_schema_version():
    """The version-skew guard: fail LOUDLY rather than silently falling back to
    hand-ported behaviour -- that fallback is exactly the RFC #843 drift."""
    with pytest.raises(PlanParseError) as e:
        parse_build_plan(MINIMAL.replace('"schemaVersion": 1', '"schemaVersion": 2'))
    assert e.value.code == "build.plan-unsupported-schema"
    assert "2" in e.value.message


def test_rejects_a_plan_missing_a_required_key():
    with pytest.raises(PlanParseError) as e:
        parse_build_plan(MINIMAL.replace('"sku": "E1M-AEN801", ', ""))
    assert e.value.code == "build.plan-invalid"
    assert "sku" in e.value.message


def test_parses_slice_env_append_and_policy():
    plan = parse_build_plan("""{
      "schemaVersion": 1, "generatedBy": "alp_orchestrate", "boardYaml": "/w/board.yaml",
      "sku": "S", "buildRoot": "build", "sharedArtefacts": [], "warnings": [],
      "executionPolicy": {"missingTool": "skip", "unknownBackend": "fail"},
      "slices": [{
        "coreId": "m55_hp", "backend": "zephyr", "buildDir": "build/m55_hp",
        "appDir": "app", "configArtefacts": [], "toolchain": null, "artifacts": [],
        "debug": {}, "command": {"tool": "west", "args": ["build"], "cwd": "build/m55_hp"},
        "env": {"ALP_SDK_ROOT": "/sdk"}, "envAppendPath": {"PYTHONPATH": ["/sdk/scripts"]}
      }]
    }""")
    assert plan.execution_policy.missing_tool is PolicyAction.SKIP
    assert plan.execution_policy.unknown_backend is PolicyAction.FAIL
    assert plan.execution_policy.null_command is None
    s = plan.slices[0]
    assert s.core_id == "m55_hp"
    assert s.command.tool == "west"
    assert s.env_append_path == {"PYTHONPATH": ["/sdk/scripts"]}


def test_null_command_slice_parses_as_none():
    """command: null is a legitimate skip-with-warning slice, not a parse error."""
    plan = parse_build_plan("""{
      "schemaVersion": 1, "generatedBy": "alp_orchestrate", "boardYaml": "/w/board.yaml",
      "sku": "S", "buildRoot": "build", "sharedArtefacts": [], "warnings": [],
      "slices": [{
        "coreId": "a55", "backend": "yocto", "buildDir": "build/a55", "appDir": "app",
        "configArtefacts": [], "toolchain": null, "artifacts": [], "debug": {},
        "command": null, "env": {}, "envAppendPath": {}
      }]
    }""")
    assert plan.slices[0].command is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd python && python -m pytest tests/core/test_build_plan.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tan.core.build_plan'`

- [ ] **Step 3: Write minimal implementation**

```python
# python/tan/core/build_plan.py
# SPDX-License-Identifier: Apache-2.0
"""The build-plan CONSUMER model (alp-sdk metadata/schemas/build-plan-v1.schema.json).

Strict producer / tolerant consumer: the required keys are enforced, the
optional-but-always-emitted ones default cleanly, and an unsupported
schemaVersion is REFUSED rather than silently hand-ported around."""
import json
from dataclasses import dataclass, field
from typing import Any

from tan.core.plan_exec import ExecutionPolicy, PolicyAction

SUPPORTED_SCHEMA_VERSION = 1

_REQUIRED_TOP = (
    "schemaVersion", "generatedBy", "boardYaml", "sku",
    "buildRoot", "slices", "sharedArtefacts", "warnings",
)
_REQUIRED_SLICE = (
    "coreId", "backend", "buildDir", "appDir", "configArtefacts",
    "toolchain", "artifacts", "debug", "command", "env", "envAppendPath",
)


class PlanParseError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass(frozen=True)
class SliceCommand:
    tool: str
    args: list[str]
    cwd: str | None


@dataclass(frozen=True)
class Slice:
    core_id: str
    backend: str
    build_dir: str
    app_dir: str
    config_artefacts: list[dict[str, Any]]
    toolchain: Any
    artifacts: dict[str, Any]
    debug: dict[str, Any]
    command: SliceCommand | None
    env: dict[str, str]
    env_append_path: dict[str, list[str]]


@dataclass(frozen=True)
class BuildPlan:
    schema_version: int
    generated_by: str
    board_yaml: str
    sku: str
    build_root: str
    slices: list[Slice]
    shared_artefacts: list[dict[str, Any]]
    warnings: list[Any]
    sdk_version: str | None = None
    sdk_commit: str | None = None
    plan_path_mode: str | None = None
    execution_policy: ExecutionPolicy | None = None


def _action(raw: Any) -> PolicyAction | None:
    return PolicyAction(raw) if raw is not None else None


def _policy(raw: dict[str, Any] | None) -> ExecutionPolicy | None:
    if raw is None:
        return None
    return ExecutionPolicy(
        unknown_backend=_action(raw.get("unknownBackend")),
        missing_tool=_action(raw.get("missingTool")),
        null_command=_action(raw.get("nullCommand")),
    )


def _slice(raw: dict[str, Any]) -> Slice:
    missing = [k for k in _REQUIRED_SLICE if k not in raw]
    if missing:
        raise PlanParseError(
            "build.plan-invalid",
            f"slice is missing required key(s): {', '.join(missing)}",
        )
    cmd = raw["command"]
    return Slice(
        core_id=raw["coreId"], backend=raw["backend"], build_dir=raw["buildDir"],
        app_dir=raw["appDir"], config_artefacts=raw["configArtefacts"],
        toolchain=raw["toolchain"], artifacts=raw["artifacts"], debug=raw["debug"],
        command=None if cmd is None else SliceCommand(
            tool=cmd["tool"], args=list(cmd.get("args", [])), cwd=cmd.get("cwd")
        ),
        env=dict(raw["env"]), env_append_path={k: list(v) for k, v in raw["envAppendPath"].items()},
    )


def parse_build_plan(text: str) -> BuildPlan:
    try:
        raw = json.loads(text)
    except ValueError as err:
        raise PlanParseError("build.plan-invalid", f"plan is not valid JSON: {err}") from err

    version = raw.get("schemaVersion")
    if version != SUPPORTED_SCHEMA_VERSION:
        raise PlanParseError(
            "build.plan-unsupported-schema",
            f"unsupported build-plan schemaVersion `{version}` (this tan supports "
            f"{SUPPORTED_SCHEMA_VERSION}) -- refusing rather than falling back to "
            f"hand-ported behaviour. Upgrade tan, or re-emit the plan.",
        )

    missing = [k for k in _REQUIRED_TOP if k not in raw]
    if missing:
        raise PlanParseError(
            "build.plan-invalid",
            f"plan is missing required key(s): {', '.join(missing)}",
        )

    return BuildPlan(
        schema_version=version, generated_by=raw["generatedBy"], board_yaml=raw["boardYaml"],
        sku=raw["sku"], build_root=raw["buildRoot"],
        slices=[_slice(s) for s in raw["slices"]],
        shared_artefacts=raw["sharedArtefacts"], warnings=raw["warnings"],
        sdk_version=raw.get("sdkVersion"), sdk_commit=raw.get("sdkCommit"),
        plan_path_mode=raw.get("planPathMode"), execution_policy=_policy(raw.get("executionPolicy")),
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd python && python -m pytest tests/core/test_build_plan.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Verify against a REAL emitted plan**

Run:
```bash
python -c "import sys; from tan.core.build_plan import parse_build_plan; p=parse_build_plan(sys.stdin.read()); print(p.sku, len(p.slices), p.plan_path_mode)"
```
piping a live plan from the alp-sdk checkout. The real invocation — note it is
`-m alp_orchestrate` with `--input`, NOT `--board-yaml` (that flag is `tan`'s,
not the planner's):
```bash
PYTHONPATH=scripts python -m alp_orchestrate --emit build-plan --input examples/multicore/rpmsg-v2n/board.yaml
```
Expected: parses without raising; prints the SKU, slice count, and `planPathMode`.

**Already confirmed (2026-07-29, reproduced twice):** a live plan for
`examples/multicore/rpmsg-v2n/board.yaml` emits `E1M-V2N101`, **2** slices, and
`planPathMode: "tokened"`. This settles a live contradiction in the sources:
ADR-0020 Amendment item 5 is correct, and the Rust comment at
`crates/tan-cli/src/commands/build/token_substitution.rs:58-59` — *"every plan
the SDK emits today has none"* — is **stale**. Task 4's token substitution is
therefore load-bearing, not a no-op path. Do not treat it as optional.

- [ ] **Step 6: Commit**

```bash
git add python/tan/core/build_plan.py python/tests/core/test_build_plan.py
git commit -m "feat(python): parse the build plan and enforce the version-skew guard"
```

---

### Task 4: Token substitution and its guards

**Files:**
- Create: `python/tan/core/plan_tokens.py`, `python/tan/commands/__init__.py`, `python/tan/commands/build/__init__.py`, `python/tan/commands/build/token_substitution.py`
- Test: `python/tests/core/test_plan_tokens.py`

**Interfaces:**
- Consumes: `BuildPlan`, `parse_build_plan`, `PlanParseError` from `tan.core.build_plan`.
- Produces: `PLAN_PATH_MODE_TOKENED = "tokened"`; `TokenValues(sdk_root, project_root, python, toolchain_root)`; `DemotedSlice(slice_index: int, core_id: str, field: str)`; `substitute_plan_tokens(plan, values) -> tuple[BuildPlan, list[DemotedSlice]]`; `sdk_commit_mismatches(plan_commit: str, resolved_commit: str) -> bool`; `project_root_diverges_from_exec_base(project_root: str, exec_base: str) -> bool`; and in `token_substitution.py`: `apply_plan_token_substitution(...) -> tuple[BuildPlan, list[SliceDemotion]]` raising `TokenSubstitutionError(code, message)`.

**Port note — the guards are the point, not the string replacement.** Each refusal below prevents a silent wrong-tree build. Reproduce every error code verbatim; they appear in envelopes the extension and the goldens match on:
`build.plan-invalid`, `build.project-root-mismatch`, `build.sdk-root-unresolved`, `build.sdk-commit-mismatch`, `build.plan-token-unresolved`, `build.toolchain-root-unresolved`.

> **CRITICAL — the field list below is the authoritative one; the reference code
> further down this task is INCOMPLETE and must not be treated as the spec.**
>
> `crates/tan-core/src/plan_tokens.rs` is the oracle. **Read it before writing
> anything** and derive the exact field set from `substitute_plan_tokens` /
> `substitute_slice` / `substitute_command_lenient` / `substitute_artefact`.
> Its own module doc states the rule: *"NO arg-parsing: every `command.args`
> entry and `cwd`, every slice `env` … `GeneratedFile`'s `path`/`contents`
> (config + shared artefacts)"* are substituted.
>
> Every one of these must be substituted:
>
> | Field | Note |
> |---|---|
> | `boardYaml` | plan level; hard error on unresolved `${TOOLCHAIN_ROOT}` (no slice to demote to) |
> | `slices[].buildDir` | |
> | `slices[].appDir` | |
> | `slices[].env.<KEY>` | |
> | **`slices[].envAppendPath.<KEY>[n]`** | **each list element.** Miss this and a literal `${SDK_ROOT}/scripts` lands on `PYTHONPATH` — a silently wrong build, not a crash |
> | **`slices[].command.cwd`** | |
> | `slices[].command.args[n]` | |
> | **`slices[].configArtefacts[n]`** | **`path` AND `contents`** — artefact bodies carry paths too |
> | **`sharedArtefacts[n]`** | **`path` AND `contents`** |
>
> Ordering matters: a slice's artefacts are substituted **with** the slice so a
> demoted slice's artefacts are stripped together with it; plan-level
> `sharedArtefacts` are substituted after all slices. Follow the Rust order.
>
> **Mandatory structural test — this is what catches a field nobody listed.**
> Beyond the per-field tests, add a test that takes a plan whose EVERY string
> field contains a token, runs the substitution, re-serialises the whole
> resulting plan to JSON, and asserts the literal substring `${` appears
> nowhere. A per-field test only proves the fields someone remembered; this
> sweep fails loudly on any field the port forgot. Treat a failure of this test
> as a missing field, never as a reason to relax the assertion.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/core/test_plan_tokens.py
# SPDX-License-Identifier: Apache-2.0
import pytest

from tan.core.build_plan import parse_build_plan
from tan.core.plan_tokens import (
    TokenValues,
    UnresolvedToolchainRoot,
    LeftoverToken,
    sdk_commit_mismatches,
    substitute_plan_tokens,
)

LEGACY = """{
  "schemaVersion": 1, "generatedBy": "g", "boardYaml": "/w/board.yaml", "sku": "S",
  "buildRoot": "build", "slices": [], "sharedArtefacts": [], "warnings": []
}"""


def _tokened(board_yaml: str, slices: str) -> str:
    return f"""{{
      "schemaVersion": 1, "generatedBy": "g", "planPathMode": "tokened",
      "boardYaml": "{board_yaml}", "sku": "S", "buildRoot": "build",
      "slices": [{slices}], "sharedArtefacts": [], "warnings": []
    }}"""


def _slice(env: str, core_id: str = "c1") -> str:
    return f"""{{
      "coreId": "{core_id}", "backend": "zephyr", "buildDir": "build/c1", "appDir": "app",
      "configArtefacts": [], "toolchain": null, "artifacts": [], "debug": {{}},
      "command": {{"tool": "west", "args": ["build"], "cwd": "build/c1"}},
      "env": {env}, "envAppendPath": {{}}
    }}"""


def values(**kw):
    base = dict(sdk_root="/sdk", project_root="/w", python="python3", toolchain_root=None)
    base.update(kw)
    return TokenValues(**base)


def test_legacy_plan_without_plan_path_mode_is_untouched():
    plan = parse_build_plan(LEGACY)
    out, demoted = substitute_plan_tokens(plan, values())
    assert out == plan
    assert demoted == []


def test_tokened_plan_substitutes_sdk_root_and_project_root():
    plan = parse_build_plan(_tokened("${PROJECT_ROOT}/board.yaml", _slice('{"ALP_SDK_ROOT": "${SDK_ROOT}"}')))
    out, demoted = substitute_plan_tokens(plan, values())
    assert out.board_yaml == "/w/board.yaml"
    assert out.slices[0].env["ALP_SDK_ROOT"] == "/sdk"
    assert demoted == []


def test_leftover_unknown_token_is_refused():
    plan = parse_build_plan(_tokened("${UNKNOWN}/board.yaml", ""))
    with pytest.raises(LeftoverToken) as e:
        substitute_plan_tokens(plan, values())
    assert "${UNKNOWN}" in str(e.value)


def test_plan_level_unresolved_toolchain_root_is_fatal():
    plan = parse_build_plan(_tokened("${TOOLCHAIN_ROOT}/board.yaml", ""))
    with pytest.raises(UnresolvedToolchainRoot):
        substitute_plan_tokens(plan, values(toolchain_root=None))


def test_slice_confined_unresolved_toolchain_root_is_demoted_not_fatal():
    """A slice-confined unresolved ${TOOLCHAIN_ROOT} has an owning slice AND a
    dispatch seam (executionPolicy.missingTool) to route to, so it must NOT fail
    the whole plan. The literal token survives in the never-dispatched slice."""
    plan = parse_build_plan(
        _tokened("${PROJECT_ROOT}/board.yaml", _slice('{"ZEPHYR_SDK_INSTALL_DIR": "${TOOLCHAIN_ROOT}"}', "m33_sm"))
    )
    out, demoted = substitute_plan_tokens(plan, values(toolchain_root=None))
    assert len(demoted) == 1
    assert demoted[0].slice_index == 0
    assert demoted[0].core_id == "m33_sm"
    assert demoted[0].field == "slices[0].env.ZEPHYR_SDK_INSTALL_DIR"
    assert out.slices[0].env["ZEPHYR_SDK_INSTALL_DIR"] == "${TOOLCHAIN_ROOT}"


def test_sdk_commit_mismatch_detection_treats_absent_as_no_signal():
    assert sdk_commit_mismatches("deadbee", "0000000") is True
    assert sdk_commit_mismatches("deadbee", "deadbee") is False
    # An SDK checkout with no .git (a release tarball) is a supported setup --
    # "could not resolve HEAD" is NO SIGNAL, never a mismatch.
    assert sdk_commit_mismatches("deadbee", "") is False
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd python && python -m pytest tests/core/test_plan_tokens.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tan.core.plan_tokens'`

- [ ] **Step 3: Write the pure pass**

```python
# python/tan/core/plan_tokens.py
# SPDX-License-Identifier: Apache-2.0
"""Pure build-plan token substitution (alp-sdk #865, hermetic build plans).

A plan is materialised on a machine other than the one that emitted it, so every
path it bakes is a token. Substituting a token with an empty string would sail
past the leftover-token guard and build against the wrong tree -- so an
unresolved token is REFUSED, never degraded."""
import re
from dataclasses import dataclass, replace
from typing import Any

from tan.core.build_plan import BuildPlan, Slice

PLAN_PATH_MODE_TOKENED = "tokened"
_TOKEN_RE = re.compile(r"\$\{([A-Z_]+)\}")


@dataclass(frozen=True)
class TokenValues:
    sdk_root: str
    project_root: str
    python: str
    toolchain_root: str | None


@dataclass(frozen=True)
class DemotedSlice:
    slice_index: int
    core_id: str
    field: str


class PlanTokenError(Exception):
    pass


class LeftoverToken(PlanTokenError):
    def __init__(self, field: str, token: str) -> None:
        super().__init__(
            f"plan is `planPathMode: tokened` but field `{field}` still names the literal "
            f"token `{token}` after substitution -- an SDK-side token this CLI does not "
            f"resolve (only ${{SDK_ROOT}}, ${{PROJECT_ROOT}}, ${{PYTHON}}, "
            f"${{TOOLCHAIN_ROOT}} are known). Upgrade tan, or check the plan for a bug."
        )
        self.field, self.token = field, token


class UnresolvedToolchainRoot(PlanTokenError):
    def __init__(self, field: str) -> None:
        super().__init__(f"plan field `{field}` names ${{TOOLCHAIN_ROOT}}")
        self.field = field


class UnknownPlanPathMode(PlanTokenError):
    def __init__(self, mode: str) -> None:
        super().__init__(f'unknown planPathMode `{mode}` (only "tokened" is defined)')
        self.mode = mode


def sdk_commit_mismatches(plan_commit: str, resolved_commit: str) -> bool:
    """An empty resolved commit is NO SIGNAL (an SDK release tarball has no
    .git), never a mismatch."""
    if not plan_commit or not resolved_commit:
        return False
    return plan_commit != resolved_commit


def project_root_diverges_from_exec_base(project_root: str, exec_base: str) -> bool:
    return project_root.replace("\\", "/").rstrip("/") != exec_base.replace("\\", "/").rstrip("/")


def _sub(value: str, values: TokenValues, field: str, demotions: list[str]) -> str:
    mapping = {
        "SDK_ROOT": values.sdk_root,
        "PROJECT_ROOT": values.project_root,
        "PYTHON": values.python,
        "TOOLCHAIN_ROOT": values.toolchain_root,
    }
    out = value
    for token in _TOKEN_RE.findall(value):
        if token not in mapping:
            raise LeftoverToken(field, f"${{{token}}}")
        resolved = mapping[token]
        if resolved is None:
            # Only TOOLCHAIN_ROOT can be None; leave the literal token in place
            # and let the caller decide fatal-vs-demote by scope.
            demotions.append(field)
            continue
        out = out.replace(f"${{{token}}}", resolved)
    return out


def substitute_plan_tokens(
    plan: BuildPlan, values: TokenValues
) -> tuple[BuildPlan, list[DemotedSlice]]:
    if plan.plan_path_mode is None:
        return plan, []
    if plan.plan_path_mode != PLAN_PATH_MODE_TOKENED:
        raise UnknownPlanPathMode(plan.plan_path_mode)

    # Plan-level fields: an unresolved TOOLCHAIN_ROOT here has no owning slice
    # to demote, so it is fatal.
    plan_level: list[str] = []
    board_yaml = _sub(plan.board_yaml, values, "boardYaml", plan_level)
    if plan_level:
        raise UnresolvedToolchainRoot(plan_level[0])

    demoted: list[DemotedSlice] = []
    new_slices: list[Slice] = []
    for i, sl in enumerate(plan.slices):
        slice_demotions: list[str] = []
        env = {
            k: _sub(v, values, f"slices[{i}].env.{k}", slice_demotions)
            for k, v in sl.env.items()
        }
        app_dir = _sub(sl.app_dir, values, f"slices[{i}].appDir", slice_demotions)
        command = sl.command
        if command is not None:
            command = replace(
                command,
                args=[
                    _sub(a, values, f"slices[{i}].command.args[{j}]", slice_demotions)
                    for j, a in enumerate(command.args)
                ],
            )
        new_slices.append(replace(sl, env=env, app_dir=app_dir, command=command))
        for f in slice_demotions:
            demoted.append(DemotedSlice(slice_index=i, core_id=sl.core_id, field=f))

    return replace(plan, board_yaml=board_yaml, slices=new_slices), demoted
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd python && python -m pytest tests/core/test_plan_tokens.py -v`
Expected: PASS (6 tests)

- [ ] **Step 5: Write the IO half**

```python
# python/tan/commands/build/token_substitution.py
# SPDX-License-Identifier: Apache-2.0
"""The IO/glue side of token substitution: resolves the SDK root, project root
and planner python, drives the pure pass, and runs the one guard that pass
cannot do itself -- comparing the plan's sdkCommit against the resolved SDK
checkout's actual git HEAD (a subprocess call)."""
import subprocess
from pathlib import Path

from tan.core.plan_tokens import (
    LeftoverToken,
    PLAN_PATH_MODE_TOKENED,
    TokenValues,
    UnknownPlanPathMode,
    UnresolvedToolchainRoot,
    project_root_diverges_from_exec_base,
    sdk_commit_mismatches,
    substitute_plan_tokens,
)


class TokenSubstitutionError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code, self.message = code, message


def git_short_head(sdk_root: Path) -> str:
    """`git rev-parse --short HEAD`. Empty string when git is missing or the
    checkout has no .git -- all NO SIGNAL, never a hard failure."""
    try:
        out = subprocess.run(
            ["git", "-C", str(sdk_root), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


def apply_plan_token_substitution(plan, *, board_yaml_path, exec_base, sdk_root,
                                  python, toolchain_root):
    if plan.plan_path_mode is None:
        return plan, []
    if plan.plan_path_mode != PLAN_PATH_MODE_TOKENED:
        raise TokenSubstitutionError(
            "build.plan-invalid",
            f"unknown planPathMode `{plan.plan_path_mode}` (only \"tokened\" is defined) -- "
            f"refusing rather than silently treating it as legacy or applying token substitution",
        )

    if board_yaml_path is None:
        raise TokenSubstitutionError(
            "build.plan-invalid",
            "a `planPathMode: tokened` plan needs a resolved board.yaml to derive "
            "${PROJECT_ROOT} from -- pass `--board-yaml <PATH>` or run from a project.",
        )
    project_root = str(Path(board_yaml_path).parent).replace("\\", "/")

    if project_root_diverges_from_exec_base(project_root, exec_base):
        raise TokenSubstitutionError(
            "build.project-root-mismatch",
            f"plan is `planPathMode: tokened`, but ${{PROJECT_ROOT}} (`{project_root}`, the "
            f"board.yaml directory) differs from where tan actually runs each slice's command "
            f"(`{exec_base}`) -- refusing to build against the wrong tree.",
        )

    if sdk_root is None:
        raise TokenSubstitutionError(
            "build.sdk-root-unresolved",
            "plan is `planPathMode: tokened` (needs ${SDK_ROOT} substituted with a real path), "
            "but no alp-sdk checkout resolved -- pass `--sdk-root <PATH>`, pin one with "
            "`tan sdk switch`, or run from a project near an alp-sdk checkout.",
        )

    if plan.sdk_commit:
        resolved = git_short_head(Path(sdk_root))
        if sdk_commit_mismatches(plan.sdk_commit, resolved):
            raise TokenSubstitutionError(
                "build.sdk-commit-mismatch",
                f"plan was emitted from alp-sdk commit `{plan.sdk_commit}`, but the resolved "
                f"SDK checkout is at `{resolved}` -- building against a different SDK checkout "
                f"than the plan was captured from can silently produce the wrong image.",
            )

    values = TokenValues(sdk_root=str(sdk_root), project_root=project_root,
                         python=python, toolchain_root=toolchain_root)
    try:
        return substitute_plan_tokens(plan, values)
    except LeftoverToken as e:
        raise TokenSubstitutionError("build.plan-token-unresolved", str(e)) from e
    except UnresolvedToolchainRoot as e:
        raise TokenSubstitutionError("build.toolchain-root-unresolved", str(e)) from e
    except UnknownPlanPathMode as e:
        raise TokenSubstitutionError("build.plan-invalid", str(e)) from e
```

Also create empty `python/tan/commands/__init__.py` and `python/tan/commands/build/__init__.py`.

- [ ] **Step 6: Cross-check against the Rust tests**

Run: `cargo test -p tan-cli token_substitution`
Expected: PASS. Read `crates/tan-cli/src/commands/build/token_substitution.rs` tests; confirm each has a Python twin. Add any missing case.

- [ ] **Step 7: Commit**

```bash
git add python/tan/core/plan_tokens.py python/tan/commands/ python/tests/core/test_plan_tokens.py
git commit -m "feat(python): port build-plan token substitution and its refusal guards"
```

---

### Task 5: Materialise artefacts and execute slices

**Files:**
- Create: `python/tan/commands/build/materialise.py`, `python/tan/commands/build/execute.py`
- Test: `python/tests/commands/test_materialise.py`, `python/tests/commands/test_execute.py`

**Interfaces:**
- Consumes: `BuildPlan`, `Slice` (`tan.core.build_plan`); `assemble_slice_env`, `resolve_action`, `PolicyAction`, `sdk_stamp_action`, `SdkStampAction` (`tan.core.plan_exec`).
- Produces: `materialise_plan(plan: BuildPlan, build_root: Path) -> list[Path]`; `SliceOutcome(core_id: str, status: str, exit_code: int | None, message: str | None)` where `status` is one of `"succeeded" | "skipped" | "failed"`; `execute_slices(plan, *, build_root, env_lookup, gap_fillers, on_output) -> list[SliceOutcome]`.

**Port note — ordering is a contract property.** ALL `sharedArtefacts` and every slice's `configArtefacts` must be written **before any slice runs**. That is the stated precondition of the plan's slice-independence invariant; violating it makes concurrent execution unsafe.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/commands/test_materialise.py
# SPDX-License-Identifier: Apache-2.0
from tan.core.build_plan import parse_build_plan
from tan.commands.build.materialise import materialise_plan

PLAN = """{
  "schemaVersion": 1, "generatedBy": "g", "boardYaml": "/w/board.yaml", "sku": "S",
  "buildRoot": "build", "warnings": [],
  "sharedArtefacts": [{"path": "shared/alp.conf", "contents": "CONFIG_A=y\\n"}],
  "slices": [{
    "coreId": "c1", "backend": "zephyr", "buildDir": "build/c1", "appDir": "app",
    "configArtefacts": [{"path": "build/c1-zephyr/alp.conf", "contents": "CONFIG_B=y\\n"}],
    "toolchain": null, "artifacts": [], "debug": {},
    "command": {"tool": "west", "args": ["build"], "cwd": "build/c1"},
    "env": {}, "envAppendPath": {}
  }]
}"""


def test_writes_shared_and_config_artefacts_with_exact_contents(tmp_path):
    written = materialise_plan(parse_build_plan(PLAN), tmp_path)
    shared = tmp_path / "shared/alp.conf"
    config = tmp_path / "build/c1-zephyr/alp.conf"
    assert shared.read_text() == "CONFIG_A=y\n"
    assert config.read_text() == "CONFIG_B=y\n"
    assert set(written) == {shared, config}


def test_refuses_to_escape_the_build_root(tmp_path):
    """Plans are trusted input, but writes stay confined under buildRoot."""
    evil = PLAN.replace('"shared/alp.conf"', '"../escaped.conf"')
    try:
        materialise_plan(parse_build_plan(evil), tmp_path)
    except ValueError as e:
        assert "escape" in str(e).lower()
    else:
        raise AssertionError("must refuse a path escaping the build root")
```

```python
# python/tests/commands/test_execute.py
# SPDX-License-Identifier: Apache-2.0
import sys

from tan.core.build_plan import parse_build_plan
from tan.commands.build.execute import execute_slices


def _plan(command: str, backend: str = "zephyr") -> str:
    return f"""{{
      "schemaVersion": 1, "generatedBy": "g", "boardYaml": "/w/board.yaml", "sku": "S",
      "buildRoot": "build", "sharedArtefacts": [], "warnings": [],
      "executionPolicy": {{"missingTool": "skip", "nullCommand": "skip", "unknownBackend": "fail"}},
      "slices": [{{
        "coreId": "c1", "backend": "{backend}", "buildDir": "build/c1", "appDir": "app",
        "configArtefacts": [], "toolchain": null, "artifacts": [], "debug": {{}},
        "command": {command}, "env": {{}}, "envAppendPath": {{}}
      }}]
    }}"""


def test_successful_slice_reports_succeeded(tmp_path):
    cmd = f'{{"tool": "{sys.executable}", "args": ["-c", "print(1)"], "cwd": null}}'
    out = execute_slices(parse_build_plan(_plan(cmd)), build_root=tmp_path,
                         env_lookup=lambda k: None, gap_fillers=[], on_output=lambda s: None)
    assert out[0].status == "succeeded"
    assert out[0].exit_code == 0


def test_failing_slice_reports_failed_with_exit_code(tmp_path):
    cmd = f'{{"tool": "{sys.executable}", "args": ["-c", "raise SystemExit(3)"], "cwd": null}}'
    out = execute_slices(parse_build_plan(_plan(cmd)), build_root=tmp_path,
                         env_lookup=lambda k: None, gap_fillers=[], on_output=lambda s: None)
    assert out[0].status == "failed"
    assert out[0].exit_code == 3


def test_null_command_is_skipped_per_policy(tmp_path):
    out = execute_slices(parse_build_plan(_plan("null")), build_root=tmp_path,
                         env_lookup=lambda k: None, gap_fillers=[], on_output=lambda s: None)
    assert out[0].status == "skipped"


def test_missing_tool_is_skipped_per_policy(tmp_path):
    cmd = '{"tool": "definitely-not-a-real-tool-xyz", "args": [], "cwd": null}'
    out = execute_slices(parse_build_plan(_plan(cmd)), build_root=tmp_path,
                         env_lookup=lambda k: None, gap_fillers=[], on_output=lambda s: None)
    assert out[0].status == "skipped"


def test_unknown_backend_fails_per_policy(tmp_path):
    cmd = f'{{"tool": "{sys.executable}", "args": ["-c", "pass"], "cwd": null}}'
    out = execute_slices(parse_build_plan(_plan(cmd, backend="martian")),
                         build_root=tmp_path, env_lookup=lambda k: None,
                         gap_fillers=[], on_output=lambda s: None)
    assert out[0].status == "failed"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd python && python -m pytest tests/commands/ -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'tan.commands.build.materialise'`

- [ ] **Step 3: Write the implementation**

```python
# python/tan/commands/build/materialise.py
# SPDX-License-Identifier: Apache-2.0
"""Write every artefact the plan carries BEFORE any slice runs -- the stated
precondition of the plan's slice-independence invariant."""
from pathlib import Path


def _resolve_confined(build_root: Path, rel: str) -> Path:
    """Plans are trusted input, but writes stay confined under buildRoot."""
    target = (build_root / rel).resolve()
    root = build_root.resolve()
    if root != target and root not in target.parents:
        raise ValueError(f"artefact path `{rel}` would escape the build root `{root}`")
    return target


def _write(build_root: Path, artefact: dict) -> Path:
    target = _resolve_confined(build_root, artefact["path"])
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(artefact.get("contents", ""), encoding="utf-8", newline="")
    return target


def materialise_plan(plan, build_root: Path) -> list[Path]:
    written = [_write(build_root, a) for a in plan.shared_artefacts]
    for sl in plan.slices:
        written.extend(_write(build_root, a) for a in sl.config_artefacts)
    return written
```

```python
# python/tan/commands/build/execute.py
# SPDX-License-Identifier: Apache-2.0
"""Dispatch each slice: assemble env, apply the execution policy, spawn, stream."""
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tan.core.plan_exec import PolicyAction, assemble_slice_env, resolve_action

KNOWN_BACKENDS = frozenset({"zephyr", "baremetal", "yocto", "native"})


@dataclass(frozen=True)
class SliceOutcome:
    core_id: str
    status: str  # "succeeded" | "skipped" | "failed"
    exit_code: int | None
    message: str | None


def _skip_or_fail(core_id: str, action: PolicyAction, message: str) -> SliceOutcome:
    status = "skipped" if action is PolicyAction.SKIP else "failed"
    return SliceOutcome(core_id, status, None, message)


def execute_slices(plan, *, build_root: Path, env_lookup, gap_fillers, on_output) -> list[SliceOutcome]:
    policy = plan.execution_policy
    outcomes: list[SliceOutcome] = []

    for sl in plan.slices:
        if sl.backend not in KNOWN_BACKENDS:
            outcomes.append(_skip_or_fail(
                sl.core_id, resolve_action(policy, "unknown_backend", PolicyAction.FAIL),
                f"unknown backend `{sl.backend}`"))
            continue

        if sl.command is None:
            outcomes.append(_skip_or_fail(
                sl.core_id, resolve_action(policy, "null_command", PolicyAction.SKIP),
                f"slice `{sl.core_id}` has no command"))
            continue

        tool = sl.command.tool
        if shutil.which(tool) is None and not Path(tool).exists():
            outcomes.append(_skip_or_fail(
                sl.core_id, resolve_action(policy, "missing_tool", PolicyAction.SKIP),
                f"tool `{tool}` not found"))
            continue

        env = dict(os.environ)
        env.update(dict(assemble_slice_env(sl.env, sl.env_append_path, env_lookup, gap_fillers)))
        cwd = build_root / sl.command.cwd if sl.command.cwd else build_root
        cwd.mkdir(parents=True, exist_ok=True)

        proc = subprocess.Popen(
            [tool, *sl.command.args], cwd=str(cwd), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            on_output(line.rstrip("\n"))
        code = proc.wait()

        outcomes.append(SliceOutcome(
            sl.core_id, "succeeded" if code == 0 else "failed", code,
            None if code == 0 else f"slice `{sl.core_id}` terminated with exit code: {code}",
        ))

    return outcomes
```

Also create empty `python/tests/commands/__init__.py`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd python && python -m pytest tests/commands/ -v`
Expected: PASS (7 tests)

- [ ] **Step 5: Add cancellation (a spec acceptance criterion, not optional)**

ADR-0020 claims Rust owns cancellation natively; the spec commits to proving
Python matches. Add a `cancelled` predicate to `execute_slices` and a test:

```python
# append to python/tests/commands/test_execute.py
def test_cancellation_terminates_a_running_slice(tmp_path):
    """Cancellation is a spec acceptance criterion -- a long slice must be
    stopped, not waited out."""
    cmd = f'{{"tool": "{sys.executable}", "args": ["-c", "import time; time.sleep(60)"], "cwd": null}}'
    out = execute_slices(parse_build_plan(_plan(cmd)), build_root=tmp_path,
                         env_lookup=lambda k: None, gap_fillers=[],
                         on_output=lambda s: None, cancelled=lambda: True)
    assert out[0].status == "cancelled"
```

Thread `cancelled: Callable[[], bool] = lambda: False` through `execute_slices`;
poll it while draining stdout, and on trip call `proc.terminate()`, wait briefly,
then `proc.kill()`. Return `SliceOutcome(core_id, "cancelled", None, ...)`.

Run: `cd python && python -m pytest tests/commands/test_execute.py -v`
Expected: PASS (6 tests), and the cancellation test finishes in ~1 s, not 60 s.

- [ ] **Step 6: Commit**

```bash
git add python/tan/commands/build/ python/tests/commands/
git commit -m "feat(python): materialise plan artefacts and dispatch slices under the execution policy"
```

---

### Task 6: Contract-golden conformance

**Files:**
- Create: `python/tests/conformance/test_contract_envelopes.py`
- Read (do not modify): `contract/envelopes/*/{args.txt,expected.exit,expected.json}`, `contract/README.md`

**Interfaces:**
- Consumes: the `tan` CLI entrypoint from Task 1 and everything wired since.
- Produces: nothing consumed downstream — this is a gate.

**Why this task matters most.** `contract/envelopes/` is a **language-agnostic conformance suite already committed to the repo** — 11 fixtures, each an `args.txt`, an `expected.exit`, and an `expected.json`. It is the cheapest, sharpest proof that the Python CLI is byte-compatible with the Rust one. Read `contract/README.md` first — it defines how the Rust side runs these, and the Python runner must apply the **same** normalisation (host paths, ordering) or it will produce false diffs.

- [ ] **Step 1: Read the contract README and the Rust runner**

Run: `cat contract/README.md` and locate the Rust test that consumes these fixtures (`grep -rn "contract/envelopes" --include=*.rs .`).
Record: how `args.txt` is split into argv, which fields are normalised, and how the working directory is chosen.

- [ ] **Step 2: Write the failing conformance test**

```python
# python/tests/conformance/test_contract_envelopes.py
# SPDX-License-Identifier: Apache-2.0
"""Run the committed contract/envelopes fixtures against the PYTHON tan and
assert byte-compatibility with the recorded expectations. Same fixtures the Rust
binary is held to -- this is the cross-language conformance gate."""
import json
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

CONTRACT = Path(__file__).resolve().parents[3] / "contract" / "envelopes"
FIXTURES = sorted(p for p in CONTRACT.iterdir() if p.is_dir()) if CONTRACT.is_dir() else []


@pytest.mark.parametrize("fixture", FIXTURES, ids=lambda p: p.name)
def test_envelope_matches_expected(fixture: Path):
    argv = shlex.split((fixture / "args.txt").read_text().strip())
    expected_exit = int((fixture / "expected.exit").read_text().strip())
    expected = json.loads((fixture / "expected.json").read_text())

    proc = subprocess.run(
        [sys.executable, "-m", "tan", *argv],
        capture_output=True, text=True, cwd=fixture,
    )

    assert proc.returncode == expected_exit, f"stderr: {proc.stderr}"
    actual = json.loads(proc.stdout)
    assert actual == expected
```

- [ ] **Step 3: Run it and record the true baseline**

Run: `cd python && python -m pytest tests/conformance/ -v`
Expected: FAIL for every fixture whose command is not yet implemented.

**This is expected and is the point.** Record which fixtures pass. The MVP's scope is `build`; fixtures for `init`/`sdk`/`generate`/`debug-config`/`examples` belong to later sub-projects. Mark those `pytest.mark.xfail(reason="command lands in sub-project 3")` **individually and by name** — never skip the whole suite, and never weaken the assertion.

- [ ] **Step 4: Make the in-scope fixtures pass**

Fix real mismatches in the Python implementation, not the test. A diff here is a genuine contract break.

- [ ] **Step 5: Commit**

```bash
git add python/tests/conformance/
git commit -m "test(python): hold the Python CLI to the committed contract envelope goldens"
```

---

### Task 7: PyInstaller one-file packaging

**Files:**
- Create: `python/scripts/build_binary.sh`
- Test: `python/tests/conformance/test_packaged_binary.py`

**Interfaces:**
- Consumes: the `tan` entrypoint.
- Produces: `dist/tan` (or `dist/tan.exe`) — a single executable.

**Hard requirement: `--onefile`, never `--onedir`.** The extension downloads a raw binary straight to one cached path and has **no unpack step** (`alp-sdk-vscode/src/alpCli/download.ts:159-162`; `service.ts:295` — *"tan-cli ships a RAW binary per target (not an archive)"*). A one-dir artifact cannot be consumed by it at all.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/conformance/test_packaged_binary.py
# SPDX-License-Identifier: Apache-2.0
"""The packaged artifact must satisfy the extension's own probe: a single file
whose `--version` first line matches /^tan \\d+\\.\\d+\\.\\d+/, answering inside
the extension's 3 s budget (alp-sdk-vscode/src/alpCli/vscodeAdapter.ts:288-290)."""
import re
import subprocess
import time
from pathlib import Path

import pytest

BINARY = Path(__file__).resolve().parents[2] / "dist" / ("tan.exe" if __import__("sys").platform == "win32" else "tan")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="run scripts/build_binary.sh first")


def test_artifact_is_a_single_file():
    assert BINARY.is_file(), "must be --onefile: the extension cannot unpack a directory"


def test_version_line_matches_the_extension_regex():
    out = subprocess.run([str(BINARY), "--version"], capture_output=True, text=True)
    first = out.stdout.splitlines()[0]
    assert re.match(r"^tan \d+\.\d+\.\d+", first), f"got: {first!r}"


def test_version_probe_completes_within_the_3s_budget():
    start = time.monotonic()
    subprocess.run([str(BINARY), "--version"], capture_output=True, text=True, timeout=3)
    elapsed = time.monotonic() - start
    assert elapsed < 3.0, f"--version took {elapsed:.2f}s; extension probe timeout is 3s"
    print(f"\nstartup: {elapsed:.3f}s")
```

- [ ] **Step 2: Run it to verify it skips**

Run: `cd python && python -m pytest tests/conformance/test_packaged_binary.py -v`
Expected: SKIPPED — `run scripts/build_binary.sh first`

- [ ] **Step 3: Add the build script**

```bash
# python/scripts/build_binary.sh
#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the single-file `tan` executable. --onefile is REQUIRED: the VS Code
# extension downloads a raw binary to one path and has no unpack step.
set -euo pipefail
cd "$(dirname "$0")/.."
python -m PyInstaller --onefile --name tan --clean --noconfirm \
  --console --distpath dist --workpath .build tan/__main__.py
```

- [ ] **Step 4: Build and run the tests**

Run: `cd python && bash scripts/build_binary.sh && python -m pytest tests/conformance/test_packaged_binary.py -v -s`
Expected: PASS (3 tests). **Record the printed startup time** — if it approaches 3 s, that is a real finding for the spec, not something to paper over.

- [ ] **Step 5: Verify the artifact runs a real build**

Run: `dist/tan build --native --sdk-root <alp-sdk> --board-yaml examples/multicore/rpmsg-v2n/board.yaml`
Expected: a real `zephyr.elf` + `zephyr.bin`, matching the verified Rust e2e.

- [ ] **Step 6: Commit**

```bash
git add python/tan.spec python/scripts/build_binary.sh python/tests/conformance/test_packaged_binary.py
git commit -m "build(python): package tan as a single-file executable the extension can consume"
```

---

### Task 8: The Rust oracle parity harness

**Files:**
- Create: `python/tests/parity/oracle.py`, `python/tests/parity/test_oracle_parity.py`

**Interfaces:**
- Consumes: the packaged Python binary and a Rust `tan` binary on PATH or at `TAN_RUST_BINARY`.
- Produces: `compare(argv: list[str], cwd: Path) -> ParityResult(matches: bool, diffs: list[str])`.

**Why:** Rust `tan` is shipped and correct. Diffing against it is what removes the "port with no reference" risk — it is the direct replacement for the `fan_out` oracle that Phase 4 deleted. Rust `tan` is retired only for capabilities this harness has confirmed.

> **Parity must be defined PER SURFACE — a naive whole-plan diff will fail for a
> reason that is not a port bug.** Established while reviewing Task 4:
> `crates/tan-core/src/build_plan.rs:138-164`'s `BuildSlice` does **not** model
> `appDir`, `toolchain`, `artifacts` or `debug`, and
> `crates/tan-cli/src/commands/build/plan_modes.rs:234` says so outright — the
> typed struct would "drop them and emit a schema-invalid plan", which is why
> Rust's `--plan --format json` passes **raw, unsubstituted** JSON through
> instead of re-serialising.
>
> So for the `build --plan` case: Rust emits the raw plan; Python models and
> substitutes four keys Rust never touches. Diffing those two whole documents
> compares different things and always differs.
>
> **Do not "fix" Python to match.** The schema requires those fields and names
> `slices[].appDir` among the tokened ones — Python is correct and the
> checked-out Rust is behind. Either scope this case's parity to the surface
> both actually produce (envelope shape, exit code, and the keys Rust models),
> or add `app_dir` to the Rust struct first. Record which was chosen.
>
> The `--version` and `validate` cases are unaffected and remain strict.

- [ ] **Step 1: Write the failing test**

```python
# python/tests/parity/test_oracle_parity.py
# SPDX-License-Identifier: Apache-2.0
"""Diff the Python tan against the shipped Rust tan on identical inputs. Any
divergence is a port bug -- Rust is authoritative until a capability is confirmed."""
import os
from pathlib import Path

import pytest

from .oracle import compare

RUST = os.environ.get("TAN_RUST_BINARY")
pytestmark = pytest.mark.skipif(not RUST, reason="set TAN_RUST_BINARY to the Rust tan")

CASES = [
    ["--version"],
    ["validate", "--format", "json"],
    ["build", "--plan", "--format", "json"],
]


@pytest.mark.parametrize("argv", CASES, ids=lambda a: " ".join(a))
def test_python_matches_rust(argv, tmp_path):
    result = compare(argv, cwd=tmp_path)
    assert result.matches, "\n".join(result.diffs)
```

```python
# python/tests/parity/oracle.py
# SPDX-License-Identifier: Apache-2.0
"""Run the same argv through both binaries and diff exit code + envelope."""
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ParityResult:
    matches: bool
    diffs: list[str]


def _run(binary: list[str], argv: list[str], cwd: Path):
    proc = subprocess.run([*binary, *argv], capture_output=True, text=True, cwd=cwd)
    try:
        payload = json.loads(proc.stdout)
    except ValueError:
        payload = {"__raw__": proc.stdout.strip()}
    return proc.returncode, payload


def compare(argv: list[str], cwd: Path) -> ParityResult:
    rust = os.environ["TAN_RUST_BINARY"]
    r_code, r_out = _run([rust], argv, cwd)
    p_code, p_out = _run([sys.executable, "-m", "tan"], argv, cwd)

    diffs: list[str] = []
    if r_code != p_code:
        diffs.append(f"exit code: rust={r_code} python={p_code}")
    for key in sorted(set(r_out) | set(p_out)):
        if r_out.get(key) != p_out.get(key):
            diffs.append(f"{key}: rust={r_out.get(key)!r} python={p_out.get(key)!r}")
    return ParityResult(not diffs, diffs)
```

- [ ] **Step 2: Run it to verify it skips without the oracle**

Run: `cd python && python -m pytest tests/parity/ -v`
Expected: SKIPPED — `set TAN_RUST_BINARY to the Rust tan`

- [ ] **Step 3: Build the Rust oracle and run for real**

Run:
```bash
cargo build --release && TAN_RUST_BINARY=target/release/tan python -m pytest python/tests/parity/ -v
```
Expected: initially FAIL with a concrete field-by-field diff.

- [ ] **Step 4: Close every diff in the Python implementation**

Fix Python, never the oracle. Each closed diff is a proven-equivalent capability.

- [ ] **Step 5: Commit**

```bash
git add python/tests/parity/
git commit -m "test(python): diff the Python CLI against the shipped Rust binary as the port oracle"
```

---

## Verification (run before calling the MVP done)

- [ ] `cd python && python -m pytest tests/ -v` — all unit, conformance and parity tests green
- [ ] `cargo fmt --all --check && cargo clippy --all-targets -- -D warnings && cargo build --all-targets && cargo test` — **the Rust workspace must still be green; this port does not touch it**
- [ ] `bash python/scripts/build_binary.sh` produces a single file; `--version` matches `/^tan \d+\.\d+\.\d+/` within 3 s
- [ ] `dist/tan build --native ...` produces a real `zephyr.elf` + `zephyr.bin`
- [ ] Startup overhead recorded in the spec

## Findings to feed back into the spec

Record these as you go — they change sub-projects 2-4:

1. The **real `planPathMode` of a live plan** (Task 3 Step 5). `token_substitution.rs:58-59` claims "every plan the SDK emits today has none", but ADR-0020 Amendment item 5 says the emit is now `tokened`. One of the two is stale.
2. **Which contract fixtures are out of MVP scope** (Task 6 Step 3) — that list is sub-project 3's real backlog.
3. **PyInstaller startup time** (Task 7 Step 4) against the 3 s probe budget.
4. Any **Rust test with no Python twin** found in Tasks 2 and 4 cross-checks.
