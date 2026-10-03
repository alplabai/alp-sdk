# ROS 2 on the E1M-V2N / V2M (Cortex-A55, Yocto)

> **Status: BENCH-UNVERIFIED.** The wiring below is read-checked against the
> upstream layers' own `layer.conf` files; no image has been built from it and
> nothing here has run on silicon. The HIL spec
> ([`tests/hil/v2n101-x-evk/v2n101-ros2-som-temperature.yaml`](../tests/hil/v2n101-x-evk/v2n101-ros2-som-temperature.yaml))
> is not bench-verified either.

ROS 2 is the **Cortex-A / Yocto** half of the ADR 0018 robotics story
(ADR 0010: Zephyr on the M-cores, Yocto on the A-cores). Alp SDK does not fork
or re-generate ROS 2; it consumes the upstream and vendor layers and adds an
opt-in switch plus one portable-API example on top (ADR 0017).

## What is consumed, what is written

| Piece | Source | ADR 0017 tier |
|---|---|---|
| ROS 2 Humble recipes (`rclcpp`, messages, `ament_*`) | upstream [`ros/meta-ros`](https://github.com/ros/meta-ros), branch `scarthgap` (`meta-ros-common` + `meta-ros2` + `meta-ros2-humble`) | Tier-1, consumed as-is |
| RZ/V2N compat bbappends for that Humble set | Renesas [`renesas-rz/rzv_ros`](https://github.com/renesas-rz/rzv_ros), `yocto/meta-rz-features-ros/meta-rzv2-ros-humble` (collection `rzv2-ros-humble`) | Tier-1, consumed as-is, optional |
| `ALP_ENABLE_ROS2` switch, `packagegroup-alp-ros`, node recipes | `meta-alp-sdk` (`conf/layer.conf` `BBFILES_DYNAMIC`, `recipes-images/alp-image-common.inc`, `dynamic-layers/ros2-humble-layer/`) | Alp glue (no driver) |
| `alp_som_temperature` node | [`examples/v2n/v2n-ros2-som-temperature`](../examples/v2n/v2n-ros2-som-temperature/) | portable `<alp/temperature.h>` only |
| micro-ROS on the CM33 | [ADR 0035](adr/0035-micro-ros-cross-core-transport.md) (Proposed, no implementation) | n/a |

`rzv_ros` targets the same ROS distro and Yocto release the SDK already uses:
**Humble** on **scarthgap** (`LAYERSERIES_COMPAT_rzv2-ros-humble = "scarthgap"`,
`meta-ros2-humble` upstream). Its bbappends fix Renesas-BSP build problems of
that set (for example ament packages installing into `share/share`, the
darknet/DRP-AI sample recipes). We do **not** apply its `meta-ros-humble.patch`
(it edits `local.conf` and installs a fixed package list into every
`core-image-*`); the Alp images pick packages through the `alp-ros` feature
group instead.

## Layer wiring

`meta-alp-sdk` never hard-depends on ROS. Its ROS recipes live in
`dynamic-layers/ros2-humble-layer/` and are parsed only when the upstream
collection `ros2-humble-layer` is in `bblayers.conf`, so builds without
`meta-ros` parse and build as before (no `BBMASK` needed).

Add to the build, after the RZ AI SDK BSP v6.30 layers (see
[`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md) steps 1-5):

```bash
git clone -b scarthgap https://github.com/ros/meta-ros ../meta-ros
git clone https://github.com/renesas-rz/rzv_ros ../rzv_ros   # optional
bitbake-layers add-layer ../meta-ros/meta-ros-common
bitbake-layers add-layer ../meta-ros/meta-ros2
bitbake-layers add-layer ../meta-ros/meta-ros2-humble
# Optional Renesas compat layer. Add it ONLY together with meta-ros: its
# bbappends have no recipe to apply to otherwise and fail the parse.
bitbake-layers add-layer ../rzv_ros/yocto/meta-rz-features-ros/meta-rzv2-ros-humble
```

Pin both clones to the commits you validated; this page does not pin them
because no build has been run.

## Opt-in switch

| `ALP_ENABLE_ROS2` | Effect |
|---|---|
| unset (default) | `"1"` if `ros2-humble-layer` is in `bblayers.conf`, else `"0"` |
| `"1"` | `alp-ros` image feature honoured: `packagegroup-alp-ros` (rclcpp, message/transport stack, `alp-perception`, `alp-ros2-temperature`). Parse error if the layer is missing |
| `"0"` | `alp-ros` is removed from `IMAGE_FEATURES` even if an image requests it |

`alp-image-edge` requests `alp-ros`; `alp-image-base` / `alp-image-prod` do not
(add `IMAGE_FEATURES += "alp-ros"` for a ROS product image). On a deployed unit
constrain DDS discovery (see
[`build-yocto-v2n.md`](build-yocto-v2n.md) "Scope of the hardening").

## Build

Machines: `e1m-v2n101-a55`, `e1m-v2m103-a55` (every V2N/V2M SKU shares the
recipe set).

```bash
# conf/local.conf (optional; defaults on when meta-ros is present)
#   ALP_ENABLE_ROS2 = "1"
MACHINE=e1m-v2n101-a55 bitbake alp-image-edge
MACHINE=e1m-v2m103-a55 bitbake alp-image-edge
MACHINE=e1m-v2n101-a55 bitbake alp-image-edge -c populate_sdk   # colcon SDK
```

Build only the new node first when iterating:
`bitbake alp-ros2-temperature`.

### Out-of-image build (colcon in the Yocto SDK)

The example is an `ament_cmake` package that needs only `rclcpp`,
`sensor_msgs` and `libalp_sdk.so`:

```bash
source /opt/<distro>/environment-setup-cortexa55-<distro>-linux   # from populate_sdk
mkdir -p ~/ros_ws/src && cd ~/ros_ws/src
ln -s <alp-sdk>/examples/v2n/v2n-ros2-som-temperature alp_som_temperature
cd .. && colcon build --packages-select alp_som_temperature
```

## Run

```bash
ros2 run alp_som_temperature som_temperature_node
ros2 topic echo /alp/som_temperature      # sensor_msgs/Temperature, degC
```

`alp_temperature_read_milli_c()` is implemented on the Zephyr AEN backend only
today; the Linux build returns `ALP_ERR_NOSUPPORT` (see
[`include/alp/temperature.h`](../include/alp/temperature.h)). On V2N/V2M the
node therefore logs one NOSUPPORT warning and publishes nothing until a Linux
backend lands; the node source does not change when it does.

## Licence-gated vendor packages (not in this repo)

ROS 2 itself needs none. The Renesas packages that sit near it are gated and
never enter the public repo: the Mali GPU DDK (`meta-rz-graphics`; the
`rzv_ros` `mali-library` bbappend only applies where that layer is present),
the ISP support package, RUHMI and the DRP-AI translator. Public recipes only
reference them opt-in (`ALP_ENABLE_DRPAI` / `RUHMI_DRPAI_TVM_DIR`); Alp-built
images use the private mirror.

## micro-ROS on the CM33

Out of scope here. The decision record is
[ADR 0035](adr/0035-micro-ros-cross-core-transport.md): an XRCE custom
transport over the ADR 0016 RPMsg framed transport plus a micro-ROS agent on
Linux. Tracked in #373 / #375.

## Verification status

| Claim | Status |
|---|---|
| Layer names / distro / `scarthgap` compat | read-checked against upstream `layer.conf` files |
| Recipes parse without `meta-ros` | by construction (`BBFILES_DYNAMIC`); not run |
| `alp-image-edge` builds with ROS on V2N/V2M | not built |
| Node publishes on silicon | not run; HIL spec not bench-verified |
