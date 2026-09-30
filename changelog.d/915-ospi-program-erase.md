### Added — the Alif OSPI/HexSPI `flash_driver_api` gains `write`/`erase` over an Octal DDR mode switch (#915)

`zephyr/drivers/flash/flash_ospi_alif.c`'s `write`/`erase` were deliberate
`-ENOTSUP` stubs (see `changelog.d/915-ospi-flash-api.md`): bench attempts
showed `WREN` (`06h`) never sets `WEL` on the fitted ISSI IS25WX256 in 1-1-1
SPI, and a follow-up erase+program on a verified-blank sector left it
unchanged. The Alif DFP's own IS25WX256 driver never programs in 1-1-1
either — it first switches the part to Octal DDR (`81h` write volatile
config, IO mode `0xE7`) and only then uses `84h` program / `21h` erase /
`70h` flag-status polling.

This change follows that same sequence. `write()`/`erase()` lazily switch
the part — and this driver's own controller framing — to Octal DDR on first
use (`ospi_alif_ensure_octal_ddr()` / `ospi_alif_octal_switch_locked()`),
then drive `84h` page-program (256 B pages, split on page boundaries) and
`21h` sector-erase (4 KiB, matching the SFDP-measured erase type 1 and
`IS25WX256.h`'s `FLASH_ISSI_SECTOR_SIZE`) through the DW-SSI "FRF-defined"
enhanced-SPI mode (`SPI_CTRLR0.SPI_FRF` = Octal, `OSPI_SPI_CTRLR0`'s
`INST_L`/`ADDR_L`/`WAIT_CYCLES`/`DDR_EN` fields), polling `70h` flag status
to a bounded completion after each op. Every register field used comes from
hal_alif's own named constants (`ospi.h`/`ospi_hal.h`), no offset/bitfield
open-coded, matching the existing read path's discipline. On any timeout or
error the controller is forced idle (`ospi_alif_recover_transfer()`), same
fail-closed behavior as the existing read helper.

Octal DDR data frames are 16 bits wide (2 bytes/frame, matching the vendor
driver's own `uint16_t`-typed buffers in that mode); `write()`, `erase()`'s
alignment check, and the new octal `read()` path all require even lengths
and reject odd ones with `-EINVAL` rather than inventing a padding byte.
`get_parameters()`'s `write_block_size` is now `2` (was `1`) to reflect that
real granularity.

**Read after the switch:** once Octal DDR is active the part no longer
answers 1-1-1 SPI at all. `read()` now checks `data->octal_ddr_active` and,
once set, follows the part into Octal DDR (opcode `7Ch`, the vendor driver's
`CMD_READ_DATA`) instead of trying to switch the part back to 1-1-1 per
call — a per-call switch-back would need its own `81h` write every time and
a mid-switch failure would leave `read()` unable to tell which framing the
part is actually in. `read_jedec_id()`/`sfdp_read()` are unchanged and stay
1-1-1-only diagnostic reads; nothing in this driver calls them after a
write/erase.

`page_layout` (`CONFIG_FLASH_PAGE_LAYOUT`) is deliberately not implemented:
a `flash_pages_layout` callback also needs the part's total array size,
which this driver does not hardcode from the one bench-known IS25WX256 (the
vendored DT binding carries no `size` property to add without diverging
from it) — a real `page_layout` needs SFDP BFPT density parsing at init,
out of this issue's scope.

`examples/aen/aen-ospi-regcheck` gains an opt-in, off-by-default
`AEN_OSPI_ENABLE_PROGRAM_ERASE_SELFTEST` self-test that erases, verifies
blank, programs a known pattern, verifies the readback, and restores the
original contents (CRC-verified) of exactly one 4 KiB sector
(`0x01FF0000`), printing its own `RESULT PASS/FAIL` line. Off by default so
flashing this app cannot destructively touch the NOR by accident.

Two defects surfaced on the bench and are fixed here. The octal array
read and the part's wait-cycle config were fed the DT `xip-wait-cycles`
value -- the controller's XiP knob, `255` on this SoC -- which overflowed
the 5-bit `SPI_CTRLR0.WAIT_CYCLES` field and wrote `0xFF` wait cycles into
the part, so every array read returned `0xFF` and a successful program
looked like a no-op; the driver now uses its own 16-cycle constant, the
DFP's `RTE_ISSI_FLASH_WAIT_CYCLES` for this SoC. And octal reads were
issued as one long transfer, which overflows the 256-entry RX FIFO at
Octal DDR rates and stalls (`-ETIMEDOUT`); they are now chunked at the
FIFO depth, as the DFP does.

**Verified on silicon:** E1M-AEN803 serial 2026W36-0001, M55-HE Flow C
RAM-run of `aen-ospi-regcheck` with the self-test enabled, two runs (one
cold, one warm with the part already in Octal DDR): `post-erase all_0xff=1`,
pattern `crc32=0xa2912082` read back byte-for-byte, a second erase + the
restore brought the sector back to its original `crc32=0xf154670a`,
`RESULT PASS`. Octal DDR `70h`/`05h` reads return real status
(`WEL` set after `06h`), and an octal `5Ah` SFDP read with 8 dummy cycles
returns the `SFDP` signature.
