/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware: shared command-handler table.
 *
 * Per the project memory rule "don't fork the protocol -- one framing
 * format, one command set, one set of reply codes; only the transport
 * layer differs", both the SPI and I2C transports call into this
 * file's protocol_dispatch().
 *
 * Handlers that need actual hardware (PWM channel programming, ADC
 * sampling, GPIO output, DA9292 I2C poll) currently call into the
 * `bridge_hw_*` HAL shims declared in hal/.  The HAL is a separate
 * compile unit and will be implemented against the GigaDevice
 * firmware library in a follow-up commit.  Today those shims return
 * STATUS_NOSUPPORT so the protocol round-trip is exercisable
 * end-to-end (PING + GET_VERSION + GET_BUILD_ID work without any
 * peripheral I/O).
 */

#include <string.h>

#include "protocol.h"
#include "../hal/bridge_hw.h"
#include "bootloader/bootloader.h"

/* --------------------------------------------------------------- */
/* CRC-16 / CCITT-FALSE -- shared with transports.                   */
/* --------------------------------------------------------------- */

uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len)
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

/* --------------------------------------------------------------- */
/* Build identifier.  Linker-emitted, ASCII hex truncated SHA-1 of   */
/* the elf.  Until the build pipeline lands, use a placeholder so    */
/* GET_BUILD_ID has a well-formed reply.                              */
/* --------------------------------------------------------------- */

#ifndef GD32_BRIDGE_BUILD_ID
#define GD32_BRIDGE_BUILD_ID "0000000000000000abcd"
#endif

_Static_assert(sizeof(GD32_BRIDGE_BUILD_ID) - 1u == GD32_BRIDGE_BUILD_ID_LEN,
               "build id must be 20 ASCII bytes");

/* --------------------------------------------------------------- */
/* LE-int helpers (firmware side, parallel to the host driver).      */
/* --------------------------------------------------------------- */

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* --------------------------------------------------------------- */
/* Per-opcode handlers                                                */
/* --------------------------------------------------------------- */

static gd32_bridge_status_t handle_ping(const uint8_t *req, size_t req_len,
                                        uint8_t *reply, size_t reply_cap,
                                        size_t *reply_len)
{
    (void)req; (void)reply; (void)reply_cap;
    if (req_len != 0u) return STATUS_INVAL;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_get_version(const uint8_t *req, size_t req_len,
                                               uint8_t *reply, size_t reply_cap,
                                               size_t *reply_len)
{
    (void)req;
    if (req_len != 0u) return STATUS_INVAL;
    if (reply_cap < 3u) return STATUS_NOMEM;
    reply[0]   = PROTOCOL_VERSION_MAJOR;
    reply[1]   = PROTOCOL_VERSION_MINOR;
    reply[2]   = PROTOCOL_VERSION_PATCH;
    *reply_len = 3u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_get_build_id(const uint8_t *req, size_t req_len,
                                                uint8_t *reply, size_t reply_cap,
                                                size_t *reply_len)
{
    (void)req;
    if (req_len != 0u) return STATUS_INVAL;
    if (reply_cap < GD32_BRIDGE_BUILD_ID_LEN) return STATUS_NOMEM;
    memcpy(reply, GD32_BRIDGE_BUILD_ID, GD32_BRIDGE_BUILD_ID_LEN);
    *reply_len = GD32_BRIDGE_BUILD_ID_LEN;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_reset_reason(const uint8_t *req, size_t req_len,
                                                uint8_t *reply, size_t reply_cap,
                                                size_t *reply_len)
{
    (void)req;
    if (req_len != 0u) return STATUS_INVAL;
    if (reply_cap < 1u) return STATUS_NOMEM;
    /* bridge_hw_reset_reason() also clears the cause on read so the
     * next caller sees UNKNOWN if no further resets have happened. */
    reply[0]   = bridge_hw_reset_reason();
    *reply_len = 1u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_gpio_read(const uint8_t *req, size_t req_len,
                                             uint8_t *reply, size_t reply_cap,
                                             size_t *reply_len)
{
    if (req_len != 4u) return STATUS_INVAL;
    if (reply_cap < 4u) return STATUS_NOMEM;
    const uint32_t mask    = get_le32(req);
    uint32_t       levels  = 0u;
    const int      rv      = bridge_hw_gpio_read(mask, &levels);
    if (rv < 0) return STATUS_IO;
    put_le32(reply, levels);
    *reply_len = 4u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_gpio_write(const uint8_t *req, size_t req_len,
                                              uint8_t *reply, size_t reply_cap,
                                              size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 8u) return STATUS_INVAL;
    const uint32_t mask   = get_le32(&req[0]);
    const uint32_t levels = get_le32(&req[4]);
    const int      rv     = bridge_hw_gpio_write(mask, levels);
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_pwm_set(const uint8_t *req, size_t req_len,
                                           uint8_t *reply, size_t reply_cap,
                                           size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 10u) return STATUS_INVAL;
    const uint8_t  channel   = req[0];
    /* req[1] is the reserved padding byte; ignore */
    const uint32_t period_ns = get_le32(&req[2]);
    const uint32_t duty_ns   = get_le32(&req[6]);
    if (duty_ns > period_ns) return STATUS_INVAL;
    const int rv = bridge_hw_pwm_set(channel, period_ns, duty_ns);
    if (rv == BRIDGE_HW_ERR_RANGE) return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_pwm_get(const uint8_t *req, size_t req_len,
                                           uint8_t *reply, size_t reply_cap,
                                           size_t *reply_len)
{
    if (req_len != 1u) return STATUS_INVAL;
    if (reply_cap < 8u) return STATUS_NOMEM;
    uint32_t period_ns = 0u;
    uint32_t duty_ns   = 0u;
    const int rv = bridge_hw_pwm_get(req[0], &period_ns, &duty_ns);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv < 0) return STATUS_IO;
    put_le32(&reply[0], period_ns);
    put_le32(&reply[4], duty_ns);
    *reply_len = 8u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_adc_read(const uint8_t *req, size_t req_len,
                                            uint8_t *reply, size_t reply_cap,
                                            size_t *reply_len)
{
    if (req_len != 2u) return STATUS_INVAL;
    uint8_t channel = req[0];
    uint8_t samples = req[1];
    if (samples == 0u) return STATUS_INVAL;
    /* Reject out-of-range sample counts outright rather than silently
     * capping -- the host driver compares `reply[0]` against the
     * originally-requested count and treats a mismatch as a wire
     * error, so silent capping would turn a documented OUT_OF_RANGE
     * caller error into a misleading ALP_ERR_IO. */
    if (samples > GD32_BRIDGE_ADC_MAX_SAMPLES) return STATUS_OUT_OF_RANGE;

    const size_t need = 1u + (size_t)samples * 2u;
    if (reply_cap < need) return STATUS_NOMEM;

    reply[0] = samples; /* echoes back the (validated) value */
    uint16_t mv[GD32_BRIDGE_ADC_MAX_SAMPLES];
    const int rv = bridge_hw_adc_read(channel, samples, mv);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv < 0) return STATUS_IO;
    for (uint8_t i = 0u; i < samples; ++i) {
        reply[1u + i * 2u]      = (uint8_t)(mv[i] & 0xFFu);
        reply[1u + i * 2u + 1u] = (uint8_t)((mv[i] >> 8) & 0xFFu);
    }
    *reply_len = need;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_da9292_forward(const uint8_t *req, size_t req_len,
                                                  uint8_t *reply, size_t reply_cap,
                                                  size_t *reply_len)
{
    (void)req;
    if (req_len != 0u) return STATUS_INVAL;
    if (reply_cap < 1u) return STATUS_NOMEM;
    reply[0]   = bridge_hw_da9292_status_cached();
    *reply_len = 1u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_dac_set(const uint8_t *req, size_t req_len,
                                           uint8_t *reply, size_t reply_cap,
                                           size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 4u) return STATUS_INVAL;
    const uint8_t  channel  = req[0];
    /* req[1] reserved padding -- ignore */
    const uint16_t value_mv = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
    const int rv = bridge_hw_dac_set(channel, value_mv);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_RANGE) return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_dac_get(const uint8_t *req, size_t req_len,
                                           uint8_t *reply, size_t reply_cap,
                                           size_t *reply_len)
{
    if (req_len != 1u) return STATUS_INVAL;
    if (reply_cap < 2u) return STATUS_NOMEM;
    uint16_t value_mv = 0u;
    const int rv = bridge_hw_dac_get(req[0], &value_mv);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    reply[0] = (uint8_t)(value_mv & 0xFFu);
    reply[1] = (uint8_t)((value_mv >> 8) & 0xFFu);
    *reply_len = 2u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_qenc_read(const uint8_t *req, size_t req_len,
                                             uint8_t *reply, size_t reply_cap,
                                             size_t *reply_len)
{
    if (req_len != 1u) return STATUS_INVAL;
    if (reply_cap < 4u) return STATUS_NOMEM;
    int32_t position = 0;
    const int rv = bridge_hw_qenc_read(req[0], &position);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    put_le32(reply, (uint32_t)position);
    *reply_len = 4u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_qenc_reset(const uint8_t *req, size_t req_len,
                                              uint8_t *reply, size_t reply_cap,
                                              size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 1u) return STATUS_INVAL;
    const int rv = bridge_hw_qenc_reset(req[0]);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_counter_read(const uint8_t *req, size_t req_len,
                                                uint8_t *reply, size_t reply_cap,
                                                size_t *reply_len)
{
    if (req_len != 1u) return STATUS_INVAL;
    if (reply_cap < 4u) return STATUS_NOMEM;
    uint32_t ticks = 0u;
    const int rv = bridge_hw_counter_read(req[0], &ticks);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    put_le32(reply, ticks);
    *reply_len = 4u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_pwm_configure(const uint8_t *req, size_t req_len,
                                                 uint8_t *reply, size_t reply_cap,
                                                 size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 7u) return STATUS_INVAL;
    const uint8_t  channel       = req[0];
    const uint8_t  align_mode    = req[1];
    const uint32_t dead_time_ns  = get_le32(&req[2]);
    const uint8_t  break_cfg     = req[6];
    if (align_mode > 3u) return STATUS_INVAL;
    const int rv = bridge_hw_pwm_configure(channel, align_mode,
                                           dead_time_ns, break_cfg);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_RANGE) return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_adc_configure(const uint8_t *req, size_t req_len,
                                                 uint8_t *reply, size_t reply_cap,
                                                 size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 7u) return STATUS_INVAL;
    const uint8_t  channel           = req[0];
    /* req[1] reserved padding */
    const uint16_t oversample_ratio  = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
    const uint16_t sample_cycles     = (uint16_t)req[4] | ((uint16_t)req[5] << 8);
    const uint8_t  resolution_bits   = req[6];
    /* Resolution is one of 6/8/10/12/14/16 per the GD32G5
     * datasheet's effective-resolution table; the firmware rejects
     * other values rather than silently rounding so callers find
     * out at protocol time. */
    switch (resolution_bits) {
    case 0u:  /* "use the firmware default" */
    case 6u: case 8u: case 10u: case 12u: case 14u: case 16u:
        break;
    default:
        return STATUS_INVAL;
    }
    const int rv = bridge_hw_adc_configure(channel, oversample_ratio,
                                           sample_cycles, resolution_bits);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_RANGE) return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_adc_stream_begin(const uint8_t *req, size_t req_len,
                                                    uint8_t *reply, size_t reply_cap,
                                                    size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 7u) return STATUS_INVAL;
    const uint8_t  stream_id      = req[0];
    const uint8_t  channel        = req[1];
    /* req[2] reserved padding */
    const uint32_t sample_rate_hz = get_le32(&req[3]);
    if (stream_id >= GD32_BRIDGE_ADC_STREAM_COUNT) return STATUS_INVAL;
    if (sample_rate_hz == 0u) return STATUS_INVAL;
    const int rv = bridge_hw_adc_stream_begin(stream_id, channel, sample_rate_hz);
    if (rv == BRIDGE_HW_ERR_INVAL) return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_RANGE) return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_adc_stream_read(const uint8_t *req, size_t req_len,
                                                   uint8_t *reply, size_t reply_cap,
                                                   size_t *reply_len)
{
    if (req_len != 2u) return STATUS_INVAL;
    const uint8_t stream_id   = req[0];
    uint8_t       max_samples = req[1];
    if (stream_id >= GD32_BRIDGE_ADC_STREAM_COUNT) return STATUS_INVAL;
    /* The wire-framing layer needs a fixed reply length so the host
     * can clock the right number of bytes.  Reply is always
     * `1 + max_samples*2` bytes: byte 0 = `got` (the valid sample
     * count, 0..max_samples); subsequent bytes are the sample data,
     * with trailing slots zero-padded when the firmware ring had
     * fewer than `max_samples` samples ready. */
    if (max_samples == 0u) return STATUS_INVAL;
    if (max_samples > GD32_BRIDGE_ADC_STREAM_READ_MAX) {
        return STATUS_OUT_OF_RANGE;
    }

    /* Reply scratch on the firmware stack -- avoid pulling in the
     * full GD32_BRIDGE_MAX_PAYLOAD_BYTES buffer.  Sized to the
     * stream-read ceiling. */
    uint16_t mv[GD32_BRIDGE_ADC_STREAM_READ_MAX];
    uint8_t  got = 0u;
    const int rv = bridge_hw_adc_stream_read(stream_id, max_samples, &got, mv);
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    if (got > max_samples) return STATUS_IO;  /* HAL contract violation */

    const size_t need = 1u + (size_t)max_samples * 2u;
    if (reply_cap < need) return STATUS_NOMEM;
    reply[0] = got;
    for (uint8_t i = 0u; i < got; ++i) {
        reply[1u + i * 2u]      = (uint8_t)(mv[i] & 0xFFu);
        reply[1u + i * 2u + 1u] = (uint8_t)((mv[i] >> 8) & 0xFFu);
    }
    /* Pad with zeros so the on-wire envelope length is deterministic
     * (host pre-committed to clocking 1 + max_samples*2 reply bytes
     * + CRC).  Filler bytes are zero so the CRC is reproducible. */
    for (uint8_t i = got; i < max_samples; ++i) {
        reply[1u + i * 2u]      = 0u;
        reply[1u + i * 2u + 1u] = 0u;
    }
    *reply_len = need;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_adc_stream_end(const uint8_t *req, size_t req_len,
                                                  uint8_t *reply, size_t reply_cap,
                                                  size_t *reply_len)
{
    (void)reply; (void)reply_cap;
    if (req_len != 1u) return STATUS_INVAL;
    const uint8_t stream_id = req[0];
    if (stream_id >= GD32_BRIDGE_ADC_STREAM_COUNT) return STATUS_INVAL;
    const int rv = bridge_hw_adc_stream_end(stream_id);
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = 0u;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_trng_read(const uint8_t *req, size_t req_len,
                                             uint8_t *reply, size_t reply_cap,
                                             size_t *reply_len)
{
    if (req_len != 1u) return STATUS_INVAL;
    const uint8_t want = req[0];
    if (want == 0u || want > 32u) return STATUS_INVAL;
    if (reply_cap < want) return STATUS_NOMEM;
    const int rv = bridge_hw_trng_read(reply, (size_t)want);
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0) return STATUS_IO;
    *reply_len = want;
    return STATUS_OK;
}

static gd32_bridge_status_t handle_tmu_compute(const uint8_t *req, size_t req_len,
                                               uint8_t *reply, size_t reply_cap,
                                               size_t *reply_len)
{
    if (req_len != 12u) return STATUS_INVAL;
    if (reply_cap < 4u) return STATUS_NOMEM;
    const uint8_t  function = req[0];
    const uint8_t  format   = req[1];
    /* req[2..3] are the reserved padding -- ignore. */
    const uint32_t in_a     = get_le32(&req[4]);
    const uint32_t in_b     = get_le32(&req[8]);
    /* Range-check function + format on the firmware side so the host
     * gets a precise STATUS_INVAL rather than a misleading STATUS_IO
     * out of the HAL backend.  The host driver already pre-validates;
     * this is the defence-in-depth pass. */
    if (function >= (uint8_t)BRIDGE_TMU_FN__COUNT) return STATUS_INVAL;
    if (format   >= (uint8_t)BRIDGE_TMU_FMT__COUNT) return STATUS_INVAL;

    uint32_t  result = 0u;
    const int rv     = bridge_hw_tmu_compute(function, format, in_a, in_b, &result);
    if (rv == BRIDGE_HW_ERR_INVAL)   return STATUS_INVAL;
    if (rv == BRIDGE_HW_ERR_RANGE)   return STATUS_OUT_OF_RANGE;
    if (rv == BRIDGE_HW_ERR_NOTIMPL) return STATUS_NOSUPPORT;
    if (rv < 0)                       return STATUS_IO;
    put_le32(reply, result);
    *reply_len = 4u;
    return STATUS_OK;
}

/* --------------------------------------------------------------- */
/* Dispatch                                                          */
/* --------------------------------------------------------------- */

typedef gd32_bridge_status_t (*cmd_handler_t)(const uint8_t *, size_t,
                                              uint8_t *, size_t, size_t *);

/* Two-tier dispatch: a sparse switch on opcode keeps the table size
 * small (vs a dense 256-entry array) without losing the "one handler
 * table" property. */
gd32_bridge_status_t protocol_dispatch(uint8_t cmd,
                                       const uint8_t *req_payload,
                                       size_t req_payload_len,
                                       uint8_t *reply_payload,
                                       size_t reply_payload_cap,
                                       size_t *reply_payload_len)
{
    cmd_handler_t h = NULL;
    switch (cmd) {
    case CMD_PING:                  h = handle_ping;           break;
    case CMD_GET_VERSION:           h = handle_get_version;    break;
    case CMD_GET_BUILD_ID:          h = handle_get_build_id;   break;
    case CMD_RESET_REASON:          h = handle_reset_reason;   break;
    case CMD_GPIO_READ:             h = handle_gpio_read;      break;
    case CMD_GPIO_WRITE:            h = handle_gpio_write;     break;
    case CMD_PWM_SET:               h = handle_pwm_set;          break;
    case CMD_PWM_GET:               h = handle_pwm_get;          break;
    case CMD_PWM_CONFIGURE:         h = handle_pwm_configure;    break;
    case CMD_ADC_READ:              h = handle_adc_read;         break;
    case CMD_ADC_CONFIGURE:         h = handle_adc_configure;    break;
    case CMD_ADC_STREAM_BEGIN:      h = handle_adc_stream_begin; break;
    case CMD_ADC_STREAM_READ:       h = handle_adc_stream_read;  break;
    case CMD_ADC_STREAM_END:        h = handle_adc_stream_end;   break;
    case CMD_TRNG_READ:             h = handle_trng_read;        break;
    case CMD_TMU_COMPUTE:           h = handle_tmu_compute;      break;
    case CMD_DA9292_STATUS_FORWARD: h = handle_da9292_forward;   break;
    case CMD_DAC_SET:               h = handle_dac_set;          break;
    case CMD_DAC_GET:               h = handle_dac_get;          break;
    case CMD_QENC_READ:             h = handle_qenc_read;        break;
    case CMD_QENC_RESET:            h = handle_qenc_reset;       break;
    case CMD_COUNTER_READ:          h = handle_counter_read;     break;
    default:
        /* Route the reserved OTA opcode range (0xF0..0xFF) through
         * the application bootloader's dispatcher.  Bodies return
         * STATUS_NOSUPPORT until the FMC HAL lands -- see
         * src/bootloader/. */
        if (cmd >= CMD_OTA_BEGIN && cmd <= 0xFFu) {
            return bl_dispatch_ota(cmd, req_payload, req_payload_len,
                                   reply_payload, reply_payload_cap,
                                   reply_payload_len);
        }
        *reply_payload_len = 0u;
        return STATUS_NOSUPPORT;
    }
    return h(req_payload, req_payload_len,
             reply_payload, reply_payload_cap, reply_payload_len);
}
