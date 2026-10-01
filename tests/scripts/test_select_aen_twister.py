# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import select_aen_twister as s  # noqa: E402

B803 = "zephyr/boards/alp/e1m_aen803_m55_he/board.yml"
B801 = "zephyr/boards/alp/e1m_aen801_m55_hp/board.yml"


def test_no_paths_runs_both():
    assert s.affected_skus([]) == ["aen801", "aen803"]


def test_sku_exclusive_board_runs_only_that_sku():
    assert s.affected_skus([B803]) == ["aen803"]
    assert s.affected_skus([B801, "changelog.d/1.md"]) == ["aen801"]


def test_shared_path_runs_both():
    assert s.affected_skus([B803, "west.yml"]) == ["aen801", "aen803"]
    assert s.affected_skus([B801, B803]) == ["aen801", "aen803"]


def test_only_neutral_paths_run_nothing():
    assert s.affected_skus(["examples/aen/x/README.md", "docs/a.rst"]) == []
    assert s.matrix([]) == []


def test_matrix_shape():
    legs = s.matrix(["aen803"])
    assert [(x["sku"], x["subset"]) for x in legs] == [("aen803", 1), ("aen803", 2)]
    assert "alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp" in legs[0]["platform_flags"]


def test_md_and_tests_scripts_neutral():
    assert s.affected_skus([B803.replace("board.yml", "README.md")]) == []
    assert s.affected_skus(["tests/scripts/test_x.py"]) == []


def test_selector_change_runs_both():
    assert s.affected_skus(["scripts/select_aen_twister.py"]) == ["aen801", "aen803"]


def test_bad_base_fails_safe_to_all_legs(tmp_path, monkeypatch):
    out = tmp_path / "out"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    assert s.main(["--base", "no-such-rev", "--github-output"]) == 0
    line = out.read_text().strip()
    assert line.startswith("matrix=")
    import json

    assert len(json.loads(line[len("matrix="):])) == 4
