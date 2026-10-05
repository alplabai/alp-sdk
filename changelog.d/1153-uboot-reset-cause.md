### Added — U-Boot reports the SoC reset cause and holds the DX-M1 in reset after a watchdog reset (#1153)

After a SoC-only watchdog reset the peripherals outside the SoC reset domain
kept stale state, and the boot log did not say why the board had restarted.
`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0012-rzv2n-dev-ALP-E1M-reset-cause-and-deepx-reset-hold.patch`
makes `board_late_init()` read `CPG_ERROR_RST2` (`0x10420B40`; bit1 =
WDT_CA55, bit0 = WDT_CM33; RZ/V2N manual R01UH1071EJ0120 Rev.1.20 §4.4.4.15,
and §4.4.6.5.4: it survives an error system reset) before anything else,
print `ALP: reset cause: WDT CA55` / `WDT CM33` / `power-on or software`,
clear the WDT flags, and export the cause as env `alp_reset_cause` and
`/chosen/alp,reset-cause` (`wdt-ca55`, `wdt-cm33`, `wdt-ca55-cm33`,
`por-or-sw`). On a v2n-m1 SoM, a WDT reset also drives M1_RESET (PA6) low
for 10 ms before the existing DEEPX rail / PCIe sequence releases it.

Not done, on purpose: GD32_NRST (P74, shared with PMIC GPIO4, topology
unconfirmed), the Ethernet PHYs and the PMIC are untouched, so a WDT reset
still leaves them in their prior state. The 10 ms hold is a placeholder until
the DEEPX datasheet minimum is confirmed. Bench-verified on E1M-V2M103
(hang injection: `WDT CA55`, DX-M1 held 10 ms, back at 8.0 GT/s x2). A plain
Linux `reboot` also reports `WDT CA55`, because the kernel restarts through
the CA55 watchdog, so `por-or-sw` means power-on only. See
`docs/rzv2n-m33-secure-boot.md` ("Reset cause and watchdog-reset behaviour")
for the test steps. New HIL spec `tests/hil/v2m103-x-evk/v2m103-reset-cause.yaml`.
