### Fixed — companion tour decoded the CC3501E diag byte against the wrong opcode namespace, making its verdict ladder unreachable (#2035)

`alp_cc3501e_diag_info_t::reserved[0]` carries the **vendor TI SDK's**
`WlanEvent_t.Id` (the bridge firmware sets it in
`hal/ti/cc3501e_hw_ti_wifi.c` with `wifi_cb_last_id = (uint32_t)event->Id;`),
not an alp protocol event opcode. `aen-cc3501e-companion-tour`'s post-fail
diagnostics (added earlier under this same issue) decoded that byte against
`ALP_CC3501E_EVT_WIFI_SCAN_RESULT` / `_CONNECTED` / `_DISCONNECTED` — the alp
async-event-ring opcodes, `0x18`/`0x19`/`0x1A`, a completely different
namespace from the vendor's `1`/`2`/`3`. The two never overlap, so every real
value fell through to `default:`, the verdict ladder's three informative
branches were unreachable, and the tour always reported "mixed signals ...
inconclusive" even when the diag byte held clear evidence. Bench-measured on
`e1m-aen-evk-01`: a post-fail diag byte of `0x01`
(`WLAN_EVENT_CONNECT`) printed as "other/non-Wi-Fi", turning the strongest
available evidence — the radio reporting it connected — into no evidence.

`include/alp/protocol/cc3501e.h` now documents `reserved[0]` explicitly as
the bridge radio's own vendor event id, NOT an `ALP_CC3501E_EVT_*` opcode,
and adds a named `alp_cc3501e_radio_evt_t` enum
(`ALP_CC3501E_RADIO_EVT_CONNECT`/`DISCONNECT`/`SCAN_RESULT`/
`AUTHENTICATION_REJECTED`/`CONNECTING`/`ASSOCIATION_REJECTED`/`ASSOCIATED`/
`EXTENDED_SCAN_RESULT`) mirroring the subset of the vendor's `WlanEvent_t`
enum a host can usefully act on; an unlisted value is documented as a legal
"some other radio event", not corruption. The companion tour now decodes
against these constants and its verdict ladder is repaired to match: a
`CONNECT`/`ASSOCIATED` event alongside a valid RSSI now reads as "the radio
associated and no lease arrived: an L3/DHCP failure", the conclusion the
bench evidence actually supports, and `AUTHENTICATION_REJECTED` /
`ASSOCIATION_REJECTED` now read as a genuine L2 failure. A leg whose read
itself failed still yields inconclusive, unchanged.

No wire behaviour changes — `ALP_CC3501E_PROTOCOL_MINOR` is not bumped; this
only documents and names what the firmware has always sent.
