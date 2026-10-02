### Added — provisioning reads payloads from the SD it boots, not over the console or SSH (#2624)

- **`prepare-sd`.** New `provision_som.py prepare-sd --bundle DIR --device DEV [--gd32-fw DIR]` (Linux / WSL) writes the release wic to the SD and appends an ext4 partition labelled `alp-payload` holding `<bundle sha256>/`: every bundle component, the GD32 images, `bundle.json` with its signature files, and `manifest.sha256`. Every file is checked against `bundle.json` first.
- **Steps.** `gd32_flash` (console path), `dxm1_npu_flash`, `write_rootfs`, `write_xspi` and `write_emmc_boot` use a stored file only after its hash, computed on the board, matches the signed bundle; a miss or mismatch pushes as before and re-caches. Evidence records `payload_source: sd-store|pushed`. `--no-payload-store` turns it off. Not yet benched.
- **Internal only.** The provisioning SD carries the license-gated DEEPX binaries and must never ship with a unit.
