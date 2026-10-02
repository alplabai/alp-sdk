### Added — bmap support: provisioning writes and verifies only the used blocks of the system image (#2623)

- **Image.** V2N-family `alp-image-*` builds now also emit `<image>.wic.bmap` (`IMAGE_FSTYPES:append:rzv2n-family = " wic.bmap"`). Not yet built.
- **Bundle.** New optional role `system_image_bmap`; `check_som_bundle.py` validates it and compares its `ImageSize` with the gunzipped `system_image`.
- **`write_rootfs`.** With a bmap, the host verifies every mapped range, then one ssh command writes only those ranges; the same ranges are read back and md5-compared, and the probe uses the same check. No `bmaptool` on the board. Without a bmap the full-image path is unchanged.
- **`rootfs_check`.** The read-only mount is `-o ro,noload`, so the ext4 journal is never replayed.
