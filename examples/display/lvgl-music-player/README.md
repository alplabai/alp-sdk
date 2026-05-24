# lvgl-music-player

Wraps the upstream **`lv_demo_music()`** -- album-art carousel,
time slider, track list, equaliser visualiser.  Pairs the LVGL
UI with the SDK's audio chain (`<alp/i2s.h>` + a WM8960 codec).

## What it shows

- LVGL renders the player UI on a 240 x 320 ST7789 panel.
- The WM8960 codec is configured via I²C (the `wm8960` chip
  driver handles the register writes) and streams audio data
  via I²S (host I²S0 → codec → headphone out).
- The MP3 decoder (`minimp3` from the §D.lib batch) supplies
  PCM frames to the I²S sink.

## Hardware needed

- E1M-AEN family SoM.
- E1M-EVK board with the audio breakout board.
- 240 x 320 ST7789 TFT.
- Wolfson WM8960 audio codec + headphone jack.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-music-player
west flash
```

native_sim builds the UI but stubs the audio path (no codec on
the host).

## Verification status

`[UNTESTED]` -- the UI is the upstream lv_demo_music() reference;
the audio chain is wired but real codec bring-up gates on the
v1.0 HiL sweep.  Use the `lvgl-widgets-demo` for a no-audio-
dependency LVGL test first.
