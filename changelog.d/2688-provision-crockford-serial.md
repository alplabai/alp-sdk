### Fixed — the provisioning serial gate accepts Crockford base32 label indexes (#2688)

`scripts/provision/gates.py` `parse_serial()` accepted only an all-digit
index, so a printed label past `0009` (for example `2026W38-000K` on an
E1M-V2N103) was refused before any step ran. The 4-character index is now
decoded with the same Crockford base32 alphabet `alp_eth_mac` and
`program_eeprom` already use (`0123456789ABCDEFGHJKMNPQRSTVWXYZ`, no
I/L/O/U): `000K` = 19, `000M` = 20, and all-digit serials keep their
value. `I`, `L`, `O`, `U` and lowercase letters stay rejected.
