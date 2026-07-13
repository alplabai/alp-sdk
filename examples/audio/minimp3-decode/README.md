# minimp3-decode

Decodes a canned MP3 byte array to 16-bit PCM with the vendored
[minimp3](https://github.com/lieff/minimp3) single-header decoder and
prints the sample count + RMS of the decoded audio. Pure compute --
no I2S/codec hardware required, so this builds and runs identically
on `native_sim` and real silicon.

**[UNTESTED]** -- native_sim build+run verified locally (see
"Verified" below); no on-silicon / HIL bench validation yet.

## What this shows

* The minimp3 STB-style single-header pattern: exactly one
  translation unit `#define`s `MINIMP3_IMPLEMENTATION` before
  `#include "minimp3.h"` to compile in the decoder body; every other
  TU would include it declaration-only.
* The `mp3dec_init()` -> loop-of-`mp3dec_decode_frame()` streaming
  pattern every MP3 decoder integration follows: each call consumes
  exactly one frame and reports how many input bytes it ate
  (`mp3dec_frame_info_t.frame_bytes`), so the caller advances its
  read pointer and calls again until the buffer is exhausted.
* Why `-Wdouble-promotion` needs a local `#pragma GCC diagnostic`
  around the *implementation* include only -- unmodified upstream
  minimp3 trips this SDK's `-Werror` build (see
  `vendors/minimp3/README.md`); we silence it locally rather than
  patch vendored, verbatim upstream source.
* `libraries: [minimp3]` in `board.yaml` -- wires the Kconfig knobs
  in `metadata/library-profiles/minimp3/hw-backends.yaml` (Helium
  MVE / Neon / FPU SIMD backends selected by SoM capability; pure-C
  is always the floor). None of those backends change this example's
  behavior since we never touch real hardware, but any app that goes
  on to stream decoded PCM out over I2S inherits the right backend
  automatically.

## Why vendored, not west-fetched

`west.yml` carries a `minimp3` project pin under the disabled
`extras-tier1` group, but the west topdir workspace this SDK builds
against hasn't fetched it. minimp3 is a single header with no build
system of its own, so it's vendored the same way as `etl`/`fmt`
instead of left as a documented gap -- see
[`vendors/minimp3/README.md`](../../../vendors/minimp3/README.md).
Because it's vendored (not a real Zephyr module with a
`zephyr/module.yml`), its include directory isn't added
automatically the way a west module's would be; this example's
`CMakeLists.txt` adds `vendors/minimp3/include` explicitly via
`target_include_directories()`.

## The embedded MP3 data

`src/main.c` embeds the first 300 bytes of a 32 kbps/44.1 kHz/mono
MP3 -- LAME's leading "Info" tag frame (silent, but a syntactically
valid MPEG-1 Layer III frame) followed by one real audio frame, plus
a few trailing bytes to exercise the "not enough data for a full
frame" path a streaming decoder must also handle cleanly. Generated
with:

```bash
ffmpeg -f lavfi -i "sine=frequency=440:duration=0.2:sample_rate=44100" \
       -ac 1 -b:a 32k -codec:a libmp3lame - \
    | <strip the leading ID3v2 tag, keep the first 300 bytes from the first 0xFF 0xFB sync>
```

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/audio/minimp3-decode \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
west build -b alp_e1m_aen801_m55_hp examples/audio/minimp3-decode
west flash
```

## Verified

```
west twister -T examples/audio/minimp3-decode -p native_sim/native/64 \
    -x ZEPHYR_MODULES="<alp-sdk>" -v
```
`native_sim/native/64` build + run: PASS, console prints
`[minimp3-decode] done`.

## Where this leads

To actually play the decoded audio, take `pcm`/`samples`/`info.hz`
out of the decode loop and feed them into
[`examples/audio/i2s-tone`](../i2s-tone)'s I2S write path (`BOARD_I2S_AUDIO`,
resolved per-EVK). That's the only board-specific step; the decoder
itself is fully portable.

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
- [`vendors/minimp3/README.md`](../../../vendors/minimp3/README.md) --
  vendoring rationale + the `-Wdouble-promotion` gotcha in full.
- [`metadata/library-profiles/README.md`](../../../metadata/library-profiles/README.md)
  -- the `libraries:` / profile-header mechanism.
