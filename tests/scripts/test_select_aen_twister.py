# SPDX-License-Identifier: Apache-2.0
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
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
    assert [(x["sku"], x["subset"]) for x in legs] == [("aen803", i) for i in range(1, s.SUBSETS + 1)]
    assert {x["subsets"] for x in legs} == {s.SUBSETS}
    assert "alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp" in legs[0]["platform_flags"]
    assert legs[0]["roots"] == s.root_args(s.ROOTS)


def test_md_and_tests_scripts_neutral():
    assert s.affected_skus([B803.replace("board.yml", "README.md")]) == []
    assert s.affected_skus(["tests/scripts/test_x.py"]) == []


def test_selector_change_runs_both():
    assert s.affected_skus(["scripts/select_aen_twister.py"]) == ["aen801", "aen803"]


def test_bad_base_fails_safe_to_all_legs(tmp_path, monkeypatch):
    out = tmp_path / "out"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    assert s.main(["--base", "no-such-rev", "--github-output"]) == 0
    line = out.read_text(encoding="utf-8").strip()
    assert line.startswith("matrix=")
    import json

    assert len(json.loads(line[len("matrix="):])) == 2 * s.SUBSETS


def test_workflow_shuffles_subsets_with_a_fixed_seed():
    """#2885: twister slices its sorted configuration list, which clustered
    the heavy TFLM builds into one shard. The workflow must shuffle with a
    FIXED seed: an unseeded shuffle reshuffles every run, so no shard's
    per-subset ccache would match its previous contents."""
    wf = (REPO / ".github" / "workflows" / "pr-twister-aen.yml").read_text(encoding="utf-8")
    twister = wf[wf.index("python3 zephyr/scripts/twister"):]
    twister = twister[: twister.index("--inline-logs")]
    assert "--subset ${{ matrix.subset }}/${{ matrix.subsets }}" in twister
    m = re.search(r"--shuffle-tests --shuffle-tests-seed (\S+)", twister)
    assert m, "pr-twister-aen.yml twister call lost --shuffle-tests --shuffle-tests-seed"
    assert m.group(1).isdigit()


EX = "examples/ai/motor-current-signature"


def test_examples_only_change_narrows_to_its_own_root_and_sku():
    paths = [f"{EX}/src/main.c", f"{EX}/README.md", "changelog.d/x.md"]
    roots = s.narrowed_roots(paths, REPO)
    assert roots == [EX]
    counts = s.config_counts(roots, REPO)
    assert counts == {"aen801": 1, "aen803": 0}  # testcase.yaml names only AEN801
    legs = s.matrix(s.affected_skus(paths), roots, counts)
    assert [(x["sku"], x["subset"], x["subsets"]) for x in legs] == [("aen801", 1, 1)]
    assert legs[0]["roots"] == f"--testsuite-root alp-sdk/{EX}"


AI = [r for r in s.ROOTS if r.startswith("examples/ai/")]


def test_narrowed_heavy_examples_still_shard():
    """#2885 review: the six TFLM examples in one unsplit leg would be the
    41 min shard again; a narrowed SKU keeps min(SUBSETS, configurations)
    shards (one configuration each today)."""
    assert len(AI) == 6
    roots = s.narrowed_roots([f"{d}/src/main.c" for d in AI], REPO)
    counts = s.config_counts(roots, REPO)
    assert counts["aen801"] == 6
    legs = [x for x in s.matrix(list(s.SKUS), roots, counts) if x["sku"] == "aen801"]
    assert [x["subset"] for x in legs] == list(range(1, min(s.SUBSETS, 6) + 1))


def test_shards_follow_each_skus_configuration_count():
    # aen-trace-runner (with nested hp_vision) carries more AEN803 than
    # AEN801 configurations; one directory must not mean one leg.
    roots = ["examples/aen/aen-trace-runner"]
    counts = s.config_counts(roots, REPO)
    legs = s.matrix(list(s.SKUS), roots, counts)
    for sku in s.SKUS:
        n = len([x for x in legs if x["sku"] == sku])
        assert n == min(s.SUBSETS, counts[sku])
    assert counts["aen803"] > 1


def test_sku_with_no_configuration_gets_no_leg():
    roots = ["examples/multicore/mproc-mailbox"]
    counts = s.config_counts(roots, REPO)
    assert counts["aen803"] == 0
    assert {x["sku"] for x in s.matrix(list(s.SKUS), roots, counts)} == {"aen801"}


def _repo_with_testcase(tmp_path, text):
    d = tmp_path / "examples" / "aen" / "demo"
    d.mkdir(parents=True)
    (d / "testcase.yaml").write_text(text, encoding="utf-8")
    return tmp_path, ["examples/aen/demo"]


def test_test_without_platform_allow_cannot_be_narrowed(tmp_path):
    """twister builds such a test for every -p platform, so dropping a SKU
    could skip it: config_counts refuses and main() runs the full split."""
    repo, roots = _repo_with_testcase(tmp_path, "tests:\n  demo.any:\n    build_only: true\n")
    assert s.config_counts(roots, repo) is None


def test_common_platform_allow_and_comments(tmp_path):
    repo, roots = _repo_with_testcase(tmp_path, (
        "# mentions alp_e1m_aen803_m55_he only in a comment\n"
        "common:\n  platform_allow: alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp native_sim\n"
        "tests:\n  demo.a: {}\n  demo.b:\n    platform_allow: [native_sim]\n"))
    assert s.config_counts(roots, repo) == {"aen801": 1, "aen803": 0}


def test_path_with_its_own_testcase_outside_examples_runs_full_split():
    # firmware/alp-stock-shim has a testcase.yaml but is not an example root.
    assert s.narrowed_roots(["firmware/alp-stock-shim/src/main.c", f"{EX}/x.c"], REPO) is None


def test_exactly_narrow_max_examples_still_narrows():
    many = [d for d in s.ROOTS if d.count("/") == 2][: s.NARROW_MAX]
    assert len(s.narrowed_roots([f"{d}/x.c" for d in many], REPO)) == s.NARROW_MAX


def test_unsafe_directory_name_runs_full_split(tmp_path):
    bad = tmp_path / "examples" / "aen" / "bad name"
    bad.mkdir(parents=True)
    (bad / "testcase.yaml").write_text("tests: {}\n", encoding="utf-8")
    assert s.narrowed_roots(["examples/aen/bad name/src/main.c"], tmp_path) is None


def _run_main(monkeypatch, tmp_path, changed):
    out = tmp_path / "out"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))

    class Done:
        stdout = "\n".join(changed) + "\n"

    monkeypatch.setattr(s.subprocess, "run", lambda *a, **k: Done())
    assert s.main(["--base", "base", "--github-output"]) == 0
    import json

    return json.loads(out.read_text(encoding="utf-8").strip()[len("matrix="):])


def test_main_narrows_an_examples_only_diff(monkeypatch, tmp_path):
    legs = _run_main(monkeypatch, tmp_path, [f"{EX}/src/main.c"])
    assert [(x["sku"], x["subsets"], x["roots"]) for x in legs] == [
        ("aen801", 1, f"--testsuite-root alp-sdk/{EX}")]


def test_main_runs_full_split_on_shared_code(monkeypatch, tmp_path):
    legs = _run_main(monkeypatch, tmp_path, [f"{EX}/src/main.c", "src/alp.c"])
    assert len(legs) == 2 * s.SUBSETS and legs[0]["roots"] == s.root_args(s.ROOTS)


def test_nested_testcase_dir_is_its_own_root():
    assert s.narrowed_roots(["examples/multicore/mproc-mailbox/peer/src/main.c"], REPO) == [
        "examples/multicore/mproc-mailbox/peer"]


def test_shared_code_runs_full_split():
    assert s.narrowed_roots([f"{EX}/src/main.c", "src/backends/mproc/x.c"], REPO) is None


def test_example_file_without_testcase_runs_full_split():
    assert s.narrowed_roots(["examples/aen/common/shared.h"], REPO) is None


def test_example_outside_roots_builds_nothing_here():
    assert s.narrowed_roots(["examples/peripheral-io/hello-world/src/main.c"], REPO) == []
    assert s.narrowed_roots(["examples/peripheral-io/hello-world/src/main.c", f"{EX}/x.c"], REPO) == [EX]


def test_too_many_examples_runs_full_split():
    many = [d for d in s.ROOTS if d.count("/") == 2][: s.NARROW_MAX + 1]
    assert len(many) == s.NARROW_MAX + 1
    assert s.narrowed_roots([f"{d}/src/main.c" for d in many], REPO) is None


def test_every_root_exists_and_is_in_both_paths_filters():
    wf = (REPO / ".github" / "workflows" / "pr-twister-aen.yml").read_text(encoding="utf-8")
    for r in s.ROOTS:
        assert (REPO / r).is_dir(), r
        assert wf.count(f"- '{r}/**'") == 2, f"{r}/** missing from a paths: filter"
    assert "${{ matrix.roots }}" in wf
    assert '--testsuite-root "$GITHUB_WORKSPACE' not in wf, "roots belong in ROOTS, not the workflow"


def test_nested_testcase_configurations_count_toward_the_parent_root():
    """twister walks a root recursively, so mproc-mailbox also builds peer/."""
    parent = s.config_counts(["examples/multicore/mproc-mailbox"], REPO)
    peer = s.config_counts(["examples/multicore/mproc-mailbox/peer"], REPO)
    assert peer["aen801"] >= 1
    assert parent["aen801"] > peer["aen801"]


def test_main_runs_full_split_when_configurations_cannot_be_counted(monkeypatch, tmp_path):
    repo, _ = _repo_with_testcase(tmp_path, "tests:\n  demo.any:\n    build_only: true\n")
    monkeypatch.setattr(s, "REPO", repo)
    legs = _run_main(monkeypatch, tmp_path, ["examples/aen/demo/src/main.c"])
    assert len(legs) == 2 * s.SUBSETS and legs[0]["roots"] == s.root_args(s.ROOTS)


def test_common_and_test_platform_allow_are_concatenated(tmp_path):
    """twister appends a test's platform_allow to common's; counting only one
    would drop the other SKU's leg (#2885 review round 3)."""
    repo, roots = _repo_with_testcase(tmp_path, (
        "common:\n  platform_allow: alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp\n"
        "tests:\n  demo.a:\n    platform_allow: alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp\n"))
    assert s.config_counts(roots, repo) == {"aen801": 1, "aen803": 1}


def test_matrix_refuses_roots_without_counts():
    import pytest

    with pytest.raises(ValueError):
        s.matrix(["aen801"], [EX])
