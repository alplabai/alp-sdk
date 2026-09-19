### Fixed — the last 2 files leave the `IMPLICIT-ENCODING` baseline, and the baseline machinery itself is retired (#2197)

This is the fourth and final #2197 drain batch. `scripts/alp_quality.py` and
`tests/scripts/test_check_example_board_overlay_parity.py` were the two files
reserved for this last batch (both also start a Python child --
`sys.executable` and the gate script itself -- the same shape the third
batch drained everywhere else). Each subprocess call now pairs `encoding="utf-8"` with a
child environment carrying `PYTHONIOENCODING=utf-8`, the same idiom the third
batch used everywhere else:
`scripts/alp_quality.py:91` ("capture_output=True, text=True") and
`tests/scripts/test_check_example_board_overlay_parity.py:64`
(`env={**os.environ,`). The parity test's seven `write_text()` calls
also get `encoding="utf-8"`, for example
`tests/scripts/test_check_example_board_overlay_parity.py:76`
("format(name=name, platform_allow=allow_lines)").

With both files clean, the `IMPLICIT_ENCODING_BASELINE` grandfather set is
empty, so it is removed rather than left as a dead compat layer -- this repo
does not keep those. `Finding.baselined`, the scan's `file_baselined` branch,
and the `n_baselined` summary trailer go with it, and `main()` now fails
`--fail-on-warning` on every IMPLICIT-ENCODING finding, the same as every
other category:
`scripts/check_cross_platform.py:1208` ("if findings and args.fail_on_warning").

`tests/scripts/test_check_cross_platform.py` drops the four baseline-specific
tests in section 12; the behaviour they covered -- an IMPLICIT-ENCODING
finding must fail `--fail-on-warning` -- already had a baseline-free test in
section 11 (added alongside the CLI wiring, not by this change):
`tests/scripts/test_check_cross_platform.py:1151`
("def test_implicit_encoding_cli_fail_on_warning_exits_one"), so nothing
replaces them. The real-repo `--fail-on-warning` regression test drops its
two shrink-only baseline guards and keeps a plain zero-findings assertion
over `linter.scan(...)`:
`tests/scripts/test_check_cross_platform.py:496`
("def test_linter_fail_on_warning_against_real_repo_passes").

`.github/workflows/cross-platform-zephyr.yml`'s step comment no longer says
IMPLICIT-ENCODING warnings "still exit 0, being grandfathered":
`.github/workflows/cross-platform-zephyr.yml:122` ("the step now fails on any finding,").
`docs/cross-platform-setup.md` §6.2 is rewritten to match, against a live run
of the script: `docs/cross-platform-setup.md:681`
("check_cross_platform: 0 finding(s)"). ADR 0012's already-merged #2195
amendment is left untouched (amend, don't rewrite a decided record) and a new
nested amendment is added beneath it:
`docs/adr/0012-cross-platform-developer-host.md:228`
("grandfather baseline that tracked it is").

**The default walk's scope was too narrow: `PY_SCAN_ROOTS` did not cover
`examples/`,** even though every tracked `*.py` file outside `scripts/` and
`tests/` lives there -- five `gen_model.py` NPU-inference generators, all
customer-facing. `PY_SCAN_ROOTS` now covers all three trees:
`scripts/check_cross_platform.py:238`
("PY_SCAN_ROOTS: tuple[str, ...] = ("). Each generator's `open(out_h, "w")`
gets `encoding="utf-8")`, for example
`examples/aen/aen-npu-inference-alif/gen_model.py:154`
("with open(out_h,") and the same `open(out_h, "w")` call at line 152 of
each of the other four generators' `gen_model.py` (`aen-npu-inference`,
`aen-npu-inference-alp`, `aen-npu-inference-alp-u55`, and
`aen-npu-inference-person-mram`). Measured: with
`PY_SCAN_ROOTS` widened but before these five fixes, `--fail-on-warning`
reported exactly 5 IMPLICIT-ENCODING findings, one per generator; after, 0.
The module docstring, its Scope section, the `discover_files` comment, and
`metadata/quality-tasks-v1.json`'s task description are updated to match, and
`tests/scripts/test_check_cross_platform.py`'s two `PY_SCAN_ROOTS` tests are
rewritten around `examples/` now being in scope and `docs/` staying out of it.

`python3 scripts/check_cross_platform.py` and
`python3 scripts/check_cross_platform.py --fail-on-warning` both print zero
findings and exit 0 against the live tree, with only the four
`INTENTIONALLY_DISCUSSES_OS_PATHS` allowlist summaries left in the output.
`metadata/catalog.json` was regenerated; its `check_cross_platform.py`
`purpose` line now names `examples/` alongside `scripts/` and `tests/`.
