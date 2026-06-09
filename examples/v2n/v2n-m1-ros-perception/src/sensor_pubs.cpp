// Copyright 2026 Alp Lab AB
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
    const alp_i2c_config_t i2c_cfg = {
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    };
    i2c_ = alp_i2c_open(&i2c_cfg);
    if (i2c_ != nullptr) {
        if (lsm6dso_init(&imu_, i2c_, LSM6DSO_I2C_ADDR_LOW) == ALP_OK) {
            // 104 Hz, ±2 g / ±250 dps -- lsm6dso_init() only binds the
            // bus; the caller selects ODR + full-scale.  These are the
            // full-scales tick_imu()'s raw-count conversion assumes.
            lsm6dso_set_accel(&imu_, LSM6DSO_ODR_104_HZ, LSM6DSO_ACCEL_FS_2G);
            lsm6dso_set_gyro(&imu_,  LSM6DSO_ODR_104_HZ, LSM6DSO_GYRO_FS_250_DPS);
        }
        // addr 0 -> INA236A default (0x40).  Example calibration: 10 mΩ
        // shunt, 4 A full-scale -- customers set their rail's values.
        ina236_init(&batt_, i2c_, 0, /*shunt_ohms=*/0.01f,
                    /*max_current_a=*/4.0f, INA236_ADCRANGE_81MV);
    }

    // GNSS UART (factory-default 9600 8N1).
    const alp_uart_config_t gps_cfg = {
        .port_id   = E1M_UART0,
        .baudrate  = 9600,
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    };
    gps_uart_ = alp_uart_open(&gps_cfg);
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
    lsm6dso_axes_t a = {};
    lsm6dso_axes_t g = {};
    if (lsm6dso_read_accel(&imu_, &a) != ALP_OK) return;
    if (lsm6dso_read_gyro(&imu_,  &g) != ALP_OK) return;

    sensor_msgs::msg::Imu msg;
    msg.header.stamp = parent_.now();
    msg.header.frame_id = "imu_link";

    // lsm6dso_read_* return raw int16 counts; scale by the configured
    // full-scale sensitivities (LSM6DSO datasheet):
    //   ±2 g     accel -> 0.061 mg/LSB
    //   ±250 dps gyro  -> 8.75  mdps/LSB
    constexpr double kAccelMgPerLsb  = 0.061;
    constexpr double kGyroMdpsPerLsb = 8.75;
    constexpr double kGravity        = 9.80665;                  // m/s² per g
    constexpr double kDegToRad       = 3.14159265358979 / 180.0;

    // Linear accel in m/s².
    msg.linear_acceleration.x = a.x * kAccelMgPerLsb / 1000.0 * kGravity;
    msg.linear_acceleration.y = a.y * kAccelMgPerLsb / 1000.0 * kGravity;
    msg.linear_acceleration.z = a.z * kAccelMgPerLsb / 1000.0 * kGravity;

    // Angular velocity in rad/s.
    msg.angular_velocity.x = g.x * kGyroMdpsPerLsb / 1000.0 * kDegToRad;
    msg.angular_velocity.y = g.y * kGyroMdpsPerLsb / 1000.0 * kDegToRad;
    msg.angular_velocity.z = g.z * kGyroMdpsPerLsb / 1000.0 * kDegToRad;

    // Orientation is unknown without AHRS; ROS convention: covariance
    // matrix's [0] = -1 signals "no orientation available".
    msg.orientation_covariance[0] = -1.0;

    imu_pub_->publish(msg);
}

void SensorPublishers::tick_slow_telem() {
    // Battery snapshot.
    int32_t mv = 0, ua = 0;
    if (ina236_read_bus_mv(&batt_, &mv) == ALP_OK &&
        ina236_read_current_ua(&batt_, &ua) == ALP_OK) {
        sensor_msgs::msg::BatteryState msg;
        msg.header.stamp = parent_.now();
        msg.voltage = mv / 1000.f;    // mV -> V
        msg.current = ua / 1.0e6f;    // µA -> A
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
