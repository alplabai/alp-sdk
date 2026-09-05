### Fixed — five AEN SoM presets still declared the OSPI0 NOR/HyperRAM chip-selects inverted, and the schema let a stray chip-select value through silently (#1990)

Auditing the `ospi_memories:`/`hyperram:` scaffold question for #1944 turned
up a real hardware-fact bug: `metadata/e1m_modules/E1M-AEN301.yaml`,
`E1M-AEN401.yaml`, `E1M-AEN501.yaml`, `E1M-AEN601.yaml` and
`E1M-AEN701.yaml` all still had `ospi_memories.ospi0.chip_select: 0` (NOR)
and `hyperram.chip_select: 1` (HyperRAM) — exactly the inverted mapping
that `E1M-AEN801.yaml` (and `E1M-AEN803.yaml`) already document as
corrected: the shared E1M-AEN-2626-R2 netlist routes U9 (the RAM footprint)
to `OSPI0_SS0` (CS0) and U10 (the NOR footprint) to `OSPI0_SS1` (CS1), and
per the family's own `metadata/e1m_modules/aen/CHANGELOG.md` invariant
("all AENs have the same revision, only SoC changes") every AEN SKU ships
one shared module PCB, so the corrected mapping applies family-wide, not
only to the SKU where it was first caught. Fixed to `ospi0.chip_select: 1`
/ `hyperram.chip_select: 0` on all five, with the same corrected-mapping
comment `E1M-AEN801.yaml` carries. `chip_select` on these blocks has no
generator consumer today (`scripts/alp_orchestrate/*` reads
`ospi_memories`/`hyperram` for capacity and key presence, not
`chip_select`), so this is a metadata correctness fix with no generated-file
fallout, not a behaviour change.

The same audit found the five presets' `hyperram:` blocks omitted
`assembled:` entirely — the exact key `som-preset-v1.schema.json`'s own
description calls "load-bearing" (defaults `true`, and E1M-AEN801 is on
record for advertising 256 Mbit of external RAM it does not populate
because of this same omission). Here the omission happened to resolve to
the right answer — each file's own `memory.dram_mbit` derivation comment
already states the HyperRAM is "populated on every AEN BOM variant" — but
leaving it implicit meant the fix was one accidental edit away from
repeating E1M-AEN801's bug. Made explicit (`assembled: true`) on all five,
citing that same in-file source.

**A stray value would have passed every gate anyway.** `chip_select` was
`{"type": "integer", "minimum": 0}` with no upper bound, on a bus that only
ever wires two chip-select lines (`OSPI0_SS0`/`OSPI0_SS1` — no shipped
preset, including `E1M-AEN803.yaml`'s 2-CS OSPI0 controller, ever declares
a third). Setting `chip_select: 9` on `E1M-AEN803.yaml` and re-running
`validate_metadata.py` left every check green. Added
`"maximum": 1` to both `$defs/ospi_memory.chip_select` and
`$defs/hyperram.chip_select` in `metadata/schemas/som-preset-v1.schema.json`,
and `tests/scripts/test_ospi_chip_select_bound.py` to pin the boundary (0/1
accepted, 2/9 rejected) so the next typo fails schema validation instead of
shipping silently.
