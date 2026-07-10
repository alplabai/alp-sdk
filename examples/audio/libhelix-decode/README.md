# libhelix-decode

Decodes a canned MP3 frame to 16-bit PCM with the [Helix](https://en.wikipedia.org/wiki/Helix_(codec))
fixed-point MP3 decoder and prints the sample count.

**[UNTESTED] -- does not build in this workspace.** See "Module
status" below for why, and "What this teaches instead" for what you
still get out of this example.

## Module status

`west.yml`'s `libhelix` project (`extras-tier1` group, `path:
modules/lib/libhelix`, `remote: xiph`, `revision: main` -- see the
`west.yml` comment: "TBD: confirm canonical upstream + tag") failed
its pinned-revision checkout in this workspace:

```
$ ls -la modules/lib/libhelix
total 12
drwxrwxr-x 3 ... .
drwxrwxr-x 14 ... ..
drwxrwxr-x 7 ... .git

$ cd modules/lib/libhelix && git status
On branch init_placeholder
No commits yet
nothing to commit (create/copy files and use "git add" to track)
```

An initialised-but-empty repository, still on a placeholder branch
with zero commits -- not the vendor's actual source. There is
consequently no `mp3dec.h` anywhere in this workspace for
`src/main.c`'s `#include "mp3dec.h"` to resolve against, and this repo
cannot inspect the module to confirm whether the real checkout would
even be the MP3 flavor of Helix or the AAC flavor (see `src/main.c`'s
trailing comment, "If this turns out to be AAC, not MP3").

## What this teaches instead

Per the parent plan's explicit escape valve for this case, `src/main.c`
still writes out the **correct, real Helix MP3 decoder API** --
`HMP3Decoder` / `MP3InitDecoder()` / `MP3FindSyncWord()` /
`MP3Decode()` / `MP3GetLastFrameInfo()` / `MP3FreeDecoder()` are the
actual exported symbols of `mp3dec.h` in every libhelix-mp3 fork; this
API predates and outlives any specific packaging of it. Compare it
against [`minimp3-decode`](../minimp3-decode) (same canned MP3 bytes,
a working decoder) to see where the two APIs genuinely differ:

| | minimp3 | Helix |
|---|---|---|
| Decoder state | `mp3dec_t` on the caller's stack | `HMP3Decoder` opaque heap handle (`MP3InitDecoder()`/`MP3FreeDecoder()`) |
| Frame sync | tolerated inline by `mp3dec_decode_frame()` | caller must locate it first, via `MP3FindSyncWord()` |
| Pointer advance | caller advances by `frame_bytes` | `MP3Decode()` advances the caller's `readPtr`/`bytesLeft` itself, by reference |
| End of input | `frame_bytes == 0` | `MP3FindSyncWord()` returns negative |
| Per-frame info | `mp3dec_frame_info_t` (channels, hz, ...) | `MP3FrameInfo` via `MP3GetLastFrameInfo()` (`nChans`, `samprate`, `outputSamps`, ...) |

## Once the module fetch is fixed

Re-pin `west.yml`'s `libhelix` `revision:` to a real tag/commit (an
upstream libhelix-mp3 fork -- not `xiph/libhelix`, which is a broken
placeholder guess; see the `west.yml` TBD comment), `west update`,
then confirm the checked-out layout puts `mp3dec.h` where
`CMakeLists.txt` expects (see that file's "NOT WIRED" comment) and
wire `target_include_directories()` the same way
[`minimp3-decode`](../minimp3-decode)'s `CMakeLists.txt` does. No
change to `src/main.c`'s logic should be needed -- only confirm the
MP3-vs-AAC flavor question the trailing comment raises.

## Board.yaml note

`board.yaml` declares no real peripherals (`peripherals: []`) --
decoding a canned in-memory frame needs no hardware. A real player
would add `i2s` and stream the decoded PCM to `BOARD_I2S_AUDIO` (see
[`examples/audio/i2s-tone`](../i2s-tone)); that's the only
board-specific step, same as `minimp3-decode`.

## Reference

- [`examples/audio/minimp3-decode`](../minimp3-decode) -- the working
  sibling example, same canned MP3 bytes, a decoder that actually
  builds.
- [`metadata/library-profiles/libhelix/hw-backends.yaml`](../../../metadata/library-profiles/libhelix/hw-backends.yaml)
  -- the FPU/I2S-DMA/pure-C backend selection this example's
  `libraries: [libhelix]` wires (unused by this example's pure-compute
  path, but exercised once real playback is added).
