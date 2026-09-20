### Fixed — `mram_main.base` resolved from the `"TBD"` sentinel to `0x80000000` on all seven AEN presets (#2053)

`mram_main.base` stayed the literal string `"TBD"` in every AEN SoM preset
(`metadata/e1m_modules/E1M-AEN301.yaml:198`,
`metadata/e1m_modules/E1M-AEN401.yaml:187`,
`metadata/e1m_modules/E1M-AEN501.yaml:184`,
`metadata/e1m_modules/E1M-AEN601.yaml:191`,
`metadata/e1m_modules/E1M-AEN701.yaml:219`,
`metadata/e1m_modules/E1M-AEN801.yaml:290`,
`metadata/e1m_modules/E1M-AEN803.yaml:236` ("name: mram_main, base: 0x80000000")), blocked twice over: first by
`scripts/gen_zephyr_board.py::_aen_check_map_overlaps()` having no
whole-device-alias exception (fixed by #2073), then by
`scripts/check_atoc_reservation.py`'s top-anchor rule being unscoped to the
SoC's declared MRAM aperture (fixed by #2069). With both landed on `dev`,
`base` now resolves to `0x80000000` on all seven presets — not copied
across, but confirmed independently per SoC: every
`metadata/socs/alif/ensemble/{e3,e4,e5,e6,e7,e8}.json` declares
`"soc_flash_base": 2147483648`, and `scripts/gen_zephyr_board.py`'s own
`_AEN_MRAM_BASE` constant carries the same value, sourced from upstream
Zephyr's `mram: flash@80000000` node present in every Ensemble SoC dtsi.

**Measured effect, re-run across all seven presets, not assumed from the
#2073 single-preset (E1M-AEN801) measurement:** identical across all
seven. No `ipc:` entry moves off `status: blocked` on any of them — but
*why* it stays blocked changes, and that change broke three tests that
asserted the old reason (see "Test fallout" below). Before this change,
`mram_main`'s `base: "TBD"` made its derived class `unresolved`, and it
was disqualified there by its authored `write_authority: composite`
(never `customer_runtime`) — `scripts/alp_orchestrate/carveout.py`'s
`_region_ipc_eligibility()` `cls == "unresolved"` tail. After, its
resolved extent equals the SoC's declared App MRAM aperture exactly (the
whole-device alias), so `classify_region()` calls it `flash` outright and
the `cls == "flash"` branch disqualifies it **unconditionally** —
`write_authority` is not even consulted once `base` resolves. No existing
carve-out candidate (`mcuboot`, `he_slot0`, `hp_slot0`, `reserved`,
`storage`, `atoc`) gains or loses an address; those six rows are
unchanged on every preset. What changes on all seven: `mram_main`'s own
`memory_map` row (`kind`/`status` go from `unresolved`/`unresolved` to
`flash`/`ok` and it gains `base: 2147483648`), and the *reason text*
every blocked AEN `ipc:` entry cites for `mram_main`, from "has an
unresolved base ('TBD') so containment against the declared MRAM
aperture can't be verified, and its authored write_authority is
'composite', not 'customer_runtime'" to "[0x80000000, 0x80580000) is
flash-class (contained in the SoC's declared on-die MRAM aperture) --
not safe as an IPC carve-out target". Same verdict, different (and now
unconditional) reason. Confirmed against `E1M-AEN301/401/501/601/701`
and `E1M-AEN803` by hand (no shipped example `board.yaml` declares an
`ipc:` carve-out for any of these six SKUs -- `E1M-AEN301` and
`E1M-AEN401` each have an example targeting them, but neither one
exercises `ipc:`) using a synthetic `m55_hp`+`m55_he` rpmsg `board.yaml`
per SKU; not committed, since it duplicates coverage this repo doesn't
otherwise ship for those SKUs.

**Test fallout, root-caused and fixed, not papered over.** Three tests
asserted the pre-change reason/verdict against a REAL preset that no
longer produces it:
`tests/scripts/test_orchestrate_memory_regions.py::test_a_region_whose_base_does_not_resolve_is_kind_unresolved`
and `::test_an_unresolved_base_carries_status_and_reason_but_no_base`
(both against `E1M-AEN301`'s `mram_main`) and
`tests/scripts/test_orchestrate_memory.py::test_resolve_carve_outs_blocks_on_unmapped_base`
(against `E1M-AEN701`'s, asserting `"write_authority is 'composite'" in
entry.reason`, which no longer appears). Re-pointed at synthetic rows
(`memory._resolved_row()` / `carveout._region_ipc_eligibility()` direct
calls, the file's own established pattern) rather than flipped to assert
the new value against the same real preset — flipping would have
deleted the ADR-0034-clause-4 "unresolved base" witness entirely, since
no shipped preset authors one any more. A fourth test,
`test_size_is_emitted_even_when_the_base_is_unresolved`, kept passing but
had gone vacuous for the same reason and is moved onto the same synthetic
row. A fifth gap — `carveout.py`'s `write_authority`-present-but-wrong
refusal on the `cls == "unresolved"` tail (the exact leg `E1M-AEN701`'s
`mram_main` used to exercise) — had no OTHER test in the tree covering
it once that real-preset producer disappeared; added
`test_unresolved_base_with_write_authority_not_customer_runtime_refuses`
to `tests/scripts/test_orchestrate_carveout_aperture_ordering.py`'s
`TestUnresolvedLegOrdering`, proven to fail under a mutation that always
grants eligibility on that branch.

**Coverage gap this leaves, not silently absorbed.** Resolving the last
`"TBD"` base removed the only shipped preset that ever authored an
unresolved `memory_map:` base, so it also removed the only end-to-end
path through the unresolved-base legs of `memory.py::_resolved_row()`
and `carveout.py::_region_ipc_eligibility()`'s `cls == "unresolved"`
tail. The three tests above now pin those legs by calling the private
helpers directly instead of through a real preset, which proves the
legs themselves still behave correctly but no longer proves the WIRING
that routes an unresolved row into them — a future change that filtered
unresolved rows out earlier (e.g. in `carveout.py::_candidate_regions()`,
before `_region_ipc_eligibility()` is ever reached) would leave every
current pin green while the guard on a live preset went dead. alp-sdk#2096
proposes a synthetic *preset* fixture (not a synthetic row) that keeps an
unresolved base in the tree purely to exercise that routing again,
without reintroducing a real unresolved sentinel into any shipped SoM.

Regenerated and diffed rather than assumed cosmetic: the `rpmsg-aen` and
`mproc-mailbox` `system-manifest` and `build-plan` `--emit` snapshots
(`tests/fixtures/emit-snapshots/`) are the only four of the gate's 37
cases that changed, exactly as #2073 predicted. Every other `--emit`
surface — `soc_caps.h`, board routes headers, the CC3501E GPIO route
tables, the pinmux/support-matrix/portability-matrix/catalog/error-catalog
docs, and the ABI snapshot — regenerates byte-identical; none of them
consume `memory_map.mram_main`. A bench build
(`scripts/bench/aen/build.sh examples/aen/aen-adc-regcheck` for
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`,
`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`) produces a byte-identical
`zephyr.dts` and the same 81848 B / 31.22% FLASH usage before and after —
`mram_main` is excluded from the disjoint-slot0 partition table (only the
six fine-grained regions feed it), so resolving its `base` has no DTS
effect at all.

`tests/scripts/test_gen_zephyr_board.py`'s `TestAenMemoryMapValidation`
whole-device-alias tests (added by #2073) mutated `E1M-AEN801.yaml`'s
`"TBD"` line to a resolved one as their own setup; with the preset now
resolved on disk that substitution's anchor text no longer exists, so the
four tests are updated to start from the already-resolved fixture instead
of re-deriving it.

`changelog.d/1430.md` cited two lines inside `E1M-AEN801.yaml` (the
`storage`/`atoc` rows) that shifted when this change's comment edit
shrank the block above them (298/299 → 288/289); re-resolved in the same
change per the fragment convention.

Prose swept for the same reason across every file that narrated the
`"TBD"`/`write_authority`-composite state as current:
`examples/multicore/rpmsg-aen/board.yaml`,
`examples/multicore/mproc-mailbox/board.yaml` (both claimed `mram_main`
"deliberately keeps `base: TBD`" as the reason the carve-out blocks —
inverted now, it blocks because the resolved row is flash-class),
`docs/board-config-features.md` (which region-class branch `mram_main`
takes), `scripts/alp_orchestrate/carveout.py`,
`scripts/check_atoc_reservation.py`, `scripts/alp_orchestrate/aperture.py`,
`scripts/alp_orchestrate/partition.py` (also documents a verified-benign
behaviour change: `_reserved_spans()` now takes the `self_region` branch
for `mram_main` and skips the `lowest + capacity == highest region top`
identity check entirely, safe because the origin is now authoritative
rather than reconstructed), `scripts/gen_zephyr_board.py`, and
`tests/scripts/test_orchestrate_carveout_aperture_ordering.py` (whose
`TestMramMainOrderingGuard` tests still pass but narrated the resolution
as a synthetic what-if it no longer is). `docs/soms/aen.md` is excluded
deliberately — it is being edited on `feat/2084-aen803-board-tree`, kept
disjoint here. Two unrelated pre-existing "all six AEN presets" counts
(there are seven) in `docs/board-config-features.md` and
`docs/adr/0027-storage-regions-are-declared-by-role.md` corrected in the
same sweep.

External OSPI0 memories (`hyperram`, `ospi0` NOR) remain a deliberate
non-goal — see #2069 — and are not touched here.
