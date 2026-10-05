### Added — TAS2563 verified tuning load and ROM/tuning mode readback (#2580)

`tas2563_load_tuning_verified()` zeroes `I2C_CKSUM` (B0/P0/0x7E), replays a tuning stream, reads the register back and compares it with the PPC3-supplied `PChkSum`, the same scheme Linux and TI's vendor driver use. The device algorithm is undocumented, so the expected byte comes from the caller; a mismatch returns `ALP_ERR_IO` (caller retries) with the readback reported. `tas2563_read_tuning_mode()` reports ROM vs tuning mode from B0/P1/0x02 (read-only; the register is not in SLASET3D and is sourced from TI E2E and PPC3 program blocks, unverified).

### Fixed — tuning streams could reset the I2C checksum mid-load (#2580)

`tas2563_load_tuning()` now refuses a record that writes B0/P0/0x7E. The TAS2563 header also no longer claims SLASET3D never mentions ROM mode (it does, PCM playback table, p.11).

Bench check outstanding: whether the driver's explicit PAGE/BOOK writes are counted by the device's checksum the same way PPC3's are.
