### Fixed — CC3501E OTA: update-mode writes no longer fail on the first chunk, and the swap is requested in the same boot (#2728)

`cc3501e_ota_update()` failed on its very first `OTA_WRITE` against bridge
firmware v0.9.2 with RESP_ERR_PROTOCOL and a reply one exchange late. The
update-mode bridge reads each request payload byte by byte from its 32-byte
SPI RX FIFO in a polled loop, and the host clocks that phase at full wire
speed, so a 70-byte frame (64 data bytes) overran the FIFO and failed its
CRC. Update-mode writes now carry 16 data bytes per frame (22 bytes on the
wire), inside the FIFO. On E1M-AEN803 2026W36-0009 the full 1,110,536-byte
image then streamed and was accepted by FINISH in about 80 s.

`cc3501e_ota_update()` also now issues PROMOTE right after FINISH. FINISH
only stages the image; the swap request the TI PSA-FWU layer needs is held in
CC3501E RAM, so any reboot between FINISH and PROMOTE left the image staged
but impossible to promote (`OTA_STATUS reserved[0]` = 119, i.e. `(int8_t)` of
`PSA_ERROR_BAD_STATE`). With PROMOTE in the same boot, the swap-reboot fires
(link drop and return in about 3.4 s).

Still open: on that bench unit the bootloader then reported no swap and the
slot was marked rejected, so the CC3501E kept its existing image. Updating
the CC3501E firmware over XDS110 is unaffected.
