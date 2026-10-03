// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// som_temperature_node -- the smallest useful Alp SDK + ROS 2 node.
// ================================================================
//
// What it shows: a ROS 2 (Humble, Linux/Yocto, Cortex-A) node that gets its
// data from the PORTABLE Alp SDK surface only -- one <alp/temperature.h>
// call -- and republishes it as a standard sensor_msgs/Temperature.  No chip
// driver, no vendor header, no SoM-specific #ifdef:
//
//        <alp/temperature.h>               ROS 2 graph
//   alp_temperature_read_milli_c() --> /alp/som_temperature
//      (SoM-resident sensor)            sensor_msgs/Temperature (degC)
//
// Why that matters (the ADR 0017 point): the node does not know or care which
// SoM it runs on.  Whether the number comes from an on-module TMP112, a
// different part, or does not exist on that SoM at all is the SDK's
// business -- the capability is a SoM fact, not something this node probes.
//
// ── Unified, graceful degradation ──────────────────────────────
//
// alp_temperature_read_milli_c() returns a status, never a fake number:
//
//   ALP_OK             -> publish the reading
//   ALP_ERR_NOSUPPORT  -> this build/SoM has no backend for it: the node
//                         stays up, publishes nothing, and says so ONCE
//   ALP_ERR_NOT_READY  -> sensor declared but not up yet: retry next tick
//   anything else      -> transient fault: throttled warning, retry
//
// Today the Yocto (Linux) build of the SDK implements this call as a
// NOSUPPORT stub (include/alp/temperature.h "Today's coverage"), so on
// V2N/V2M you will see the one-time NOSUPPORT notice and no topic data
// until a Linux backend lands.  The node source does not change when it does.
//
// ── Parameters ─────────────────────────────────────────────────
//
//   period_ms  (int,    default 1000)       publish period
//   frame_id   (string, default "som")      header.frame_id
//
// Build: see this example's README (colcon inside the Yocto SDK, or the
// alp-ros2-temperature recipe in meta-alp-sdk).

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/temperature.hpp>

// The ONLY Alp SDK include: the portable API.  (alp_status_t comes with it.)
#include "alp/temperature.h"

using namespace std::chrono_literals;

class SomTemperatureNode : public rclcpp::Node
{
  public:
	SomTemperatureNode() : Node("alp_som_temperature")
	{
		const auto period_ms = declare_parameter<int>("period_ms", 1000);
		frame_id_            = declare_parameter<std::string>("frame_id", "som");

		// Sensor-data QoS: best-effort, shallow queue -- a late temperature
		// sample is worthless, so never block on a slow subscriber.
		pub_   = create_publisher<sensor_msgs::msg::Temperature>("/alp/som_temperature",
		                                                         rclcpp::SensorDataQoS());
		timer_ = create_wall_timer(std::chrono::milliseconds(period_ms), [this] { tick(); });
	}

  private:
	void tick()
	{
		int32_t      milli_c = 0;
		alp_status_t s       = alp_temperature_read_milli_c(&milli_c);

		if (s == ALP_OK) {
			sensor_msgs::msg::Temperature msg;
			msg.header.stamp    = now();
			msg.header.frame_id = frame_id_;
			// The SDK promises signed integer milli-degC and nothing about
			// resolution, so convert exactly and claim no variance (0 = unknown
			// per sensor_msgs/Temperature).
			msg.temperature = static_cast<double>(milli_c) / 1000.0;
			msg.variance    = 0.0;
			pub_->publish(msg);
			return;
		}

		if (s == ALP_ERR_NOSUPPORT) {
			// Permanent for this build: say it once, keep the node alive so a
			// launch file that also starts other nodes is not torn down.
			if (!warned_nosupport_) {
				RCLCPP_WARN(get_logger(),
				            "no on-module temperature backend on this build "
				            "(ALP_ERR_NOSUPPORT); /alp/som_temperature stays silent");
				warned_nosupport_ = true;
			}
			return;
		}

		// NOT_READY / IO: transient -- log at most every 10 s.
		RCLCPP_WARN_THROTTLE(get_logger(),
		                     *get_clock(),
		                     10000,
		                     "alp_temperature_read_milli_c failed: status=%d",
		                     static_cast<int>(s));
	}

	std::string                                                 frame_id_;
	rclcpp::Publisher<sensor_msgs::msg::Temperature>::SharedPtr pub_;
	rclcpp::TimerBase::SharedPtr                                timer_;
	bool                                                        warned_nosupport_ = false;
};

int main(int argc, char **argv)
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<SomTemperatureNode>());
	rclcpp::shutdown();
	return 0;
}
