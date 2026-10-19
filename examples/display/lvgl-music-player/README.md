# lvgl-music-player

Wraps the upstream **`lv_demo_music()`** -- album-art carousel,
time slider, track list, equaliser visualiser.  This is a
**display-only** demo: it renders and animates the player UI but
does not decode or play any audio.

## What it shows

- The panel opens through the portable `<alp/display.h>` surface
  and binds to LVGL via `alp_gui_lvgl_attach()` (`<alp/gui.h>`) --
  no direct `<zephyr/drivers/display.h>` calls in app code.
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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/display/lvgl-music-player`)

board.yaml declares only `m55_hp`, so tan also builds the SDK's idle alp-stock-shim image for `m55_he` (`build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

native_sim builds the UI against the dummy display.

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/display/lvgl-music-player/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/display/lvgl-music-player
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/display/lvgl-music-player -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Verification status

`[UNTESTED]` -- the UI is the upstream lv_demo_music() reference;
panel bring-up still needs HiL.  Use the
`lvgl-widgets-demo` for the simplest LVGL test first.
