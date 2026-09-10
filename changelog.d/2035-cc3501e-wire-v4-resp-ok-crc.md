### Changed — CC3501E wire protocol bumped to MAJOR 4.0: `RESP_OK` moved off `0x00`, every frame gained a CRC (#2035)

`ALP_CC3501E_RESP_OK` was `0x00` -- the same literal byte a dead SPI phase
clocks back for every byte it touches, so a link that stopped shifting
mid-reply was byte-identical to a genuine, successful zero-payload reply
(#1378). A prior change on this same issue closed the alias for the reply
that could actually be observed on the bench (`cc3501e_wifi_get_mac()`
returning `00:00:00:00:00:00` as a "successful" read); this change is the
structural fix that prior commit named as a separate, firmware-paired
follow-up.

`ALP_CC3501E_RESP_OK` is now `0x5A`, chosen by Hamming distance from every
value a stuck link can forge (distance 4 from both `0x00` and `0xFF`,
distance 8 from `ALP_CC3501E_SYNC_IDLE` `0xA5`, and none of its eight
single-bit neighbours is an assigned status code -- see
`include/alp/protocol/cc3501e.h`). Every frame, both directions, now carries
a CRC-16/CCITT-FALSE trailer (the same algorithm and construction the
gd32-bridge protocol already uses, now shared via the new
`include/alp/protocol/crc16.h`) -- placed at the last 2 bytes of the padded
reply payload, so a reply whose unpadded length already left room in its
existing DMA-alignment padding carries the CRC for **zero extra wire bytes**;
requests grow by 2.

**This is a coordinated hardware change and needs a matching firmware
release.** The host driver is deliberately BILINGUAL through the migration:
`cc3501e_reset()`'s version gate now accepts wire MAJOR 3 (the previous,
CRC-less wire) as well as this driver's own MAJOR 4, and
`cc3501e_request_locked()` picks the legacy (no-CRC, `0x00`-is-OK) or the 4.0
(CRC-required, `0x5A`-is-OK) decode per-peer off the negotiated
`fw_proto_major` -- so a 4.0 host still talks correctly to a board still
running 3.1 firmware. **Migration order matters:** roll the host first (it
depends only on MCUboot + the ATOC, not the coprocessor), then OTA every
coprocessor to 4.0 *through* that already-bilingual host; only a later
release may drop the MAJOR-3 legacy decode, and only once the whole fleet
confirms it is on 4.0. A board still on 3.1 firmware needs that OTA before a
host that has already dropped the legacy branch will talk to it at all.

Two live aliasing bugs, both caused by the same `0x00`-is-both-OK-and-dead
byte, are closed as part of the same change: `cc3501e_reset()`'s own
`GET_VERSION` compat probe (a dead payload phase could not silently be
misread as legacy MAJOR 0), and the OTA flush hold-off in
`cc3501e_ota.c` (a dead `OTA_STATUS` phase could not abort a healthy session
mid-flush) -- both now covered by the CRC on a 4.0 wire, with the pre-existing
guard kept, unchanged, for the legacy MAJOR-3 branch.

The CRC stays software on both sides: neither this SDK's Alif Ensemble
support nor the CC3501E bridge firmware plumbs a CRC peripheral today, the
cost is noise at the reply sizes involved, and computing a shared-peripheral
CRC inside the firmware's SPI-callback (ISR) context risks a silently wrong
result if ever done without care -- see `include/alp/protocol/crc16.h` for
the reasoning and the upgrade path if a future measurement justifies
hardware offload.
