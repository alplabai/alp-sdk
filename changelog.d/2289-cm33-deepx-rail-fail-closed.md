### Changed

- `examples/v2n/v2n-cm33-deepx-rail` now fails closed unless `CONFIG_V2N_CM33_BOOT_CONFIRMED=y` (#2289): `power-tree.yaml` `boot_modes.cm33_boot` stays `blocked`, so the example refuses to master RIIC8 by default; unsourced GPIO5/GPIO7 claims and stale `boot_mode_core`/`handover` references are removed, and the CA55 release register bases (non-secure aliases) are marked unverified pending the manual cross-check.
