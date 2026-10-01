# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/gen_example_alp_conf.py -- the per-example
`generated/alp.conf` pre-generation twister / bare `west build` rely on (#866)."""
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_example_alp_conf.py"


def _load():
    spec = importlib.util.spec_from_file_location("_gea", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


GEN = _load()
CASES = GEN.parity.find_cases()


def _example(rel):
    return REPO / rel


@pytest.mark.parametrize("rel,core", [
    ("examples/multicore/rpmsg-aen/m55_hp", "m55_hp"),  # per-core subdir, board.yaml in parent
    ("examples/audio/audio-noise-suppression", None),   # plain example
])
def test_generated_conf_is_byte_identical_to_alp_project_emit(rel, core):
    d = _example(rel)
    assert GEN.main([str(d)]) == 0
    out = d / "generated" / "alp.conf"
    case = next(c for c in CASES if c[0] == d)
    want = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "alp_project.py"), "--input",
         str(case[1]), "--emit", "zephyr-conf", "--core", case[2]],
        capture_output=True, text=True, encoding="utf-8", cwd=REPO,
        env={**os.environ, "PYTHONIOENCODING": "utf-8"}, check=True).stdout
    assert out.read_bytes() == want.encode("utf-8")


def test_unbuildable_example_is_skipped_not_failed(capsys):
    assert GEN.main([str(_example("examples/multicore/rpmsg-imx93/m33"))]) == 0
    assert "SKIP" in capsys.readouterr().out


def _testcases():
    for app_dir, _board, _core in CASES:
        tc = app_dir / "testcase.yaml"
        if tc.is_file():
            yield tc


@pytest.mark.parametrize("tc", list(_testcases()), ids=lambda p: p.parent.relative_to(REPO).as_posix())
def test_every_test_loads_generated_alp_conf_first(tc):
    """Each test must pass EXACTLY ONE EXTRA_CONF_FILE (a second -D replaces
    the first) and it must lead with generated/alp.conf so later overlays win."""
    for name, t in yaml.safe_load(tc.read_text(encoding="utf-8"))["tests"].items():
        args = t.get("extra_args") or []
        args = args.split() if isinstance(args, str) else args
        key = f"{tc.parent.name}_EXTRA_CONF_FILE" if t.get("sysbuild") else "EXTRA_CONF_FILE"
        vals = [a.split("=", 1)[1].strip('"') for a in args if a.startswith(key + "=")]
        assert len(vals) == 1, f"{name}: {vals}"
        assert vals[0].split(";")[0] == "generated/alp.conf", f"{name}: {vals[0]}"


def test_no_testcase_references_alp_conf_in_an_uncovered_dir():
    """Reverse guard: a testcase.yaml naming generated/alp.conf in a dir the
    generator doesn't cover would fail configure with 'File not found'."""
    covered = {c[0] for c in CASES}
    for tc in (REPO / "examples").glob("**/testcase.yaml"):
        if "generated/alp.conf" in tc.read_text(encoding="utf-8"):
            assert tc.parent in covered, tc.relative_to(REPO).as_posix()


def test_unmatched_dir_fails(capsys):
    # aen-mcuboot-smoke has no board.yaml, so no per-core alp.conf to write.
    assert GEN.main([str(_example("examples/aen/aen-mcuboot-smoke"))]) == 1


def test_twin_sku_gets_its_own_som_facts(tmp_path):
    """#2597: AEN803's populated OSPI memories reach an AEN801-declared app."""
    d = tmp_path / "alp-console"
    d.mkdir()
    src = _example("examples/peripheral-io/alp-console")
    case = next(c for c in CASES if c[0] == src)
    GEN.generate(d, case[1], case[2])
    base = (d / "generated" / "alp.conf").read_text(encoding="utf-8")
    twin = (d / "generated" / "aen803" / "alp.conf").read_text(encoding="utf-8")
    assert "SOM_DRAM_MBIT" not in base
    assert "CONFIG_ALP_SDK_SOM_DRAM_MBIT=512" in twin
    assert "CONFIG_ALP_SDK_SOM_FLASH_MBIT=256" in twin
