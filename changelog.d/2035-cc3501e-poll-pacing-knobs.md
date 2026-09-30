### Added — two host-side pacing knobs to confirm the CC3501E link-wedge mechanism on hardware (#2035)

The CC3501E link wedges mid-session on two different apps: after some
operation, every later opcode fails with pre-decode transport failures until
a cold cycle. The best-supported explanation is that the firmware tears down
and re-opens its SPI slave (`SPI_close` / `SPI_open` / re-arm) on its own task
after certain worker-routed operations, with no interlock against the host
still clocking — the READY line is an open connection on this board, so the
host's gates fall back to fixed delays. Bytes the host clocks across that
window are absorbed by the freshly armed transfer, leaving the slave
permanently one transfer behind. That predicts risk scales with how densely
the host is polling when the teardown lands, which matches the observed
pattern of the wedge following fast operations.

Two `chips/cc3501e/` Kconfig knobs let that be confirmed on the bench without
flashing the coprocessor, both defaulting to today's behaviour:

* `CONFIG_ALP_SDK_CC3501E_POLL_GAP_MIN_MS` raises `poll_by_repeat()`'s
  exponential-backoff floor (default `1`) up to the existing 50 ms ceiling,
  flattening the early dense-polling window where the teardown race is
  riskiest.
* `CONFIG_ALP_SDK_CC3501E_POST_SUCCESS_GUARD_MS` (default `0`, no-op) adds an
  optional delay after a worker-routed request succeeds, before the next
  request goes out, giving the firmware's teardown a chance to land on an
  idle bus. Scoped to `poll_by_repeat()`'s success path — the worker-routed
  request boundary — not to `cc3501e_request()`'s fast, non-worker-routed
  calls (`GET_VERSION`, `GET_CAPABILITIES`, `STREAM_WRITE`).

Both are mitigation/diagnostic knobs, not a fix: they change host timing
only, nothing about the wire protocol. Two earlier hypotheses were already
refuted and are not reintroduced here — there is no host-side timeout to
tighten (`cc3501e_request()` discards its timeout parameter outright), and a
FIFO flush or a `cc3501e_sync()` byte-walk in the firmware's re-open path
provably parks the slave instead of recovering it.
