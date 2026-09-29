### Fixed — the CC3501E host driver could return `ALP_OK` for a reply that never came off the wire (#2035)

`ALP_CC3501E_RESP_OK` is `0x00`, and a dead SPI phase clocks back literal
`0x00` for every byte, so a link that stopped shifting mid-transaction was
byte-identical to a successful reply with an all-zero payload. The reply
header echoed the opcode with a legal length, the payload phase read all
zeros, the status byte was `0x00` (`RESP_OK`), and `cc3501e_request_locked()`
(`chips/cc3501e/cc3501e_core.c`) returned `ALP_OK`.

The existing guard only caught this for `ALP_CC3501E_CMD_WIFI_CONNECT_STA`
and `ALP_CC3501E_CMD_WIFI_AP_START`, and only for the bare single-status-byte
shape. Every opcode whose reply carries data beyond the status byte was
unprotected, and `cc3501e_wifi_get_mac()` shipped exactly that shape:
observed on real silicon, it returned `ALP_OK` with a MAC of
`00:00:00:00:00:00`.

**The guard is now structural instead of a two-opcode allowlist.** For a
multi-byte reply (status + data), `cc3501e_reply_may_be_all_zero()` in
`cc3501e_core.c` defaults every opcode to PROTECTED — an all-zero reply is
treated as the dead-phase alias and reported as `ALP_ERR_IO` — unless the
opcode is named on a short, individually-justified exemption list
(`ALP_CC3501E_CMD_WIFI_STATUS`, `WIFI_GET_RSSI`, `WIFI_GET_IP`,
`DIAG_GET_STATS`, `SOCK_RECV`, `OTA_STATUS`, `OTA_UPDATE_MODE`, `GPIO_READ`,
`SPI1_TRANSFER`) whose reply payload can genuinely be all-zero for a real,
documented device state. `GET_VERSION`, `GET_CAPABILITIES` and
`GET_DIAG_INFO` — the other identity-style getters with the same shape —
are protected by the same default. The old bare-status two-opcode check
(`WIFI_CONNECT_STA` / `WIFI_AP_START`) is unchanged; that ambiguity has no
general fix without a protocol version bump (tracked separately, #1696).

**`cc3501e_wifi_get_mac()` (`chips/cc3501e/cc3501e_wifi.c`) now also rejects
an all-zero MAC and one with the IEEE group/multicast bit set** via the new
`cc3501e_mac_is_valid()`, catching a garbled-but-nonzero dead-phase reply the
byte-level guard above cannot see.

**Behaviour change: some calls that previously returned `ALP_OK` with zero
data now return `ALP_ERR_IO`.** A consumer that read a zero MAC, a zero
firmware version, or a zero capability bitmap as a (wrong) successful result
will now see an explicit failure instead — that is the fix, not a
regression. This is a host-side guard against the current firmware's wire
format; it does not touch `ALP_CC3501E_PROTOCOL_VERSION` and needs no bridge
reflash.
