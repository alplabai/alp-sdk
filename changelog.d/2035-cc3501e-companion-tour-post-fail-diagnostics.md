### Changed — companion tour reads RSSI/IP/diag-event on a failed CC3501E connect, to tell the bridge's two `FAIL_TIMEOUT` writers apart (#2035)

`aen-cc3501e-companion-tour`'s `WIFI_CONNECT` step returned an `ALP_ERR_TIMEOUT`
that could not be diagnosed from the host: the bridge firmware has **two**
writers of `ALP_CC3501E_WIFI_FAIL_TIMEOUT` and both publish the identical
latch — the 30 s association wait expiring (a genuine L2 failure), and a
10 s DHCP-lease gate that runs *after* a successful `WLAN_EVENT_CONNECT` when
the interface still has no address (verified present in the shipped v0.8.0
image: two `FAIL_TIMEOUT` sites, the second immediately after a
50-iteration, 200 ms DHCP poll). A bench connect returned `-4` at
14.45-16.87 s against a 70 s budget — a bracket that fits ~5 s of
association plus the fixed 10 s DHCP poll, not the 30 s path.

The failed-connect branch now adds three read-only, non-behaviour-changing
checks after that status is printed: `cc3501e_wifi_rssi()` (a plausible dBm
means the radio associated at L2; unavailable means it never did — read
after a 1 s wait, past the RSSI read's own after-associate block window and
past the 10 s DHCP window), `cc3501e_wifi_get_ip()` polled once a second for
~30 s (a late-arriving lease is the single most informative outcome — it
means the 10 s DHCP gate is too short at this link budget and the
association works), and the `alp_cc3501e_diag_info_t::reserved[0]` byte
(the last Wi-Fi event ID the firmware's callback saw), which the tour's
existing `GET_DIAG_INFO` call already fetched but never printed. A short
plain-language verdict follows, stated only when the reads themselves
succeeded — a failed read is reported as inconclusive rather than inferred.

No timeout, no success-path behaviour, and no other file changed.
