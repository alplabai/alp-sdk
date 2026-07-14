# WS6-c Phase 1, Batch B (iot/audio) — canonical library manifests

Tokens: `coremqtt_sn, tinygsm, libwebsockets, bearssl, minimp3, opus, libhelix`

## Files created

- `metadata/libraries/coremqtt-sn.yaml` — MIT, v1.0.1, Tier B, `CONFIG_ALP_MQTTSN_NO_TLS=y`
- `metadata/libraries/tinygsm.yaml` — LGPL-3.0, v0.11.7, Tier B, `CONFIG_ALP_TINYGSM_SYNC_IO=y`
- `metadata/libraries/libwebsockets.yaml` — MIT, v4.3.4, Tier B, `CONFIG_ALP_LWS_NO_TLS=y`
- `metadata/libraries/bearssl.yaml` — MIT, `master` (unpinned SHA per west.yml TBD), Tier B, `CONFIG_ALP_BEARSSL_PURE_C=y`
- `metadata/libraries/minimp3.yaml` — CC0-1.0, `master` (vendored 2026-07-10, `vendors/minimp3/`), Tier B, `CONFIG_ALP_MINIMP3_PURE_C=y`
- `metadata/libraries/opus.yaml` — BSD-3-Clause, v1.5.2, Tier B, `CONFIG_ALP_OPUS_PURE_C=y`
- `metadata/libraries/libhelix.yaml` — RPSL-1.0 (RealNetworks RCSL/RPSL), exact SHA `0a0e0673f82bc6804e5a3ddb15fb6efdcde747cd`, Tier B, `CONFIG_ALP_LIBHELIX_PURE_C=y`

All 7 mirror `metadata/libraries/nanopb.yaml`'s shape (`schema_version`,
`name`, `description`, `tier`, `version`, `license`,
`integration.zephyr.{module,kconfig}`); none carry `requires:` (no
`depends on CPU_CORTEX_M`-style Kconfig constraint or yocto section
exists for any of the seven — inventing one would not be grounded).
Each header comment cites the `_LIBRARY_KCONFIG` source line
(`scripts/alp_project_emit/__init__.py:250-264`), points at
`metadata/library-profiles/<token>/hw-backends.yaml` for the
Phase-3-deferred HW-backend fold, and the west.yml `extras-tier1`
pin it was grounded against.

## Validation result

Ran `jsonschema` (Draft 2020-12, matches schema's own `$schema`) against
`metadata/schemas/library-v1.schema.json` for all 7 files:

- **PASS**: `coremqtt-sn.yaml`, `libwebsockets.yaml`, `bearssl.yaml`, `opus.yaml` (4/7)
- **FAIL** (all on the `license` enum, nothing else): `tinygsm.yaml`,
  `minimp3.yaml`, `libhelix.yaml` (3/7)

Also ran `scripts/check_library_registry.py`: none of this batch's 7
aliases appear in its unresolved-alias output anymore (the remaining
reported gaps — `gfx_compat`, `littlefs`, `madgwick_ahrs`, `mbedtls`,
`tflite_micro`, `u8g2` — belong to a different batch, out of this task's
scope). Script still exits 1 overall because of those other tokens, which
is expected and outside scope here.

## Concerns (license-enum failures — genuine, not authoring errors)

The schema's `license` field only accepts
`Apache-2.0/MIT/BSD-2-Clause/BSD-3-Clause/Zlib/MIT-0` (ADR 0018's
permissive allowlist). Three of the seven tokens have real upstream
licences outside that set; I recorded the true licence rather than
mis-stating a passing one:

1. **tinygsm — LGPL-3.0** (copyleft; the exact "surprise" class this
   allowlist exists to catch). Needs a maintainer/legal call: either
   confirm a different licence grant, or a deliberate decision to keep
   `tinygsm` unlisted/gated until re-licensed use is cleared.
2. **minimp3 — CC0-1.0**, confirmed verbatim from the vendored
   `vendors/minimp3/LICENSE` file (public domain — *more* permissive than
   every allowlist entry, not less; a false-negative from the enum's
   narrow list rather than a real risk).
3. **libhelix — RPSL-1.0** (RealNetworks Public Source License), grounded
   in `west.yml`'s own comment and `examples/audio/libhelix-decode/
   README.md`'s explicit "Helix is RealNetworks RCSL/RPSL — source-available,
   not Apache-2.0" callout. Genuinely non-permissive/source-available;
   already flagged in-repo as "review before shipping in a product."

Per `library-v1.schema.json`'s own `license` field description,
"extending this allowlist is a deliberate human decision (legal review),
not a metadata edit" — I did not fabricate a passing licence value for
any of the three to force a green validation. These 3 files are left in
place with the honest data and an in-file `CONCERN` comment block each;
they will fail `check_library_registry.py`'s eventual per-file schema
validation step (Phase 1 item 3, not yet wired to call jsonschema per
file) until that human call is made.

STATUS: 4/7 fully schema-valid; 3/7 authored with grounded, honest data
that fails only the `license` enum by design — flagged for legal/maintainer
follow-up, not papered over.
