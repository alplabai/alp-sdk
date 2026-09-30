### Added

- `tests/hil/run_smoke.py` `ssh-run` specs may now carry `ssh_args:` (argv passed to the remote binary) and `ssh_files:` (`{local, remote}` pairs scp'd alongside the binary before it runs) -- an example that takes filename arguments, not just its own binary, previously had no way to get them onto the target. New E1M-V2M103 spec `v2n-drpai-inference.yaml` for `examples/v2n/v2n-drpai-inference` (`<model.tar> <frame0.bin>` argv) documents the expected printouts; not bench-run by this change -- no compiled DRP-AI model bundle exists in-tree yet (#1160, #2236).
