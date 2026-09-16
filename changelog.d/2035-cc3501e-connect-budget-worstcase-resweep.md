### Fixed — the CC3501E bench apps budgeted a connect against a worst case that no longer exists (#2035)

Four apps derived their connect budget from the bridge firmware's own bound,
quoted as "30 s L2 association + 10 s DHCP = 40 s". Both halves are now wrong,
and a third term was never counted at all:

- the DHCP lease poll is **20 s**, not 10 s — `CC3501E_STA_DHCP_TRIES` went from
  50 to 100 so the poll covers lwIP's fourth DISCOVER at t=14 s instead of
  stopping four seconds short of it;
- a connect issued as the **first radio op of a boot** carries `Wlan_Start`, a
  `Wlan_Set` and a 10 s `Wlan_RoleUp` *inside* the connect body, because the
  image never calls `cc3501e_hw_wifi_boot_start()`.

So the real bound is about **60 s**, and `55000u` no longer clears it. By these
apps' own stated rule, that budget was buying failures the radio never suffered:
the call returns `-4` with the association still in progress, and nothing about
the output says so.

This is not hypothetical. Two bench sessions in this campaign were hard to
compare because one ran at the tour's 15000u default and one at an overridden
70000u, and the fractions moved for reasons that had nothing to do with the
firmware.

Re-derived: `SOCKTP_CONNECT_TIMEOUT_MS` and `CONNTWICE_CONNECT_TIMEOUT_MS`
55000u → 75000u, and `WEDGEPM_QUIET_WAIT_MS` likewise — that last one is
load-bearing, since a silent wait that no longer exceeds the firmware's bound
would let a merely-slow board be reported as wedged, the exact false conclusion
that app exists to refuse.

The companion tour keeps its short default, which is correct for touring, but now
says plainly that a **measurement** run must override to 70000u and why.

Historical bench prose describing the original v0.8.0 image's 10 s gate is kept
and labelled as history rather than rewritten, with what changed since.
