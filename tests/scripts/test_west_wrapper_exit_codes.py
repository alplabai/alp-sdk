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

import pytest

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


WRAPPERS = [("alp_quality", "AlpQuality"),
            ("alp_lock", "AlpLock"),
            ("alp_migrate", "AlpMigrate")]


@pytest.mark.parametrize("module_name,class_name", WRAPPERS)
@pytest.mark.parametrize("rc", [1, 2, 99])
def test_do_run_raises_command_error_on_failure(module_name, class_name, rc,
                                                monkeypatch):
    """A non-zero rc must surface as CommandError carrying that same code."""
    mod = _load(module_name)
    monkeypatch.setattr(mod, "run", lambda _args: rc)

    with pytest.raises(mod.CommandError) as excinfo:
        getattr(mod, class_name)().do_run(object(), [])

    assert excinfo.value.returncode == rc, (
        f"{module_name}.do_run raised CommandError but with returncode "
        f"{excinfo.value.returncode!r}, not the {rc!r} run() reported -- west "
        f"exits with whatever the exception carries, so a wrong code here is "
        f"as bad as no exception")


@pytest.mark.parametrize("module_name,class_name", WRAPPERS)
def test_do_run_stays_quiet_on_success(module_name, class_name, monkeypatch):
    """rc 0 must NOT raise -- a fix that made these always fail would be worse
    than the bug it replaced."""
    mod = _load(module_name)
    monkeypatch.setattr(mod, "run", lambda _args: 0)

    assert getattr(mod, class_name)().do_run(object(), []) == 0


@pytest.mark.parametrize("module_name,_class_name", WRAPPERS)
def test_command_error_is_importable_without_west(module_name, _class_name):
    """Each wrapper's no-west fallback must define CommandError, not just
    WestCommand -- otherwise `do_run`'s raise is a NameError on any host
    without west (CI, and the standalone `main()` path)."""
    mod = _load(module_name)
    assert issubclass(mod.CommandError, Exception)
    assert mod.CommandError(7).returncode == 7
