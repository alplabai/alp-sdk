// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// SensorPublishers: 50 Hz IMU + 1 Hz 3V3-rail monitor that pump
// sensor_msgs onto the ROS 2 graph.  Targets the E1M-X EVK's on-board
// parts (ICM-42670 IMU, INA236 U21) -- see sensor_pubs.cpp.

#pragma once

#include <memory>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/battery_state.hpp>

#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/ina236.h"

namespace alp
{

class SensorPublishers
{
  public:
	explicit SensorPublishers(rclcpp::Node &parent);
	~SensorPublishers();

  private:
	void tick_imu();        // 50 Hz.
	void tick_slow_telem(); // 1 Hz 3V3-rail monitor.

	rclcpp::Node                                                &parent_;
	rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr          imu_pub_;
	rclcpp::Publisher<sensor_msgs::msg::BatteryState>::SharedPtr rail_pub_;
	rclcpp::TimerBase::SharedPtr                                 imu_timer_;
	rclcpp::TimerBase::SharedPtr                                 telem_timer_;

	alp_i2c_t *i2c_ = nullptr;
	icm42670_t imu_{};
	ina236_t   rail_{};
};

} // namespace alp
