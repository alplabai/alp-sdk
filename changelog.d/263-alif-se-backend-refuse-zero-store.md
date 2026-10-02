### Fixed — Alif SE boot-metadata provider: verify flag and zero store address (#263)

The silicon fixes described for the Alif SE provider in #2564 reached the
pure helper (`ulog_alif_se_build_entry()` now takes the TOC entry's
`flags_string` verify character) but not the backend that calls it.
`src/backends/update_log/alif_se_boot_metadata.c` still passed the numeric
`flags` word as that character, and still hashed `[store_address,
store_address + image_size)` even though SES v1.110 reports `store_address`
`0x00000000` for every TOC entry.

The backend now passes `resp_flags_string[ULOG_ALIF_TOC_FLAG_STRING_VERIFY_IDX]`
and returns `ALP_ERR_NOSUPPORT` for a zero store address.

Bench, E1M-AEN803 serial 2026W36-0009, M55-HE RAM-run with
`CONFIG_ALP_SDK_UPDATE_LOG_BOOT_ALIF_SE=y`, three reads per build:
- Before: `HE_APP` (flags `0x00000063`, flags_string `uLVB`) read back as
  `ALP_UPDATE_STATUS_VERIFY_FAILED`. `A32_APP` (size `0x71100`) took a
  precise BUS FAULT at BFAR `0x40000`, because hashing `[0, 0x71100)` runs
  off the end of the HE ITCM.
- After: both return `ALP_ERR_NOSUPPORT`, with no fault.
