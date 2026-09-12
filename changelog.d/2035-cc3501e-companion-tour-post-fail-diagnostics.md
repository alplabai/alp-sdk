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
association works), and the `alp_cc3501e_diag_info_t::reserved[0]` byte (the
last Wi-Fi event ID the firmware's callback saw), which the tour's existing
`GET_DIAG_INFO` call already fetched but never printed.

`reserved[0]` carries the **vendor TI SDK's** `WlanEvent_t.Id`
(`hal/ti/cc3501e_hw_ti_wifi.c`: `wifi_cb_last_id = (uint32_t)event->Id;`),
not an alp protocol event opcode — and the two namespaces actually **collide**:
the vendor's `WlanEventId_e` (`wlan_if.h`, `simplelink_wifi_sdk_10_10_01_08`)
assigns genuine ids `1..31`, and three of those — `24`/`25`/`26`
(`0x18`/`0x19`/`0x1A`) — alias `ALP_CC3501E_EVT_WIFI_SCAN_RESULT` /
`_CONNECTED` / `_DISCONNECTED` exactly. Decoding the byte against those
opcodes doesn't just miss; it can land on a real, wrong match, e.g. a
`WLAN_EVENT_FW_CRASH` (`26` = `0x1A`) reading as `EVT_WIFI_DISCONNECTED` — a
radio firmware crash reported as an association that dropped.
`include/alp/protocol/cc3501e.h` documents `reserved[0]` explicitly against
this collision and adds a named `alp_cc3501e_radio_evt_t` enum
(`ALP_CC3501E_RADIO_EVT_CONNECT`/`DISCONNECT`/`SCAN_RESULT`/
`AUTHENTICATION_REJECTED`/`CONNECTING`/`ASSOCIATION_REJECTED`/`ASSOCIATED`/
`EXTENDED_SCAN_RESULT`/`FW_CRASH`/`COMMAND_TIMEOUT`/`ERROR`) mirroring the
subset of the vendor enum a host can usefully act on; an unlisted value is
documented as a legal "some other radio event", not corruption.

A short plain-language verdict follows the three reads, stated only when the
reads themselves succeeded — a failed or contradicted read (e.g. no RSSI
alongside a `CONNECT`/`ASSOCIATED` radio event) is reported as inconclusive
rather than inferred, and a `FW_CRASH`/`COMMAND_TIMEOUT`/`ERROR` radio event
reads as a radio fault rather than an association failure.

No wire behaviour changes and no timeout changes anywhere —
`ALP_CC3501E_PROTOCOL_MINOR` is not bumped; this only reads, documents, and
names what the firmware has always sent.
