# ALP-B008: unknown chip in `chips:`

A `board.yaml` top-level `chips:` entry doesn't resolve to a real chip driver
manifest under `metadata/chips/` (or to one of the SDK-level "block" helpers
`chips:` may also name -- `button_led`, `pdm_mic`), OR it names a manifest
that exists but carries `driver_status: planned` -- no `chips/<id>/` driver
and no `ALP_SDK_CHIP_<NAME>` Kconfig symbol ship for it yet.

`chips:` is schema-validated only against a permissive identifier shape
(`^[a-z][a-z0-9_]+$`); a typo or invented name still matches that pattern, so
without this cross-reference check the entry would pass schema validation
clean and be emitted verbatim into the generated `alp.conf` as
`CONFIG_ALP_SDK_CHIP_<NAME>=y` -- a Kconfig symbol nothing declares, so the
driver is silently not built and the app fails at runtime instead of at
validation time.

## Cause

- A typo in the chip name (e.g. `bme208` for `bme280`).
- A chip driver that was renamed or retired since the `board.yaml` was
  written.
- A part that genuinely has no Alp SDK driver yet -- see
  [`chips/README.md`](../../chips/README.md) for the shape a new driver
  takes, or file an issue.
- The manifest exists but is `driver_status: planned` -- the part is on the
  roadmap, but no `chips/<id>/` driver or `ALP_SDK_CHIP_<NAME>` Kconfig
  symbol has shipped for it yet, so naming it in `chips:` would emit an
  undeclared symbol the same way a typo would.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

```
error[ALP-B008]: chips: unknown chip 'bme208' (no metadata/chips/bme208.yaml)
```

A `driver_status: planned` manifest reports the same code with different
text -- the manifest is not missing, the driver just isn't built yet:

```
error[ALP-B008]: chips: 'murata_lbee0zz2kl' has driver_status: planned -- no Alp SDK driver or ALP_SDK_CHIP_MURATA_LBEE0ZZ2KL symbol yet
```

List every chip manifest this checkout actually ships (read-only):

```sh
ls metadata/chips/*.yaml
```

A typo close to a real manifest gets a "did you mean" suggestion from the
validator.

## Fix

Correct the `chips:` entry to a real manifest name under `metadata/chips/`
(the filename stem, e.g. `metadata/chips/bme280.yaml` -> `bme280`), or to one
of the SDK block helpers (`button_led`, `pdm_mic`) if that's what was meant.

If the manifest exists but is `driver_status: planned`, there is no
alternate manifest to switch to -- the chip isn't buildable yet. Drop it
from `chips:` until a driver ships, or track the part on its tracking issue.

## Escalate

If the part is real and genuinely has no Alp SDK driver, that's a new chip
to add, not a typo -- open an issue describing the part and its bus (see
`chips/README.md` for the layout a new `chips/<part>/` driver takes, and
`docs/adr/0017-alp-sdk-over-the-vendor-sdk.md` for the Tier 1/1.5/2/3
ladder that decides whether it binds a vendor HAL or goes upstream-native).
