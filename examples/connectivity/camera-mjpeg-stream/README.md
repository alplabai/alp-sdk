# camera-mjpeg-stream

Capture, JPEG-encode, and serve an MJPEG stream from the board over a plain
Zephyr BSD TCP socket — `<alp/camera.h>` → `<alp/jpeg.h>` → HTTP
`multipart/x-mixed-replace`, no host tool required.

On the E1M-AEN family this rides the VeriSilicon ISP-Pico camera pipeline and
the Hantro VC9000E JPEG hardware encoder. Any other target (including
native_sim, which has no camera) streams a synthetic test-pattern frame
through the portable software JPEG encoder instead — the same binary always
has something to serve; see `src/main.c` for the runtime capability/format
selection.

## Build + run (E1M-AEN803, ISP-Pico + Hantro)

```bash
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/camera-mjpeg-stream -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_camera_module_1"
# flash + run per docs/aen-bench-bringup.md.
```

Console prints the DHCP lease and the two URLs once bound (illustrative
address below, RFC 5737 documentation range — the real lease depends on
your DHCP server):

```
[camera-mjpeg-stream] DHCP lease = 192.0.2.10
[camera-mjpeg-stream]   stream:   http://192.0.2.10:8080/stream
[camera-mjpeg-stream]   snapshot: http://192.0.2.10:8080/snapshot.jpg
```

### 1280x960 build variant (E1M-AEN801/AEN803, 15 fps capture/encode request)

`CONFIG_CAMERA_MJPEG_STREAM_1280X960` (this directory's `Kconfig`) switches
`src/main.c` to 1280x960 at 15 fps capture/encode and raises the JPEG
output cap to 160 KiB — see `boards/overlay-1280x960.conf` for the
matching ISP-buffer-count and SRAM0-sizing deltas that resolution needs
(same on both SKUs — AEN801 and AEN803 are the same PCB/SoC; the
accounting is also sensor-neutral, it depends only on the 1280x960 NV12
frame size, not which sensor produced it), and `src/main.c`'s
`FRAME_W`/`FRAME_H` comment for why the synthetic-frame fallback is
compiled out entirely at this size (no SRAM0 budget left for it).
Delivered rate is lower than the 15 fps capture/encode rate — see bench
run 243 below: the HTTP send path is the bottleneck. Not meaningful on
native_sim. This variant runs behind either of two sensors depending on
the build's `SHIELD` — OV5647 (`raspberry_pi_camera_module_1`, this
section) or IMX296 (`raspberry_pi_global_shutter_camera`, see "IMX296
build variant" below).

Sensor mode (issue #2286): this build now programs the OV5647's real
full-FOV 2x2-binned mode (`zephyr/drivers/video/ov5647.c`,
`ov5647_set_mode_regs()`) instead of Stage A's 1280x960 centre crop — the
delivered output size and the 15 fps request here are UNCHANGED, but the
field of view is now the whole sensor array, not a ~49%-width crop. The
sensor mode also newly supports 30 fps (`OV5647_HTS_1280X960_BINNED`);
this example still requests 15 fps by default, pending a bench pass to
confirm SRAM0/JPEG/send-path headroom at 30 fps. Bench runs 242/243 below
were measured under Stage A's crop mode; this driver-mode change (#2286)
has since been re-benched on E1M-AEN803 2026W36-0001, full-FOV 2x2-binned
1280x960 at 15 fps: delivered rate depends on JPEG size — 15.00 fps at
~35 KB frames in a daylight scene, 13.80 fps at ~39 KB (~0.54 MB/s) after
the send-window fix below, and 7.50 fps at ~109-133 KB frames in earlier
runs — always 0 CSI/IPI errors and no banding.

```bash
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/camera-mjpeg-stream -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_camera_module_1" \
    "-DEXTRA_CONF_FILE=boards/overlay-1280x960.conf"
# flash + run per docs/aen-bench-bringup.md.
```

Bench run 242 (E1M-AEN803 + OV5647, **1280x960**, 15 fps request): capture
path clean (0 CSI/IPI fatals, 0 ISP auto-stop), frames 0-11 encoded (~130 KB
JPEG, dark scene while AE converged) — then every later encode failed with
`alp_jpeg_encode failed (rc=-7 ...)` (`ALP_ERR_NOMEM`) as the scene
brightened: quality 80 (the 640x480 default) routinely exceeded
`MJPEG_HTTP_MAX_JPEG` (160 KiB) at this pixel count, and 160 KiB has no
SRAM0 headroom left to grow (98.5% bank usage). Every `GET /stream` client
also got re-served one stale frame and disconnected after ~2.1 s —
`wait_and_claim()`'s single 2 s wait for a new frame gave up and dropped
the client the moment one encode-failure burst outlasted it, even with the
capture pipeline still alive.

Both fixed: `src/main.c` now starts 1280x960 encodes at quality 60 (still
comfortably under the cap for a typical frame) and retries a failing encode
once at a lower quality (`src/jpeg_quality_ladder.h`, floor 40) instead of
dropping the frame outright; `src/mjpeg_http.c`'s `wait_and_claim()` now
gives a `/stream` client up to `STREAM_STALL_TIMEOUT_S` (30 s) total, spent
across multiple shorter waits, instead of disconnecting after one
`IDLE_TIMEOUT_S` (2 s) miss — a transient encode-retry burst no longer
looks like a dead client. `src/main.c` also prints a once-a-second stats
line (fps, encoded/failed/retried counts, JPEG size min/avg/max, encode
+ send ms) to make the next bench run's numbers easy to read off the
console.

Bench run 243 (E1M-AEN803 2026W36-0001, re-ran against these fixes): 0
CSI/IPI fatals, 0 JPEG buffer-full, board encodes ~15 fps capture/encode
(encode ~2 ms), JPEG 131-135 KB at quality 60 against the 163,840 B cap —
the encode-retry ladder never triggered (0 retries; every frame encoded
under cap on the first attempt at quality 60). Delivered rate is lower:
the host receives ~7.50 fps / 997,544 B/s — the HTTP send path, not
capture/encode, is the bottleneck (~1 MB/s), and `STREAM_STALL_TIMEOUT_S`
(30 s) comfortably covers it with 0 dropped `/stream` clients.

### IMX296 build variant (E1M-AEN801/AEN803, 1280x960 ROI, issue #2287 Stage B unit 5)

Streams the Sony IMX296 (`zephyr/drivers/video/imx296.c`) through the SAME
`CONFIG_CAMERA_MJPEG_STREAM_1280X960` path the OV5647 variant above uses —
IMX296's own 1280x960 ROI crop (`IMX296_ROI_WIDTH`/`IMX296_ROI_HEIGHT`) is
the identical frame size `boards/overlay-1280x960.conf`'s SRAM0/buffer-pool
accounting already covers, so no separate Kconfig or overlay was needed,
only a different `SHIELD`. AE is left ON (this example's own default —
`src/main.c` never disables it) and AWB stays at the ISP's own default
(`OP_TYPE_AUTO`, `isp_pico.c`'s `isp_init_controls()`) — there is **no
IMX296-fitted AWB/CCM calibration yet** (`CONFIG_VIDEO_ISP_VSI_CALIB_OV5647`
gates the only calibration tables that exist, `hal_alif` patches 0008/0011;
an IMX296 build falls through to the stock ARX3A0 AWB/CCM defaults,
`isp_param_conf.h`'s `#else` arm — colour will not be correct, only the
capture/encode/serve pipeline itself is exercised here).

```bash
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/camera-mjpeg-stream -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" \
    "-DEXTRA_CONF_FILE=boards/overlay-1280x960.conf"
# flash + run per docs/aen-bench-bringup.md.
```

Bench run 312 (E1M-AEN803 2026W36-0001, night room) confirmed the pipeline
end to end — DHCP lease obtained, `/stream` and `/snapshot.jpg` both served
a valid 1280x960 JPEG — but also found AE running gain to the GAIN
register's full 48 dB ceiling in the dim scene blew the 160 KiB JPEG
output-buffer budget on every subsequent frame (`mjpeg_http.h`'s own
comment has the full accounting). Fixed by two changes: `src/main.c`'s
quality ladder now actually engages on a buffer-full encode and persists/
recovers the reduced quality across frames (it previously never engaged at
all), and `hal_alif` patch 0013 caps the AE library's own gain ceiling at
24 dB (the GAIN register's analog-only side of the gain-vs-code bend,
datasheet p.56) instead of the full 48 dB.

Bench run 313 (same board, that fix) confirmed the fix: encode 61 fps,
0 fails, 0 retries, ~31 KB JPEGs (no more buffer-full at all with AE
capped at 24 dB — `again` converged to 0x3f65 = 16229, the new ceiling).
One HTTP client measured 17.6 fps / 552 KB/s delivered (encode-bound
headroom exists; the send path is where run 313's ~3.5x gap between
encode and delivered rate sits, same shape as OV5647's own run 243
above) with a 0.116 s worst-case frame gap. Also found and fixed:
`isp_pico.c`'s `isp_apply_ae()` was pushing the sensor's full 0-480
manual gain range as the AE library's ceiling on every
`isp_stream_start()`, silently overwriting patch 0013's lower 16229
calibration ceiling and logging "AE attr mismatch after set" (the
library's own compiled-in clamp held the real ceiling throughout, so
this was a spurious log, not a functional bug) — now clamped to agree
(`CONFIG_VIDEO_ISP_VSI_AE_AGAIN_MAX_DB_TENTHS`).

Bench run 314 (same board, this round's fixes) confirmed BOTH: no more
"AE attr mismatch after set" log line, and — the thing run 312/313 could
not explain — the DHCP lease/URL lines now print. Encode 61 fps, 0 fails,
0 retries, ~31.2 KB JPEGs; one HTTP client measured 28.3 fps / 885 KB/s
delivered (up from run 313's 17.6 fps / 552 KB/s at the same encode rate
— the VSI AE/AWB LOG_INF chatter this round's `prj.conf` fix silenced was
apparently also competing with the HTTP send thread for CPU/UART-DMA
time, not just wrapping the RAM console), 0.057 s worst-case frame gap.
The board's Ethernet MAC address also changed again between runs
(02:01:56:95:c4:a8 → ...:86; earlier boots saw ...:62 and ...:102) — see
this app's own investigation note below.

This example's `prj.conf` sets `CONFIG_VIDEO_LOG_LEVEL_WRN=y` (see that
file's own comment) specifically to stop the VSI AE/AWB library's
continuous per-frame `LOG_INF` chatter from wrapping the RAM console
ring buffer — a side effect is that `isp_pico.c`'s own `AE readback: ...`
line (below) and `video_csi_dw.c`'s `CSI IPI Controller-mode: ... line
time ...` line are ALSO `LOG_INF` and will NOT print in this example's
builds. Rebuild with `CONFIG_VIDEO_LOG_LEVEL_INF=y` (overriding this
file's `_WRN` choice) to see them again for diagnostics.

Also from run 312/313: this variant's FLASH usage on the ITCM bench
profile (`scripts/bench/aen/aen-flowc-itcm.conf`) has been running under
~300 B of headroom on a 256 KiB region — check the build's own `Memory
region` summary (`west build` prints it after linking) before adding to
`src/main.c` or its Kconfig-selected code paths; do not assume a fixed
byte count still applies, it moves with every change to this file or its
dependencies.

### Trigger mode (issue #2287 unit 4, UNBENCHED)

`CONFIG_APP_CAMERA_TRIGGER` (this directory's `Kconfig`, default `n`) puts
the camera in `ALP_CAMERA_TRIGGER_EXTERNAL` mode
(`alp_camera_set_trigger_mode()`, `<alp/camera.h>`) instead of free-run, and
starts a `k_timer` in `src/main.c` that pulses a GPIO at
`CONFIG_APP_CAMERA_TRIGGER_HZ` (default 15, range 5-60) for
`CONFIG_APP_CAMERA_TRIGGER_PULSE_US` (default 5000 us / 5 ms, range
10-100,000) to actually supply that frame timing. A `BUILD_ASSERT` in
`src/main.c` additionally enforces that the pulse width is at most half the
`_HZ` period — the two Kconfigs' individual `range`s can't express that
cross-symbol pairing, so an incompatible combination fails the build
instead of overlapping pulses at runtime. Only IMX296 answers this control
today (`zephyr/drivers/video/imx296.c`'s `VIDEO_CID_ALP_TRIGGER_MODE`) — on
any other shield `alp_camera_set_trigger_mode(ALP_CAMERA_TRIGGER_EXTERNAL)`
returns `ALP_ERR_NOSUPPORT`, which this example logs and falls back to
plain free-run streaming rather than treating as fatal. `CONFIG_APP_CAMERA_
TRIGGER` itself only builds against a board/shield that actually wired the
trigger GPIO in devicetree (a Kconfig `depends on`, not just a runtime
check) — the two AEN board overlays in this directory are the only ones
that do.

**The pulse width IS the exposure time on IMX296's fast-trigger mode** (the
sensor's exposure is set by how long XTRIG is held low, not by
`VIDEO_CID_EXPOSURE`) — `CONFIG_APP_CAMERA_TRIGGER_PULSE_US`'s 5000 us
default is an arbitrary starting point, not a calibrated exposure; tune it
for the scene once this path is actually bench-tested. Auto-exposure/gain
controls do not apply while this mode is active on that class of sensor —
see `ALP_CAMERA_TRIGGER_EXTERNAL`'s own doc comment in `<alp/camera.h>`.

```bash
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/connectivity/camera-mjpeg-stream -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-alp-sdk>;<path-to-hal_alif>" \
    "-DSHIELD=e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" \
    "-DEXTRA_CONF_FILE=boards/overlay-1280x960.conf" \
    "-DCONFIG_APP_CAMERA_TRIGGER=y"
# flash + run per docs/aen-bench-bringup.md.
```

The GPIO pin reused for the pulse is the same one
`examples/aen/aen-camera-firstlight/trigger_gpio.overlay` documents in
full: P5_1 / Arduino D4 (`EVK_PIN_CK_DIO4`), wired to the INNO-MAKER
CAM-IMX296RAW-TRIGGER module's own J3 Trig+ header — **not** the E1M-EVK
carrier and **not** the 15-pin CSI FFC between them. Both AEN board
overlays in this directory (`boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay`)
reserve that pin unconditionally under a `/zephyr,user` node (there is no
Kconfig-conditional devicetree in this build system — the overlay set is
fixed before Kconfig evaluates), but it stays inert unless
`CONFIG_APP_CAMERA_TRIGGER=y` actually configures and drives it.

**Electrical polarity is UNVERIFIED, same as `trigger_gpio.overlay`'s own
caveat — do not wire J3 until the INNO-MAKER module's own documentation (or
a bench measurement) confirms whether its input circuit inverts.** The
`GPIO_ACTIVE_HIGH` flag in both board overlays' trigger blocks is a
placeholder, not a confirmed fact; see `trigger_gpio.overlay`'s header
comment for the two possible cases it chooses between and the reasoning
either way.

**Nothing about this Kconfig option, this GPIO wiring, or this README
section has been benched on real silicon.** `testcase.yaml`'s
`aen_imx296_trigger` scenario is a compile-time check only (`build_only:
true`) — no camera, Ethernet, or trigger-source hardware exists in the CI
runner that builds it, and no bench run against a physical trigger pulse
has been recorded for this variant, unlike the IMX296 free-run variant
above (runs 312-314).

### Ethernet MAC address changes every boot (investigated, not fixed)

Bench runs 312-314 each logged a DIFFERENT MAC address
(`02:01:56:95:c4:a8`, then `...:86`, and two earlier boots at `...:62`
and `...:102`) for the SAME physical board. NOT a bug and NOT specific
to this example: `boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay`'s
`&ethernet` node has its own comment saying exactly this is expected —
"inherits `zephyr,random-mac-address` from the SoC dtsi (per-boot random
locally-administered `02:01:56:xx:xx:xx`) -- DHCP doesn't care which MAC
asks, and a random one means two boards on the same network never
collide." `zephyr/drivers/ethernet/eth_dwmac_alif_ensemble.c`'s own
header comment documents the SAME mechanism as standard upstream Zephyr
ethernet-controller semantics: `zephyr,random-mac-address` set (the
SoC-dtsi default) generates a fresh address via `gen_random_mac()` every
boot; the ONLY alternative the binding offers is deleting that DT flag
and supplying a real, per-unit `local-mac-address` instead (`local-mac-
address` is otherwise IGNORED while the random flag is set) — that
requires an actual assigned address per physical board, which a shared
generic example has no way to carry. `examples/aen/aen-ethernet-link`
and `examples/aen/aen-evk-demo` both inherit the identical SoC-dtsi
default for the same reason (searched: no `local-mac-address` override
in either). No EEPROM/OTP-sourced fixed-MAC mechanism was found in
`zephyr/drivers/ethernet/` or the SoC devicetree for this board family —
if alp-sdk gains one later (e.g. from the SoM's own identity/provisioning
EEPROM), this example and its two AEN siblings above would all want the
same change together, not this one alone. Not changed here per your
instruction (report only).

## Watch it

- **Browser** — open the `stream` URL directly; any modern browser renders
  `multipart/x-mixed-replace` as a live view.
- **VLC** — `vlc http://<ip>:8080/stream`
- **ffmpeg** (measure frame rate over a fixed window, 300 frames to `/dev/null`):
  ```bash
  ffmpeg -i http://<ip>:8080/stream -frames:v 300 -f null -
  ```
- **A single frame** (curl, straight to a file):
  ```bash
  curl -o snapshot.jpg http://<ip>:8080/snapshot.jpg
  ```

## Measured on silicon

Bench run 204 (E1M-AEN803 + OV5647, **320×240**, before this example's
resolution was raised to its intended 640×480): `/snapshot.jpg` returned
a valid 15571 B JPEG; `/stream` ran at 17 fps, ~12.8 KB/frame, 255 parts
served in 15 s with no error, with a 1536 B Ethernet net_buf data size
(run 203, at the smaller 128 B default, frames over 896 B were dropped
and no JPEG body arrived at all -- `CONFIG_ETH_DWMAC_ALIF` now defaults
`CONFIG_NET_BUF_DATA_SIZE` to 1536 itself, #2260).

Bench run 205 (E1M-AEN803 + OV5647, **640×480**, dark scene): 10 fps,
~37 KB/frame. **Taken before** the ISP `bytesused` fix (#2263), the MRSZ
chroma-row fix (#2267/#2269) and the Hantro output-buffer-overrun fix
(#2268) landed -- treat these numbers as a pre-fix baseline, not a
measurement of the current code; rerun the ffmpeg command above for a
current figure.

Bench run 220 (E1M-AEN803 + OV5647, **640×480**, bright daylight, 30 fps
request, #2276): 901 complete JPEGs streamed in each of three 30 s
captures = **30.03 fps** -- confirms the requested 30 fps is achievable
end to end through this pipeline. AE settled around intLine ~331-351
(boot readback `AE readback: int_time_max=32667 ...`, matching the 30 fps
frame period — that line is `LOG_INF`, so it prints at this run's log
level but NOT in a build with this file's own `CONFIG_VIDEO_LOG_LEVEL_WRN=y`
(added later, for the IMX296 variant above; see that entry's own note)),
no `E:`/`W:` log lines.

Bench runs 224-228 (E1M-AEN803 + OV5647, **640×480**, #2285): a timing
probe in run 223 found the camera loop itself keeping up with 60 fps
while `/stream`'s HTTP send took 24-30 ms per 32-41 KB frame, dropping
45-51% of frames -- so the bottleneck was the send path, not the camera
or JPEG encode. `boards/alp_e1m_aen80{1,3}_..._rtss_he.conf`'s Ethernet
TX/RX buffer sizing alone took drops to 0%; combined with
`CONFIG_ALP_SDK_FAST_MEMCPY` (the word-copy `memcpy()` override for -Os
images, `zephyr/kconfigs/core.kconfig`) on top, that configuration
reached **60.1 fps**, the OV5647 sensor's own rate -- 1803 frames in
30 s, interval stdev 0.1 ms. The PHY was confirmed at 100 Mbit full
duplex; disabling TCP congestion avoidance had no effect. That 60.1 fps
figure is not attributable to the `memcpy()` override on its own --
see the follow-up A/B below.

**Not a clean A/B against runs 205/220/223 above**: the 60.1 fps run
used a simpler scene with a smaller mean frame (14.7 KB) than those
runs.

**`CONFIG_ALP_SDK_FAST_MEMCPY` same-scene A/B** (E1M-AEN803, module
2026W36-0001, **640×480 @ 30 fps**, camera-bound, against the
buffer-sizing-only baseline): A1 (memcpy ON) 30.03 fps / 1,089,750 B/s,
B1 (memcpy OFF) 30.03 fps / 1,092,219 B/s, A2 (memcpy ON) 30.03 fps /
1,088,302 B/s, 0 drops in every leg -- **no measurable memcpy gain** in
this camera-bound scene. `CONFIG_ALP_SDK_FAST_MEMCPY` defaults to `n`
for this reason; this example's AEN801/AEN803 board confs enable it
explicitly, on the strength of the combined 60.1 fps figure above, not
this A/B. The send-bound case (the one run 223 actually hit, with its
24-30 ms sends) is still unmeasured -- open follow-up work.

## Limits

This is a teaching example, not a production camera server:

- **One client at a time.** `zsock_listen(..., 1)` and a single server
  thread -- a second concurrent client is queued behind the first (or
  refused once the one-deep backlog is full), not served in parallel.
- **No authentication, no TLS.** Plain HTTP; anyone who can reach port
  8080 on the LAN sees the stream.
- **LAN-only by design** -- there is no port-forwarding/NAT guidance
  because this is not meant to be exposed past your local network.
- **fps is bounded by encode + send time, not just camera fps.**
  `mjpeg_http_publish_frame()` drops the newly-encoded frame outright if
  a `/stream` client is still mid-send of the previous one (see
  `src/mjpeg_http.c`'s file header) -- so the achievable rate is roughly
  `1 / (t_encode + t_send)`. Run 220 measured this at 30.03 fps (bright
  daylight), confirming the requested 30 fps is reachable end to end at
  640×480 with this pipeline's encode+send cost. Run 205's 10 fps at
  640×480 (~37 KB/frame, dark scene, pre-#2276) is an older measurement
  at the 15 fps request this example used before #2276 raised the
  default, and predates the ISP/Hantro fixes noted above -- not a target
  this example tries to hit. Runs 224-228 (#2285) show the "send"
  side of that bound is itself movable: the AEN board confs' Ethernet
  buffer sizing alone (bench-proven) plus `CONFIG_ALP_SDK_FAST_MEMCPY`
  reached 60.1 fps combined, the sensor's own cap -- see the Measured
  section above for both the non-A/B caveat on that number and the
  separate `CONFIG_ALP_SDK_FAST_MEMCPY` same-scene A/B, which found no
  measurable memcpy gain in this camera-bound scene.

**Fixed since run 205:** the bottom two rows of every frame rendered solid
green -- an ISP main-resizer chroma-row rounding bug (`ISP_MRSZ_SCALE_VC`
truncation dropping the last 4:2:0 chroma row), now fixed at the driver
level (#2267/#2269). YUV/NV12 frames dequeued from the ISP also used to
report `bytesused` 0 (a pitch/size bug that sized buffers off the luma
plane alone), which this example's camera path never hit directly but
which sat upstream of it in the same pipeline; also fixed at the driver
level (#2263).

**Unverified on silicon:**

- **ISP NV12 output content (exposure/color).** Runs 204 and 205 *did*
  produce frames through this path end to end (camera → ISP → JPEG →
  HTTP, no pipeline errors), but the frame content itself was
  dark/near-black (AWB `noWhitePixel` flagged on every channel, gains
  read back 0x0) -- under separate investigation (scene/lens vs. AE),
  not a defect in this example's own code, and not yet re-benched since
  the driver fixes above landed. Treat the *pipeline* as bench-proven and
  the *image quality* as not yet.

Hantro repeat-encode stability is **not** on the unverified list: runs
204 and 205 repeat-encoded 255+ and 300+ frames respectively, back-to-back,
with no encoder error.

## Native_sim (CI)

No real camera, no Ethernet: `board.yaml` still declares `som.sku:
E1M-AEN803`, so `alp_has(ALP_CAP_ID_HW_MIPI_CSI)` reads true even under
native_sim (capability comes from the configured SoM, not the host
running the binary) -- the synthetic-frame fallback fires because
`alp_camera_open()` (no real camera backend links on native_sim) fails,
not because `alp_has()` is false. `boards/native_sim.conf` turns on
`CONFIG_NET_LOOPBACK`, the capture loop streams the synthetic frame
through the software JPEG encoder, and `src/selftest.c` (compiled in
only for `native_sim`) loopback-connects to the app's own server on
`127.0.0.1:8080`: it checks `GET /snapshot.jpg` (body starts with the
JPEG SOI marker `FF D8`, ends with EOI `FF D9`) and `GET /stream`'s
multipart framing (the `--alpframe` boundary line, a `Content-Type` +
`Content-Length` header pair, the blank line, and the trailing CRLF after
the body):

```
[camera-mjpeg-stream] selftest ok
```

## Files

| File | What |
|---|---|
| `src/main.c` | Camera-capture + JPEG-encode loop (the app's main thread); pixfmt selection from `alp_jpeg_caps_t::pixfmt_mask`; DHCP kick-off + lease/URL printing. |
| `src/mjpeg_http.c` | The HTTP server: `GET /stream` + `GET /snapshot.jpg`, its own thread, `zsock_*` sockets, zero-copy ping-pong frame hand-off (two SRAM0 buffers, `mjpeg_http_claim_write_buffer()`/`mjpeg_http_publish_frame()`), `SO_RCVTIMEO`/`SO_SNDTIMEO` on every accepted socket. |
| `src/mjpeg_http.h` | The hand-off API + `MJPEG_HTTP_MAX_JPEG` — the one shared cap both this file's buffers and main.c's `alp_jpeg_encode()` call use — plus `mjpeg_http_get_stats()`, main.c's window into `mjpeg_http.c`'s own last-send timing. |
| `src/jpeg_quality_ladder.h` | `jpeg_quality_step_down()` — the pure, native_sim-testable retry-ladder step `src/main.c` uses on an `ALP_ERR_NOMEM` encode failure (`tests/unit/mjpeg_quality_ladder`). |
| `src/aen_eth_phy.c` | AEN-only, interim: PHY power/reset + refclk-mode bring-up; not linked on other targets. |
| `src/selftest.c` | native_sim-only CI selftest (`GET /snapshot.jpg` JPEG marker check, `GET /stream` multipart-framing check). |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay` | ISP graph rewiring (mirrors `aen-isp-ov5647-viewfinder`) + interim Ethernet RMII/PHY DT wiring (mirrors `aen-ethernet-link`) + the `CONFIG_APP_CAMERA_TRIGGER` GPIO reservation (`/zephyr,user`, inert unless that Kconfig is on). Content-identical across the two SKUs. |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.conf` | AEN hardware-path Kconfig (ISP pipeline sized for 640×480 NV12, Hantro JPEG encoder, Ethernet DMA-region glue, `CONFIG_DCACHE=n`) — board-scoped so native_sim stays clean of undefined-symbol Kconfig warnings. Content-identical across the two SKUs. |
| `boards/overlay-1280x960.conf` | 1280x960 variant (AEN801 or AEN803, same PCB/SoC): sets `CONFIG_CAMERA_MJPEG_STREAM_1280X960`, drops the ISP raw-buffer count to 2, resizes the video buffer pool for the larger NV12 frame, and resets `CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE` to 0 (auto) — the board confs' 64 KiB window starves this overlay's 16/8 TX pools — layered on top of either board conf via `EXTRA_CONF_FILE`. |
| `Kconfig` | `CONFIG_CAMERA_MJPEG_STREAM_1280X960` — the resolution select `src/main.c` and `src/mjpeg_http.h` both key off — plus `CONFIG_APP_CAMERA_TRIGGER`/`_HZ` (see "Trigger mode" above). |
| `boards/native_sim.conf` + `boards/native_sim_native_64.conf` | Content-identical pair (Zephyr resolves a different filename per qualifier string, so one file alone doesn't cover both `native_sim` and `native_sim/native/64`): `CONFIG_NET_LOOPBACK` + a zeroed `CONFIG_NET_TCP_TIME_WAIT_DELAY` so `src/selftest.c`'s two back-to-back loopback connections don't collide on a lingering TIME_WAIT port. |

## Portability

See `board.yaml`'s header comment for why this example is Ring 3
(SoM-bound) despite having no `chips:` list and a fully capability-gated
`src/main.c`.
