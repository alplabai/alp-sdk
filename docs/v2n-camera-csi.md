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
