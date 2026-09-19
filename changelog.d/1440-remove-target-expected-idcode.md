### Fixed — `gd32_swd.yaml`'s wrong-board guard presented an unattested value as armed; nothing was ever consuming it (#1440)

Nothing gated on `metadata/chips/gd32_swd.yaml`'s `target_expected_idcode`.
`gd32_swd_connect()` (`chips/gd32_swd/gd32_swd.c`) reads the SW-DP IDR into
`ctx->idcode` and never compares it against anything. The manifest field
itself has no code consumer: it is not read by any generator or by
`validate_metadata.py`, and it was only ever optional in
`metadata/schemas/chip-v1.schema.json`. The only place a comparison
happened at all was `examples/v2n/v2n-gd32-swd-flash/src/main.c`, against
the *header* macro this field mirrored, and that comparison already only
logged and continued on a mismatch — never fatal. So the field read as an
armed wrong-board guard while functioning as inert documentation, and the
value it carried, `0x6BA02477`, was not a GD32 measurement at all:
`CHANGELOG.md` records it as a measurement of a *different* target on the
same board — the V2N CM33 DAP on a V2N bench unit (`Found SW-DP with ID
0x6BA02477`, `Found Cortex-M33 r0p4`). The only other GD32 candidate on
record, `0x0BE12477`, has no attribution at all — no bench transcript, no
datasheet reference, no commit message.

A guard armed at a wrong ID is worse than an unarmed one: if the wrong
value happens to match the other board, it passes on exactly the board it
exists to exclude — and `0x6BA02477` is exactly that other board's own
measured value.

`target_expected_idcode` is now absent from `gd32_swd.yaml`, and
`metadata/schemas/chip-v1.schema.json` no longer declares the property at
all — with the schema's existing `"additionalProperties": false`, that is
a mechanical guarantee against silently repopulating it, not a comment a
future edit can ignore. The precedent this follows is
`metadata/schemas/soc-spec-v1.schema.json`'s own `expect_dpidr` field
guidance for every Alif Ensemble SoC variant (#1355): an absent key is the
correct published "unknown", and a guessed value is strictly worse than
absent (`metadata/socs/alif/ensemble/e8.json`'s `expect_dpidr` note is the
live, actually-measured instance of that stance). This is **not** the same
container as the V2N/V2M `gd32_bridge` SoM helper-firmware entries — those
carry no `flash_method` and no `flash_args` block at all (removed by
#1439), so there was never an `expect_dpidr` key there to leave unset in
the first place; an earlier draft of this fix cited that as the precedent
and it does not hold up.

The header macro `include/alp/chips/gd32_swd.h` mirrored is renamed
`GD32_SWD_GENERIC_CM33_R0P1_IDCODE` (was `GD32_SWD_EXPECTED_IDCODE`) —
keeping the old name while it names a value the manifest now deliberately
refuses to carry, and while the macro's own `@warning` says a correctly-
wired GD32 may not match it, was the same "confident fiction" this fix
otherwise removes. Its value (`0x6BA02477`) is unchanged; it stays only as
an informational generic-architecture reference that
`chips/gd32_swd/gd32_swd.c`'s connect-path comment and the
`v2n-gd32-swd-flash` example log, and neither gates on. `docs/abi/v0.16-snapshot.json`
(the current, not-yet-frozen snapshot) is regenerated for the rename;
every released `docs/abi/v0.*-snapshot.json` before it is untouched.

Also corrected, all without picking a winner between the two candidate
values: `chips/gd32_swd/gd32_swd.c`'s connect-path comment said "The
GD32G553 carries the Cortex-M33 r0p1 IDCODE" as fact — reworded to name it
the generic value, never measured on a GD32. The header's own comment said
"the bench measures the GD32 bridge answering `0x0BE12477`" a few lines
above stating that value has no attribution at all — `scripts/bench/aen/bench-env.sh`
itself says `0x0BE12477` is "NOT bench-verified", so "measures" was wrong;
reworded to match. The `v2n-gd32-swd-flash` example's `main.c` and
`README.md` said a correctly-wired GD32 is "expected to FAIL" the
comparison and that a mismatch is "expected on a real GD32G553" — both
assert a hardware fact only a measurement could establish, which is the
exact premise #1369 denies; reworded to "unknown, and the comparison
proves nothing either way", consistent with the rest of this fix.
`metadata/e1m_modules/README.md` and `docs/gd32-bridge.md` said the
manifest "expects" / "currently arms" its guard with `0x6BA02477`, which
is no longer true now that the field is absent.

`docs/glossary.md`, `docs/bring-up-aen.md`, `docs/test-plan.md` /
`docs/verification-status.md` (generated from it), `docs/_aen-runbook-section.md`,
and `docs/tutorials/07-recovering-a-bricked-bridge.md` already correctly
hedged this from earlier work (#1512, #1999) and needed no further change
here. `scripts/bench/aen/bench-env.sh` and
`scripts/bench/aen/flash-jlink-mramxip.sh` also carry `0x0BE12477` (a
*different*, AEN-Flow-D wrong-board rejection gate, not this manifest
field) and are deliberately left untouched: both are mid-rewrite on the
open #2032/#2064 branch.

**Closes #1440. Refs #1369.** #1440's ask — the manifest presenting an
unattested value as an armed guard — is resolved by removing the field
and closing the enforcement gap in the schema. #1369's ask is not resolved
by anything here and stays open: which of `0x0BE12477` / `0x6BA02477` (if
either) a GD32 actually answers is unknown, and settling it needs a probe
on a GD32G553, which no authorized bench place currently provides.
`alplabai/tan-cli#610` covers only that a `tan flash` consumer must
degrade cleanly when `expect_dpidr` is absent — it does not settle which
value is correct either, and remains not sufficient on its own to close
#1369.
