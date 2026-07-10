# libhelix-decode

Demonstrates the [Helix](https://en.wikipedia.org/wiki/Helix_(codec))
fixed-point MP3 decoder workflow: MP3 frame synchronisation, header
parsing, and (on ARM silicon) fixed-point PCM decode.

**[UNTESTED]** — the portable frame-sync + header-parse path builds
and runs on `native_sim`; the real Helix PCM decode has not been
bench-run on silicon from this example.

## Why native_sim only parses headers

Helix is a **fixed-point decoder written for embedded ARM**. Its hot
inner loops go through `real/assembly.h`, which supplies hand-tuned
`MULSHIFT32` for ARM (and Win32/x86 inline asm) and

```
#error Unsupported platform in assembly.h
```

for everything else — including `native_sim`'s generic x86-64 host.
So the real decoder compiles for a Cortex-M55 SoM (E1M-AEN801) but
**not** for the host simulator.

This example therefore runs the portable part of the pipeline —
locating each MP3 frame's 11-bit sync word and decoding its header —
in plain C on `native_sim`, printing exactly what Helix's
`MP3FindSyncWord()` + `MP3GetLastFrameInfo()` report (frame count,
sample rate, samples-per-frame). The real `MP3Decode()` PCM path is
documented in `src/main.c`'s banner and wired (commented) in
`CMakeLists.txt`.

Expected `native_sim` output:

```
[libhelix-decode] alp-sdk libhelix-decode starting
[libhelix-decode] frame 0: layer 3, 44100 Hz, 1152 samples/frame
[libhelix-decode] frame 1: layer 3, 44100 Hz, 1152 samples/frame
[libhelix-decode] found 2 frame(s), 2304 total samples/channel
[libhelix-decode] done
```

## Getting real PCM on an ARM SoM

1. Fetch the module (RCSL-licensed — see below):
   `west update --group-filter +extras-tier1`.
   `west.yml` pins `libhelix` to
   [`ultraembedded/libhelix-mp3`](https://github.com/ultraembedded/libhelix-mp3)
   (the canonical community Helix MP3 source; the old `xiph/libhelix`
   pin was an empty repo).
2. Build this example for an ARM target (e.g. `som.sku: E1M-AEN801`).
3. Uncomment the `target_sources(...)` / `target_include_directories(...)`
   block in `CMakeLists.txt` and switch `src/main.c` to the
   `MP3Decode()` sequence shown in its banner.

### Licence

Helix is **RealNetworks RCSL/RPSL** — source-available, **not**
Apache-2.0. alp-sdk references it only as a west pin (never vendors
its source into this tree), and it is disabled by default behind the
`extras-tier1` group. Review the RCSL/RPSL terms before shipping it in
a product.

## API compared to minimp3

[`minimp3-decode`](../minimp3-decode) decodes the *same* canned MP3
bytes with a portable (CC0) decoder that builds everywhere — a useful
contrast:

| | minimp3 | Helix |
|---|---|---|
| Decoder state | `mp3dec_t` on the caller's stack | `HMP3Decoder` opaque heap handle (`MP3InitDecoder()`/`MP3FreeDecoder()`) |
| Frame sync | tolerated inline by `mp3dec_decode_frame()` | caller locates it first, via `MP3FindSyncWord()` |
| Pointer advance | caller advances by `frame_bytes` | `MP3Decode()` advances the caller's `readPtr`/`bytesLeft` by reference |
| Per-frame info | `mp3dec_frame_info_t` (channels, hz, ...) | `MP3FrameInfo` via `MP3GetLastFrameInfo()` (`nChans`, `samprate`, `outputSamps`) |
| Host build | yes (pure C) | no (ARM fixed-point asm) |

## Board.yaml note

`board.yaml` declares no real peripherals — parsing an in-memory frame
needs no hardware. A real player adds `i2s` and streams decoded PCM to
`BOARD_I2S_AUDIO` (see [`examples/audio/i2s-tone`](../i2s-tone)).

## Reference

- [`examples/audio/minimp3-decode`](../minimp3-decode) — working
  host-buildable sibling, same canned MP3 bytes.
- [`metadata/library-profiles/libhelix/hw-backends.yaml`](../../../metadata/library-profiles/libhelix/hw-backends.yaml)
  — the FPU/I2S-DMA/pure-C backend selection `libraries: [libhelix]`
  wires (exercised once real playback is added).
