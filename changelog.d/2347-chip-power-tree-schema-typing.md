### Changed — Chip manifest items and the power-tree `chips` block are now schema-typed (#2347)

`chip-v1.schema.json` types `rails[]`, `channels[]` and `register_table[]` items
(required keys, no unknown keys, `write` limited to `allow` / `guarded` /
`deny`), and `power-tree-v1.schema.json` types the `chips` block, with the
DA9292 `dev_id` / `rev_id` / `cfg_rev` identity bytes limited to 0..255.
`gen_power_tree.py` now also fails when the power tree's ACT88760
`addr_add1` / `addr_add2` differ from the `add1` / `add2` `addr_7bit` rows in
`metadata/chips/act8760.yaml`.
