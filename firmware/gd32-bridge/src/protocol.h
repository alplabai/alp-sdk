/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal header for the gd32-bridge firmware.  Declares the
 * protocol opcodes + the dispatcher entry that both transports
 * (SPI + I2C) feed into.
 *
 * The wire-side spec is in ../docs/gd32-bridge-protocol.md; opcode
 * numbering MUST stay in sync with <alp/chips/gd32g553.h> on the
 * host side.
 */

#ifndef GD32_BRIDGE_PROTOCOL_H
#define GD32_BRIDGE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------- */
/* Wire constants -- keep in sync with the host header.              */
/* --------------------------------------------------------------- */

#define GD32_BRIDGE_SOF                  0xA5u
#define GD32_BRIDGE_I2C_REG_CMD          0x00u
#define GD32_BRIDGE_ADC_MAX_SAMPLES      8u
#define GD32_BRIDGE_BUILD_ID_LEN         20u

/* WIRE-PROTOCOL version (compatibility gate) -- GET_VERSION returns this
 * triple and the host driver refuses a mismatched MAJOR.  This is NOT
 * the firmware *release* version: that is its own semver in
 * firmware-version.txt, surfaced via GET_BUILD_ID ("<ver>+<sha>").  The
 * two axes move independently. */
#define PROTOCOL_VERSION_MAJOR           0u
#define PROTOCOL_VERSION_MINOR           5u
#define PROTOCOL_VERSION_PATCH           0u

/* Number of concurrent DMA-backed ADC streams the firmware supports.
 * Bounded by the GD32G553's two DMA controllers (DMA0 + DMA1 with
 * 7 channels each, per the datasheet).  Stream 0 binds to a DMA0
 * channel; stream 1 binds to a DMA1 channel; both can run at
 * different sample rates against different ADC channels. */
#define GD32_BRIDGE_ADC_STREAM_COUNT        2u

/* Number of ADC samples the bridge's streaming ring buffer can hold
 * per stream (firmware-side DMA destination).  Host polls
 * CMD_ADC_STREAM_READ for batches; a non-empty ring lets the firmware
 * decouple DMA cadence from host poll cadence.  Sized in u16 mV slots. */
#define GD32_BRIDGE_ADC_STREAM_RING_SAMPLES 128u

/* Maximum samples returned by a single CMD_ADC_STREAM_READ reply.
 * Bounded by the wire's MAX_PAYLOAD_BYTES; tuned to keep the SPI
 * read transaction under ~1 ms at 10 MHz. */
#define GD32_BRIDGE_ADC_STREAM_READ_MAX     32u

/* Maximum wire payload either direction.  Driven by the larger of
 * CMD_ADC_READ (1 + N*2 bytes) and CMD_ADC_STREAM_READ (1 + N*2
 * bytes), so the same formula applies; sample-count ceiling is
 * the larger of the two opcode-side limits. */
#define GD32_BRIDGE_MAX_PAYLOAD_BYTES                                          \
    (1u + ((GD32_BRIDGE_ADC_STREAM_READ_MAX > GD32_BRIDGE_ADC_MAX_SAMPLES      \
            ? GD32_BRIDGE_ADC_STREAM_READ_MAX                                  \
            : GD32_BRIDGE_ADC_MAX_SAMPLES) * 2u))

typedef enum {
    CMD_PING                  = 0x00,
    CMD_GET_VERSION           = 0x01,
    CMD_GET_BUILD_ID          = 0x02,
    CMD_RESET_REASON          = 0x03,
    CMD_GPIO_READ             = 0x10,
    CMD_GPIO_WRITE            = 0x11,
    CMD_PWM_SET               = 0x20,
    CMD_PWM_GET               = 0x21,
    /* v0.3: sticky per-channel PWM tuning (align mode, dead time, fault
     * inputs).  On V2N every E1M PWM channel rides one of the GD32's
     * 16-bit advanced timers (PWM0..3 -> TIMER0 channels MCH0..MCH3,
     * PWM4..7 -> TIMER7 channels MCH0..MCH3 per
     * `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`).  The 16-bit
     * counter at the GD32's 240 MHz core clock gives ~4.16 ns LSB
     * resolution + 273 us maximum period; CMD_PWM_GET reports the
     * actual programmed value so callers can see what rounding the
     * firmware applied. */
    CMD_PWM_CONFIGURE         = 0x22,
    CMD_ADC_READ              = 0x30,
    /* v0.3: sticky per-channel ADC tuning -- oversampling ratio,
     * sample-and-hold cycles, resolution (12/14/16-bit, the latter via
     * 16x or 256x oversample on the GD32G5).  CMD_ADC_READ honours
     * the configured tuning on the next call. */
    CMD_ADC_CONFIGURE         = 0x32,
    /* v0.3: DMA-backed streaming.  Two streams can run concurrently
     * (one per GD32 DMA controller).  STREAM_BEGIN binds a stream_id
     * to a channel + sample rate; STREAM_READ drains the ring;
     * STREAM_END releases the DMA + ring.  Stream id encoded in the
     * payload of each opcode so a single handler covers both
     * controllers. */
    CMD_ADC_STREAM_BEGIN      = 0x33,
    CMD_ADC_STREAM_READ       = 0x34,
    CMD_ADC_STREAM_END        = 0x35,
    CMD_DA9292_STATUS_FORWARD = 0x40,
    /* v0.2 additions -- the GD32 carries every E1M-standard analog
     * and counter peripheral on V2N (per gd32-io-mcu-map.tsv); the
     * SDK's portable surface routes through these. */
    CMD_DAC_SET               = 0x50,
    CMD_DAC_GET               = 0x51,
    CMD_QENC_READ             = 0x60,
    CMD_QENC_RESET            = 0x61,
    CMD_COUNTER_READ          = 0x70,
    /* v0.3: GD32G5 security block.  TRNG is the NIST SP800-90B
     * pre-certified true-random generator -- 32-bit pull per op (or
     * 128-bit on the NIST path; firmware-internal choice).  CAU
     * (AES/DES hardware) is reserved for v0.4 once the PSA Crypto
     * entropy / cipher driver registration lands. */
    CMD_TRNG_READ             = 0x80,
    /* v0.4: GD32G5 TMU (CORDIC) math accelerator.  General-purpose
     * fixed-function trig / sqrt / log / exp / vector-magnitude block
     * with a 12-input function table.  Request payload
     * `function:u8 format:u8 reserved:u16 in_a:u32 in_b:u32` (12 B);
     * reply `result:u32 status:u8` (5 B).  Format 0 = Q31 fixed-point;
     * format 1 = IEEE-754 single (the firmware's default plumbing). */
    CMD_TMU_COMPUTE           = 0x90,
    /* v0.5: ADC-stream DSP pipeline configuration -- attaches a chain
     * of FIR / IIR / WINDOW / FFT stages to a streaming ADC source so
     * raw samples never leave the GD32 when the customer's intent is
     * filtered or spectral data (the link bandwidth wins more from
     * pushing the post-processing onto the bridge than from sending
     * the raw samples).  RESERVED + tombstoned at v0.5: the original
     * single-shot configure payload won't fit one FIR stage's 256-byte
     * Q31-tap blob inside the 65-byte wire envelope, so the actual
     * upload path is the three chunked sub-opcodes CMD_ADC_DSP_CHAIN_*
     * (0x37/0x38/0x39) below.  The 0x36 opcode keeps its slot to
     * avoid renumbering across the v0.5.x line; firmware default-case
     * dispatch returns STATUS_NOSUPPORT for it. */
    CMD_ADC_STREAM_CONFIGURE_DSP = 0x36,
    /* v0.5 (§2B): chunked DSP-chain upload path.  Three sub-opcodes:
     *   CHAIN_OPEN   allocates a firmware-side chain handle, returns
     *                its u8 id in the reply payload.
     *   STAGE_PUSH   uploads one chunk of one stage's per-kind params
     *                (FIR taps / IIR sections / WINDOW shape / FFT
     *                size + output format) into the named chain at
     *                the named stage_index + chunk_offset.  Variable
     *                request payload up to GD32_BRIDGE_MAX_PAYLOAD_BYTES;
     *                empty reply.  Repeated as many times as needed
     *                to assemble the full per-kind blob.
     *   CHAIN_BIND   attaches the chain to an existing streaming ADC
     *                source by stream_id.  From then on the stream's
     *                samples flow through the chain instead of straight
     *                to the host -- the v0.5.x wire format flip from
     *                "host-side DSP" to "GD32-side DSP" that's the real
     *                wave-2 value.
     * All three are RESERVED at protocol v0.5; firmware default-case
     * dispatch returns STATUS_NOSUPPORT until the bridge_hw_adc_dsp_*
     * HAL bodies land in the GD32 firmware tree.  Host helpers in
     * chips/gd32g553/ honour the same NOSUPPORT contract by routing
     * the wire dispatch through cmd_send unchanged.  See
     * memory/project_wave2_dsp_pipeline_design.md for design context. */
    CMD_ADC_DSP_CHAIN_OPEN       = 0x37,
    CMD_ADC_DSP_STAGE_PUSH       = 0x38,
    CMD_ADC_DSP_CHAIN_BIND       = 0x39,
    /* v0.5 (§2B.2): advanced timer extras.  PWM_CAPTURE turns a
     * PWM channel's pin into an input-capture source for frequency
     * / pulse-width measurement; PWM_SINGLE_PULSE drives a one-shot
     * pulse of caller-specified duration on a PWM channel then
     * stops; TIMER_SYNC links TIMER0 / TIMER7 / TIMER19 in
     * master-slave configuration for synchronised multi-channel
     * output.  All five opcodes are RESERVED at protocol v0.5;
     * the firmware default-case path returns STATUS_NOSUPPORT
     * until the corresponding bridge_hw_* HAL bodies land in
     * the GD32 firmware tree.  The portable surfaces are declared
     * in <alp/pwm.h> / <alp/counter.h> with the same NOSUPPORT
     * contract on builds that don't ship the bridge HAL yet. */
    CMD_PWM_CAPTURE_BEGIN        = 0x23,
    CMD_PWM_CAPTURE_READ         = 0x24,
    CMD_PWM_CAPTURE_END          = 0x25,
    CMD_PWM_SINGLE_PULSE         = 0x26,
    CMD_TIMER_SYNC               = 0x27,
    /* v0.5 (§2B.3): system-wide power-mode transition.  Host requests
     * SLEEP / DEEP_SLEEP / STANDBY via the supervisor, which prepares
     * the GD32 + signals the Renesas SoC to enter the matching mode,
     * then re-runs the bridge handshake on wakeup so the host can
     * resume bridge calls.  RESERVED at protocol v0.5; firmware
     * dispatcher returns STATUS_NOSUPPORT until the HAL body lands.
     * Portable surface in <alp/power.h>. */
    CMD_POWER_MODE_SET           = 0x28,
} gd32_bridge_cmd_t;

/* TMU function index sent in CMD_TMU_COMPUTE's request payload byte 0.
 * Mirrors @ref gd32g553_tmu_function_t on the host side. */
typedef enum {
    BRIDGE_TMU_FN_SIN     = 0u,
    BRIDGE_TMU_FN_COS     = 1u,
    BRIDGE_TMU_FN_TAN     = 2u,
    BRIDGE_TMU_FN_ATAN    = 3u,
    BRIDGE_TMU_FN_ATAN2   = 4u,
    BRIDGE_TMU_FN_SQRT    = 5u,
    BRIDGE_TMU_FN_LOG     = 6u,
    BRIDGE_TMU_FN_EXP     = 7u,
    BRIDGE_TMU_FN_SINH    = 8u,
    BRIDGE_TMU_FN_COSH    = 9u,
    BRIDGE_TMU_FN_TANH    = 10u,
    BRIDGE_TMU_FN_HYPOT   = 11u,
    BRIDGE_TMU_FN__COUNT  = 12u, /**< sentinel; not a valid function. */
} gd32_bridge_tmu_function_t;

/* TMU operand / result format byte 1 of CMD_TMU_COMPUTE.  Mirrors
 * @ref gd32g553_tmu_format_t on the host side. */
typedef enum {
    BRIDGE_TMU_FMT_Q31    = 0u, /**< Q31 fixed-point (signed).  */
    BRIDGE_TMU_FMT_F32    = 1u, /**< IEEE-754 single precision. */
    BRIDGE_TMU_FMT__COUNT = 2u, /**< sentinel.                   */
} gd32_bridge_tmu_format_t;

/* Wire-side status byte; mirrors the table in docs/gd32-bridge-protocol.md §6.
 * Note the unsigned magnitude vs the host's negative alp_status_t -- the
 * firmware doesn't depend on the host's signed enum. */
typedef enum {
    STATUS_OK            = 0x00,
    STATUS_INVAL         = 0x01,
    STATUS_NOT_READY     = 0x02,
    STATUS_BUSY          = 0x03,
    STATUS_TIMEOUT       = 0x04,
    STATUS_IO            = 0x05,
    STATUS_NOSUPPORT     = 0x06,
    STATUS_NOMEM         = 0x07,
    STATUS_OUT_OF_RANGE  = 0x08,
    STATUS_NO_PENDING    = 0x80, /* I2C-only: read before any matching write */
} gd32_bridge_status_t;

/* --------------------------------------------------------------- */
/* Dispatcher                                                         */
/* --------------------------------------------------------------- */

/*
 * protocol_dispatch -- called by either transport when a complete
 * request envelope has been validated (CRC OK, framing OK).
 *
 * Inputs:
 *   cmd            -- opcode (one of CMD_*).
 *   req_payload    -- pointer to N request payload bytes (may be
 *                     NULL when req_payload_len == 0).
 *   req_payload_len-- length of req_payload.
 *
 * Outputs (caller-supplied):
 *   reply_payload      -- buffer for M reply payload bytes.
 *   reply_payload_cap  -- capacity of reply_payload.
 *   reply_payload_len  -- [out] M (bytes actually written).
 *
 * Return:  STATUS_OK on success; STATUS_NOSUPPORT for unknown
 *          opcodes; STATUS_INVAL on bad payload lengths /
 *          out-of-range args; STATUS_TIMEOUT / STATUS_IO for
 *          downstream peripheral errors (e.g. an ADC or timer
 *          peripheral fault).
 */
gd32_bridge_status_t protocol_dispatch(uint8_t cmd,
                                       const uint8_t *req_payload,
                                       size_t req_payload_len,
                                       uint8_t *reply_payload,
                                       size_t reply_payload_cap,
                                       size_t *reply_payload_len);

/* --------------------------------------------------------------- */
/* CRC-16 / CCITT-FALSE -- shared between transports.                */
/* --------------------------------------------------------------- */

uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len);

#endif /* GD32_BRIDGE_PROTOCOL_H */
