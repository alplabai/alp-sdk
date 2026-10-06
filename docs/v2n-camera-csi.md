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

Bench status: the OV9281 path (`innomaker_cam_ov9281`) is bench-proven on
E1M-V2M103; every other module is **BENCH-UNVERIFIED**. Each generated
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
(`raspberry_pi_global_shutter_camera`, patch `0018`) and the 2-lane
`innomaker_cam_imx335` (the native driver is 4-lane only; patch `0019`) get
their fragments, and IMX296 its config line, the moment those patch files
land and `gen_camera_dt.py` is re-run.

`clock-noncontinuous` (the OV9281 endpoint): the native 6.1 `ov9282.c`
ignores it and always writes the gated MIPI clock (`0x4800 = 0x20`); the
v6.6 backport of patch `0020` writes it only when the endpoint sets the
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
- CAM1: its control bus (E1M-X I2C3) terminates on the GD32 bridge.
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

The default image is dark (the `ov9282` driver defaults to exposure 642,
analogue gain 16). Raise them on the **sensor** subdev (find it with
`media-ctl -p`):

```
v4l2-ctl -d /dev/v4l-subdevN -c exposure=1000,analogue_gain=100
```

Pass: 10 frames, no CSI errors in `dmesg`. If `i2cdetect` sees nothing,
check the mux select (`gpioinfo | grep cam0-mux-sel` must read output,
low) and the module's I2C address.
