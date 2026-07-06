/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto mqtt driver-class backend.  Lifts the body of
 * src/yocto/iot_yocto.c (the legacy v0.4 direct-impl libmosquitto
 * wrapper) into a registry-shaped backend behind the alp_mqtt
 * dispatcher's ops vtable (#33).  Registered at priority 100 with
 * vendor "linux"; the sw_fallback backend (priority 0) still wins on
 * builds where libmosquitto is absent (this TU is only compiled when
 * CMake's pkg_check_modules finds `libmosquitto`).
 *
 * Selected on any silicon (silicon_ref "*") because libmosquitto
 * rides plain sockets -- the kernel network stack is SoC-agnostic.
 *
 * The Wi-Fi half of <alp/iot.h> stays sw_fallback on Linux: Wi-Fi
 * bring-up on real Yocto images is a system-config concern
 * (wpa_supplicant / NetworkManager) rather than an SDK-side one --
 * customers configure the radio out of band and the SDK consumes
 * the resulting network through plain sockets.
 *
 * Connection model
 * ----------------
 * Caller-driven loop.  alp_mqtt_loop(handle, timeout_ms) pumps the
 * underlying mosquitto event machine for at most `timeout_ms`
 * before returning.  No background thread; apps that want async
 * delivery wrap alp_mqtt_loop in their own pthread.
 *
 * Subscription dispatch
 * ---------------------
 * libmosquitto's on_message callback fires once per inbound message,
 * regardless of filter.  We keep a small per-handle table of
 * `(filter, cb, user)` tuples; on_message walks the table and
 * dispatches to every callback whose filter matches the message's
 * topic via `mosquitto_topic_matches_sub`.  No external locking is
 * needed -- libmosquitto serialises callbacks for a single client
 * instance and the only thread touching the handle is the one
 * calling alp_mqtt_loop.
 *
 * TLS
 * ---
 * `mqtts://` URIs route through libmosquitto's `mosquitto_tls_set`
 * (OpenSSL underneath on a stock Yocto image).  CA / client cert /
 * client key paths come from `alp_mqtt_config_t.tls`; a NULL tls
 * pointer falls back to the host OS's default CA path
 * (`/etc/ssl/certs` on Debian/Ubuntu/Yocto) and no client cert.
 * Production deployments pin `tls->ca_file` to a known-good CA
 * bundle so the broker's identity isn't trustable to any root the
 * host happens to know about.  The `tls->insecure` flag skips peer
 * verification entirely -- development-only; production builds
 * should refuse to ship with it on.
 */

#if defined(__linux__)

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "mqtt_ops.h"

#ifndef ALP_SDK_YOCTO_MAX_MQTT_HANDLES
#define ALP_SDK_YOCTO_MAX_MQTT_HANDLES 2
#endif

#ifndef ALP_SDK_YOCTO_MAX_MQTT_SUBS
#define ALP_SDK_YOCTO_MAX_MQTT_SUBS 8
#endif

#ifndef ALP_SDK_YOCTO_MQTT_HOST_MAX
#define ALP_SDK_YOCTO_MQTT_HOST_MAX 128
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Default system CA path on Yocto / Debian / Ubuntu.  Overridden by
 * tls->ca_file when the caller supplies it.  Picked at compile time
 * rather than runtime-discovered: every E1M Yocto image uses
 * openssl-misc's default layout. */
#ifndef ALP_SDK_YOCTO_DEFAULT_CA_PATH
#define ALP_SDK_YOCTO_DEFAULT_CA_PATH "/etc/ssl/certs"
#endif

struct mqtt_sub {
	char             *filter;
	alp_mqtt_msg_cb_t cb;
	void             *user;
};

/* Per-handle backend data.  The dispatcher owns the public
 * struct alp_mqtt pool; this backend carries only the
 * mosquitto-specific per-handle blob behind state->be_data. */
struct mqtt_be {
	bool              in_use;
	struct mosquitto *mosq;
	char              host[ALP_SDK_YOCTO_MQTT_HOST_MAX];
	int               port;
	uint16_t          keepalive_s;
	bool              use_tls;
	bool              connected;
	struct mqtt_sub   subs[ALP_SDK_YOCTO_MAX_MQTT_SUBS];
	size_t            nsubs;
};

static struct mqtt_be g_mqtt_be_pool[ALP_SDK_YOCTO_MAX_MQTT_HANDLES];

/* libmosquitto requires a one-time global init.  Initialise lazily
 * on first open() so apps that never touch MQTT don't pay the cost;
 * cleanup is intentionally skipped (it's mostly bookkeeping NOPs
 * and process exit reaps the rest). */
static bool g_mosq_lib_init_done;

static struct mqtt_be *be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_mqtt_be_pool); ++i) {
		if (!g_mqtt_be_pool[i].in_use) {
			memset(&g_mqtt_be_pool[i], 0, sizeof(g_mqtt_be_pool[i]));
			g_mqtt_be_pool[i].in_use = true;
			return &g_mqtt_be_pool[i];
		}
	}
	return NULL;
}

static void be_release(struct mqtt_be *be)
{
	if (be == NULL) {
		return;
	}
	if (be->mosq != NULL) {
		if (be->connected) {
			(void)mosquitto_disconnect(be->mosq);
		}
		mosquitto_destroy(be->mosq);
		be->mosq = NULL;
	}
	for (size_t i = 0; i < be->nsubs; ++i) {
		free(be->subs[i].filter);
		be->subs[i].filter = NULL;
	}
	be->nsubs  = 0;
	be->in_use = false;
}

/** @brief Map a libmosquitto return code to the closest alp_status_t. */
static alp_status_t mosq_to_alp(int rc)
{
	switch (rc) {
	case MOSQ_ERR_SUCCESS:
		return ALP_OK;
	case MOSQ_ERR_INVAL:
		return ALP_ERR_INVAL;
	case MOSQ_ERR_NOMEM:
		return ALP_ERR_NOMEM;
	case MOSQ_ERR_PROTOCOL:
	case MOSQ_ERR_NOT_SUPPORTED:
		return ALP_ERR_NOSUPPORT;
	case MOSQ_ERR_NO_CONN:
	case MOSQ_ERR_CONN_LOST:
	case MOSQ_ERR_CONN_REFUSED:
		return ALP_ERR_NOT_READY;
	case MOSQ_ERR_AUTH:
		return ALP_ERR_INVAL;
	default:
		return ALP_ERR_IO;
	}
}

/* Parse the broker URI into the backend's host / port / use_tls
 * fields.  Accepts:
 *   - "mqtt://host[:port]"   (default port 1883)
 *   - "mqtts://host[:port]"  (default port 8883)
 * Returns ALP_OK on success; ALP_ERR_INVAL on malformed input. */
static alp_status_t parse_broker_uri(struct mqtt_be *be, const char *uri)
{
	if (uri == NULL) {
		return ALP_ERR_INVAL;
	}
	const char *rest = NULL;
	int         default_port;
	if (strncmp(uri, "mqtts://", 8) == 0) {
		rest         = uri + 8;
		default_port = 8883;
		be->use_tls  = true;
	} else if (strncmp(uri, "mqtt://", 7) == 0) {
		rest         = uri + 7;
		default_port = 1883;
		be->use_tls  = false;
	} else {
		return ALP_ERR_INVAL;
	}
	size_t rlen = strlen(rest);
	if (rlen == 0 || rlen >= sizeof(be->host)) {
		return ALP_ERR_INVAL;
	}
	memcpy(be->host, rest, rlen + 1);
	be->port = default_port;

	char *colon = strchr(be->host, ':');
	if (colon != NULL) {
		*colon        = '\0';
		const char *p = colon + 1;
		if (*p == '\0') {
			return ALP_ERR_INVAL;
		}
		long parsed = strtol(p, NULL, 10);
		if (parsed <= 0 || parsed > 65535) {
			return ALP_ERR_INVAL;
		}
		be->port = (int)parsed;
	}
	if (be->host[0] == '\0') {
		return ALP_ERR_INVAL;
	}
	return ALP_OK;
}

/* Apply TLS parameters to the freshly-created mosquitto handle.
 * Returns ALP_OK on success.  ALP_ERR_IO if mosquitto_tls_set fails
 * (typically because the CA file doesn't exist or isn't a valid
 * PEM).  Skipped when the URI scheme is "mqtt://". */
static alp_status_t apply_tls(struct mqtt_be *be, const alp_mqtt_tls_config_t *tls)
{
	if (!be->use_tls) {
		return ALP_OK;
	}
	const char *cafile   = (tls != NULL) ? tls->ca_file : NULL;
	const char *certfile = (tls != NULL) ? tls->cert_file : NULL;
	const char *keyfile  = (tls != NULL) ? tls->key_file : NULL;
	/* mosquitto_tls_set requires at least one of (cafile, capath)
	 * for peer verification.  Fall through to the system CA dir
	 * when the caller doesn't pin a specific bundle. */
	const char *capath = (cafile == NULL) ? ALP_SDK_YOCTO_DEFAULT_CA_PATH : NULL;
	int         rc     = mosquitto_tls_set(be->mosq, cafile, capath, certfile, keyfile, NULL);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq_to_alp(rc);
	}
	if (tls != NULL && tls->insecure) {
		/* mosquitto_tls_insecure_set must be called *after*
		 * mosquitto_tls_set; documented behavior. */
		rc = mosquitto_tls_insecure_set(be->mosq, true);
		if (rc != MOSQ_ERR_SUCCESS) {
			return mosq_to_alp(rc);
		}
	}
	return ALP_OK;
}

static int qos_to_int(alp_mqtt_qos_t q)
{
	switch (q) {
	case ALP_MQTT_QOS_1:
		return 1;
	case ALP_MQTT_QOS_2:
		return 2;
	case ALP_MQTT_QOS_0:
	default:
		return 0;
	}
}

static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	(void)mosq;
	struct mqtt_be *be = (struct mqtt_be *)obj;
	if (be == NULL || msg == NULL || msg->topic == NULL) {
		return;
	}
	for (size_t i = 0; i < be->nsubs; ++i) {
		bool match = false;
		int  rc    = mosquitto_topic_matches_sub(be->subs[i].filter, msg->topic, &match);
		if (rc != MOSQ_ERR_SUCCESS) {
			continue;
		}
		if (match && be->subs[i].cb != NULL) {
			be->subs[i].cb(msg->topic,
			               (const uint8_t *)msg->payload,
			               (size_t)msg->payloadlen,
			               be->subs[i].user);
		}
	}
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
	(void)mosq;
	struct mqtt_be *be = (struct mqtt_be *)obj;
	if (be != NULL) {
		be->connected = (rc == 0);
	}
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
	(void)mosq;
	(void)rc;
	struct mqtt_be *be = (struct mqtt_be *)obj;
	if (be != NULL) {
		be->connected = false;
	}
}

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t
y_open(const alp_mqtt_config_t *cfg, alp_mqtt_backend_state_t *state, alp_capabilities_t *caps_out)
{
	/* The dispatcher has already validated cfg / broker_uri /
	 * client_id non-NULL. */
	if (!g_mosq_lib_init_done) {
		if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
			return ALP_ERR_IO;
		}
		g_mosq_lib_init_done = true;
	}

	struct mqtt_be *be = be_acquire();
	if (be == NULL) {
		return ALP_ERR_NOMEM;
	}

	alp_status_t rc = parse_broker_uri(be, cfg->broker_uri);
	if (rc != ALP_OK) {
		be_release(be);
		return rc;
	}
	be->keepalive_s = (cfg->keepalive_s != 0) ? cfg->keepalive_s : 60;

	be->mosq = mosquitto_new(cfg->client_id, cfg->clean_session, be);
	if (be->mosq == NULL) {
		be_release(be);
		return (errno == ENOMEM) ? ALP_ERR_NOMEM : ALP_ERR_INVAL;
	}
	if (cfg->username != NULL) {
		int mrc = mosquitto_username_pw_set(be->mosq, cfg->username, cfg->password);
		if (mrc != MOSQ_ERR_SUCCESS) {
			be_release(be);
			return mosq_to_alp(mrc);
		}
	}
	/* TLS hookup happens before the callback wiring so a TLS config
	 * error surfaces here rather than later at connect() time when
	 * it's harder to attribute to a misconfigured CA bundle. */
	alp_status_t tls_rc = apply_tls(be, cfg->tls);
	if (tls_rc != ALP_OK) {
		be_release(be);
		return tls_rc;
	}
	mosquitto_message_callback_set(be->mosq, on_message);
	mosquitto_connect_callback_set(be->mosq, on_connect);
	mosquitto_disconnect_callback_set(be->mosq, on_disconnect);

	state->be_data  = be;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t y_connect(alp_mqtt_backend_state_t *state, uint32_t timeout_ms)
{
	struct mqtt_be *be = (struct mqtt_be *)state->be_data;
	if (be == NULL || !be->in_use || be->mosq == NULL) {
		return ALP_ERR_NOT_READY;
	}
	/* mosquitto_connect is synchronous-by-default (handshakes the
	 * TCP + MQTT CONNECT/CONNACK exchange before returning) and
	 * already honours network timeouts through the underlying
	 * socket layer.  The caller's timeout_ms acts as a guard for
	 * the post-connect handshake -- we drive one mosquitto_loop
	 * pass with it so the on_connect callback flips be->connected
	 * before this function returns. */
	int rc = mosquitto_connect(be->mosq, be->host, be->port, be->keepalive_s);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq_to_alp(rc);
	}
	/* Drive the loop once so the CONNACK callback fires. */
	rc = mosquitto_loop(be->mosq, (int)timeout_ms, 1);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq_to_alp(rc);
	}
	return be->connected ? ALP_OK : ALP_ERR_NOT_READY;
}

static alp_status_t y_publish(alp_mqtt_backend_state_t *state,
                              const char               *topic,
                              const uint8_t            *payload,
                              size_t                    len,
                              alp_mqtt_qos_t            qos,
                              bool                      retain)
{
	struct mqtt_be *be = (struct mqtt_be *)state->be_data;
	if (be == NULL || !be->in_use || be->mosq == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (len > INT_MAX) {
		return ALP_ERR_INVAL;
	}
	int rc = mosquitto_publish(be->mosq, NULL, topic, (int)len, payload, qos_to_int(qos), retain);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq_to_alp(rc);
	}
	return ALP_OK;
}

static alp_status_t y_subscribe(alp_mqtt_backend_state_t *state,
                                const char               *topic_filter,
                                alp_mqtt_qos_t            qos,
                                alp_mqtt_msg_cb_t         cb,
                                void                     *user)
{
	struct mqtt_be *be = (struct mqtt_be *)state->be_data;
	if (be == NULL || !be->in_use || be->mosq == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (be->nsubs >= ARRAY_SIZE(be->subs)) {
		return ALP_ERR_NOMEM;
	}
	char *filter_copy = strdup(topic_filter);
	if (filter_copy == NULL) {
		return ALP_ERR_NOMEM;
	}
	int rc = mosquitto_subscribe(be->mosq, NULL, topic_filter, qos_to_int(qos));
	if (rc != MOSQ_ERR_SUCCESS) {
		free(filter_copy);
		return mosq_to_alp(rc);
	}
	be->subs[be->nsubs].filter = filter_copy;
	be->subs[be->nsubs].cb     = cb;
	be->subs[be->nsubs].user   = user;
	++be->nsubs;
	return ALP_OK;
}

static alp_status_t y_loop(alp_mqtt_backend_state_t *state, uint32_t timeout_ms)
{
	struct mqtt_be *be = (struct mqtt_be *)state->be_data;
	if (be == NULL || !be->in_use || be->mosq == NULL) {
		return ALP_ERR_NOT_READY;
	}
	/* mosquitto_loop's timeout is int-typed; clamp at INT_MAX so a
	 * caller passing UINT32_MAX-as-forever doesn't roll over. */
	int t  = (timeout_ms > (uint32_t)INT_MAX) ? INT_MAX : (int)timeout_ms;
	int rc = mosquitto_loop(be->mosq, t, 1);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq_to_alp(rc);
	}
	return ALP_OK;
}

static void y_close(alp_mqtt_backend_state_t *state)
{
	struct mqtt_be *be = (struct mqtt_be *)state->be_data;
	if (be == NULL) {
		return;
	}
	be_release(be);
	state->be_data = NULL;
}

/* ---------- Registration ---------- */

static const alp_mqtt_ops_t _ops = {
	.open      = y_open,
	.connect   = y_connect,
	.publish   = y_publish,
	.subscribe = y_subscribe,
	.loop      = y_loop,
	.close     = y_close,
};

ALP_BACKEND_REGISTER(mqtt,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
