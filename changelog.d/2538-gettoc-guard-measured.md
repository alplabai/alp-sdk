### Tests — the ATOC `gettoc` completeness guard is now backed by a measured truncation (#2538)

A stalled SE-UART read was measured on an E1M-AEN803 (serial 2026W36-0009,
`SES A1 v1.110.0`) to make `maintenance -opt gettoc` exit 0 with a table that
has no closing `+---+` line, so the closing-line rule in
`scripts/aen_atoc.py` (and its bash twin `bench_atoc_replace_guard`) is the
primary defence against a stalled read. Fixtures for 7 rows, header only and
8 rows without the closing line (all exit 0), plus a complete table with a
`readSerial reporting disconnected` error (exit 1), now pin it. The factory
`MCUBOOT-` name (step 2) stays open.
