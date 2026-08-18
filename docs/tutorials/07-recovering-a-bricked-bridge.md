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

* SWDIO + SWCLK + (optional) NRST routed from a Renesas RZ/V2N
  GPIO bank to the GD32's SWD pads.  The 2026-05-12 hardware
  decision committed the V2N board to this routing; specific pad
  assignments are documented per-board.
* A known-good bridge firmware ELF to flash.

## The flow

```c
gd32_swd_t swd;
gd32_swd_init(&swd, swdio_pin, swclk_pin, nrst_pin);

/* 1. Link up -- line reset + JTAG-to-SWD switch + DPIDR read. */
gd32_swd_connect(&swd);
if (swd.idcode != GD32_SWD_EXPECTED_IDCODE) {
    /* Log and CONTINUE -- do not abort here.  See the IDCODE
     * caveat below: on this bench a healthy GD32 answers
     * 0x0BE12477, not GD32_SWD_EXPECTED_IDCODE's 0x6BA02477,
     * so a hard stop refuses exactly the board you came to
     * recover.  Match the shipped example, which warns and
     * proceeds. */
    printf("[swd] WARN: IDCODE mismatch -- got 0x%08X, expected 0x%08X\n",
           (unsigned)swd.idcode, (unsigned)GD32_SWD_EXPECTED_IDCODE);
}

/* 2. Stop the running firmware so it doesn't trash FMC concurrently. */
gd32_swd_halt(&swd);

/* 3. Erase the destination region. */
gd32_swd_flash_erase(&swd, GD32_SWD_FMC_FLASH_BASE, image_size);

/* 4. Program the new image. */
gd32_swd_flash_write(&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);

/* 5. Read back and compare. */
gd32_swd_flash_verify(&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);

/* 6. Hand control back to the chip. */
gd32_swd_reset_and_run(&swd);
```

## The IDCODE caveat -- read this before you trust step 1

`GD32_SWD_EXPECTED_IDCODE` is `0x6BA02477`, and **that value has
never been measured on a GD32.** It is the generic ADIv5
expectation for a Cortex-M33 r0p1 SW-DPv2, carried over from the
part's core, not read off the part.

What the bench actually records, both measured on place
`e1mx-v2n-m1-01` (`scripts/bench/aen/bench-env.sh`):

| probe        | SW-DP IDR    |
|--------------|--------------|
| GD32 bridge  | `0x0BE12477` |
| V2N CM33 DAP | `0x6BA02477` |

So `0x6BA02477` is this bench's **V2N CM33 DAP** -- and the
measurement that produced it also reported `Found Cortex-M33
r0p4`, not the `r0p1` the constant's comment claims. A healthy,
correctly-wired GD32 answers `0x0BE12477` and therefore *fails*
the `GD32_SWD_EXPECTED_IDCODE` comparison.

Consequences for anyone writing a recovery tool from this page:

* **A mismatch here is not evidence of mis-wiring or a wrong
  part.** Until the constant is settled against a probe on a
  GD32, the comparison tells you almost nothing. Check the wiring
  because the link failed, not because the IDCODE differed.
* **Do not turn the comparison into a hard stop**, which is what
  this tutorial used to do (#1512). Nothing shipped does:
  `gd32_swd_connect()` deliberately does not reject a mismatch
  (`chips/gd32_swd/gd32_swd.c`), and
  `examples/v2n/v2n-gd32-swd-flash/src/main.c` warns and
  continues. A production test that *wants* to refuse on a
  mismatch should opt into that explicitly, against a value it
  has measured on its own hardware.

Settling the constant needs a probe on a GD32 and is tracked at
#1440 (`needs-silicon`) with #1369.

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
the three GPIOs are wired and the GD32 has power, this path works.

## Pacing

The bit-bang controller defaults to ~1 MHz SWCLK on a Cortex-A55
at full clock.  Override via `gd32_swd_set_clock_delay()` if your
host's GPIO is much faster (a tighter spin loop on a different
silicon) or you need to slow it down for noisy boards.

## See also

* [`<alp/chips/gd32_swd.h>`](../../include/alp/chips/gd32_swd.h)
* [`examples/v2n/v2n-gd32-swd-flash/`](../../examples/v2n/v2n-gd32-swd-flash/)
* [`docs/gd32-bridge-protocol.md`](../gd32-bridge-protocol.md) §10
  -- recovery / OTA path tree.
