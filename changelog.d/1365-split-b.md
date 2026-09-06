### Fixed — the IPC/flash-partition resolvers now derive flash class from the declared aperture, not just the legacy `carveout:` flag (#1365, split B)

Split A (#1365) added `soc_flash_base` and `write_authority` as additive
metadata and changed no allocator behaviour. Split B makes the two consumers
that matter -- `scripts/alp_orchestrate/carveout.py`'s IPC carve-out resolver
and `scripts/alp_orchestrate/partition.py`'s flash-device resolver -- derive
a region's class against the SoC's declared on-die MRAM aperture
(`alp_orchestrate.aperture`, factored out of `check_atoc_reservation.py`'s
own aperture math so both stay in sync) instead of trusting an authored
`carveout:` flag that, on `mram_main`, was never authored at all.

**IPC carve-out eligibility** (`carveout.py`'s `_region_ipc_eligibility()`)
derives a region's class (`flash` / `ram` / `unclassified` / `unresolved`)
against the aperture, and containment is checked BEFORE authorship: a
region CONTAINED in the aperture is `flash` and refused regardless of who
authored it. A region outside the aperture that the SoM preset did NOT
author is `ram` by construction (needs no authority) -- every V2N/V2M/NX9101
row lands here, which is what keeps their resolution byte-identical. A
region outside the aperture that the preset DID author is `unclassified`
(containment is one-sided: outside proves nothing) and needs
`write_authority: customer_runtime` to be eligible. Where the class can't
be derived at all -- no aperture declared for this SoC (every non-Alif SoM,
handled before this function is ever called), or the region's own `base`
is unresolved (`mram_main`'s `"TBD"`) -- the authored flag decides, and
`carveout:` is honoured FIRST when present, ahead of `write_authority`
(the conservative, pre-split-B signal); `write_authority` is consulted only
when `carveout:` is absent, and the region refuses, naming both, when
neither is present (ADR-0034 clause 4). NOTE: "eligible" here is not the
same predicate as "resolves `base_is_unmapped` fine downstream" -- an
unresolved-base region can pass eligibility on an authored flag alone and
still land `status: blocked` at the separate `_region_top_init()` check
that requires an actual, mapped `base`/`size` to allocate against.

A present `carveout:` that DISAGREES with a resolvable derived class --
`flash`, `ram`, or an `unclassified` row's `write_authority`-derived
answer -- is refused, naming BOTH facts: the derived class (with the
addresses that produced it) and the authored flag. This closes a review-
round regression the first split-B cut of this eligibility function
introduced: a preset-authored row outside the aperture with
`write_authority: customer_runtime` AND an explicit `carveout: false`
used to resolve `status: ok` on the write-authority answer alone, silently
dropping the author's `carveout: false` -- proven end-to-end by appending
`{name: ospi_xip, base: 0xA0000000, size_kib: 1024, carveout: false,
write_authority: customer_runtime}` to a real AEN preset's `memory_map:`.
`tests/scripts/test_orchestrate_carveout_aperture_ordering.py`'s
`TestCarveoutAgreementBlocker` reproduces exactly this probe and asserts
the refusal; `TestUnclassifiedWriteAuthorityLegCoverage` separately proves
the `write_authority == "customer_runtime"` leg on the `unclassified`
branch is still load-bearing on its own (mutation-tested: dropping it
turns that test red).

Closes the ordering hazard split A's changelog entry documented: an
`a32_cluster`/`m55_*` `ipc:` entry can no longer resolve into the on-die
MRAM aperture once `mram_main`'s base is eventually filled in, because the
derived-flash-class refusal does not depend on that base staying
unresolved. `TestMramMainOrderingGuard` (same test file) pins this
directly: with `mram_main`'s base synthetically resolved in-memory, the
`rpmsg-aen` / `mproc-mailbox` examples' `ipc:` entries still block.

**Flash-device eligibility** (`partition.py`'s `_is_flash_sub_partition()`,
P2) is NOT an equality check: a region is a partition INSIDE a flash
device (excluded from `_known_flash_devices()` / `storage[].flash_device:`)
when its resolved extent is a PROPER SUBSET of the aperture
(`aperture.is_partition_inside_aperture()` returns `True`); it is a flash
DEVICE in its own right (kept in the advertised set) only when the extent
equals the aperture exactly (`False` -- e.g. `mram_main` once resolved).
Everything else -- extent outside the aperture, or unresolvable on either
side -- returns `None` and falls back to the legacy `carveout:` flag
(`region.get("carveout") is False`), never reading "not proven inside" as
"is a device" (review-round MAJOR 3).

**Measured, not assumed:** this change's own diff re-baselines exactly
four golden snapshots -- `rpmsg-aen.system-manifest`, `rpmsg-aen.build-plan`,
`mproc-mailbox.system-manifest`, `mproc-mailbox.build-plan`. In every one,
`status:` stays `blocked` and no address ever resolves; only the `reason:`
text changes, from "base is TBD" to naming the actual exclusion (the
fine-grained regions' derived `flash-class` containment, and `mram_main`'s
disqualifying `write_authority: composite`). A later review-round fix
capped the number of per-region details joined into one ineligibility
reason at 6 (`carveout.py`'s `_MAX_EXCLUDED_DETAIL`) -- an uncapped join
was genuinely unbounded (a future preset with more `memory_map:` rows
grows it without limit), reaching ~1.5 kB single-line in the emitted C
comment and DTS comment on the real 5-excluded-region AEN case today.
The cap sits one above that proven ceiling deliberately: every AEN
SKU's `memory_map:` has exactly 7 rows, so no `ipc:` entry on any SoM
declared today ever hits the cap, and excluded regions can carry
DIFFERENT reasons (flash-class containment vs. an unresolved base) --
truncating one silently would misrepresent why it was excluded, which
is why the cap bounds future growth without touching today's output.
`check_emit_snapshots.py` reports all 37 tracked `--emit` surfaces
byte-identical against the (unchanged) goldens.

`carveout:` stays in the `som-preset-v1` schema (removing it would break
every existing `carveout: false` row and any customer copy) but is now
documented, and enforced, as a legacy override that must AGREE with the
derived class where one is resolvable, never silently overridden by
`write_authority` or silently OR'd in -- see
`docs/board-config-features.md`.
