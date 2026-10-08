# V2N / V2N-M1 MIPI CSI-2 camera path (Linux, opt-in, bench-unverified)

Issue #1149. The RZ/V2N has two MIPI CSI-2 receivers feeding the CRU
(R01UH1071, "Camera Data Receiver Unit (CRU)" chapter). Until now no
carrier DT node wired one to a sensor. This adds an **opt-in** Linux
path on the A55; the default dtb is unchanged.

**Status:** the OV9281 path is bench-proven on E1M-V2M103; every other
module is **BENCH-UNVERIFIED**. The generated fragments compile with dtc
against both cam0 wrapper dts; bitbake has not been run on them.

## What is wired

The camera devicetree and kernel config are **generated** from metadata by
`scripts/gen_camera_dt.py` (#2633); adding a camera is a metadata-only change
(`metadata/camera_modules/<module_id>.yaml` plus its chip's `drivers.linux`
block) followed by `python3 scripts/gen_camera_dt.py`.
`scripts/check_camera_parity.py` fails CI when a committed generated file
drifts from the metadata.

| Piece | File (`meta-alp-sdk/recipes-kernel/linux/linux-renesas/`) | Source |
|---|---|---|
| Sensor + CSI-2 + CRU0 fragment, one per connector x module | `e1m-x-evk-cam0-<module_id>.dtsi` (generated) | `metadata/camera_modules/`, `metadata/chips/<chip>.yaml` `drivers.linux`, `metadata/boards/e1m-x-evk.yaml` `camera_connectors`, SoM `pad_routes`, SoC `linux_dt` |
| Kernel config: CSI-2 receiver, CRU and every module's sensor driver, built in on every V2N/V2M machine | `camera-sensors.cfg` (generated) | SoC `linux_dt.CSI0.kconfig`, each chip's `drivers.linux.kconfig` |
| Board dtbs = base board + fragment | `e1m-v2n101-x-evk-cam0.dts`, `e1m-v2m101-x-evk-cam0.dts` | hand-written |
| Selection | `ALP_CAMERA_CAM0` in `linux-renesas_%.bbappend` | |

What the generator derives for a module on `CAM0`: the sensor compatible,
clock, supplies, reset property, `link-frequencies` and `clock-noncontinuous`
from the chip's `drivers.linux`; the I2C address and oscillator from the
module; the receiver / capture labels, the RIIC2 pin muxing and the GD32
bridge line of the mux-select hog from the SoM `pad_routes` and SoC
`linux_dt`; the sensor `*-supply` binding to a fixed always-on regulator and
`lane-polarities` from the connector (`supply`, `lane_polarity`). A pad the
SoM route records as `TBD` (CAM0 enable and reset) is listed in the
fragment's header as not modelled. A module that routes more lanes than the
connector carries gets no fragment.

Bench status: the OV9281 path (`innomaker_cam_ov9281`) and the IMX296LQ
path (`raspberry_pi_global_shutter_camera`: 1 lane, 54 MHz inck, RIIC2 at
400 kHz, 60 fps `SBGGR10_1X10` 1456x1088, zero CSI/CRU errors over 300
frames; colour / AWB not tuned) are bench-proven on E1M-V2M103; every other
module is **BENCH-UNVERIFIED**. The IMX296 fragment names `sony,imx296lq`
(`linux_compatible` in the module yaml) rather than the auto-detecting
`sony,imx296`: the module carries the colour part, and the explicit variant
skips the `SENSOR_INFO` read that returns `0x0000` right after standby
(patch 0028 adds the settle delay, but only the `lq` node was bench-run). Each generated
fragment compiles with dtc against both cam0 wrapper dts; bitbake has not
been run on them.

## Enable

```
# conf/local.conf: one module_id from metadata/camera_modules/
ALP_CAMERA_CAM0 = "innomaker_cam_ov9281"
```

An unknown id aborts the bake and lists the available ones. Bake as usual
(`docs/build-yocto-v2n.md`). The cam0 dtb is built next to the stock one
(`renesas/e1m-v2{n,m}101-x-evk-cam0.dtb`). Point the bootloader `fdtfile` at
it to use it; the stock dtb stays the fallback. Unset (the default) leaves
the shipped dtb unchanged.

A module only gets a fragment (and its sensor driver a line in
`camera-sensors.cfg`) when the BSP kernel can serve it, per
`metadata/os/linux-kernel-drivers.yaml`: the driver is native, or the alp
patch that adds it (or its lane count) exists in `linux-renesas/`. Today
`raspberry_pi_camera_module_2` (IMX219), `raspberry_pi_camera_module_1`
(OV5647) and `innomaker_cam_ov9281` (OV9281) qualify. The IMX296
(`raspberry_pi_global_shutter_camera`, patch `0028`) and the 2-lane
`innomaker_cam_imx335` (the native driver is 4-lane only; patch `0029`) get
their fragments, and IMX296 its config line, the moment those patch files
land and `gen_camera_dt.py` is re-run.

`clock-noncontinuous` (the OV9281 endpoint): the native 6.1 `ov9282.c`
ignores it and always writes the gated MIPI clock (`0x4800 = 0x20`); the
v6.6 backport of patch `0030` writes it only when the endpoint sets the
property. The chip's `endpoint_flags` carries it so the bench-proven gated
clock survives the driver swap.

## Not modelled (open, see the CSI TODO in `e1m-x-evk.dtsi`)

- CAM0 enable / reset (E1M IO18 / IO20, SoM route `TBD`) and the GD32-owned
  camera LDOs: Linux does not sequence sensor power. The mux select (IO16)
  is a GD32 bridge line and is hogged by the fragment.
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
1. Before baking, diff the `csi20` / `cru0` labels and OF-graph in the
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

## Sensor drivers available (#2618)

`camera-sensors.cfg` builds these drivers, plus the RZ/G2L-family CSI-2
receiver and CRU, into the kernel (`=y`) on every V2N/V2M machine -- built
in rather than as modules because the `alp-image-*` images install no
`kernel-modules` package -- so a camera works once its devicetree node is in the dtb,
without any `ALP_ENABLE_CAM0_*` switch. Every module with a generated CAM0
fragment (above) selects its node with `ALP_CAMERA_CAM0`; any other sensor
needs your own node on the CAM0 I2C bus and a `csi20` endpoint.

| Sensor | Kernel driver | Lanes | Modes / formats | Notes |
|---|---|---|---|---|
| OV9281 (mono) | `ov9282` (v6.6, patch 0030) | 2 | 1280x720, 1280x800, 640x400; `Y10_1X10` and `Y8_1X8` | Bench-proven at 1280x720 only |
| OV5647 | `ov5647` (in 6.1) | 2 | `SBGGR10_1X10` modes of the 6.1 driver | Unverified |
| IMX219 | `imx219` (in 6.1) | 2 | Bayer modes of the 6.1 driver | Unverified (#1149) |
| IMX296 (mono / colour) | `imx296` (v6.6 backport, patch 0028) | 1 | 1456x1088; `Y10_1X10` mono, `SBGGR10_1X10` colour | IMX296LQ (colour) bench-verified on E1M-V2M103 CAM0/J5 (1 lane, 54 MHz inck, SBGGR10 1456x1088, 60 fps, zero CSI/CRU errors over 300 frames); colour / AWB is not tuned. No external trigger (XTRIG) in the driver: free-running only. No `sony,imx296.yaml binding in 6.1, so `dtbs_check` does not validate the node |
| IMX335 | `imx335` (6.1 + patch 0029) | 2 or 4 | 2 lanes: 1296x972 `SRGGB10_1X10`; 4 lanes: 2592x1940 `SRGGB12_1X12` and 1296x972 `SRGGB10_1X10` | With 2 lanes the 12-bit full frame does not fit the link at the default HMAX, so it is hidden |

Devicetree each driver needs (all on the sensor's I2C node and its `port`
endpoint; lane polarity goes on the `csi20` endpoint, see patch 0017):

- **IMX296**: `compatible = "sony,imx296"` (or `sony,imx296ll` / `sony,imx296lq`
  to force mono / colour; plain `sony,imx296` auto-detects, which needs the settle delay patch 0028
  adds after leaving standby), `reg = <0x1a>`.
  `clocks` of 37.125, 54 or 74.25 MHz (`clock-names = "inck"`),
  `avdd-supply`, `dvdd-supply`, `ovdd-supply`, endpoint `data-lanes = <1>`.
  The driver does not parse the endpoint, so the lane count is only what
  the receiver endpoint says.
- **IMX335**: `compatible = "sony,imx335"`, `reg = <0x1a>`. A 24 MHz
  `clocks` entry (any other rate is rejected), `avdd-supply`, `ovdd-supply`,
  `dvdd-supply`, optional `reset-gpios`. Endpoint
  `data-lanes = <1 2>` (2 lanes) or `<1 2 3 4>` and
  `link-frequencies = /bits/ 64 <594000000>;` for both. The CRU captures
  the 10-bit binned mode as `CR10` with bytesperline `1296 * 8`. The
  2-lane 12-bit full frame is intentionally not offered.
- **OV9281 / OV9282**: endpoint `link-frequencies = /bits/ 64 <400000000>;`,
  `data-lanes = <1 2>`, and `clock-noncontinuous;` for the gated MIPI clock
  (without it the driver uses a continuous clock and different timing).

## OV9281 on CAM0 (J5), opt-in (#2612)

A monochrome OmniVision OV9281 (RPi-style 15-pin module) on the E1M-X-EVK
CAM0 connector. Same receiver path (`csi20` -> `cru0`), selected with
`ALP_CAMERA_CAM0 = "innomaker_cam_ov9281"`. Bench-proven on E1M-V2M103 (30/30
frames, 16.9 fps); needs patches 0016 and 0017.

Carrier facts the fragment encodes:

- CSI: J5 (2 lanes + clock) goes through a 2:1 CSI mux onto E1M-X CSI0.
  Mux select = E1M IO16 (`CAM0_MUX.SEL`), a GD32 bridge GPIO on V2N/V2M
  (line 7 of `gd32_gpio`, read from its `gpio-line-names`). It is hogged LOW
  (`cam0-mux-sel`) to pick J5.
- I2C: J5 -> level shifter -> E1M-X I2C2 (pads E63/E64) = RIIC2 (P34/P35)
  = `&i2c2`. Sensor at `0x60` (typical RPi OV9281); some modules use
  `0x70` -- change `i2c_addr_7bit` in the module YAML and regenerate if
  `i2cdetect` shows that.
- Camera enable (J5 pin 11, E1M IO18): not modelled. On V2M, IO18 is a
  DEEPX DX-M1 GPIO/strap, not a Linux GPIO; the carrier pull-down leaves
  the FET off.
- Power: J5 3V3 is always on. Clock: the module's own 24 MHz oscillator,
  represented by a `fixed-clock`.

`ov9282.c` facts (the v6.6 driver, backported by patch
`0030-media-i2c-ov9282-add-1280x800-and-640x400-modes.patch`): compatible
`ovti,ov9282` or `ovti,ov9281` (the fragment uses `ovti,ov9281`; the
OV9281 shares chip ID `0x9281`); 2 lanes; link frequency `400000000` Hz
only; three modes, **1280x720** (default), **1280x800** (full array) and
**640x400** (2x2 binned), each as `Y10_1X10` or `Y8_1X8`. The v6.6 driver
only writes the gated MIPI clock (`0x4800 = 0x20`) when the endpoint has
`clock-noncontinuous`; the bench-proven 0016/0017 run used the gated clock,
so the fragment sets it on `cam0_sensor_out`. It also requests
`avdd`/`dovdd`/`dvdd` supplies; none is described (J5 3V3 is always on), so
three "using dummy regulator" lines in `dmesg` are expected.
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
the generated fragment sets `lane-polarities = <1 1 1>;` (clock + 2
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

The other modes use the same recipe with the size changed, for example
the full array:

```
media-ctl -d /dev/media0 -V "'ov9282 <bus>-0060':0 [fmt:Y10_1X10/1280x800]"
media-ctl -d /dev/media0 -V "'<csi2 entity>':0 [fmt:Y10_1X10/1280x800]"
media-ctl -d /dev/media0 -V "'cru-ip-16000000.video':0 [fmt:Y10_1X10/1280x800]"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=800,pixelformat=CR10
```

(`640x400` likewise; `CR10` bytesperline is always `width * 8`.) The
non-720p modes and `Y8_1X8` are unverified on the bench.

The default image is dark (the `ov9282` driver defaults to exposure 642,
analogue gain 16). Raise them on the **sensor** subdev (find it with
`media-ctl -p`):

```
v4l2-ctl -d /dev/v4l-subdevN -c exposure=1000,analogue_gain=100
```

Pass: 10 frames, no CSI errors in `dmesg`. If `i2cdetect` sees nothing,
check the mux select (`gpioinfo | grep cam0-mux-sel` must read output,
low) and the module's I2C address.
