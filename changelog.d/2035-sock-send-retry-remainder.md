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
- Zero-progress iterations back off (bounded to whatever budget remains)
  before retrying, so a peer that never reads cannot turn this into a hot
  loop hammering the SPI link; partial-progress iterations continue
  immediately.
- `sent_out` now reports the **total** queued across every iteration: the
  full `len` on `ALP_OK`, or a partial count on `ALP_ERR_TIMEOUT` so a caller
  that does check it can recover. A non-`ALP_OK` status from
  `poll_by_repeat()` itself still returns immediately, unchanged.

`<alp/chips/cc3501e/sockets.h>`'s `cc3501e_sock_send()` doc comment is updated
to state the new contract. No caller changes needed -- this is what makes
every existing `NULL`-`sent_out` caller correct again under the firmware
change instead of only by accident.
