### Added — `aen-evk-demo`: real sound-out/PDM-in phase, honest screen-DSI evidence (#2037)

Phase 11 (sound out → PDM in) was a blind `SKIPPED` stub deferred for a
hardware-safety reason: both on-EVK TAS2563 amps can drive ~10 W peak, and a
rushed first volume-ramp write was judged not something that should run for
the first time unverified. It is now a real phase, gated on two independent
safety levers set to their quietest value *before* either amp is ever told
to switch — the amp's own `tas2563_set_amp_level(TAS2563_AMP_LEVEL_MIN)`
over I2C, and `alp_audio_out_set_volume()` opened and started at a small
fraction of unity before `tas2563_set_mode(ACTIVE)` is ever called — with
the volume only ramped up afterwards, capped well below half of unity, for
one short tone. Both amps (U27 `0x4D`, U28 `0x4E`) are brought up, both
fault words are read over I2C before and after (a
`TAS2563_FAULT_SHUTDOWN_CAUSES` bit set after the tone is a real `FAIL`,
never swallowed), and the amplifier is idle-restored on every exit path,
including every failure — the same discipline phase 6 (RGB LED) uses, not
phase 9 (SD mux)'s deliberate non-restore.

The verdict is a PDM-energy correlation: a room-noise baseline is captured
before the tone starts, a second capture runs interleaved with the tone
itself, and `sound_pdm_capture_correlated()` (new, `sound_verdict.h`,
unit-tested in `tests/zephyr/chips`) requires the during-tone energy to
clear both a 2x ratio over baseline and an absolute floor. This is
deliberately an energy check, not a frequency or amplitude one — it proves
the loop closed, not what played.

The I2S mux ENABLE (E1M `IO8` → CC3501E `GPIO_30`) and SELECT (`IO13` →
`GPIO_13`) turned out to be CC3501E-proxied pads, the same mechanism phase 9
already uses for the SD mux — contradicting `examples/aen/aen-i2s-amp-alif`'s
README and `metadata/boards/e1m-evk.yaml`'s `EVK_PIN_I2S_MUX_EN` doc string,
both of which say "Alif side P7.1". `metadata/e1m_modules/E1M-AEN801.yaml`'s
`pad_routes` — what the GPIO dispatcher actually resolves through — says
otherwise for both pins, and is what this phase (and
`src/cc3501e_gpio_routes.c`) trusts. Neither of those two other files is
touched here; they are out of this change's scope.

Phase 12 (screen/DSI) is not a blind stub either now, though it still
reports `SKIPPED` — no panel is on this bench. It calls `alp_display_open()`
for real and prints the grounded reason nothing can resolve: this SoC's
peripherals dtsi declares only the shared CSI/DSI D-PHY node
(`d-phy@49033000`), itself `status = "disabled"` with a flagged placeholder
clock, and there is no separate DSI protocol-layer host-controller node or
driver anywhere in this tree — checked, not assumed. `LCD_PWR_EN`/`LCD_RST`
(TCAL9538 `P0`/`P1`) are still never driven, for a stronger reason than
phase 4's: there is no controller downstream that could do anything with a
powered panel.

**Image size**: FLASH grew from 210424 B (80.27%) to 225704 B (86.10%) of
the 256 KB ITCM region — roughly 15 KB for both phases combined, almost all
of it phase 11 (the I2S3 + HP-PDM Zephyr drivers, the TAS2563 driver body,
and `<alp/audio.h>`'s backend). ~36 KB of headroom remains; phase 14 (NPU)
stays a stub for the same reason as before (ITCM, not hardware or driver
availability) and this change narrows the margin it has to work with.

**What this change does not do, on purpose**: it does not touch
`metadata/boards/e1m-evk.yaml`'s stale `EVK_PIN_I2S_MUX_EN` doc string or
`aen-i2s-amp-alif`'s README, both flagged above but out of scope for an
`examples/aen/aen-evk-demo/`-only change.
