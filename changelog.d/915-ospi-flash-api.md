### Added — the Alif OSPI/HexSPI driver registers a real `flash_driver_api` (#915)

`zephyr/drivers/flash/flash_ospi_alif.c` previously registered its
`ospi0`/`ospi1` device with a `NULL` API pointer, so no Zephyr flash call
could reach the controller at all — its init proved only that
`alif_hal_ospi_initialize()` compiles, links, and completes on the
controller-register side. It now implements `read`, `read_jedec_id`,
`sfdp_read` and `get_parameters` over a new command-then-read helper in the
DW-SSI EEPROM-read transfer mode (`TMOD = 0b11`), 1-1-1 standard SPI: the
opcode/address goes out from the TX FIFO with no slave selected, `SER`
starts the transfer, and RX is drained as it arrives. `alif_hal_ospi_transfer()`
cannot do this itself — it programs receive-only mode and relies on the
`SPI_CTRLR0` instruction phase, which only the enhanced (dual/quad/octal)
frame formats have; in standard format a receive-only transfer never shifts
the opcode out and the part never answers. Reads over the 65536-frame
(`CTRLR1` `NDF`) limit are split into multiple commands. A failed or
stalled transfer forces the controller idle (disable, mask interrupts,
deassert CS) before returning, so a timeout cannot leave the shared bus
wedged for a later caller; a per-device mutex serializes concurrent
`flash_driver_api` calls onto the one controller.

`write` and `erase` are deliberate fail-closed `-ENOTSUP` stubs, not gaps —
leaving Zephyr's mandatory `flash_driver_api` callbacks `NULL` would fault
the caller instead. Bench attempts recorded against this issue show `WREN`
(`06h`) never sets `WEL` on the fitted ISSI IS25WX256 in 1-1-1 SPI (`RDSR`
reads `0x00` before and after, from both TX-only and EEPROM-read mode), and
a follow-up erase+program on a verified-blank sector left it unchanged. The
Alif DFP's own IS25WX256 driver never programs in 1-1-1 either — it first
switches the part to Octal DDR (`81h` write volatile config, IO mode
`0xE7`) and only then uses `84h` program / `21h` erase / `70h` flag-status
polling, and its own comment says the status register only reads correctly
after that switch. Implementing program/erase means driving that mode
switch and the enhanced octal SPI frame format through hal_alif — a
distinct, silicon-gated follow-up this pass does not fake by guessing at an
untested command sequence.

`CONFIG_OSPI_ALIF` now `select`s `FLASH_HAS_DRIVER_ENABLED` and
`FLASH_JESD216` (an app opts into `CONFIG_FLASH_JESD216_API` itself to reach
`read_jedec_id`/`sfdp_read`; `examples/aen/aen-ospi-regcheck` does not enable
it yet and makes no device-level call).

Unchanged: no XiP path. `alif_hal_ospi_xip_enable()` still targets the
`OSPI_XIP_SER` register, absent on AE822 (`SOC_FEAT_OSPI_HAS_XIP_SER 0`),
and this driver's new `flash_driver_api` never calls it either — see the
file-header FOURTH section for the full citation chain. Regenerated the
E1M-AEN803 board `.dts` comments and `scripts/gen_zephyr_board.py`'s shared
`_AEN_OSPI_XIP_GAP` string, which both used to say "ships no
`flash_driver_api` at all"; that clause is no longer true, so they now say
the API "never calls it" instead, with the reasoning unchanged.

`examples/aen/aen-ospi-regcheck`'s board overlays now set `cs-pin = <1>` and
`bus-speed = <20000000>` on both the E1M-AEN803 and E1M-AEN801 targets (the
dtsi default is CS0 @ 100 MHz — CS0 is the HyperRAM footprint, not the NOR,
and 100 MHz is the rate issue #915's own bench capture measured returning
garbage). Any future `flash_driver_api` call on this node inherits the
correct chip select and rate from the overlay rather than the unverified
dtsi default.

Note: `fix/915-ospi-flash-api` (PR #2223, open) already carries this same
driver plus a regcheck rewrite that exercises the API end to end on
E1M-AEN803 silicon; that PR predates recent `dev` by hundreds of files and
was not used as a base here. This fragment's own regcheck app still makes no
device-level transfer — see the `CONFIG_FLASH_JESD216_API` paragraph above.

Not run here: a bench build/flash/read cycle on real E1M-AEN803 silicon —
this pass is code + a native build verification only (`west build` for
both `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he` and
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`), not a bench run. The
read path's command framing matches what issue #915's own bench thread
already captured working on silicon (JEDEC ID `9d 5b 19`, SFDP signature
`53 46 44 50`, `flash_read()` returning `0xFF` off the erased array), but a
maintainer with bench access should re-run `aen-ospi-regcheck` on
E1M-AEN803 before treating this pass as bench-proven itself. Program/erase
remain open and need the Octal-DDR mode-switch work above, which is a
silicon-gated follow-up, not implementable blind.

Bench-verified on E1M-AEN803 2026W36-0001 (Flow C RAM-run on the M55-HE, a
throwaway probe calling the flash API on the `snps,designware-ospi` device with
this branch's `aen-ospi-regcheck` AEN803 overlay):
`flash_read_jedec_id` -> `9d 5b 19` (ISSI), `flash_sfdp_read` -> signature
`SFDP`, revision 1.9, 3 parameter headers, `flash_read` at offset 0 -> `0xff`
(erased), and `flash_write` -> `-ENOTSUP` as designed.
