### Added — the E1M-AEN801's on-module housekeeping I2C bus (BRD_I2C) is documented, and the "not usable as built" verdict on it is withdrawn (#1848, #1814)

[`docs/soms/aen.md`](docs/soms/aen.md) gains **"On-module housekeeping I2C
(BRD_I2C)"** — the customer-facing reference for the two parts every AEN801
carries and nothing on the carrier is needed to use: the Micro Crystal
**RV-3028-C7** RTC at `0x52` and the TI **TMP112** thermometer at `0x48`
(design address, ADD0 strapped to GND), both fitted. The OPTIGA Trust M at
`0x30` is DNP on this batch and must not appear in a shipped devicetree. The
bus is **SoC I2C0 function C** on `P7_0`/`P7_1`, isolated from the
I2C2/EEPROM segment (`R93`/`R94` DNP), so the manifest EEPROM is not on it.
Both parts are driven by upstream Zephyr — `CONFIG_RTC_RV3028` and
`CONFIG_TMP112`, both `default y` off their devicetree nodes — with
`examples/aen/aen-rtc-alarm` and `examples/aen/aen-temp-sensor` as the worked
examples, and the bus, the child nodes and the `alp-i2c2` / `rtc` /
`ambient-temp0` aliases generated into the board devicetree from
`metadata/e1m_modules/aen/on-module-links.yaml`, and the RTC alarm path documented end to end (`/INT` →
`P15_0` → LPGPIO bit 0 → IRQ 171, open-drain active low, `R98` 100 kΩ to
`+1V8` fitted).

Four limitations are stated up front rather than left to be discovered on a
bench, because each of them surprises people:

* **The RTC does not keep time across a power cycle.** `VDD_BAT`/`VBACKUP`
  has no supply fitted on this batch (`R4`/`R68` both DNP) — no backup cell,
  no trickle source. Hence `backup-switch-mode = "disabled"` (the binding
  requires the property) and no `trickle-resistor-ohms`. Persistence is a
  populate change, not a firmware setting.
* **`RTC_CLKOUT` reaches only the E1M edge connector (pin `AH16`)**, not the
  SoC, so it is a carrier-designer's clock and firmware cannot consume it.
* **`MODULE_STBY`/`EVI` (edge pin `O2`) is a carrier-driven event input.**
* **One bench module answered at `0x40` instead of `0x48`.** `0x40` is not a
  legal TMP112 address, and that part fingerprinted as a genuine TMP112 on
  three of three registers (`CONFIG` `0x60a0`, `T_LOW` `0x4b00`, `T_HIGH`
  `0x5000` — the datasheet power-on defaults), so its ADD0 is not actually at
  GND: check **U20 pin 3 continuity to GND** on that unit. A per-module
  defect, so the devicetree keeps the design address `0x48` and the examples,
  not the DT, tolerate and report the anomaly.

### Fixed — a bench verdict measured on r1 hardware was recorded as a property of the bus, and said it was unusable (#1848)

`zephyr/dts/alif/ensemble_e8_peripherals.dtsi`'s BRD_I2C/`i2c0` comments
carried a "BENCH-SETTLED 2026-08-31 … this bus is NOT usable as built …
needs `R93`/`R94` stuffed" block, and `docs/bring-up-aen.md` §5.1,
`docs/glossary.md`, `src/backends/rtc/lprtc_calendar_shim.c` and
`examples/aen/aen-brd-i2c-scan` all repeated it. That run was taken on an
**r1** module, where the RTC and TMP112 sit on LPI2C0 (`P7_4`/`P7_5`) and
**nothing is attached to `P7_0`/`P7_1`** — so nothing could have ACKed there
whatever the pad biasing, and the run characterised two floating pins rather
than a populated bus. Its open-drain-vs-push-pull inference came from the
same floating net.

Bench-settled again on 2026-09-05 on **2626-R2** silicon (Flow A, cold-cycle
proven), the revision that actually routes BRD_I2C to I2C0: the bus works on
the SoC's **internal pull-up alone**. The RV-3028-C7 ACKs at `0x52` (ID
register `0x28` = `0x44`, seconds `0x01` → `0x02`, so the oscillator runs),
the TMP112 fingerprints correctly and reads 28.062 °C, and every
non-response is a clean `rc=-5` (`-EIO`) NACK — **zero `-ETIMEDOUT`, zero
`User Abort on i2c@49010000`** across the whole run. `R93`/`R94` stay DNP and
are not needed.

The r1 measurement is kept as history everywhere it appeared, marked with
why its conclusion does not carry, rather than deleted. The RTC shim's own
comment is corrected too: nothing electrical blocks `rv3028c7` from backing
the AEN calendar any more, only a deliberate backend-selection change
(#1814) — and that is not a free upgrade, since with no `VBACKUP` supply the
external RTC loses the time on a cold boot exactly as the LPRTC does.
