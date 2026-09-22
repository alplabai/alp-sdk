# aen-camera-firstlight — RPi-style CSI-2 camera first light

First-light bench proof for the InnoMaker CAM-OV9281 Raspberry-Pi-style MIPI
CSI-2 camera module on the E1M-EVK's J5 connector, on an E1M-AEN801/AEN803
SoM (Alif Ensemble E8, M55-HE). Exercises the portable `<alp/camera.h>` API
only — open, start, capture-with-timeout, release, stop, close. **Bench-
verified** (2026-09-21, an E1M-AEN803 on the E1M-EVK: real GREY8 frames land
in memory in all three modes -- 640x400, 1280x720, 1280x800 -- each at its
configured frame rate, with the sensor test pattern also verified in all
three). See `docs/boards/e1m-evk.md`'s Camera section and
`docs/camera-shields.md`.

**This SoM/EVK combination needs a P/N-crossing adapter on the camera
connector.** Without one, the sensor answers its I2C probe but no frame
ever arrives — see the note in `docs/boards/e1m-evk.md`'s Camera section
for what to build.

## Build

```bash
ZEPHYR_BASE=<zephyr> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-camera-firstlight -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"            # OV9281, GREY8 640x400
```

The camera shield's Kconfig auto-enables `CONFIG_VIDEO_OV9281` (`default y`
under the sensor's devicetree node), and `src/main.c` picks the matching
width/height/format on that symbol.

## What each printed line means

```
=== aen-camera-firstlight: innomaker_cam_ov9281 (OV9281, GREY8 640x400) ===
[camfl] alp_camera_open(id=0, 640x400) ...
[camfl] alp_camera_open OK
[camfl] alp_camera_start -> ALP_OK
[camfl] alp_camera_capture: waiting up to 2000 ms for one frame ...
[camfl] alp_camera_capture OK: 256000 bytes @ 123456 us
[camfl]   CRC32 = 0xDEADBEEF
[camfl]   histogram (bin:count), darkest..brightest quarter:
[camfl]     [ 0] 12345
...
[camfl]   row 0    (offset 0): 12 34 56 ...
[camfl]   row mid  (offset 128000): 12 34 56 ...
[camfl]   row last (offset 255360): 12 34 56 ...
RESULT: capture ok
[camfl] alp_camera_stop -> ALP_OK
[camfl] alp_camera_close done
```

| Step | Line | Meaning |
|---|---|---|
| 1. open | `alp_camera_open FAILED: ALP_ERR_NOT_READY` | The sensor's chip-ID probe never answered on I2C1 during driver init — **not powered / not answering**. Check the module is seated on J5 and self-enabling (J5 pin 11 / `CAM_EN` must stay 0, see `docs/boards/e1m-evk.md`'s Camera section). A failed open is a valid, informative bench result — it is not a bug in this app. |
| 2. start | `alp_camera_start -> ALP_ERR_*` | Stream start rejected after a successful open — driver-level failure past the sensor probe; check the CSI-2 D-PHY / CPI clock programming (`fix(aen): program the CSI/CPI pixel-clock dividers...` on this branch). |
| 3. capture | `alp_camera_capture TIMED OUT` | Stream started but no frame landed in 2 s — the sensor answered its I2C probe but nothing is arriving over the CSI-2 lanes (bad lane count/polarity, D-PHY not locking, wrong pixel clock). The single most common cause on this SoM/EVK combination is the missing P/N-crossing camera-connector adapter (see `docs/boards/e1m-evk.md`'s Camera section) — without it the sensor still answers its I2C probe but no frame ever synchronizes. This app has no bench-diagnostic register readout for this path (see `src/main.c`'s comment at the timeout print: `video_csi_dw.c` exposes no public status query) — the timeout itself is the whole bench result today. |
| 3. capture | `alp_camera_capture OK: N bytes ...` | A frame arrived. CRC32 + the histogram + three sample rows follow. A non-zero CRC alone is weak evidence — a warm RAM-run can leave a previous frame in SRAM0, so a non-zero buffer does not by itself prove a *new* frame arrived. The real proof is a buffer pre-filled with a known sentinel (`0xA5`) coming back overwritten, plus the sensor's own test pattern appearing in the data when enabled — see the bench result below. |
| 4. stop/close | `alp_camera_stop` / `alp_camera_close done` | Always run, even after a failure above (except a failed `open`, which has nothing to stop/close). |

## Expected result

| Shield | Sensor | Format | Result |
|---|---|---|---|
| `innomaker_cam_ov9281` | OV9281 | GREY8 640x400 (this example); driver also offers 1280x720 and 1280x800 GREY8 | ADR 0017 Tier-1.5 port of the Espressif driver. **BENCH-VERIFIED 2026-09-21** on an E1M-AEN803 on the E1M-EVK, in all three modes: 640x400 (Espressif's), 1280x720 (Espressif's) and 1280x800 (Alp-authored, derived from the 1280x720 table) all captured live frames -- a `0xA5`-prefilled pool overwritten plus the sensor test pattern appearing, verified in all three -- each at its configured frame rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps). |

## Frame buffers live in SRAM0, not DTCM

The portable camera backend's `video_buffer_aligned_alloc()` pool defaults to a
2 MiB heap placed in the HE core's local DTCM — DTCM is only 256 KiB, so the
default does not fit, and even a right-sized pool would not be reachable
there: the CPI's AXI capture master cannot address core-local DTCM at all (the
same reachability gap the JPEG / DMA / Ethernet AEN examples found first).
`prj.conf` moves the pool into the global on-chip SRAM0 bank instead
(`CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION=y` +
`CONFIG_VIDEO_BUFFER_POOL_ZEPHYR_REGION_NAME="SRAM0"`), the same 4 MiB
AXI-visible bank at `0x02000000` those siblings use. Unlike those siblings this
example does **not** set `CONFIG_DCACHE=n`: `video_alif.c`'s own
enqueue/dequeue path already runs `sys_cache_data_flush_and_invd_range` /
`sys_cache_data_invd_range` on every buffer, so cache coherency is handled
per-buffer by the driver.

The pool itself is also a `sys_heap`: with the kernel's SRAM being the 256 KiB
DTCM, Zephyr defaults to `SYS_HEAP_SMALL_ONLY`, whose heaps top out at 262136
bytes — far short of this 2 MiB pool. `prj.conf` sets `CONFIG_SYS_HEAP_AUTO=y`
so the heap picker sizes each heap's chunk headers to fit the pool it is
actually given, instead of silently misbehaving at run time (4-byte-aligned
buffers, then `ALP_ERR_NOMEM` on the second frame).

## Compile proof in CI; real results on the bench

The `testcase.yaml` scenario is `build_only: true` regardless of bench
status — twister has no bench access, so a green build only proves the image
compiles and links against the real board target. The real result
(2026-09-21, an E1M-AEN803 on the E1M-EVK, J-Link RAM-run, same flow as the
sibling `*-regcheck` apps) is real GREY8 frames landing in memory in all
three modes, each at its configured frame rate.
