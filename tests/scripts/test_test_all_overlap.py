"""Failure accounting for scripts/test-all.sh's twister/read-only-stage overlap.

The overlap (`launch_bg` / `launch` / `launch_skip` / `writer_stage` /
`finish_overlap`) moves stage results through files instead of the live
`run_stage` path, so a lost or misread result is the risk: a stage that failed
must still turn the run red, a prerequisite gap must still exit 2, and a
tree-writing stage must never start before the background twister has been
joined.

Like test_test_all_prerequisite_gap.py this extracts the real functions from
the script by marker and drives them with fake stages in a throwaway bash
harness. Each scenario is also run against a deliberately broken copy of
`finish_overlap` (MUTANTS): the scenario must go red there, proving the test
would catch that regression.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux") or shutil.which("bash") is None,
    reason="drives extracted fragments of scripts/test-all.sh under bash on Linux "
    "(same restriction as test_test_all_prerequisite_gap.py)",
)


def _function(text: str, name: str) -> str:
    m = re.search(rf"^{re.escape(name)}\(\) \{{\n.*?^\}}\n", text, re.MULTILINE | re.DOTALL)
    assert m, f"could not find `{name}()` in {TEST_ALL}"
    return m.group(0)


def _overlap_block(text: str) -> str:
    start = text.index("# -------- Overlapped execution")
    end = text.index("# `import jsonschema` succeeding")
    return text[start:end]


def _harness(body: str, tmp_path: Path, env_extra=None, mutant=None) -> subprocess.CompletedProcess:
    text = TEST_ALL.read_text(encoding="utf-8")
    block = _overlap_block(text)
    if mutant is not None:
        old, new = mutant
        assert old in block, f"mutant target {old!r} not in the overlap block -- test rotted"
        block = block.replace(old, new)
    script = tmp_path / "harness.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        "set -uo pipefail\n"
        "declare -a STAGE_NAMES STAGE_STATUS STAGE_NOTES STAGE_KIND STAGE_SECS\n"
        "TARGET=dev\nSTART=0\n"
        f"D={tmp_path.as_posix()!r}\n"
        f"{_function(text, '_record_stage')}\n"
        f"{_function(text, 'run_stage')}\n"
        f"{_function(text, 'skip_stage')}\n"
        f"{block}\n"
        "fake_pass() { echo pass-output; return 0; }\n"
        "fake_fail() { echo boom-output; return 1; }\n"
        "fake_gap()  { return 99; }\n"
        "fake_slow() { sleep 1; touch \"${D}/twister-done\"; return 0; }\n"
        # A twister that cannot finish until a pool stage releases it, then
        # takes 4 s more: a writer that starts before the join sees no marker.
        "fake_gated() { n=0; while [ ! -f \"${D}/release\" ] && [ \"${n}\" -lt 1200 ]; do "
        "sleep 0.05; n=$((n+1)); done; sleep 4; touch \"${D}/twister-done\"; return 0; }\n"
        "fake_release() { touch \"${D}/release\"; return 0; }\n"
        "fake_die()  { kill -9 \"${BASHPID}\"; }\n"
        "fake_writer() { [ -f \"${D}/twister-done\" ] || { echo writer-ran-early; return 1; }; return 0; }\n"
        f"{body}\n"
        "END=0\n"
        f"{_summary(text)}\n",
        encoding="utf-8",
    )
    env = {k: v for k, v in os.environ.items()
           if k not in ("ALP_GATE_SERIAL", "ALP_TWISTER_JOBS")}
    env["ALP_GATE_MIN_MEM_KB"] = "0"
    env.update(env_extra or {})
    return subprocess.run(["bash", script.as_posix()], capture_output=True, text=True,
                          encoding="utf-8", timeout=60, env=env)


def _summary(text: str) -> str:
    return text[text.index("# -------- Summary "):]


# ---- scenarios: (body, assertion) -- each is also run against a mutant -------

def _s_bg_fail(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_fail\nlaunch a fake_pass\nfinish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 1, p.stdout + p.stderr
    assert "All runnable stages passed" not in p.stdout


def _s_readonly_fail_prints_output(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_pass\nlaunch a fake_fail\nfinish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 1, p.stdout + p.stderr
    assert "boom-output" in p.stdout
    assert "All runnable stages passed" not in p.stdout


def _s_readonly_gap_exit_2(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_pass\nlaunch a fake_gap\nfinish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 2, p.stdout + p.stderr
    assert "[GAP]" in p.stdout
    assert "All runnable stages passed" not in p.stdout


def _s_deferred_gap_skip_exit_2(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_pass\nlaunch_skip s "tool missing" gap\nfinish_overlap\n',
                 tmp_path, mutant=mutant)
    assert p.returncode == 2, p.stdout + p.stderr
    assert "[GAP]" in p.stdout


def _s_missing_result_is_fail(tmp_path, mutant=None):
    # twister dies without writing its .res: that must read as FAIL, not PASS.
    p = _harness('launch_bg twister fake_die\nlaunch a fake_pass\nfinish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 1, p.stdout + p.stderr
    assert "All runnable stages passed" not in p.stdout


def _s_writers_run_after_join(tmp_path, mutant=None):
    # Twister is released by pool stage `a` and then needs 4 more seconds, so
    # only a real join keeps the writer from running first (no reliance on
    # how fast the host happens to be).
    p = _harness('launch_bg twister fake_gated\nlaunch a fake_release\n'
                 'writer_stage w fake_writer\nfinish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "writer-ran-early" not in p.stdout
    assert p.stdout.index("[twister] PASS") < p.stdout.index("[w] PASS")


def _s_failing_writer_is_reported(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_pass\nwriter_stage w fake_fail\nfinish_overlap\n',
                 tmp_path, mutant=mutant)
    assert p.returncode == 1, p.stdout + p.stderr
    assert "[w] FAIL" in p.stdout


def _s_output_in_script_order(tmp_path, mutant=None):
    p = _harness('launch_bg twister fake_slow\nlaunch a fake_pass\nlaunch b fake_pass\n'
                 'finish_overlap\n', tmp_path, mutant=mutant)
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout
    assert out.index("===== [twister] =====") < out.index("===== [a] =====") < out.index("===== [b] =====")


def _s_pool_is_bounded(tmp_path, mutant=None):
    # Four 1 s stages, ALP_GATE_STAGE_JOBS=2: never more than 2 at once, and
    # really 2 at once (not serial).
    body = (
        'cnt() { touch "${D}/r.$1"; ls "${D}"/r.* | wc -l >> "${D}/max"; sleep 1; rm -f "${D}/r.$1"; }\n'
        'c1() { cnt 1; }\nc2() { cnt 2; }\nc3() { cnt 3; }\nc4() { cnt 4; }\n'
        'launch s1 c1\nlaunch s2 c2\nlaunch s3 c3\nlaunch s4 c4\nfinish_overlap\n'
    )
    p = _harness(body, tmp_path, env_extra={"ALP_GATE_STAGE_JOBS": "2"}, mutant=mutant)
    assert p.returncode == 0, p.stdout + p.stderr
    seen = [int(x) for x in (tmp_path / "max").read_text(encoding="utf-8").split()]
    assert max(seen) == 2, seen


def _s_slow_stages_start_first(tmp_path, mutant=None):
    body = (
        'st() { echo "$1" >> "${D}/order"; }\n'
        'qa() { st a; }\nqb() { st b; }\nqp() { st pytest; }\nqr() { st required; }\n'
        'launch a qa\nlaunch b qb\nlaunch pytest-scripts qp\nlaunch required-gate-scripts qr\n'
        'finish_overlap\n'
    )
    p = _harness(body, tmp_path, env_extra={"ALP_GATE_STAGE_JOBS": "1"}, mutant=mutant)
    assert p.returncode == 0, p.stdout + p.stderr
    assert (tmp_path / "order").read_text(encoding="utf-8").split() == ["pytest", "required", "a", "b"]


def _s_writers_wait_for_the_whole_pool(tmp_path, mutant=None):
    # No twister at all (a docs-only run): the pool alone must finish before
    # the writer starts.
    body = (
        'slow() { sleep 1; touch "${D}/pool-done"; }\n'
        'w() { [ -f "${D}/pool-done" ] || { echo writer-ran-early; return 1; }; }\n'
        'launch a slow\nlaunch b fake_pass\nwriter_stage w w\nfinish_overlap\n'
    )
    p = _harness(body, tmp_path, mutant=mutant)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "writer-ran-early" not in p.stdout


SCENARIOS = {
    "pool_bounded": _s_pool_is_bounded,
    "slow_first": _s_slow_stages_start_first,
    "writers_after_pool": _s_writers_wait_for_the_whole_pool,
    "bg_fail": _s_bg_fail,
    "readonly_fail": _s_readonly_fail_prints_output,
    "readonly_gap": _s_readonly_gap_exit_2,
    "deferred_gap_skip": _s_deferred_gap_skip_exit_2,
    "missing_result": _s_missing_result_is_fail,
    "writers_after_join": _s_writers_run_after_join,
    "failing_writer": _s_failing_writer_is_reported,
    "order": _s_output_in_script_order,
}


@pytest.mark.parametrize("name", sorted(SCENARIOS))
def test_scenario_on_the_real_script(name, tmp_path):
    SCENARIOS[name](tmp_path)


# Each mutant breaks one property of finish_overlap; the named scenario must
# notice.
MUTANTS = {
    "result_default_pass": ("rc=1; secs=0", "rc=0; secs=0", "missing_result"),
    "skip_kind_forced_scope": ('set -- "${name}" "${sreason}" "${skind}"',
                               'set -- "${name}" "r" scope', "deferred_gap_skip"),
    "no_join_before_writers": ('wait "${TWISTER_PID}"', ":", "writers_after_join"),
    "writers_dropped": ('run_stage "${WRITER_NAMES[$i]}" "${WRITER_FNS[$i]}"', ":", "failing_writer"),
    "rc_ignored": ('_record_stage "${name}" "${rc}" "${secs}"',
                   '_record_stage "${name}" 0 "${secs}"', "readonly_fail"),
    "output_not_printed": ('cat "${DEFER_DIR}/${name}.out"', ":", "readonly_fail"),
    "pool_unbounded": ('[ "${#RUN_PIDS[@]}" -lt "${jobs}" ] && [ "${next}" -lt "${n}" ]',
                       '[ "${next}" -lt "${n}" ]', "pool_bounded"),
    "no_priority": ('POOL_FIRST="pytest-scripts required-gate-scripts bash32-parse"',
                    'POOL_FIRST="none"', "slow_first"),
    "pool_not_awaited": ("        _run_pool\n", "        :\n", "writers_after_pool"),
}


@pytest.mark.parametrize("mutant", sorted(MUTANTS))
def test_scenario_fails_against_a_broken_finish_overlap(mutant, tmp_path):
    old, new, scenario = MUTANTS[mutant]
    with pytest.raises(AssertionError):
        SCENARIOS[scenario](tmp_path, mutant=(old, new))


# ---- fall-through, opt-outs, cleanup -----------------------------------------

def test_without_a_background_twister_the_pool_still_runs_in_script_order(tmp_path):
    # Twister skipped (docs-only change): the read-only stages still overlap
    # with each other, and print in script order.
    p = _harness('launch a fake_pass\nwriter_stage w fake_pass\nlaunch_skip s why scope\n'
                 'finish_overlap\n', tmp_path)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "started in the background" not in p.stdout
    o = p.stdout
    # writers always come last, after every read-only row
    assert o.index("===== [a] =====") < o.index("===== [s] SKIP") < o.index("===== [w] =====")


@pytest.mark.parametrize("env", [{"ALP_GATE_SERIAL": "1"}, {"ALP_TWISTER_JOBS": "4"},
                                 {"ALP_GATE_MIN_MEM_KB": "999999999999"}])
def test_opt_outs_run_serially(env, tmp_path):
    if "ALP_GATE_MIN_MEM_KB" in env and not Path("/proc/meminfo").exists():
        pytest.skip("no /proc/meminfo")
    p = _harness('launch_bg twister fake_pass\nlaunch a fake_pass\nfinish_overlap\n', tmp_path, env_extra=env)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "started in the background" not in p.stdout
    assert "===== [twister] =====" in p.stdout


def test_sigterm_mid_pool_prints_captured_output_and_kills_every_stage(tmp_path):
    # `killer` (a pool stage) TERMs the script while twister and another pool
    # stage are still sleeping: exit 143, the finished stage's output must
    # still be shown, and no child may outlive the script.
    tw_pid = (tmp_path / "tw.pid").as_posix()
    sl_pid = (tmp_path / "sl.pid").as_posix()
    body = (
        f'fake_tw() {{ sleep 300 & echo $! > "{tw_pid}"; wait; }}\n'
        f'sleeper() {{ sleep 300 & echo $! > "{sl_pid}"; wait; }}\n'
        f'killer() {{ while [ ! -s "{tw_pid}" ] || [ ! -s "{sl_pid}" ]; do sleep 0.1; done; '
        'kill -TERM $$; sleep 30; }\n'
        'launch_bg twister fake_tw\n'
        'launch a fake_fail\nlaunch sleeper sleeper\nlaunch killer killer\n'
        'finish_overlap\n'
    )
    p = _harness(body, tmp_path)
    assert p.returncode == 143, p.stdout + p.stderr
    assert "boom-output" in p.stdout, "captured output must survive an abnormal exit"
    for f in (tw_pid, sl_pid):
        pid = int(Path(f).read_text(encoding="utf-8").strip())
        for _ in range(30):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        else:
            os.kill(pid, 9)
            pytest.fail("a stage's child process outlived the script")
