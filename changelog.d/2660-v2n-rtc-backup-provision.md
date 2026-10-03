### Added -- provisioning sets the V2N RTC time and enables its backup switchover and trickle charge (#2660)

The RV-3028-C7 on the E1M-V2N/V2M SoM shipped with no time and with backup
switchover and trickle charge off (register `0x37` = `0x10`). The SoM device
tree leaves `trickle-resistor-ohms` unset: trickle charge is a carrier decision
(a rechargeable supercap only, never a primary lithium cell), and the property
makes `rtc-rv3028` rewrite the configuration EEPROM at every boot. A new `rtc_set` provisioning step
sets the system time from the provisioning host (UTC), runs `hwclock -w`, reads
it back, and enables level-mode backup switchover through the driver's
`RTC_PARAM_SET`. The census records `rtc_backup_switch_mode` and `rtc_trickle`;
`rtc_time_set`, `rtc_backup_mode` and the new `rtc_trickle` functional checks
(expected trickle `"off"` unless the carrier sets a resistor) now block shipping. Bit meanings are from the RV-3028-C7 Application Manual
Rev. 1.4. Bench verification is still pending.
