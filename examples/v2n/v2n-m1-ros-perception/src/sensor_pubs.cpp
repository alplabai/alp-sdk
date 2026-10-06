// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// SensorPublishers implementation: IMU + 3V3-rail monitor fed onto the
// ROS 2 graph through standard sensor_msgs types.
//
// Everything here is a part that is actually fitted to the E1M-X EVK
// (metadata/boards/e1m-x-evk.yaml): the ICM-42670 IMU (U12) and the
// INA236 on the +3V3 rail (U21), both on the sensor I2C bus.  The EVK
// has NO LSM6DSO and NO GNSS receiver, so this example publishes
// neither; add your own on a carrier that has them.

#include "sensor_pubs.hpp"

#include "alp/boards/alp_e1m_x_evk.h" // XEVK_* bus, addresses, shunt values

namespace alp
{

using namespace std::chrono_literals;

SensorPublishers::SensorPublishers(rclcpp::Node &parent) : parent_(parent)
{
	// Sensor I2C bus (Linux i2c-0): ICM-42670, BMI323, BMP581, the
	// INA236 monitors.  Use the board macro, not a raw E1M-X bus id.
	const alp_i2c_config_t i2c_cfg = {
		.bus_id     = XEVK_I2C_BUS_SENSORS,
		.bitrate_hz = 400000,
	};
	i2c_ = alp_i2c_open(&i2c_cfg);
	if (i2c_ != nullptr) {
		// U12 ICM-42670, canonical primary IMU (0x69); the alternate
		// IMU, BMI323, is at 0x68.
		if (icm42670_init(&imu_, i2c_, XEVK_I2C_ADDR_ICM42670) == ALP_OK) {
			// 100 Hz, +-2 g / +-250 dps -- icm42670_init() only binds the
			// bus; the caller selects ODR + full-scale.  These are the
			// full-scales tick_imu()'s raw-count conversion assumes.
			icm42670_set_accel(&imu_, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_2G);
			icm42670_set_gyro(&imu_, ICM42670_ODR_100_HZ, ICM42670_GYRO_FS_250_DPS);
		}
		// U21 INA236A on the +3V3 rail (0x40).  Its 20 mOhm shunt and
		// 4 A limit come from the board metadata -- calibrating with the
		// wrong shunt scales every current reading by the ratio.
		ina236_init(&rail_,
		            i2c_,
		            XEVK_I2C_ADDR_INA236_3V3,
		            XEVK_INA236_SHUNT_3V3_OHMS,
		            XEVK_INA236_MAX_3V3_A,
		            INA236_ADCRANGE_81MV);
	}

	// Publishers + timers.  /alp/* prefix lets customer launch files
	// remap easily.  This is a board supply rail, not a battery, hence
	// /alp/rail_3v3 rather than a battery topic.
	imu_pub_  = parent_.create_publisher<sensor_msgs::msg::Imu>("/alp/imu", 50);
	rail_pub_ = parent_.create_publisher<sensor_msgs::msg::BatteryState>("/alp/rail_3v3", 5);

	imu_timer_   = parent_.create_wall_timer(20ms, [this] { tick_imu(); });          // 50 Hz
	telem_timer_ = parent_.create_wall_timer(1000ms, [this] { tick_slow_telem(); }); // 1 Hz
}

SensorPublishers::~SensorPublishers()
{
	if (i2c_) alp_i2c_close(i2c_);
}

void SensorPublishers::tick_imu()
{
	icm42670_axes_t a = {};
	icm42670_axes_t g = {};
	if (icm42670_read_accel(&imu_, &a) != ALP_OK) return;
	if (icm42670_read_gyro(&imu_, &g) != ALP_OK) return;

	sensor_msgs::msg::Imu msg;
	msg.header.stamp    = parent_.now();
	msg.header.frame_id = "imu_link";

	// icm42670_read_* return raw int16 counts; scale by the configured
	// full-scale sensitivities (ICM-42670-P datasheet):
	//   +-2 g     accel -> 16384 LSB/g   (0.061 mg/LSB)
	//   +-250 dps gyro  -> 131   LSB/dps (7.63 mdps/LSB)
	constexpr double kAccelMgPerLsb  = 1000.0 / 16384.0;
	constexpr double kGyroMdpsPerLsb = 1000.0 / 131.0;
	constexpr double kGravity        = 9.80665; // m/s^2 per g
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

void SensorPublishers::tick_slow_telem()
{
	// +3V3 rail snapshot (INA236 U21), published as BatteryState only
	// because sensor_msgs has no plain "supply rail" message.
	//
	// Board note: on the E1M-X EVK V2 the U21 bus-voltage register reads
	// ~0 V (VBUS-sense wiring under investigation); the shunt/current
	// path is fine.  Trust `current` today; `voltage` only on a rev
	// where the sense wiring is fixed.
	int32_t mv = 0, ua = 0;
	if (ina236_read_bus_mv(&rail_, &mv) == ALP_OK &&
	    ina236_read_current_ua(&rail_, &ua) == ALP_OK) {
		sensor_msgs::msg::BatteryState msg;
		msg.header.stamp        = parent_.now();
		msg.header.frame_id     = "rail_3v3";
		msg.voltage             = mv / 1000.f; // mV -> V
		msg.current             = ua / 1.0e6f; // uA -> A
		msg.present             = true;
		msg.power_supply_status = sensor_msgs::msg::BatteryState::POWER_SUPPLY_STATUS_UNKNOWN;
		rail_pub_->publish(msg);
	}
}

} // namespace alp
