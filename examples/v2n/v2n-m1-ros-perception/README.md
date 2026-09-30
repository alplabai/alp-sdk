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
              ├──── /alp/rail_3v3   sensor_msgs/BatteryState( 1 Hz)
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
- E1M-X-EVK board.  What the sensor topics read is what is fitted
  to it (`metadata/boards/e1m-x-evk.yaml`):
  - IMU: **ICM-42670** (U12, I2C `0x69`) on the sensor bus
    (`XEVK_I2C_BUS_SENSORS`).  The EVK has no LSM6DSO.  An unreworked
    EVK V2 also answers `0x69` with its BMI323 (U13), so the two IMUs
    collide there; if `/alp/imu` stays silent check for that.
  - Power: **INA236 U21** on the +3V3 rail (`0x40`, 20 mOhm shunt,
    calibrated from `XEVK_INA236_SHUNT_3V3_OHMS`).  This is a supply
    rail, not a battery.  On EVK V2 its bus-voltage register reads
    about 0 V; the current reading is the one to trust.
  - No GNSS receiver is fitted, so there is no `/alp/gnss` topic.
    `E1M_X_UART0` is the EVK console (`XEVK_UART_PORT_DEBUG`), never
    a GNSS port; add a GNSS on the Arduino-header UART
    (`XEVK_UART_PORT_ARDUINO`) in your own code if you need one.
  - Camera: the shipped image does not enable a camera and no sensor
    is fitted.  The capture path is kept, but `/alp/image` needs a
    camera module on a CSI connector plus enabling it (tracked in
    #1149).
- (Optional) external WiFi for remote ROS 2 graph access.

## Run

```bash
ros2 launch alp_perception perception.launch.py
ros2 topic list           # confirm /alp/* topics
ros2 topic echo /alp/imu  # 50 Hz Imu messages
ros2 topic echo /alp/rail_3v3  # 1 Hz +3V3 rail voltage/current
```

## Yocto packaging

A skeleton recipe lives under `recipes-ros/alp-perception_0.5.bb`
in `meta-alp-sdk`.  The recipe DEPENDS on `alp-sdk`,
`ros-rclcpp`, `ros-vision-msgs`, `ros-sensor-msgs`, and (on
V2N-M1) `dx-rt`.

## Verification status

`[UNTESTED]` -- builds clean against the Yocto SDK; HiL
bring-up gates on v0.8 V2M.  The `sensor_msgs` publishers
exercise the alp-sdk chip drivers (ICM-42670, INA236)
through their portable surfaces -- once V2N silicon is in the
lab, switching the topics' tick rate up is a one-line change
in `sensor_pubs.cpp`.
