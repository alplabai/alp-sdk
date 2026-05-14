# v2n-m1-ros-perception

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Builds against a
> Yocto SDK that includes ROS 2 + the alp-sdk runtime.  Real
> bring-up (V2N-M1 + DEEPX runtime + camera capture pipeline)
> gates on v0.8 V2M HiL.

ROS 2 perception node for **V2N + V2N-M1**.

## What it shows

- **V2N** (Renesas RZ/V2N quad-Cortex-A55 + Cortex-M33) acts as
  a ROS 2 compute node on Yocto Linux.
- **V2N-M1** adds a DEEPX DX-M1 NPU over PCIe.  Object detection
  runs on DEEPX; on V2N (no DEEPX) the dispatcher falls back to
  the on-die **DRP-AI3** automatically.
- The **same C++ source** builds for both SKUs.  Customers swap
  `som.sku` in `board.yaml` (`E1M-V2N101` ↔ `E1M-V2M101`) to
  retarget; the alp-sdk inference dispatcher resolves the right
  backend from the SoM's `capabilities:` block.

## ROS 2 graph

```
              ┌──── /alp/imu        sensor_msgs/Imu         (50 Hz)
              ├──── /alp/gnss       sensor_msgs/NavSatFix   ( 1 Hz)
              ├──── /alp/battery    sensor_msgs/BatteryState( 1 Hz)
              ├──── /alp/image      sensor_msgs/Image       (10 Hz)
   alp_       ├──── /alp/detections vision_msgs/Detection2DArray (10 Hz)
   perception │
   (this node)│
              └─◄── /alp/cmd_vel    geometry_msgs/Twist
                                    (from your planning node)
```

The `launch/perception.launch.py` shows how to remap `/alp/*`
into a wider `/robot/*` namespace.

## Build

This is a ROS 2 colcon package that lives inside the alp-sdk's
Yocto SDK environment.

```bash
# Inside the Yocto SDK's environment-setup-* shell:
source /opt/poky/4.0/environment-setup-cortexa55-poky-linux

# Build the ROS workspace containing this package:
cd ~/ros_ws/src
ln -s /path/to/alp-sdk/examples/v2n/v2n-m1-ros-perception alp_perception
cd ~/ros_ws
colcon build --packages-select alp_perception
```

## Hardware needed

- E1M-V2N101 or E1M-V2M101 SoM.
- E1M-X-EVK carrier with the camera connector populated.
- (Optional) external WiFi for remote ROS 2 graph access.

## Run

```bash
ros2 launch alp_perception perception.launch.py
ros2 topic list           # confirm /alp/* topics
ros2 topic echo /alp/imu  # 50 Hz Imu messages
```

## Yocto packaging

A skeleton recipe lives under `recipes-ros/alp-perception_0.5.bb`
in `meta-alp-sdk`.  The recipe DEPENDS on `alp-sdk`,
`ros-rclcpp`, `ros-vision-msgs`, `ros-sensor-msgs`, and (on
V2N-M1) `dx-rt`.

## Verification status

`[UNTESTED]` -- builds clean against the Yocto SDK; HiL
bring-up gates on v0.8 V2M.  The `sensor_msgs` publishers
exercise the alp-sdk chip drivers (LSM6DSO, NEO-M9N, INA236)
through their portable surfaces -- once V2N silicon is in the
lab, switching the topics' tick rate up is a one-line change
in `sensor_pubs.cpp`.
