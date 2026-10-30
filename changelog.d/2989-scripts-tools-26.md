### Fixed — provision: verify legacy bundle components, never ledger pass without an EEPROM write (#2989)

The legacy (non-V2N) provision flow flashed bundle_dir/<file> after only a schema check, so a truncated, swapped or escaping component reached the flash backend. It now checks each component stays inside the bundle and matches size_bytes and sha256 before anything is flashed. The same flow never programs the EEPROM, yet recorded ledger result pass after a passing smoke test. A passing test now records pending-hw; fail is unchanged.
