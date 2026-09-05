### Added — `system-manifest-v1` declares a `memory[]` contract, ahead of its producer (#1365, split C)

`system-manifest-v1` gains an optional root `memory:` array: the resolved
memory-region view of the SoM, one row per region `resolve_memory_map()`
yields, each carrying the flash/RAM class derived from the SoC's declared
aperture (split A) and the region's authored `write_authority`. Rows join
the manifest's existing panes by name — `ipc[].region` and
`storage[].flash_device` each name a `memory[].name`.

**No emitter writes this key yet, and that is deliberate.** Teaching both
producers to emit it waits on ADR-0026 section D, which decides who owns
manifest *bytes* once the planner moves to tan. This schema is alp-sdk's
under every outcome of that decision (ADR-0026 clause 2), so publishing the
contract now moves no emitted byte — `check_emit_snapshots.py` still reports
37 `--emit` surfaces byte-identical and `check_system_manifest.py` still
validates all four manifests. Emitting it now would instead rewrite roughly
a hundred byte-parity fixtures in a repo that may not own them next quarter.

So a consumer must read an absent `memory:` as *"this producer does not emit
it yet"*, never as *"this SoM has no memory regions"*. The field description
says so normatively, alongside two other rules worth stating once: a region
whose base does not resolve carries no `base` and says why (`status:
unresolved` plus a `reason`) rather than guessing one (ADR-0034 clause 4),
and `dt_label` appears only when verified against the board `.dts` (#1556),
never as the name-derived fallback.

The join is partial by construction, and the schema now says that too: an
`on_module.ospi_memories:` key is a legal `storage[].flash_device` target
but is a controller-instance name carrying a `capacity_mbit` and no base, so
it lies outside every aperture and gets no `memory[]` row. Partitions stay
in `storage[]` and carve-outs in `ipc[]`; `memory[]` is the set of
SoM-declared regions those two refer *into*, not a third copy of them.

The root stays `additionalProperties: false`. The schema description
previously advised consumers to tolerate unknown fields two lines above that
closed root, which reads as a contradiction; it now states why it is not
one. The closed root is a **producer** gate — it is what makes
`check_system_manifest.py` catch an emitter key typo instead of shipping it
— while tolerating unknown fields is **consumer** advice, for a tool reading
a manifest emitted by a newer SDK than it was built against. Relaxing the
root to accommodate a strict consumer would remove the only gate that
catches emitter typos; the strict consumer is the thing to fix.
