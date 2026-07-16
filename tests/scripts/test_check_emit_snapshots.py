"""Unit tests for scripts/check_emit_snapshots.py path normalisation."""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_emit_snapshots.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_emit_snapshots", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_normalize_posix_root():
    mod = _load()
    repo = "/home/ci/work/alp-sdk"
    text = f'{{"ALP_SDK_ROOT": "{repo}/build"}}'
    out = mod._normalize_host_paths(text, repo, "/usr/bin/python3")
    assert repo not in out
    assert "<SDK_ROOT>/build" in out


def test_normalize_windows_json_escaped_root():
    """A JSON-escaped Windows checkout root (doubled backslashes) must
    normalise -- this is the regression from issue #472."""
    mod = _load()
    repo = "C:\\Users\\dev\\alp-sdk"
    # As it appears inside JSON: backslashes doubled.
    text = '{"ALP_SDK_ROOT": "C:\\\\Users\\\\dev\\\\alp-sdk"}'
    out = mod._normalize_host_paths(text, repo, "C:\\Python312\\python.exe")
    assert "C:\\\\Users" not in out
    assert "<SDK_ROOT>" in out


def test_normalize_windows_forward_slash_root():
    mod = _load()
    repo = "C:\\Users\\dev\\alp-sdk"
    text = '{"path": "C:/Users/dev/alp-sdk/gen"}'
    out = mod._normalize_host_paths(text, repo, "C:\\Python312\\python.exe")
    assert "C:/Users/dev/alp-sdk" not in out
    assert "<SDK_ROOT>/gen" in out


def test_normalize_idempotent_when_root_absent():
    mod = _load()
    out = mod._normalize_host_paths(
        '{"k": "v"}', "/home/ci/alp-sdk", "/usr/bin/python3")
    assert out == '{"k": "v"}'


def test_normalize_posix_python_executable():
    mod = _load()
    python = "/opt/workspace/.venv/bin/python"
    text = f'{{"arg": "-DPython3_EXECUTABLE={python}"}}'
    out = mod._normalize_host_paths(text, "/home/ci/alp-sdk", python)
    assert python not in out
    assert "-DPython3_EXECUTABLE=<PYTHON_EXECUTABLE>" in out


def test_normalize_windows_json_escaped_python_executable():
    mod = _load()
    python = "C:\\Users\\dev\\workspace\\.venv\\Scripts\\python.exe"
    text = ('{"arg": "-DPython3_EXECUTABLE='
            'C:\\\\Users\\\\dev\\\\workspace\\\\.venv\\\\Scripts\\\\python.exe"}')
    out = mod._normalize_host_paths(text, "C:\\repo\\alp-sdk", python)
    assert "C:\\\\Users" not in out
    assert "-DPython3_EXECUTABLE=<PYTHON_EXECUTABLE>" in out
