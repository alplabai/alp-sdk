### Added — AEN E8 OSPI exposes a fail-closed flash API and reads the E1M-AEN803 NOR JEDEC ID (#915)

The Alif OSPI/HexSPI driver now registers a valid Zephyr `flash_driver_api`
instead of a null device API. Its `read_jedec_id` operation sends the standard
`0x9f` command to E1M-AEN803 OSPI0 CS1 and expects the fitted ISSI IS25WX256
identity (`9d 5b 19`) captured earlier by a raw-register probe. The transfer is
serialized, checks every hal_alif setup result, reports FIFO loss, and recovers
the controller and chip select after a failed normal deassert.

Addressed read, program, and erase remain deliberately unsupported until their
device sequences are proven on silicon. Their mandatory callbacks return
`-ENOTSUP`; leaving them null would turn an ordinary Zephyr flash API call into
a null-function dereference. `aen-ospi-regcheck` now builds for both SKUs:
E1M-AEN801 keeps its controller-only proof because it fits no OSPI memory, while
E1M-AEN803 selects CS1 at the capture-proven 20 MHz rate and requires the exact
three-byte JEDEC response. The new Zephyr API path is ready for its own silicon
run; this change does not claim that bench result in advance.
