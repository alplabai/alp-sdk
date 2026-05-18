<!-- Last verified: 2026-05-18 against slice-3b state. -->

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
  decision committed the V2N carrier to this routing; specific pad
  assignments are documented per-board.
* A known-good bridge firmware ELF to flash.

## The flow

```c
gd32_swd_t swd;
gd32_swd_init(&swd, swdio_pin, swclk_pin, nrst_pin);

/* 1. Link up -- line reset + JTAG-to-SWD switch + DPIDR read. */
gd32_swd_connect(&swd);
if (swd.idcode != GD32_SWD_EXPECTED_IDCODE) {
    /* 0x6BA02477 -- Cortex-M33 r0p1 SW-DPv2.  Mismatch means
     * mis-wiring or a non-G5x3 part. */
    abort();
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
