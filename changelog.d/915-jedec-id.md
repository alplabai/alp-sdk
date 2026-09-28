### Added — AEN E8 OSPI flash API reads the E1M-AEN803 NOR: JEDEC ID, SFDP and 4-byte-address reads (#915)

The Alif OSPI/HexSPI driver now registers a valid Zephyr `flash_driver_api`
instead of a null device API. Its `read_jedec_id` operation sends the standard
`0x9f` command to E1M-AEN803 OSPI0 CS1 and expects the fitted ISSI IS25WX256
identity (`9d 5b 19`) captured earlier by a raw-register probe. The read uses
the DW-SSI EEPROM-read transfer mode in standard single-lane SPI: the opcode
goes out from the TX FIFO and three frames are clocked back. hal_alif's
`alif_hal_ospi_transfer()` could not do this. It programs receive-only mode
and relies on an instruction phase that only the dual/quad/octal frame formats
have, so in standard format the opcode never left the controller. The first
silicon run of that path timed out (`-ETIMEDOUT`, all-zero ID, 3 of 3). The
read is serialized and bounded, and it always deasserts chip select and forces
the controller idle afterwards.

The same command-then-read path also serves `sfdp_read` (JESD216 `5Ah`,
24-bit address, one dummy byte) and `read`. `read` uses the 1-1-1 READ with a
4-byte address (`13h`), which the part advertises in its SFDP 4-byte address
instruction table (DWORD1 `43 0e ff ff`, bit 0 set, read on silicon). Reads
longer than one 65536-frame transfer are split. Program and erase remain
deliberately unsupported until their device sequences are proven on silicon. Their mandatory callbacks return
`-ENOTSUP`; leaving them null would turn an ordinary Zephyr flash API call into
a null-function dereference. `aen-ospi-regcheck` now builds for both SKUs:
E1M-AEN801 keeps its controller-only proof because it fits no OSPI memory, while
E1M-AEN803 selects CS1 at the capture-proven 20 MHz rate and requires the exact
three-byte JEDEC response. Its CTRLR0 check now asks only that the register
file is live: CTRLR0 keeps whatever the previous image programmed, and
`alif_hal_ospi_initialize()` does not rewrite it. It read `0x00C00407` after
a cold boot, `0x01c0080f` after a resident image had used OSPI, and
`0x00000c07` after an earlier run of this app, so "equals the POR value" failed
on every warm run.

Bench-verified on E1M-AEN803 2026W36-0009 (Flow C RAM-run, M55-HE):
`flash_read_jedec_id() rc=0 id=9d 5b 19` and `RESULT PASS`, 3 of 3 warm runs
plus 1 from a cold boot. The later build with SFDP and read also passed,
warm and cold:
- SFDP signature `53 46 44 50`, revision 1.9, four parameter headers:
  BFPT, `0xFF84`, `0xFF05` and `0xFF0A`.
- `flash_read()` of 4 KiB at `0x00000000` and at `0x01000000` completes and
  repeats byte-identically.

The array is erased, so those reads return `0xFF`, which an idle bus would
also give. A known-pattern read has to wait for program support.
