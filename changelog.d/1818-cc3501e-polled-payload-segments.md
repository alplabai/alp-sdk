### Fixed — CC3501E OTA writes: send request payloads in <= 64 B segments while the bridge is in update mode (#1818)

In OTA update mode the CC3501E slave runs a polled/blocking SPI path that
mis-receives any request payload phase longer than 70 bytes: 70 B or fewer is
received intact, 71 B or more is lost (the host sees `-5` with an all-zero reply
header), and 1024 B or more misframes outright. Every `OTA_WRITE` chunk (a 256 B
page plus header and CRC, 262 B on the wire) therefore failed, while the tiny
`OTA_BEGIN` / `OTA_STATUS` payloads worked. Normal (callback SPI) mode is
unaffected and takes 4092 B in one transfer.

- When the peer is polled, `cc3501e_request_locked()` now sends the request
  payload phase as consecutive SS0-framed transfers of at most
  `CC3501E_POLLED_PAYLOAD_SEG` bytes (default 64, below the 70/71 boundary), with a
  `CC3501E_POLLED_SEG_SETTLE_US` gap (default 50 us) between them. The slave counts
  bytes per phase and ignores SS0 deasserts between transfers. The CRC still
  covers the whole payload, and the normal-mode path is a single transfer with no
  added delay, exactly as before.
- Both knobs are `#ifndef`-guarded in `chips/cc3501e/cc3501e_internal.h`.
- The `cc3501e_host_driver` and `cc3501e_host_ota` fake-bridge models now
  reassemble segmented payloads, and new cases pin the split arithmetic and a
  polled, segmented `CONNECT`.
