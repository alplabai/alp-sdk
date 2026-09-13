### Notes — attended bench run: the decoded angle never moved; raw pad sampling added to `aen-qenc-readout` to discriminate an unfitted encoder from an uncounted signal (#2037)

An attended run of `examples/aen/aen-qenc-readout` on `e1m-aen-evk-03`
(E1M-AEN803), with the maintainer turning the encoder shaft continuously for
the whole 60 s window, measured a clean stuck-at-zero: all 200 samples read
`0 deg`, 200/200 clean reads, `RESULT SKIPPED`. Reading live over SWD at four
points across the window plus once after, `UTIMER_CNTR` (`0x4800D0A0`) was
`0x00000000` every time -- the channel never counted, and it is not counting
then resetting.

Every other register checked matched the correctly-configured resting state
already documented in the driver:

- `UTIMER_CNTR_CTRL` (`0x4800D080`) read `0x00000021` (EN and CNTR_TRIG set,
  RUNNING clear). `zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c:432`
  ("So CNTR_CTRL reading 0x00000021 here") documents that as the CORRECT
  resting state for a trigger-counting channel -- do not "fix" this by
  starting the counter, which caused the withdrawn #2038 free-run.
- `UTIMER_CNTR_PTR` (`0x4800D0A4`) read `0x0000005F` (95, correct for
  counts-per-revolution 96).
- `UTIMER_FILTER_CTRL_A` (`0x4800D084`) read `0x00100101`, matching the
  devicetree filter config (filter enabled, taps `0x1`, prescaler `0x10`).
- `UTIMER_UP_1_SRC` (`0x4800D01C`) read `0x00000069` and `UTIMER_DOWN_1_SRC`
  (`0x4800D024`) read `0x00000096`, with `UP_0_SRC` (`0x4800D018`) and
  `DOWN_0_SRC` (`0x4800D020`) both `0x00000000`.
- Pinmux is right on both sides. `metadata/e1m_modules/aen/from-alif.tsv:9`
  ("ENC0_X") and `metadata/e1m_modules/aen/from-alif.tsv:61` ("ENC0_Y") map
  the carrier's `ENC0_X`/`ENC0_Y` nets to `QEC0_X_A`/`QEC0_Y_A` on
  `P3_0`/`P3_1`, which is what the example's board overlay muxes.

**Still not discriminated: whether the encoder's edges physically reach
`P3_0`/`P3_1` at all.** Every line on this path carries a pull-up, so a
fitted-at-rest encoder and an empty footprint read identically at rest, and
this bench run could not tell them apart from the decoded angle alone.

`examples/aen/aen-qenc-readout/src/main.c` now samples the raw pad levels of
`P3_0`/`P3_1` directly off the GPIO3 controller's external-port register
alongside every decoded-angle sample,
`examples/aen/aen-qenc-readout/src/main.c:104` (`0x49003050u`), and prints
the pad-control (mux + electrical) register values for both pins once at
start-up, `examples/aen/aen-qenc-readout/src/main.c:121`
(`AEN_PINCTRL_BASE`), so a missing input-enable is visible without a
debugger. The verdict now distinguishes three outcomes instead of two: pads
and angle both changing means the decoder works; pads changing but the angle
staying put is the #2037 defect reproduced under a hand,
`examples/aen/aen-qenc-readout/src/main.c:302`
("signal reaches the SoC pins and the UTIMER QEC0 channel does not count it");
pads never changing at all means nothing reached the pads, which cannot tell
an absent or unfitted encoder from a broken one.

**Open**: the next attended run with this build settles which of those three
it is. No decoder configuration, mux, or counter-start behaviour changed
here.
