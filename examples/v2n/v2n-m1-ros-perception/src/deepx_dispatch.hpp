// Copyright 2026 ALP Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// DeepxDispatcher: bridges the alp-sdk inference dispatcher to a
// ROS 2 Detection2DArray topic.  Picks DEEPX on V2N-M1, DRP-AI on
// V2N (no DEEPX).

#pragma once

#include <atomic>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>
#include <sensor_msgs/msg/image.hpp>

#include "alp/inference.h"

namespace alp {

class DeepxDispatcher {
public:
    explicit DeepxDispatcher(rclcpp::Node &parent);
    ~DeepxDispatcher();

    /// Returns "deepx_dxm1" / "drpai" / "cpu" / "stub" depending on
    /// which backend the AUTO selector resolved to at startup.
    const char *backend_name() const;

private:
    void capture_loop();    // Owns its thread.

    rclcpp::Node &parent_;
    rclcpp::Publisher<vision_msgs::msg::Detection2DArray>::SharedPtr det_pub_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr            img_pub_;

    alp_inference_t        *inf_     = nullptr;
    alp_inference_backend_t backend_ = ALP_INFERENCE_BACKEND_AUTO;
    std::thread             thread_;
    std::atomic<bool>       stop_{false};
};

}  // namespace alp
