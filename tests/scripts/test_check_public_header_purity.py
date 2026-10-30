"""Unit tests for scripts/check_public_header_purity.py (C++ purity check)."""

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_public_header_purity.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_public_header_purity", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _write(root: Path, name: str, body: str) -> None:
    path = root / "include" / "alp" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")


def test_bare_c11_static_assert_fails_cxx_check(tmp_path):
    mod = _load()
    cxx = mod.find_cxx()
    if cxx is None:
        pytest.skip("no C++ compiler on PATH")
    _write(tmp_path, "bad.h", '_Static_assert(1 == 1, "c only");\n')
    offenders = mod.check_cxx(tmp_path, cxx)
    assert len(offenders) == 1 and "include/alp/bad.h" in offenders[0]


def test_shared_macro_passes_cxx_check(tmp_path):
    mod = _load()
    cxx = mod.find_cxx()
    if cxx is None:
        pytest.skip("no C++ compiler on PATH")
    shared = (REPO / "include" / "alp" / "static_assert.h").read_text(encoding="utf-8")
    _write(tmp_path, "static_assert.h", shared)
    _write(
        tmp_path,
        "good.h",
        '#include <alp/static_assert.h>\nALP_STATIC_ASSERT(1 == 1, "ok");\n',
    )
    assert mod.check_cxx(tmp_path, cxx) == []
