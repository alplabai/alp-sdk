### Added — soc-spec-v1 `cm33_boot.mtd_name`: the V2N xSPI boot partition `xspi_offset` is relative to (tan-cli#1314)

RZ/V2N `n44.json` now records `"mtd_name": "fip"` beside `xspi_offset` (`0x1A0000`), and `soc-spec-v1` accepts the optional key. A tool writing the CM33 image from running Linux resolves `/proc/mtd` by this name, so a project manifest cannot point the offset-scoped erase at a different partition. Consumed by tan-cli's `linux_mtd` flash backend.
