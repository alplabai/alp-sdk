### Added — `check_chip_reg_parity.py` ties act8760/da9292/tps628640 register tables to their manifests (#2347)

`chips/act8760/act8760.c`'s `rail_table[]` and `raw_write_allow[]`
(`chips/act8760/act8760.c:131` ("static const uint8_t raw_write_allow[]")),
`chips/da9292/da9292.c`'s per-channel register `#define`s and
`DA9292_RAW_WRITE_FIRST`/`_LAST` window, and `chips/tps628640/tps628640.c`'s
`TPS628640_REG_*` channel registers restate the same addresses as their
`metadata/chips/<part>.yaml` manifests, hand-copied for readability. Nothing
compared the two: a digit slip in either table, or a manifest `write:`
flip between `allow`/`guarded`/`deny`, built and tested clean with no error.

The new gate, `scripts/check_chip_reg_parity.py:4` ("Cross-check a chip"),
hand-parses each driver's raw-write allow surface and per-rail/per-channel
register `#define`s or macro-computed offsets and diffs them against the
manifest, wired into `.github/workflows/pr-metadata-validate.yml:537` ("check_chip_manifest_parity.py") and `metadata/quality-tasks-v1.json`'s `chip-reg-parity` task.

Not done here: `chip-v1.schema.json` typing `rails[]`/`channels[]`/
`register_table[]` items (also proposed in #2347) touches all 88 chip
manifests and belongs in its own PR.
