### Added — Real MCUboot and Alif SE providers for the update-log boot-metadata seam (#263)

PR #416 landed `alp_update_log_entry_from_boot_metadata()` /
`alp_update_log_append_boot()` and the internal provider seam
(`src/update_log/boot_metadata.h:7` ("Internal provider seam for authenticated boot metadata"),
but the default provider always returned
`ALP_ERR_NOSUPPORT` -- no platform had a real producer yet. This adds the
two agreed producers:

- `src/backends/update_log/mcuboot_boot_metadata.c` -- reads the booted
  image's version and SHA-256 straight from MCUboot's own image header
  and `IMAGE_TLV_SHA256` trailer TLV via Zephyr's public `flash_area` API
  on the active slot (`boot_fetch_active_slot()`), gated on
  `CONFIG_ALP_SDK_UPDATE_LOG_BOOT_MCUBOOT`. Not `blinfo_lookup()`: Zephyr
  v4.4's blinfo only exposes generic bootloader facts
  (mode/slot/bootloader version -- `include/zephyr/retention/blinfo.h`
  "for MCUboot: minor TLV" is scoped to `TLV_MAJOR_BLINFO` only), and
  MCUboot's measured-boot shared-data TLV carries a CBOR-encoded
  attestation report rather than a plain version/hash pair.
- `src/backends/update_log/alif_se_boot_metadata.c` -- scans the Alif
  ATOC (`se_service_get_toc_number()` + a `SERVICE_SYSTEM_MGMT_GET_TOC_INFO`
  packet over the public `se_service_send_request()` generic-send seam,
  the same pattern `src/backends/security/se_cryptocell.c:393`
  ("return se_service_send_request((uint32_t *)packet, (uint32_t)size);")
  and `src/backends/ext/alif/storage.c:189`
  ("int rc = se_service_send_request((uint32_t *)&pkt, (uint32_t)sizeof(pkt));")
  already use) for the TOC entry
  matching `CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE_IMAGE_ID`, then
  SHA-256s that entry's own `[store_address, store_address + image_size)`
  MRAM range -- the ATOC has no per-entry digest field of its own. The
  entry's verify bit maps to `ALP_UPDATE_STATUS_CONFIRMED` /
  `ALP_UPDATE_STATUS_VERIFY_FAILED`. Gated on
  `CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE`.
- `src/update_log/boot_providers.{h,c}` -- the pure, hardware-independent
  TLV-parser and identity/verify-policy helpers behind both providers,
  with no Zephyr/vendor include, so they build and are unit-tested on
  native_sim without any bootloader or SE present
  (`tests/unit/update_log/src/test_update_log.c` gains cases for a good
  SHA-256 TLV, a bad magic, a truncated entry, a no-match TLV, a good/bad
  MCUboot header magic, and the Alif SE verify-bit / image-id-mismatch /
  invalid-hash policy paths).
- A new mutually-exclusive `choice` in
  `zephyr/kconfigs/gpu2d-update-log.kconfig` selects at most one provider
  (`NONE` default, `MCUBOOT`, `ALIF_SE`) so only one strong definition of
  `alp_update_log_boot_metadata_read()` can ever link into one image.

Deterministic bad-data policy throughout, per the issue: a short read, a
bad magic, a missing/truncated TLV, an image-id mismatch, or an
implausible `image_size` all report `ALP_ERR_NOSUPPORT` -- neither
provider ever appends a fabricated or zero-filled entry.

Verified: `tests/unit/update_log` passes on `native_sim/native/64`
(twister, 87/87 cases). Both providers build clean with
`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y` -- the Alif SE provider on the
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` AEN target, the MCUboot
provider on `nrf52840dk/nrf52840` (no in-tree AEN board wires
`CONFIG_BOOTLOADER_MCUBOOT` yet).

Bench, E1M-AEN803 serial 2026W36-0001 and 2026W36-0009, SES v1.110, Alif SE
provider driven from a throwaway M55-HE RAM-run. Three defects found and
fixed before this landed:

- The SE reports verification as a **character** in the TOC entry's
  `flags_string` (`FLAG_STRING_VERIFY` is an index into it, and a verified
  entry reads `V`), not as bit 2 of the numeric `flags` word. A verified
  entry (`flags=0x00000063`, `flags_string` `uLVB`) had come back
  `VERIFY_FAILED`.
- `version` is packed `major<<24 | minor<<16 | patch` (the way SETOOLS
  prints it), so `0x01000000` is now `1.0.0`, not `16777216`.
- `GET_TOC_INFO` returns `store_address` `0x00000000` for **every**
  entry on this SES, including `A32_APP` and `BOOTLOAD`, which SETOOLS'
  own `gettoc` places at `0x80020000` and `0x80002000`. Hashing
  `[0, image_size)` hashed live TCM, and two runs gave two different
  digests. The provider now refuses with `ALP_ERR_NOSUPPORT` whenever the
  SE reports a zero store address.

Consequence: on current silicon, the Alif SE provider always returns
`ALP_ERR_NOSUPPORT` (measured for `ALP-HE`, `HP_APP` and `A32_APP`), so it
never fabricates an entry. A working digest needs the image's location
from the ATOC package itself rather than from this SE service. That is
tracked on #263, which stays open. The MCUboot provider is untested on
silicon: no AEN board wires `CONFIG_BOOTLOADER_MCUBOOT` yet.
