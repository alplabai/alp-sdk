### Added — bmap support: provisioning writes and verifies only the used blocks of the system image (#2623)

- **Image.** V2N-family `alp-image-*` builds now also emit `<image>.wic.bmap` (`IMAGE_FSTYPES:append:rzv2n-family = " wic.bmap"`). Not yet built.
- **Bundle.** New optional role `system_image_bmap`; `check_som_bundle.py` validates it and compares its `ImageSize` with the gunzipped `system_image`.
- **`write_rootfs`.** With a bmap, the host verifies every mapped range, then one ssh command writes only those ranges; the same ranges are read back and md5-compared, and the probe uses the same check. No `bmaptool` on the board. Without a bmap the full-image path is unchanged.
- **`rootfs_check`.** The read-only mount is `-o ro,noload`, so the ext4 journal is never replayed.
- **bmap parser.** `bmap.parse()` now refuses what it cannot vouch for: a `BmapFileChecksum` that does not match the file (bmaptool's rule: sha256 of the file with the checksum zeroed), a `MappedBlocksCount` that disagrees with the ranges, a `<!DOCTYPE`/`<!ENTITY` declaration, and anything but bmap format 2.x with `ChecksumType` sha256 (no sha1 default, no format-1.x `sha1=` ranges). Checks from #2629.
