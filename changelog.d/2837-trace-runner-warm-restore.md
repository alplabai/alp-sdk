### Fixed — the trace-runner keeps its volume and brightness across an HE-only warm reset

When the I2C1 handover reports a warm boot, `tr_vol_he_boot()` and `tr_knob_boot()` restore the tagged `vol` / `bl` records (clamped to the build's limits, the backlight re-applied to the LED, a mute kept) instead of publishing the 30 % defaults; a cold boot and a garbage word behave as before.
