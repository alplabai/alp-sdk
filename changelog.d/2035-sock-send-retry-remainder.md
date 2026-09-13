### Fixed — `cc3501e_sock_send()` now retries the remainder instead of assuming a full queue (#2035)

`cc3501e-bridge-firmware#107` moves the bridge's `SOCK_SEND` handler from a
blocking `lwip_send(..., 0)` to `lwip_send(..., MSG_DONTWAIT)`: the blocking
call parked the firmware's worker -- and READY along with it -- for as long
as a peer declined to read, wedging every opcode on the bridge for that whole
span. `MSG_DONTWAIT` never blocks, so a full peer receive buffer now reports
0 bytes queued, `ALP_OK`, immediately, where the old firmware would
eventually have blocked until it could report the full count.

That exposed a latent host-side assumption: `cc3501e_sock_send()` built one
frame, polled it once, and reported whatever the firmware queued -- callers
that pass `NULL` for `sent_out` (e.g.
`src/zephyr/console/alp_console_companion_sock.c`) relied on a short queue
being effectively impossible. It no longer is.

`cc3501e_sock_send()` now keeps issuing the remainder as its own short
transaction -- never parking the firmware -- until every byte of `len` is
queued or `timeout_ms` elapses:

- Each iteration is a **new logical send** carrying only the remaining bytes,
  and gets a **new** `sock_send_seq`: the firmware caches the single most
  recent `(seq, reply)` pair so a genuine retry of an identical frame is
  served from that cache rather than re-submitted, but a new iteration's
  shrunk payload must not be aliased to the previous one's cached reply
  (alp-sdk#1746 / cc3501e-bridge-firmware#88's original mechanism, unchanged).
- The overall budget is tracked from actually-elapsed time (one deadline read
  off the same monotonic clock `poll_by_repeat()` itself uses, rebudgeted
  into each iteration's `poll_by_repeat()` call), not a declared per-attempt
  cost -- the same class of bug #2035 already fixed once in
  `cc3501e_wifi_connect()`'s status-poll loop.
- Zero-progress iterations back off a fixed 20 ms before retrying, so a peer
  that never reads cannot turn this into a hot loop hammering the SPI link;
  partial-progress iterations continue immediately. A new iteration is never
  started once less than that 20 ms remains -- checked fresh at the top of
  every iteration but the first, specifically INCLUDING right after the
  back-off's own sleep, not just before it (checking only beforehand could
  hand the next iteration a sliver too small to plausibly get a fresh frame
  answered, making an overrun of `timeout_ms` the normal ending under
  backpressure instead of a rare edge). The back-off itself is also skipped
  in favour of an immediate `ALP_ERR_TIMEOUT` once 20 ms or less remains, so
  it cannot overshoot a small `timeout_ms` outright (a 4 ms budget used to
  come back after ~23 ms).
- `sent_out` now reports the **total** queued across every iteration: the
  full `len` on `ALP_OK`. On `ALP_ERR_TIMEOUT` it is an EXACT count if the
  last in-flight frame's outcome was collected, or a LOWER BOUND if even the
  post-timeout grace (below) expired -- a lower bound is safe to REPORT, not
  to resume from: retrying more bytes on the assumption it is exact
  duplicates whatever the bridge already queued for the abandoned frame.
  After a lower-bound `ALP_ERR_TIMEOUT`, close the socket instead of
  resuming.
- **Review follow-up.** A `timeout_ms` that runs out while a frame's own
  `poll_by_repeat()` retry is genuinely in flight (the firmware answered
  `RESP_ERR_BUSY` -- it accepted the job but has not finished) used to
  abandon that frame outright. The shipping bridge
  (cc3501e-bridge-firmware `fix/107-bound-sock-send-seqguard`) discards a
  finished-but-uncollected job once a request with a different seq arrives,
  so a later, unrelated `cc3501e_sock_send()` call is never handed a stale
  count as if it were its own reply -- but that job's bytes were still
  queued, and once discarded, that count is gone for good. Fixed with a
  bounded (`CC3501E_SOCK_SEND_COLLECT_GRACE_MS`, 250 ms) post-timeout re-poll
  of the SAME frame (same seq, same remaining bytes) to collect its outcome
  before giving up: if that collects the queued count, it is folded into
  `sent_out` (exact, not a lower bound); if the grace instead collects a
  genuine, definitive non-OK status (e.g. a decoded device-side error), that
  status is returned directly and `sent_out` is exact -- that frame is done,
  not merely timed out; if the grace itself cannot resolve it, `sent_out` is
  documented as a LOWER bound.
- **Review follow-up.** `ALP_ERR_BUSY` does NOT enter that grace: a
  transport-lock-acquire timeout on `poll_by_repeat()`'s very FIRST attempt
  means no frame reached the bridge at all, so it is unambiguous and is
  returned directly. `poll_by_repeat()` itself now also retries a
  lock-acquire timeout on a LATER attempt within its own deadline instead of
  returning `ALP_ERR_BUSY` immediately, since by then an earlier attempt
  already reached the bridge and that status would misreport "nothing
  sent". This is a small behaviour change for every `poll_by_repeat()`
  caller: a rare, concurrent-lock-contention `ALP_ERR_BUSY` that used to be
  indistinguishable from (for `cc3501e_ble_gatt_register()`) a genuine
  terminal firmware reject now correctly comes back as `ALP_ERR_TIMEOUT`
  instead once the whole budget is exhausted contended.
- **Review follow-up.** A decoded `ALP_OK` reply with fewer than 2 data bytes
  (a firmware/wire gap, not backpressure) used to fold silently into "0
  queued" and retry to the whole budget; it now returns `ALP_ERR_IO`
  immediately, same as the equivalent short-reply guard in
  `cc3501e_sock_open()`.

`<alp/chips/cc3501e/sockets.h>`'s `cc3501e_sock_send()` doc comment is updated
to state the new contract, including the exact-vs-lower-bound `sent_out`
distinction and that only an exact count is safe to resume from. No caller
changes needed -- this is what makes every existing `NULL`-`sent_out` caller
correct again under the firmware change instead of only by accident.
