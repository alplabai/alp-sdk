### Fixed — the functest shell runner no longer hangs or times out spuriously on a loaded host (#2673)

`test_the_runner_frames_each_check_runs_lanes_concurrently_and_kills_an_overrun`
flaked on CI in two ways. The runner's per-check watcher was reaped with
`kill` and `wait`; a TERM landing while its subshell was still starting is
deferred past the watcher's `sleep` (dash), so the final `wait` blocked for
the whole check timeout. The watcher now polls for the check's `.rc` file and
leaves on its own, with nothing signalling it. Separately, the test left every
check that must succeed on the 5 s default timeout, so a stalled runner turned
`rc` or `nope` into the timeout marker `T`; those now carry an explicit 120 s
margin and only `hang` keeps its 1 s timeout. The test failed 7 of 12 runs under a
64-process CPU burner before, passed 12 of 12 under 24 burners after, and 30 of 30 unloaded.
