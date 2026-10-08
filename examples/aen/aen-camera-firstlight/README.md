# aen-camera-firstlight — RPi-style CSI-2 camera first light

First-light bench proof for Raspberry-Pi-style MIPI CSI-2 camera modules on
the E1M-EVK's J5 connector, on an E1M-AEN801/AEN803 SoM (Alif Ensemble E8,
M55-HE). Exercises the portable `<alp/camera.h>` API only — open, start,
capture-with-timeout, release, stop, close — the same four calls whichever
sensor shield is stacked underneath. **OV9281 and OV5647 are fully
bench-verified**: OV9281 (2026-09-21, an E1M-AEN803 on the E1M-EVK: real
GREY8 frames land in memory in all three modes -- 640x400, 1280x720,
1280x800 -- each at its configured frame rate, with the sensor test pattern
also verified in all three) and OV5647 (2026-09-22, an E1M-AEN803 on the
E1M-EVK, issue #2248, RAW10 640x480). Both are due for a re-bench after
issue #2287 Stage B changed the shared CPI driver's buffer-starvation-pause
behaviour -- re-bench on these two sensors is pending. **IMX296 is Stage A
+ Stage B bench-verified** (issue #2287): Stage A through this app --
I2C identity (bench run 229), CSI-2 streaming, and a real 1456x1088 RAW10
frame (bench run 292; 0.98 correlation against a diag control capture);
Stage B through `examples/aen/aen-isp-capture` (ISP-Pico data path, run 297;
auto-exposure, runs 309/310) and `examples/connectivity/camera-mjpeg-stream`
(continuous MJPEG streaming, runs 312-314). Fast-trigger mode is added but
unbenched, and IMX296 colour (AWB/CCM) is not calibrated. **IMX335 raw
capture is bench-verified** (issue #2327, same unit + EVK, runs 316-331:
6/6 consecutive clean 1296x972 RAW10 frames (kept frame: 0 CSI/IPI errors; the
discarded first frame of each start reports one `SEQ_FRAME_FATAL`, status `0x1`), re-confirmed
on the committed product code in run 331); frame rate/fps, ISP/AE/colour
and the sensor's full-resolution mode remain
unverified. See `docs/boards/e1m-evk.md`'s Camera section and
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
  -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"   # OV5647, RAW10 640x480

# ... or:
  -DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"            # OV9281, GREY8 640x400
  -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" # IMX296, RAW10 1456x1088
  -DSHIELD="e1m_evk_rpi_csi innomaker_cam_imx335"            # IMX335, RAW10 1296x972 (2x2-binned)
```

Which shield is stacked selects the capture format at **compile time**: exactly
one of `CONFIG_VIDEO_OV5647` / `CONFIG_VIDEO_OV9281` / `CONFIG_VIDEO_IMX296` /
`CONFIG_VIDEO_IMX335` auto-enables (each `default y` under its sensor's
devicetree node), and
`src/main.c`'s `#if` ladder on those same symbols picks the matching
width/height/format. IMX296 has a single full-frame mode (1456x1088 RAW10,
3,168,256 bytes unpacked -- the sensor transmits its colour-processing
margin around the 1440x1080 recording area), so this example's `Kconfig`
drops the backend to one frame buffer and grows the SRAM0 pool to 3.5 MiB
for that shield only. IMX335 requests its 2x2-binned mode explicitly
(1296x972 RAW10, 2,519,424 bytes unpacked -- the driver boots at its native
2592x1944), and this example's `Kconfig` similarly drops the backend to one
buffer and grows the pool to 2.75 MiB for that shield only; the first
captured frame is discarded for this sensor (bench runs 329-331 found the
first post-STANDBY frame bad in 3 of 4 checks -- run 330's was the one
clean exception -- while the kept second frame was clean every time). A
new shield is one more `#elif` plus one more `testcase.yaml` scenario.

## What each printed line means

```
=== aen-camera-firstlight: raspberry_pi_camera_module_1 (OV5647, RAW10 640x480) ===
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

## Opt-in: external trigger (IMX296 only, UNBENCHED)

`-DAEN_CAMERA_TRIGGER=ON` (issue #2287) puts IMX296 in its datasheet Fast
Trigger Mode instead of free-run, and pulses a GPIO (Alif P5_1 / Arduino D4 /
`EVK_PIN_CK_DIO4`, see `trigger_gpio.overlay`) that must be wired to
the **sensor module's own** J3 Trig+ header (on the INNO-MAKER module itself,
not the E1M-EVK carrier) to capture, timestamp (pulse time vs. frame arrival
time) and content-check `TRIGGER_FRAME_COUNT` (3) frames instead of one
free-run capture. Only meaningful with the IMX296 shield; compiles clean (0
warnings) with any other shield, since `CONFIG_VIDEO_IMX296` gates the
trigger code path too. **Not benched by this change** -- see
`docs/camera-shields.md`'s IMX296 driver section.

> **HARDWARE CAUTION -- do not wire J3 yet.** This mode's electrical
> polarity is unverified: the INNO-MAKER module's J3 Trig+/Trig- input
> circuit (opto-isolated? logic-level? which voltage? current-limited on the
> E1M side?) has not been checked against that module's own documentation.
> `trigger_gpio.overlay`'s `GPIO_ACTIVE_HIGH` flag on
> `imx296-trigger-gpios` is a placeholder, not a confirmed fact, and is the
> one place to flip if the module's documentation (or a bench measurement)
> says the polarity is inverted. Confirm the module's own J3 documentation
> before connecting anything to it.

A `RESULT: capture ok` line alone does **not** prove the trigger actually
worked -- it only means `alp_camera_capture()` returned a frame-shaped
buffer before its timeout, which free-run capture on a mis-wired/unwired
trigger line could also do if the sensor happens to still be streaming from
a prior state. This mode's per-frame print instead reports the pulse
timestamp against the frame's own arrival timestamp (a frame that arrives
close to the pulse, not just at some arbitrary free-running interval, is
better evidence) and a content check that flags two stuck-data signatures
(every sample `0x3FF`, or every sample identical) -- still not proof of a
real triggered image, only a cheaper way to rule out the most obvious
failure modes before trusting the capture.

```bash
... -DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera" -DAEN_CAMERA_TRIGGER=ON
```

## Expected results per module

| Shield | Sensor | Format | Expected on this batch |
|---|---|---|---|
| `raspberry_pi_camera_module_1` | OV5647 | RAW10 640x480 | ADR 0017 Tier-1 upstream-pending backport (see `docs/camera-shields.md`). **BENCH-VERIFIED** (2026-09-22, an E1M-AEN803 on the E1M-EVK, issue #2248), needs the J5 pin-11 pull-up rework (`docs/boards/e1m-evk.md`). Due for a re-bench after issue #2287 Stage B changed the shared CPI driver's buffer-starvation-pause behaviour -- re-bench pending. |
| `innomaker_cam_ov9281` | OV9281 | GREY8 640x400 (this example); driver also offers 1280x720 and 1280x800 GREY8 | ADR 0017 Tier-1.5 port of the Espressif driver. **BENCH-VERIFIED 2026-09-21** on an E1M-AEN803 on the E1M-EVK, in all three modes: 640x400 (Espressif's), 1280x720 (Espressif's) and 1280x800 (Alp-authored, derived from the 1280x720 table) all captured live frames -- a `0xA5`-prefilled pool overwritten plus the sensor test pattern appearing, verified in all three -- each at its configured frame rate (measured 60-frame bursts: 640x400 ~100 fps, 1280x720 ~50 fps, 1280x800 ~100 fps). Due for a re-bench after issue #2287 Stage B changed the shared CPI driver's buffer-starvation-pause behaviour -- re-bench pending. |
| `raspberry_pi_global_shutter_camera` | IMX296 | RAW10 1456x1088, 1 lane | ADR-0017-ADJACENT, written from the Sony datasheet (issue #2287). **Stage A + Stage B bench-verified** (Stage A through this app -- I2C identity, bench run 229, 2026-09-24: the module answers at CCI 0x1A and the undocumented SENSOR_INFO signature 0x3148/0x3149 = 0x4A00 matches the colour IMX296LQR-C variant this driver targets; and a real captured frame, bench run 292 -- mean 61.19, max 108, clean close, 0.98 correlation against a diag control capture; Stage B via `aen-isp-capture` / `camera-mjpeg-stream` -- ISP-Pico data path, AE, continuous MJPEG streaming) -- see `docs/camera-shields.md`'s IMX296 driver section for the full bench numbers. Still unverified: colour (AWB/CCM), fast-trigger mode. |
| `innomaker_cam_imx335` | IMX335 | RAW10 1296x972 (2x2-binned), 2 lanes | ADR 0017 Tier 1, upstream-native, plus one repo patch (issue #2327 -- see `docs/camera-shields.md`'s IMX335 driver section). **Raw capture bench-verified** (E1M-AEN803 2026W36-0001, runs 316-331): 6/6 consecutive clean 1296x972 RAW10 frames (run 330), 0 CSI CRC errors, 0 IPI-fatal events on the kept frame (the discarded first frame shows one `SEQ_FRAME_FATAL`, status `0x1`, per start -- expected), correct stride, no overrun -- this also verifies 2-lane D-PHY lock at 1188 Mbps/lane on this unit/EVK through the SoM R2 pinout adapter. Run 331 re-confirmed this on the committed product code (patch 0004 only, no diag code). The first post-start frame was bad in 3 of the 4 first frames checked (run 329 and both run-331 loads; run 330's was clean); the kept second frame was clean every time, and is what this example reports -- this bench-justifies discarding the first frame. Still unverified: frame rate/fps, ISP/AE/colour, full-resolution mode, self-enable on a stock (non-reworked) carrier, D-PHY lock on other units/carriers. |

## Expected log line: one `SEQ_FRAME_FATAL` per stream start

Each stream start prints one `E: Fatal Interrupt due to incorrect frame
sequence for a specific VC. status - 0x1` (`SEQ_FRAME_FATAL`) on the
discarded first frame -- bench-seen on 3/3 IMX335 runs on 2026-10-07, and also
at ISP stream start. It is expected: the first frame is dropped and the kept
frame has 0 CSI/IPI errors. Any other CSI/IPI error, or one on the kept frame,
is a real fault.

## Troubleshooting: `ALP_ERR_NOSUPPORT` on `alp_camera_open`

Log signature (CSI path, e.g. IMX335 on E1M-AEN803):

```
E: Failed to set CSI pixel clock rate! ret - -134
E: Zephyr clock_control_alif lacks CSI pixel-clock set_rate -- alp-sdk zephyr/patches.yml patch 0001 not applied; run scripts/bootstrap.sh ...
alp_camera_open FAILED: ALP_ERR_NOSUPPORT
```

Cause: the Zephyr workspace does not carry alp-sdk's `zephyr/patches.yml`
patches. `west update` / `west build` never apply them and an unpatched build
still links, so upstream's `clock_control_alif_set_rate()` returns
`-ENOTSUP` (-134) for the CSI pixel clock. Since #2766 the configure step
refuses such a build with a `CMake Error` naming the missing patch; if you only
see the runtime signature, you built with `-DALP_SKIP_PATCH_CHECK=ON` or from
an older SDK.

Fix, from the workspace root, then rebuild with `-p always`:

```sh
bash scripts/bootstrap.sh            # applies + verifies every patch
# or just the Zephyr ones:
west patch --dst-module zephyr apply
python3 scripts/verify_west_patches.py   # must exit 0
```

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

All six `testcase.yaml` scenarios are `build_only: true` regardless of
bench status — twister has no bench access, so a green build only proves the
image compiles and links against the real board target. The OV9281 shield's
real result (2026-09-21, an E1M-AEN803 on the E1M-EVK, J-Link RAM-run, same
flow as the sibling `*-regcheck` apps) is real GREY8 frames landing in memory
in all three modes, each at its configured frame rate; the OV5647 shield's
real result (2026-09-22, issue #2248) is a live RAW10 640x480 capture on the
same board. The IMX296 shield's real result (issue #2287, bench run 292) is
a live RAW10 1456x1088 capture on the same board -- mean pixel value 61.19,
max 108, a clean close and no mod-4-column pattern in the captured image,
0.98 correlation against a diag control capture (run 293). Run 292's build
had `CONFIG_LOG` unset (no log output at all), so its clean console says
nothing about `INT_IPI_PIXEL_IF_HLINE_ERR`/`_FIFO_OVERFLOW` -- their status
on the product build is UNKNOWN, not "not seen"; the HLINE_ERR counts cited
elsewhere in this repo were measured in earlier diag runs (logging
enabled), before the fixes, and continuous streaming on the product
(logging-enabled) build is still open. The IMX335 shield's real result
(issue #2327, bench run 330, same unit/EVK) is 6/6 consecutive live RAW10
1296x972 captures with 0 CSI CRC errors and 0 IPI-fatal events on the kept frame (the discarded first frame of each start reports one `SEQ_FRAME_FATAL`, status `0x1`) -- frame
rate/fps was not measured on this path. Run 331 re-confirmed this same
result on the exact committed product code (patch 0004 only, no diag-only
code): a clean second capture, `CSI_PIXCLK_CTRL` reading back `0x00020001`.
