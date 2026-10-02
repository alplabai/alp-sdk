### Fixed — the #2473 doxygen-cwd test skips on Windows (#2473)

`tests/scripts/test_test_all_doxygen_cwd.py` failed `python-smoke
(windows-latest)` on every PR: on the Windows runner `bash` resolves to the WSL
launcher, which has no distribution installed, so the extracted
`stage_doxygen` never ran. The test now skips on Windows, the same scope
`tests/scripts/test_abi_snapshot_freeze_gate.py` already uses for
`scripts/test-all.sh` functions. It still runs on Linux, macOS and WSL.
