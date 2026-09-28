### Fixed

- `provision_som.py`'s SoM-presence gate no longer reports the OPTIGA Trust M (`0x30`) as missing on a healthy module. The Trust M NACKs the first I2C access after idle and ACKs one made right after it; one `i2cdetect -r` scan missed it. An expected-but-silent address is now re-probed as wake-then-scan in one shell command, because the part is asleep again by the next SSH round trip. Bench, E1M-V2M103 2026W38-0001: three runs after idle all clean; the old check refused this unit. The `i2cdetect` parser also reads fixed columns now, so a range-limited scan no longer misplaces cells.
