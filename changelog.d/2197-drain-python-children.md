### Fixed — 83 files leave the `IMPLICIT-ENCODING` baseline, and every Python child now writes the UTF-8 its parent decodes (#2197)

This is the third #2197 drain batch. It covers every remaining baselined file
except `scripts/alp_quality.py` and
`tests/scripts/test_check_example_board_overlay_parity.py`, which are kept for
the last batch. These are the files that start a Python child process. They
also hold 164 plain file-IO calls, which are fixed here too.

The batch changes 307 calls:

- 163 `read_text()`/`write_text()` calls and one `os.fdopen()`;
- 109 subprocess calls whose child is Python itself: `sys.executable`,
  `python3`, `west`, `twister`, `pyocd`, `bmaptool`, `vela` or `dxcom`;
- 17 subprocess calls that reach Python through bash: `scripts/test-all.sh`
  stages, the `release.yml` and `pr-generated-files.yml` `run:` steps, and
  `scripts/bootstrap.sh`;
- 17 subprocess calls whose child is not Python: eight `git` calls, `hostname`, `whoami`,
  `JLinkExe`, `dd`, `pwsh`, a `bash -c printf` probe, and three bash runs
  that never reach their `python3` line.

**A Python child needs two changes, not one.** Today both ends of the pipe use
the locale, cp1252 on `windows-latest`, and they agree by accident. Setting only
the parent's `encoding="utf-8"` breaks that agreement; #2201 did exactly this
and #2202 fixed it. So each of the 126 Python-child calls gets
`encoding="utf-8"` and also a child environment with `PYTHONIOENCODING=utf-8`.
Four shapes cover them all:

- With no `env=`, the call passes `env={**os.environ, "PYTHONIOENCODING": "utf-8"}`.
  64 files needed a new `import os` for this.
- An `env` built from `os.environ` gets the key added to its dict. For example
  `scripts/alp_model/adapters/drpai.py:213` ("PYTHONIOENCODING").
- A minimal environment built from scratch gets the key inside that dict, and
  `os.environ` is not merged in, so the test still controls exactly what the
  child sees: `tests/scripts/test_check_bootstrap_manifest.py:596`
  ("str(shim_dir)") and `tests/scripts/test_release_tag_verify.py:73`
  ("PYTHONIOENCODING").
- A helper that receives `env` from its caller merges the key over that `env`,
  or over `os.environ` when there is none:
  `scripts/alp_mcp/server.py:477` ("env={**(env or os.environ)"),
  `tests/scripts/test_provision_som.py:70` ("env={**(env or os.environ)"), and
  the ten `**kw` helpers that the linter fix above exposed, for example
  `tests/scripts/test_check_test_coverage.py:16` ("env={**(kw.pop(").

The two shared helpers are fixed once for every caller: the MCP server's
`_run` above, and `_run_loader`, which every `test_project_*.py` file uses:
`tests/scripts/_project_support.py:46` ("env={**os.environ,").

**The `release.yml` changelog step's output file is UTF-8 too.** That step
redirects the child's stdout into `release_notes.md`, so `PYTHONIOENCODING`
sets the encoding of the file itself:
`tests/scripts/test_release_changelog_slice.py:95` ("PYTHONIOENCODING").

**`errors="replace"` goes only where tool output passes straight through.**
Nine calls get it: `west build` in `scripts/alp_orchestrate/kconfig_symbols.py`,
`west flash` in `scripts/flash_backends/zephyr_west_flash.py:137`
("errors="), the J-Link, pyOCD/OpenOCD, `dd` and `bmaptool` flash calls, the
build/flash/capture runner in `tests/hil/run_smoke.py`, and `pwsh`. Compiler,
flasher and UART bytes reach these pipes without going through Python, and
`pwsh` writes the console codepage, so `PYTHONIOENCODING` cannot make that
output UTF-8. The J-Link command script written through `os.fdopen()` is now
UTF-8 as well:
`scripts/flash_backends/swd_probe.py:271` ("os.fdopen(fd,").

**The adapter tests now check the child encoding.** Their fake
`subprocess.run` functions accept the new keywords, and the Vela and DRP-AI
tests assert that the call both decodes and requests UTF-8:
`tests/scripts/test_alp_model_adapters.py:135` ("assert seen[").

`IMPLICIT_ENCODING_BASELINE` shrinks from 85 to 2 files:
`scripts/check_cross_platform.py:360`
("IMPLICIT_ENCODING_BASELINE: frozenset"). The size pin moves with it:
`tests/scripts/test_check_cross_platform.py:536`
("IMPLICIT_ENCODING_BASELINE) <= 2"). The baselined finding count drops from
316 to 9.

**Measured: the child's encoding now wins over the caller's locale.** The 113
test files that are touched here, or that run a changed script, were run with
`PYTHONIOENCODING=cp1252` and then with `PYTHONIOENCODING=ascii` set in the
pytest environment. Before this change, every child inherited that setting:

- under `cp1252`, 64 tests failed with `UnicodeDecodeError: 'utf-8' codec can't
  decode byte 0xb7` (or `0xa7`), because the child wrote `·` or `§` as cp1252;
- under `ascii`, 28 tests failed with `UnicodeEncodeError: 'ascii' codec can't
  encode character '\xa7'` inside the child.

Both runs now pass all 2105 tests. The largest group is
`tests/scripts/test_check_template_catalog.py:27` ("PYTHONIOENCODING"), with 17
failures under `cp1252`.

Six calls whose child is not Python today also set `PYTHONIOENCODING`, because
the command they run belongs to the project or the bench and may call Python
later. These are the J-Link command in `swd_probe.py` (the `JLINK_EXE`
override can be a wrapper script), `pwsh bootstrap.ps1`, `test-all.sh --help`,
two `bench-env.sh` runs through the shared `_sanitized_env()` helper, and
`git worktree add` on the real checkout, which runs the local `post-checkout`
hook. The variable has no effect on a child that is not Python.
