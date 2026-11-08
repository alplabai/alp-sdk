/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rpmsg_link -- a datagram pipe from the Cortex-M33 to the A55 over the
 * ADR 0016 RPMsg channel.  main.c's micro-ROS transport callbacks sit on
 * top of these four calls and know nothing about OpenAMP.
 */

#ifndef RPMSG_LINK_H
#define RPMSG_LINK_H

#include <stddef.h>
#include <stdint.h>

/** Largest datagram rpmsg_link_send() accepts / rpmsg_link_recv() can return.
 *  RPMsg buffers are 512 B with a 16 B header = 496 B on the wire, minus the
 *  5-byte "xrce\0" method header of the <alp/rpc.h> framing. */
#define RPMSG_LINK_MTU 491

/** Bring up libmetal + the rpmsg vdev on a manager thread.  0 on success. */
int rpmsg_link_start(void);

/** Block until the A55 bridge has sent its first frame (the M33 is the rpmsg
 *  slave: it cannot address the A55 until it has heard from it).
 *  @return 0 when the peer is known, -1 on timeout (@p timeout_ms < 0 = forever). */
int rpmsg_link_wait_peer(int32_t timeout_ms);

/** Send one datagram (<= RPMSG_LINK_MTU).  @return bytes sent, 0 on error. */
size_t rpmsg_link_send(const uint8_t *data, size_t len);

/** Receive one datagram, waiting up to @p timeout_ms (< 0 = forever).
 *  @return bytes received, 0 on timeout (or a datagram larger than @p cap,
 *  which is dropped). */
size_t rpmsg_link_recv(uint8_t *buf, size_t cap, int timeout_ms);

#endif /* RPMSG_LINK_H */
