### Fixed — CC3501E OTA writes: limit each frame to 64 data bytes while the bridge is in update mode (#1818)

Host workaround for a firmware limitation in the update-mode polled SPI slave.
That slave mis-receives any request payload phase longer than 70 bytes: 70 B or
fewer is received intact, 71 B or more is lost (the host sees `-5` with an
all-zero reply header), and 1024 B or more misframes. Splitting one payload
phase across several SS0-framed transfers does not help. Every `OTA_WRITE`
chunk of more than 64 data bytes therefore failed, while the tiny `OTA_BEGIN` /
`OTA_STATUS` payloads worked. Normal (callback SPI) mode is unaffected.

- While the peer is polled, `cc3501e_ota_write()` splits a larger buffer into
  consecutive `OTA_WRITE` frames of at most `CC3501E_POLLED_OTA_CHUNK` (64) data
  bytes, advancing the offset per frame (4 offset + 64 data + 2 CRC = 70 B, the
  largest frame on the proven side of the boundary). Callers that pass 508 or
  1024 B chunks keep working unchanged. Outside update mode a write is still one
  frame.
- `cc3501e_ota_update()` clamps its own chunk to one polled-safe frame, so a
  BUSY/IO hold-off retry re-sends exactly the chunk that was refused.
- The `cc3501e_host_ota` fake slave now records frame count and size; new cases
  pin a 1024 B write as 16 frames with the image landing in order, a 130 B write
  as 64 + 64 + 2, and normal mode as a single frame.
