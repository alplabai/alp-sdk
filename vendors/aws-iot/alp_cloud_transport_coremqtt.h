/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file alp_cloud_transport_coremqtt.h
 * @brief coreMQTT `TransportInterface_t` over <alp/cloud_transport.h>.
 *
 * Built when both CONFIG_ALP_AWS_IOT and CONFIG_ALP_CLOUD_TRANSPORT are set.
 * Kept apart from <alp/cloud_transport.h> because coreMQTT leaves
 * `struct NetworkContext` for the transport to define, and this header does:
 * an application that brings its own coreMQTT transport must not include it.
 */

#ifndef ALP_CLOUD_TRANSPORT_COREMQTT_H
#define ALP_CLOUD_TRANSPORT_COREMQTT_H

#include <alp/cloud_transport.h>
#include <transport_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/** coreMQTT's network context for this transport. */
struct NetworkContext {
	/** The connection coreMQTT sends and receives on. */
	alp_cloud_transport_t *transport;
};

/**
 * @brief Point a coreMQTT `TransportInterface_t` at a transport.
 *
 * @p ctx and @p t must outlive the `MQTTContext_t` that uses @p iface.
 * `writev` is left NULL, so coreMQTT falls back to `send`.  Does nothing when
 * @p iface or @p ctx is NULL.
 */
void alp_cloud_transport_coremqtt(TransportInterface_t  *iface,
                                  NetworkContext_t      *ctx,
                                  alp_cloud_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CLOUD_TRANSPORT_COREMQTT_H */
