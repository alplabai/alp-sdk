# lvgl-music-player

Wraps the upstream **`lv_demo_music()`** -- album-art carousel,
time slider, track list, equaliser visualiser.  This is a
**display-only** demo: it renders and animates the player UI but
does not decode or play any audio.

## What it shows

- LVGL renders the player UI on a 240 x 320 ST7789 panel.
- `lv_demo_music()` drives the progress bar + equaliser bands on
  its own animation timer -- no real track is decoded or played.

> No audio path.  `lv_demo_music()` only animates the UI; this
> example makes no `alp_i2s_*` or codec calls.  Adding a real
> codec-over-I²S output path plus an MP3 decoder is intentionally
> out of scope -- the point here is the LVGL UI.

## Hardware needed

- E1M-AEN family SoM.
- 240 x 320 ST7789 TFT.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-music-player
west flash
```

native_sim builds the UI against the dummy display.

## Verification status

`[UNTESTED]` -- the UI is the upstream lv_demo_music() reference;
panel bring-up gates on the v1.0 HiL sweep.  Use the
`lvgl-widgets-demo` for the simplest LVGL test first.
