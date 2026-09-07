### Fixed — several `scripts/bench/aen/` helpers silently REPLACED the whole ATOC instead of adding to it, deleting resident boot entries with no warning (#2025)

`flash-run.sh` generates a fresh SETOOLS ATOC config with exactly two
entries (`DEVICE` + `ALP-HE`) and burns it with `app-write-mram -p`, which
**replaces** the device's entire Application Table of Contents rather than
merging into it — the header comment's "keeps the factory DEVICE cfg" was
true but incomplete: it kept `DEVICE` and silently dropped everything else.
This destroyed a live A32 Linux boot chain (`BOOTLOAD`/`A32_APP`/`HP_APP`/
`HE_APP`) on `e1m-aen-evk-01` on 2026-09-07 — one Flow A run left only
`DEVICE` and the new `ALP-HE`, with no error and no SES warning (`[SES]
ATOC ok` prints either way).

Added a GUARD that runs before the write: query the currently-resident ATOC
with SETOOLS' `maintenance -c $SE_UART -opt gettoc` (a non-destructive TOC
read, not a merge engine), log the pre-write table unconditionally, and
refuse to proceed (exit 5) if any resident entry other than `DEVICE`/
`ALP-HE` is found, naming what would be delisted. Also refuses if the query
itself can't be verified (tool missing, no parseable table, and no `No
ATOC` state either) rather than assuming it is safe to write blind. A new
`--replace-atoc` flag opts out of the guard for an intentional replace.
Updated the step-1 header comment to say the ATOC is REPLACED, not kept.

The same hazard exists in every other `scripts/bench/aen/` helper that
commits a freshly-generated `app-gen-toc` package: `loadbin $PKG $ATOC_ADDR`
over J-Link burns the identical signed ATOC structure `app-write-mram -p`
does, at the same MRAM location, so it replaces rather than merges too.
Factored the guard out of `flash-run.sh` into a shared
`bench_atoc_replace_guard()` in `bench-env.sh` (query + parse + allow-list +
abort logic, parameterized by the entries the caller is itself about to
write) and applied it to:

- `flash-run-dualcore.sh` — allows its own `ALP-HP`/`ALP-HE` two-entry write
  (the whole point of that script), refusing only a genuinely foreign
  resident entry. New `--replace-atoc` flag.
- `flash-update-log-dual.sh` — allows `HP-OWNER`/`HE-CLIENT`. New
  `--replace-atoc` flag (combinable with the existing `--package-only`).
- `flash-update-log-firewall-probe.sh` — allows `HE-PROBE`. Same new flag.

`flash-run.sh` itself now calls the shared function instead of carrying its
own copy.

Swept the rest of `scripts/bench/aen/` for the same shape: `erase-storage.sh`
never touches the ATOC band (writes only the customer `storage` window and
asserts it is adjacent to, not overlapping, `atoc`); `ram-run.sh` is a
RAM-only Flow C run with no MRAM write; `flash-all-flowd.sh` has no ATOC
generation of its own, it only invokes `flash-jlink.sh` in a loop.
`flash-jlink.sh`, `flash-jlink-hp.sh`, and `flash-jlink-mramxip.sh` generate
and burn a replacing ATOC the same way (Flow D, `DEVICE`+`ALP-HE`/`HP-APP`
only) but currently have no SE-UART dependency at all — extending this guard
to them means deciding whether they should newly depend on `SE_UART` for a
read-only `gettoc` query, a design tradeoff against their "no SE-UART" Flow D
premise that this change does not resolve. Refs #2027.

**Validated against real silicon captures** off `e1m-aen-evk-01` (2026-09-07,
ANSI intact), and three defects the earlier synthetic-fixture coverage
missed fixed as a result: the transcript path (`${TMPDIR:-/tmp}/<tag>-atoc-
before.log`) was NOT run-unique, so two concurrent runs of the same script
against different boards could interleave and one could read the other's
transcript — now `mktemp`-generated per run; `getbanner`'s real line has a
LEADING SPACE ("` SES A1 v1.110.0 ...`"), which a bare `^SES` anchor
rejected outright, aborting every run on real hardware — anchor now
tolerates leading whitespace; SETOOLS colours the WHOLE LINE, not just the
cell text, so the ANSI-stripped `gettoc` row also keeps a leading space,
which the table-row match (`/^\|/`) never matched at all — `resident`
silently computed empty on every real coloured transcript. Also: `SERAM0`/
`SERAM1` (the two on-module SE firmware banks, never touched by
`app-write-mram -p`) are resident on every real board regardless of the
last app write and are now exempted alongside `DEVICE`, and `getbanner`'s
own exit status is now folded into the precheck (a non-zero exit with an
otherwise well-formed banner line no longer reads as verified).
