### Fixed — 52 files leave the `IMPLICIT-ENCODING` baseline: 168 file-IO and non-Python subprocess calls now pass `encoding="utf-8"` (#2197)

This is the second #2197 drain batch. It covers every baselined file whose
remaining findings are all file IO or subprocess calls with a non-Python child:

- 123 `read_text()`/`write_text()` calls;
- 45 subprocess calls whose child is `git`, `nm`, `cmake`, `cc`, `gh`, `awk` or
  plain `bash`.

Each call now passes `encoding="utf-8"`. Before this change, all 168 calls used
the platform locale: cp1252 on `windows-latest`, ASCII under `LC_ALL=C`. With
`LC_ALL=C` and `PYTHONUTF8=0`, the drained test files had 17 failures and 7
errors, all `UnicodeDecodeError: 'ascii' codec`. They read non-ASCII metadata
and `scripts/test-all.sh`, for example
`tests/scripts/test_topology_unresolved_core_type.py:90` ("E1M-AEN301.yaml").
All 564 tests now pass. `IMPLICIT_ENCODING_BASELINE` shrinks from 137 to 85
files, and the size pin moves with it:
`tests/scripts/test_check_cross_platform.py:536`
("IMPLICIT_ENCODING_BASELINE) <= 85"). The baselined finding count drops from
484 to 316.

**A file joins this batch only if none of its calls starts a Python child** -- with one exception handled in place: `baremetal_cmake_flash.py`'s `cmake --build --target flash` runs a project-supplied target that is often a Python flasher, so that call also sets `PYTHONIOENCODING=utf-8` in the child's environment.
Python children include `sys.executable`, `python3`, `west`, `twister`, `pyocd`,
`vela`, `dxcom`, and any bash script that runs `python3` while its output is
captured. For those calls, `encoding=` is only half the fix. The child also
needs `PYTHONIOENCODING=utf-8` (see #2201), so those files wait for the next
batch.

Two bash-driven test files stay in this batch after tracing:

- `tests/scripts/test_bench_jlink_run.py` and
  `tests/scripts/test_bench_ram_run_two_session.py` both source
  `scripts/bench/aen/bench-env.sh`. That script's only `python3` call runs when
  `LG_PLACE` is set, and both tests unset it.
- The fake J-Link wrapper in the ram-run test also calls `python3`, but it
  writes a timestamp to a file, not to the captured pipe.

**`errors="replace"` goes only where output is shown to a person or searched
for an ASCII marker.** Seven calls get it:

- the `cmake` configure, build and flash steps;
- the `cc` compile diagnostics in `scripts/check_stub_symbol_matrix.py`;
- the conflicting `git merge` in the conflict-resolver test.

For example, `scripts/flash_backends/baremetal_cmake_flash.py:87` ("errors=").
Calls whose output is parsed as data stay strict: git paths, `nm` symbols and
`gh` JSON.

**Four `read_text()` calls inside double-quoted f-strings use
`encoding='utf-8'`.** `requires-python` is `>=3.10`, and reusing an f-string's
own quote inside `{}` needs Python 3.12:
`scripts/gen_soc_caps.py:632` ("read_text(encoding='utf-8')").

**The edit was mechanical and checked file by file.** The keyword was inserted
at each linter-reported (line, column) position, guided by the AST. Two checks
passed for every edited file:

- the file re-parses, and its `ast.dump` matches the original once the added
  keywords are stripped;
- the added keywords are on exactly the flagged `read_text`, `write_text`,
  `run` and `Popen` calls.
