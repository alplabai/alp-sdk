### Fixed

- E1M-X EVK (V2N family): the TAS2563 ALSA card (`sound_card`) and `&rcar_sound` are now `disabled` (#2331). Playback on SSI1 drove the amps' SDOUT net (P46 -> `I2S0_SDI`), not their SDIN, so it produced no audio and could contend with the amps' IV-sense transmit. With both nodes off, `sound_pins` is never applied and P44/P45/P46 stay at reset. The TAS2563 codec nodes still probe over I2C0. Audio returns once playback moves to SSI2's data line (P47 -> `I2S0_SDO`).
