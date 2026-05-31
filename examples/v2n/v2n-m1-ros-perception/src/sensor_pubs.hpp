// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// SensorPublishers: 50 Hz IMU + 1 Hz GNSS + 1 Hz battery monitor
// that pump sensor_msgs onto the ROS 2 graph.

#pragma once

#include <memory>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <sensor_msgs/msg/battery_state.hpp>

#include "alp/peripheral.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ublox_neo_m9n.h"
#include "alp/chips/ina236.h"

namespace alp {

class SensorPublishers {
public:
    explicit SensorPublishers(rclcpp::Node &parent);
    ~SensorPublishers();

private:
    void tick_imu();         // 50 Hz.
    void tick_slow_telem();  // 1 Hz GNSS + battery.

    rclcpp::Node                                              &parent_;
    rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr        imu_pub_;
    rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr  gnss_pub_;
    rclcpp::Publisher<sensor_msgs::msg::BatteryState>::SharedPtr batt_pub_;
    rclcpp::TimerBase::SharedPtr                               imu_timer_;
    rclcpp::TimerBase::SharedPtr                               telem_timer_;

    alp_i2c_t      *i2c_     = nullptr;
    alp_uart_t     *gps_uart_ = nullptr;
    lsm6dso_t       imu_     {};
    ublox_neo_m9n_t gps_     {};
    ina236_t        batt_    {};
};

}  // namespace alp
