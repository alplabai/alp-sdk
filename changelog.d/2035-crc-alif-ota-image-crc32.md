### Added — GD32 bridge OTA image CRC32 now hardware-accelerated where available, with a software fallback (#2035)

`gd32g553_ota_image_crc32()` (`<alp/chips/gd32g553.h>`) computes the OTA
wire protocol's `expected_crc32` / `computed_crc32` (docs/gd32-bridge-protocol.md
§10, "CRC-32 is IEEE 802.3 reflected (zlib-compatible)") the way the wire
contract already expects it, without changing that contract or any wire
byte. On a build that instantiates the Alif Ensemble E8 hardware CRC
engine (`zephyr/drivers/crc/crc_alif.c`, ADR 0017 Tier-1.5, already landed
in #210/#1856) and has it `device_is_ready()`, it computes the CRC on
silicon; otherwise -- the accelerator absent from the build, not enabled
in the active devicetree, PD-6 unpowered (HWRM Table 15-26: CRC0/CRC1 are
not available in STANDBY/STOP, and this polled driver has no PD-6 status
read), or an image length the engine's 32-bit word path cannot consume
whole (HWRM 15.2.5.3.6) -- it falls back to a portable software CRC32.
Both paths are proven bit-identical against the CRC-32/ISO-HDLC check
value (`0xCBF43926` for `"123456789"`) and a longer buffer cross-checked
against `zlib.crc32`; the hardware register-level configuration (BYTE_SWAP
+ BIT_SWAP on input, REFLECT_CRC + INVERT_CRC on output, per HWRM
Table 15.2.5.3.1) was additionally verified against an independent
software model derived only from the manual's register semantics, with a
mutation check confirming the model is sensitive to both swap bits.
OTA image verification therefore never hard-depends on the accelerator
existing or being powered -- see `src/zephyr/gd32g553_ota_crc_zephyr.c`
for the split (kept out of `chips/gd32g553/gd32g553.c`, which stays
Zephyr-agnostic).
