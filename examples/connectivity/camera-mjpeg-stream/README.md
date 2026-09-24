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
  `1 / (t_encode + t_send)`, not the camera's requested 15 fps. Run
  205's 10 fps at 640×480 (~37 KB/frame) is a real measurement of that,
  not a target this example tries to hit.

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
| `src/mjpeg_http.h` | The hand-off API + `MJPEG_HTTP_MAX_JPEG` — the one shared cap both this file's buffers and main.c's `alp_jpeg_encode()` call use. |
| `src/aen_eth_phy.c` | AEN-only, interim: PHY power/reset + refclk-mode bring-up; not linked on other targets. |
| `src/selftest.c` | native_sim-only CI selftest (`GET /snapshot.jpg` JPEG marker check, `GET /stream` multipart-framing check). |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay` | ISP graph rewiring (mirrors `aen-isp-ov5647-viewfinder`) + interim Ethernet RMII/PHY DT wiring (mirrors `aen-ethernet-link`). Content-identical across the two SKUs. |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.conf` | AEN hardware-path Kconfig (ISP pipeline sized for 640×480 NV12, Hantro JPEG encoder, Ethernet DMA-region glue, `CONFIG_DCACHE=n`) — board-scoped so native_sim stays clean of undefined-symbol Kconfig warnings. Content-identical across the two SKUs. |
| `boards/native_sim.conf` + `boards/native_sim_native_64.conf` | Content-identical pair (Zephyr resolves a different filename per qualifier string, so one file alone doesn't cover both `native_sim` and `native_sim/native/64`): `CONFIG_NET_LOOPBACK` + a zeroed `CONFIG_NET_TCP_TIME_WAIT_DELAY` so `src/selftest.c`'s two back-to-back loopback connections don't collide on a lingering TIME_WAIT port. |

## Portability

See `board.yaml`'s header comment for why this example is Ring 3
(SoM-bound) despite having no `chips:` list and a fully capability-gated
`src/main.c`.
