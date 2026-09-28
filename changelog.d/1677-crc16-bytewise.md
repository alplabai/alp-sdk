### Changed — CC3501E bridge: the shared CRC-16 is byte-wise, +22% STREAM_WRITE throughput

`alp_crc16_ccitt_false_update()` (`include/alp/protocol/crc16.h`) ran an
8-step bit loop per byte. The CC3501E bridge CRCs every request once the
link negotiates wire v4, and bulk frames reach 4 KiB, so on the M55-HE that
loop cost about 1.3 ms per 4092-byte `STREAM_WRITE` frame. It now folds a
byte at a time with the standard shift form of the 0x1021 polynomial:
identical output, no lookup table.

Measured on an E1M-AEN803 (2026W36-0009), `aen-cc3501e-socket-throughput`
bridge sweep, D-cache on and `CONFIG_SPI_DW_ALIF_PACK32=y`: 4092-byte frames
went from 561 KB/s to 687 KB/s, 2 of 2 runs identical.

A new ztest (`test_crc16_bytewise_matches_bitwise`) compares the helper
against the bit loop over every 97th length up to 4096 bytes, from
non-default starting registers and across a header/payload split, next to
the existing `123456789` -> `0x29B1` check vector. The GD32 bridge uses the
same helper and gets the same output.
