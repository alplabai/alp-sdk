### Changed — Trace Runner audio is louder on the 8 ohm 5 W speakers (#2883)

The Trace Runner sound backend now runs at unity volume and a new amp-level option sets the TAS2563 playback level, default 0x10 (4.97 W full scale into 8 ohm, the 5 W speaker rating). Demo runs no longer leave the live wire humming. Bench 2026-10-15, E1M-AEN803 2026W36-0002: PB_CFG1 reads back 0x20 on both amps, and a 5-minute soak at 100 % backlight plus 100 % volume shows 0 SN65DSI83 bridge losses, no TAS2563 OC/OT/brown-out/limiter latches, 0 audio underruns, DPS input peak 0.990 A at 16 V.
