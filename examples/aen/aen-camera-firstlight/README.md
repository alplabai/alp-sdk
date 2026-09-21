# aen-camera-firstlight — RPi-style CSI-2 camera first light

First-light bench proof for Raspberry-Pi-style MIPI CSI-2 camera modules on
the E1M-EVK's J5 connector, on an E1M-AEN801/AEN803 SoM (Alif Ensemble E8,
M55-HE). Exercises the portable `<alp/camera.h>` API only — open, start,
capture-with-timeout, release, stop, close — the same four calls whichever
sensor shield is stacked underneath. The OV9281 path is **bench-verified**
(2026-09-21, an E1M-AEN803 on the E1M-EVK: real GREY8 frames land in
memory in all three modes -- 640x400, 1280x720, 1280x800 -- each at its
configured frame rate, with the sensor test pattern also verified in all
three); the IMX219, OV5647 and IMX296 paths have not yet run on real
silicon. See `docs/boards/e1m-evk.md`'s Camera section and
`docs/camera-shields.md`.

**This SoM/EVK combination needs a P/N-crossing adapter on the camera
connector.** Without one, the sensor answers its I2C probe but no frame
ever arrives — see the note in `docs/boards/e1m-evk.md`'s Camera section
for what to build.

## Build (one image per camera shield)

```bash
ZEPHYR_BASE=<zephyr> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-camera-firstlight -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_2"   # IMX219, RAW10 640x480

# ... or:
  -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"   # OV5647, RAW10 640x480
  -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"            # OV9281, GREY8 640x400
  -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" # IMX296, RAW10 1456x1088
```

Which shield is stacked selects the capture format at **compile time**: exactly
one of `CONFIG_VIDEO_IMX219` / `CONFIG_VIDEO_OV5647` / `CONFIG_VIDEO_OV9281` /
`CONFIG_VIDEO_IMX296` auto-enables (each `default y` under its sensor's
devicetree node), and `src/main.c`'s `#if` ladder on those same four symbols
picks the matching width/height/format. IMX296 has a single full-frame mode
(1456x1088 RAW10, 3,168,256 bytes unpacked -- the sensor transmits its
colour-processing margin around the 1440x1080 recording area), so this
example's `Kconfig` drops the backend to one frame buffer and grows the SRAM0
pool to 3.5 MiB for that shield only. A new shield is one more `#elif` plus
one more `testcase.yaml` scenario.

## What each printed line means

```
=== aen-camera-firstlight: raspberry_pi_camera_module_2 (IMX219, RAW10 640x480) ===
[camfl] alp_camera_open(id=0, 640x480) ...
[camfl] alp_camera_open OK
[camfl] alp_camera_start -> ALP_OK
[camfl] alp_camera_capture: waiting up to 2000 ms for one frame ...
[camfl] alp_camera_capture OK: 614400 bytes @ 123456 us
[camfl]   CRC32 = 0xDEADBEEF
[camfl]   histogram (bin:count), darkest..brightest quarter:
[camfl]     [ 0] 12345
...
[camfl]   row 0    (offset 0): 12 34 56 ...
[camfl]   row mid  (offset 307200): 12 34 56 ...
[camfl]   row last (offset 613760): 12 34 56 ...
RESULT: capture ok
[camfl] alp_camera_stop -> ALP_OK
[camfl] alp_camera_close done
```

| Step | Line | Meaning |
|---|---|---|
| 1. open | `alp_camera_open FAILED: ALP_ERR_NOT_READY` | The sensor's chip-ID probe never answered on I2C1 during driver init — **not powered / not answering**. Check the module is seated on J5 and self-enabling (J5 pin 11 / `CAM_EN` must stay 0, see `docs/boards/e1m-evk.md`'s Camera section). A failed open is a valid, informative bench result — it is not a bug in this app. |
| 2. start | `alp_camera_start -> ALP_ERR_*` | Stream start rejected after a successful open — driver-level failure past the sensor probe; check the CSI-2 D-PHY / CPI clock programming (`fix(aen): program the CSI/CPI pixel-clock dividers...` on this branch). |
| 3. capture | `alp_camera_capture TIMED OUT` | Stream started but no frame landed in 2 s — the sensor answered its I2C probe but nothing is arriving over the CSI-2 lanes (bad lane count/polarity, D-PHY not locking, wrong pixel clock). The single most common cause on this SoM/EVK combination is the missing P/N-crossing camera-connector adapter (see `docs/boards/e1m-evk.md`'s Camera section) — without it the sensor still answers its I2C probe but no frame ever synchronizes. This app has no bench-diagnostic register readout for this path (see `src/main.c`'s comment at the timeout print: `video_csi_dw.c` exposes no public status query) — the timeout itself is the whole bench result today. |
| 3. capture | `alp_camera_capture OK: N bytes ...` | A frame arrived. CRC32 + the histogram + three sample rows follow. A non-zero CRC alone is weak evidence — a warm RAM-run can leave a previous frame in SRAM0, so a non-zero buffer does not by itself prove a *new* frame arrived. The real proof (see the OV9281 bench pass below) is a buffer pre-filled with a known sentinel (`0xA5`) coming back overwritten, plus the sensor's own test pattern appearing in the data when enabled. |
| 4. stop/close | `alp_camera_stop` / `alp_camera_close done` | Always run, even after a failure above (except a failed `open`, which has nothing to stop/close). |

## Expected results per module

| Shield | Sensor | Format | Expected on this batch |
|---|---|---|---|
| `raspberry_pi_camera_module_2` | IMX219 | RAW10 640x480 | Upstream driver, compiled against but **not yet run on hardware** (`docs/boards/e1m-evk.md`) — bench result unknown; a clean `open` failing `NOT_READY` most likely means the module isn't seated/self-enabling, not a driver bug. |
| `raspberry_pi_camera_module_1` | OV5647 | RAW10 640x480 | ADR 0017 Tier-1 upstream-pending backport (see `docs/camera-shields.md`), BENCH-UNVERIFIED. |
| `innomaker_cam_ov9281` | OV9281 | GREY8 640x400 (this example); driver also offers 1280x720 and 1280x800 GREY8 | ADR 0017 Tier-1.5 port of the Espressif driver. **BENCH-VERIFIED 2026-09-21** on an E1M-AEN803 on the E1M-EVK, in all three modes: 640x400 (Espressif's), 1280x720 (Espressif's) and 1280x800 (Alp-authored, derived from the 1280x720 table) all captured live frames -- a `0xA5`-prefilled pool overwritten plus the sensor test pattern appearing, verified in all three -- each at its configured frame rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps). |
| `raspberry_pi_global_shutter_camera` | IMX296 | RAW10 1456x1088, 1 lane | ADR-0017-ADJACENT, written from the Sony datasheet, BENCH-UNVERIFIED. Probe reads back STANDBY's power-on default (the part has no chip-ID register). |

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

The four `testcase.yaml` scenarios are `build_only: true` regardless of bench
status — twister has no bench access, so a green build only proves the image
compiles and links against the real board target. The OV9281 shield's real
result (2026-09-21, an E1M-AEN803 on the E1M-EVK, J-Link RAM-run, same flow
as the sibling `*-regcheck` apps) is real GREY8 frames landing in memory in
all three modes, each at its configured frame rate; the other three shields
still need that same bench pass.
