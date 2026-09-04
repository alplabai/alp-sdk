# SPDX-License-Identifier: Apache-2.0
"""`do_run` must RAISE on a non-zero rc, never return it (#1427).

west's `WestApp.run_extension` calls `do_run` for effect and derives the
process exit status from exceptions only -- a plain `return rc` is silently a
no-op, so `west alp-quality --profile pr` printed `[FAIL] ...` and still exited
0. `west.commands.CommandError(returncode)` is the mechanism west actually
reads.

The existing `test_alp_quality.py` / `test_alp_lock.py` / `test_alp_migrate.py`
do NOT cover this: they exercise each SCRIPT's `main()` exit code, which was
always correct. The defect lived only in the west wrapper layer, so those
tests pass identically before and after the fix. Hence this file.

`run()` is patched rather than executed so the assertion is about the wrapper's
rc plumbing alone, independent of whether the live gates pass on this OS.
"""
import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
WEST_COMMANDS = REPO / "scripts" / "west_commands"
sys.path.insert(0, str(REPO / "scripts"))


def _load(module_name):
    """Import a wrapper by path -- `scripts/west_commands/` is not a package."""
    spec = importlib.util.spec_from_file_location(
        f"_wrapper_{module_name}", WEST_COMMANDS / f"{module_name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _stub_run(mod, monkeypatch, rc):
    """Three of the four wrappers call a module-level `run()`; patching it
    keeps the assertion about rc plumbing alone, independent of whether the
    live gates pass on this OS. Returns the `args` do_run should be given."""
    monkeypatch.setattr(mod, "run", lambda _args: rc)
    return object()


def _stub_emit(mod, monkeypatch, rc):
    """`alp_emit` has no module-level `run()` -- it shells the orchestrator --
    so the call to intercept is `subprocess.call`. Its two `log.die()`
    pre-flight probes run first and would exit the interpreter outside a
    workspace, so both are stubbed too; the board.yaml path is only
    stringified into the child argv, which never runs, so any existing file
    serves. Returns the `args` namespace do_run reads."""
    monkeypatch.setattr(mod, "find_sdk_root", lambda: REPO)
    monkeypatch.setattr(mod, "resolve_board_yaml", lambda *_a: REPO / "README.md")
    monkeypatch.setattr(mod.subprocess, "call", lambda *_a, **_k: rc)
    return SimpleNamespace(mode="build-plan", board_yaml=None, build_root=None, core=None)


# (module, class, stub). `alp_emit` was missed by #1427 and its own fix, #1476,
# is what added the fourth row -- see test_every_registered_west_command_is_covered.
WRAPPERS = [("alp_quality", "AlpQuality", _stub_run),
            ("alp_lock", "AlpLock", _stub_run),
            ("alp_migrate", "AlpMigrate", _stub_run),
            ("alp_emit", "AlpEmit", _stub_emit)]


@pytest.mark.parametrize("module_name,class_name,stub", WRAPPERS)
@pytest.mark.parametrize("rc", [1, 2, 99])
def test_do_run_raises_command_error_on_failure(module_name, class_name, stub, rc,
                                                monkeypatch):
    """A non-zero rc must surface as CommandError carrying that same code."""
    mod = _load(module_name)
    args = stub(mod, monkeypatch, rc)

    with pytest.raises(mod.CommandError) as excinfo:
        getattr(mod, class_name)().do_run(args, [])

    assert excinfo.value.returncode == rc, (
        f"{module_name}.do_run raised CommandError but with returncode "
        f"{excinfo.value.returncode!r}, not the {rc!r} run() reported -- west "
        f"exits with whatever the exception carries, so a wrong code here is "
        f"as bad as no exception")


@pytest.mark.parametrize("module_name,class_name,stub", WRAPPERS)
def test_do_run_stays_quiet_on_success(module_name, class_name, stub, monkeypatch):
    """rc 0 must NOT raise -- a fix that made these always fail would be worse
    than the bug it replaced."""
    mod = _load(module_name)
    args = stub(mod, monkeypatch, 0)

    assert getattr(mod, class_name)().do_run(args, []) == 0


def test_every_registered_west_command_is_covered() -> None:
    """Guard against the guard, and the reason #1476 existed at all.

    `scripts/west-commands.yml` registers four commands; `WRAPPERS` listed
    three, so `alp-emit` kept a bare `return subprocess.call(...)` for two
    releases after #1427 fixed the identical defect in its siblings, and no
    test went red. Nothing else cross-checks the registry against this list.
    """
    registry = yaml.safe_load((REPO / "scripts" / "west-commands.yml").read_text(encoding="utf-8"))
    registered = {
        (Path(entry["file"]).stem, command["class"])
        for entry in registry["west-commands"]
        for command in entry["commands"]
    }
    covered = {(module_name, class_name) for module_name, class_name, _stub in WRAPPERS}
    assert registered == covered, (
        f"scripts/west-commands.yml and WRAPPERS have drifted -- registered but "
        f"untested: {sorted(registered - covered)}; tested but unregistered: "
        f"{sorted(covered - registered)}. Every west extension command derives its "
        f"exit status from a raised CommandError, so an uncovered wrapper can exit "
        f"0 on failure indefinitely (#1476)")


@pytest.mark.parametrize("module_name,_class_name,_stub", WRAPPERS)
def test_command_error_is_importable_without_west(module_name, _class_name, _stub):
    """Each wrapper's no-west fallback must define CommandError, not just
    WestCommand -- otherwise `do_run`'s raise is a NameError on any host
    without west (CI, and the standalone `main()` path)."""
    mod = _load(module_name)
    assert issubclass(mod.CommandError, Exception)
    assert mod.CommandError(7).returncode == 7
