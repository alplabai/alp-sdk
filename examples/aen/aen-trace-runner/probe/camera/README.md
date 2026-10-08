# camera probe

DIAGNOSTIC, NOT PRODUCT CODE. Camera open fails `ALP_ERR_IO` on **a bench E1M-AEN803**,
reproducibly. `<alp/camera.h>`'s Zephyr backend (`alp-sdk-lcd/src/backends/camera/zephyr_video.c`,
`z_open()`) collapses every failure from `video_get_caps()`, `video_set_format()` and
`video_enqueue()` into `ALP_ERR_IO` by design (`_errno_to_alp()` maps any errno it does not
recognise to it) -- so `ALP_ERR_IO` alone never says which call failed or why, and
`alp_last_error()` returns that same translated status.

This probe deliberately bypasses `<alp/*>` and calls the same Zephyr `video_*`/`i2c_*` functions
directly, in the same order `z_open()` calls them, printing each one's **raw return value**. It is
standalone -- its own `CMakeLists.txt` / `prj.conf` / `src/main.c`, no shared source with the game
under `src/`, and it does not touch anything outside `probe/camera/`.

Leading suspect: an I2C NACK during the OV9281 format write. This board's I2C is also documented to
sometimes **fail open** (every address ACKs, every register read returns a byte equal to its own
address instead of real data) -- a plausible-looking value alone proves nothing, so this probe reads
the sensor's fixed chip-ID registers as a ground-truth control both before anything else touches the
bus and again after the video sequence.

## Build

Zephyr module is `alp-sdk-lcd`, **not** `alp-sdk` -- a plain `west build` resolves a different Zephyr
checkout that knows none of the alp-sdk shields and fails.

```sh
cd examples/aen/aen-trace-runner
ZEPHYR_BASE=$ZEPHYR_BASE \
west build -p always -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
  -d <build-dir> probe/camera \
  -- -DEXTRA_ZEPHYR_MODULES="<alp-sdk>;<hal_alif>" \
     -DPython3_EXECUTABLE=/usr/bin/python3
```

Both shields (`e1m_evk_rpi_csi` + `innomaker_cam_ov9281`) are hardcoded in `CMakeLists.txt` -- no
`-DSHIELD=` needed on the command line, only `EXTRA_ZEPHYR_MODULES`.

## Run (bench)

From the `alp-sdk-lcd` checkout, using the OpenOCD RAM-run flow (the only one that can address either
M55 core on this bench):

```sh
cd <alp-sdk>
scripts/bench/aen/openocd-ram-run.sh <build-dir> he
```

Resolve the board's USB path fresh from `labgrid-client -p <place> show` per the script's own
header; do not reuse a path from a doc or a previous run. Board under test: **E1M-AEN803
the bench unit**. Read `ram_console_buf` over SWD after the run -- this board's console FTDI passes
zero bytes, so a UART capture will show nothing (`prj.conf` sets `CONFIG_RAM_CONSOLE=y` /
`CONFIG_UART_CONSOLE=n` for exactly this reason).

## Reading the output

Every line is prefixed `[camprobe]` except the final `RESULT:` line. In order:

```
=== camprobe: camera bring-up diagnostic, a bench E1M-AEN803 ===
sensor: ov9281@60 @ 0x60 on bus i2c@...
device_is_ready(sensor) = 0/1
device_is_ready(video)  = 0/1
```

**Step 1 -- identity + readiness.** The sensor and video devices are checked separately:
`device_is_ready(video)` can fail for reasons that have nothing to do with the sensor (a D-PHY or CPI
init failure, for example), so a video-not-ready result does not by itself implicate the sensor, and
vice versa.

```
i2c control (start): NNN/112 addresses ACKed (0x08..0x77) [-- suspiciously high, most of a fail-open bus]
i2c control (start): reg 0x300a -> rc=... (...) val=0x..
i2c control (start): reg 0x300b -> rc=... (...) val=0x..
i2c (start): HEALTHY (chip id 0x9281) | FAIL-OPEN SUSPECTED | NACK | UNEXPECTED (...)
```

**Step 2 -- I2C ground truth, before the video sequence touches the bus.** A healthy bus ACKs a
handful of addresses (the OV9281 at 0x60, whatever else is on E1M I2C1); roughly 112/112 means the
bus is answering *everything*, which is the fail-open signature, not 112 real devices. The chip-ID
read is the real test: 0x300A must read back `0x92` and 0x300B must read back `0x81` -- fixed OV9281
silicon constants, so they cannot legitimately be anything else. If either instead reads back `0x30`,
`0x0A` or `0x0B` -- a byte of the register address itself echoed back as if it were data -- the
verdict line says `FAIL-OPEN SUSPECTED` explicitly; treat every other value on that bus as unproven
until this control passes.

```
video_get_caps           -> rc=... (...)
  cap: GREY 640x400..1280x800   (one line per format_caps entry, or "format_caps: SKIPPED")
video_set_format          -> rc=... (...)
video_buffer_alloc[0]     -> 0x... | NULL (no errno available)
video_enqueue[0]          -> rc=... (...)
video_buffer_alloc[1]     -> ...
video_enqueue[1]          -> ...
video_stream_start        -> rc=... (...) | SKIPPED (0 of 2 buffers enqueued)
video_dequeue              -> rc=... (...) | SKIPPED (stream not started)
  frame: bytesused=... first16=xx xx ...
```

**Step 3 -- the same calls `z_open()` makes**, in the same order, each printed as `rc=<signed int>
(<ERRNO_NAME>)`. Every step runs even if an earlier one failed, *except* where running it would be
meaningless rather than merely likely-to-fail: `video_stream_start` is skipped if no buffer was
successfully enqueued, and `video_dequeue` is skipped if the stream never started. A skip says so and
why, so one run still shows every step that actually executed, not just the first failure.

```
i2c control (end): ...
i2c (end): ...
FINDING: bus was HEALTHY at start and is NOT healthy at end -- ... | bus health unchanged: HEALTHY before and after. | bus was already unhealthy at start ...
```

**Step 4 -- repeat the chip-ID read.** If the bus was healthy at the start and is not healthy at the
end, the video sequence (the format write is the leading suspect) broke the bus -- that is itself a
finding, independent of whatever the individual step return codes say.

```
RESULT: first failing step = <name>, raw errno = <signed int> (<ERRNO_NAME>)
```
or
```
RESULT: no step returned a nonzero/NULL result
```

**Step 5 -- one summary line** naming the first step (in call order) that returned nonzero or NULL,
and its raw errno. `video_buffer_alloc` has no errno on failure (it returns a pointer); that case
reads `NULL return (no errno available)` instead of a fabricated errno.

## What this probe deliberately does NOT do

- **Does not use `<alp/camera.h>` or any other `<alp/*>` header.** That is the entire point: it
  exists to see the raw errnos the portable API's `_errno_to_alp()` throws away, not to demonstrate
  the portable call sequence (that is `aen-camera-firstlight` in `alp-sdk-lcd`, already bench-verified
  on this hardware).
- **Does not touch anything outside `probe/camera/`.** Nothing under `src/`, `tests/`, `art/`,
  `tools/`, or the top-level `CMakeLists.txt` is read or written by anything here.
- **Does not stream a display.** Only `e1m_evk_rpi_csi` + `innomaker_cam_ov9281` are stacked -- the
  simplest configuration that can still reproduce the failure. Display contention was already ruled
  out on the bench (the open failed with the display never initialised).
- **Does not claim a bench result.** This app has never been run on real hardware by whoever wrote it
  -- every "rc=..." / "HEALTHY" / "FAIL-OPEN" line above is a description of the output format, not a
  reported outcome.
