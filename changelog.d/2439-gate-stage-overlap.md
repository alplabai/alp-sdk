### Changed — `test-all.sh` overlaps twister with the read-only stages and times every stage (#2439)

`scripts/test-all.sh` used to run twister to completion and only then walk the
cheap gates one by one, so the minutes of clang-format, shellcheck, bash32-parse,
metadata, gate scripts and pytest were added on top of the twister wall -- and a
docs-only run that skipped twister still ran them all serially (1005 s measured).
Twister now starts in the background (its output captured) and the read-only
stages run as a bounded pool (`ALP_GATE_STAGE_JOBS`, default 4; slowest first)
whether or not twister runs. Once everything is done the captured output of
every stage is printed in the original stage order. The stages that write the
checkout -- `generated-files`, `alp-lock`, `abi-strict` and the
`pytest -m repo_writes` half (now its own `pytest-repo-writes` row) -- run only
after twister and the pool, because regenerating headers under a live twister
build is what flakes `ALP_SOC_REF_STR undeclared`.

Two pieces of duplicate work went with it. `check_stub_symbol_matrix.py` now
compiles its 31 override combinations concurrently (through `ccache` when
present), 49 s -> 10 s on a Windows box with the same byte-identical golden.
And three pytest tests that re-ran a live-repo gate script the same run had
already executed -- `test_live_repo_is_clean`, `test_default_corpus_byte_identical`,
`test_every_committed_golden_is_in_sync` -- carry a new `gate_duplicate` marker
that a full `test-all.sh` run deselects; CI's pytest sweep still runs them.

Every stage line now ends with its wall time (`[twister] PASS (412s)`) and the
SUMMARY rows carry it too. PASS/FAIL/SKIP semantics, exit codes 0/1/2 and the
`All runnable stages passed.` text are unchanged. `ALP_GATE_SERIAL=1` restores
the old strictly-serial, live-output order for debugging. The overlap also
switches itself off when `ALP_TWISTER_JOBS` is set or `MemAvailable` is under
12 GiB (`ALP_GATE_MIN_MEM_KB`), caps the concurrent stages' pytest workers and
gate-script jobs, and kills twister's whole process group on Ctrl-C or an
abnormal exit while still printing any captured output that was never shown.
