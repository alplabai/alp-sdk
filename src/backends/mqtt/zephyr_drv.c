/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr mqtt_client backend for the <alp/iot.h> MQTT
 * surface.  Lifts the body of src/zephyr/iot_zephyr.c (the legacy
 * v0.2 MQTT wrapper -- the Wi-Fi half migrated earlier on this
 * branch) into a registry-shaped backend.  Registers as
 * silicon_ref="*" at priority 100 -- mirrors the design spec
 * Section 2 backend matrix (zephyr_drv wins on every SoC unless a
 * more specific backend registers).
 *
 * AEN CC3501E note: Wi-Fi/BLE radio operations route through the exact
 * CC3501E backends, not this MQTT backend.  MQTT remains a protocol client
 * above a socket provider; the chip-level cc3501e_sock_* helpers stay under
 * <alp/chips/cc3501e.h> for bridge diagnostics.
 *
 * Gated on CONFIG_ALP_SDK_IOT_MQTT -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with
 * CONFIG_MQTT_LIB + CONFIG_NET_TCP in the device tree.
 *
 * Backend-owned state:
 *   - struct mqtt_be (per-handle; mqtt_client, sockaddr_storage,
 *     rx/tx scratch, topic scratch, msg_cb/user pair, connected flag,
 *     msg-id counter, the client_id / username / password copies so
 *     reconnects survive the customer's source-cfg lifetime, and the
 *     resumable PUBLISH-payload drain state, issue #1938).
 *
 * Allocated from a fixed-size per-handle pool (sized by
 * CONFIG_ALP_SDK_MAX_MQTT_HANDLES) and indexed by slot lookup at
 * open / close edges -- the dispatcher's slot pool and this pool
 * have a 1:1 mapping but stay independent so the backend can compile
 * standalone for unit tests.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "alp_errno.h"
#include "alp_slot_claim.h"
#include "mqtt_ops.h"

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

LOG_MODULE_REGISTER(alp_iot_mqtt_zephyr, CONFIG_LOG_DEFAULT_LEVEL);
#endif

/* ------------------------------------------------------------------ */
/* Pool sizes                                                          */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_MQTT_HANDLES
#define CONFIG_ALP_SDK_MAX_MQTT_HANDLES 2
#endif

/* MQTT scratch buffers per client.  256 B is enough for the kind of
 * JSON payloads the v0.2 reference apps publish (status + a handful
 * of inference results); apps that publish larger blobs override via
 * CONFIG_ALP_SDK_MQTT_BUF_SIZE. */
#ifndef CONFIG_ALP_SDK_MQTT_BUF_SIZE
#define CONFIG_ALP_SDK_MQTT_BUF_SIZE 256
#endif

/* ------------------------------------------------------------------ */
/* Per-handle backend state                                            */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
struct mqtt_be {
	alp_mqtt_msg_cb_t       msg_cb;
	void                   *msg_user;
	struct mqtt_client      client;
	struct sockaddr_storage broker_addr;
	uint8_t                 rx_buf[CONFIG_ALP_SDK_MQTT_BUF_SIZE];
	uint8_t                 tx_buf[CONFIG_ALP_SDK_MQTT_BUF_SIZE];
	uint8_t                 topic_buf[128]; /* scratch for incoming topic */
	bool                    connected;
	char                    client_id_buf[64];
	char                    username_buf[64];
	char                    password_buf[64];
	struct mqtt_utf8        username_utf8;
	struct mqtt_utf8        password_utf8;
	uint16_t                next_msg_id; /* monotonic, wraps past 0xFFFF */

	/* Resumable PUBLISH-payload drain (issue #1938) -- see
	 * mqtt_drain_step()'s doc comment.  A drain can span more than one
	 * alp_mqtt_loop()/alp_mqtt_connect() call, so its progress has to
	 * live here rather than on any one call's stack. */
	uint32_t drain_deadline; /* spanning lifetime bound; stamped once, when the drain starts */
	uint32_t call_deadline;  /* the CURRENT loop()/connect() call's own deadline */
	size_t   drain_total;    /* the PUBLISH's advertised payload length */
	size_t   drain_owed;     /* payload bytes not yet off the wire */
	size_t   drain_rx_got;   /* bytes placed into rx_buf so far (drain_deliver only) */
	uint16_t drain_msg_id;
	uint8_t  drain_qos;
	bool     drain_pending; /* a drain is in progress, possibly spanning calls */
	bool     drain_deliver; /* false: pure discard (msg_cb == NULL).  A payload past
	                         * rx_buf's cap stays drain_deliver == true -- mqtt_drain_step()
	                         * just redirects the overflow into a scratch buffer instead. */

	/* Moved to the last member (was first) so the atomic-claim zeroing
	 * below (memset up to offsetof(..., in_use)) still resets every
	 * other field, matching the pre-fix full-struct memset -- issue
	 * #629: the claim now flips this flag with a single compare-
	 * exchange instead of an unlocked check-then-set. */
	bool in_use;
};

static struct mqtt_be g_mqtt_be_pool[CONFIG_ALP_SDK_MAX_MQTT_HANDLES];

static struct mqtt_be *mqtt_be_acquire(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_mqtt_be_pool); ++i) {
		if (alp_slot_try_claim(&g_mqtt_be_pool[i].in_use)) {
			memset(&g_mqtt_be_pool[i], 0, offsetof(struct mqtt_be, in_use));
			return &g_mqtt_be_pool[i];
		}
	}
	return NULL;
}

static void mqtt_be_release(struct mqtt_be *be)
{
	if (be != NULL) alp_slot_release(&be->in_use);
}

static alp_status_t errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * This switch was one of 27 hand-copied copies that had drifted; the
	 * arms it carried all agreed with the baseline, so the mapping it
	 * produced for them is unchanged. */
	return alp_status_from_zephyr_errno(err);
}

/* Parse "mqtt(s)?://host[:port]" into host/port/tls.  Returns 0 on
 * success.  No URI-encoding handling -- broker addresses in v0.2 are
 * expected to be plain hostnames or IPs. */
static int parse_broker_uri(const char *uri,
                            char       *host_buf,
                            size_t      host_buf_len,
                            uint16_t   *port_out,
                            bool       *tls_out)
{
	if (uri == NULL) return -EINVAL;

	bool        tls    = false;
	const char *cursor = uri;
	if (strncmp(cursor, "mqtts://", 8) == 0) {
		tls = true;
		cursor += 8;
	} else if (strncmp(cursor, "mqtt://", 7) == 0) {
		tls = false;
		cursor += 7;
	} else {
		return -EINVAL;
	}

	/* Default port: 1883 for plain, 8883 for TLS. */
	uint16_t port = tls ? 8883 : 1883;

	const char *colon = strrchr(cursor, ':');
	const char *slash = strchr(cursor, '/');
	size_t      host_len;

	if (colon != NULL && (slash == NULL || colon < slash)) {
		host_len    = (size_t)(colon - cursor);
		long parsed = strtol(colon + 1, NULL, 10);
		if (parsed <= 0 || parsed > 65535) return -EINVAL;
		port = (uint16_t)parsed;
	} else if (slash != NULL) {
		host_len = (size_t)(slash - cursor);
	} else {
		host_len = strlen(cursor);
	}

	if (host_len == 0 || host_len >= host_buf_len) return -EINVAL;
	memcpy(host_buf, cursor, host_len);
	host_buf[host_len] = '\0';

	*port_out = port;
	*tls_out  = tls;
	return 0;
}

static int resolve_broker_addr(const char *host, uint16_t port, struct sockaddr_storage *out)
{
	/* Prefer numeric IPv4 first -- keeps the wrapper resolver-free for
     * the common "broker is a static IP" case.  When CONFIG_DNS_RESOLVER
     * is enabled the caller can pre-resolve via getaddrinfo and pass the
     * numeric form. */
	struct sockaddr_in *sin = (struct sockaddr_in *)out;
	memset(out, 0, sizeof(*out));
	sin->sin_family = AF_INET;
	sin->sin_port   = htons(port);
	if (zsock_inet_pton(AF_INET, host, &sin->sin_addr) == 1) return 0;

#if defined(CONFIG_DNS_RESOLVER)
	struct zsock_addrinfo  hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res   = NULL;
	int                    err   = zsock_getaddrinfo(host, NULL, &hints, &res);
	if (err != 0 || res == NULL) {
		if (res != NULL) zsock_freeaddrinfo(res);
		return -EHOSTUNREACH;
	}
	*sin          = *(const struct sockaddr_in *)res->ai_addr;
	sin->sin_port = htons(port);
	zsock_freeaddrinfo(res);
	return 0;
#else
	return -EHOSTUNREACH;
#endif
}

/* Pull the active socket fd out of the mqtt client.  Path differs
 * between non-secure (transport.tcp) and TLS (transport.tls) variants;
 * v0.2 only ships non-secure (TLS lands with security.h in v0.3) but
 * the helper is shaped to extend cleanly.  Moved above the payload-read
 * helper below (was originally defined after alp_mqtt_evt_cb) so that
 * helper can poll the socket fd while waiting on -EAGAIN (issue #1938). */
static int alp_mqtt_get_fd(struct mqtt_client *c)
{
#if defined(CONFIG_MQTT_LIB_TLS)
	if (c->transport.type == MQTT_TRANSPORT_SECURE) {
		return c->transport.tls.sock;
	}
#endif
	return c->transport.tcp.sock;
}

/* A drain that never receives another byte must not hold the connection
 * open forever -- this is the SPANNING lifetime bound (issue #1938): once
 * a drain becomes pending, it has this long in total, across as many
 * alp_mqtt_loop() calls as it takes, before mqtt_drain_step() gives up and
 * tears the connection down.  Same budget PR #1658 used for #1645. */
#define ALP_MQTT_DRAIN_TIMEOUT_MS 5000u

/* Clamps a caller-supplied timeout to a value k_uptime_get_32() + timeout_ms
 * can safely subtract back out of.  A wraparound-safe "time left" check
 * elsewhere in this file casts (deadline - now) to int32_t; a timeout_ms
 * above INT32_MAX would make that cast lie (issue #1938). */
static uint32_t mqtt_clamp_timeout_ms(uint32_t timeout_ms)
{
	return (timeout_ms > (uint32_t)INT32_MAX) ? (uint32_t)INT32_MAX : timeout_ms;
}

/* True when a POSITIVE zsock_poll() return means the fd itself is torn
 * down or errored, not that data is ready.  Zephyr reports a torn-down/
 * invalid fd as POLLNVAL/POLLERR in revents with a POSITIVE poll() return
 * -- never rc < 0 -- so every zsock_poll() call in this file (the drain's
 * own, plus z_connect()'s and z_loop()'s connection-wide polls) has to
 * check revents in addition to rc, or a dead socket hot-spins at 100% CPU
 * for the rest of whatever deadline is in play (issue #1938 item 4). */
static bool mqtt_poll_fd_is_dead(short revents)
{
	return (revents & (ZSOCK_POLLNVAL | ZSOCK_POLLERR)) != 0;
}

/* Advances the pending PUBLISH-payload drain in `be` by whatever the
 * socket has ready before `until` (a k_uptime_get_32() timestamp -- the
 * CURRENT alp_mqtt_loop()/alp_mqtt_connect() call's own deadline, never
 * the drain's full spanning lifetime) elapses.
 *
 * mqtt_rx.c sets internal.remaining_payload from the PUBLISH's advertised
 * header length the instant the header is decoded -- before the payload
 * bytes have necessarily all arrived on the wire.  mqtt_read_publish_payload()
 * is the non-blocking variant and returns -EAGAIN verbatim once the socket
 * has no more buffered bytes right now, which for a payload spanning more
 * than one TCP segment is not a hard failure, just "not here yet".  Leaving
 * remaining_payload > 0 makes client_read() answer -EBUSY on every
 * subsequent mqtt_input() -- the connection is wedged until torn down
 * (issue #1645).
 *
 * Bounding the retry to the CALLER's own budget (rather than blocking here
 * for up to ALP_MQTT_DRAIN_TIMEOUT_MS, as earlier revisions of this fix
 * did) is the point of #1938: a customer's short poll interval must not
 * abort a perfectly healthy connection that just hasn't finished
 * delivering one publish yet.  Running out of THIS call's budget with
 * bytes still owed is not a failure -- be->drain_pending stays true and a
 * later call resumes exactly where this one left off.  Only running out of
 * the drain's own spanning lifetime (be->drain_deadline, stamped once when
 * the drain first became pending) is a real failure, because at that point
 * the peer itself has gone silent for ALP_MQTT_DRAIN_TIMEOUT_MS, not just
 * the caller's poll interval.
 *
 * Returns true once be->drain_owed reaches 0 (drain complete -- the caller
 * must still finish delivery via mqtt_drain_finish()).  Returns false
 * otherwise; the caller tells "try again later" apart from "connection is
 * gone" via be->drain_pending (left true vs. cleared) -- a hard read/poll
 * error or the spanning deadline both call mqtt_abort() themselves and
 * clear it. */
static bool mqtt_drain_step(struct mqtt_be *be, uint32_t until)
{
	struct mqtt_client *client = &be->client;
	uint8_t             scratch[64];

	while (be->drain_owed > 0) {
		uint8_t *dst;
		size_t   chunk;
		if (be->drain_deliver && be->drain_rx_got < sizeof(be->rx_buf)) {
			dst   = be->rx_buf + be->drain_rx_got;
			chunk = MIN(be->drain_owed, sizeof(be->rx_buf) - be->drain_rx_got);
		} else {
			dst   = scratch;
			chunk = MIN(be->drain_owed, sizeof(scratch));
		}

		int n = mqtt_read_publish_payload(client, dst, chunk);
		if (n > 0) {
			if (dst != scratch) be->drain_rx_got += (size_t)n;
			be->drain_owed -= (size_t)n;
			continue;
		}
		if (n != -EAGAIN) {
			LOG_WRN("mqtt: publish read failed (%d) with %zu of %zu bytes still owed; "
			        "aborting the connection rather than leaving it wedged at -EBUSY",
			        n,
			        be->drain_owed,
			        be->drain_total);
			goto tear_down;
		}

		/* -EAGAIN: nothing buffered right now.  Poll bounded by whichever
		 * of this call's own budget or the drain's spanning lifetime
		 * comes first -- never past either. */
		uint32_t bound   = ((int32_t)(be->drain_deadline - until) < 0) ? be->drain_deadline : until;
		int32_t  left_ms = (int32_t)(bound - k_uptime_get_32());
		if (left_ms <= 0) {
			if ((int32_t)(be->drain_deadline - k_uptime_get_32()) <= 0) {
				LOG_WRN("mqtt: publish drain exceeded its %u ms lifetime with %zu of "
				        "%zu bytes still owed; aborting the connection",
				        ALP_MQTT_DRAIN_TIMEOUT_MS,
				        be->drain_owed,
				        be->drain_total);
				goto tear_down;
			}
			return false; /* this call's own budget is spent -- resume later */
		}

		struct zsock_pollfd pfd = { .fd = alp_mqtt_get_fd(client), .events = ZSOCK_POLLIN };
		int                 rc  = zsock_poll(&pfd, 1, left_ms);
		if (rc < 0) {
			if (errno == EINTR) continue; /* benign, retryable -- not a real failure */
			LOG_WRN("mqtt: publish drain poll failed (errno %d) with %zu of %zu bytes "
			        "still owed; aborting the connection",
			        errno,
			        be->drain_owed,
			        be->drain_total);
			goto tear_down;
		}
		/* See mqtt_poll_fd_is_dead()'s doc comment (issue #1938 item 4):
		 * a torn-down/invalid fd reports here as rc > 0, not rc < 0. */
		if (rc > 0 && mqtt_poll_fd_is_dead(pfd.revents)) {
			LOG_WRN("mqtt: publish drain socket torn down (revents 0x%x) with %zu of "
			        "%zu bytes still owed; aborting the connection",
			        pfd.revents,
			        be->drain_owed,
			        be->drain_total);
			goto tear_down;
		}
		/* rc == 0 (this poll slice timed out) or POLLIN is ready either
		 * way: loop back up -- the left_ms <= 0 check above is what
		 * actually detects running out of time. */
	}

	be->drain_pending = false;
	return true;

tear_down:
	(void)mqtt_abort(client);
	/* mqtt_abort() does not itself run the MQTT_EVT_DISCONNECT path, so
	 * be->connected would otherwise still read true -- flip it here so
	 * z_loop()/z_connect() can tell the caller their connection is gone
	 * (issue #1938 item 1) instead of returning ALP_OK on the very call
	 * that tore it down. */
	be->connected     = false;
	be->drain_pending = false;
	return false;
}

/* Finishes a drain mqtt_drain_step() has just reported complete: the QoS-1
 * PUBACK (withheld until now -- see mqtt_drain_step()'s doc comment on
 * issue #1645) and the user's msg_cb, if this drain was for a bound
 * subscription rather than a background discard.
 *
 * The PUBACK is unconditional on QoS, never on drain_deliver: a discarded
 * PUBLISH (msg_cb == NULL) is still a PUBLISH the broker is owed an ack
 * for, or it redelivers forever.  Returning before it here was issue
 * #1938 item 5's regression -- pre-restructure, every drain path acked
 * before this function existed at all. */
static void mqtt_drain_finish(struct mqtt_be *be)
{
	if (be->drain_qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = { .message_id = be->drain_msg_id };
		(void)mqtt_publish_qos1_ack(&be->client, &ack);
	}

	if (!be->drain_deliver) return;

	if (be->drain_rx_got < be->drain_total) {
		/* alp_mqtt_msg_cb_t (<alp/iot.h>) has no channel to report a
		 * truncated delivery to the caller -- drain_rx_got below is
		 * silently short.  Log it so the drop is at least visible;
		 * widening the callback signature is a public-API change out
		 * of scope for this fix. */
		LOG_WRN("mqtt: payload %u B truncated to rx_buf's %u B",
		        (unsigned)be->drain_total,
		        (unsigned)be->drain_rx_got);
	}

	be->msg_cb((const char *)be->topic_buf, be->rx_buf, be->drain_rx_got, be->msg_user);
}

/* Resumes a drain an earlier alp_mqtt_loop()/alp_mqtt_connect() call left
 * pending, spending up to THIS call's own budget on it before returning --
 * never the drain's whole spanning lifetime in one shot.  Called before
 * touching any new event: mqtt_input() itself won't produce another
 * MQTT_EVT_PUBLISH (or process anything else) while Zephyr's own
 * internal.remaining_payload is still non-zero (see mqtt_drain_step()'s
 * doc comment), so resuming here first is the only way forward. */
static alp_status_t mqtt_drain_resume(struct mqtt_be *be, uint32_t call_deadline)
{
	bool was_connected = be->connected;

	if (mqtt_drain_step(be, call_deadline)) {
		mqtt_drain_finish(be);
	}
	if (was_connected && !be->connected) return ALP_ERR_IO;
	return ALP_OK;
}

/* Event handler -- called from mqtt_input() in the user's loop
 * thread.  Maps Zephyr MQTT events into the alp surface (CONNACK
 * sets connected=true; PUBLISH dispatches to the user callback). */
static void alp_mqtt_evt_cb(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	struct mqtt_be *be = CONTAINER_OF(client, struct mqtt_be, client);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		be->connected = (evt->result == 0);
		break;
	case MQTT_EVT_DISCONNECT:
		be->connected = false;
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;

		/* Starts a fresh drain (issue #1938) -- be->drain_pending is
		 * always false on entry here: mqtt_input() cannot deliver
		 * another MQTT_EVT_PUBLISH while an earlier one's
		 * remaining_payload is still non-zero, and z_loop()/z_connect()
		 * resume any pending drain before ever calling mqtt_input(). */
		be->drain_deliver  = (be->msg_cb != NULL);
		be->drain_total    = pub->message.payload.len;
		be->drain_owed     = pub->message.payload.len;
		be->drain_rx_got   = 0;
		be->drain_qos      = (uint8_t)pub->message.topic.qos;
		be->drain_msg_id   = pub->message_id;
		be->drain_deadline = k_uptime_get_32() + ALP_MQTT_DRAIN_TIMEOUT_MS;
		be->drain_pending  = true;

		if (be->drain_deliver) {
			/* Copy the topic into our scratch buffer so we can
             * NUL-terminate it for the public callback (the wire form
             * is length-delimited). */
			size_t topic_len = MIN(pub->message.topic.topic.size, sizeof(be->topic_buf) - 1);
			memcpy(be->topic_buf, pub->message.topic.topic.utf8, topic_len);
			be->topic_buf[topic_len] = '\0';
		}

		if (mqtt_drain_step(be, be->call_deadline)) {
			mqtt_drain_finish(be);
		}
		/* Else: be->drain_pending tells z_loop()/z_connect() apart --
		 * still true means "resume next call", cleared (by
		 * mqtt_drain_step() itself) means the connection is gone. */
		break;
	}
	default:
		break;
	}
}
#endif /* CONFIG_ALP_SDK_IOT_MQTT */

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t
z_open(const alp_mqtt_config_t *cfg, alp_mqtt_backend_state_t *st, alp_capabilities_t *caps_out)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = mqtt_be_acquire();
	if (be == NULL) {
		caps_out->flags = 0u;
		return ALP_ERR_NOMEM;
	}

	char     host[64];
	uint16_t port;
	bool     tls;
	int      err = parse_broker_uri(cfg->broker_uri, host, sizeof(host), &port, &tls);
	if (err != 0) {
		mqtt_be_release(be);
		caps_out->flags = 0u;
		return ALP_ERR_INVAL;
	}

#if !defined(CONFIG_MQTT_LIB_TLS)
	if (tls) {
		/* Caller asked for `mqtts://` but CONFIG_MQTT_LIB_TLS is off --
		 * fail closed (GHSA-gqjv-932h-c5gm).  A secure scheme must
		 * never be reinterpreted as plaintext; reject here, before
		 * resolving the broker address or copying any credentials
		 * into backend state, so nothing is transmitted. */
		mqtt_be_release(be);
		caps_out->flags = 0u;
		return ALP_ERR_NOSUPPORT;
	}
#endif

	err = resolve_broker_addr(host, port, &be->broker_addr);
	if (err != 0) {
		mqtt_be_release(be);
		caps_out->flags = 0u;
		return errno_to_alp(err);
	}

	/* Stash the client id locally so we own its memory across
     * reconnects -- the caller's pointer can go out of scope. */
	strncpy(be->client_id_buf, cfg->client_id, sizeof(be->client_id_buf) - 1);

	mqtt_client_init(&be->client);
	be->client.broker         = &be->broker_addr;
	be->client.evt_cb         = alp_mqtt_evt_cb;
	be->client.client_id.utf8 = (uint8_t *)be->client_id_buf;
	be->client.client_id.size = strlen(be->client_id_buf);
	if (cfg->username != NULL) {
		strncpy(be->username_buf, cfg->username, sizeof(be->username_buf) - 1);
		be->username_utf8.utf8 = (uint8_t *)be->username_buf;
		be->username_utf8.size = strlen(be->username_buf);
		be->client.user_name   = &be->username_utf8;
	} else {
		be->client.user_name = NULL;
	}
	if (cfg->password != NULL) {
		strncpy(be->password_buf, cfg->password, sizeof(be->password_buf) - 1);
		be->password_utf8.utf8 = (uint8_t *)be->password_buf;
		be->password_utf8.size = strlen(be->password_buf);
		be->client.password    = &be->password_utf8;
	} else {
		be->client.password = NULL;
	}
	be->client.protocol_version = MQTT_VERSION_3_1_1;
#if defined(CONFIG_MQTT_LIB_TLS)
	be->client.transport.type = tls ? MQTT_TRANSPORT_SECURE : MQTT_TRANSPORT_NON_SECURE;
#else
	/* tls is always false here -- the check above already rejected
	 * an `mqtts://` request when CONFIG_MQTT_LIB_TLS is off. */
	be->client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
	be->client.rx_buf        = be->rx_buf;
	be->client.rx_buf_size   = sizeof(be->rx_buf);
	be->client.tx_buf        = be->tx_buf;
	be->client.tx_buf_size   = sizeof(be->tx_buf);
	be->client.keepalive     = cfg->keepalive_s;
	be->client.clean_session = cfg->clean_session ? 1U : 0U;
	be->connected            = false;
	be->next_msg_id          = 1;

	st->be_data     = be;
	caps_out->flags = 0u;
	return ALP_OK;
#else
	(void)cfg;
	(void)st;
	caps_out->flags = 0u;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_connect(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = (struct mqtt_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	/* A drain left pending by a PREVIOUS session (e.g. z_publish()'s
	 * ALP_ERR_IO path leaves drain_pending set, or a caller that just
	 * retries alp_mqtt_connect() on the same handle per
	 * docs/tutorials/11-mqtt-tls-publish.md:219) must not resume against
	 * the FRESH socket mqtt_connect() is about to open below -- its
	 * spanning deadline is stale and mqtt_drain_step() would abort the
	 * brand-new connection the instant it next runs.  Clear it before
	 * connecting so the wait loop's mqtt_drain_resume() call never sees
	 * state from before this connect (issue #1938 item 3). */
	be->drain_pending  = false;
	be->drain_deadline = 0;
	be->drain_owed     = 0;
	be->drain_rx_got   = 0;
	be->topic_buf[0]   = '\0';

	int err = mqtt_connect(&be->client);
	if (err != 0) {
		return errno_to_alp(err);
	}

	/* Pump input until we get CONNACK (which the evt cb sets) or the
     * timeout expires.  poll() with a short slice keeps the wait
     * responsive without busy-spinning. */
	timeout_ms        = mqtt_clamp_timeout_ms(timeout_ms);
	uint32_t deadline = k_uptime_get_32() + timeout_ms;
	be->call_deadline = deadline; /* bounds a stray PUBLISH's drain during connect (#1938) */
	while ((int32_t)(deadline - k_uptime_get_32()) > 0) {
		if (be->drain_pending) {
			/* Resume before anything else -- see mqtt_drain_resume()'s
			 * doc comment; mqtt_input() below won't make progress on a
			 * fresh event until this clears. */
			alp_status_t rc = mqtt_drain_resume(be, deadline);
			if (rc != ALP_OK) return rc;
			continue;
		}

		struct zsock_pollfd fds[1] = { 0 };
		fds[0].fd                  = alp_mqtt_get_fd(&be->client);
		fds[0].events              = ZSOCK_POLLIN;
		int rc                     = zsock_poll(fds, 1, 200);
		if (rc < 0) return errno_to_alp(-errno);
		/* See mqtt_poll_fd_is_dead()'s doc comment (issue #1938 item 4):
		 * a torn-down/invalid fd reports here as rc > 0, not rc < 0. */
		if (rc > 0 && mqtt_poll_fd_is_dead(fds[0].revents)) {
			be->connected = false;
			return ALP_ERR_IO;
		}
		if (rc > 0) {
			err = mqtt_input(&be->client);
			if (err != 0) return errno_to_alp(err);
		}
		err = mqtt_live(&be->client);
		if (err != 0 && err != -EAGAIN) return errno_to_alp(err);

		if (be->connected) return ALP_OK;
	}
	return ALP_ERR_TIMEOUT;
#else
	(void)st;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_publish(alp_mqtt_backend_state_t *st,
                              const char               *topic,
                              const uint8_t            *payload,
                              size_t                    len,
                              alp_mqtt_qos_t            qos,
                              bool                      retain)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = (struct mqtt_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	struct mqtt_publish_param p = { 0 };
	p.message.topic.topic.utf8  = (const uint8_t *)topic;
	p.message.topic.topic.size  = strlen(topic);
	p.message.topic.qos         = (uint8_t)qos;
	p.message.payload.data      = (uint8_t *)payload;
	p.message.payload.len       = len;
	p.dup_flag                  = 0;
	p.retain_flag               = retain ? 1 : 0;
	if (qos == ALP_MQTT_QOS_0) {
		p.message_id = 0;
	} else {
		if (be->next_msg_id == 0) be->next_msg_id = 1; /* msg-id 0 is reserved */
		p.message_id = be->next_msg_id++;
	}

	int err = mqtt_publish(&be->client, &p);
	return errno_to_alp(err);
#else
	(void)st;
	(void)topic;
	(void)payload;
	(void)len;
	(void)qos;
	(void)retain;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_subscribe(alp_mqtt_backend_state_t *st,
                                const char               *topic_filter,
                                alp_mqtt_qos_t            qos,
                                alp_mqtt_msg_cb_t         cb,
                                void                     *user)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = (struct mqtt_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	be->msg_cb   = cb;
	be->msg_user = user;

	struct mqtt_topic topic = {
		.topic.utf8 = (const uint8_t *)topic_filter,
		.topic.size = strlen(topic_filter),
		.qos        = (uint8_t)qos,
	};
	if (be->next_msg_id == 0) be->next_msg_id = 1;
	struct mqtt_subscription_list list = {
		.list       = &topic,
		.list_count = 1,
		.message_id = be->next_msg_id++,
	};
	int err = mqtt_subscribe(&be->client, &list);
	return errno_to_alp(err);
#else
	(void)st;
	(void)topic_filter;
	(void)qos;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_loop(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = (struct mqtt_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;

	timeout_ms             = mqtt_clamp_timeout_ms(timeout_ms);
	uint32_t call_deadline = k_uptime_get_32() + timeout_ms;

	if (be->drain_pending) {
		/* Resumable drain (issue #1938): a caller's short timeout_ms
		 * must never abort a payload that just hasn't fully arrived yet
		 * -- finish what an earlier call left owed, bounded by THIS
		 * call's own budget, before touching anything new. See
		 * mqtt_drain_resume()'s doc comment. */
		return mqtt_drain_resume(be, call_deadline);
	}

	bool was_connected = be->connected;
	be->call_deadline  = call_deadline; /* bounds any drain a fresh PUBLISH starts below */

	int32_t remaining_ms = (int32_t)(call_deadline - k_uptime_get_32());
	if (remaining_ms < 0) remaining_ms = 0;

	struct zsock_pollfd fds[1] = { 0 };
	fds[0].fd                  = alp_mqtt_get_fd(&be->client);
	fds[0].events              = ZSOCK_POLLIN;
	int rc                     = zsock_poll(fds, 1, remaining_ms);
	if (rc < 0) return errno_to_alp(-errno);
	/* See mqtt_poll_fd_is_dead()'s doc comment (issue #1938 item 4): a
	 * torn-down/invalid fd reports here as rc > 0, not rc < 0. */
	if (rc > 0 && mqtt_poll_fd_is_dead(fds[0].revents)) {
		be->connected = false;
		return ALP_ERR_IO;
	}
	if (rc > 0) {
		int err = mqtt_input(&be->client);
		if (err != 0) return errno_to_alp(err);
	}
	/* A broker disconnect (graceful MQTT_EVT_DISCONNECT, or our own
	 * mqtt_abort() from a drain that gave up) must surface as
	 * ALP_ERR_IO on the very call that tore the connection down --
	 * docs/tutorials/11-mqtt-tls-publish.md documents exactly this
	 * contract, "no separate is-connected query" (issue #1938 item 1).
	 * Without this check, mqtt_input() answers 0 (the abort's own
	 * packet, if any, still parsed cleanly) and mqtt_live() answers
	 * -EAGAIN whenever no keepalive is due, so the caller would see
	 * ALP_OK on the exact call that just lost the link. */
	if (was_connected && !be->connected) return ALP_ERR_IO;
	if (be->drain_pending) return ALP_OK; /* a fresh drain above is still owed */

	int err = mqtt_live(&be->client);
	if (err != 0 && err != -EAGAIN) return errno_to_alp(err);
	if (was_connected && !be->connected) return ALP_ERR_IO;
	return ALP_OK;
#else
	(void)st;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_mqtt_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
	struct mqtt_be *be = (struct mqtt_be *)st->be_data;
	if (be == NULL) return;
	if (be->connected) {
		(void)mqtt_disconnect(&be->client, NULL);
		be->connected = false;
	}
	mqtt_be_release(be);
	st->be_data = NULL;
#else
	(void)st;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_mqtt_ops_t _ops = {
	.open      = z_open,
	.connect   = z_connect,
	.publish   = z_publish,
	.subscribe = z_subscribe,
	.loop      = z_loop,
	.close     = z_close,
};

ALP_BACKEND_REGISTER(mqtt,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
