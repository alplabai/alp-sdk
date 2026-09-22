### Documented — OV5647 bring-up root-caused to a module fault, plus two upstream driver defects (#2248)

The InnoMaker CAM-OV5647 on the E1M-EVK's J5 connector was root-caused on an
E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2. With the sensor in
mainline's running-parked state, in which all three MIPI lanes should idle at
LP-11, `CSI_PHY_STOPSTATE` sampled 50 times at 20 ms reads `DATA_0` 50/50 and
`DATA_1` / `CLK` 0/50 — a fault in the module under test, not in the SDK. A
marginal LP swing would have made `DATA_0` flicker; it did not. The shared
physical path is exonerated by interleaving an OV9281 that reached
`CSI_PHY_STOPSTATE` = `0x00010003` with a CRC-verified frame on the same cable,
P/N-crossing adapter and connector between two identical OV5647 failures.

Two defects in the vendored upstream-pending driver were found along the way and
are real regardless of that module. Both are now fixed in-tree and verified on
silicon, as an authorized divergence from the verbatim-backport rule in
`zephyr/drivers/video/ov5647.c`'s header; that header now warns that the fixes
must be confirmed present upstream before the vendored copy is retired.

- The driver never performed mainline's "coax lanes into LP-11" park, so in
  software standby the sensor presented no LP-11 and any receiver that gates on
  Stop-state before stream start could not open it, even with healthy silicon.
- `PIXEL_RATE` was derived from a datasheet fps figure rather than the PLL the
  driver actually leaves programmed, understating the link rate by 2.1x and
  selecting the wrong D-PHY frequency bin.

`docs/camera-shields.md` records the measurements, the eliminations and both
defects. The OV5647 shield remains bench-attempted and BLOCKED, not verified.
