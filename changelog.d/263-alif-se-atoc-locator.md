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

Bench, E1M-AEN803 serial 2026W36-0009, SES v1.110, M55-HE RAM-run, three
reads per image: `HE_APP`, `HP_APP` and `A32_APP` each report
`ALP_UPDATE_STATUS_CONFIRMED`, with version 0.1.0 / 1.0.0 / 0.1.0 and SHA-256
digests `a38da2c7…`, `1020e3e3…` and `a0a083b1…`. Each digest is identical
across reads and equal to the hash in that entry's signed content certificate
(cross-checked against a full-MRAM dump). Hashing the 0x71100-byte `A32_APP`
takes about 18 ms at 160 MHz. The call chain needs about 1 KiB of stack.
One run with a 1 KiB main stack overflowed inside `sha256_transform`, so the
Kconfig help now asks for at least 2 KiB.
