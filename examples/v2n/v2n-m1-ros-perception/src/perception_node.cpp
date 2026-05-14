// Copyright 2026 ALP Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// alp_perception
// ==============
//
// ROS 2 perception node for V2N + V2N-M1.  On V2N-M1 the on-module
// DEEPX DX-M1 NPU runs the object-detection inference; on V2N (no
// DEEPX) the runtime falls back to the on-die DRP-AI3.  Either way
// the ROS 2 message contract is the same, so customer downstream
// nodes (navigation, planning, etc.) don't care which SoM is below.
//
//
// ── Topology ───────────────────────────────────────────────────
//
//   ┌─────────────────────────────────────────────────────────┐
//   │              alp_perception (this node)                  │
//   │                                                          │
//   │   ┌──────────┐ ┌──────────┐ ┌──────────────────┐         │
//   │   │ sensors  │ │ camera   │ │ DEEPX / DRP-AI   │         │
//   │   │ (LSM6DSO │ │ capture  │ │ object detector  │         │
//   │   │  GNSS    │ │ pipeline │ │ (cnn yolov*-style)│         │
//   │   │  INA236) │ │          │ │                  │         │
//   │   └─────┬────┘ └─────┬────┘ └────────┬─────────┘         │
//   │         │            │               │                   │
//   │         ▼            ▼               ▼                   │
//   │   /alp/imu        /alp/image    /alp/detections           │
//   │   /alp/gnss      (sensor_msgs)  (vision_msgs/             │
//   │   /alp/battery                    Detection2DArray)       │
//   │   (sensor_msgs)                                           │
//   └─────────────────────────────────────────────────────────┘
//
//                              ▲
//                              │  /alp/cmd_vel
//                              │  (geometry_msgs/Twist)
//                              │
//                       customer planning node
//
//
// ── How V2N vs V2N-M1 differs ──────────────────────────────────
//
// Both SKUs run the same Yocto image + the same ROS 2 graph.  The
// only difference is the backend the inference dispatcher resolves
// to at startup:
//
//   - V2N101 / V2N102 ->
//       alp_inference_open(backend=AUTO) -> ALP_INFERENCE_BACKEND_DRPAI
//   - V2M101 / V2M102 (V2N + DEEPX) ->
//       alp_inference_open(backend=AUTO) -> ALP_INFERENCE_BACKEND_DEEPX_DX
//
// The board.yaml's `inference: backend: deepx_dx` line ALSO emits
// CONFIG_ALP_SDK_INFERENCE_DEEPX=y in the alp.conf the Yocto recipe
// embeds.  On V2N101 builds (no DEEPX silicon), the dispatcher
// returns NOSUPPORT for DEEPX and AUTO falls through to DRP-AI.

#include <chrono>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>
#include <geometry_msgs/msg/twist.hpp>

#include "alp/peripheral.h"
#include "alp/inference.h"

#include "sensor_pubs.hpp"
#include "deepx_dispatch.hpp"

using namespace std::chrono_literals;

namespace alp {

/// Main ROS 2 node combining the sensor publishers + the object-
/// detection dispatcher.  All ROS traffic is asynchronous; sensor
/// reads happen on a 50 Hz timer + the inference loop runs on its
/// own thread (managed by DeepxDispatcher).
class PerceptionNode : public rclcpp::Node {
public:
    PerceptionNode() : Node("alp_perception") {
        RCLCPP_INFO(get_logger(),
                    "alp_perception starting up on V2N family");

        // Open the inference backend (AUTO -- the SDK picks DEEPX on
        // V2M, DRP-AI on V2N).  The dispatcher publishes detections
        // on /alp/detections.
        dispatcher_ = std::make_unique<DeepxDispatcher>(*this);

        // Bring up the sensor publishers (IMU + GNSS + battery).
        // sensor_pubs.cpp owns the alp_i2c_t handles + the 50 Hz
        // sample timer.
        sensors_ = std::make_unique<SensorPublishers>(*this);

        // Subscribe to /alp/cmd_vel so customer planning nodes can
        // hand us motion commands.  We don't drive motors directly
        // from here (that's the drone-autopilot demo's job); this
        // subscription proves the bidirectional ROS contract.
        cmd_sub_ = create_subscription<geometry_msgs::msg::Twist>(
            "/alp/cmd_vel", 10,
            [this](geometry_msgs::msg::Twist::SharedPtr msg) {
                RCLCPP_DEBUG(get_logger(),
                             "cmd_vel: linear=%.2f angular=%.2f",
                             msg->linear.x, msg->angular.z);
            });

        // Watchdog: log node-up + the resolved inference backend.
        watchdog_ = create_wall_timer(
            5s, [this] {
                RCLCPP_INFO(get_logger(),
                            "alive -- inference backend %s",
                            dispatcher_->backend_name());
            });
    }

private:
    std::unique_ptr<DeepxDispatcher>   dispatcher_;
    std::unique_ptr<SensorPublishers>  sensors_;
    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_sub_;
    rclcpp::TimerBase::SharedPtr       watchdog_;
};

}  // namespace alp

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<alp::PerceptionNode>());
    rclcpp::shutdown();
    return 0;
}
