<!-- Last verified: 2026-08-17 against dev (#1512).  The flow itself is
     still UNPROVEN on silicon -- see "Status of this flow" below; this date
     records a documentation review, not a bench run. -->

# 07 -- Recovering a bricked bridge

When the GD32G553 supervisor MCU's firmware goes bad -- corrupt
image, factory-fresh chip, dev-board first-flash -- the
application-bootloader OTA path (`CMD_OTA_*` opcodes) can't help
because the bridge itself doesn't answer.

The SDK ships **two** recovery paths.  This tutorial walks the
host-driven SWD bit-bang controller (`chips/gd32_swd/`); for the
external-probe alternative see
[`docs/bring-up-v2n.md`](../bring-up-v2n.md) §2a.

## What you need

* SWDIO (`P70`) + SWCLK (`P71`) + **NRST (`P74`, mandatory)** routed from
  the Renesas RZ/V2N to the GD32's SWD pads.  The 2026-05-12 hardware
  decision committed the V2N board to this routing; the CM33 boards publish
  the three pads in their `alp,gd32-pads` devicetree node, reached through
  the reserved ids `GD32G553_PAD_ID_*` -- never by an index into the
  positional pin array (index 0 of that array is the GD32 SPI chip-select).
  The portable `alp_gpio_open()` **refuses** those ids; only the SWD driver
  (and the V2N supervisor, for ATTN) opens them, so application code passes
  no pin handles.
* A known-good bridge firmware ELF to flash.

## P71 has two roles -- why NRST comes first

`P71` is the SWD clock **and**, since bridge protocol v0.15, the bridge's
data-ready input: the GD32's `PA14` is `SWCLK` out of reset, but the GD32
drives it as the active-high **ATTN** output while the `ATTN` link feature is
granted.  The host therefore keeps `P71` an **input** (with a rising-edge
interrupt) in every boot stage and drives it as an output only here, and only
while `GD32_NRST` (`P74`) holds the GD32 in reset -- a reset clears every link
feature and returns `PA14` to SWCLK.  Recovery without `NRST` is not
supported: the driver opens `P70`/`P71`/`P74` itself and `gd32_swd_init()`
fails (`ALP_ERR_NOT_READY`) on a board that does not publish all three.

The driver does this as **connect-under-reset**:

1. `gd32_swd_init()` closes the SPI bridge link and keeps it closed (the
   V2N supervisor answers every bridge command `ALP_ERR_BUSY` and will not
   re-initialise or renegotiate until `gd32_swd_deinit()`), asserts `NRST`
   (switched to an output with an **initial low** level -- never driven high,
   not even briefly -- and failing if it cannot), and only then makes
   `P70`/`P71` outputs;
2. `NRST` stays asserted through `gd32_swd_connect()`: line reset,
   JTAG-to-SWD switch, DPIDR, debug power-up, then `DHCSR.C_DEBUGEN` and
   `DEMCR.VC_CORERESET` (halt on the reset vector) -- and only then `NRST`
   is released and `DHCSR.S_HALT` is read back: if the core did not halt,
   `NRST` is asserted again and the connect fails.  The core stops at its reset
   vector, `PA13`/`PA14` are still SWD, no application code has run, so `ATTN`
   cannot have been granted;
3. `gd32_swd_deinit()` releases `NRST`, returns `P70`/`P71` to inputs and
   ends the session; the supervisor's next init re-arms `P71` as input +
   interrupt and renegotiates the bridge from scratch.

`NRST` is open-drain on the board (shared with the primary PMIC's reset-out);
the Renesas GPIO driver has no open-drain mode, so the driver emulates it --
low to assert, input (hi-Z) to release -- and never drives the pad high.

## The flow

```c
gd32_swd_t swd;
/* No pin handles: the driver opens its own pads.  Asserts NRST FIRST. */
gd32_swd_init(&swd);

/* 1. Link up -- line reset + JTAG-to-SWD switch + DPIDR read, then halt-on-reset
 *    is armed and NRST released: the core stops at its reset vector. */
gd32_swd_connect(&swd);
if (swd.idcode != GD32_SWD_GENERIC_CM33_R0P1_IDCODE) {
    /* Log and CONTINUE -- do not abort here.  See the IDCODE
     * caveat below: whether a real GD32 matches
     * GD32_SWD_GENERIC_CM33_R0P1_IDCODE is UNKNOWN (#1369), so a
     * hard stop here could refuse exactly the board you came to
     * recover.  Match the shipped example, which logs and
     * proceeds. */
    printf("[swd] note: IDCODE != generic reference -- this is not a "
           "wrong-board signal, the reference value is unattested on "
           "a GD32 (#1369)\n");
}

/* 2. Stop the running firmware so it doesn't trash FMC concurrently. */
gd32_swd_halt(&swd);

/* 3. Erase the destination region. */
gd32_swd_flash_erase(&swd, GD32_SWD_FMC_FLASH_BASE, image_size);

/* 4. Program the new image. */
gd32_swd_flash_write(&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);

/* 5. Read back and compare. */
gd32_swd_flash_verify(&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);

/* 6. Hand control back to the chip (disarms halt-on-reset; with NRST held the
 *    pads go back to inputs, THEN NRST is released). */
gd32_swd_reset_and_run(&swd);

/* 7. End the session: P70/P71 back to inputs, bridge link allowed again. */
gd32_swd_deinit(&swd);
```

## The IDCODE caveat -- read this before you trust step 1

`GD32_SWD_GENERIC_CM33_R0P1_IDCODE` is `0x6BA02477`, and **that value has
never been measured on a GD32.** It is the generic ADIv5
expectation for a Cortex-M33 r0p1 SW-DPv2, carried over from the
part's core, not read off the part.

What the bench actually records for the V2N bench unit
(`scripts/bench/aen/bench-env.sh`) -- only the V2N CM33 DAP row below
is a measurement; the GD32 row is a claimed-but-unattested candidate
value, not a bench reading (see #1369):

| probe        | SW-DP IDR    | status                              |
|--------------|--------------|--------------------------------------|
| GD32 bridge  | `0x0BE12477` | **unattested** -- no bench transcript, no datasheet reference, no commit message |
| V2N CM33 DAP | `0x6BA02477` | bench-measured, `scripts/bench/aen/bench-env.sh` |

So `0x6BA02477` is this bench's **measured V2N CM33 DAP** value -- and
the measurement that produced it also reported `Found Cortex-M33
r0p4`, not the `r0p1` the constant's comment describes. Whether a
healthy, correctly-wired GD32 answers `0x0BE12477`, `0x6BA02477`, or
something else entirely is UNKNOWN: neither value has been read off a
GD32 with a probe attached, so a comparison against
`GD32_SWD_GENERIC_CM33_R0P1_IDCODE` proves nothing about the target
either way.

Consequences for anyone writing a recovery tool from this page:

* **A mismatch here is not evidence of mis-wiring or a wrong
  part -- and neither is a match.** Until a real GD32's IDR is
  measured, the comparison tells you almost nothing. Check the wiring
  because the link failed, not because the IDCODE differed or matched.
* **Do not turn the comparison into a hard stop**, which is what
  this tutorial used to do (#1512). Nothing shipped does:
  `gd32_swd_connect()` deliberately does not reject a mismatch
  (`chips/gd32_swd/gd32_swd.c`), and
  `examples/v2n/v2n-gd32-swd-flash/src/main.c` logs and
  continues. A production test that *wants* to refuse on a
  mismatch should opt into that explicitly, against a value it
  has measured on its own hardware.

Settling which value (if either) a real GD32 answers needs a probe on
one and is tracked at #1369 (`needs-silicon`).
`metadata/chips/gd32_swd.yaml` deliberately carries no
`target_expected_idcode` for this reason (#1440) -- same stance
`metadata/schemas/soc-spec-v1.schema.json`'s own `expect_dpidr`
guidance takes for every Alif Ensemble SoC variant: an absent key is
the correct published "unknown", not a guessed value.

## Status of this flow

`chips/gd32_swd/` is `driver_status: partial` /
`hil_silicon: untested` (`metadata/chips/gd32_swd.yaml`) -- the
packet layer, DPIDR read, halt and FMC erase/write/verify are all
coded and the V2N pad assignments are resolved (P70/P71/P74), but
**none of it has been exercised on real silicon yet.** Treat this
whole procedure as paper-correct, not proven.

## Why this works when the bridge is bricked

SWD is a hardware debug bus.  It runs *underneath* the firmware --
even a totally corrupt application can't disable the SW-DP because
the SW-DP is implemented in silicon, not in firmware.  As long as
the three GPIOs are wired and the GD32 has power, this path works.  Holding
the core in reset while the debug port is brought up is the standard
connect-under-reset technique; the debug logic is not cleared by a system
reset, so the DP answers while `NRST` is low.

## Pacing

The bit-bang controller defaults to ~1 MHz SWCLK on a Cortex-A55
at full clock.  Override via `gd32_swd_set_clock_delay()` if your
host's GPIO is much faster (a tighter spin loop on a different
silicon) or you need to slow it down for noisy boards.

## Cross-reference -- the wrong-board recovery hazard

This tutorial's IDCODE caveat table now agrees with
[`docs/gd32-bridge.md`](../gd32-bridge.md)'s "Recovering a bricked
bridge" section: both treat `0x0BE12477` as an unattested,
claimed-but-unmeasured value (see #1369), and neither presents it as
what a healthy GD32 answers. That was not always true -- this
tutorial previously stated `0x0BE12477` as the value a healthy,
correctly-wired GD32 answers, contradicting `docs/gd32-bridge.md`'s
DPIDR preflight, which refuses to accept that same value as a pass
condition. #1369 is why: neither candidate value is measured, so
neither document gets to assert one as ground truth.

**`docs/gd32-bridge.md`'s hazard section governs the recovery-flash
decision for the J-Link/external-probe path it documents** (this
tutorial's on-SoM bit-bang route, `chips/gd32_swd/` -- SWDIO/SWCLK/NRST
on P70/P71/P74, no J-Link, no cloned serial -- is unaffected by that
section's detach / `lsusb -t` / `SelectEmuBySN` procedure), because it
is the section an operator follows immediately before a J-Link write
that can reach the wrong board.  This tutorial's table reproduces
`scripts/bench/aen/bench-env.sh`'s `GD32_DPIDR` export, and that export
formerly carried its own "BENCH-VERIFIED" banner covering `GD32_DPIDR`
too; that banner cited `docs/aen-bench-bringup.md`, a document that
does not mention the GD32 at all, and is now hedged
(`scripts/bench/aen/bench-env.sh:394-397` -- "GD32_DPIDR is NOT
bench-verified ... treat it as unattested").
Whether `0x0BE12477` was ever read off a GD32 with a probe attached
remains open at #1369 and needs silicon to close, not doc surgery.

## See also

* [`<alp/chips/gd32_swd.h>`](../../include/alp/chips/gd32_swd.h)
* [`examples/v2n/v2n-gd32-swd-flash/`](../../examples/v2n/v2n-gd32-swd-flash/)
* [`docs/gd32-bridge-protocol.md`](../gd32-bridge-protocol.md) §10
  -- recovery / OTA path tree.
* [`docs/gd32-bridge.md`](../gd32-bridge.md) "Prebuilt recovery images"
  -- where the recovery binaries and their flashing guide are published.
