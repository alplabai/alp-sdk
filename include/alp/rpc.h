/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rpc.h
 * @brief Alp SDK framed RPC over OpenAMP / RPMsg.
 *
 * v0.6 deliverable -- this header is the high-level surface customers
 * call from per-core app slices in a heterogeneous board.yaml
 * project.  It sits on top of the lower-level primitives in
 * `<alp/mproc.h>` (mailbox + shared memory + hardware semaphore) and
 * the build-time-generated carve-out + endpoint-id macros in
 * `<alp/system_ipc.h>` (auto-emitted by
 * `scripts/alp_orchestrate/`).
 *
 * The canonical usage example (matching spec §6.6 of
 * `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`):
 *
 * @code
 *     #include <alp/system_ipc.h>      // generated header
 *     #include <alp/rpc.h>
 *
 *     static void on_temperature(const void *payload, size_t len, void *u) {
 *         (void)u;
 *         float c;
 *         if (len == sizeof c) {
 *             memcpy(&c, payload, sizeof c);
 *             // ... use c ...
 *         }
 *     }
 *
 *     int main(void) {
 *         alp_rpc_channel_t *ch = alp_rpc_open(&(alp_rpc_config_t){
 *             .name    = ALP_IPC_ALP_DEFAULT_RPMSG_NAME,
 *             .src_ept = ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT,
 *             .dst_ept = ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT,
 *             .mbox_ch = ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH,
 *         });
 *         alp_rpc_subscribe(ch, "temperature", on_temperature, NULL);
 *         for (;;) { ... }
 *     }
 * @endcode
 *
 * @par Framing.
 * On the wire every RPMsg payload carries a tiny ASCII method header
 * followed by an opaque application-defined byte string.  The header
 * is a single-line `<method>\0` C-string (1-32 bytes, including the
 * NUL terminator).  Method names match the regex `[A-Za-z0-9_.-]+`.
 * The remaining payload bytes are passed through verbatim to the
 * callback; the SDK does not parse them.
 *
 * @note The framing format is deliberately minimal in v0.6.  A
 *       richer schema (length-prefixed binary blocks or protobuf
 *       envelopes via nanopb) is an opt-in upgrade tracked for
 *       v0.7; it will reuse the same `<alp/rpc.h>` public API.  Both
 *       ends of an `alp_rpc_channel_t` must agree on the framing
 *       format -- mixing framed and raw firmwares is a wire-protocol
 *       break.
 *
 * @par Backends.
 *   - Zephyr / M-class side: `subsys/ipc/ipc_service` with the
 *     `rpmsg` backend (`src/zephyr/rpc_zephyr.c`).
 *   - Linux / A-class side:  libmetal + librpmsg user-space chardev
 *     access to `/dev/rpmsg*` (`src/yocto/rpc_yocto.c`).
 *   - Bare-metal builds get a NOSUPPORT stub via the existing
 *     `src/common/stub_backend.c` mechanism (out of scope here).
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.6 framed RPC surface.  Adding optional fields to
 *      `alp_rpc_config_t` is permitted; reshaping the callback
 *      signatures is not.  See docs/abi-markers.md for the
 *      convention.
 */

#ifndef ALP_RPC_H
#define ALP_RPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum method-name length in bytes including the trailing NUL.
 *  Matches the on-wire framing limit (32 bytes).  Method names
 *  longer than this fail @ref alp_rpc_subscribe / @ref alp_rpc_send
 *  with @ref ALP_ERR_INVAL. */
#define ALP_RPC_METHOD_MAX_LEN 32

/** Default mailbox-channel index used when @ref alp_rpc_config_t::mbox_ch
 *  is left at zero by the caller.  Matches the channel reserved for
 *  `alp_default_rpmsg` in each SoM preset's `mailbox.channels[]`. */
#define ALP_RPC_DEFAULT_MBOX_CH 0u

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/** Opaque RPC channel handle.  Allocate via @ref alp_rpc_open and
 *  release via @ref alp_rpc_close.  All API entry points except
 *  @ref alp_rpc_open take the handle as their first argument. */
typedef struct alp_rpc_channel alp_rpc_channel_t;

/**
 * @brief Configuration passed to @ref alp_rpc_open.
 *
 * Every member except @c name is optional; uninitialised fields
 * adopt the defaults documented per-field.  Pass a designated-
 * initialiser literal directly to @ref alp_rpc_open for terse use.
 */
typedef struct {
	/** RPMsg endpoint name (matches @c ALP_IPC_<NAME>_NAME from the
     *  generated `<alp/system_ipc.h>`).  Must be non-NULL + non-empty
     *  and ≤ 32 bytes including the NUL terminator. */
	const char *name;

	/** This side's endpoint ID (@c ALP_IPC_<NAME>_SRC_EPT).  When 0
     *  the backend assigns a deterministic value from the channel
     *  name via FNV-1a hash; matching `<alp/system_ipc.h>` saves a
     *  hop. */
	uint32_t src_ept;

	/** Peer side's endpoint ID (@c ALP_IPC_<NAME>_DST_EPT).  Must
     *  agree with the value seen by the peer's @ref alp_rpc_open
     *  call -- the generated header on both sides is the contract.
     *  When 0 the backend uses `src_ept + 1` (same convention as
     *  the orchestrator). */
	uint32_t dst_ept;

	/** Mailbox channel index (@c ALP_IPC_<NAME>_MBOX_CH).  Defaults
     *  to @ref ALP_RPC_DEFAULT_MBOX_CH when 0. */
	uint32_t mbox_ch;

	/** Memory-caching policy for the carve-out.  @c false picks the
     *  non-cacheable region (the v0.6 default).  Set to @c true on
     *  AEN where M55 caches are enabled and the carve-out is in
     *  cacheable MRAM. */
	bool cacheable;
} alp_rpc_config_t;

/**
 * @brief Default-initialize an @ref alp_rpc_config_t for channel @p id.
 *
 * Identity from @p id (the @c name); every other field uses the
 * ALREADY-documented per-field default: @c src_ept = 0 (the backend
 * derives it from @c name via FNV-1a hash), @c dst_ept = 0 (the
 * backend uses `src_ept + 1`), @c mbox_ch = @ref
 * ALP_RPC_DEFAULT_MBOX_CH, @c cacheable = false (the v0.6 default --
 * non-cacheable carve-out).
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_RPC_CONFIG_DEFAULT(id)                                                                 \
	((alp_rpc_config_t){ .name      = (id),                                                        \
	                     .src_ept   = 0u,                                                          \
	                     .dst_ept   = 0u,                                                          \
	                     .mbox_ch   = ALP_RPC_DEFAULT_MBOX_CH,                                     \
	                     .cacheable = false })

/**
 * @brief Generic inbound-message callback.
 *
 * Receives the parsed method name plus the opaque payload that
 * followed it.  Runs on the backend's RX worker (Zephyr: an
 * `ipc_service` callback thread; Linux: a per-channel poll thread
 * inside `librpmsg`).  Keep the body short -- the worker is shared
 * with every other subscribe on this channel.
 *
 * @param[in] method   NUL-terminated method name (≤ 32 bytes).
 * @param[in] payload  Opaque payload bytes.  Owned by the SDK; copy
 *                     what you need before returning.
 * @param[in] len      Payload length in bytes (may be 0).
 * @param[in] user     The @c user pointer registered with the
 *                     subscribe call.
 */
typedef void (*alp_rpc_msg_cb_t)(const char *method, const void *payload, size_t len, void *user);

/**
 * @brief Per-method-subscribe callback.
 *
 * Typed wrapper around @ref alp_rpc_msg_cb_t for ergonomics: when
 * the application calls @ref alp_rpc_subscribe with a specific
 * method name the dispatch is already filtered, so the callback
 * doesn't need to re-check the method.
 *
 * @param[in] payload  Opaque payload bytes (same lifetime rules as
 *                     @ref alp_rpc_msg_cb_t).
 * @param[in] len      Payload length in bytes.
 * @param[in] user     The @c user pointer registered with the
 *                     subscribe call.
 */
typedef void (*alp_rpc_method_cb_t)(const void *payload, size_t len, void *user);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Open an RPC channel.
 *
 * Resolves the carve-out memory region declared in the generated
 * `<alp/system_ipc.h>`, brings up the OpenAMP virtio queues, and
 * registers the local endpoint with the underlying RPMsg device.
 * The peer must have called @ref alp_rpc_open with matching
 * @c name / @c src_ept / @c dst_ept values; the resolver will retry
 * the name-service handshake transparently for up to 2 s (Zephyr)
 * or 5 s (Linux) before returning NULL.
 *
 * @param[in] cfg  Channel configuration.  Must be non-NULL with a
 *                 non-NULL, non-empty @c name field.
 * @return Open handle on success, or NULL on resolution failure.
 *         Call @ref alp_last_error to learn the reason:
 *           - @ref ALP_ERR_INVAL     — @c cfg / @c name NULL or
 *                                       method-name too long
 *           - @ref ALP_ERR_NOMEM     — channel pool exhausted
 *           - @ref ALP_ERR_NOT_READY — RPMsg device or memory
 *                                       region not yet up
 *           - @ref ALP_ERR_NOSUPPORT — SDK built without
 *                                       CONFIG_ALP_SDK_RPC / no
 *                                       OpenAMP backend available
 */
alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg);

/**
 * @brief Close an RPC channel.
 *
 * Drops every subscription, deregisters the local endpoint, and
 * releases the channel handle back to the pool.  Outstanding
 * @ref alp_rpc_call invocations on the channel return
 * @ref ALP_ERR_NOT_READY before unblocking.
 *
 * @param[in] ch  Handle from @ref alp_rpc_open, or NULL (no-op).
 */
void alp_rpc_close(alp_rpc_channel_t *ch);

/**
 * @brief Query the capabilities of an opened RPC channel.
 *
 * @param ch  Handle from @ref alp_rpc_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p ch is NULL.
 */
const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch);

/* ------------------------------------------------------------------ */
/* Subscriptions                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Subscribe a callback to a named method on this channel.
 *
 * Replaces any prior registration for the same @c (ch, method) pair.
 *
 * @param[in] ch      Channel handle.
 * @param[in] method  NUL-terminated method name (≤ 32 bytes, regex
 *                    `[A-Za-z0-9_.-]+`).
 * @param[in] cb      Callback invoked when the peer sends a frame
 *                    whose method matches.  May be NULL to remove
 *                    the registration (equivalent to
 *                    @ref alp_rpc_unsubscribe).
 * @param[in] user    Opaque pointer forwarded to @p cb.
 * @return  - @ref ALP_OK          on success
 *          - @ref ALP_ERR_NOT_READY @c ch is NULL or closed
 *          - @ref ALP_ERR_INVAL   @c method is NULL, empty, or
 *                                  longer than 32 bytes
 *          - @ref ALP_ERR_NOMEM   per-channel subscribe table full
 *                                  (v0.6 cap: 8 entries)
 */
alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user);

/**
 * @brief Remove a prior @ref alp_rpc_subscribe registration.
 *
 * @param[in] ch      Channel handle.
 * @param[in] method  Method name previously passed to
 *                    @ref alp_rpc_subscribe.
 * @return  - @ref ALP_OK          on success
 *          - @ref ALP_ERR_NOT_READY @c ch is NULL or closed
 *          - @ref ALP_ERR_INVAL   no registration matched
 */
alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method);

/* ------------------------------------------------------------------ */
/* Send + call                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Fire-and-forget send to the peer endpoint.
 *
 * Frames the @c (method, payload) pair and hands it to the OpenAMP
 * TX queue.  Returns as soon as the queue accepts the frame; does
 * not wait for the peer to receive it.
 *
 * @param[in] ch       Channel handle.
 * @param[in] method   NUL-terminated method name (≤ 32 bytes).
 * @param[in] payload  Application-defined payload, or NULL when
 *                     @p len == 0.
 * @param[in] len      Payload length in bytes (excludes the method
 *                     header).
 * @return  - @ref ALP_OK          on success
 *          - @ref ALP_ERR_NOT_READY @c ch is NULL or closed
 *          - @ref ALP_ERR_INVAL   @c method invalid or
 *                                  @c payload == NULL with @c len > 0
 *          - @ref ALP_ERR_NOMEM   TX buffer pool exhausted; retry
 *          - @ref ALP_ERR_IO      OpenAMP backend returned an error
 *          - @ref ALP_ERR_NOSUPPORT backend doesn't implement send
 *                                    on this OS yet
 */
alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len);

/**
 * @brief Synchronous request/response.
 *
 * Sends @c (method, req, req_len) to the peer and blocks the caller
 * until the peer replies with a frame whose method matches @p method
 * OR @p timeout_ms elapses.  The peer is expected to reply on the
 * same channel with the same method name (the SDK does not impose a
 * "method.reply" convention -- both sides agree on the framing per
 * application).
 *
 * @param[in]     ch          Channel handle.
 * @param[in]     method      Method name (≤ 32 bytes).
 * @param[in]     req         Request payload.  May be NULL when
 *                            @c req_len == 0.
 * @param[in]     req_len     Request payload length.
 * @param[out]    resp        Buffer to receive the response payload.
 *                            May be NULL when the caller doesn't
 *                            care about the response body.
 * @param[in,out] resp_len    On entry: @p resp buffer capacity.
 *                            On successful return: actual response
 *                            length.  May be NULL when @p resp is
 *                            also NULL.
 * @param[in]     timeout_ms  Max wait in milliseconds.  Use
 *                            @c UINT32_MAX for unbounded wait.
 * @return  - @ref ALP_OK          on success
 *          - @ref ALP_ERR_NOT_READY @c ch is NULL or closed
 *          - @ref ALP_ERR_INVAL   @c method invalid, or
 *                                  @c resp != NULL with
 *                                  @c resp_len == NULL
 *          - @ref ALP_ERR_TIMEOUT no response within @p timeout_ms
 *          - @ref ALP_ERR_NOMEM   response too large for the
 *                                  caller-supplied @p resp buffer
 *          - @ref ALP_ERR_IO      OpenAMP backend returned an error
 *          - @ref ALP_ERR_NOSUPPORT backend doesn't implement
 *                                    synchronous call on this OS yet
 *                                    (Linux side ships partial in
 *                                    v0.6 -- see src/yocto/rpc_yocto.c)
 *
 * @note Concurrent calls on the same channel from multiple threads
 *       are serialised by the SDK; the second caller blocks until
 *       the first call returns or times out.
 */
alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_RPC_H */
