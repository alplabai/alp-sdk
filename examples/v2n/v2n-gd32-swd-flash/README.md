# v2n-gd32-swd-flash

Demonstrate the host-driven SWD bit-bang controller
([`chips/gd32_swd/`](../../../chips/gd32_swd/)) by attaching from the
Renesas RZ/V2N to the on-module GD32G553 over three GPIOs
(SWDIO + SWCLK + optional NRST), reading the SW-DP IDCODE, halting
the Cortex-M33, erasing a flash sector, writing + verifying a
scratch pattern, and resetting.

This is the **recovery / first-flash path** documented in
[`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) §10
Path B + [`docs/bring-up-v2n.md`](../../../docs/bring-up-v2n.md) §2b.
It does not depend on an external probe and stays available even
when the application-bootloader OTA path is unreachable (corrupt
bridge image, factory first-flash, dev-board bring-up).

## What it shows

1. Opening three `alp_gpio_t` handles for SWDIO, SWCLK, NRST.
2. `gd32_swd_init` configures SWDIO + SWCLK as outputs at the SWD
   idle state.
3. `gd32_swd_connect` line-resets the link, performs the
   JTAG-to-SWD switch sequence, reads DPIDR.  Logs whether the
   IDCODE matches the expected `0x6BA02477` (Cortex-M33 r0p1
   SW-DPv2 — the GD32G553's documented value).
4. `gd32_swd_halt` puts the Cortex-M33 into debug-halt via DHCSR
   DBGKEY + C_HALT.
5. `gd32_swd_flash_erase` erases the enclosing 2 KiB sector.
6. `gd32_swd_flash_write` programs a 64-byte ramp pattern.
7. `gd32_swd_flash_verify` reads it back via AHB-AP memory reads
   and reports the comparison.
8. `gd32_swd_reset_and_run` releases the core via HW NRST pulse
   (when wired) or AIRCR.SYSRESETREQ otherwise.

## Pin model + carrier dependency

The SWD pin assignments on V2N are **TBD** in the schematic
(per `metadata/chips/gd32_swd.yaml`).  The example uses
studio-resolved pin ids — `alp_gpio_open` returns NULL if the
carrier preset hasn't routed the lines yet, and the example exits
cleanly with a log message rather than wedging the host.

If your carrier wires the SWD lines to non-default pads, override
the three `pin_id` constants at the top of `src/main.c` (or extend
`board.yaml` with the studio routing for your custom pad mapping).

## Safety

The example writes to **the last 2 KiB sector** of the GD32G553's
512 KB flash (`0x0807F800..0x0807FFFF`).  If your bridge firmware
happens to live in that sector the write will trash it; reflash a
fresh bridge image afterwards via either an external probe or this
same SWD driver pointing at the right address.

Adjust `WRITE_ADDR` and `WRITE_BYTES` for a different target
sector; the driver rounds out to sector boundaries automatically.

## Expected output (real silicon)

```
[swd] connected -- IDCODE = 0x6BA02477 (expected 0x6BA02477)
[swd] target halted
[swd] flash_verify -> 0 (OK)
[swd] target reset + running
[swd] done
```

## Expected output (carrier hasn't routed SWD yet)

```
[swd] alp_gpio_open failed (SWDIO + SWCLK required); carrier may not have routed SWD lines yet
[swd] done
```

## See also

* [`<alp/chips/gd32_swd.h>`](../../../include/alp/chips/gd32_swd.h)
  — driver header.
* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) §10
  — protocol-level reservation of the SWD recovery path.
* [`docs/bring-up-v2n.md`](../../../docs/bring-up-v2n.md) §2b
  — bench bring-up procedure for the recovery path.
