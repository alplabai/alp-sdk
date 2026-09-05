### Fixed — the IPC/flash-partition resolvers now derive flash class from the declared aperture, not just the legacy `carveout:` flag (#1365, split B)

Split A (#1365) added `soc_flash_base` and `write_authority` as additive
metadata and changed no allocator behaviour. Split B makes the two consumers
that matter -- `scripts/alp_orchestrate/carveout.py`'s IPC carve-out resolver
and `scripts/alp_orchestrate/partition.py`'s flash-device resolver -- derive
a region's class against the SoC's declared on-die MRAM aperture
(`alp_orchestrate.aperture`, factored out of `check_atoc_reservation.py`'s
own aperture math so both stay in sync) instead of trusting an authored
`carveout:` flag that, on `mram_main`, was never authored at all.

**IPC carve-out eligibility** (`carveout.py`) is now: the region's base
resolves and its derived class is not `flash`, and -- for a region the SoM
preset authored itself -- `write_authority` resolves to `customer_runtime`.
A region the loader DERIVED (SoC-level `memory_regions`, or the
silicon-variant fallback) needs no authority; it is RAM by construction.
Where the SoC declares no aperture at all (every non-Alif SoM), this is a
byte-identical no-op: the legacy `carveout: false` filter still runs
verbatim. Where a region's own base is unresolved (`mram_main`'s `"TBD"`,
unchanged by this split), the authored flag is honoured and named in the
block reason rather than guessed at (ADR-0034 clause 4) -- `write_authority:
composite` (not `customer_runtime`) now excludes `mram_main` on that basis,
closing the ordering hazard split A's changelog entry documented: an
`a32_cluster`/`m55_*` `ipc:` entry can no longer resolve into the on-die
MRAM aperture once `mram_main`'s base is eventually filled in, because the
derived-flash-class refusal does not depend on that base being unresolved.
A regression test (`tests/scripts/test_orchestrate_carveout_aperture_ordering.py`)
pins this ordering directly: with `mram_main`'s base synthetically resolved
in-memory, the `rpmsg-aen` / `mproc-mailbox` examples' `ipc:` entries still
block.

**Flash-device eligibility** (`partition.py`) is now: a region is a flash
DEVICE iff its resolved extent equals a declared aperture's extent (the
whole-device alias, e.g. `mram_main` once resolved); a region strictly
CONTAINED in an aperture is a partition inside a device, not a device of
its own -- the same practical outcome `carveout: false` encoded, now
derived instead of declared. Falls back to the legacy flag verbatim
wherever the derivation can't resolve (no aperture, or this region's own
base unresolved), so this too is a no-op on every current fixture.

**Measured, not assumed:** `check_emit_snapshots.py` moved from 37 to 33
byte-identical surfaces + 4 changed (`rpmsg-aen.system-manifest`,
`rpmsg-aen.build-plan`, `mproc-mailbox.system-manifest`,
`mproc-mailbox.build-plan`). Both `E1M-AEN801` examples' `alp_default_rpmsg`
/ `alp_shmem0` entries still resolve `status: blocked` -- no address moved,
no carve-out was ever allocated -- but the reason text changed from "base is
TBD" to naming the actual exclusion (the fine-grained regions' derived
`flash-class` containment, and `mram_main`'s disqualifying
`write_authority: composite`). This is the derivation becoming more
correct, not a functional regression; the golden snapshots were
deliberately left unrefreshed in this change so the difference stays
visible in review rather than silently re-baselined.

`carveout:` stays in the `som-preset-v1` schema (removing it would break
every existing `carveout: false` row and any customer copy) but is now
documented as a legacy override that must AGREE with the derived class,
never silently OR'd in -- see `docs/board-config-features.md`.
