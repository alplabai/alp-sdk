### Changed — U-Boot picks the kernel dtb from the on-module EEPROM manifest at boot

The FIP's U-Boot used to load `boot/$CONFIG_ALP_E1M_FDTFILE`, a name fixed at
build time per MACHINE. A blank E1M-V2N103 bootstrapped with a V2M FIP then
looked for `e1m-v2m101-x-evk.dtb`, which the V2N image does not contain, and
could not boot its own wic. New U-Boot patch
`0013-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch` adds an `alp_fdtfile`
command, run by `CONFIG_BOOTCOMMAND` right after `env default -a`, that sets
`fdtfile` from the validated manifest family: `v2n-m1` ->
`e1m-v2m101-x-evk.dtb`, `v2n` -> `e1m-v2n101-x-evk.dtb`. One U-Boot binary now
boots the right dtb on whichever SoM it runs on.

A missing or invalid manifest, or an unknown family, keeps the
`CONFIG_ALP_E1M_FDTFILE` default (`fdtfile-v2m.cfg` still sets it for V2M
MACHINEs) and prints one `ALP: fdtfile ...` line. If the derived dtb is absent
from the image the load falls back to that default; a failure of that last
attempt still stops at the prompt rather than booting a stale `0x48000000`.

Built and inspected only (patch series applied in order on the pinned
renesas-u-boot-cip `bcf29d98`, compiled for `rzv2n-dev`); not yet booted on
silicon.
