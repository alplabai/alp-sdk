### Changed — `aen-power-iwic` passes only when the PM hook actually entered deep IWIC sleep (#916)

The example's `RESULT PASS` rested on the core waking every round, but a wake
also follows a plain kernel-idle WFI, so it could pass without the PM policy
ever selecting the suspend-to-idle state. The IWIC entry hook now counts its
entries and the example fails unless it ran once per round
(`examples/aen/aen-power-iwic/src/main.c:176`
("if (aen_iwic_entries < PM_ROUNDS) {")).

Bench, E1M-AEN803 serial 2026W36-0001, M55-HE, Flow C RAM-run:
`RESULT PASS: 8 PM suspend-to-idle (deep IWIC) rounds entered + woken; uptime
0->400 ms (beacon=8, IWIC entries=8)`.
