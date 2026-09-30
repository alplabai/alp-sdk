### Changed — `aen-cc3501e-socket-throughput` retries a Wi-Fi connect that timed out (#2394)

STEP 3 now makes up to three `WIFI_CONNECT` attempts. It retries only when
the firmware's own `WIFI_STATUS` verdict is `CONN_FAILED` with
`FAIL_TIMEOUT`. A credential, security or role rejection, or an unreadable
status, still stops the run on the first attempt, because a retry cannot
fix those.

Why: the CC3501E's on-module antenna hears a same-room AP at about -80 to
-84 dBm, roughly 40 dB below a laptop in the same room (E1M-AEN803
2026W36-0009, 2026-09-28). On that marginal link about half of the
associations ran out of the firmware's connect budget (#2394), while the
others connected on the first attempt.

Bench, same unit, 2026-09-28, cold boots, bench lab AP (WPA2/WPA3
transition mode):
- WPA2-PSK: 0 of 6 attempts associated over two runs. Every attempt was
  `FAIL_TIMEOUT`, so the retry cannot help a run where the radio never gets
  through.
- WPA3-SAE: the first two attempts timed out and the third associated, got
  a DHCP lease, and completed the HTTP download.

A single-attempt build would have reported that run as a failure.
