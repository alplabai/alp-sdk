### Fixed — CC3501E bridge link recovers in-band after a missed reply, and soft-AP start no longer races the next request (#2699)

On bridge firmware v0.9.0, one host transfer that clocks before the bridge
has armed its phase leaves the bridge's replies one whole transfer late, and
every later command fails until a warm reset (which drops the soft-AP and
every socket). Before any warm reset, the host's auto-recovery probe now
tries the firmware's own heals in order. First, three `0xFF` resync headers
on their own: this restores the dead state (60 of 60 PINGs on E1M-AEN803
2026W36-0009, fw 0x0900). Only if the PING still fails, a request header
declaring a payload that never comes (tripping the bridge's 250 ms stall
watchdog) followed by the burst: this restores the lag state (100 of 100).
A last burst follows if that still fails. The cost is about 0.15 s for the
burst and about 0.6 s for the chain, plus the probe PINGs, instead of a 3.5 s
reset. This order was not tested end to end inside the driver on v0.9.0. On
fw v0.9.2 the resync never had to fire: 10 of 10 induced misses recovered
without it.

`cc3501e_wifi_ap_start()` also waits 300 ms after the AP role confirms.
v0.9.0 publishes the role before its post-AP_START SPI re-open, so a request
fired at once failed 3 to 5 of 20 times with rc=-4. With the wait it failed
0 of 20.
