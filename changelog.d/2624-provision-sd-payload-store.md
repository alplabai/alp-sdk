### Added — provisioning reads payloads from the SD it boots, not over the console or SSH (#2624)

- **`prepare-sd`.** New `provision_som.py prepare-sd --bundle DIR --device DEV [--gd32-fw DIR]` (Linux / WSL) writes the release wic to the SD and appends an ext4 partition labelled `alp-payload` holding `<bundle sha256>/`: every bundle component, the GD32 images, `bundle.json` with its signature files, and `manifest.sha256`. Every file is checked against `bundle.json` first.
- **Steps.** `gd32_flash` (console path), `dxm1_npu_flash`, `write_rootfs`, `write_xspi` and `write_emmc_boot` use a stored file only after its hash, computed on the board, matches the signed bundle; a miss or mismatch pushes as before and re-caches. Evidence records `payload_source: sd-store|pushed`. `--no-payload-store` turns it off. Not yet benched.
- **Internal only.** The provisioning SD carries the license-gated DEEPX binaries and must never ship with a unit.
- **Read-only store.** The store is mounted `ro,noatime`, remounted rw only while a file is cached, and unmounted (best effort) before every power cycle or reboot the tool drives.
- **`--create-payload-store`.** With no `alp-payload` partition on the boot SD, `run --execute` can append one from the board (`sfdisk`, `partx`, `mke2fs`; MBR only, never the eMMC); a missing tool, GPT or no room falls back to the push with a `payload_store_note`. Not yet benched.
- **`prepare-sd` safety.** A block device that is not removable is refused unless `--i-know-this-is-the-sd <size in bytes>` repeats its size.
- **Console and bmap hardening.** A flash write over the console is never Ctrl-C-ed or re-sent; an rc=127 is re-sent only when the command did not run; `bmap` refuses an image whose unmapped blocks are not zero; a non-block-aligned image is padded the same way on the host-stream and store paths.
- **Boot ROM `Address Error!!!`.** `load_writer` now names the cause (the SoC is probably not booting on the CA55) instead of timing out.
