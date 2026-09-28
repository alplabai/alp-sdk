### Changed — `chips/gd32g553/gd32g553.c` migrated its frame CRC-16 onto the shared `alp_crc16_ccitt_false()` (#2035)

`<alp/protocol/crc16.h>`'s doc comment claimed both bridge wire protocols
share one CRC-16/CCITT-FALSE implementation, but `chips/gd32g553/gd32g553.c`
still carried its own `static crc16_ccitt_false()` copy -- the exact "two
copies that could silently drift apart" the header was written to prevent.
The two implementations were bit-identical (same poly `0x1021`, same init
`0xFFFF`, non-reflected, no final XOR, same span semantics), so the local
copy is now deleted and every call site in `gd32g553.c` (`spi_xfer()` and
`i2c_xfer()`, request and reply/error-reply CRC checks) uses
`alp_crc16_ccitt_false()` instead. `crc16.h` is header-only and
`stddef`/`stdint`-only, so including it does not add a Zephyr dependency
to `gd32g553.c`, which stays deliberately Zephyr-agnostic. Verified
bit-identical to the removed implementation against the check vector
(`"123456789"` -> `0x29B1`) and several frame shapes matching this file's
real call sites, with a mutation check confirming a wrong init value or
wrong polynomial would have been caught.
