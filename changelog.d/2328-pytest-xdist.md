### Changed — `pytest tests/scripts/` runs in parallel with pytest-xdist in CI and in `test-all.sh` (#2328)

The `tests/scripts/` suite (about 4,400 tests) ran serially in three places.
`pr-metadata-validate`'s pytest step had reached 19 to 20 minutes of its
20-minute cap, and each `cross-platform-zephyr` `python-smoke` job took 9 to 15
minutes per OS. The suite is almost entirely independent tests plus
subprocess spawns, so it parallelises cleanly.

- `pytest-xdist>=3.5` joins the `[dev]` extra. `uv.lock` gains `pytest-xdist`
  and its dependency `execnet`, and nothing else.
- Both CI invocations now pass `-n auto`, one worker per runner core:
  `pr-metadata-validate.yml`'s `Pytest -- linter + schema regression +
  topology defaults` step and `cross-platform-zephyr.yml`'s
  `pytest tests/scripts/` step. The first step's comment claimed these tests
  "run in <2 s" and cited a stale 13.5-minute worst case; it now describes
  the whole suite. The step's `timeout-minutes: 20` cap is unchanged.
- **Tests that write into the real checkout now run on their own.** Nine
  modules temporarily write into the checkout and clean up afterwards: fake
  `docs/abi/v99.9x-snapshot.json` baselines, `.test-*` fixtures under
  `metadata/e1m_modules/` and `metadata/chips/`, and a `git worktree add`
  against the shared `.git`. Under xdist, another worker can glob one of
  those transient files mid-test. This was observed at 12 workers:
  `test_abi_snapshot`'s real-tree check took the freeze-gate test's fake
  `v99.99` snapshot as the last released baseline.
  `tests/scripts/conftest.py` now lists those modules in one place,
  `_REPO_WRITER_MODULES`, and gives their tests a `repo_writes` marker. Every
  runner runs `-n auto -m "not repo_writes"` first, then `-m repo_writes`
  serially (123 of 4,465 tests, about 4 s). In both workflows the two phases
  are separate steps: on Windows, `run:` is pwsh, which only checks the last
  command's exit code.
- `scripts/test-all.sh`'s `pytest-scripts` stage runs the same two phases
  when `pytest-xdist` is importable, and otherwise still runs the suite
  serially, so a venv without the new extra keeps working.

Locally with 12 workers, the parallel phase took 3:00 in each of three
repeated runs, with the same result every time; the same suite serially takes
12:51. No test failed because of parallel execution, and none left a file
behind in the checkout.
