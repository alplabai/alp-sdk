# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/gen_example_alp_conf.py -- the per-example
`generated/alp.conf` pre-generation twister / bare `west build` rely on (#866)."""
import importlib.util
import os
import re
import shutil
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
    (d / "CMakeLists.txt").write_text(GEN._HOOK, encoding="utf-8")
    src = _example("examples/peripheral-io/alp-console")
    case = next(c for c in CASES if c[0] == src)
    GEN.generate(d, case[1], case[2])
    base = (d / "generated" / "alp.conf").read_text(encoding="utf-8")
    twin = (d / "generated" / "aen803" / "alp.conf").read_text(encoding="utf-8")
    assert "SOM_DRAM_MBIT" not in base
    assert "CONFIG_ALP_SDK_SOM_DRAM_MBIT=512" in twin
    assert "CONFIG_ALP_SDK_SOM_FLASH_MBIT=256" in twin


def test_load_board_yaml_sku_override():
    from alp_orchestrate import load_board_yaml
    case = next(c for c in CASES if c[0] == _example("examples/peripheral-io/alp-console"))
    assert load_board_yaml(case[1]).sku != "E1M-AEN803"
    assert load_board_yaml(case[1], sku="E1M-AEN803").sku == "E1M-AEN803"


def test_identical_twin_fragment_writes_no_subdir(tmp_path, monkeypatch):
    case = next(c for c in CASES if c[0] == _example("examples/peripheral-io/alp-console"))
    d = tmp_path / "app"
    d.mkdir()
    (d / "CMakeLists.txt").write_text(GEN._HOOK, encoding="utf-8")
    monkeypatch.setattr(GEN, "_slice_alp_conf", lambda *a: "same\n")
    GEN.generate(d, case[1], case[2])
    assert [p.name for p in (d / "generated").iterdir()] == ["alp.conf"]


def test_twins_only_for_examples_with_the_hook():
    hooked = 0
    for app_dir, board_yaml, core in CASES:
        GEN.generate(app_dir, board_yaml, core)
        twins = list((app_dir / "generated").glob("*/alp.conf"))
        if GEN._has_hook(app_dir):
            hooked += 1
        else:
            assert not twins, app_dir
    assert hooked >= 5
    assert (_example("examples/peripheral-io/alp-console")
            / "generated/aen803/alp.conf").is_file()


# --- the CMakeLists.txt selector, run by real CMake ------------------------------------

def _hooked_examples():
    return sorted(p.parent for p in (REPO / "examples").rglob("CMakeLists.txt") if GEN._has_hook(p.parent))


def _selected_conf(cmakelists: Path, tmp_path: Path, board: str, twin_dirs: tuple[str, ...]) -> str:
    """Run the example's own selector (everything above find_package) in CMake
    script mode against a fake generated/ tree; return the EXTRA_CONF_FILE it picks."""
    head = cmakelists.read_text(encoding="utf-8").split("find_package(Zephyr", 1)[0]
    (tmp_path / "generated").mkdir(parents=True, exist_ok=True)
    (tmp_path / "generated" / "alp.conf").write_text("", encoding="utf-8")
    for sku in twin_dirs:
        (tmp_path / "generated" / sku).mkdir(exist_ok=True)
        (tmp_path / "generated" / sku / "alp.conf").write_text("", encoding="utf-8")
    script = tmp_path / "selector.cmake"
    script.write_text('set(EXTRA_CONF_FILE "generated/alp.conf")\n' + head
                      + 'message("SELECTED=${EXTRA_CONF_FILE}")\n', encoding="utf-8")
    proc = subprocess.run(["cmake", f"-DBOARD={board}", "-P", str(script)], capture_output=True,
                          text=True, encoding="utf-8", check=False)
    assert proc.returncode == 0, proc.stderr
    return re.search(r"SELECTED=(.*)", proc.stderr + proc.stdout)[1].strip()


@pytest.mark.skipif(shutil.which("cmake") is None, reason="needs cmake")
@pytest.mark.parametrize("app_dir", _hooked_examples(), ids=lambda p: p.name)
def test_selector_keeps_the_base_fragment_when_the_board_has_no_twin_dir(app_dir, tmp_path):
    # Only generated/aen803/ exists (the twin of an AEN801 board.yaml). A build
    # for the AEN801 board must stay on generated/alp.conf: pointing it at a
    # generated/aen801/alp.conf nobody wrote is a CMake "File not found".
    cm = app_dir / "CMakeLists.txt"
    got = _selected_conf(cm, tmp_path, "alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp", ("aen803",))
    assert got == "generated/alp.conf"


@pytest.mark.skipif(shutil.which("cmake") is None, reason="needs cmake")
@pytest.mark.parametrize("app_dir", _hooked_examples(), ids=lambda p: p.name)
def test_selector_picks_the_twin_fragment_when_it_exists(app_dir, tmp_path):
    cm = app_dir / "CMakeLists.txt"
    got = _selected_conf(cm, tmp_path, "alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp", ("aen803",))
    assert got == "generated/aen803/alp.conf"
    assert _selected_conf(cm, tmp_path, "native_sim/native/64", ("aen803",)) == "generated/alp.conf"


def test_stale_twin_fragment_is_removed(tmp_path, monkeypatch):
    src = _example("examples/peripheral-io/alp-console")
    d = tmp_path / "alp-console"
    shutil.copytree(src, d, ignore=shutil.ignore_patterns("generated", "build*"))
    board = next(c[1] for c in CASES if c[0] == src)
    twin = d / "generated" / "aen803" / "alp.conf"
    twin.parent.mkdir(parents=True)
    twin.write_text("CONFIG_STALE=y\n", encoding="utf-8")
    # Force the twin to converge with the base: no twin is written, so only the
    # pre-regeneration unlink can remove the planted file.
    monkeypatch.setattr(GEN, "_slice_alp_conf", lambda *_a, **_k: "CONFIG_SAME=y\n")
    GEN.generate(d, board, next(c[2] for c in CASES if c[0] == src))
    assert not twin.exists()
