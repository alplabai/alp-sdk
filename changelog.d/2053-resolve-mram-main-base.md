### Fixed — `mram_main.base` resolved from the `"TBD"` sentinel to `0x80000000` on all seven AEN presets (#2053)

`mram_main.base` stayed the literal string `"TBD"` in every AEN SoM preset
(`metadata/e1m_modules/E1M-AEN301.yaml:198`,
`metadata/e1m_modules/E1M-AEN401.yaml:187`,
`metadata/e1m_modules/E1M-AEN501.yaml:184`,
`metadata/e1m_modules/E1M-AEN601.yaml:191`,
`metadata/e1m_modules/E1M-AEN701.yaml:219`,
`metadata/e1m_modules/E1M-AEN801.yaml:290`,
`metadata/e1m_modules/E1M-AEN803.yaml:231`), blocked twice over: first by
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

**Measured effect, re-run across both E8 presets (E1M-AEN801 and
E1M-AEN803), not assumed from the #2073 single-preset measurement:**
identical on both. No `ipc:` entry moves off `status: blocked` —
`mram_main` was already disqualified as a carve-out target by its authored
`write_authority: composite` (never `customer_runtime`), independent of
whether `base` resolves. No existing carve-out candidate (`mcuboot`,
`he_slot0`, `hp_slot0`, `reserved`, `storage`, `atoc`) gains or loses an
address; those six rows are unchanged. What changes: `mram_main`'s own
`memory_map` row (`kind`/`status` go from `unresolved`/`unresolved` to
`flash`/`ok` and it gains `base: 2147483648`), and the *reason text* every
blocked AEN `ipc:` entry cites for `mram_main`, from "has an unresolved
base ('TBD') so containment against the declared MRAM aperture can't be
verified..." to "[0x80000000, 0x80580000) is flash-class (contained in the
SoC's declared on-die MRAM aperture) -- not safe as an IPC carve-out
target". Same verdict, more precise reason. Confirmed against
`E1M-AEN803` by hand (no example `board.yaml` targets that SKU yet — its
board tree is a separate in-flight change, #2084) using the same
`rpmsg-aen` board.yaml with `som.sku` substituted; not committed, since it
duplicates an example this repo doesn't otherwise ship for that SKU.

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

External OSPI0 memories (`hyperram`, `ospi0` NOR) remain a deliberate
non-goal — see #2069 — and are not touched here.
