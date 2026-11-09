/* SPDX-License-Identifier: Apache-2.0 */

/* The loopback peer the tests talk to: a listening socket on 127.0.0.1. */

#ifndef CLOUD_TRANSPORT_TEST_PEER_H
#define CLOUD_TRANSPORT_TEST_PEER_H

#include <stdint.h>

#define PEER_HOST "127.0.0.1"

/* Start listening on @p port.  Returns the listening socket. */
int peer_listen(uint16_t port);

/* Accept the pending connection on @p listen_fd.  Returns the peer socket. */
int peer_accept(int listen_fd);

#endif /* CLOUD_TRANSPORT_TEST_PEER_H */
