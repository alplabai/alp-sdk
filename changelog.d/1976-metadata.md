### Fixed — `0x58` on the SoM EEPROM bus is documented instead of unexplained (#1976)

An I2C census of SoC I2C2 sees an ACK at `0x58` that no metadata claimed, which
is an address a future part could be assigned to and collide with
(alp-sdk#1976). It is the **same physical N24S128** as the `0x50` array
answering its second device-select header (`1010` → `0x50`, `1011` → `0x58`) —
one part, nothing extra to source.

All **eleven** SoM presets that populate `eeprom_24c128` now record it, not just
the one it was measured on: the E1M-AEN line is a single shared PCB differing
only in SoC MPN, and the onsemi `N24S128C4DYT3G` is equally the fitted default on
E1M-V2N/V2M, so the collision hazard was identical on every SKU. The measurement
provenance stays singular and honest — bench-measured on `E1M-AEN803` serial
`2026W36-0003`, cited as one measurement rather than eleven.
`examples/aen/aen-eeprom-manifest` demonstrates reading it through the new
`eeprom_24c128_read_identity()`, and the bench/test/verification docs no longer
present their device census as a closed account that omits it.
