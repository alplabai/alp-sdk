# V2N / V2N-M1 MIPI CSI-2 camera path (Linux, opt-in, bench-unverified)

Issue #1149. The RZ/V2N has two MIPI CSI-2 receivers feeding the CRU
(R01UH1071, "Camera Data Receiver Unit (CRU)" chapter). Until now no
carrier DT node wired one to a sensor. This adds an **opt-in** Linux
path on the A55; the default dtb is unchanged.

**Status: BENCH-UNVERIFIED.** No sensor was on the bench and the DT has
not been through dtc. Nothing here is proof the pipeline streams.

## What is wired

| Piece | File (`meta-alp-sdk/recipes-kernel/linux/linux-renesas/`) |
|---|---|
| Sensor + CSI-2 + CRU0 fragment | `e1m-x-evk-cam0-imx219.dtsi` |
| Board dtbs = base board + fragment | `e1m-v2n101-x-evk-cam0.dts`, `e1m-v2m101-x-evk-cam0.dts` |
| Kernel config | `camera-csi.cfg` |
| Switch | `ALP_ENABLE_CAM0_IMX219` in `linux-renesas_%.bbappend` |

Sensor: Sony IMX219, 2 lanes, CCI `0x10` on E1M-X I2C2 (RIIC2), fixed
24 MHz xclk. The sensor is a **placeholder**: the carrier metadata names
no camera part (`metadata/boards/e1m-x-evk.yaml`, bare CSI connectors).

## Enable

```
# conf/local.conf
ALP_ENABLE_CAM0_IMX219 = "1"
```

Bake as usual (`docs/build-yocto-v2n.md`). The cam0 dtb is built next to
the stock one (`renesas/e1m-v2{n,m}101-x-evk-cam0.dtb`). Point the
bootloader `fdtfile` at it to use it; the stock dtb stays the fallback.

## Not modelled (open, see the CSI TODO in `e1m-x-evk.dtsi`)

- CAM0 enable / reset / mux GPIOs and the GD32-owned camera LDOs: no
  SoC-side crosswalk in tree, so Linux does not sequence sensor power.
- RIIC2 (P34/P35) A55 claim is not recorded in
  `metadata/e1m_modules/v2n/core-ownership.yaml`, so
  `check_amp_pad_claims.py` cannot see a CM33 conflict on those pads.
  Add a BENCH-PENDING entry once the cam0 dtb is bench-verified.
- CAM1: its control bus (E1M-X I2C3) terminates on the GD32 bridge; Linux
  reaches it as adapter `i2c3` through the bridge's I2C proxy (protocol >= 0.17).
- Connector-to-receiver and lane mapping (CAM0 -> receiver 0, 2 lanes
  is an assumption).
- The Zephyr/CM33 side (`src/backends/camera/v2n_n44_isp.c`) is a
  separate gap: no upstream Zephyr RZ/V CSI-2 driver. Untouched.

## First-boot runbook

0. Safety: the RIIC2 lines have no internal pull-up in the fragment.
   Confirm the CAM0 camera LDO is on (bridge GPIO, GD32-owned) before
   booting the cam0 dtb, and check whether the carrier fits external
   pull-ups; if it does not, add `bias-pull-up` back only after that.
1. Before baking, diff the `csi2` / `cru0` labels and OF-graph in the
   fragment against the OV5645 nodes in the BSP's
   `r9a09g056n48-rzv2n-evk.dts`; correct the fragment if they differ.
2. Boot the cam0 dtb (the image needs `i2c-tools` and `v4l-utils`; they
   are not BusyBox applets), then:
   ```
   dmesg | grep -iE "imx219|csi|cru"
   i2cdetect -y 2                     # expect 0x10 (UU once bound)
   media-ctl -p                       # find the CRU video node + entities
   ```
3. Set the pipeline (entity names come from `media-ctl -p`; format for
   IMX219 2-lane RAW10):
   ```
   media-ctl -d /dev/media0 -V "'imx219 2-0010':0 [fmt:SRGGB10_1X10/1920x1080]"
   media-ctl -d /dev/media0 -V "'<csi2 entity>':0 [fmt:SRGGB10_1X10/1920x1080]"
   ```
4. Capture:
   ```
   v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=RG10 \
            --stream-mmap --stream-count=30 --stream-to=/tmp/f.raw
   ```
   Pass: 30 frames, no `dmesg` CSI errors. Fail modes to record: no
   `0x10` on I2C (power/enable sequencing, not modelled), probe fails
   (wrong sensor / lanes), no frames (lane count or graph mismatch).

Report the outcome on #1149 before treating any of this as verified.

## OV9281 on CAM0 (J5), opt-in (#2612)

A monochrome OmniVision OV9281 (RPi-style 15-pin module) on the E1M-X-EVK
CAM0 connector. Same receiver path as the IMX219 above (`csi20` -> `cru0`),
selected with a second switch. Bench-proven on E1M-V2M103 (30/30 frames, 16.9 fps); needs patches 0016 and 0017.

```
# conf/local.conf  (mutually exclusive with ALP_ENABLE_CAM0_IMX219 -- the
# bake aborts if both are "1")
ALP_ENABLE_CAM0_OV9281 = "1"
```

The fragment is `e1m-x-evk-cam0-ov9281.dtsi`; `camera-csi.cfg` adds
`CONFIG_VIDEO_OV9282=y`. The default dtb is unchanged with the switch off.

Carrier facts the fragment encodes:

- CSI: J5 (2 lanes + clock) goes through a 2:1 CSI mux onto E1M-X CSI0.
  Mux select = E1M IO16 (`CAM0_MUX.SEL`), a GD32 bridge GPIO on V2N/V2M
  (line 7 of `gd32_gpio`). It is hogged LOW (`cam0-mux-sel`) to pick J5.
- I2C: J5 -> level shifter -> E1M-X I2C2 (pads E63/E64) = RIIC2 (P34/P35)
  = `&i2c2`. Sensor at `0x60` (typical RPi OV9281); some modules use
  `0x70` -- change `reg` if `i2cdetect` shows that.
- Camera enable (J5 pin 11, E1M IO18): not modelled. On V2M, IO18 is a
  DEEPX DX-M1 GPIO/strap, not a Linux GPIO; the carrier pull-down leaves
  the FET off.
- Power: J5 3V3 is always on. Clock: the module's own 24 MHz oscillator,
  represented by a `fixed-clock`.

6.1.141-cip43 `ov9282.c` facts: compatible is `ovti,ov9282` only (the
OV9281 shares chip ID `0x9281`); 2 lanes; link frequency `400000000` Hz
only; one mode, **1280x720 `Y10_1X10`** (no native 1280x800 mode).
Streaming needs kernel patch
`0016-media-rzg2l-cru-add-Y10-Y8-greyscale-formats.patch` (applied
unconditionally by `linux-renesas_%.bbappend`): without it the RZ/G2L CSI-2
receiver has no Y10 entry, `media-ctl -V` of `Y10_1X10` on the `csi20` pad
reads back as `UYVY8_1X16`, and `VIDIOC_STREAMON` fails with `-EPIPE`.
With the patch the CRU captures it as `CR10` (RAW10, 64-bit packed: six
10-bit pixels per 8 bytes, MSBs padded).

**Lane polarity (patch 0017).** On E1M-V2M103 + X-EVK J5 the CSI0 P/N
pairs arrive swapped at the RZ/V2N receiver. Without the swap the receiver
sees `CSI2nRXST = 0`, `DLST0`/`DLST1 = 0x4` (ECT ErrControl on the data
lanes), `PMST = 0x000F40CF` (clock-lane ULPS flags) and never a packet.
`e1m-x-evk-cam0-ov9281.dtsi` sets `lane-polarities = <1 1 1>;` (clock + 2
data lanes inverted) on the `csi20` endpoint, and
`0017-media-rzg2l-csi2-honour-lane-polarities-via-SWAPCTL.patch` (applied
unconditionally) programs `CRUm_SWAPCTL` from it: bit 5 `S_DPDN_SWAP_DAT`
(all data lanes), bit 4 `S_DPDN_SWAP_CLK` (clock lane), bits 1/0 left 0
(RZ/V2H manual R01UH1032EJ0130 section 9.2). The hardware swaps all data
lanes or none, so a mixed data-lane setting fails probe with `-EINVAL`.
Nothing needs to be written by hand. Where in the chain the swap
physically happens (camera FPC, carrier or SoM) is TBD.

Bench verification (needs `i2c-tools`, `v4l-utils`; find the RIIC2 bus
number with `ls -l /sys/bus/i2c/devices/` -- the adapter whose device
path contains `14400c00.i2c`):

```
dmesg | grep -iE "ov9282|csi|cru"
i2cdetect -y <riic2 bus>           # expect 0x60 (UU once bound)
media-ctl -p                       # CRU video node + entity names
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Set the pipeline (entity names from `media-ctl -p`):

```
media-ctl -d /dev/media0 -V "'ov9282 <bus>-0060':0 [fmt:Y10_1X10/1280x720]"
media-ctl -d /dev/media0 -V "'<csi2 entity>':0 [fmt:Y10_1X10/1280x720]"
media-ctl -d /dev/media0 -V "'cru-ip-16000000.video':0 [fmt:Y10_1X10/1280x720]"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=720,pixelformat=CR10
v4l2-ctl -d /dev/video0 --get-fmt-video   # 10240 bytesperline, 7372800 sizeimage
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 --stream-to=/tmp/f.raw
```

`CR10` bytesperline is `1280 * 8 = 10240` and sizeimage
`10240 * 720 = 7372800` (the driver reserves 8 bytes per pixel; the CRU
writes the 64-bit packed data into each line). Observed line layout: 1280 px
= 214 packed 64-bit words (1712 B) plus 2 trailing words, well inside the
10240 B bytesperline. An 8-bit greyscale sensor
(`Y8_1X8`) uses `pixelformat=GREY` instead.

The default image is dark (the `ov9282` driver defaults to exposure 642,
analogue gain 16). Raise them on the **sensor** subdev (find it with
`media-ctl -p`):

```
v4l2-ctl -d /dev/v4l-subdevN -c exposure=1000,analogue_gain=100
```

Pass: 10 frames, no CSI errors in `dmesg`. If `i2cdetect` sees nothing,
check the mux select (`gpioinfo | grep cam0-mux-sel` must read output,
low) and the module's I2C address.

## Using `<alp/camera.h>` on Linux

The portable `alp_camera_open()` works on the A55 through the V4L2 /
media-controller backend (`src/backends/camera/yocto_drv.c`). It is
sensor-agnostic: nothing in it names a sensor, so any camera with a mainline
V4L2 subdev driver and a media-graph path to a capture node works.
**Bench-unverified as a backend** (unit-tested against an ioctl hook; the
OV9281 Y10 -> `CR10` path above is the hardware-proven pipeline it targets).

`camera_id` N is resolved through a device-tree alias. The carrier or sensor
fragment **must** name the sensor node:

```
aliases {
	alp-camera0 = &ov9281;   /* the sensor's i2c node label */
};
```

Without that alias `alp_camera_open()` returns `ALP_ERR_NOT_READY` (as it
does for an alias with no probed sensor). The backend then scans every
`/dev/media*`, finds the `MEDIA_ENT_F_CAM_SENSOR` entity bound to that node,
follows the enabled links to the capture node, and never changes a link.

| Request | Result |
|---|---|
| `ALP_PIXFMT_GREY8` | Y8 sensor passes through; Y10 (`CR10`) is unpacked and `>>2` |
| `ALP_PIXFMT_RAW8` | Bayer8 / Y8 passes through |
| `ALP_PIXFMT_RAW10` | Bayer10 / Y10 (`CR10`) unpacked to one `uint16` per pixel |
| colour (RGB/YUV) | `ALP_ERR_NOSUPPORT` (no ISP in the path) |
| width x height | must be a size the sensor produces natively, else `ALP_ERR_INVAL` |
| `fps` | a request: VBLANK is clamped to the sensor's range; read the settled rate with `alp_camera_get_fps()` |

**Bayer colour sensors (IMX296LQ colour).** Without the ISP a colour Bayer
sensor is served as `ALP_PIXFMT_RAW10` (one `uint16` per pixel, still the
mosaic); `RGB565` and `GREY8` stay `ALP_ERR_NOSUPPORT` and `RAW8` needs an
8-bit Bayer code the sensor does not offer. The backend does not demosaic.
Bench facts this path is written against (E1M-V2M103, IMX296LQ colour on
CAM0/J5, 2026-10-08, observed with the V4L2 tools; the backend itself has not
yet run on the board): the chain is `imx296 9-001a`:0 ->
`csi-16000400.csi20`:0/1 -> `cru-ip-16000000.vide0`:0/1 -> `CRU output`
(`/dev/video0`) with every link immutable and enabled; the sensor emits
`SBGGR10_1X10/1456x1088`; the capture fourcc is `CR10`, 11648 bytes per line,
12673024 bytes per frame; `CR10` packs 6 pixels per little-endian 64-bit word,
LSB first, with 4 padding bits; the stream runs at 60.04-60.10 fps.

A frame the kernel marks corrupt (`V4L2_BUF_FLAG_ERROR`) or short is dropped
and `alp_camera_capture()` returns `ALP_ERR_IO`. `configure_isp` is
`ALP_ERR_NOSUPPORT`. RAW8 Bayer fourccs are unverified against the CRU format
table.
