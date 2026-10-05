### Changed — U-Boot picks the kernel dtb from the on-module EEPROM manifest at boot (#2691)

The FIP's U-Boot used to load `boot/$CONFIG_ALP_E1M_FDTFILE`, a name fixed at
build time per MACHINE. A blank E1M-V2N103 bootstrapped with a V2M FIP then
looked for `e1m-v2m101-x-evk.dtb`, which the V2N image does not contain, and
could not boot its own wic. New U-Boot patch
`0013-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch` adds an `alp_fdtfile`
command, run by `CONFIG_BOOTCOMMAND` right after `env default -a`, that sets
`fdtfile` from the validated manifest family: `v2n-m1` ->
`e1m-v2m101-x-evk.dtb`, `v2n` -> `e1m-v2n101-x-evk.dtb`. One U-Boot binary now
boots the right dtb on whichever SoM it runs on.

The choice fails closed. A unit with a valid manifest and a known family loads
only its own family's dtb (`fdtfile_alt` stays empty); if the image lacks it
the boot prints "dtb for family <f> (<dtb>) missing from image -- refusing to
boot another SoM's device tree" and stops at the prompt, never falling through
to the other family's dtb. Only a blank unit (no valid manifest or an unknown
family; provisioning's `boot_sd_linux` runs before `eeprom_manifest`, so first
boot is always blank) prints one `ALP: fdtfile ...` line, keeps the
`CONFIG_ALP_E1M_FDTFILE` default (`fdtfile-v2m.cfg` sets it for V2M MACHINEs),
and falls through `boot/${fdtfile}` then `boot/${fdtfile_alt}` (the other dtb
in the one family table; no name is tried twice), so it boots whichever dtb its
image ships (each image holds only its own MACHINE's dtb). A failed load never
boots a stale `0x48000000`. The `env_set` results are checked.

Built and inspected only (patch regenerated with `git format-patch` on the
pinned renesas-u-boot-cip `bcf29d98` plus 0001..0011, applies with and without
0003, compiled for `rzv2n-dev`); not yet booted on
silicon.
