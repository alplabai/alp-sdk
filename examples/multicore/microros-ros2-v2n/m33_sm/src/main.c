/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * microros-ros2-v2n / m33_sm -- a micro-ROS node on the RZ/V2N Cortex-M33.
 *
 * WHAT THIS DEMONSTRATES
 *   A ROS 2 node running on a microcontroller: it publishes a
 *   std_msgs/Int32 counter on the topic `alp_counter` once per second, and
 *   an ordinary `ros2 topic echo` on the Cortex-A55 (Yocto side) prints it.
 *   No ROS 2 is installed on the M33; micro-ROS is a thin client that speaks
 *   the Micro XRCE-DDS protocol to a "micro-ROS agent", and the agent turns
 *   that into real DDS traffic for the rest of the ROS 2 graph.
 *
 * WHERE THE BYTES GO  (docs/adr/0035-micro-ros-cross-core-transport.md, Option B)
 *
 *   this file            rpmsg_link.c        A55: linux/src/bridge.c     A55: agent
 *   rclc publisher --> custom transport --> RPMsg (ADR 0016) --> UDP 127.0.0.1:8888 --> micro-ROS
 *   (rcl_publish)      (4 callbacks below)  <alp/rpc.h> "xrce"    unmodified udp4      agent
 *
 *   The XRCE client normally talks over serial or UDP.  Both need a device
 *   the M33 does not have towards the A55 (the V2N Yocto BSP has no kernel
 *   rpmsg tty/netdev), so we use micro-ROS's *custom transport* hook instead:
 *   four callbacks -- open / close / write / read -- that move opaque
 *   datagrams.  Our callbacks are thin wrappers over rpmsg_link.c.
 *
 * WHY framing = false
 *   Serial transports need HDLC framing + CRC to find packet boundaries in a
 *   byte stream.  RPMsg is already message-oriented (one send = one receive),
 *   so we pass framing=false and each callback moves exactly one XRCE
 *   datagram.
 *
 * STATUS: build-ready, NOT built or bench-run yet.  See README.md
 * ("What is untested").
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rcl/error_handling.h>
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rmw_microros/rmw_microros.h>
#include <std_msgs/msg/int32.h>

#include "rpmsg_link.h"

LOG_MODULE_REGISTER(microros_v2n, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Custom XRCE transport: open / close / write / read over RPMsg       */
/* ------------------------------------------------------------------ */

/* Called by the XRCE client when it opens a session.  Returns true when the
 * link is usable.  The M33 is the RPMsg *slave* and cannot address the A55
 * until the bridge has spoken first, so "open" means "wait for the bridge's
 * first frame".  The 1 s timeout lets the caller's ping loop keep retrying
 * (and keeps the log alive) instead of blocking forever. */
static bool transport_open(struct uxrCustomTransport *t)
{
	ARG_UNUSED(t);
	return rpmsg_link_wait_peer(1000) == 0;
}

/* Nothing to tear down: the RPMsg endpoint lives for the life of the app so a
 * new XRCE session (after an agent restart) can reuse it. */
static bool transport_close(struct uxrCustomTransport *t)
{
	ARG_UNUSED(t);
	return true;
}

/* One call = one datagram to the agent.  Returns the number of bytes written;
 * 0 plus *err != 0 tells the client the write failed.  Datagrams larger than
 * the RPMsg payload (RPMSG_LINK_MTU) are refused -- the XRCE MTU must be
 * configured at or below it (README.md, "What is untested"). */
static size_t
transport_write(struct uxrCustomTransport *t, const uint8_t *buf, size_t len, uint8_t *err)
{
	ARG_UNUSED(t);
	size_t n = rpmsg_link_send(buf, len);

	*err = (n == len) ? 0 : 1;
	return n;
}

/* Wait up to timeout_ms (the client passes its own poll interval) for one
 * datagram from the agent.  Returns 0 on timeout -- that is the normal idle
 * case, not an error, so *err stays 0. */
static size_t
transport_read(struct uxrCustomTransport *t, uint8_t *buf, size_t len, int timeout_ms, uint8_t *err)
{
	ARG_UNUSED(t);
	*err = 0;
	return rpmsg_link_recv(buf, len, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* The micro-ROS node                                                  */
/* ------------------------------------------------------------------ */

#define PUBLISH_PERIOD_MS 1000

/* rclc calls return rcl_ret_t; anything but RCL_RET_OK aborts this session. */
#define RC_OK(call) \
	({ \
		rcl_ret_t rc_ = (call); \
		if (rc_ != RCL_RET_OK) { \
			LOG_ERR("%s failed: %d (line %d)", #call, (int)rc_, __LINE__); \
		} \
		rc_ == RCL_RET_OK; \
	})

/* One XRCE session: create the node + publisher, publish until a publish
 * fails (agent gone / bridge restarted), then tear everything down so main()
 * can start a fresh session.  Returns when the session is over. */
static void run_session(void)
{
	rcl_allocator_t      allocator = rcl_get_default_allocator();
	rclc_support_t       support;
	rcl_node_t           node;
	rcl_publisher_t      pub;
	std_msgs__msg__Int32 msg = { .data = 0 };

	if (!RC_OK(rclc_support_init(&support, 0, NULL, &allocator))) {
		return;
	}
	if (!RC_OK(rclc_node_init_default(&node, "alp_v2n_m33", "", &support))) {
		goto fini_support;
	}
	if (!RC_OK(rclc_publisher_init_default(
	        &pub, &node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32), "alp_counter"))) {
		goto fini_node;
	}

	LOG_INF("publishing std_msgs/Int32 on /alp_counter every %d ms", PUBLISH_PERIOD_MS);
	while (RC_OK(rcl_publish(&pub, &msg, NULL))) {
		msg.data++;
		k_msleep(PUBLISH_PERIOD_MS);
	}

	(void)rcl_publisher_fini(&pub, &node);
fini_node:
	(void)rcl_node_fini(&node);
fini_support:
	(void)rclc_support_fini(&support);
}

int main(void)
{
	if (rpmsg_link_start() != 0) {
		LOG_ERR("rpmsg link bring-up failed");
		return -1;
	}

	/* framing=false: RPMsg already delimits messages (see file header).
	 * args=NULL: our callbacks keep their state in rpmsg_link.c. */
	rmw_uros_set_custom_transport(
	    false, NULL, transport_open, transport_close, transport_write, transport_read);

	for (;;) {
		/* Ping the agent (through the transport above) until it answers:
		 * 1 s timeout, 5 attempts per round.  This is what makes start
		 * order irrelevant -- the M33 boots first, the agent may come
		 * up minutes later. */
		if (rmw_uros_ping_agent(1000, 5) != RMW_RET_OK) {
			LOG_INF("waiting for the micro-ROS agent (is the A55 bridge running?)");
			continue;
		}
		LOG_INF("agent reachable, starting session");
		run_session();
		LOG_WRN("session ended, retrying");
		k_msleep(1000);
	}
	return 0;
}
