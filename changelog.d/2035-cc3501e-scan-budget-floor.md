### Fixed — a short `cc3501e_wifi_scan()` budget no longer manufactures a link failure (#2035)

`cc3501e_wifi_get_mac()` floors its poll budget at
`CC3501E_WIFI_DOWN_WINDOW_MS` so a short caller timeout cannot give up
while the radio is still coming up. `cc3501e_wifi_scan()` had no such
floor, and it needs a larger one: a scan issued as the first Wi-Fi
operation of a boot pays the STA role-up first, so the firmware's own
bounded worst case is `CC3501E_WIFI_ROLE_TIMEOUT_MS` = 10 s for the
`Wlan_RoleUp` plus a 6 s wait on the scan result, 16 s in total.

A 15 s caller budget produced `WIFI_SCAN_START rc=-4 elapsed_ms=15062` on
silicon — 1062 ms **inside** the firmware's own bound — and every opcode
after it in that sweep was tagged as post-wedge.

**Be careful what that proves**, because this fix was first written on a
reading of it that later evidence weakened. It was read as the caller's
clock running out. It is equally consistent with the link having been down
for the whole radio op: a polled scan issued as the *first* radio operation
of a boot does not recover, while the same scan returns records when it is
not first — 6 records in one run, and 5 across four earlier cold-booted
runs.

So this floor is **not** claimed to fix that failure. It does the narrower,
defensible thing: a caller budget below the firmware's own bound cannot
express a healthy outcome at all, so a timeout at that budget tells you
nothing about the radio. Removing an uninformative failure mode is worth
doing on its own.

Floored at `CC3501E_WIFI_SCAN_WINDOW_MS` = 20 s: the 16 s bound plus
margin for the reply round trip and host scheduling, the same shape the
socket-throughput app's 55 s connect budget uses over its own 40 s worst
case. The parameter's documentation in `<alp/chips/cc3501e/wifi.h>` now
states the floor and why it exists, so a caller passing a smaller value
knows what happens to it.
