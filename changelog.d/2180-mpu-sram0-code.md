### Added — E8 custom MPU table now supports `zephyr,flash = &sram0` (#2180)

`zephyr/soc-bridge/alif/mpu_regions_e8.c` rejected any build whose code was
linked into SRAM0 (`BUILD_ASSERT`: flash base had to be the ITCM or MRAM
slot-0 base), so SRAM0-linked images could not be built for bench
experiments such as warm re-entry. When `zephyr,flash` resolves to `sram0`
the table now adds an `SRAM0` region (base and size from the `sram0` node,
executable, read-only, cacheable normal memory -- the same attributes as the
`ITCM` entry). Data must then live in DTCM (`zephyr,sram = &dtcm`). The
region is compiled out in every other configuration: the ITCM build's
`zephyr.bin` is byte-identical (`cmp`) to one built from `origin/dev`, and MRAM
builds are untouched.

Verified by building `aen-sdhc-probe` for
`alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he` with `-Werror` in the ITCM
variant and an SRAM0-code variant (code `LOAD` at `0x02000000`). Not yet run
on silicon.

Refs #2180; the warm re-entry measurement is still to do.
