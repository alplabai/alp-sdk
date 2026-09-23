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

Console prints the DHCP lease and the two URLs once bound:

```
[camera-mjpeg-stream] DHCP lease = 192.168.10.137
[camera-mjpeg-stream]   stream:   http://192.168.10.137:8080/stream
[camera-mjpeg-stream]   snapshot: http://192.168.10.137:8080/snapshot.jpg
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

TBD (bench) — fps (ffmpeg command above) and bytes per frame (quality 80,
320×240) once run against a real E1M-AEN803 + OV5647.

## Native_sim (CI)

No camera, no Ethernet: `boards/native_sim_native_64.conf` turns on
`CONFIG_NET_LOOPBACK`, the capture loop streams the synthetic frame through
the software JPEG encoder, and `src/selftest.c` (compiled in only for
`native_sim`) loopback-connects to the app's own server on `127.0.0.1:8080`,
fetches `GET /snapshot.jpg`, and checks the response body starts with the
JPEG SOI marker (`FF D8`) and ends with EOI (`FF D9`):

```
[camera-mjpeg-stream] selftest ok
```

## Files

| File | What |
|---|---|
| `src/main.c` | Camera-capture + JPEG-encode loop (the app's main thread); pixfmt selection from `alp_jpeg_caps_t::pixfmt_mask`; DHCP kick-off + lease/URL printing. |
| `src/mjpeg_http.c` | The HTTP server: `GET /stream` + `GET /snapshot.jpg`, its own thread, `zsock_*` sockets. |
| `src/aen_eth_phy.c` | AEN-only, interim: PHY power/reset + refclk-mode bring-up (mirrors `examples/aen/aen-ethernet-link`); not linked on other targets. |
| `src/selftest.c` | native_sim-only CI selftest (loopback `GET /snapshot.jpg`, JPEG marker check). |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.overlay` | ISP graph rewiring (mirrors `aen-isp-ov5647-viewfinder`) + interim Ethernet RMII/PHY DT wiring (mirrors `aen-ethernet-link`). Content-identical across the two SKUs. |
| `boards/alp_e1m_aen80{1,3}_..._rtss_he.conf` | AEN hardware-path Kconfig (ISP pipeline, Hantro JPEG encoder, Ethernet DMA-region glue) — board-scoped so native_sim stays clean of undefined-symbol Kconfig warnings. Content-identical across the two SKUs. |

## Portability

Ring 1 by design: no `chips:` in `board.yaml`, camera path gated on
`alp_has(ALP_CAP_ID_HW_MIPI_CSI)` rather than SoC identity — the same
`src/main.c` and `src/mjpeg_http.c` build and run on any family, falling
back to the synthetic frame wherever there's no camera. `board.yaml` pins
`som.sku: E1M-AEN803` because the camera + JPEG hardware pipeline this
example demonstrates end to end is only populated on that family today
(`check_example_portability.py` classifies it Ring 3, SoM-bound, on that
basis — an accepted category, not a portability gap: see
`docs/portability.md` §4.4).
