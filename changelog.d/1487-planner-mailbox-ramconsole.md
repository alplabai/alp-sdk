### Fixed — planner no longer aliases mailbox channel 0 for unreserved `ipc:` entries or crashes on a leading-zero RAM console size (alplabai/tan-cli#1487)

An `ipc:` entry not named after a `mailbox.channels[].reserved_for` tag used to fall back to mailbox channel 0, the channel reserved for `alp_default_rpmsg`, so two links silently shared one doorbell. `resolve_carve_outs()` now gives such an entry the lowest unclaimed `reserved_for: app` channel and, when none is left, marks the entry `blocked` with a `no mailbox channel` reason naming the entry and the SoM's reservations. Channel 0 is only ever handed to an entry named for its reservation.

`CONFIG_RAM_CONSOLE_BUFFER_SIZE=016384` in an app `prj.conf` raised an uncaught `ValueError` from `int(x, 0)`. It is now read the way Kconfig reads an int symbol: `0x` prefix is hex, anything else is decimal, so `016384` is 16384.
