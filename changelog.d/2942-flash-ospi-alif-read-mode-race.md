### Fixed — flash_ospi_alif: read() and the JEDEC/SFDP reads race with the Octal DDR switch (#2942)

ospi_alif_read() tested octal_ddr_active without data->lock and then sent
1-1-1 READ4 chunks one lock at a time, so a concurrent write()/erase()
switching the part to Octal DDR mid-read made the remaining chunks go out as
1-1-1 to an Octal DDR part (garbage data, rc 0). read() now holds the lock
across the mode check and every chunk. read_jedec_id() and sfdp_read(), which
only speak 1-1-1, check the mode under the lock too and return -ENOTSUP once
the part is in Octal DDR instead of returning garbage.
