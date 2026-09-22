### Documented — OV5647 bring-up root-caused to a module fault, plus one real and one disproven upstream driver defect (#2248)

The InnoMaker CAM-OV5647 on the E1M-EVK's J5 connector was root-caused on an
E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2. With the sensor in
mainline's running-parked state, in which all three MIPI lanes should idle at
LP-11, `CSI_PHY_STOPSTATE` sampled 50 times at 20 ms reads `DATA_0` 50/50 and
`DATA_1` / `CLK` 0/50 — a fault in the module under test, not in the SDK. A
marginal LP swing would have made `DATA_0` flicker; it did not. The shared
physical path is exonerated by interleaving an OV9281 that reached
`CSI_PHY_STOPSTATE` = `0x00010003` with a CRC-verified frame on the same cable,
P/N-crossing adapter and connector between two identical OV5647 failures.

Two defects in the vendored upstream-pending driver were claimed along the
way. Only one is real; it is now fixed in-tree and verified on silicon, as an
authorized divergence from the verbatim-backport rule in
`zephyr/drivers/video/ov5647.c`'s header, which now warns that the fix must be
confirmed present upstream before the vendored copy is retired. The other was
investigated and disproven, and a follow-up "fix" for it was reverted.

- FIXED: the driver never performed mainline's "coax lanes into LP-11" park,
  so in software standby the sensor presented no LP-11 and any receiver that
  gates on Stop-state before stream start could not open it, even with
  healthy silicon.
- DISPROVEN: `PIXEL_RATE` was claimed to be off by 2.1x against the PLL the
  driver actually leaves programmed, and to select the wrong D-PHY frequency
  bin. Checked against mainline Linux's own driver, the pixel rate was
  approximately correct (within ~5%), the D-PHY bin was already right, and
  the attempted PLL-derived correction was reverted because it doubled the
  programmed frame length instead of fixing the link rate.

`docs/camera-shields.md` records the measurements, the eliminations, the real
fix and the disproven claim. The OV5647 shield remains bench-attempted and
BLOCKED, not verified.
