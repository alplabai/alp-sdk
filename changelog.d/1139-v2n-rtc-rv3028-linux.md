### Fixed — the V2N-family SoC RTC has no crystal fitted; Linux now uses the on-module RV-3028-C7 instead (#1139)

Bench-proven 2026-09-24 on an E1M-V2N-family SoM (E1M-X-EVK, image
som-0.2.0, kernel 6.1.141-cip43): `rtc-rtca3 11c00800.rtc: error
-ETIMEDOUT: Failed to setup the RTC!`, then `probe of 11c00800.rtc failed
with error -110` on every boot. Root cause: RTXIN/RTXOUT has no 32.768 kHz
crystal fitted on the E1M V2N-family SoM (schematic crystal DNP) — the
SoC RTC was never going to probe. `docs/soms/v2n.md`'s catalogue always
carried the real RTC (a Micro Crystal RV-3028-C7 at I2C `0x52` on
BRD_I2C), but nothing on the Linux side used it: `# CONFIG_RTC_DRV_RV3028
is not set` in the kernel config left `/sys/class/rtc` empty, and the
carrier dtsi separately re-enabled the crystal-less SoC RTC anyway
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`
previously carried `&rtxin_clk { clock-frequency = <32768>; };` and
`` &rtc { status = "okay"; }; `` — ported from the Renesas EVK reference
dts, which does populate the crystal). Also stale:
alp-sdk#1139's own current-state line, "RTC (internal) — `&rtc` okay on
A55".

Fixed at the SoM layer, not the carrier: `` &rtc { status = "disabled";
}; `` now lives in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi` (that
line's own state is superseded a few hours later, see the note below --
no line-number citation here since it no longer says "disabled"), with
the carrier's conflicting override removed (SoM dtsi composes before the
carrier dtsi, so leaving the carrier's `"okay"` in place would have
silently won). A new `rtc@52` node under the existing `&i2c8` block,
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi:302`
("rv3028: rtc@52 {"), binds `compatible = "microcrystal,rv3028"`, and a
new `aliases { rtc0 = &rv3028; };` makes it `/dev/rtc0`.
`CONFIG_RTC_DRV_RV3028=y` is now built in,
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/rv3028-rtc.cfg:7`
("CONFIG_RTC_DRV_RV3028=y"), so `hwclock` has a working RTC at early
boot, not racing module load order. No `trickle-resistor-ohms` is set — whether
the RV-3028-C7's VBACKUP net carries a backup cap/coin cell on this SoM
is not confirmed on the schematic. RTC_ALARM (INT) is not wired to an
`interrupts` property either; the SoC-side pin/mux isn't confirmed yet —
both are follow-ups, not guessed here.

**Maintainer decision:** CA55/Linux is the sole master, in `a55_boot`
mode, of the *whole* RIIC8/BRD_I2C bus — every device on it (the RTC,
DA9292, the GD32 I2C slave, TMP112, the clock generator), not just the
RTC this fix touches — the CM33 does not master RIIC8 outside its
`cm33_boot` pre-handoff window. (Superseded in part by
`changelog.d/2045-cm33-boot-deepx-rail.md` in this same release, which
adds that qualified `cm33_boot` window; never concurrent with the A55, so
no contradiction, but "must not master RIIC8 at all" is no longer true
unqualified.)
`metadata/e1m_modules/v2n/core-ownership.yaml` (`core: "a55"` on
`RIIC8_SCL8`/`RIIC8_SDA8`) records that. The rest of the reconciliation —
`supervisor-links.yaml`'s `brd_i2c` link, the generated CM33 Zephyr
board's `i2c8`/`alp-i2c0` pinctrl, `src/zephyr/v2n_power_mgmt.c`'s DA9292
bring-up, `examples/v2n/v2n-rtc-multi-alarm`, and the clock-ownership
kernel patch's `riic_8_ckm` hunk — lands in #2045.

**Superseded same day:** the `&rtc { status = "disabled"; };` cited
above was itself corrected to `"okay"` a few hours later, once #2045
bench work found RTXIN's actual reference (fed by the on-module clock
generator, not a discrete crystal) and a runtime fix for it — see the
later `#2045` fragment (clock-generator fixup). The RV-3028-C7 stays
the RTC of record (`rtc0` alias, unchanged); the SoC RTC is a second,
now-also-enabled timebase.
