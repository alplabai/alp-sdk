# Execution prompt — chip + library ecosystem expansion (alp-sdk + alp-sdk-community)

> Paste the block below into a fresh Claude Code session at the
> alp-sdk repo root.  The prompt is self-contained; the agent
> reads the design spec from disk and executes phase by phase.

---

You're executing the chip + library ecosystem expansion for the
alp-sdk repo (github.com/alplabai/alp-sdk).  Branch: main.
The full design is in
[`docs/superpowers/specs/2026-05-14-chip-and-library-ecosystem-design.md`](docs/superpowers/specs/2026-05-14-chip-and-library-ecosystem-design.md).
Read it first.

## What lands in this work

**Phase 1 — Tier 1 chip drivers (alp-sdk)**

49 new chip drivers under `chips/<name>/`.  Each driver is one
commit per the project's §C.<N> convention.  Batch by domain:

- **§D.AI**: 18 chips (ov2640, ov5645, ov7670, ov9281, ar0234,
  imx219, imx477, gc2145, ti_ds90ub953_954,
  maxim_max9295_9296, st7789, ili9341, ili9488, ra8875,
  sh1106, il3820, gdew0154t8, hailo_8l)
- **§D.industrial**: 18 chips (bmp390, ms5611, lps22hb,
  vl53l1x, vl53l5cx, a02yyuw, drv8833, drv8825, tmc2209,
  a4988, as5048a_b, mt6701, hx711, max31855, max31865,
  tsl2591, qmc5883l, veml7700)
- **§D.iot**: 9 chips (quectel_bg95, quectel_bg77,
  ublox_sara_r5, semtech_sx1262, semtech_sx1276,
  ublox_neo_m9n, ublox_max_m10s, atgm336h, atecc608b)
- **§D.audio**: 6 chips (ics_43434, inmp441, wm8960,
  tlv320aic3204, max98357a, es8388)

Each driver follows the existing `chips/<name>/` pattern:

```
chips/<name>/
├── <name>.c              # implementation
└── (no separate include/ -- public header lives at
    include/alp/chips/<name>.h)
```

Plus:

- `include/alp/chips/<name>.h` — public API, Doxygen-clean
  (`@brief` / `@param` / `@return` triplets on every public
  function).  `@par ABI status: [ABI-EXPERIMENTAL]` until the
  first SoM verification lands.
- `metadata/chips/<name>.yaml` — chip metadata (vendor, part,
  ALP family compat, datasheet ref).
- One entry in `scripts/alp_project.py`'s `_CHIP_KCONFIG`
  table mapping `<name>` → required Zephyr subsystem
  Kconfigs.
- `tests/zephyr/chips/src/main.c` gets one ZTEST per new chip
  (NULL-handle + NULL-arg guard pattern matching existing
  driver tests).
- `CHANGELOG.md` entry under `### Added (2026-05-14 -- §D.<batch>: <chips>)`.
- `docs/abi/v0.5-snapshot.json` regenerated if the new header
  adds public symbols.

**Phase 2 — Tier 1 library knobs + HW-backend wiring (alp-sdk)**

17 new libraries added to `board.yaml`'s `libraries:` enum
+ the matching Kconfig emission in `alp_project.py`, AND each
library's HW-backend wiring per the design spec's
§"Hardware-acceleration backend wiring" section:

- **§D.lib.ai**: `tflite_micro`, `u8g2`, `gfx_compat`
- **§D.lib.industrial**: `madgwick_ahrs`, `pid`, `modbus`
- **§D.lib.iot**: `coremqtt_sn`, `libcoap`, `tinygsm`,
  `nanopb`, `libwebsockets`, `jsmn`, `bearssl`
- **§D.lib.audio**: `minimp3`, `opus`, `libhelix`
- **§D.lib.test**: `catch2`

Each library:

- New entry in `metadata/schemas/board-config-v1.schema.json`
  `libraries:` enum.
- New entry in `scripts/alp_project.py`'s `_LIBRARY_KCONFIG`
  table.
- Library profile / config header staged under
  `metadata/library-profiles/<name>/` for the v0.4 loader hook.
- **`metadata/library-profiles/<name>/hw-backends.yaml`** —
  declares which accelerators the library binds to (NPU /
  GPU / DSP-SIMD / crypto / DMA), per the design spec's
  per-library HW table.  Pure-SW fallback required.
- `metadata/library-registry.yaml` updated (if exists; create
  if not).
- For libraries that exist in Zephyr's west.yml already
  (tflite-micro, nanopb): add the name to the
  `name-allowlist` in our `west.yml`.
- For libraries NOT in Zephyr (u8g2, jsmn, etc.): pin them
  in our `west.yml` as new top-level projects under a new
  `extras-tier1` group (parallel to the existing
  `extras-v04` group), default-on.

**Phase 2b — Cross-library HW-backend loader hook**

One commit (`§D.lib.loader`) adds the loader hook itself:

- Extend `scripts/alp_project.py` (~80 LoC) to read each
  enabled library's `hw-backends.yaml` and cross-reference
  with the active SoM's `metadata/e1m_modules/<SKU>.yaml`
  `capabilities:` block.  Emit the highest-priority matching
  `CONFIG_*` knob per accelerator class.
- Fill `capabilities:` blocks for each existing SoM
  (AEN-DK / AEN-CARRIER / V2N-DEV / V2N-M101 / V2N-M102 /
  GD32-Discovery and any others).
- Add `Kconfig.alp-libraries` with the per-library +
  per-backend Kconfig symbols
  (`CONFIG_ALP_TFLM_ETHOS_U`, `CONFIG_ALP_LVGL_GPU2D`, etc.).
- Reuse the existing peripheral-backend selection paths:
  libraries reach ADC / PWM / timer / GPIO / I²C / SPI /
  UART / I²S / CAN / RTC / WDT / USB / DAC through the
  portable `<alp/*>` surfaces, and the SoM-specific backend
  already picks the DMA / hardware-timer / IRQ-slot path
  per the §C.15+ work in flight.  This phase ONLY adds the
  library-level accelerator wiring (NPU / GPU / SIMD / crypto)
  on top of that.

**Phase 3 — Tier 2 repo bootstrap (alplabai/alp-sdk-community)**

Create a NEW repo `alplabai/alp-sdk-community`.  Land the
skeleton per the design's §"Repo skeleton" section:

- `README.md` (how-to-contribute + tier-2 quality bar).
- `LICENSE` (Apache-2.0 umbrella).
- `registry.yaml` (initially empty `contributions: []`).
- `west.yml` (consumable as a project from alp-sdk's
  workspace).
- `templates/chip-skeleton/`, `templates/library-skeleton/`
  (cp-r'able starting points).
- `.github/workflows/`:
  - `pr-build-contribution.yml`
  - `pr-metadata-validate.yml`
  - `pr-lint.yml`
- `.github/CODEOWNERS` (alplabai/* owns registry + infra).

Plus, in alp-sdk:

- `metadata/schemas/contribution-v1.schema.json`
  (schema file Tier 2 contributions validate against).
- `docs/contributing-tier-2.md` (per-contribution
  checklist + customer integration patterns A/B/C).

**Phase 4 — Seed Tier 2**

Port 10 popular embedded-community chips to alp-sdk-community
to prove the template + CI before inviting external contributors:

`bme680`, `mpu6050`, `pca9685`, `sx126x` (LoRa alt to the
Tier 1 sx1262), `nrf24l01`, `dhtxx`, `pcf8574`, `ssd1351`,
`rfm95w`, `ds18b20`.

## Conventions to follow

- **Branch model**: per `docs/branching-and-merge-policy.md`.
  Branch protection on main is configured + active
  (`enforce_admins: true`).  Each commit needs to go through
  the override-push-rearm pattern OR via PR (which currently
  blocks because solo-maintainer can't self-approve).  Use
  override pattern:

  ```bash
  gh api --method DELETE \
    repos/alplabai/alp-sdk/branches/main/protection/enforce_admins
  git push origin main
  gh api --method POST \
    repos/alplabai/alp-sdk/branches/main/protection/enforce_admins
  ```

  Log each override in the commit message body.

- **Commit cadence**: one chip / one library per commit.
  Title format `feat(chip): <name> driver (§D.<batch>.<n>)`
  or `feat(lib): <name> knob (§D.lib.<batch>)`.

- **CHANGELOG**: each commit appends one bullet under its
  `### Added (2026-05-14 -- §D.<batch>: ...)` section.

- **Validators that must stay green** (run before each push):
  - `python3 scripts/validate_metadata.py` — 0 failures
  - `python3 scripts/check_example_portability.py` — 30/30
  - `python3 scripts/check_pin_conflicts.py` — clean
  - `python3 scripts/abi_snapshot.py --diff docs/abi/v0.5-snapshot.json`
    — clean or matching the commit's intended ABI change

- **Mandatory SDK rules** (each one is a hard pass/fail
  gate; non-compliance blocks merge):

  - **Chip driver naming convention.**  Drivers under
    `chips/<part>/` use the chip's natural name —
    `lsm6dso_init()`, not `alp_lsm6dso_init()`.  `alp_` is
    reserved for SDK abstractions only.

  - **Portable peripheral surfaces only in app + library
    code.**  PWM, ADC, encoder, I²C, SPI, UART, GPIO, CAN,
    I²S, RTC, WDT, USB, DAC MUST go through `alp_*_open` +
    `ALP_*` instance IDs in chip drivers, libraries, and
    example apps.  Chip-specific backends (`gd32g553_*`,
    `alif_*`, `renesas_*`, `nxp_*`) live inside the SDK's
    own backends + dedicated bridge demos — never in
    general examples.

  - **Portable HW-offload surfaces.**  TMU / FFT / FAC /
    power / CAU live in `<alp/*.h>` with no vendor part name
    visible to application code.  SW fallback (libm /
    CMSIS-DSP / Zephyr `pm`) is preferred over NOSUPPORT
    whenever reasonable.  HW-backends.yaml entries MUST name
    accelerator *classes*, not vendor parts.

  - **No zero-value re-export headers.**  Every public
    `<alp/*.h>` must add value (portability shim, defaults,
    type normalisation).  Pure `#include "upstream.h"`
    headers are rejected.

  - **No Nordic-branded tooling.**  SDK targets Alif /
    Renesas / NXP, not Nordic.  Vendor-neutral DTS / Kconfig
    extensions only.

  - **Example apps are documentation.**  Files under
    `examples/<peripheral>-<demo>/src/main.c` are reference
    material for hand-written firmware — comment them as
    teaching artefacts (~50 % comment ratio).

  - **Doxygen on public headers.**  Every public function in
    `include/alp/**.h` and `include/alp/chips/**.h` gets
    `@brief` / `@param` / `@return` triplets.  This overrides
    the default "no comments" instinct on documentation
    surfaces.

  - **Standalone usage is first-class.**  Hand-written
    firmware that bypasses alp-studio is a supported
    consumer path, not a workaround.  Don't gate behaviour
    behind alp-studio dependencies.

  - **Descriptive filenames.**  `hw-revisions.yaml`,
    `board.yaml` — every file's name describes its
    *contents*, not its category.  The directory carries
    the scope.

  - **No local paths or SoM design leaks.**  Never write
    `OneDrive/...` or `C:\Users\caner\...` paths into
    tracked files.  Minimise SoM schematic-level integration
    detail in public docs; route detail-rich content into
    the private `e1m-som-metadata` repo.

  - **No Claude co-author footer.**  Commits attribute
    solely to alpCaner.  No `Co-Authored-By: Claude` line.

  - **Pending exact hardware configurations.**  The user
    will hand-write authoritative pin assignments and
    per-SoM HW config.  Mark unknowns `TBD`, never invent
    values.

## How to run nonstop

1. Read the design spec end-to-end.
2. Start with Phase 1 §D.AI (image sensors are the most
   self-contained — pure I²C / DVP / MIPI configuration).
3. One commit per chip.  Override + push + re-arm each.
   Run validators before each push.
4. Re-tag CHANGELOG and v0.5 snapshot per commit.
5. After §D.AI's 18 chips are in, run a CI sweep to confirm
   pr-twister + pr-static-analysis + pr-metadata-validate
   stay green.
6. Move to §D.industrial, §D.iot, §D.audio in that order.
7. Then Phase 2 (library knobs + per-library hw-backends.yaml)
   — one commit per library; mostly metadata + script edits.
8. Then Phase 2b (cross-library HW-backend loader hook) —
   one commit that flips the whole library set onto the
   active SoM's accelerators.  Verify by building one
   sample per (library × SoM-family) pairing and reading
   back the emitted `CONFIG_*` set.
9. Then Phase 3 — create the NEW repo via
   `gh repo create alplabai/alp-sdk-community --public
   --license apache-2.0 --description "Community
   chip drivers + libraries for alp-sdk"`, push the
   skeleton.
10. Then Phase 4 — port the 10 seed chips into the new repo.
11. Final closeout commit on alp-sdk:
    - Update `docs/v1.0-readiness.md` Pillar 4 + Pillar 9
      paragraphs to reference the new ecosystem.
    - Bump the chip count claim in `README.md` + any
      `docs/test-coverage-audit.md` row.

## Stop conditions

- All 49 chips + 17 libraries land.
- alp-sdk-community repo skeleton public + the 10 seed
  chips imported.
- All validators green.
- pr-twister + pr-static-analysis + pr-metadata-validate +
  pr-generated-files + pr-doxygen + pr-plain-cmake +
  pr-gd32-bridge-build green on the latest commit.
- Branch protection re-armed after the last push.

If any chip's driver hits a non-trivial design question
(unusual bus, multi-die device, MIPI-CSI-2 negotiation),
mark it `[stub-impl]` with a one-paragraph note in the
header and move on -- don't block the run.  Real
implementations can land in follow-up commits once HiL is
available.
