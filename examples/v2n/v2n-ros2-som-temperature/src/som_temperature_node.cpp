// Copyright 2026 Alp Lab AB
// SPDX-License-Identifier: Apache-2.0
//
// som_temperature_node -- the smallest useful Alp SDK + ROS 2 node.
// ================================================================
//
// What it shows: a ROS 2 (Humble, Linux/Yocto, Cortex-A) node that gets its
// data from the PORTABLE Alp SDK surface only -- <alp/temperature.h> -- and
// republishes it as standard sensor_msgs/Temperature.  No chip driver, no
// vendor header, no SoM-specific #ifdef:
//
//        <alp/temperature.h>                  ROS 2 graph
//   alp_temperature_read_milli_c()     --> /alp/som_temperature
//      (on-module sensor, ambient)         sensor_msgs/Temperature (degC)
//   alp_temperature_read_die_milli_c() --> /alp/soc_temperature
//      (SoC die / junction)                sensor_msgs/Temperature (degC)
//
// The two are different physical quantities and are never folded together
// (issue #2066).  On V2N/V2M (Linux) the SoC die reading is the one with a
// backend today: it comes from the Linux thermal zones the on-die TSU feeds.
//
// Why that matters (the ADR 0017 point): the node does not know or care which
// SoM it runs on.  Whether a number comes from an on-module sensor, the SoC
// die, or does not exist on that SoM at all is the SDK's business -- the
// capability is a SoM fact, not something this node probes.
//
// ── Unified, graceful degradation ──────────────────────────────
//
// Each read returns a status, never a fake number:
//
//   ALP_OK             -> publish the reading
//   ALP_ERR_NOSUPPORT  -> this build/SoM has no backend for it: the node
//                         stays up, publishes nothing on that topic, and
//                         says so ONCE
//   ALP_ERR_NOT_READY  -> sensor declared but not up yet: retry next tick
//   anything else      -> transient fault: throttled warning, retry
//
// Today the Yocto (Linux) build implements alp_temperature_read_milli_c() as
// a NOSUPPORT stub (include/alp/temperature.h "Today's coverage"), so on
// V2N/V2M /alp/som_temperature stays silent after one notice while
// /alp/soc_temperature carries data.  The node source does not change when
// an on-module Linux backend lands.
//
// ── Parameters ─────────────────────────────────────────────────
//
//   period_ms  (int,    default 1000)       publish period
//   frame_id   (string, default "som")      header.frame_id (both topics)
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
		// A period below 1 ms would busy-spin a core (0) or make rclcpp throw
		// out of this constructor (negative), so clamp it and say so.
		auto period_ms = declare_parameter<int>("period_ms", 1000);
		if (period_ms < 1) {
			RCLCPP_WARN(get_logger(), "period_ms=%d is below 1; using 1 ms", period_ms);
			period_ms = 1;
		}
		RCLCPP_INFO(get_logger(), "publishing every %d ms", period_ms);
		frame_id_ = declare_parameter<std::string>("frame_id", "som");

		// Sensor-data QoS: best-effort, shallow queue -- a late temperature
		// sample is worthless, so never block on a slow subscriber.
		som_.pub = create_publisher<sensor_msgs::msg::Temperature>("/alp/som_temperature",
		                                                           rclcpp::SensorDataQoS());
		soc_.pub = create_publisher<sensor_msgs::msg::Temperature>("/alp/soc_temperature",
		                                                           rclcpp::SensorDataQoS());
		timer_   = create_wall_timer(std::chrono::milliseconds(period_ms), [this] { tick(); });
	}

  private:
	// One published reading: its topic handle plus the one-shot NOSUPPORT flag.
	struct Channel {
		rclcpp::Publisher<sensor_msgs::msg::Temperature>::SharedPtr pub;
		bool                                                        warned_nosupport = false;
	};

	void tick()
	{
		int32_t milli_c = 0;
		publish(
		    som_, "alp_temperature_read_milli_c", alp_temperature_read_milli_c(&milli_c), milli_c);
		publish(soc_,
		        "alp_temperature_read_die_milli_c",
		        alp_temperature_read_die_milli_c(&milli_c),
		        milli_c);
	}

	void publish(Channel &ch, const char *api, alp_status_t s, int32_t milli_c)
	{
		if (s == ALP_OK) {
			sensor_msgs::msg::Temperature msg;
			msg.header.stamp    = now();
			msg.header.frame_id = frame_id_;
			// The SDK promises signed integer milli-degC and nothing about
			// resolution, so convert exactly and claim no variance (0 = unknown
			// per sensor_msgs/Temperature).
			msg.temperature = static_cast<double>(milli_c) / 1000.0;
			msg.variance    = 0.0;
			ch.pub->publish(msg);
			return;
		}

		if (s == ALP_ERR_NOSUPPORT) {
			// Permanent for this build: say it once, keep the node alive so a
			// launch file that also starts other nodes is not torn down.
			if (!ch.warned_nosupport) {
				RCLCPP_WARN(get_logger(),
				            "%s: ALP_ERR_NOSUPPORT on this build; %s stays silent",
				            api,
				            ch.pub->get_topic_name());
				ch.warned_nosupport = true;
			}
			return;
		}

		// NOT_READY / IO: transient -- log at most every 10 s.
		RCLCPP_WARN_THROTTLE(
		    get_logger(), *get_clock(), 10000, "%s failed: status=%d", api, static_cast<int>(s));
	}

	std::string                  frame_id_;
	Channel                      som_;
	Channel                      soc_;
	rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char **argv)
{
	rclcpp::init(argc, argv);
	rclcpp::spin(std::make_shared<SomTemperatureNode>());
	rclcpp::shutdown();
	return 0;
}
