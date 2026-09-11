### Fixed — `aen-cc3501e-socket-throughput`'s connect budget no longer equals the firmware's exact worst case (#2035)

`SOCKTP_CONNECT_TIMEOUT_MS` was `40000u`, derived in the comment beside it
as the bridge firmware's own worst case for one station connect: 30 s of
L2 association plus `CC3501E_STA_DHCP_TRIES * CC3501E_STA_DHCP_POLL_US`
= 50 * 200 ms = 10 s of DHCP. Setting the caller's budget to exactly that
sum leaves zero margin — a healthy association that happens to take the
full firmware budget, plus any host-side scheduling or round-trip cost on
top of it, expires the caller's timer first and is reported as a connect
failure. The comment already said a budget *below* 40 s races a healthy
association; equal to it races just as surely, only by a smaller amount.

Raised to `55000u`, keeping the derivation in the comment and adding the
margin the derivation implies.
