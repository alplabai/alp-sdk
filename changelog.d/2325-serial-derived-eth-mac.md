### Added — fleet-unique Ethernet MAC derived from the SoM serial (#2325)

E1M-V2N / E1M-V2M SoMs have two RAVB Ethernet interfaces (Linux end0/end1)
and no per-unit MAC source: the RZ/V2N has no MAC OTP and the module has no
MAC EEPROM. Left alone, both interfaces booted with whatever compiled-in
default the enabled BSP feature layers happened to bake into U-Boot's
environment — identical on every unit, and not a purchased IEEE OUI either
way — and buying an OUI block is off the table.

U-Boot now derives `ethaddr`/`eth1addr` at boot from the unit serial already
in the validated identity-EEPROM manifest — an injective encoding, not a
hash, into IEEE 802c SLAP locally-administered space — making both MACs
fleet-unique by construction (unique across every serial the allocator has
issued), not globally unique. A real user/production override, or the known
compiled-in vendor default, is respected; only a genuinely unset value is
derived. See `docs/soms/v2n.md#ethernet-mac-address-policy` for the policy
and override instructions, and `scripts/alp_eth_mac.py` for the host-side
implementation the device side stays bit-for-bit identical to.
