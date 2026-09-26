### Added — fleet-unique Ethernet MAC derived from the SoM serial (#2325)

E1M-V2N / E1M-V2M SoMs have two RAVB Ethernet interfaces (Linux end0/end1)
and no per-unit MAC source: the RZ/V2N has no MAC OTP and the module has no
MAC EEPROM. Left alone, both interfaces booted with whatever compiled-in
default the enabled BSP feature layers happened to bake into U-Boot's
environment — identical on every unit, and not a purchased IEEE OUI either
way — and buying an OUI block is off the table.

U-Boot now derives `ethaddr`/`eth1addr` from the unit serial already in the
validated identity-EEPROM manifest — an injective encoding, not a hash, into
IEEE 802c SLAP locally-administered space — making both MACs fleet-unique by
construction (unique across every serial the allocator has issued), not
globally unique. The derivation runs twice: once in `board_late_init()` (so
U-Boot's own networking has a MAC before `bootcmd`), and again from a new
`alp_eth_mac` command `CONFIG_BOOTCOMMAND` invokes immediately after `env
default -a` — that second call is the one Linux actually sees, since
`image_setup_libfdt()` runs `fdt_fixup_ethernet()` before `ft_system_setup()`.
`CONFIG_BOOTCOMMAND` opens with `env default -a` on every boot, so there is
no env override slot on this board: the derived MAC always applies whenever
the serial parses. See `docs/soms/v2n.md#ethernet-mac-address-policy` for
the full policy, and `scripts/alp_eth_mac.py` for the host-side
implementation the device side stays bit-for-bit identical to.
