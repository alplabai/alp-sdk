### Added — `aen-rtc-tick-probe` gained a core-identity + CGU-register + SysTick/RTC-ratio section that runs before every other test (#2037)

A bench run of this app's own image measured the SysTick at 400.0 MHz
against a build that declares 160 MHz — derived from `k_cycle_get_32()`
wrapping once across a 12 s RTC window (4.80e9 nominal cycles, minus one
`2^32` wrap, lands within 0.79 ms of the measured total). 160 MHz is the
M55-HE's documented rate; 400 MHz is not on the HE's clock menu at all
(HWRM AHRM0012NDA v0.3 S8.3.2.3.4 — `ESCLK_SEL[ES1_PLL]` only ever selects
80 or 160 MHz) — it is the M55-HP's `ES0_PLL` default. So either this image
is executing on the HP while believed to be on the HE, or this HE unit
genuinely self-clocks at 400 MHz, and nothing in the tree previously
distinguished the two.

Three new sub-tests run first, before TEST A/B:

- **TEST 0a — core identity via DTCM global-alias readback.** Writes a
  magic value to a local DTCM variable and reads it back through both
  cores' "external access" global aliases (HE `0x5880_0000`+offset, HP
  `0x5080_0000`+offset). The obvious-looking premise — "whichever alias
  shows the magic is your core" — does not hold once HWRM Table 10-2
  (p.326) is actually checked: a core's *own* external alias is
  access-blocked for that same core, so **the alias that faults
  identifies the executing core**, the inverse of a first read of the two
  addresses. This test checks both signals (which read faults, which
  shows the magic) and reports whichever the data supports, or
  `INCONCLUSIVE` with the raw values if neither pattern matches. A read
  that might fault runs in a disposable worker thread — Zephyr's own
  fault path (`z_fatal_error()` → `k_thread_abort()` on the current
  thread once `k_sys_fatal_error_handler()` returns) kills only that
  thread, not `main()`, so a fault here is reported, not fatal to the
  rest of the run.
- **TEST 0b — CGU registers, read-only.** `PLL_LOCK_CTRL` (`0x1A602004`),
  `PLL_CLK_SEL` (`0x1A602008`, bit 20 `ES1` = HE's oscillator-vs-PLL
  select), `ESCLK_SEL` (`0x1A602010`, `ES1_PLL`/`ES1_OSC`/`ES0_PLL`
  rate-select fields), and `CLK_ENA` (`0x1A602014`) — every address and
  bit position checked against HWRM S8.3.2.3.3/.4 directly. Never writes
  any CGU register, and never writes `CLK_ENA` in particular.
- **TEST 0c — SysTick/RTC ratio at ~2 s / ~10 s / ~60 s after boot**,
  printed alongside `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`, reusing this
  app's own `read_burst()`/`burst_unix_time()`. Guards the exact wrap
  that cost real analysis time in the original investigation —
  `k_cycle_get_32()` wraps every ~10.7 s at 400 MHz — using
  `k_cycle_get_64()` where the platform provides a real 64-bit counter,
  and otherwise folding every 32-bit wrap in explicitly via a tracker
  polled more often than the fastest plausible wrap period.

`CONFIG_I2C_LOG_LEVEL_DBG` is now `CONFIG_I2C_LOG_LEVEL_ERR`: this app's
first bench run wrapped its console and destroyed a result because the
DesignWare I2C driver at `DBG` level floods even a 64 KiB ring buffer.
`ERR` still carries the NACK-vs-timeout distinction the sibling probes
rely on. No existing test's behaviour changes — TEST A/B are untouched.
