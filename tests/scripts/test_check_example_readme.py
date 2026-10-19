# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_example_readme.py (tmp_path trees, no subprocess)."""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_example_readme as gate  # noqa: E402

BOARD = """\
som:
  sku: E1M-AEN801
cores:
  a32_cluster:
    os: "off"
  m55_he:
    app: ./src
"""
GOOD = """\
# demo

```sh
tan build
```

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he app
```
"""


def make(tmp_path: Path, readme: str, board: str | None = BOARD,
         testcase: str | None = None) -> Path:
    d = tmp_path / "examples" / "aen" / "demo"
    d.mkdir(parents=True)
    (d / "README.md").write_text(readme, encoding="utf-8")
    if board is not None:
        (d / "board.yaml").write_text(board, encoding="utf-8")
    if testcase is not None:
        (d / "testcase.yaml").write_text(testcase, encoding="utf-8")
    return tmp_path


def test_clean_passes(tmp_path):
    assert gate.find_problems(make(tmp_path, GOOD)) == []


def test_r1_missing_tan_build(tmp_path):
    out = gate.find_problems(make(tmp_path, "# demo\n\n```sh\nwest build -t run\n```\n"))
    assert len(out) == 1 and "R1" in out[0]
    assert out[0].startswith("examples/aen/demo/README.md:1:")


def test_r1_tan_build_outside_fence_does_not_count(tmp_path):
    out = gate.find_problems(make(tmp_path, "tan build\n"))
    assert any("R1" in o for o in out)


def test_r2_readme_mentions_ensemble(tmp_path):
    out = gate.find_problems(make(tmp_path, GOOD + "\nboard ensemble_e8_dk\n"))
    assert len(out) == 1 and "R2" in out[0] and "README.md:11:" in out[0]


def test_r2_testcase_mentions_ensemble(tmp_path):
    out = gate.find_problems(make(tmp_path, GOOD, testcase="platform_allow: ensemble_e8_dk\n"))
    assert len(out) == 1 and "testcase.yaml:1: R2" in out[0]


def test_r3_wrong_sku(tmp_path):
    bad = GOOD.replace("aen801", "aen803")
    out = gate.find_problems(make(tmp_path, bad))
    assert len(out) == 1 and "R3" in out[0] and "alp_e1m_aen801_" in out[0]


def test_r3_core_not_enabled(tmp_path):
    bad = GOOD.replace("m55_he", "m55_hp")
    out = gate.find_problems(make(tmp_path, bad))
    assert len(out) == 1 and "R3" in out[0] and "m55_hp" in out[0]


def test_r3_core_off_is_not_enabled(tmp_path):
    board = BOARD + '  m55_hp:\n    os: "off"\n'
    out = gate.find_problems(make(tmp_path, GOOD.replace("m55_he", "m55_hp"), board))
    assert len(out) == 1 and "R3" in out[0]


def test_r3_target_on_continuation_line(tmp_path):
    readme = GOOD + "\n```sh\nwest build -p always \\\n  -b alp_e1m_aen803_m55_he app\n```\n"
    out = gate.find_problems(make(tmp_path, readme))
    assert len(out) == 1 and "R3" in out[0]


def test_native_sim_ignored(tmp_path):
    readme = GOOD + "\n```sh\nwest build -b native_sim/native/64 app\n```\n"
    assert gate.find_problems(make(tmp_path, readme)) == []


def test_no_board_yaml_exempt(tmp_path):
    assert gate.find_problems(make(tmp_path, "no tan here\n", board=None)) == []


def test_main_exit_codes(tmp_path, capsys):
    root = make(tmp_path, "nothing\n")
    assert gate.main(["--root", str(root)]) == 1
    assert "R1" in capsys.readouterr().out


TWIN = GOOD + "\n```sh\nwest build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he app\n```\n"
TWIN_TC = """\
tests:
  demo.aen:
    platform_allow:
      - alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he
      - alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he
"""


def test_r3_twin_sku_built_by_testcase_is_accepted(tmp_path):
    assert gate.find_problems(make(tmp_path, TWIN, testcase=TWIN_TC)) == []


def test_r3_twin_sku_not_in_testcase_still_fails(tmp_path):
    out = gate.find_problems(make(tmp_path, TWIN))
    assert len(out) == 1 and "R3" in out[0] and "README.md:12:" in out[0]


def test_r3_core_built_by_testcase_is_accepted(tmp_path):
    readme = GOOD.replace("m55_he/ae822fa0e5597ls0/rtss_he", "m55_hp/ae822fa0e5597ls0/rtss_hp")
    tc = "tests:\n  demo.hp:\n    platform_allow: alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp\n"
    assert gate.find_problems(make(tmp_path, readme, testcase=tc)) == []


def test_r3_prose_and_inline_west_build_ignored(tmp_path):
    readme = GOOD + "\nDo not run `west build -b alp_e1m_aen803_m55_he/...` by hand.\n"
    assert gate.find_problems(make(tmp_path, readme)) == []


def test_r3_indented_fence_is_checked(tmp_path):
    readme = GOOD + "\n1. Build:\n\n   ```bash\n   west build -b alp_e1m_aen803_m55_he app\n   ```\n"
    out = gate.find_problems(make(tmp_path, readme))
    assert len(out) == 1 and "R3" in out[0]


def test_r3_each_west_line_in_one_block_is_checked(tmp_path):
    readme = GOOD + ("\n```sh\nwest build -b alp_e1m_aen803_m55_he app\n"
                     "west build -b alp_e1m_aen801_m55_hp app\n```\n")
    out = gate.find_problems(make(tmp_path, readme))
    assert len(out) == 2 and all("R3" in o for o in out)


def test_r3_other_sku_marker_skips_only_its_block(tmp_path):
    readme = GOOD + ("\nFor V2N silicon, change som.sku first:\n\n"
                     "<!-- example-readme:other-sku -->\n"
                     "```sh\nwest build -b alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33 app\n```\n"
                     "\n```sh\nwest build -b alp_e1m_aen803_m55_he app\n```\n")
    out = gate.find_problems(make(tmp_path, readme))
    assert len(out) == 1 and "aen803" in out[0]


def test_r3_trailing_backtick_stripped_before_testcase_match(tmp_path):
    readme = GOOD + "\n```sh\nwest build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he`\n```\n"
    assert gate.find_problems(make(tmp_path, readme, testcase=TWIN_TC)) == []
