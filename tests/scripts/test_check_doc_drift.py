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


def test_dotted_filename_indexed_passes(tmp_path):
    # A dotted stem (v1.0-readiness.md) linked in the index must be
    # recognised -- the link-extraction regex must include '.'.
    _scaffold(tmp_path, docs={"v1.0-readiness.md": "fine\n"})
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


def test_e1m_only_pinout_guidance_in_general_docs_fails(tmp_path):
    _scaffold(
        tmp_path,
        header_syms="ALP_E1M_I2C0 ALP_E1M_X_I2C0",
        docs={
            "getting-started.md": (
                "Standalone firmware should pick instance IDs from "
                "`<alp/e1m_pinout.h>` such as `ALP_E1M_I2C0`.\n"
            )
        },
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "E1M-only pinout guidance" in proc.stderr


def test_dual_namespace_pinout_guidance_passes(tmp_path):
    _scaffold(
        tmp_path,
        header_syms="ALP_E1M_I2C0 ALP_E1M_X_I2C0",
        docs={
            "getting-started.md": (
                "Standalone firmware should pick instance IDs from "
                "`<alp/e1m_pinout.h>` (`ALP_E1M_I2C0`) or "
                "`<alp/e1m_x_pinout.h>` (`ALP_E1M_X_I2C0`).\n"
            )
        },
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_forward_looking_tbd_doc_excluded(tmp_path):
    # v0.6-tbd-and-assumptions.md documents in-flight TBDs / proposed APIs
    # by intent; a "dead" symbol there must NOT fail the scan.  (It is
    # still index-linked.)
    _scaffold(tmp_path, docs={"v0.6-tbd-and-assumptions.md": "proposed `alp_sdio_open`\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_matrix_total_matches_passes(tmp_path):
    _scaffold(tmp_path, docs={
        "portability-matrix.md": (
            "## E1M family\n\n"
            "**18 / 21 cells generate cleanly (3 FAILING).**\n\n"
            "## E1M-X family\n\n"
            "**8 / 12 cells generate cleanly (4 FAILING).**\n"
        ),
        "v1.0-readiness.md": (
            "The matrix records 18/21 cells green for E1M and 8 of 12 "
            "for E1M-X.\n"
        ),
    })
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_matrix_total_mismatch_fails(tmp_path):
    _scaffold(tmp_path, docs={
        "portability-matrix.md": (
            "## E1M family\n\n"
            "**18 / 21 cells generate cleanly (3 FAILING).**\n\n"
            "## E1M-X family\n\n"
            "**8 / 12 cells generate cleanly (4 FAILING).**\n"
        ),
        "v1.0-readiness.md": (
            "The matrix records 12/12 cells green for E1M-X now.\n"
        ),
    })
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "12/12" in proc.stdout + proc.stderr
    assert "8/12" in proc.stdout + proc.stderr  # the expected value


def test_matrix_total_readme_mismatch_fails(tmp_path):
    # README.md hand-quotes both totals too (root-relative, not under
    # docs/) -- it must be covered by _MATRIX_TOTAL_QUOTING_DOCS same as
    # the docs/ trio.
    _scaffold(tmp_path, docs={
        "portability-matrix.md": (
            "## E1M family\n\n"
            "**18 / 21 cells generate cleanly (3 FAILING).**\n\n"
            "## E1M-X family\n\n"
            "**8 / 12 cells generate cleanly (4 FAILING).**\n"
        ),
    })
    (tmp_path / "README.md").write_text(
        "The matrix: 18 / 21 E1M cells, 12 / 12 E1M-X cells.\n",
        encoding="utf-8",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "12 / 12" in proc.stdout + proc.stderr
    assert "8/12" in proc.stdout + proc.stderr  # the expected value


def test_matrix_total_unrelated_fraction_not_flagged(tmp_path):
    # A denominator that isn't one of the matrix's own totals (e.g. a
    # tutorial-count fraction) must not be treated as a stale total.
    _scaffold(tmp_path, docs={
        "portability-matrix.md": (
            "## E1M family\n\n"
            "**18 / 21 cells generate cleanly (3 FAILING).**\n\n"
            "## E1M-X family\n\n"
            "**8 / 12 cells generate cleanly (4 FAILING).**\n"
        ),
        "v1.0-readiness.md": (
            "Tutorial count: 16 of 16 planned for v1.0 shipped.\n"
        ),
    })
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def _write_example_board_yaml(root: Path, rel: str, preset: str) -> None:
    p = root / "examples" / rel / "board.yaml"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"preset: {preset}\n", encoding="utf-8")


def test_example_preset_count_matches_passes(tmp_path):
    _scaffold(tmp_path, docs={
        "board-config-schema.md": (
            "Most example projects under `examples/` target the EVK or "
            "X-EVK (2 do today — 1 on `e1m-evk`, 1 on `e1m-x-evk`), "
            "so they share a single board definition each.\n"
        ),
    })
    _write_example_board_yaml(tmp_path, "a", "e1m-evk")
    _write_example_board_yaml(tmp_path, "b", "e1m-x-evk")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_example_preset_count_mismatch_fails(tmp_path):
    _scaffold(tmp_path, docs={
        "board-config-schema.md": (
            "(2 do today — 1 on `e1m-evk`, 1 on `e1m-x-evk`)\n"
        ),
    })
    _write_example_board_yaml(tmp_path, "a", "e1m-evk")
    _write_example_board_yaml(tmp_path, "b", "e1m-evk")
    _write_example_board_yaml(tmp_path, "c", "e1m-x-evk")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    out = proc.stdout + proc.stderr
    assert "1 on `e1m-evk`" in out  # the stale stated count
    assert "3 do today" in out  # the expected value


def test_yocto_machine_var_is_known(tmp_path):
    # A Yocto MACHINE variable defined in meta-alp-sdk is a real identifier.
    _scaffold(tmp_path, docs={"build-yocto-v2n.md": "Set `ALP_BOOT_DEVICE`.\n"})
    conf = tmp_path / "meta-alp-sdk" / "conf" / "machine"
    conf.mkdir(parents=True)
    (conf / "e1m.conf").write_text('ALP_BOOT_DEVICE = "mmc"\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


# --- #1228: CONFIG_-prefixed symbols must be resolved, not skipped ---
#
# Before the fix, `\b` never fires between "CONFIG_" and "ALP_" (both are
# \w), so `_SYMBOL_RE` never matched inside a `CONFIG_ALP_SDK_FOO=y` line
# at all -- the token was invisible to the scan, not merely "known": a
# dead `CONFIG_ALP_*` Kconfig line in a doc could drift silently forever.


def test_config_prefixed_dead_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={"kconfig-table.md": "Enable `CONFIG_ALP_DEAD_SYMBOL=y`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_DEAD_SYMBOL" in proc.stdout + proc.stderr


def test_config_prefixed_known_symbol_passes(tmp_path):
    # The CONFIG_ prefix must be stripped before the known-symbol check
    # (Kconfig source spells the same symbol bare) -- not just "any
    # CONFIG_-prefixed token is flagged". Paired with
    # test_config_prefixed_dead_symbol_fails above: on its own this test
    # passes whether or not CONFIG_-prefixed tokens are scanned at all, so
    # the dead-symbol test is what actually proves the scan runs.
    _scaffold(tmp_path, docs={"kconfig-table.md": "Enable `CONFIG_ALP_REAL_SYMBOL=y`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


# --- #1228: vendors/**/*.md is a scanned surface, vendors/**/CMakeLists.txt
# is a known-symbol source layer ---


def test_vendors_readme_dead_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={})
    readme = tmp_path / "vendors" / "somelib" / "README.md"
    readme.parent.mkdir(parents=True)
    readme.write_text("Override with `ALP_SOMELIB_DEAD_OVERRIDE`.\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_SOMELIB_DEAD_OVERRIDE" in proc.stdout + proc.stderr


def test_vendors_cmakelists_symbol_is_known(tmp_path):
    # A vendor integration anchor (e.g. option(ALP_HAS_SOMELIB ...)) can be
    # the ONLY place its own vendors/<lib>/README.md-documented symbol is
    # declared -- not under include/ or src/. Paired with
    # test_vendors_readme_dead_symbol_fails above: on its own this test
    # passes whether or not vendors/**/*.md is even scanned, so the
    # dead-symbol test is what actually proves the surface is scanned.
    _scaffold(tmp_path, docs={})
    vdir = tmp_path / "vendors" / "somelib"
    vdir.mkdir(parents=True)
    (vdir / "README.md").write_text("Enable with `ALP_HAS_SOMELIB`.\n", encoding="utf-8")
    (vdir / "CMakeLists.txt").write_text("option(ALP_HAS_SOMELIB \"\" ON)\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_gate_script_comments_do_not_self_confirm_symbols(tmp_path):
    # collect_known_symbols() harvests scripts/**/*.py as a "generators /
    # tooling" source layer.  Every scripts/check_*.py gate script is
    # excluded from it as a class (this file included): a gate script
    # narrates ALP_*/alp_* tokens by name in comments, docstrings, and
    # allowlist rationale without ever declaring or generating one.  Uses
    # a controlled fixture, not the live tree, so this exercises the
    # class-based exclusion mechanism directly, independent of what
    # check_doc_drift.py's own comments happen to say at the time (#1228).
    _scaffold(tmp_path, docs={"kconfig-table.md": "Uses `ALP_ONLY_MENTIONED_IN_COMMENT`.\n"})
    other_gate = tmp_path / "scripts" / "check_something_else.py"
    other_gate.parent.mkdir(parents=True, exist_ok=True)
    other_gate.write_text(
        "# Discusses ALP_ONLY_MENTIONED_IN_COMMENT in prose; never declares it.\n",
        encoding="utf-8",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_ONLY_MENTIONED_IN_COMMENT" in proc.stdout + proc.stderr


# --- #1228: filename-existence layers the check_*.py exclusion newly
# depends on (real docs referenced tokens only a laundering check_*.py
# comment used to confirm) ---


def test_board_scoped_conf_filename_is_known(tmp_path):
    # The full board+SoC+core target identity (not just the bare board
    # name collect_real_board_names() returns) is real by the EXISTENCE of
    # its boards/<target>.conf file, e.g.
    # docs/aen-bench-bringup.md's `boards/<target>.conf` reference.
    _scaffold(tmp_path, docs={
        "bench.md": "See `boards/alp_e1m_som_target_variant.conf`.\n",
    })
    conf = tmp_path / "examples" / "demo" / "boards" / "alp_e1m_som_target_variant.conf"
    conf.parent.mkdir(parents=True)
    conf.write_text("CONFIG_ALP_REAL_SYMBOL=y\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_board_scoped_conf_outside_examples_is_not_known(tmp_path):
    # The boards/*.conf / boards/*.overlay filename harvest is bounded to
    # examples/ (a directory-scoped harvest, not a whole-tree walk) -- a
    # boards/<target>.conf living outside examples/ must NOT make the
    # target identity it encodes "known".
    _scaffold(tmp_path, docs={
        "bench.md": "See `boards/alp_e1m_other_target_variant.conf`.\n",
    })
    conf = tmp_path / "tests" / "zephyr" / "boards" / "alp_e1m_other_target_variant.conf"
    conf.parent.mkdir(parents=True)
    conf.write_text("CONFIG_ALP_REAL_SYMBOL=y\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_e1m_other_target_variant" in proc.stdout + proc.stderr


def test_release_signing_key_filename_is_known(tmp_path):
    # keys/*.pem is real by filename existence, e.g.
    # docs/som-release-signing.md's `alp_release_signing_ecdsa_p256`.
    _scaffold(tmp_path, docs={
        "signing.md": "Verify with `alp_release_signing_ecdsa_p256`.\n",
    })
    keys = tmp_path / "keys"
    keys.mkdir()
    (keys / "alp_release_signing_ecdsa_p256.pub.pem").write_text("", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


# --- #1228: a stale #ifdef reference is not a Kconfig definition ---


def test_config_ifdef_reference_in_c_source_does_not_confirm_symbol(tmp_path):
    # A `#elif defined(CONFIG_ALP_SDK_FOO)` in a .c/.cpp source is a
    # REFERENCE to a Kconfig symbol, not a DEFINITION of the bare
    # ALP_SDK_FOO identifier.  If nothing defines the Kconfig symbol any
    # more (renamed/removed), a doc that names it is dead -- even though a
    # stale #ifdef of the old name still sits in the source.  Mutation on
    # the real tree: renaming `config ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65`
    # out of zephyr/kconfigs/iot-audio-inference.kconfig left
    # src/backends/inference/tflm.cpp's `#elif defined(CONFIG_...)`
    # unchanged, and the known-symbol harvest treated that reference as
    # proof the doc's mention was real -- the exact #1228 drift shape,
    # now passing green.
    _scaffold(tmp_path, docs={"kconfig-table.md": "Enable `ALP_SDK_FOO`.\n"})
    src = tmp_path / "src" / "backends"
    src.mkdir(parents=True)
    (src / "thing.cpp").write_text(
        "#elif defined(CONFIG_ALP_SDK_FOO)\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_SDK_FOO" in proc.stdout + proc.stderr
