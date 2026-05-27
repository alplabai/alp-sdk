"""Unit tests for scripts/check_doc_drift.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_doc_drift.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _scaffold(root: Path, *, header_syms="ALP_REAL_SYMBOL alp_real_open",
              docs=None, index_links=None):
    """Build a minimal repo-shaped tree: include/alp/api.h + docs/*.md +
    docs/README.md.  `docs` maps relpath-under-docs -> file body.
    `index_links` is the list of filenames the index links (defaults to
    every top-level *.md in `docs`)."""
    inc = root / "include" / "alp"
    inc.mkdir(parents=True)
    (inc / "api.h").write_text(
        "\n".join(f"#define {s}" if s.isupper() else f"int {s}(void);"
                  for s in header_syms.split()) + "\n",
        encoding="utf-8",
    )
    docs = docs or {}
    ddir = root / "docs"
    ddir.mkdir(parents=True, exist_ok=True)
    for rel, body in docs.items():
        p = ddir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body, encoding="utf-8")
    top_level = [rel for rel in docs if "/" not in rel and rel != "README.md"]
    if index_links is None:
        index_links = top_level
    (ddir / "README.md").write_text(
        "# Index\n" + "".join(f"- [{n}]({n})\n" for n in index_links),
        encoding="utf-8",
    )


def test_known_symbol_passes(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Use `ALP_REAL_SYMBOL` and `alp_real_open`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_dead_uppercase_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Call `ALP_DEAD_SYMBOL` now.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_DEAD_SYMBOL" in proc.stdout + proc.stderr


def test_dead_lowercase_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Call `alp_dead_open()`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_dead_open" in proc.stdout + proc.stderr


def test_allowlisted_symbol_passes(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Build emits `ALP_SOC_MADEUP_KIB`.\n"})
    proc = _run("--root", str(tmp_path), "--allow", "ALP_SOC_MADEUP_KIB")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_wildcard_family_prefix_passes(tmp_path):
    # `ALP_E1M_*` family reference: token `ALP_E1M_` is a prefix of a real
    # symbol, so it must NOT be flagged.
    _scaffold(tmp_path, header_syms="ALP_E1M_PWM0",
              docs={"e1m-pinout.md": "The `ALP_E1M_*` pads.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_excluded_surface_not_scanned(tmp_path):
    # docs/abi/** and docs/adr/** and docs/superpowers/** are excluded; a dead symbol there is OK.
    _scaffold(tmp_path, docs={
        "abi/snap.md": "frozen `ALP_DEAD_SYMBOL`\n",
        "adr/0001.md": "decided `alp_dead_open`\n",
        "superpowers/plan.md": "wip `ALP_DEAD_SYMBOL`\n",
    })
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_changelog_not_scanned(tmp_path):
    _scaffold(tmp_path, docs={})
    (tmp_path / "CHANGELOG.md").write_text("Renamed `ALP_DEAD_SYMBOL`.\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_readme_is_scanned(tmp_path):
    _scaffold(tmp_path, docs={})
    (tmp_path / "README.md").write_text("See `ALP_DEAD_SYMBOL`.\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_DEAD_SYMBOL" in proc.stdout + proc.stderr


def test_index_gap_fails(tmp_path):
    _scaffold(tmp_path, docs={"orphan.md": "no symbols here\n"}, index_links=[])
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "orphan.md" in proc.stdout + proc.stderr


def test_index_complete_passes(tmp_path):
    _scaffold(tmp_path, docs={"linked.md": "fine\n"})  # default: index links it
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_subdir_doc_not_required_in_index(tmp_path):
    # Only top-level docs/*.md must be indexed; subdir docs are not.
    _scaffold(tmp_path, docs={"soms/v2n.md": "fine\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_kconfig_symbol_is_known(tmp_path):
    # A symbol defined only in Kconfig (not a header) is real, not drift.
    _scaffold(tmp_path, docs={"glossary.md": "Enable `ALP_SDK_FANCY_BACKEND`.\n"})
    (tmp_path / "zephyr").mkdir()
    (tmp_path / "zephyr" / "Kconfig").write_text(
        "config ALP_SDK_FANCY_BACKEND\n\tbool\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_generator_emitted_symbol_is_known(tmp_path):
    # A board-name / CMake-helper emitted by scripts/alp_project.py is real.
    _scaffold(tmp_path, docs={"board-config.md": "CMake calls `alp_hw_info_build`.\n"})
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "alp_project.py").write_text(
        "BOARD_FN = 'alp_hw_info_build'\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_forward_looking_plan_doc_excluded(tmp_path):
    # cc3501e-integration-plan.md documents a proposed API by intent;
    # a "dead" symbol there must NOT fail the scan.  (It is still index-linked.)
    _scaffold(tmp_path, docs={"cc3501e-integration-plan.md": "proposed `alp_sdio_open`\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_yocto_machine_var_is_known(tmp_path):
    # A Yocto MACHINE variable defined in meta-alp-sdk is a real identifier.
    _scaffold(tmp_path, docs={"build-yocto-v2n.md": "Set `ALP_BOOT_DEVICE`.\n"})
    conf = tmp_path / "meta-alp-sdk" / "conf" / "machine"
    conf.mkdir(parents=True)
    (conf / "e1m.conf").write_text('ALP_BOOT_DEVICE = "mmc"\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
