### Added — Alif SE update_log boot metadata locates the image through the ATOC package (#263)

`src/backends/update_log/alif_se_boot_metadata.c` no longer refuses when
GET_TOC_INFO reports `store_address` 0 (SES v1.110 does this for every
entry). The new pure helper `ulog_alif_atoc_locate()` in
`src/update_log/boot_providers.c` walks the ATOC package in MRAM (trailer,
`OEMTOC01` header, entry, CryptoCell-312 certificate chain) to find the
image's address and length plus the signed SHA-256 from its content
certificate. The backend hashes the located bytes and reports
`VERIFY_FAILED` if they differ from the signed hash.

- The certificate format follows TF-M's public CryptoCell-312 runtime; the
  package framing was measured on SES v1.110 (E1M-AEN803 serial
  2026W36-0009), not taken from a vendor spec, and self-checks against the
  signed hash (all 5 entries of that module matched).
- Any unexpected layout, encrypted image or out-of-range pointer returns
  `ALP_ERR_NOSUPPORT` rather than a fabricated entry.
- Native_sim unit tests cover the located entries and each refusal path.
