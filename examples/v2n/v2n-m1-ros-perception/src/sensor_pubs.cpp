// Copyright 2026 ALP Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// SensorPublishers implementation: IMU + GNSS + battery readings
// fed onto the ROS 2 graph through standard sensor_msgs types.

#include "sensor_pubs.hpp"

#include "alp/e1m_pinout.h"

namespace alp {

using namespace std::chrono_literals;

SensorPublishers::SensorPublishers(rclcpp::Node &parent) : parent_(parent) {
    // Shared I²C bus -- LSM6DSO + INA236 sit on it.
    i2c_ = alp_i2c_open((const alp_i2c_config_t *)&(const alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    if (i2c_ != nullptr) {
        lsm6dso_init(&imu_,  i2c_, LSM6DSO_I2C_ADDR_LOW);
        ina236_init(&batt_, i2c_, INA236_I2C_ADDR_DEFAULT, /*shunt_ohms=*/0.01f);
    }

    // GNSS UART (factory-default 9600/8N1).
    gps_uart_ = alp_uart_open((const alp_uart_config_t *)&(const alp_uart_config_t){
        .port_id   = E1M_UART0,
        .baud_rate = 9600,
    });
    if (gps_uart_ != nullptr) {
        ublox_neo_m9n_init(&gps_, gps_uart_);
    }

    // Publishers + timers.  /alp/* prefix lets customer launch files
    // remap easily.
    imu_pub_   = parent_.create_publisher<sensor_msgs::msg::Imu>("/alp/imu", 50);
    gnss_pub_  = parent_.create_publisher<sensor_msgs::msg::NavSatFix>("/alp/gnss", 5);
    batt_pub_  = parent_.create_publisher<sensor_msgs::msg::BatteryState>("/alp/battery", 5);

    imu_timer_   = parent_.create_wall_timer(20ms,   [this] { tick_imu(); });        // 50 Hz
    telem_timer_ = parent_.create_wall_timer(1000ms, [this] { tick_slow_telem(); }); // 1 Hz
}

SensorPublishers::~SensorPublishers() {
    if (i2c_)      alp_i2c_close(i2c_);
    if (gps_uart_) alp_uart_close(gps_uart_);
}

void SensorPublishers::tick_imu() {
    lsm6dso_accel_t a = {};
    lsm6dso_gyro_t  g = {};
    if (lsm6dso_read_accel(&imu_, &a) != ALP_OK) return;
    if (lsm6dso_read_gyro(&imu_,  &g) != ALP_OK) return;

    sensor_msgs::msg::Imu msg;
    msg.header.stamp = parent_.now();
    msg.header.frame_id = "imu_link";

    // Linear accel in m/s².  LSM6DSO returns milli-g; convert with
    // 9.80665 m/s² per g.
    msg.linear_acceleration.x = a.x_mg / 1000.0 * 9.80665;
    msg.linear_acceleration.y = a.y_mg / 1000.0 * 9.80665;
    msg.linear_acceleration.z = a.z_mg / 1000.0 * 9.80665;

    // Angular velocity in rad/s.  Gyro returns milli-dps.
    msg.angular_velocity.x = g.x_mdps / 1000.0 * (3.14159265 / 180.0);
    msg.angular_velocity.y = g.y_mdps / 1000.0 * (3.14159265 / 180.0);
    msg.angular_velocity.z = g.z_mdps / 1000.0 * (3.14159265 / 180.0);

    // Orientation is unknown without AHRS; ROS convention: covariance
    // matrix's [0] = -1 signals "no orientation available".
    msg.orientation_covariance[0] = -1.0;

    imu_pub_->publish(msg);
}

void SensorPublishers::tick_slow_telem() {
    // Battery snapshot.
    int32_t mv = 0, ma = 0;
    if (ina236_read_voltage_mv(&batt_, &mv) == ALP_OK &&
        ina236_read_current_ma(&batt_, &ma) == ALP_OK) {
        sensor_msgs::msg::BatteryState msg;
        msg.header.stamp = parent_.now();
        msg.voltage = mv / 1000.f;
        msg.current = ma / 1000.f;
        msg.present = true;
        msg.power_supply_status =
            sensor_msgs::msg::BatteryState::POWER_SUPPLY_STATUS_DISCHARGING;
        batt_pub_->publish(msg);
    }

    // GNSS -- read one NMEA line; real parse arrives in v0.6 (libnmea
    // or u-blox UBX-PARSER lands as a Tier 2 contribution).
    if (gps_uart_ != nullptr) {
        uint8_t line[128];
        size_t  n = 0;
        if (ublox_neo_m9n_read_nmea_line(&gps_, line, sizeof(line), &n,
                                         /*timeout_ms=*/100) == ALP_OK) {
            // Emit a NavSatFix with zeros until the parser lands;
            // the message timestamp at least keeps downstream nodes
            // updated about node liveness.
            sensor_msgs::msg::NavSatFix msg;
            msg.header.stamp = parent_.now();
            msg.header.frame_id = "gps_link";
            msg.status.status   = sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX;
            gnss_pub_->publish(msg);
        }
    }
}

}  // namespace alp
