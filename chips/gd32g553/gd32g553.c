/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side driver for the GD32G553 supervisor MCU bridge on V2N.
 *
 * The wire protocol is in docs/gd32-bridge-protocol.md.  The firmware
 * counterpart that implements the same command-handler table lives
 * under firmware/gd32-bridge/.
 *
 * This file is deliberately Zephyr-agnostic: all bus access goes
 * through <alp/peripheral.h> so the same source compiles into either
 * the Zephyr backend or the future baremetal backend.  The chip
 * driver does not introduce any platform timing primitives -- the
 * tens-of-microseconds gap between alp_spi_write and alp_spi_read
 * naturally falls out of the host-side function-call overhead and
 * is documented as the firmware's reply-staging window in
 * docs/gd32-bridge-protocol.md §4.1.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/chips/gd32g553.h"

/* ----------------------------------------------------------------- */
/* CRC-16 / CCITT-FALSE  (poly 0x1021, init 0xFFFF, non-reflected,    */
/* xor-out 0x0000).  Reference vector: "123456789" -> 0x29B1.         */
/* Matches Zephyr's `crc16_itu_t(0xFFFF, ...)` and the matching       */
/* firmware/gd32-bridge/src/protocol.c implementation byte-for-byte.           */
/* ----------------------------------------------------------------- */

static uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)buf[i] << 8;
        for (unsigned b = 0; b < 8; ++b) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ----------------------------------------------------------------- */
/* Status-byte translation: wire encoding (unsigned) -> alp_status_t  */
/* (signed, negative-numbered).  See docs/gd32-bridge-protocol.md §6. */
/* ----------------------------------------------------------------- */

static alp_status_t status_from_wire(uint8_t s)
{
    switch (s) {
    case 0x00u:
        return ALP_OK;
    case 0x01u:
        return ALP_ERR_INVAL;
    case 0x02u:
        return ALP_ERR_NOT_READY;
    case 0x03u:
        return ALP_ERR_BUSY;
    case 0x04u:
        return ALP_ERR_TIMEOUT;
    case 0x05u:
        return ALP_ERR_IO;
    case 0x06u:
        return ALP_ERR_NOSUPPORT;
    case 0x07u:
        return ALP_ERR_NOMEM;
    case 0x08u:
        return ALP_ERR_OUT_OF_RANGE;
    case 0x80u: /* I2C: no pending command since last START */
        return ALP_ERR_NOT_READY;
    default:
        return ALP_ERR_IO;
    }
}

/* ----------------------------------------------------------------- */
/* Envelope sizes                                                     */
/* ----------------------------------------------------------------- */

/* Maximum wire-side payload either direction.  Driven by the largest
 * of (a) ADC_STREAM_READ which can reply up to
 * `1 + GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2` = 65 bytes (count
 * byte + u16 samples), and (b) ADC_DSP_STAGE_PUSH whose request
 * envelope carries a 7-byte chunk header plus up to
 * `GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES` = 58 bytes of stage
 * payload = 65 bytes total.  Matches the firmware-side
 * `GD32_BRIDGE_MAX_PAYLOAD_BYTES` so host + firmware serialise the
 * same wire envelopes; legacy small opcodes (ADC_READ at 17 bytes,
 * GET_BUILD_ID at 20 bytes, etc.) sit comfortably below. */
#define GD32G553_MAX_PAYLOAD_BYTES (1u + (GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2u))

/* SOF/CMD + payload + CRC for SPI; CMD + payload + CRC for I2C. */
#define GD32G553_MAX_SPI_FRAME_BYTES                                                               \
    (1u /* SOF */ + 1u /* CMD or STATUS */ + GD32G553_MAX_PAYLOAD_BYTES + 2u /* CRC */)

#define GD32G553_MAX_I2C_WRITE_BYTES                                                               \
    (1u /* reg=0x00 */ + 1u /* CMD */ + GD32G553_MAX_PAYLOAD_BYTES + 2u /* CRC */)

#define GD32G553_MAX_I2C_READ_BYTES (1u /* STATUS */ + GD32G553_MAX_PAYLOAD_BYTES + 2u /* CRC */)

/* ----------------------------------------------------------------- */
/* Transport: SPI                                                     */
/* ----------------------------------------------------------------- */

/* Reply re-read schedule.  The slave stages the reply inside the
 * REQUEST transaction's CS-rising handler; for slow handlers (ADC
 * conversion ~7 us/sample, TRNG conditioning, OTA FMC programming
 * ~70-140 us/chunk) the host's first reply read can land before the
 * reply is armed and clock idle/stale bytes instead.  Such a miss
 * does NOT consume the staged reply -- the slave's drain path rewinds
 * its staging cursor and re-arms the TX DMA on every CS cycle
 * (firmware >= v0.2.1; silicon-validated 2026-06-04) -- so re-READING
 * is always safe (no re-dispatch, no side effects).
 *
 * The schedule has two parts:
 *
 * 1. A fixed STAGING GAP before the first read.  The slave's CS-rising
 *    handler re-initialises its SPI peripheral (RCU flush), decodes,
 *    dispatches and re-arms both DMA channels before the reply exists
 *    -- a floor of a few tens of microseconds even for trivial
 *    handlers (bench 2026-06-04: at a ~15 us effective gap even
 *    GET_VERSION missed its first read every time, and each miss
 *    costs a wasted drain transaction plus a ladder wait, which is
 *    strictly worse than waiting out the floor).  35 us covers the
 *    rising path + trivial handlers; commands with real work fall
 *    through to the ladder.
 *
 * 2. A short-first backoff ladder between re-reads: cheap early
 *    retries keep ADC-burst-class handlers near the wire floor; the
 *    geometric tail covers OTA flash programming, bounding the total
 *    wait at ~3.2 ms. */
#define GD32G553_REPLY_STAGING_GAP_US 35u
static const uint16_t gd32g553_reply_retry_us[] = { 25u, 50u, 100u, 200u, 400u, 800u, 1600u };
#define GD32G553_REPLY_READ_TRIES                                                                  \
    (1u + (sizeof(gd32g553_reply_retry_us) / sizeof(gd32g553_reply_retry_us[0])))

/* Staging gap for one command.  ADC conversions run inside the request's
 * CS-rising handler at ~18-20 us per SAMPLE on top of the base rising
 * path (bench-bracketed 2026-06-04 from the miss boundary at two sample
 * counts and three gap sizes) -- too slow for the host to wait out in
 * full (chasing it with a sized-to-cover gap measured WORSE than letting
 * the ladder's first rung catch the tail, because the oversized gap is
 * paid even when conversions finish early).  The 8 us/sample partial
 * cover is the measured optimum: it keeps the first read out of the
 * conversion burst's COLLISION window (a read racing the burst can be
 * swallowed whole by coalesced CS edges, turning one miss into two:
 * 356 us avg vs 196 us) while the ladder absorbs the remainder.  The
 * real fix for ADC reply latency is slave-side (sampling-time config),
 * tracked for the next firmware rev. */
static uint32_t reply_staging_gap_us(uint8_t cmd, const uint8_t *req_payload,
                                     size_t req_payload_len)
{
    uint32_t gap = GD32G553_REPLY_STAGING_GAP_US;

    if (cmd == GD32G553_CMD_ADC_READ && req_payload != NULL && req_payload_len >= 2u) {
        gap += 8u * (uint32_t)req_payload[1];
    }
    return gap;
}

/* Forward declaration for the failed-command resync epilogue below. */
static alp_status_t spi_xfer(gd32g553_t *ctx, uint8_t cmd, const uint8_t *req_payload,
                             size_t req_payload_len, uint8_t *reply_payload,
                             size_t reply_payload_len);

/* Resync after a failed command.  The slave clears its staged reply
 * ONLY when it decodes a fresh request -- drains re-arm it (the
 * documented re-read semantics).  So when a command gives up (ladder
 * exhausted, or a short error envelope decoded), its stale staged
 * reply stays armed, and if the NEXT command's request is ever lost
 * to edge coalescing, that command reads the leftover -- at best a
 * CRC mismatch, at worst a perfectly valid error envelope attributed
 * to the WRONG command (observed 2026-06-04: GET_VERSION "returned"
 * a lingering TRNG BUSY).  One throwaway PING here forces a decode,
 * replacing the leftover with PING's reply and consuming it --
 * bounding staleness to the command that already failed.  Recursion
 * is depth-1 by construction: the epilogue never runs for PING. */
static void spi_resync_after_failure(gd32g553_t *ctx)
{
    (void)spi_xfer(ctx, 0x00u /* CMD_PING */, NULL, 0u, NULL, 0u);
}

static alp_status_t spi_xfer(gd32g553_t *ctx, uint8_t cmd, const uint8_t *req_payload,
                             size_t req_payload_len, uint8_t *reply_payload,
                             size_t reply_payload_len)
{
    if (ctx->spi == NULL) return ALP_ERR_NOSUPPORT;

    /* Request envelope: SOF | CMD | PAYLOAD | CRC(SOF..PAYLOAD) */
    uint8_t req[GD32G553_MAX_SPI_FRAME_BYTES];
    if (2u + req_payload_len + 2u > sizeof(req)) return ALP_ERR_INVAL;

    req[0] = GD32G553_BRIDGE_SOF;
    req[1] = cmd;
    if (req_payload_len > 0u && req_payload != NULL) {
        memcpy(&req[2], req_payload, req_payload_len);
    }
    const size_t   crc_covered = 2u + req_payload_len;
    const uint16_t crc         = crc16_ccitt_false(req, crc_covered);
    req[crc_covered]           = (uint8_t)(crc & 0xFFu);
    req[crc_covered + 1u]      = (uint8_t)((crc >> 8) & 0xFFu);

    /* v0.7 STATUS_SEQ outer loop: one RE-SEND when a CRC-valid reply
     * turns out to be STALE -- its [7:4] stamp never advanced past the
     * previous accepted reply, meaning the slave never decoded our
     * request and is re-serving the old staged reply (the residual
     * hazard documented in the firmware's transport_spi.c; byte-exact
     * replays silicon-fingerprinted 2026-06-06).  A stale verdict is
     * also proof the request was never EXECUTED, so the re-send is the
     * first execution -- safe even for non-idempotent opcodes.
     * Without the negotiated feature the loop body runs exactly once. */
    for (unsigned send_attempt = 0u;; ++send_attempt) {
        alp_status_t s = alp_spi_write(ctx->spi, req, crc_covered + 2u);
        if (s != ALP_OK) return s;

        /* Let the slave's CS-rising handler stage the reply before the first
         * read (see the schedule comment above) -- cheaper than eating a
         * wasted drain transaction + a ladder wait on every command. */
        alp_delay_us(reply_staging_gap_us(cmd, req_payload, req_payload_len));

        /* Reply envelope: SOF | STATUS | PAYLOAD | CRC(SOF..PAYLOAD).
         * Total bytes the host must clock = 1 + 1 + reply_payload_len + 2.
         * Read with the re-read schedule above: a too-early read is a
         * harmless miss, not a failure.
         *
         * STATUS byte on SPI = [7:4] sequence stamp (zero until the
         * v0.7 STATUS_SEQ feature is negotiated; old firmware never
         * sets these bits) | [3:0] status code -- masking the code is
         * unconditionally safe on this transport. */
        uint8_t      reply[GD32G553_MAX_SPI_FRAME_BYTES];
        const size_t reply_len = 2u + reply_payload_len + 2u;
        if (reply_len > sizeof(reply)) return ALP_ERR_INVAL;

        bool reply_ok = false;
        bool stale    = false;
        for (unsigned attempt = 0u; attempt < GD32G553_REPLY_READ_TRIES; ++attempt) {
            s = alp_spi_read(ctx->spi, reply, reply_len);
            if (s != ALP_OK) return s; /* transport-level error: fail loud */

            if (reply[0] == GD32G553_BRIDGE_SOF) {
                const uint16_t expect_crc = crc16_ccitt_false(reply, 2u + reply_payload_len);
                const uint16_t got_crc    = (uint16_t)reply[2u + reply_payload_len] |
                                         (uint16_t)reply[2u + reply_payload_len + 1u] << 8;
                if (got_crc == expect_crc) {
                    reply_ok = true;
                    break;
                }

                /* ERROR-ENVELOPE FALLBACK.  Firmware error replies carry NO
                 * payload (stage_error_reply: SOF | STATUS | CRC = 4 bytes)
                 * regardless of the opcode's success-reply width, so for any
                 * payload-bearing opcode the full-width CRC check above
                 * fails on a legitimate error reply -- the bytes past the
                 * 4-byte envelope are TX-FIFO idle filler.  Without this
                 * fallback every firmware error surfaced as ALP_ERR_IO,
                 * masking the real status (silicon 2026-06-04: an entire
                 * class of "-5 from cycle 1" HiL rows -- pwm_capture's
                 * documented NOSUPPORT among them -- were short error
                 * replies the host could not decode). */
                if (reply_payload_len > 0u &&
                    (reply[1] & GD32G553_STATUS_CODE_MASK) != 0x00u /* STATUS_OK */) {
                    const uint16_t err_crc = crc16_ccitt_false(reply, 2u);
                    const uint16_t err_got = (uint16_t)reply[2] | ((uint16_t)reply[3] << 8);
                    if (err_got == err_crc) {
                        if (ctx->seq_enabled) {
                            const uint8_t stamp = (uint8_t)(reply[1] >> 4);
                            if (stamp == ctx->seq_last) {
                                stale = true; /* stale ERROR reply: re-send */
                                break;
                            }
                            ctx->seq_last = stamp;
                        }
                        if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
                        return status_from_wire(reply[1] & GD32G553_STATUS_CODE_MASK);
                    }
                }
            }
            if (attempt + 1u < GD32G553_REPLY_READ_TRIES) {
                alp_delay_us(gd32g553_reply_retry_us[attempt]);
            }
        }
        if (!reply_ok && !stale) {
            if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
            return ALP_ERR_IO;
        }

        if (reply_ok) {
            const uint8_t code  = (uint8_t)(reply[1] & GD32G553_STATUS_CODE_MASK);
            const uint8_t stamp = (uint8_t)(reply[1] >> 4);

            if (cmd == GD32G553_CMD_LINK_FEATURES && code == 0x00u && reply_payload_len >= 1u) {
                /* Negotiation reply: arm/disarm host-side sequencing and
                 * take THIS reply's stamp as the baseline (the firmware
                 * stamps the negotiation reply itself once it grants). */
                ctx->seq_enabled = (reply[2] & GD32G553_LINK_FEAT_STATUS_SEQ) != 0u;
                ctx->seq_last    = stamp;
            } else if (ctx->seq_enabled) {
                if (stamp == ctx->seq_last) {
                    stale = true; /* slave never decoded the request */
                } else {
                    ctx->seq_last = stamp;
                }
            }

            if (!stale) {
                const alp_status_t firmware_status = status_from_wire(code);
                if (firmware_status != ALP_OK) return firmware_status;

                if (reply_payload_len > 0u && reply_payload != NULL) {
                    memcpy(reply_payload, &reply[2], reply_payload_len);
                }
                return ALP_OK;
            }
        }

        /* Stale reply detected.  One re-send: the hazard is a
         * single-transaction race, so the second attempt is expected
         * to land; a SECOND stale in a row means the link is wedged
         * beyond this mechanism -- resync + fail loud. */
        ctx->seq_stale_count++;
        if (send_attempt >= 1u) {
            if (cmd != 0x00u /* PING */) spi_resync_after_failure(ctx);
            return ALP_ERR_IO;
        }
    }
}

/* ----------------------------------------------------------------- */
/* Transport: I2C                                                     */
/* ----------------------------------------------------------------- */

static alp_status_t i2c_xfer(gd32g553_t *ctx, uint8_t cmd, const uint8_t *req_payload,
                             size_t req_payload_len, uint8_t *reply_payload,
                             size_t reply_payload_len)
{
    if (ctx->i2c == NULL) return ALP_ERR_NOSUPPORT;

    /* Write side: [reg=0x00][CMD][PAYLOAD][CRC(CMD..PAYLOAD) lo, hi] */
    uint8_t wbuf[GD32G553_MAX_I2C_WRITE_BYTES];
    if (1u + 1u + req_payload_len + 2u > sizeof(wbuf)) return ALP_ERR_INVAL;

    wbuf[0] = GD32G553_BRIDGE_I2C_REG_CMD; /* virtual command register */
    wbuf[1] = cmd;
    if (req_payload_len > 0u && req_payload != NULL) {
        memcpy(&wbuf[2], req_payload, req_payload_len);
    }
    /* CRC covers CMD | PAYLOAD (NOT the reg byte, NOT the I2C address). */
    const size_t   crc_covered      = 1u + req_payload_len;
    const uint16_t crc              = crc16_ccitt_false(&wbuf[1], crc_covered);
    wbuf[2u + req_payload_len]      = (uint8_t)(crc & 0xFFu);
    wbuf[2u + req_payload_len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);

    const size_t wlen               = 2u + req_payload_len + 2u;

    /* Read side: [STATUS][PAYLOAD][CRC(STATUS..PAYLOAD) lo, hi] */
    uint8_t      rbuf[GD32G553_MAX_I2C_READ_BYTES];
    const size_t rlen = 1u + reply_payload_len + 2u;
    if (rlen > sizeof(rbuf)) return ALP_ERR_INVAL;

    /* Combined write-then-(repeated-start)-read.  The firmware can
     * clock-stretch between phases to give itself time to process
     * the request; the bridge doc §5 documents this behaviour. */
    alp_status_t s = alp_i2c_write_read(ctx->i2c, ctx->i2c_addr, wbuf, wlen, rbuf, rlen);
    if (s != ALP_OK) return s;

    const uint16_t expect_crc = crc16_ccitt_false(rbuf, 1u + reply_payload_len);
    const uint16_t got_crc =
        (uint16_t)rbuf[1u + reply_payload_len] | (uint16_t)rbuf[1u + reply_payload_len + 1u] << 8;
    if (got_crc != expect_crc) {
        /* Error-envelope fallback -- same trap as the SPI side: firmware
         * error replies carry no payload ([STATUS][CRC] = 3 bytes on
         * I2C), so the full-width CRC fails on a legitimate error.
         * Decode the short shape before declaring transport failure. */
        if (reply_payload_len > 0u && rbuf[0] != 0x00u /* STATUS_OK */) {
            const uint16_t err_crc = crc16_ccitt_false(rbuf, 1u);
            const uint16_t err_got = (uint16_t)rbuf[1] | ((uint16_t)rbuf[2] << 8);
            if (err_got == err_crc) {
                return status_from_wire(rbuf[0]);
            }
        }
        return ALP_ERR_IO;
    }

    const alp_status_t firmware_status = status_from_wire(rbuf[0]);
    if (firmware_status != ALP_OK) return firmware_status;

    if (reply_payload_len > 0u && reply_payload != NULL) {
        memcpy(reply_payload, &rbuf[1], reply_payload_len);
    }
    return ALP_OK;
}

/* ----------------------------------------------------------------- */
/* Transport router                                                   */
/* ----------------------------------------------------------------- */

static alp_status_t cmd_send(gd32g553_t *ctx, gd32g553_transport_t t, uint8_t cmd,
                             const uint8_t *req_payload, size_t req_payload_len,
                             uint8_t *reply_payload, size_t reply_payload_len)
{
    if (t == GD32G553_TRANSPORT_DEFAULT) t = ctx->default_transport;
    switch (t) {
    case GD32G553_TRANSPORT_SPI:
        return spi_xfer(ctx, cmd, req_payload, req_payload_len, reply_payload, reply_payload_len);
    case GD32G553_TRANSPORT_I2C:
        return i2c_xfer(ctx, cmd, req_payload, req_payload_len, reply_payload, reply_payload_len);
    default:
        return ALP_ERR_INVAL;
    }
}

/* ----------------------------------------------------------------- */
/* Public API                                                          */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_init(gd32g553_t *ctx, alp_spi_t *spi, alp_i2c_t *i2c, uint8_t i2c_addr_7bit)
{
    if (ctx == NULL) return ALP_ERR_INVAL;
    if (spi == NULL && i2c == NULL) return ALP_ERR_INVAL;
    if (i2c != NULL && i2c_addr_7bit > 0x7Fu) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->spi               = spi;
    ctx->i2c               = i2c;
    ctx->i2c_addr          = i2c_addr_7bit;
    ctx->default_transport = (spi != NULL) ? GD32G553_TRANSPORT_SPI : GD32G553_TRANSPORT_I2C;
    ctx->initialised       = true;

    alp_status_t s         = gd32g553_ping(ctx);
    if (s != ALP_OK) {
        ctx->initialised = false;
        return s;
    }

    gd32g553_version_t v;
    s = gd32g553_get_version(ctx, &v);
    if (s != ALP_OK) {
        ctx->initialised = false;
        return s;
    }
    if (v.major != GD32G553_HOST_PROTOCOL_MAJOR) {
        ctx->initialised = false;
        return ALP_ERR_NOSUPPORT;
    }
    ctx->version = v;

    /* v0.7 link-feature negotiation (best-effort, SPI only -- the
     * STATUS_SEQ stamp is an SPI-framing feature).  Old firmware
     * answers STATUS_NOSUPPORT and the link stays on the legacy
     * framing; ctx->seq_enabled records the outcome (spi_xfer arms it
     * from the negotiation reply itself, which carries the stamp
     * baseline).  Any error here is deliberately non-fatal: the
     * feature is an integrity upgrade, not a liveness requirement. */
    if (ctx->spi != NULL) {
        uint8_t want    = GD32G553_LINK_FEAT_STATUS_SEQ;
        uint8_t granted = 0u;
        (void)cmd_send(ctx, GD32G553_TRANSPORT_SPI, GD32G553_CMD_LINK_FEATURES, &want, 1u, &granted,
                       1u);
    }
    return ALP_OK;
}

alp_status_t gd32g553_set_default_transport(gd32g553_t *ctx, gd32g553_transport_t t)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (t == GD32G553_TRANSPORT_SPI && ctx->spi == NULL) return ALP_ERR_NOSUPPORT;
    if (t == GD32G553_TRANSPORT_I2C && ctx->i2c == NULL) return ALP_ERR_NOSUPPORT;
    if (t != GD32G553_TRANSPORT_SPI && t != GD32G553_TRANSPORT_I2C) return ALP_ERR_INVAL;
    ctx->default_transport = t;
    return ALP_OK;
}

alp_status_t gd32g553_ping(gd32g553_t *ctx)
{
    if (ctx == NULL) return ALP_ERR_INVAL;
    /* Allow ping during init even before initialised==true: this is the
     * link-liveness probe gd32g553_init() runs.  The transport pointers
     * have already been set at that point. */
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PING, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_ping_via(gd32g553_t *ctx, gd32g553_transport_t t)
{
    if (ctx == NULL) return ALP_ERR_INVAL;
    return cmd_send(ctx, t, GD32G553_CMD_PING, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_get_version(gd32g553_t *ctx, gd32g553_version_t *out)
{
    if (ctx == NULL || out == NULL) return ALP_ERR_INVAL;
    uint8_t      reply[3];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GET_VERSION, NULL, 0u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    out->major = reply[0];
    out->minor = reply[1];
    out->patch = reply[2];
    return ALP_OK;
}

alp_status_t gd32g553_get_build_id(gd32g553_t *ctx, char build_id[GD32G553_BUILD_ID_LEN + 1])
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (build_id == NULL) return ALP_ERR_INVAL;
    uint8_t      reply[GD32G553_BUILD_ID_LEN];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GET_BUILD_ID, NULL, 0u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    memcpy(build_id, reply, GD32G553_BUILD_ID_LEN);
    build_id[GD32G553_BUILD_ID_LEN] = '\0';
    return ALP_OK;
}

alp_status_t gd32g553_get_reset_reason(gd32g553_t *ctx, gd32g553_reset_cause_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;
    uint8_t      reply;
    alp_status_t s =
        cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_RESET_REASON, NULL, 0u, &reply, 1u);
    if (s != ALP_OK) return s;
    *out = (gd32g553_reset_cause_t)reply;
    return ALP_OK;
}

/* Little-endian uint32 read / write helpers. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

alp_status_t gd32g553_gpio_read(gd32g553_t *ctx, uint32_t mask, uint32_t *levels)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (levels == NULL) return ALP_ERR_INVAL;
    uint8_t req[4];
    put_le32(req, mask);
    uint8_t      reply[4];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GPIO_READ, req,
                              sizeof(req), reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *levels = get_le32(reply);
    return ALP_OK;
}

alp_status_t gd32g553_gpio_write(gd32g553_t *ctx, uint32_t mask, uint32_t levels)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t req[8];
    put_le32(&req[0], mask);
    put_le32(&req[4], levels);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_GPIO_WRITE, req, sizeof(req),
                    NULL, 0u);
}

alp_status_t gd32g553_pwm_set(gd32g553_t *ctx, uint8_t channel, uint32_t period_ns,
                              uint32_t duty_ns)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (duty_ns > period_ns) return ALP_ERR_INVAL;
    uint8_t req[10];
    req[0] = channel;
    req[1] = 0u; /* reserved */
    put_le32(&req[2], period_ns);
    put_le32(&req[6], duty_ns);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_SET, req, sizeof(req), NULL,
                    0u);
}

alp_status_t gd32g553_pwm_get(gd32g553_t *ctx, uint8_t channel, uint32_t *period_ns,
                              uint32_t *duty_ns)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (period_ns == NULL || duty_ns == NULL) return ALP_ERR_INVAL;
    uint8_t      req = channel;
    uint8_t      reply[8];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_GET, &req, 1u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *period_ns = get_le32(&reply[0]);
    *duty_ns   = get_le32(&reply[4]);
    return ALP_OK;
}

alp_status_t gd32g553_adc_read(gd32g553_t *ctx, uint8_t channel, uint8_t samples, uint16_t *mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mv == NULL) return ALP_ERR_INVAL;
    if (samples == 0u || samples > GD32G553_BRIDGE_ADC_MAX_SAMPLES) {
        return ALP_ERR_INVAL;
    }

    uint8_t req[2] = { channel, samples };
    /* Reply payload echoes `samples` then carries 2 bytes per reading.
     * cmd_send needs the reply length known up-front; the host passes
     * the same `samples` it requested. */
    const size_t reply_payload_len = 1u + (size_t)samples * 2u;
    uint8_t      reply[1u + (GD32G553_BRIDGE_ADC_MAX_SAMPLES * 2u)];

    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_READ, req,
                              sizeof(req), reply, reply_payload_len);
    if (s != ALP_OK) return s;

    /* The firmware MUST echo the same samples value back at byte 0.
     * If it doesn't, the host treats the frame as corrupt -- the
     * CRC has already passed so this guards against a firmware bug,
     * not a wire glitch. */
    if (reply[0] != samples) return ALP_ERR_IO;

    for (uint8_t i = 0u; i < samples; ++i) {
        mv[i] = (uint16_t)reply[1u + i * 2u] | (uint16_t)reply[1u + i * 2u + 1u] << 8;
    }
    return ALP_OK;
}

alp_status_t gd32g553_da9292_status_forward(gd32g553_t *ctx, uint8_t *status)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (status == NULL) return ALP_ERR_INVAL;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DA9292_STATUS_FORWARD, NULL, 0u,
                    status, 1u);
}

alp_status_t gd32g553_dac_set(gd32g553_t *ctx, uint8_t channel, uint16_t value_mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (channel >= GD32G553_BRIDGE_DAC_CHANNELS) return ALP_ERR_INVAL;
    uint8_t req[4];
    req[0] = channel;
    req[1] = 0u; /* reserved */
    req[2] = (uint8_t)(value_mv & 0xFFu);
    req[3] = (uint8_t)((value_mv >> 8) & 0xFFu);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DAC_SET, req, sizeof(req), NULL,
                    0u);
}

alp_status_t gd32g553_dac_get(gd32g553_t *ctx, uint8_t channel, uint16_t *value_mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (value_mv == NULL) return ALP_ERR_INVAL;
    if (channel >= GD32G553_BRIDGE_DAC_CHANNELS) return ALP_ERR_INVAL;
    uint8_t      reply[2];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_DAC_GET, &channel, 1u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *value_mv = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
    return ALP_OK;
}

alp_status_t gd32g553_qenc_read(gd32g553_t *ctx, uint8_t encoder, int32_t *position_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (position_out == NULL) return ALP_ERR_INVAL;
    if (encoder >= GD32G553_BRIDGE_QENC_CHANNELS) return ALP_ERR_INVAL;
    uint8_t      reply[4];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_QENC_READ, &encoder, 1u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *position_out = (int32_t)get_le32(reply);
    return ALP_OK;
}

alp_status_t gd32g553_qenc_reset(gd32g553_t *ctx, uint8_t encoder)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (encoder >= GD32G553_BRIDGE_QENC_CHANNELS) return ALP_ERR_INVAL;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_QENC_RESET, &encoder, 1u, NULL,
                    0u);
}

alp_status_t gd32g553_counter_read(gd32g553_t *ctx, uint8_t counter, uint32_t *ticks_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (ticks_out == NULL) return ALP_ERR_INVAL;
    if (counter >= GD32G553_BRIDGE_COUNTER_CHANNELS) return ALP_ERR_INVAL;
    uint8_t      reply[4];
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_COUNTER_READ, &counter,
                              1u, reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *ticks_out = get_le32(reply);
    return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* v0.3 -- GD32G5 HW knobs                                            */
/* ------------------------------------------------------------------ */

alp_status_t gd32g553_pwm_configure(gd32g553_t *ctx, uint8_t channel,
                                    gd32g553_pwm_align_t align_mode, uint32_t dead_time_ns,
                                    uint8_t break_cfg)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if ((unsigned)align_mode > 3u) return ALP_ERR_INVAL;
    uint8_t req[7];
    req[0] = channel;
    req[1] = (uint8_t)align_mode;
    put_le32(&req[2], dead_time_ns);
    req[6] = break_cfg;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CONFIGURE, req, sizeof(req),
                    NULL, 0u);
}

alp_status_t gd32g553_adc_configure(gd32g553_t *ctx, uint8_t channel, uint16_t oversample_ratio,
                                    uint16_t sample_cycles, uint8_t resolution_bits)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    /* Resolution must be one of the supported widths (or 0 for
     * "firmware default") -- catch early so a typo doesn't have
     * to round-trip the wire. */
    switch (resolution_bits) {
    case 0u:
    case 6u:
    case 8u:
    case 10u:
    case 12u:
    case 14u:
    case 16u:
        break;
    default:
        return ALP_ERR_INVAL;
    }
    uint8_t req[7];
    req[0] = channel;
    req[1] = 0u; /* reserved */
    req[2] = (uint8_t)(oversample_ratio & 0xFFu);
    req[3] = (uint8_t)((oversample_ratio >> 8) & 0xFFu);
    req[4] = (uint8_t)(sample_cycles & 0xFFu);
    req[5] = (uint8_t)((sample_cycles >> 8) & 0xFFu);
    req[6] = resolution_bits;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_CONFIGURE, req, sizeof(req),
                    NULL, 0u);
}

alp_status_t gd32g553_adc_stream_begin(gd32g553_t *ctx, uint8_t stream_id, uint8_t channel,
                                       uint32_t sample_rate_hz)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
    if (sample_rate_hz == 0u) return ALP_ERR_INVAL;
    uint8_t req[7];
    req[0] = stream_id;
    req[1] = channel;
    req[2] = 0u; /* reserved */
    put_le32(&req[3], sample_rate_hz);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_STREAM_BEGIN, req,
                    sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_adc_stream_read(gd32g553_t *ctx, uint8_t stream_id, uint8_t max_samples,
                                      uint8_t *got_samples, uint16_t *mv)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (got_samples == NULL || mv == NULL) return ALP_ERR_INVAL;
    if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
    if (max_samples == 0u) return ALP_ERR_INVAL;
    if (max_samples > GD32G553_BRIDGE_ADC_STREAM_READ_MAX) {
        max_samples = GD32G553_BRIDGE_ADC_STREAM_READ_MAX;
    }

    /* cmd_send needs the reply length up-front; for streaming we
     * pre-commit to `1 + max_samples * 2` reply bytes regardless of
     * how many samples the firmware actually has ready -- byte 0
     * carries the real count `got`, and slots past `got` are
     * zero-padded by the firmware so the on-wire envelope length
     * stays deterministic. */
    uint8_t       reply[1u + (GD32G553_BRIDGE_ADC_STREAM_READ_MAX * 2u)];
    const size_t  reply_len = 1u + ((size_t)max_samples * 2u);
    const uint8_t req[2]    = { stream_id, max_samples };
    alp_status_t  s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_STREAM_READ, req,
                               sizeof(req), reply, reply_len);
    if (s != ALP_OK) {
        *got_samples = 0u;
        return s;
    }
    const uint8_t got = reply[0];
    if (got > max_samples) return ALP_ERR_IO; /* firmware contract violation */
    *got_samples = got;
    for (uint8_t i = 0u; i < got; ++i) {
        mv[i] = (uint16_t)reply[1u + i * 2u] | ((uint16_t)reply[1u + i * 2u + 1u] << 8);
    }
    return ALP_OK;
}

alp_status_t gd32g553_adc_stream_end(gd32g553_t *ctx, uint8_t stream_id)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_STREAM_END, &stream_id, 1u,
                    NULL, 0u);
}

alp_status_t gd32g553_trng_read(gd32g553_t *ctx, uint8_t *dest, size_t len)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (dest == NULL) return ALP_ERR_INVAL;
    if (len == 0u || len > GD32G553_BRIDGE_TRNG_MAX_BYTES) return ALP_ERR_INVAL;
    const uint8_t req = (uint8_t)len;

    /* A max-length pull spans a whole 256-bit NIST conditioning round,
     * so the firmware legitimately answers STATUS_BUSY when its FIFO
     * runs dry mid-request (it bounds its in-handler wait instead of
     * overrunning the reply window).  Ride out a few rounds here --
     * conditioning completes in low milliseconds and randomness is
     * not a latency-critical surface. */
    alp_status_t s = ALP_ERR_BUSY;
    for (unsigned attempt = 0u; attempt < 4u && s == ALP_ERR_BUSY; ++attempt) {
        if (attempt != 0u) alp_delay_us(2000u);
        s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_TRNG_READ, &req, 1u, dest, len);
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* v0.4 -- GD32G5 TMU (CORDIC) math accelerator                        */
/* ------------------------------------------------------------------ */

alp_status_t gd32g553_tmu_compute(gd32g553_t *ctx, gd32g553_tmu_function_t function,
                                  gd32g553_tmu_format_t format, uint32_t in_a, uint32_t in_b,
                                  uint32_t *result_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (result_out == NULL) return ALP_ERR_INVAL;
    /* Range-check the function + format enums host-side so callers
     * find typos at the API boundary rather than after a wire trip. */
    if ((unsigned)function > (unsigned)GD32G553_TMU_FN_HYPOT) return ALP_ERR_INVAL;
    if ((unsigned)format > (unsigned)GD32G553_TMU_FMT_F32) return ALP_ERR_INVAL;

    uint8_t req[12];
    req[0] = (uint8_t)function;
    req[1] = (uint8_t)format;
    req[2] = 0u; /* reserved */
    req[3] = 0u; /* reserved */
    put_le32(&req[4], in_a);
    put_le32(&req[8], in_b);

    uint8_t      reply[4] = { 0 };
    alp_status_t s        = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_TMU_COMPUTE, req,
                                     sizeof(req), reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *result_out = get_le32(reply);
    return ALP_OK;
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B.2 + §2B.3) -- advanced timer extras + power-mode set    */
/*                                                                    */
/* Wire frames per docs/gd32-bridge-protocol.md.  Firmware returns   */
/* STATUS_NOSUPPORT today via the default-case dispatch until the    */
/* corresponding bridge_hw_* HAL bodies land; the host helpers below */
/* return ALP_ERR_NOSUPPORT in lockstep.                              */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_pwm_capture_begin(gd32g553_t *ctx, uint8_t channel, uint8_t edge)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (edge > 2u) return ALP_ERR_INVAL;
    uint8_t req[2] = { channel, edge };
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CAPTURE_BEGIN, req,
                    sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_pwm_capture_read(gd32g553_t *ctx, uint8_t channel, uint32_t *period_ns,
                                       uint32_t *pulse_ns)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (period_ns == NULL && pulse_ns == NULL) return ALP_ERR_INVAL;
    uint8_t      req[1]   = { channel };
    uint8_t      reply[8] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CAPTURE_READ, req,
                              sizeof(req), reply, sizeof(reply));
    if (s != ALP_OK) return s;
    if (period_ns != NULL) *period_ns = get_le32(&reply[0]);
    if (pulse_ns != NULL) *pulse_ns = get_le32(&reply[4]);
    return ALP_OK;
}

alp_status_t gd32g553_pwm_capture_end(gd32g553_t *ctx, uint8_t channel)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t req[1] = { channel };
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_CAPTURE_END, req, sizeof(req),
                    NULL, 0u);
}

alp_status_t gd32g553_pwm_single_pulse(gd32g553_t *ctx, uint8_t channel, uint32_t pulse_ns)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (pulse_ns == 0u) return ALP_ERR_INVAL;
    uint8_t req[8];
    req[0] = channel;
    req[1] = 0u; /* reserved */
    req[2] = 0u;
    req[3] = 0u;
    put_le32(&req[4], pulse_ns);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_PWM_SINGLE_PULSE, req,
                    sizeof(req), NULL, 0u);
}

alp_status_t gd32g553_timer_sync(gd32g553_t *ctx, uint8_t master, uint8_t slave, uint8_t mode)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    uint8_t req[3] = { master, slave, mode };
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_TIMER_SYNC, req, sizeof(req),
                    NULL, 0u);
}

alp_status_t gd32g553_power_mode_set(gd32g553_t *ctx, uint8_t mode, uint32_t wake_bitmap,
                                     uint32_t wake_after_ms)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (mode > 3u) return ALP_ERR_INVAL;
    uint8_t req[10];
    req[0] = mode;
    req[1] = 0u; /* reserved */
    put_le32(&req[2], wake_bitmap);
    put_le32(&req[6], wake_after_ms);
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_POWER_MODE_SET, req, sizeof(req),
                    NULL, 0u);
}

/* ----------------------------------------------------------------- */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                     */
/*                                                                    */
/* Wire frames per docs/gd32-bridge-protocol.md §3.x.  Firmware       */
/* returns STATUS_NOSUPPORT today via the default-case dispatch       */
/* until the bridge_hw_adc_dsp_* HAL bodies land; the host helpers    */
/* below short-circuit to ALP_ERR_NOSUPPORT in lockstep with that     */
/* contract -- the wire request still serialises correctly so the    */
/* firmware-side handler tree sees real envelopes when it ships.     */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_adc_dsp_chain_open(gd32g553_t *ctx, uint8_t *chain_id_out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (chain_id_out == NULL) return ALP_ERR_INVAL;

    uint8_t      reply[1] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_DSP_CHAIN_OPEN,
                              NULL, 0u, reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *chain_id_out = reply[0];
    return ALP_OK;
}

alp_status_t gd32g553_adc_dsp_stage_push(gd32g553_t *ctx, uint8_t chain_id, uint8_t stage_index,
                                         uint8_t kind, const uint8_t *stage_params,
                                         uint16_t stage_params_len)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (stage_index >= GD32G553_BRIDGE_ADC_DSP_MAX_STAGES) return ALP_ERR_INVAL;
    if (kind > 3u) return ALP_ERR_INVAL;
    if (stage_params == NULL && stage_params_len != 0u) return ALP_ERR_INVAL;
    if (stage_params_len > GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES) return ALP_ERR_OUT_OF_RANGE;

    /* Chunk the per-kind blob into wire envelopes no larger than
     * MAX_CHUNK_BYTES.  Empty blobs (stage_params_len == 0) still
     * send a single zero-length STAGE_PUSH so the firmware sees the
     * kind declaration arrive at the requested stage_index.  The
     * 7-byte header is chain_id + stage_index + kind + chunk_offset:u16
     * + chunk_total_size:u16; chunk_total_size carries the FULL
     * per-kind blob length (not this chunk's length) so the firmware
     * knows when assembly is complete. */
    uint16_t offset = 0u;
    do {
        uint16_t this_chunk = (uint16_t)(stage_params_len - offset);
        if (this_chunk > GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES) {
            this_chunk = GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES;
        }
        uint8_t req[7u + GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES];
        req[0] = chain_id;
        req[1] = stage_index;
        req[2] = kind;
        req[3] = (uint8_t)(offset & 0xFFu);
        req[4] = (uint8_t)((offset >> 8) & 0xFFu);
        req[5] = (uint8_t)(stage_params_len & 0xFFu);
        req[6] = (uint8_t)((stage_params_len >> 8) & 0xFFu);
        if (this_chunk > 0u) {
            memcpy(&req[7], stage_params + offset, this_chunk);
        }
        alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_DSP_STAGE_PUSH,
                                  req, (size_t)(7u + this_chunk), NULL, 0u);
        if (s != ALP_OK) return s;
        offset = (uint16_t)(offset + this_chunk);
    } while (offset < stage_params_len);

    return ALP_OK;
}

alp_status_t gd32g553_adc_dsp_chain_bind(gd32g553_t *ctx, uint8_t chain_id, uint8_t stream_id)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (stream_id >= GD32G553_BRIDGE_ADC_STREAM_COUNT) return ALP_ERR_INVAL;
    uint8_t req[2] = { chain_id, stream_id };
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_ADC_DSP_CHAIN_BIND, req,
                    sizeof(req), NULL, 0u);
}

/* ----------------------------------------------------------------- */
/* OTA helpers -- the firmware-side opcodes return STATUS_NOSUPPORT  */
/* against the scaffold today, which maps to ALP_ERR_NOSUPPORT here. */
/* When the bridge ships real bodies the same call paths return the  */
/* documented payloads.  GET_STATE answers concretely against the    */
/* scaffold so customer telemetry already works.                      */
/* ----------------------------------------------------------------- */

alp_status_t gd32g553_ota_begin(gd32g553_t *ctx, uint32_t size_bytes, uint32_t expected_crc32,
                                const gd32g553_version_t *fw_version, uint16_t *chunk_max_bytes,
                                gd32g553_ota_slot_t *target_slot)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (chunk_max_bytes == NULL || target_slot == NULL) return ALP_ERR_INVAL;
    if (size_bytes == 0u) return ALP_ERR_INVAL;

    /* v0.7 additive form: size:u32, crc:u32 [, maj:u8, min:u8, pat:u8].
     * The version triple lands in the bridge's A/B metadata record at
     * COMMIT (fw_version[slot], "0 = unknown").  NULL = send the legacy
     * 8-byte form; older firmware ignores the 3 trailing bytes of the
     * long form, so either pairing degrades gracefully. */
    uint8_t      req[11];
    const size_t req_len = (fw_version != NULL) ? 11u : 8u;
    put_le32(&req[0], size_bytes);
    put_le32(&req[4], expected_crc32);
    if (fw_version != NULL) {
        req[8]  = fw_version->major;
        req[9]  = fw_version->minor;
        req[10] = fw_version->patch;
    }

    /* Reply: chunk_max_bytes:u16 + target_slot:u8. */
    uint8_t      reply[3] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_BEGIN, req, req_len,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *chunk_max_bytes = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
    *target_slot     = (gd32g553_ota_slot_t)reply[2];
    return ALP_OK;
}

alp_status_t gd32g553_ota_write_chunk(gd32g553_t *ctx, uint32_t offset, const uint8_t *data,
                                      size_t data_len, uint32_t *received_bytes)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (data == NULL || data_len == 0u) return ALP_ERR_INVAL;
    if (received_bytes == NULL) return ALP_ERR_INVAL;

    /* Chunk size is bounded by the wire payload ceiling.  Bridges
     * that advertise a smaller chunk_max_bytes in BEGIN are honoured
     * by the caller -- this helper only enforces the absolute limit.
     * (v0.6: the payload carries an explicit len byte after the
     * offset, so the data ceiling is MAX_PAYLOAD - 5.) */
    if (data_len > GD32G553_MAX_PAYLOAD_BYTES - 5u) return ALP_ERR_INVAL;

    uint8_t req[GD32G553_MAX_PAYLOAD_BYTES] = { 0 };
    put_le32(&req[0], offset);
    req[4] = (uint8_t)data_len; /* v0.6 anti-extension cross-check */
    memcpy(&req[5], data, data_len);

    uint8_t      reply[4] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_WRITE_CHUNK, req,
                              5u + data_len, reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *received_bytes = get_le32(reply);
    return ALP_OK;
}

alp_status_t gd32g553_ota_verify(gd32g553_t *ctx, bool *verified, uint32_t *computed_crc32)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (verified == NULL || computed_crc32 == NULL) return ALP_ERR_INVAL;

    /* Reply: computed_crc32(u32) + verified(u8). */
    uint8_t      reply[5] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_VERIFY, NULL, 0u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    *computed_crc32 = get_le32(&reply[0]);
    *verified       = (reply[4] != 0u);
    return ALP_OK;
}

alp_status_t gd32g553_ota_commit(gd32g553_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_COMMIT, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_ota_rollback(gd32g553_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_ROLLBACK, NULL, 0u, NULL, 0u);
}

alp_status_t gd32g553_ota_get_state(gd32g553_t *ctx, gd32g553_ota_state_info_t *out)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (out == NULL) return ALP_ERR_INVAL;

    /* Reply: state(u8) + active(u8) + pending(u8) + boot_count(u16 LE). */
    uint8_t      reply[5] = { 0 };
    alp_status_t s = cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_GET_STATE, NULL, 0u,
                              reply, sizeof(reply));
    if (s != ALP_OK) return s;
    out->state        = (gd32g553_ota_state_t)reply[0];
    out->active_slot  = (gd32g553_ota_slot_t)reply[1];
    out->pending_slot = (gd32g553_ota_slot_t)reply[2];
    out->boot_count   = (uint16_t)reply[3] | ((uint16_t)reply[4] << 8);
    return ALP_OK;
}

alp_status_t gd32g553_ota_abort(gd32g553_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    return cmd_send(ctx, GD32G553_TRANSPORT_DEFAULT, GD32G553_CMD_OTA_ABORT, NULL, 0u, NULL, 0u);
}

void gd32g553_deinit(gd32g553_t *ctx)
{
    if (ctx == NULL) return;
    /* Bus handles are owned by the caller -- don't close them. */
    ctx->initialised = false;
    ctx->spi         = NULL;
    ctx->i2c         = NULL;
}
