### Fixed — `aen-cc3501e-command-sweep` asked for a scan budget that could not succeed (#2035)

The sweep called `cc3501e_wifi_scan()` with a 15 s budget. The firmware's
own bounded worst case for a scan issued as the first Wi-Fi operation of a
boot is 16 s — a 10 s STA role-up, then a 6 s wait on the scan result — so
that budget could not express a healthy outcome.

It did exactly what that implies on silicon: `WIFI_SCAN_START rc=-4
elapsed_ms=15062`, 1062 ms **inside** the firmware's own bound. The sweep's
consecutive-failure heuristic then declared a link wedge and tagged every
later opcode `[post-wedge]`, which is a lot of downstream evidence resting
on a clock that ran out early.

`cc3501e_wifi_scan()` now floors any budget at 20 s, so this call was
already being corrected silently. Raised here to 25 s anyway, so the literal
in the source means what it says rather than relying on the floor, with the
reasoning recorded beside it.
