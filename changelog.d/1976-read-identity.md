### Added — `eeprom_24c128_read_identity()` reads the N24S128's second I2C device-select header (#1976)

The on-module 24C128 EEPROM (Onsemi N24S128) answers at both `0x50` (the 16 KB
array) and `0x50 + 0x08` — one part, a second device-select header exposing a
64-byte Secure Data Page, a 16-byte factory Unique ID, a Lock Status byte, and
the Device Configuration Register. `eeprom_24c128_read_identity()` reads all
four in one call into a new `eeprom_24c128_identity_t`, with a per-field
`_valid` flag rather than an all-or-nothing error: the approved
footprint-compatible second source (STMicro M24128-BFMH6TG, DNP) has no such
header, so on that part every read NACKs and the function still returns
`ALP_OK` with every `_valid` false.

Read-only by design — a stray write at the Device Configuration Register
selector would move the EEPROM off its strapped I2C address and can
permanently write-protect the array, so this change adds no write/lock path.
Bench-verified 2026-09-06 on an E1M-AEN803 (serial 2026W36-0003) over SoC
I2C2 — the shipped function itself was executed on silicon, not just linked.
It returned `ALP_OK` with all four objects answering at `0x58`: the Secure
Data Page erased (64 bytes of `0xFF`), a stable 16-byte Unique ID, Lock Status
reporting unlocked, and the Device Configuration Register reading the
datasheet delivery state `0x1D`. `0x58` was confirmed to be a distinct address
space rather than an alias of the manifest in the array at `0x50`, and the
three documented error paths each returned their documented status on the same
run.
