### Fixed — `cc3501e_wifi_connect()` no longer gives up at ~1/3 of its caller's declared timeout (#2035)

Its poll loop bounded itself with a decrementing `remaining` ledger that
phantom-debited `CC3501E_REQ_TMO_MS` (100 ms) as a defensive worst-case
estimate on every failed `WIFI_STATUS` read, on top of the real 50 ms poll
gap it then slept — 150 ms charged per failed iteration for ~54 ms of actual
wall clock. On a board where the link is unusable for the whole association
body (`E1M-AEN801`: READY never latches, every poll fails), that phantom
debit ran the ledger to zero at roughly a third of the caller's declared
`timeout_ms` — silicon-measured twice at `timeout_ms=15000` giving up at
5.385 s and 5.386 s. The caller then saw `ALP_ERR_TIMEOUT` and reported it as
a radio failure, though the radio's real verdict was never observed. The loop
now bounds itself on real elapsed time instead — an accumulator that only
ever grows by the poll gap actually slept — which cannot exit before
`timeout_ms` of real time has passed.

`examples/aen/aen-cc3501e-socket-throughput`'s `SOCKTP_CONNECT_TIMEOUT_MS`
also raced a healthy association even with correct accounting: the bridge
firmware's own worst case for one connect is up to 30 s for L2 association
plus up to 10 s for DHCP (`hal/ti/cc3501e_hw_ti_wifi.c`), so it moves from
15000 to 40000, with the derivation left as a comment. The same app now
waits out those budgets and reads the independent `cc3501e_wifi_status()`
latch when `cc3501e_wifi_connect()` reports failure, so a bench run can tell
a real radio rejection from a host accounting timeout instead of reporting
neither.

`examples/aen/aen-cc3501e-companion-tour`'s `TOUR_CONNECT_TIMEOUT` (15000)
has the identical too-small ceiling and is not changed here.
`src/zephyr/console/alp_console_companion_wifi.c`'s
`ALP_COMPANION_WIFI_CONN_MS` (50000) and `src/backends/wifi/cc3501e.c`
(passes the caller's own `timeout_ms` straight through) are already correct.
