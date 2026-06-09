// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// DeepxDispatcher implementation -- the bridge from the alp-sdk
// inference dispatcher (which picks DEEPX vs DRP-AI vs CPU per
// SoM) to the ROS 2 vision_msgs Detection2DArray topic.

#include "deepx_dispatch.hpp"

#include <chrono>

#include <std_msgs/msg/header.hpp>

namespace alp
{

namespace
{
constexpr auto kCaptureInterval = std::chrono::milliseconds(100); // 10 Hz.
}

DeepxDispatcher::DeepxDispatcher(rclcpp::Node &parent) : parent_(parent)
{
    // Publish topics.
    det_pub_ = parent_.create_publisher<vision_msgs::msg::Detection2DArray>("/alp/detections", 10);
    img_pub_ = parent_.create_publisher<sensor_msgs::msg::Image>("/alp/image", 5);

    // Open the inference backend.  AUTO -> the SDK picks the
    // highest-priority backend the active SoM populates:
    //   V2M101 / V2M102 -> ALP_INFERENCE_BACKEND_DEEPX_DXM1
    //   V2N101 / V2N102 -> ALP_INFERENCE_BACKEND_DRPAI
    //   Anything else / no NPU -> ALP_INFERENCE_BACKEND_CPU
    // Field names + order match alp_inference_config_t in
    // <alp/inference.h>: model_data, model_size, format, backend,
    // arena_bytes, arena.
    const alp_inference_config_t inf_cfg = {
        .model_data = nullptr, // Customer drops their model into
                               // /etc/alp/models/perception.dxnn at
                               // image-bake time.
        .model_size = 0,
        .format     = ALP_INFERENCE_MODEL_DXNN, // DEEPX-native;
                                                // dispatcher transcodes for
                                                // DRP-AI if needed.
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .arena_bytes = 0,
        .arena       = nullptr,
    };
    inf_ = alp_inference_open(&inf_cfg);
    if (inf_ == nullptr) {
        RCLCPP_WARN(parent_.get_logger(),
                    "alp_inference_open returned NULL -- detections topic will be empty");
        backend_ = ALP_INFERENCE_BACKEND_CPU;
    } else {
        // We requested AUTO; the SDK resolved a concrete backend
        // internally.  <alp/inference.h> does not currently expose a
        // public accessor to read that resolved backend back, so we
        // leave backend_ at AUTO and backend_name() reports "auto".
        // (TODO: switch to an alp_inference_get_backend() accessor if
        // one is added to the public header.)
        backend_ = ALP_INFERENCE_BACKEND_AUTO;
    }

    thread_ = std::thread(&DeepxDispatcher::capture_loop, this);
}

DeepxDispatcher::~DeepxDispatcher()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (inf_) alp_inference_close(inf_);
}

const char *DeepxDispatcher::backend_name() const
{
    switch (backend_) {
    case ALP_INFERENCE_BACKEND_DEEPX_DXM1:
        return "deepx_dxm1";
    case ALP_INFERENCE_BACKEND_DRPAI:
        return "drpai";
    case ALP_INFERENCE_BACKEND_ETHOS_U:
        return "ethos_u";
    case ALP_INFERENCE_BACKEND_CPU:
        return "cpu";
    case ALP_INFERENCE_BACKEND_AUTO:
        return "auto"; // resolved by SDK; not readable back
    default:
        return "stub";
    }
}

void DeepxDispatcher::capture_loop()
{
    while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kCaptureInterval);

        // Capture frame from /dev/video0 (V4L2; on V2N the GStreamer
        // pipeline lands the MIPI sensor data here).  v0.5 stubs the
        // path; v0.6 wires libcamera or v4l2 capture through
        // <alp/camera.h>.
        sensor_msgs::msg::Image img_msg;
        img_msg.header.stamp    = parent_.now();
        img_msg.header.frame_id = "camera_link";
        img_msg.height          = 240;
        img_msg.width           = 240;
        img_msg.encoding        = "rgb8";
        img_msg.step            = img_msg.width * 3;
        img_msg.data.assign(img_msg.height * img_msg.step, 0);
        img_pub_->publish(img_msg);

        // Inference -- real path runs the dxnn model through the
        // DEEPX runtime.  v0.5 emits an empty detection array so
        // the topic shows up in `ros2 topic list`.
        vision_msgs::msg::Detection2DArray det_msg;
        det_msg.header = img_msg.header;
        det_pub_->publish(det_msg);
    }
}

} // namespace alp
