### Fixed — `cc3501e_wifi_get_ip()` could not tell "no address yet" from a broken transport (#2035)

`cc3501e_wifi_get_ip()` returned `ALP_ERR_IO` both when the transport
genuinely failed and when the firmware simply had no address to report yet
— its `WIFI_GET_IP` handler answers `RESP_ERR_RADIO` for all three "no
address" conditions (network stack not up, address lookup failed, or a
genuine 0.0.0.0 lease), having no dedicated not-ready status of its own on
this opcode. The two cases were indistinguishable at the call site, and
that ambiguity is what drove a multi-week bench investigation down the
wrong path: the radio had actually associated (an RSSI read of -75 dBm
immediately after the "failed" connect, corroborated by the scan's
identical reading), yet thirty one-second `get_ip` polls all came back
`ALP_ERR_IO` and were read as "the instrument did not read" rather than
"there is no address" — because the code could not say which.

The host CAN tell the two apart: `cc3501e_request()` poisons
`ctx->rx_scratch[0]` to `ALP_CC3501E_RX_SCRATCH_NO_STATUS` on every
pre-decode exit, so a real `ALP_CC3501E_RESP_ERR_*` value there means a
status byte was genuinely decoded, never leftover wire residue. When the
status is `ALP_ERR_IO` and that byte is `ALP_CC3501E_RESP_ERR_RADIO`,
`cc3501e_wifi_get_ip()` now returns `ALP_ERR_NOT_READY` instead — "poll
again" — and leaves every other failure, including a short reply, as
`ALP_ERR_IO`. The connect path's own ambiguity is unchanged: it has two
firmware-side writers of the same latch and cannot be fixed from the host.
