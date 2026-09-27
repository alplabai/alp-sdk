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
- `scripts/test-all.sh`'s `pytest-scripts` stage adds `-n auto` when
  `pytest-xdist` is importable, and otherwise still runs serially, so a venv
  without the new extra keeps working.

Locally, on the full suite with 4 workers, the run took 4:09 against 12:51
serial, and no test failed because of parallel execution.
