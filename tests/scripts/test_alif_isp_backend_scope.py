from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _silicon_refs(path: str) -> set[str]:
    return set(re.findall(r'\.silicon_ref\s*=\s*"([^"]+)"', _text(path)))


def test_alif_isp_pico_backend_is_e8_only():
    assert _silicon_refs("src/backends/camera/alif_isp_pico.c") == {
        "alif:ensemble:e8"
    }


def test_alif_camera_header_supported_silicon_is_e8_only():
    header = _text("include/alp/ext/alif/camera.h")
    supported_lines = [
        line.strip()
        for line in header.splitlines()
        if line.strip().startswith("* @par Supported silicon:")
    ]

    assert len(supported_lines) == 4
    for line in supported_lines:
        assert "alif:ensemble:e8" in line
        assert "alif:ensemble:e4" not in line
        assert "alif:ensemble:e6" not in line


def test_accelerator_design_does_not_claim_e4_e6_isp_registration():
    design = _text("docs/aen-accelerator-backends-design.md")

    assert "This table is hardware metadata, not a current backend-support matrix" in design
    assert "the in-repo ISP-Pico backend registration is E8-only" in design
    assert "registered against `alif:ensemble:e8` only" in design
    assert "registered against `alif:ensemble:e4` / `e6` / `e8`" not in design
    assert "silicon_ref is E4 / E6 / E8" not in design


def test_camera_registry_unit_test_no_longer_fabricates_e4_isp_backend():
    test_src = _text("tests/unit/camera_registry/src/test_camera_registry.c")

    assert '.silicon_ref = "alif:ensemble:e8"' in test_src
    assert '.silicon_ref = "alif:ensemble:e4"' not in test_src
