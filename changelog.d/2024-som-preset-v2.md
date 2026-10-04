### Changed (BREAKING for out-of-tree presets) — som-preset schema is now v2: `write_authority` required, `npu_population` removed (#2024)

`metadata/schemas/som-preset-v1.schema.json` is renamed `som-preset-v2.schema.json` and
`schema_version` is pinned to 2 (`metadata/schemas/som-preset-v2.schema.json:56` ("schema_version")).
There is no v1 compatibility path: a preset still declaring `schema_version: 1` fails validation.

`memory_region` now lists `write_authority` in `required`
(`metadata/schemas/som-preset-v2.schema.json:622` ("write_authority")), so a `memory_map:` row
that omits it is rejected by the schema itself, per ADR-0034 clause 4 (absence is a validation
failure, never a permissive default). The semantic gate `_check_som_write_authority_present` in
`scripts/validate_metadata.py` existed only to cover the v1 gap and is deleted with its test.
`inference.npu_population` and the `npu_instance` definition are deleted; Ethos-U instances are
derived from the SoC JSON `npus[]`.

Regions the loader derives when a preset declares no `memory_map:` now state their authority
explicitly (`scripts/alp_project_loader.py:504` ("def resolve_memory_map(")): SRAM/TCM and SoC-level
RAM regions are `customer_runtime`, the derived `mram_main` whole-device alias is `composite`. The
`rpmsg-v2n` and `hetero-offload` system-manifest emit snapshots gain that field.

**Migrating an out-of-tree preset:** set `schema_version: 2`, add `write_authority` to every
`memory_map:` row (values in the schema's `write_authority` description), and delete any
`inference.npu_population` block.
