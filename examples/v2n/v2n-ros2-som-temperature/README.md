# v2n-ros2-som-temperature

The smallest useful Alp SDK + ROS 2 node. It calls **one portable function**,
`alp_temperature_read_milli_c()` from `<alp/temperature.h>`, and republishes
the value as `sensor_msgs/Temperature` on `/alp/som_temperature`.

> **`[UNTESTED]`** -- builds against a Yocto SDK that has ROS 2 Humble and
> the alp-sdk runtime; not built in CI, not run on silicon. The HIL spec
> (`tests/hil/v2n101-x-evk/v2n101-ros2-som-temperature.yaml`) is not
> bench-verified.

```
alp_temperature_read_milli_c()  -->  /alp/som_temperature
   (portable Alp SDK API)            sensor_msgs/Temperature [degC]
```

## What it shows

- ROS 2 on the **Cortex-A55 / Yocto** side, data from the **portable** API
  only -- no chip driver, no vendor header, no SoM `#ifdef`. The same source
  runs on any E1M SoM that carries Linux.
- Graceful degradation by status code: `ALP_OK` publishes,
  `ALP_ERR_NOSUPPORT` logs once and stays quiet, transient errors retry.
- Parameters: `period_ms` (default 1000), `frame_id` (default `som`).

## Coverage today

`alp_temperature_read_milli_c()` is implemented on the Zephyr AEN backend
only. The Linux/Yocto build returns `ALP_ERR_NOSUPPORT`, so on V2N/V2M this
node logs one warning and publishes nothing until a Linux backend lands. The
node source will not change when it does. (For a V2N node that reads real
sensors today, see [`v2n-m1-ros-perception`](../v2n-m1-ros-perception/).)

## Build

Two ways, same `CMakeLists.txt`:

1. **Yocto image** -- `MACHINE=e1m-v2n101-a55 bitbake alp-image-edge`
   with `meta-ros2-humble` in `bblayers.conf` (`ALP_ENABLE_ROS2`, see
   [`docs/bring-up-ros2.md`](../../../docs/bring-up-ros2.md)). The node
   ships as the `alp-ros2-temperature` package in `packagegroup-alp-ros`.
2. **colcon in the Yocto SDK** (`bitbake alp-image-edge -c populate_sdk`):

```bash
source /opt/<distro>/environment-setup-cortexa55-<distro>-linux
mkdir -p ~/ros_ws/src && cd ~/ros_ws/src
ln -s <alp-sdk>/examples/v2n/v2n-ros2-som-temperature alp_som_temperature
cd .. && colcon build --packages-select alp_som_temperature
```

## Run

```bash
ros2 run alp_som_temperature som_temperature_node
ros2 topic echo /alp/som_temperature
```
