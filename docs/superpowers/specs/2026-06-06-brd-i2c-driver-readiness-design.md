# BRD_I2C Driver Readiness for V2N SoM Bring-up — Design

**Date:** 2026-06-06
**Status:** Approved
**Branch:** `dev`

## Context

The E1M-X V2N SoM routes eight ICs over the shared `BRD_I2C` management bus
(master: Renesas RZ/V2N, `bus_id 0` / RIIC8). The bus is currently
hardware-blocked — something pulls the lines low; a board patch is pending.
Software must be fully prepared so bring-up can start the moment the patch
lands. No on-silicon testing is possible until then.

Authoritative device list (`metadata/e1m_modules/E1M-V2N101.yaml`,
identical across the V2N/V2M family):

| Chip | Role | 7-bit addr |
|---|---|---|
| rv3028c7 | RTC | 0x52 |
| optiga_trust_m | Secure element | 0x30 |
| tmp112 | Temp sensor | 0x40 |
| clk_5l35023b | Clock generator | 0x68 |
| gd32g553 | Supervisor MCU (I2C slave) | 0x70 |
| act8760 | Main PMIC page 0 / page 1 | 0x25 / 0x26 |
| da9292 | Secondary PMIC | 0x1E |
| tps628640 | LPDDR4X 0.6 V buck | 0x4D (assembled: optional) |

All eight already have real drivers under `chips/<name>/` with genuine
I2C probes in `init()`. Remaining gaps, confirmed by audit:

1. `act8760_rail_get_vset()` / `act8760_rail_set_vset()` return
   `ALP_ERR_NOSUPPORT` — per-rail VSET register offsets were never
   confirmed against vendor documentation.
2. `da9292` decodes `PMC_STATUS_00` assuming its bit order mirrors
   `PMC_MASK_00`; never cross-checked against the datasheet status table.
3. No example exercises the whole bus — nothing to flash on patch day.
4. None of the BRD_I2C chip drivers have mocked-I2C unit tests.

## Work stream 1 — Datasheet gap-fixes

Sources (team design drive, RZ-V2N E1M project, *Design Documentation /
Datasheets*; referenced by document name only — never by local path):

- *ACT88760 Users Guide Rev 3.0* + *ACT88760 Datasheet Rev C* +
  the ActiveCiPS **DieLib XML** (machine-readable register map) +
  the Alp CMI 120.E1 `.iact` configs and CMI power-sequence document.
- *DA9292 Datasheet 2v2* (status/event/mask register tables).

Changes:

- **act8760**: fill the per-rail VSET offset table from the Users Guide,
  cross-checked against the DieLib XML; un-stub `rail_get_vset` /
  `rail_set_vset`. Validate the VSET encoding (step size, range, bit
  field) against both sources. Any rail the documents do not confirm
  stays `NOSUPPORT` with a TODO naming the missing table. Update
  `metadata/chips/act8760.yaml` if it carries the same unverified data.
- **da9292**: verify each `DA9292_STATUS00_*` bit position against the
  datasheet status-register table; correct the defines if the
  mirror-of-mask assumption is wrong. Same check for the event/fault
  registers the driver touches. Update `metadata/chips/da9292.yaml`
  in the same slice if affected.
- Register facts must be quoted with their table/section number in the
  driver comments. No invented values — unverifiable facts stay TBD.

## Work stream 2 — Bring-up example `examples/v2n/v2n-brd-i2c-bringup/`

A flash-on-patch-day diagnostic. Strictly **read-only toward every PMIC**
— the example never writes a voltage, enable, or control register.

- **Phase 0 — bus health.** Before any transfer: detect a stuck-low bus
  and report it explicitly (distinguishing "bus held low" from
  per-device NAKs — the exact failure the board currently exhibits).
  Then a full 0x08–0x77 scan, diffed against the expected-device table.
- **Phase 1 — per-IC probe**, one section per chip, all read-only:
  - rv3028c7: STATUS read + time read (sanity: year in plausible range)
  - tmp112: CONF read (R1:R0 = 11 fingerprint) + temperature read
  - clk_5l35023b: General Control + Dash Code ID + strap check
  - act8760: register 0x00 status read on both pages
  - da9292: DEV_ID / REV_ID + status read
  - tps628640: VOUT1 read; absent device reports `SKIP (optional fit)`,
    not FAIL
  - optiga_trust_m: I2C_STATE register read
  - gd32g553: PING + GET_VERSION over the **I2C transport** (0x70)
- Output: a single aligned PASS / FAIL / SKIP table on the console with
  per-device detail lines (IDs, readings) above it.
- Teaching-artifact comment density (examples are documentation).
- Twister-buildable: `native_sim.conf` overlay per the established
  convention; real-board overlay for the V2N M33 Zephyr board.

## Work stream 3 — Unit tests in `tests/zephyr/chips/`

Extend the existing fake-device pattern (`fake_bme280.c` et al.) with
fakes + ztest suites for: rv3028c7, tmp112, clk_5l35023b, act8760
(dual-page), da9292, tps628640, optiga_trust_m (probe path only).

Behaviour locked by tests (register-level, against the fake):

- rv3028c7: BCD time encode/decode round-trip, alarm mask encoding,
  POR-flag clear on init
- tmp112: 12-bit and 13-bit (extended) sign-extension and milli-°C math
- clk_5l35023b: strap-address validation rejects mis-strapped boards
- act8760: dual-page probe, VSET mV↔register round-trip (post-WS1)
- da9292: STATUS00 decode (post-WS1 truth), the VSTEP=1→0 clear-before-
  write sequence in `v2n_m1_enable_deepx_rail` (the 1.5 V OTP trap),
  voltage mV↔register round-trip and range clamps
- tps628640: voltage round-trip, CONTROL shadow read-modify-write,
  optional-fit NAK → clean error
- optiga: probe ACK/NAK paths
- Every driver: NULL/uninitialised-context guards, I2C-error propagation

## Out of scope

- OPTIGA full APDU / info-pack transport (separate vendoring project)
- GD32 firmware-side changes (host-side I2C transport already exists)
- Issue #90 (pr-twister `tests/unit` gap) — separate follow-up
- Any write path toward PMICs in the example

## Acceptance

- `act8760` VSET paths real (or documented-as-missing per rail);
  `da9292` status decode datasheet-verified with citations
- Example builds for native_sim + the V2N M33 board under twister
- All new unit tests green on native_sim
- Full local CI gates (twister scope + pytest + clang-format + doc-lint)
  pass on Windows + WSL before push to `dev`
