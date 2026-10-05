### Fixed — CC3501E bridge link recovers in-band after a missed reply, and soft-AP start no longer races the next request (#2699)

On bridge firmware v0.9.0, one host transfer that clocks before the bridge
has armed its phase leaves the bridge's replies one whole transfer late, and
every later command fails until a warm reset (which drops the soft-AP and
every socket). Before any warm reset, the host's auto-recovery probe now
drives two of the firmware's own heals in order: a request header declaring
a payload that never comes, which trips the bridge's 250 ms stall watchdog,
then three `0xFF` resync headers. On E1M-AEN803 2026W36-0009 this restored
the link 100 of 100 PINGs from the lagged state, at a cost of about 0.5 s
instead of a 3.5 s reset.

`cc3501e_wifi_ap_start()` also waits 300 ms after the AP role confirms.
v0.9.0 publishes the role before its post-AP_START SPI re-open, so a request
fired at once failed 3 to 5 of 20 times with rc=-4. With the wait it failed
0 of 20.
