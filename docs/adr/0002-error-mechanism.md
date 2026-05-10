# 0002. `alp_last_error()` + compile-time SoC capability validation

Status: Accepted
Date: 2026-05-10

## Context

Hardware capabilities differ between SoMs.  An Alif Ensemble E3 ships
a 24-bit ADC plus three 12-bit ADCs; NXP i.MX 93 tops out at 12 bits;
Renesas RZ/V2N has 24 ADC channels but at 12 bits.  Apps that declare
a 16-bit ADC need to fail predictably when run on a SoC that can't
satisfy them.

Pre-this-ADR the SDK's error mechanism conflated all open-time failure
cases into a single signal: `alp_*_open` returns NULL.  Apps couldn't
distinguish:

- `bus_id` out of range (programmer error)
- DT alias unset (board-bring-up issue)
- Pool exhausted (handle quota too low)
- Config exceeds the SoC's hardware caps (the canonical
  "16-bit ADC on a 12-bit SoC" case)
- Zephyr device not ready (transient)

The studio catches most of this at codegen time, but hand-written
firmware that bypasses the studio had no diagnostic path.

## Decision

Three-layer error mechanism:

1. **Studio codegen** is the *first* line of defense.  The studio
   reads `metadata/socs/<vendor>/<family>/<part>.json` and rejects
   block configurations that exceed the active SoC's documented
   caps **before** codegen runs.  This stays the cheapest layer.

2. **SDK runtime `*_open`** validates config against a compile-time
   capability table:
   - `scripts/gen_soc_caps.py` reads `metadata/socs/**.json` and
     emits `include/alp/soc_caps.h` containing per-SoC `#define`s
     gated by Kconfig (`CONFIG_ALP_SOC_<VENDOR>_<FAMILY>_<PART>`).
   - The studio's generated build selects the active SoC; the
     `ALP_SOC_*_COUNT` / `ALP_SOC_*_MAX_*` macros activate.
   - Each `alp_*_open` checks the config against the matching
     macros.  Out-of-range configs return NULL with last_error
     set to `ALP_ERR_OUT_OF_RANGE`.

3. **`alp_last_error()`** — a thread-local accessor that returns the
   reason for the most recent failed open.  Internal helpers
   (`alp_z_set_last_error`, `alp_z_clear_last_error`) live in
   `src/zephyr/last_error.c`; the public read accessor is in
   `<alp/peripheral.h>`.

A new status code, `ALP_ERR_OUT_OF_RANGE` (= -8), distinguishes
"config exceeds hardware caps" from `ALP_ERR_INVAL` (programmer
error) and `ALP_ERR_NOSUPPORT` (Zephyr returned `-ENOTSUP`).

## Alternatives

**A. Change `*_open` to return `alp_status_t` and write the handle to
an out-pointer:** `alp_adc_open(cfg, &handle)`.  Cleanest API but
breaks the v0.1 ABI snapshot we just shipped.  Rejected because the
v0.1 contract is meant to be add-only; adding `alp_last_error()`
preserves binary compatibility.

**B. Global last-error (errno-style).**  Rejected because the SDK is
expected to be safe under multi-threaded use.  Concurrent open()s
on different threads would clobber each other's diagnostic.

**C. Pure runtime checking against Zephyr's reported device caps,
without compile-time SoC tables.**  Rejected because the runtime
device only knows what's wired in DT — it can't catch a 16-bit
request when the DT happens to be configured for 12 bits but the
studio's resolution selection would have rejected it.  The
compile-time SoC cap is the authoritative reference.

**D. Per-SoC Kconfig fragments hand-written in `zephyr/Kconfig.soc`.**
Rejected because it duplicates `metadata/socs/*.json` and decays
out of sync.  Generating from the metadata files is the
single-source-of-truth path.

## Consequences

**Good:**
- Apps can distinguish failure modes with one extra call:
  `alp_*_open` returns NULL → `alp_last_error()` returns the reason.
- The 16-bit-ADC-on-12-bit-SoC case fails at open(), not at
  read().  The error is precise (`ALP_ERR_OUT_OF_RANGE`).
- Capability data lives in one place (`metadata/socs/*.json`) and
  flows automatically to the SDK via the generator.
- Multi-threaded safety is preserved.

**Bad / costs:**
- The Kconfig SoC selection is one more thing the studio's build
  template has to set.  Without it, validation is permissive
  (default is `UINT16_MAX`).
- Adding a new capability field requires updating `gen_soc_caps.py`
  *and* the metadata schema.  Schema-then-generator is the rule.
- The thread-local storage adds a small per-thread fixed cost
  (4 bytes on 32-bit targets).  Negligible.

## Open follow-ups

- A future ADR will cover how the studio actually selects the SoC
  Kconfig token in its generated `prj.conf`.
- The same validation pattern should retrofit to the v0.1
  peripherals (I2C/SPI/UART/GPIO).  Tracked as a v0.3 item.

## See also

- `scripts/gen_soc_caps.py` — the generator.
- `include/alp/soc_caps.h` — the generated header.
- `src/zephyr/last_error.c` — the thread-local store.
- `src/zephyr/peripheral_adc.c` — the canonical example of
  validation-at-open.
