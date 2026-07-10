from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


SE_BACKEND_SOURCES = {
    "soc_info": ROOT / "src" / "backends" / "soc_info" / "alif_se.c",
    "power_profile": ROOT / "src" / "backends" / "power" / "alif_se_profile.c",
    "mproc_boot": ROOT / "src" / "backends" / "mproc" / "alif_se_boot.c",
    "security": ROOT / "src" / "backends" / "security" / "se_cryptocell.c",
}


def _silicon_refs(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r'\.silicon_ref\s*=\s*"([^"]+)"', text))


def test_alif_se_portable_backends_are_e8_only():
    """SE-backed portable surfaces are characterized for E8/AEN801 only."""
    for cls, path in SE_BACKEND_SOURCES.items():
        assert _silicon_refs(path) == {"alif:ensemble:e8"}, cls


def test_os_support_matrix_scopes_se_rows_to_aen801_e8():
    matrix = (ROOT / "docs" / "os-support-matrix.md").read_text(encoding="utf-8")

    for prefix in (
        "| SoC identity |",
        "| Power profiles (operating points) |",
        "| Peer-core boot |",
    ):
        line = next(line for line in matrix.splitlines() if line.startswith(prefix))
        assert "AEN801" in line
        assert "alif:ensemble:e8" in line
        assert "SE backend on AEN)" not in line
        assert "SE-service backend on AEN)" not in line
        assert "boot authority on AEN)" not in line


def _effective_kconfig_text() -> str:
    """Concatenate the top-level Kconfig with every sourced subsystem
    fragment (issue #458 split the former zephyr/Kconfig monolith into
    zephyr/kconfig/*.kconfig).  Mirrors what the real Kconfig parser sees
    after `rsource` inlines each fragment in place."""
    parts = [(ROOT / "zephyr" / "Kconfig").read_text(encoding="utf-8")]
    fragment_dir = ROOT / "zephyr" / "kconfig"
    for fragment in sorted(fragment_dir.glob("*.kconfig")):
        parts.append(fragment.read_text(encoding="utf-8"))
    return "\n".join(parts)


def test_kconfig_help_does_not_claim_aen_wide_se_fallback():
    kconfig = _effective_kconfig_text()

    assert "returns ALP_ERR_NOSUPPORT on AEN builds" not in kconfig
    assert "return ALP_ERR_NOSUPPORT on AEN builds" not in kconfig
    assert "calls return ALP_ERR_NOSUPPORT on AEN builds" not in kconfig
    assert (
        len(re.findall(r"non-E8 AEN SoMs without a matching\s+silicon_ref", kconfig))
        >= 3
    )


def test_aen_se_query_example_pins_e8_soc_profile():
    prj_conf = (
        ROOT / "examples" / "aen" / "aen-se-service-query" / "prj.conf"
    ).read_text(encoding="utf-8")

    assert "CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y" in prj_conf
