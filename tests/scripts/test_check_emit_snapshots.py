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


def test_normalize_docs_ref_released_tag():
    mod = _load()
    text = ("[`docs/cross-platform-setup.md`](https://github.com/alplabai/"
            "alp-sdk/blob/v0.16.0/docs/cross-platform-setup.md)")
    out = mod._normalize_host_paths(text, "/home/ci/alp-sdk", "/usr/bin/python3")
    assert "blob/v0.16.0/" not in out
    assert "blob/<DOCS_REF>/docs/cross-platform-setup.md" in out


def test_normalize_docs_ref_main_and_tag_agree():
    """A tags-fetched checkout emits `v<version>`, CI's tagless clone emits
    `main`, from the same commit (#1686/#1738).  Both must normalise to the
    same bytes or the golden can only ever be green on one of them."""
    mod = _load()
    base = "https://github.com/alplabai/alp-sdk/blob/{}/docs/a.md"
    args = ("/home/ci/alp-sdk", "/usr/bin/python3")
    assert (mod._normalize_host_paths(base.format("main"), *args)
            == mod._normalize_host_paths(base.format("v0.16.0"), *args))
    # and it must keep holding at the NEXT release, not just this one
    assert (mod._normalize_host_paths(base.format("v0.17.0"), *args)
            == mod._normalize_host_paths(base.format("main"), *args))


def test_normalize_docs_ref_keeps_the_doc_path_real():
    """Only the ref segment is tokenised -- a doc that moves still fails."""
    mod = _load()
    args = ("/home/ci/alp-sdk", "/usr/bin/python3")
    moved = mod._normalize_host_paths(
        "https://github.com/alplabai/alp-sdk/blob/main/docs/moved.md", *args)
    orig = mod._normalize_host_paths(
        "https://github.com/alplabai/alp-sdk/blob/main/docs/a.md", *args)
    assert moved != orig


def test_normalize_docs_ref_leaves_other_repos_alone():
    mod = _load()
    text = "https://github.com/other/repo/blob/main/a.md"
    assert mod._normalize_host_paths(
        text, "/home/ci/alp-sdk", "/usr/bin/python3") == text


def test_normalize_docs_ref_covers_tree_urls():
    """Scaffolded READMEs link sibling EXAMPLES as `tree/<ref>/...`, not just
    docs as `blob/<ref>/...` -- sensor-v2n101 carries three of them, and a
    blob-only pattern left that golden red in a tagless clone (#1738)."""
    mod = _load()
    args = ("/home/ci/alp-sdk", "/usr/bin/python3")
    base = ("https://github.com/alplabai/alp-sdk/tree/{}"
            "/examples/peripheral-io/i2c-scanner")
    assert (mod._normalize_host_paths(base.format("main"), *args)
            == mod._normalize_host_paths(base.format("v0.16.0"), *args))
    assert "tree/<DOCS_REF>/examples/peripheral-io/i2c-scanner" in \
        mod._normalize_host_paths(base.format("v0.16.0"), *args)
