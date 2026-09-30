### Fixed

- E1M-X EVK (V2N family): re-enable the TAS2563 ALSA card, now playing out of SSI2's data line (P47 -> `I2S0_SDO` -> amp SDIN) with SCK/WS from SSI1 (P44/P45) (#2331). RZ/V2N SSIU_SSI_MODE1.ssi2_pin = 110b (R01UH1072EJ0120, 8.5.2.3.16) makes this possible; new kernel patch 0013 teaches rsnd the SSI2-under-SSI1 parent case (`alp,shared-pin-ssi1` on `&ssi2`). P46 (the amps' SDOUT net) is never muxed. Bench listen still pending.
