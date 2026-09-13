### Added — TAS2563: tuning replay, I2S/TDM configuration and fault-pin handling (#2040)

`include/alp/chips/tas2563.h` promised three things for v0.3.x and shipped
none of them: a tuning-blob loader, I2S configuration paired to the host bus,
and fault-pin handling on the amp's open-drain `IRQ_N`. All three are here
now, plus a fake TAS2563 i2c-emul target and 15 ZTests — the driver had no
register-level test of any kind before this.

Everything is cited to **TI SLASET3D** (April 2019, revised January 2024) by
section, table and page. The manifest's `datasheet.primary` said `SLASEU1`, a
number that predates a TI literature renumbering of the same device; it now
names SLASET3D, matching what the `physical:` block already used.

**New public API** (all natural-prefix, per `chips/README.md`):

- `tas2563_configure_i2s()` — translates the `alp_i2s_config_t` the caller
  opened the host bus with into `TDM_CFG0`/`TDM_CFG1`/`TDM_CFG2`
  (§7.5.8–§7.5.10, p.68–70): sample rate, RX word/slot length, frame offset
  and which slot this mono amp plays. `AUTO_RATE` stays at its reset value,
  so the setting is a declaration rather than an override.
- `tas2563_configure_iv_sense()` — the SDOUT return path: powers the sense
  blocks (`PWR_CTL.ISNS_PD`/`VSNS_PD`, reset *powered down*, §7.5.4 p.66) and
  then hands each a transmit slot (`TDM_CFG5`/`TDM_CFG6`, §7.5.13/§7.5.14
  p.71). Enable powers up before enabling slots; disable stops transmitting
  before powering down.
- `tas2563_configure_fault_pin()`, `tas2563_fault_asserted()`,
  `tas2563_read_faults()`, `tas2563_clear_faults()` — binds `IRQ_N` to a host
  GPIO with a pull-up, optionally enables the part's own 20 kΩ pull-up
  (`MISC_CFG1.IRQZ_PU`, §7.5.6 p.68), points `IRQZ_PIN_CFG` at latched
  interrupts (§7.5.43 p.86), and reads `INT_LTCH0/1/3/4` back as one
  byte-aligned `uint32_t` behind named `TAS2563_FAULT_*` bits.
- `tas2563_set_amp_level()` — sets `PB_CFG1.AMP_LEVEL` (§7.5.5 Table 7-105,
  p.67), validated against the `01h`..`1Ch` range the table lists. See the
  safety note below for why this exists.
- `tas2563_load_tuning()` — replays a `tas2563_tuning_reg_t`
  (book, page, register, value) stream, tracking the selected book/page and
  restoring book 0 / page 0 on both the success and the failure path, because
  every other function in the driver addresses that page.

**Safety.** This is a part rated ~10 W peak into 4 Ω (§1, p.1), so:

- `tas2563_init()` now explicitly writes `PWR_CTL.MODE = 10b` (software
  shutdown) after a successful probe. Releasing `SD_N` does land there
  (§7.3.11.1, p.34), but a warm restart with `SD_N` board-tied high never
  goes through it and software shutdown preserves register state (§7.3.11.2),
  so the previous firmware's `ACTIVE` could otherwise survive into `init`.
- `tas2563_set_mode()` refuses any encoding outside the three modelled modes.
  `MODE = 11b` runs load diagnostics into the speaker terminals and then
  leaves the device ACTIVE (§7.3.11.5, p.35) — not somewhere a stray cast
  integer should be able to put the amplifier.
- `tas2563_load_tuning()` rejects any record targeting a driver-owned control
  register in book 0 / page 0 — `SW_RESET` (0x01, wipes the caller's whole
  configuration mid-load), `PWR_CTL` (0x02, the operating mode), `MISC`
  (0x32, `IRQZ_POL`: flipping it inverts the fault pin under
  `tas2563_fault_asserted()` with no readback saying so, §7.5.45 Table 7-145
  p.87) and `TG_CFG0` (0x3F, the tone generator, §7.5.50 p.89). Validation
  runs over the whole stream before the first write, so an illegal record
  does not leave a half-applied tuning.
- `tas2563_deinit()` falls back to writing software shutdown when no `SD_N`
  pin was supplied; previously it did nothing in that case.

**`PB_CFG1` is deliberately NOT on that blocklist, and the output level is not
low by default.** `AMP_LEVEL` powers up at `10h` = 16.0 dBV / 8.92 Vpk —
roughly 9.9 W peak into 4 Ω, only 6 dB below the `1Ch` = 22 dBV / 17.8 Vpk
maximum. Blocking `PB_CFG1` would make the loader refuse real PPC3 exports,
since output level is exactly what a smart-amp tuning sets; the hazard is
handled the other way round instead. `tas2563_set_amp_level()` lets a caller
pick a level for its enclosure, and the header now carries a `@warning`
telling it to do so before the first `tas2563_set_mode(ACTIVE)` — and *after*
`tas2563_load_tuning()` if both are used.

### Fixed — TAS2563: `init` now selects book 0, not just page 0 (#2040)

`tas2563_init()` selected `PAGE = 0` and then read `REVID` and wrote
`PWR_CTL` — but never touched `BOOK`. `BOOK` survives software shutdown along
with the rest of the register state (§7.3.11.2, p.34), which is exactly the
warm-restart case the park-write exists for: a device left mid-tuning by a
previous firmware comes up with a non-zero `BOOK`, and both accesses would
then land in coefficient space instead of the control registers. `init` now
uses the driver's existing `select_book0_page0()`.

### Fixed — TAS2563: mode writes no longer disturb the IV-sense power bits (#2040)

`tas2563_set_mode()` masked `PWR_CTL` with `0x07`. `MODE` is bits 1..0 only —
bit 2 is `VSNS_PD` and bit 3 is `ISNS_PD` (§7.5.4 Table 7-104, p.66) — so
every mode change also cleared `VSNS_PD`, silently powering the voltage-sense
block up as a side effect of going ACTIVE. The mask is now `0x03`. The
in-code comment citing "Table 7-50"/"table 7-58" for this register was wrong
in the same way and has been replaced with the real citations.

### Datasheet conflicts, recorded rather than resolved

SLASET3D contradicts itself in two places this driver touches. Neither is
decidable without silicon, so both are documented at the point of use instead
of being quietly picked:

- **Reading `INT_LTCH` may or may not clear it.** §7.3.12 (p.36) says
  "Reading the latched fault status register (INT_LTCH[7:0]) clears the
  register"; the field tables for those same registers (§7.5.36–§7.5.39,
  p.82–85) say every bit is "cleared using CLR_INTP_LTCH". If p.36 is right,
  `tas2563_read_faults()` is destructive and a retry after a transient bus
  error loses the fault. Its doc comment now carries that warning and no
  longer promises latched bits survive the read. The fake models the
  field-table behaviour only, and says so — that is a choice, not evidence,
  and no test here can decide it.
- **Two sample-rate encodings are listed twice, differently.** §7.4.2
  Table 7-23 (p.40) marks `000b` and `010b` **Reserved**; §7.5.8 Table 7-108
  (p.69) — the field description for the bits actually being written — lists
  them as 7.35/8 kHz and 22.05/24 kHz. §1 (p.1) advertises "8kHz to 96kHz",
  backing `000b`; nothing outside Table 7-108 backs `010b`. The driver follows
  the field table and emits both, with the conflict spelled out in
  `samp_rate_code()`: refusing a rate the field table documents is the more
  surprising of the two guesses, and `FS_RATE` (§7.5.19, p.73) will settle it
  once there is hardware. Two further Table 7-23 caveats are now recorded:
  `110b` (176.4/192 kHz) is "supported only by QFN device package" (the fitted
  TAS2563RPP is QFN, so it applies here), and 192 kHz is internally
  down-sampled to 96 kHz, so content above 40 kHz aliases at that rate.

### Deliberately not included

- **Parsing TI's PPC3 export container.** Converting a PPC3 `.bin`/`.cfg`
  export into a `tas2563_tuning_reg_t` array is a host-side step. That format
  is a TI tool format, described nowhere in SLASET3D, and writing a parser for
  it without a real export to check against would be guesswork — the one
  thing this change refuses to do. `tas2563_load_tuning()` takes the record
  array; producing it is the follow-up.
- **A packed multi-register burst encoding.** The record form costs four bytes
  per register written, which is expensive for a large DSP tuning. A packed
  form needs the register address to auto-increment across a multi-byte
  write; §7.3.7 (p.33) titles itself "Multiple-Byte Write and Incremental
  Multiple-Byte Write" but never restates that it does -- it only describes
  the electrical framing (each data byte gets its own ACK). **Correction,
  #2077:** §7.3.5 "Single-Byte and Multiple-Byte Transfers" (p.32), the
  section immediately before it, does state it in prose: "the register
  issued then serves as the starting point, and the amount of data
  subsequently transmitted... determines to how many registers are
  written." So auto-increment IS documented, just not in §7.3.7 specifically
  -- there is still no silicon here to have exercised that path, so this
  driver stays on single-byte writes for now regardless. #2077 makes the
  same correction where the driver's own code comment repeated this
  fragment's imprecise framing (`chips/tas2563/tas2563.c`,
  `include/alp/chips/tas2563.h`).
- **A 20-bit `RX_WLEN` mapping.** Table 7-110 encodes it, but `<alp/i2s.h>`
  documents `alp_i2s_config_t.word_bits` as 16/24/32, so no host bus this
  function pairs with can be opened at 20 bits. A mapping unreachable through
  the real API is a mapping that cannot be tested, so 20 lands in
  `ALP_ERR_OUT_OF_RANGE` with every other unencodable width.
- **PCM frame-sync formats.** `ALP_I2S_FMT_PCM_SHORT`/`_PCM_LONG` return
  `ALP_ERR_NOSUPPORT`: `TDM_CFG1` has no field expressing a short- or
  long-frame-sync PCM frame, so there is nothing to write, and framing them
  silently as I2S would be worse than refusing. `ALP_I2S_FMT_RIGHT_JUSTIFIED`
  *is* now supported — `TDM_CFG1.RX_JUSTIFY` (bit 6, Table 7-109 p.69) encodes
  exactly that.
- **Widening the interrupt masks.** `INT_MASK0..3` keep their reset values,
  which already leave over-temperature, over-current, VBAT brown-out, VBAT POR
  and speaker open/short load unmasked (§7.5.28–§7.5.31, p.77–80). TDM clock
  error is masked at reset and a caller who wants it on the pin must write
  `INT_MASK0` directly.

### Testing

`tests/zephyr/chips/src/fake_tas2563.c` is a new i2c-emul target that models
the book/page paging (§7.3.10, p.34, including BOOK being reachable only from
page 0), seeds the datasheet POR values, models the self-clearing
`CLR_INTP_LTCH`, and keeps an ordered write log so a test can assert write
*sequence* rather than only end state. 17 ZTests in `test_audio.c` use it;
each was verified by mutation (break the datasheet fact, watch the test go
red, restore, watch it go green) — 24 mutations, none surviving.

**Still unproven.** The header's `[UNTESTED]` status is unchanged and stays
accurate: no speaker is connected to the bench, this driver has never run on
silicon, and nothing above has been heard or measured. Every claim is a
datasheet claim plus a test against a model built from the same datasheet.
