# 0002. `alp_last_error()` + compile-time SoC capability validation

Status: Accepted — see **Amendment** below (2026-08-27): records the
`ALP_ERR_NOT_READY` / `ALP_ERR_INVAL` convention measured across the
dispatch layer and corrects a scope misreading of the "programmer error"
parenthetical. The decision below is otherwise unchanged.
Date: 2026-05-10

## Amendment (2026-08-27 — record the ALP_ERR_NOT_READY / ALP_ERR_INVAL convention measured across the dispatch layer; correct a scope misreading)

Issue #1646: ~115 sites across 29 files (27 `src/*_dispatch.c` plus
`src/i2c_regfile.c` and `src/yocto/inference_yocto.c`) do
`if (h == NULL || !alp_handle_op_enter(...)) return ALP_ERR_NOT_READY;` for
a NULL primary handle. Read against this ADR's "programmer error"
parenthetical below, that looked like the code violating the ADR. It is
not — see the scope correction first.

**Scope correction.** This ADR's subject is open-time diagnosis:
`alp_last_error()` plus compile-time SoC capability validation, both
scoped to `alp_*_open()`. The "programmer error" parenthetical spans two
sections, not one: the Context section's list item "`bus_id` out of range
(programmer error)", and the Decision section's closing sentence "A new
status code, `ALP_ERR_OUT_OF_RANGE` (= -8), distinguishes 'config exceeds
hardware caps' from `ALP_ERR_INVAL` (programmer error) and
`ALP_ERR_NOSUPPORT` (Zephyr returned `-ENOTSUP`)." Together they contrast
three *open()*-time codes — `ALP_ERR_OUT_OF_RANGE` vs `ALP_ERR_INVAL`
(programmer error) vs `ALP_ERR_NOSUPPORT` (Zephyr `-ENOTSUP`) — they do not
define `ALP_ERR_INVAL` for the whole public API. Inside that scope
(`open()`) the code honours the contrast fully. The ~115 non-open sites
above are simply outside what this ADR ever governed. (Quoted rather than
line-cited: this amendment's own insertion already shifted every line
number below it once — a line-number citation into a document that gets
amended is a trap that recurs on the next amendment too.)

**The convention, measured from the dispatch layer** (not part of this
ADR's original decision; recorded here because #1646 asked where it's
written down):

- **`ALP_ERR_NOT_READY`** — the handle is not in a state to perform this
  operation. Covers a NULL primary handle (definitionally indistinguishable
  from one closed concurrently), a closed handle, and any handle-shaped
  argument threaded through the same `alp_handle_op_enter` lifecycle guard
  as the primary handle. The one worked example of that last case in the
  whole dispatch layer is `src/ble_dispatch.c`'s `alp_ble_gatt_notify`: its
  secondary `conn` argument (lines 422-424) returns `ALP_ERR_NOT_READY`,
  not `ALP_ERR_INVAL`, when NULL — the `:416-421` comment says why:
  `gatt_notify` dereferences `conn->state`, so a racing
  `alp_ble_disconnect(conn)` has to be blocked exactly as a racing close on
  `h` would be (issue #629's UAF fix), which makes a NULL `conn`
  indistinguishable from a concurrently-closed one, same as a NULL primary
  handle. This is the *only* dual-lifecycle-guarded call site found across
  the 29-file/115-site surface — it is the rule's sole direct evidence, not
  a widely repeated pattern.

- **`ALP_ERR_INVAL`** — a malformed argument that is *not* under a
  lifecycle guard: a NULL `cfg` at open time, or a NULL output buffer with
  a non-zero length. `include/alp/peripheral.h:1010-1011` already states
  both halves in one doc comment (`ALP_ERR_NOT_READY if @p rb is NULL or
  detached. ALP_ERR_INVAL if @p out is NULL with @p max_len > 0.`), and
  `alp_ble_gatt_notify` draws the same line internally: its
  `payload == NULL && len > 0` check returns `ALP_ERR_INVAL` two lines
  below the `conn` check that returns `ALP_ERR_NOT_READY` — one function,
  two arguments, two codes, for the stated reason.

- **`ALP_ERR_NOSUPPORT`** — a capability the backend or silicon lacks,
  including a valid form-factor identity this SoM does not populate.

**Grandfathered per-site outliers — named, not changed.** These predate the
convention above and return `ALP_ERR_INVAL`, not `ALP_ERR_NOT_READY`, for a
NULL primary handle, ahead of any op-enter/closed-handle check. Two of the
three families are pinned by tests, which argues *for* grandfathering them
rather than against — the earlier claim that "nothing consumes the
distinction" was wrong; the corrected reason still supports the same
conclusion:

- **adc** — `src/adc_dispatch.c:127-131` (`alp_adc_read_raw`) and `:152-154`
  (`alp_adc_read_uv`). The in-code comment on the first: "Preserve the
  original contract: a NULL handle OR NULL out-param is ALP_ERR_INVAL (not
  NOT_READY)." — an explicit decision to preserve the original contract,
  not an oversight. Pinned by
  `tests/unit/adc_registry/src/test_adc_registry.c:122-132`
  (`test_read_raw_inval_on_null_handle`, `test_read_uv_inval_on_null_handle`).
  The same NULL-check shape recurs in the vendor-extension entry points
  `src/backends/adc/alif_e7.c:234` and `alif_e8.c:256`
  (`alp_alif_adc_set_trigger_source`).
- **jpeg** — `src/jpeg_dispatch.c:146` (`alp_jpeg_capabilities`'s
  `h == NULL`), with the same check in its build-stub twin
  `src/common/stub/stub_jpeg.c:38`. Pinned by
  `tests/unit/jpeg_registry/src/test_jpeg_registry.c:34`.
- **update_log** — `src/update_log_dispatch.c` (lines 123, 148, 169, 179,
  190): `log == NULL` returns `ALP_ERR_INVAL` at all 5 call sites, ahead of
  the op-enter/closed-handle check that would otherwise produce
  `ALP_ERR_NOT_READY`. This diverges the *same* way adc and jpeg do (NULL
  jumps ahead of the guard); no test pins it. A closed non-NULL handle
  still returns `ALP_ERR_NOT_READY` here — the "INVAL even on a closed
  handle" behaviour some earlier draft attributed to `update_log` actually
  belongs to the Yocto backend divergence below.
- **Other `alp_<vendor>_*` ext entry points** doing the same NULL-primary-
  handle → `ALP_ERR_INVAL` check, untested: `src/backends/ext/alif/camera.c`
  (`:70,100,126`), `ext/alif/storage.c` (`:33,46`),
  `ext/deepx/inference.c` (`:46,60,71`), `ext/nxp/storage.c` (`:38,52`),
  `ext/renesas/camera.c` (`:64,103,137`), `ext/renesas/inference.c`
  (`:51,66,79`), `ext/renesas/power.c` (`:45`).

**Yocto backend divergence — i2c/spi/uart resolved by #1834, gpio resolved
by #1734 (this amendment).** This was not a handful of one-off call sites;
it was an entire backend answering the documented `ALP_ERR_NOT_READY`-on-a-
closed-handle contract differently from every Zephyr dispatcher.
`src/yocto/peripheral_{gpio,i2c,spi,uart}.c` each gated every op on
`pin/bus/port == NULL || !...->in_use` and returned `ALP_ERR_INVAL` for
*both* a NULL handle *and* a closed (non-NULL, `in_use == false`) one.

Issue #1834 splits the lifecycle check out of `peripheral_i2c.c`
(`alp_i2c_write`, `alp_i2c_read`, `alp_i2c_write_read`),
`peripheral_spi.c` (`alp_spi_transceive`, `alp_spi_write`,
`alp_spi_read`), and `peripheral_uart.c` (`alp_uart_write`,
`alp_uart_read`) so each now returns `ALP_ERR_NOT_READY` for a
NULL-or-closed handle and reserves `ALP_ERR_INVAL` for a genuinely
malformed argument, checked only once the handle itself is known good.
`tests/yocto/peripheral_{i2c,spi,uart}.c` cover the NULL-handle half of
#1834's fix; `tests/yocto/peripheral_{i2c,spi,uart}_closed_status.c` add
the sharper non-NULL, closed-handle case per class.

Issue #1734 brings `peripheral_gpio.c` into the same agreement:
`alp_gpio_configure`, `alp_gpio_write`, `alp_gpio_read`,
`alp_gpio_irq_enable` and `alp_gpio_irq_disable` each split the fused
`pin == NULL || !pin->in_use || <malformed arg>` condition into a
lifecycle check first (`ALP_ERR_NOT_READY`) and a malformed-argument
check second (`ALP_ERR_INVAL`, a NULL `level` out-param on read or a
NULL `cb` on IRQ enable), checked only once the handle is known good.
Pinned by `tests/yocto/peripheral_gpio_closed_pin_status.c`, which covers
the closed-but-non-NULL case a NULL-check-only test would miss.

All four Yocto peripheral families now match the Zephyr convention;
`alp_gpio_write(closed_pin, ...)` (and its i2c/spi/uart equivalents) no
longer answers differently depending which backend it was built against.

**Gap: signex unverified.** No signex checkout exists on the host this
amendment was written from — searched `/home`, `/opt`, `/srv`, `/mnt`,
`/data`, and a full-depth `*signex*` filename search; only this repo's own
plugin skill docs matched, not a signex repository. If a signex checkout
elsewhere renders or branches on these SDK status codes, this convention
is unverified against that consumer.

`metadata/error-catalog.json`'s `ALP_ERR_NOT_READY` summary is updated
alongside this amendment, from "Peripheral not initialised." (too narrow
— a closed handle or a read-only handle mid-operation is "not ready" but
was never uninitialised) to "The handle is not in a state to perform this
operation."

See also: issue #1646; `src/ble_dispatch.c`'s `alp_ble_gatt_read` (the
primary-handle `ALP_ERR_NOT_READY` case, line 586) and
`alp_ble_gatt_notify` (both halves, lines 407-437) as the worked examples.

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
- `src/adc_dispatch.c` — the canonical example of validation-at-open,
  with backend-specific bodies in `src/backends/adc/`.
