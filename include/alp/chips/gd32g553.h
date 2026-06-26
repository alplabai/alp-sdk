/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gd32g553.h
 * @brief Host-side driver for the GD32G553MEY7TR supervisor MCU on
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *        the E1M-X V2N / V2N-M1 SoMs.
 *
 * The GD32G553 is the V2N module's general-purpose supervisor MCU.
 * It owns a fleet of pads that don't fit on the Renesas RZ/V2N pinout
 * (notably two of the eight E1M PWM channels post-2026-05-11 schematic
 * revision and several E1M I2C management lines).  The DA9292 fault
 * pins do NOT reach the GD32 on the current SoM revision -- the
 * DA9292_INT/DA9292_TW nets land only on Renesas P37/P36, so the
 * 0x40 forwarder answers the 0xFF sentinel (see
 * gd32g553_da9292_status_forward); the GD32 also has no I2C path to
 * the PMIC, whose register-level status is read by the CM33/host over
 * BRD_I2C via the `chips/da9292` driver.  Host code reaches the
 * supervisor over a **hybrid** transport:
 *
 *   - **SPI fast path** (Renesas SCI7 Simple-SPI master / GD32 slave on
 *     the GD32's `PA8`/`PA9`/`PA10`/`PB15` pads) -- low-latency,
 *     dedicated link.
 *   - **I2C management path** on BRD_I2C (Renesas RIIC8 master / GD32
 *     slave on the GD32's `PA15`/`PB9` pads at I2C address `0x70` by
 *     default) -- shares the bus with PMICs, RTC, OPTIGA, telemetry
 *     INA236s on the EVK.
 *
 * Both transports carry the **same wire protocol** declared in
 * `docs/gd32-bridge-protocol.md` -- only the framing layer differs.
 * Pick which transport to use per call with @ref gd32g553_set_default_transport
 * or override on a single call with the `*_via` family of helpers.
 *
 * @par Why a hybrid bridge
 * - SPI is faster and dedicated (no contention from PMIC traffic),
 *   so high-frequency telemetry / PWM updates ride SPI by default.
 * - I2C is the management path: if the application code is already
 *   on BRD_I2C for PMIC + RTC work, it can poke the GD32 from the
 *   same bus without setting up a second peripheral; useful for
 *   bring-up, JTAG-less production-test scripts, and post-mortem
 *   recovery when the SPI link has failed.
 *
 * @par Liveness
 * @ref gd32g553_init issues `PING` then `GET_VERSION` on the chosen
 * default transport.  Mismatched major version refuses to operate;
 * see `docs/gd32-bridge-protocol.md` §8.
 *
 * @par Threadsafety
 * The driver context is not internally synchronised.  Concurrent
 * calls into the same `gd32g553_t` from multiple threads must be
 * serialised by the caller (or each thread can hold its own context
 * pointing at the same bus handles).
 */

#ifndef ALP_CHIPS_GD32G553_H
#define ALP_CHIPS_GD32G553_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- */
/* Wire constants — kept in sync with docs/gd32-bridge-protocol.md   */
/* and firmware/gd32-bridge/src/protocol.c on the firmware side.              */
/* --------------------------------------------------------------- */

/** Start-of-frame marker carried by every SPI frame. */
#define GD32G553_BRIDGE_SOF 0xA5u

/** Virtual "command register" address on the I2C side. */
#define GD32G553_BRIDGE_I2C_REG_CMD 0x00u

/** Default 7-bit I2C slave address (compile-time configurable on the
 *  firmware side; production builds may relocate to dodge address
 *  collisions on a specific board). */
#define GD32G553_BRIDGE_DEFAULT_I2C_ADDR 0x70u

/** Default reply timeout in milliseconds for the SPI path (the GD32
 *  is required to populate its TX FIFO inside this window before the
 *  host clocks the reply bytes). */
#define GD32G553_BRIDGE_REPLY_TIMEOUT_MS 10u

/** Maximum number of ADC samples the bridge accepts per `ADC_READ`. */
#define GD32G553_BRIDGE_ADC_MAX_SAMPLES 8u

/** Maximum samples returned by a single `CMD_ADC_STREAM_READ` reply
 *  (v0.3+).  Bounded so a stream-drain transaction fits in the wire's
 *  reply envelope; the firmware's ring buffer is sized larger so
 *  back-to-back reads don't lose samples. */
#define GD32G553_BRIDGE_ADC_STREAM_READ_MAX 32u

/** Number of concurrent DMA-backed ADC streams the firmware supports.
 *  Bounded by the GD32G553's two DMA controllers (DMA0 + DMA1, 7
 *  channels each per the datasheet); the firmware binds stream 0 to
 *  DMA0 and stream 1 to DMA1 so they run truly concurrently. */
#define GD32G553_BRIDGE_ADC_STREAM_COUNT 2u

/** Maximum concurrent DSP chains the firmware's wave-2 pipeline can
 *  hold open (v0.5 wire format).  Each chain binds 1..@ref
 *  GD32G553_BRIDGE_ADC_DSP_MAX_STAGES stages onto one streaming source
 *  via `CMD_ADC_DSP_CHAIN_BIND`.  Sized so every active ADC stream
 *  plus a spare can carry a distinct chain configuration.  v0.5
 *  reserves the opcodes but firmware HAL bodies land in a follow-up
 *  drop -- today every CHAIN_OPEN reply rides the default-case
 *  STATUS_NOSUPPORT path. */
#define GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS 4u

/** Maximum stages per chain (mirrors @c ALP_DSP_MAX_STAGES in
 *  `<alp/dsp.h>`). */
#define GD32G553_BRIDGE_ADC_DSP_MAX_STAGES 4u

/** Maximum reassembled per-kind stage payload the firmware accepts
 *  for a single chain stage.  Sized for the largest legal blob: FIR
 *  stage with @c ALP_DSP_MAX_FIR_TAPS taps (64) at 4 bytes each
 *  (F32 or Q31) + 4 bytes of leading metadata = 260 bytes.  IIR
 *  (160-byte max), WINDOW, and FFT (each 4-byte fixed) fit
 *  comfortably below. */
#define GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES 260u

/** Maximum `chunk_data` bytes per `CMD_ADC_DSP_STAGE_PUSH` call.  The
 *  request payload header is 7 bytes (`chain_id:u8 stage_index:u8
 *  kind:u8 chunk_offset:u16 chunk_total_size:u16`); the remaining
 *  capacity inside the wire envelope is what's left here.  A 260-byte
 *  FIR-tap blob lands in @code ceil(260 / 58) = 5 @endcode STAGE_PUSH
 *  calls. */
#define GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES 58u

/** Maximum bytes returned by a single `CMD_TRNG_READ` reply (v0.3+). */
#define GD32G553_BRIDGE_TRNG_MAX_BYTES 32u

/** Number of DAC output channels the bridge exposes (mirrors the
 *  E1M v1.0 `DAC0..DAC1` allocation; GD32 pads `PA4` + `PA6` per
 *  `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`). */
#define GD32G553_BRIDGE_DAC_CHANNELS 2u

/** Number of quadrature-encoder channels the bridge exposes
 *  (E1M `ENC0..ENC3` on GD32 pads `PA0/PB3`, `PC6/PC7`, `PB6/PB5`,
 *  `PB2/PA1`). */
#define GD32G553_BRIDGE_QENC_CHANNELS 4u

/** Number of free-running counters the bridge exposes.  v0.2 of
 *  the protocol surfaces a single reader; boards that need
 *  multiple counters await the v0.3 opcode set. */
#define GD32G553_BRIDGE_COUNTER_CHANNELS 1u

/** Length of the truncated SHA-1 build-id (ASCII hex, NUL-terminated). */
#define GD32G553_BUILD_ID_LEN 20u

/** Protocol major version the **host driver** speaks.  Hosts whose
 *  major differs from the GD32 firmware's `GET_VERSION` reply refuse
 *  to operate (see docs/gd32-bridge-protocol.md §8). */
#define GD32G553_HOST_PROTOCOL_MAJOR 0u

/** v0.7 link-feature bits (CMD_LINK_FEATURES payload).  STATUS_SEQ:
 *  once granted, every SPI reply's STATUS byte carries a 4-bit
 *  slave-side sequence stamp in bits [7:4] that advances per freshly
 *  decoded request -- a reply whose stamp has not advanced is a STALE
 *  re-serve (the request was never decoded) and the host driver
 *  re-sends once before failing.  I2C replies are never stamped. */
#define GD32G553_LINK_FEAT_STATUS_SEQ 0x01u
/** SPI STATUS byte: code lives in bits [3:0]; [7:4] = sequence stamp
 *  (zero until STATUS_SEQ is negotiated). */
#define GD32G553_STATUS_CODE_MASK 0x0Fu

/** Wire opcodes -- mirror docs/gd32-bridge-protocol.md §3. */
typedef enum {
	GD32G553_CMD_PING                  = 0x00,
	GD32G553_CMD_GET_VERSION           = 0x01,
	GD32G553_CMD_GET_BUILD_ID          = 0x02,
	GD32G553_CMD_RESET_REASON          = 0x03,
	GD32G553_CMD_GPIO_READ             = 0x10,
	GD32G553_CMD_GPIO_WRITE            = 0x11,
	GD32G553_CMD_PWM_SET               = 0x20,
	GD32G553_CMD_PWM_GET               = 0x21,
	GD32G553_CMD_ADC_READ              = 0x30,
	GD32G553_CMD_DA9292_STATUS_FORWARD = 0x40,
	/* v0.8: secure-element (OPTIGA Trust M) reset, SE_RST = GD32 PC13. */
	GD32G553_CMD_SE_RESET = 0x41,
	/* v0.2 additions -- analog + counter peripherals routed via the
     * GD32 on V2N (see metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv). */
	GD32G553_CMD_DAC_SET      = 0x50,
	GD32G553_CMD_DAC_GET      = 0x51,
	GD32G553_CMD_QENC_READ    = 0x60,
	GD32G553_CMD_QENC_RESET   = 0x61,
	GD32G553_CMD_COUNTER_READ = 0x70,
	/* v0.3 additions -- expose GD32G5 ADC + PWM HW knobs
     * (oversampling, sample-and-hold cycles, alignment / dead-time
     * for PWM, DMA-backed continuous acquisition).  On V2N every
     * E1M PWM channel rides one of the GD32's 16-bit advanced
     * timers (TIMER0 MCH0..MCH3 on PWM0..3, TIMER7 MCH0..MCH3 on
     * PWM4..7) -- ~4.63 ns LSB at the 216 MHz CK_TIMER, 303 us
     * maximum period.  CMD_PWM_GET reads the LIVE timer registers
     * (never an echo of the last request -- see gd32g553_pwm_get). */
	GD32G553_CMD_PWM_CONFIGURE    = 0x22,
	GD32G553_CMD_ADC_CONFIGURE    = 0x32,
	GD32G553_CMD_ADC_STREAM_BEGIN = 0x33,
	GD32G553_CMD_ADC_STREAM_READ  = 0x34,
	GD32G553_CMD_ADC_STREAM_END   = 0x35,
	/* v0.3 security/crypto block.  TRNG today; CAU (AES/DES) lands in
     * v0.4 with PSA Crypto driver registration. */
	GD32G553_CMD_TRNG_READ = 0x80,
	/* v0.4: GD32G5 TMU (CORDIC) math accelerator.  Standalone block
     * (NOT an ADC postprocessor): sin/cos/tan, atan/atan2, sqrt, log,
     * exp, sinh/cosh/tanh, hypot.  Function + format encoded in the
     * request payload; reply carries `result:u32 status:u8`.  Format
     * 0 = Q31 fixed-point (signed); format 1 = IEEE-754 single. */
	GD32G553_CMD_TMU_COMPUTE = 0x90,
	/* v0.5: ADC-stream DSP pipeline configuration -- RESERVED +
     * tombstoned.  Original single-shot configure can't fit a FIR
     * stage's 256-byte Q31-tap blob in the 65-byte wire envelope, so
     * the actual upload path is the three chunked sub-opcodes
     * CMD_ADC_DSP_CHAIN_* (0x37/0x38/0x39) immediately below.  The
     * 0x36 opcode keeps its slot to avoid renumbering across the
     * v0.5.x line; firmware default-case dispatch returns
     * STATUS_NOSUPPORT for it. */
	GD32G553_CMD_ADC_STREAM_CONFIGURE_DSP = 0x36,
	/* v0.5 (§2B wave-2): chunked DSP-chain upload.  CHAIN_OPEN
     * allocates a firmware-side chain handle and replies with the
     * assigned `chain_id:u8`; STAGE_PUSH uploads one chunk of one
     * stage's per-kind params (FIR taps / IIR sections / WINDOW shape
     * / FFT size + output format) by `chain_id:u8 stage_index:u8
     * kind:u8 chunk_offset:u16 chunk_total_size:u16 chunk_data[...]`,
     * repeated until the per-kind blob is fully assembled; CHAIN_BIND
     * attaches the completed chain to a streaming ADC source by
     * `chain_id:u8 stream_id:u8` -- after which the stream's samples
     * flow through the chain instead of straight to the host.  All
     * three opcodes are RESERVED at protocol v0.5 -- firmware default-
     * case dispatch returns STATUS_NOSUPPORT until the
     * `bridge_hw_adc_dsp_*` HAL bodies land in the GD32 firmware
     * tree.  Host helpers in `chips/gd32g553/` honour the same
     * NOSUPPORT contract by routing through cmd_send unchanged.
     * See `docs/gd32-bridge-protocol.md` §3.x for the wire layout and
     * `memory/project_wave2_dsp_pipeline_design.md` for design
     * context. */
	GD32G553_CMD_ADC_DSP_CHAIN_OPEN = 0x37,
	GD32G553_CMD_ADC_DSP_STAGE_PUSH = 0x38,
	GD32G553_CMD_ADC_DSP_CHAIN_BIND = 0x39,
	/* v0.5 (§2B.2): advanced timer extras.  PWM_CAPTURE turns a PWM
     * channel's pin into an input-capture source for frequency / pulse-
     * width measurement; PWM_SINGLE_PULSE drives a one-shot pulse of
     * caller-specified duration then stops; TIMER_SYNC links the
     * GD32G5's TIMER0 / TIMER7 / TIMER19 in master-slave configuration.
     * All five opcodes are RESERVED at protocol v0.5; the firmware
     * default-case path returns STATUS_NOSUPPORT until the corresponding
     * bridge_hw_* HAL bodies land.  Portable surfaces in <alp/pwm.h> +
     * <alp/counter.h> mirror the same NOSUPPORT contract on builds
     * without the HW backend. */
	GD32G553_CMD_PWM_CAPTURE_BEGIN = 0x23,
	GD32G553_CMD_PWM_CAPTURE_READ  = 0x24,
	GD32G553_CMD_PWM_CAPTURE_END   = 0x25,
	GD32G553_CMD_PWM_SINGLE_PULSE  = 0x26,
	GD32G553_CMD_TIMER_SYNC        = 0x27,
	/* v0.5 (§2B.3): system-wide power-mode transition request.
     * Portable surface in <alp/power.h>.  Reserved opcode at v0.5;
     * firmware dispatcher returns STATUS_NOSUPPORT today. */
	GD32G553_CMD_POWER_MODE_SET = 0x28,
	/* v0.7: link-feature negotiation (request `features:u8` wanted,
     * reply `features:u8` granted+armed).  Older firmware answers
     * STATUS_NOSUPPORT and the host stays on the legacy framing --
     * gd32g553_init() negotiates automatically and records the
     * outcome in ctx->seq_enabled. */
	GD32G553_CMD_LINK_FEATURES = 0x81,
	/* Reserved range 0xF0..0xFF -- application-bootloader OTA. */
	GD32G553_CMD_OTA_BEGIN       = 0xF0,
	GD32G553_CMD_OTA_WRITE_CHUNK = 0xF1,
	GD32G553_CMD_OTA_VERIFY      = 0xF2,
	GD32G553_CMD_OTA_COMMIT      = 0xF3,
	GD32G553_CMD_OTA_ROLLBACK    = 0xF4,
	GD32G553_CMD_OTA_GET_STATE   = 0xF5,
	GD32G553_CMD_OTA_ABORT       = 0xF6,
} gd32g553_cmd_t;

/** Transport-selection enum.  Use with @ref gd32g553_set_default_transport
 *  or the `*_via` helpers. */
typedef enum {
	GD32G553_TRANSPORT_DEFAULT = 0, /**< Use ctx->default_transport. */
	GD32G553_TRANSPORT_SPI     = 1, /**< Force SPI fast path.        */
	GD32G553_TRANSPORT_I2C     = 2, /**< Force I2C management path.  */
} gd32g553_transport_t;

/** Reset-cause codes (per `gd32g553_reset_cause_t` on the firmware side). */
typedef enum {
	GD32G553_RESET_UNKNOWN  = 0, /**< Cause not recorded / not classified. */
	GD32G553_RESET_POWER_ON = 1, /**< Power-on / cold reset (POR). */
	GD32G553_RESET_NRST_PIN = 2, /**< External NRST pin asserted. */
	GD32G553_RESET_SOFT     = 3, /**< Software reset (SYSRESETREQ). */
	GD32G553_RESET_WDT      = 4, /**< Watchdog timeout. */
	GD32G553_RESET_BROWNOUT = 5, /**< Brown-out / supply-voltage reset. */
	GD32G553_RESET_LOWPOWER = 6, /**< Exit from a low-power/standby reset. */
} gd32g553_reset_cause_t;

/** Firmware version triple returned by `GET_VERSION`. */
typedef struct {
	uint8_t major; /**< Major: bumped on a breaking wire change; host refuses
	                    a mismatch vs @ref GD32G553_HOST_PROTOCOL_MAJOR. */
	uint8_t minor; /**< Minor: additive opcode/payload additions. */
	uint8_t patch; /**< Patch: bug-fix-only firmware build. */
} gd32g553_version_t;

/** Driver context.
 *
 *  At least one of @c spi / @c i2c MUST be non-NULL at @ref gd32g553_init
 *  time.  Passing both attaches the context to **both** transports and
 *  routes commands through @c default_transport unless a `*_via` helper
 *  is used.
 */
typedef struct {
	bool                 initialised;
	alp_spi_t           *spi;               /**< NULL if SPI not wired. */
	alp_i2c_t           *i2c;               /**< NULL if I2C not wired. */
	uint8_t              i2c_addr;          /**< Valid when i2c != NULL.*/
	gd32g553_transport_t default_transport; /**< Picked at init time.    */
	gd32g553_version_t   version;           /**< Cached after init.      */
	bool                 seq_enabled;       /**< v0.7 STATUS_SEQ granted
                                                 (negotiated at init).   */
	uint8_t              seq_last;          /**< Last accepted stamp.    */
	uint32_t             seq_stale_count;   /**< Stale replies caught +
                                                 recovered (telemetry).  */
} gd32g553_t;

/**
 * @brief Probe + handshake the bridge over the chosen transport.
 *
 * Pass exactly one or both bus handles:
 *
 *   - SPI-only:  @c spi != NULL, @c i2c == NULL.  Default transport
 *                becomes SPI.
 *   - I2C-only:  @c spi == NULL, @c i2c != NULL.  Default transport
 *                becomes I2C; @c i2c_addr_7bit must be the firmware's
 *                configured address (typically
 *                @ref GD32G553_BRIDGE_DEFAULT_I2C_ADDR).
 *   - Hybrid:    both non-NULL.  Default transport becomes SPI (the
 *                faster path); callers can flip via
 *                @ref gd32g553_set_default_transport or per-call via
 *                the `*_via` helpers.
 *
 * Sends `PING` then `GET_VERSION` to confirm the link.  Caches the
 * version in @c ctx->version.  Refuses to initialise if the firmware's
 * `major` does not match @ref GD32G553_HOST_PROTOCOL_MAJOR.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_init(gd32g553_t *ctx, alp_spi_t *spi, alp_i2c_t *i2c, uint8_t i2c_addr_7bit);

/** @brief Pick which transport future commands ride by default. */
alp_status_t gd32g553_set_default_transport(gd32g553_t *ctx, gd32g553_transport_t t);

/** @brief Probe -- round-trips an empty frame.  Useful for liveness. */
alp_status_t gd32g553_ping(gd32g553_t *ctx);

/** @brief Probe over a specific transport (overrides ctx->default). */
alp_status_t gd32g553_ping_via(gd32g553_t *ctx, gd32g553_transport_t t);

/** @brief Read the bridge firmware version (cached at init by
 *         @ref gd32g553_init; this helper re-issues `GET_VERSION` so
 *         the host can confirm the firmware has not been swapped
 *         out-of-band, e.g. across a deep-sleep + OTA cycle). */
alp_status_t gd32g553_get_version(gd32g553_t *ctx, gd32g553_version_t *out);

/** @brief Read the bridge firmware's truncated SHA-1 build-id.
 *
 *  @param ctx       GD32G553 bridge context (must be initialised first).
 *  @param build_id  Destination buffer for the NUL-terminated ASCII
 *                   hex string of length up to @ref GD32G553_BUILD_ID_LEN.
 *                   Buffer size MUST be at least
 *                   @ref GD32G553_BUILD_ID_LEN + 1 bytes. */
alp_status_t gd32g553_get_build_id(gd32g553_t *ctx, char build_id[GD32G553_BUILD_ID_LEN + 1]);

/** @brief Read the cause of the GD32's most recent reset (cleared on
 *         the firmware side after this call). */
alp_status_t gd32g553_get_reset_reason(gd32g553_t *ctx, gd32g553_reset_cause_t *out);

/** @brief Read a masked subset of the GD32's pad levels.
 *
 *  @param ctx     GD32G553 bridge context (must be initialised first).
 *  @param mask    Logical GD32 pad indices the caller cares about.
 *                 Mapping is documented in firmware/gd32-bridge/README.md;
 *                 the host MUST NOT assume bit `n` is `Pxn`.
 *  @param levels  Output: bit `i` set iff (mask bit i set) and
 *                 (the corresponding pad reads high). */
alp_status_t gd32g553_gpio_read(gd32g553_t *ctx, uint32_t mask, uint32_t *levels);

/** @brief Atomically set/clear masked subset of GD32 pad outputs. */
alp_status_t gd32g553_gpio_write(gd32g553_t *ctx, uint32_t mask, uint32_t levels);

/** @brief Set a PWM channel's period + duty (nanoseconds).
 *
 *  Duty `0` shuts the channel off; `duty_ns == period_ns` drives it
 *  permanently high.  The firmware rounds to its hardware-achievable
 *  resolution; the caller can read back via @ref gd32g553_pwm_get to
 *  see what actually got programmed. */
alp_status_t
gd32g553_pwm_set(gd32g553_t *ctx, uint8_t channel, uint32_t period_ns, uint32_t duty_ns);

/** @brief Read back what a PWM channel's timer is ACTUALLY generating.
 *
 *  The firmware reads the live timer registers (period/compare), never
 *  a software echo of the last request, so the reply reflects silicon
 *  truth (fw >= v0.2.3).  Two consequences worth knowing:
 *    - the period is SHARED per underlying timer, so a
 *      @ref gd32g553_pwm_set on a sibling channel moves this channel's
 *      reported period too;
 *    - before the first set, a channel reports the boot default
 *      (65.536 ms period, 0 duty), not zeros. */
alp_status_t
gd32g553_pwm_get(gd32g553_t *ctx, uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns);

/** @brief Read @p samples ADC measurements from @p channel.
 *
 *  @param ctx      GD32G553 bridge context (must be initialised first).
 *  @param channel  Firmware-defined logical ADC channel index.
 *  @param samples  Number of consecutive measurements
 *                  (1..@ref GD32G553_BRIDGE_ADC_MAX_SAMPLES).  Values
 *                  beyond the maximum are silently capped by the
 *                  firmware.
 *  @param mv       Caller-supplied buffer for the reply.  Length
 *                  MUST be at least @p samples entries; the firmware
 *                  writes one millivolt reading per slot.  */
alp_status_t gd32g553_adc_read(gd32g553_t *ctx, uint8_t channel, uint8_t samples, uint16_t *mv);

/** @brief Read the bridge's DA9292 fault-pin forward byte.
 *
 *  Opcode `0x40` (DA9292_STATUS_FORWARD).  On the **current SoM
 *  revision this always returns `0xFF`**: the DA9292_INT/DA9292_TW
 *  fault nets land only on Renesas pads P37/P36 -- the GD32 has no
 *  connection to them (schematic-verified 2026-06-04) and no I2C path
 *  to the PMIC.  The byte packing is reserved for a future HW rev
 *  that mirrors the fault nets onto GD32 inputs:
 *    - bit0 = DA9292_INT asserted (active-low net)
 *    - bit1 = DA9292_TW  asserted (active-low net)
 *    - bits 2-6 reserved (0)
 *    - `0xFF` = "no sample available" sentinel
 *
 *  On today's hardware use @ref da9292_get_fault_pins (CM33 reads
 *  P37/P36 directly, same packing) for live fault-pin state, and
 *  @ref da9292_get_status for register-level PMIC status
 *  (PMC_STATUS_00 etc.) over BRD_I2C -- both in the `chips/da9292`
 *  driver. */
alp_status_t gd32g553_da9292_status_forward(gd32g553_t *ctx, uint8_t *status);

/** @brief Drive the secure-element (OPTIGA Trust M) reset line,
 *         SE_RST = GD32 `PC13`.
 *
 *  The OPTIGA sits on the shared BRD_I2C management bus.  Use this to
 *  recover a bus the SE has clock-stretched low (it can hold SCL down
 *  mid-APDU after an aborted transaction): pulse the line -- assert,
 *  wait, release -- then leave ~15 ms before re-probing while the SE
 *  warm-boots.  The SPI transport reaches the GD32 even while BRD_I2C is
 *  held low, so this recovers the bus even when the I2C transport is dead.
 *
 *  @param ctx     GD32G553 bridge context (must be initialised first).
 *  @param assert  @c true holds the SE in reset; @c false releases it
 *                 (the firmware owns the active-low polarity).
 *
 *  @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT (firmware lacks
 *          the HAL body) / transport error.
 */
alp_status_t gd32g553_se_reset(gd32g553_t *ctx, bool assert);

/** @brief Program a DAC channel's output voltage in millivolts.
 *
 *  The firmware rounds to its hardware-achievable resolution
 *  (12-bit on the GD32's built-in DAC); the host can read back via
 *  @ref gd32g553_dac_get to see what actually got programmed.
 *
 *  @param ctx       GD32G553 bridge context (must be initialised first).
 *  @param channel   DAC channel (0..@c GD32G553_BRIDGE_DAC_CHANNELS - 1).
 *  @param value_mv  Requested output in mV.  Saturates to the DAC's
 *                   reference rail on the firmware side.
 *
 *  @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT (firmware
 *          lacks the HAL body) / transport error.
 */
alp_status_t gd32g553_dac_set(gd32g553_t *ctx, uint8_t channel, uint16_t value_mv);

/** @brief Read back a DAC channel's currently-programmed output (mV). */
alp_status_t gd32g553_dac_get(gd32g553_t *ctx, uint8_t channel, uint16_t *value_mv);

/** @brief Read the signed accumulated count of a quadrature encoder.
 *
 *  @param ctx          GD32G553 bridge context (must be initialised first).
 *  @param encoder      Encoder index (0..@c GD32G553_BRIDGE_QENC_CHANNELS - 1).
 *  @param position_out Signed count since the last reset / boot.
 *                      Wraps modulo 2^32 on overflow.
 */
alp_status_t gd32g553_qenc_read(gd32g553_t *ctx, uint8_t encoder, int32_t *position_out);

/** @brief Reset a quadrature encoder's accumulated count to zero. */
alp_status_t gd32g553_qenc_reset(gd32g553_t *ctx, uint8_t encoder);

/** @brief Read a free-running counter's current tick value.
 *
 *  v0.2 of the protocol does not expose a counter-frequency opcode,
 *  so the caller must know the bridge counter's tick rate out-of-
 *  band (firmware-defined; see `firmware/gd32-bridge/README.md`).  A future
 *  minor revision will add `CMD_COUNTER_GET_FREQ` so the host can
 *  convert ticks ↔ microseconds without that out-of-band knowledge.
 *
 *  @param ctx       GD32G553 bridge context (must be initialised first).
 *  @param counter   Counter index (0..@c GD32G553_BRIDGE_COUNTER_CHANNELS - 1).
 *  @param ticks_out Current tick value.
 */
alp_status_t gd32g553_counter_read(gd32g553_t *ctx, uint8_t counter, uint32_t *ticks_out);

/* ------------------------------------------------------------------ */
/* v0.3 protocol: GD32G5 HW knobs                                     */
/* ------------------------------------------------------------------ */

/** PWM channel alignment mode -- passes through to the GD32 timer's
 *  CAM field (see GD32G553 reference manual §17). */
typedef enum {
	GD32G553_PWM_ALIGN_EDGE        = 0, /**< Edge-aligned (counter counts up only). */
	GD32G553_PWM_ALIGN_CENTER_UP   = 1, /**< Center-aligned, flags set on up-count. */
	GD32G553_PWM_ALIGN_CENTER_DOWN = 2, /**< Center-aligned, flags set on down-count. */
	GD32G553_PWM_ALIGN_CENTER_BOTH = 3, /**< Center-aligned, flags set on both. */
} gd32g553_pwm_align_t;

/** Sticky per-channel PWM tuning.  Subsequent @ref gd32g553_pwm_set
 *  honours the configured alignment + dead-time + fault inputs.
 *
 *  Resolution note: on V2N every E1M PWM channel maps to a TIMER0 /
 *  TIMER7 channel (PWM0..3 -> TIMER0_MCH0..MCH3 on GD32 pads
 *  PA11 / PB1 / PB14 / PC5; PWM4..7 -> TIMER7_MCH0..MCH3 on PC10 /
 *  PC11 / PC12 / PD0).  Both are 16-bit advanced timers running at
 *  the 216 MHz CK_TIMER, giving ~4.63 ns LSB and 303 us max
 *  period; the firmware rounds caller `period_ns` / `duty_ns` down
 *  to the closest achievable tick count, and @ref gd32g553_pwm_get
 *  returns the rounded actual.
 *
 *  @param ctx             GD32G553 bridge context (must be initialised first).
 *  @param channel         PWM channel (0..7 per the E1M spec).
 *  @param align_mode      One of @ref gd32g553_pwm_align_t.
 *  @param dead_time_ns    Programmable dead-time for complementary
 *                         outputs (0 = no dead time).  Firmware
 *                         rounds to the next achievable value.
 *  @param break_cfg       Bit 0: enable external break input.  Other
 *                         bits reserved for future fault sources.
 *
 *  @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE /
 *          ALP_ERR_NOSUPPORT (firmware HAL body not yet wired).
 */
alp_status_t gd32g553_pwm_configure(gd32g553_t          *ctx,
                                    uint8_t              channel,
                                    gd32g553_pwm_align_t align_mode,
                                    uint32_t             dead_time_ns,
                                    uint8_t              break_cfg);

/** Sticky per-channel ADC tuning.  @ref gd32g553_adc_read honours
 *  the configured oversampling + sample-and-hold cycles + resolution
 *  on its next call.
 *
 *  @param ctx                GD32G553 bridge context (must be initialised first).
 *  @param channel            ADC channel (0..7 per the E1M spec).
 *  @param oversample_ratio   1 / 2 / 4 / 8 / 16 / 32 / 64 / 128 / 256.
 *                            Firmware rounds down to the nearest
 *                            power-of-two; 0 means "firmware default".
 *  @param sample_cycles      Sample-and-hold time in ADC cycles --
 *                            one of 2/6/12/24/47/92/247/640 (rounded
 *                            down on the firmware side).  0 means
 *                            "firmware default".
 *  @param resolution_bits    6 / 8 / 10 / 12 / 14 / 16.  14- and
 *                            16-bit modes require oversampling >= 4 /
 *                            16 respectively per the datasheet's
 *                            effective-resolution table.  0 means
 *                            "firmware default" (12-bit).
 *
 *  @return ALP_OK / ALP_ERR_INVAL (bad resolution) / ALP_ERR_OUT_OF_RANGE /
 *          ALP_ERR_NOSUPPORT (firmware HAL body not yet wired).
 */
alp_status_t gd32g553_adc_configure(gd32g553_t *ctx,
                                    uint8_t     channel,
                                    uint16_t    oversample_ratio,
                                    uint16_t    sample_cycles,
                                    uint8_t     resolution_bits);

/** Start DMA-backed continuous ADC acquisition into the firmware's
 *  ring buffer.  Host drains samples via @ref gd32g553_adc_stream_read.
 *
 *  Two streams supported concurrently: stream 0 binds to GD32 DMA0,
 *  stream 1 binds to DMA1 (per the chip's dual-DMA-controller
 *  topology).  Different channels and different sample rates can run
 *  simultaneously across the two streams, with one constraint: each
 *  stream needs its own ADC converter, so a second BEGIN whose
 *  channel shares the first stream's converter returns
 *  @ref ALP_ERR_INVAL (channels 0-1 = ADC3, 2-3 = ADC2, 4-5 = ADC1,
 *  6-7 = ADC0).  Calling BEGIN on a stream_id that's already active
 *  also returns @ref ALP_ERR_INVAL.
 *
 *  @param ctx            GD32G553 bridge context (must be initialised first).
 *  @param stream_id      Stream slot (0 .. @ref GD32G553_BRIDGE_ADC_STREAM_COUNT - 1).
 *  @param channel        ADC channel (0..7).
 *  @param sample_rate_hz Target rate, realised by a firmware pacing
 *                        timer (fw v0.2.4+): one conversion per timer
 *                        period, 1 Hz..100 kHz, quantised to the
 *                        pacer tick (1 us at >=16 Hz, 100 us below).
 *                        0 returns @ref ALP_ERR_INVAL; above the cap
 *                        returns @ref ALP_ERR_OUT_OF_RANGE.
 */
alp_status_t gd32g553_adc_stream_begin(gd32g553_t *ctx,
                                       uint8_t     stream_id,
                                       uint8_t     channel,
                                       uint32_t    sample_rate_hz);

/** Drain up to @p max_samples samples from the named stream's ring.
 *  Caller buffer must be at least @p max_samples entries.
 *
 *  @param[in]  ctx          GD32G553 bridge context (must be initialised first).
 *  @param[in]  stream_id    Stream slot (must match a previous BEGIN).
 *  @param[in]  max_samples  Caller's drain limit.  Firmware caps at
 *                           @ref GD32G553_BRIDGE_ADC_STREAM_READ_MAX.
 *  @param[out] got_samples  Number actually copied into @p mv.
 *                           May be 0 if the ring was empty since the
 *                           last poll.
 *  @param[out] mv           Caller buffer.  Length >= @p max_samples.
 *
 *  @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *          ALP_ERR_BUSY (firmware ring overran since last poll;
 *          host should poll faster).
 */
alp_status_t gd32g553_adc_stream_read(
    gd32g553_t *ctx, uint8_t stream_id, uint8_t max_samples, uint8_t *got_samples, uint16_t *mv);

/** Stop the named stream, free its DMA channel, flush the ring. */
alp_status_t gd32g553_adc_stream_end(gd32g553_t *ctx, uint8_t stream_id);

/** Pull true-random bytes from the GD32G5's NIST SP800-90B
 *  pre-certified TRNG.
 *
 *  @param[in]  ctx   GD32G553 bridge context (must be initialised first).
 *  @param[out] dest  Caller buffer.  Length >= @p len.
 *  @param[in]  len   Bytes requested (1 .. @ref GD32G553_BRIDGE_TRNG_MAX_BYTES).
 *
 *  @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *          ALP_ERR_NOSUPPORT (firmware HAL body not yet wired) /
 *          ALP_ERR_IO (TRNG startup or in-service self-check failed).
 */
alp_status_t gd32g553_trng_read(gd32g553_t *ctx, uint8_t *dest, size_t len);

/* ------------------------------------------------------------------ */
/* v0.4 protocol: GD32G5 TMU (CORDIC math accelerator)                */
/* ------------------------------------------------------------------ */

/** TMU function selector -- mirrors `gd32_bridge_tmu_function_t` on
 *  the firmware side.  The chip's CORDIC unit supports 12 fixed
 *  primitives; the enum value is sent verbatim on the wire. */
typedef enum {
	GD32G553_TMU_FN_SIN   = 0u,  /**< sin(x).             1 input.  */
	GD32G553_TMU_FN_COS   = 1u,  /**< cos(x).             1 input.  */
	GD32G553_TMU_FN_TAN   = 2u,  /**< tan(x).             1 input.  */
	GD32G553_TMU_FN_ATAN  = 3u,  /**< atan(x).            1 input.  */
	GD32G553_TMU_FN_ATAN2 = 4u,  /**< atan2(y, x).        2 inputs. */
	GD32G553_TMU_FN_SQRT  = 5u,  /**< sqrt(x).            1 input.  */
	GD32G553_TMU_FN_LOG   = 6u,  /**< natural log(x).     1 input.  */
	GD32G553_TMU_FN_EXP   = 7u,  /**< exp(x).             1 input.  */
	GD32G553_TMU_FN_SINH  = 8u,  /**< sinh(x).            1 input.  */
	GD32G553_TMU_FN_COSH  = 9u,  /**< cosh(x).            1 input.  */
	GD32G553_TMU_FN_TANH  = 10u, /**< tanh(x).            1 input.  */
	GD32G553_TMU_FN_HYPOT = 11u, /**< sqrt(x*x + y*y).    2 inputs. */
} gd32g553_tmu_function_t;

/** Operand / result format on the wire.  Reflects the two number
 *  formats the GD32G5's TMU accepts natively.  IEEE-754 single is
 *  the default the firmware wires; Q31 is reserved for callers that
 *  already operate in fixed-point and want to skip a float<->int
 *  conversion at the boundary.
 *
 *  Q31 encoding: 32-bit two's-complement, full-scale = ±1.0; bit 31
 *  is the sign.  The trig functions interpret inputs in units of pi
 *  (so x = 0x40000000 -> +0.5 -> +pi/2 rad).  Callers that prefer
 *  IEEE-754 should use @ref GD32G553_TMU_FMT_F32 -- the firmware
 *  performs the same internal conversion, just on the GD32 side. */
typedef enum {
	GD32G553_TMU_FMT_Q31 = 0u, /**< Q31 fixed-point (signed). */
	GD32G553_TMU_FMT_F32 = 1u, /**< IEEE-754 single-precision (binary32). */
} gd32g553_tmu_format_t;

/**
 * @brief Issue one TMU compute request and read the result back.
 *
 * The request payload is `function:u8 format:u8 reserved:u16
 * in_a:u32 in_b:u32` (12 bytes).  Single-input functions (sin / cos /
 * tan / atan / sqrt / log / exp / sinh / cosh / tanh) MUST set
 * @p in_b = 0; two-input functions (atan2, hypot) use both.  The reply
 * is `result:u32 status:u8` (5 bytes).  Both @p in_a / @p in_b and
 * @p result_out are interpreted in the chosen @p format -- the host
 * does NOT byte-swap or reinterpret on the caller's behalf.
 *
 * Angle units: F32 trig angles are RADIANS (the firmware converts to
 * the TMU's native units-of-pi internally; usable domain |x| <= pi --
 * the firmware does not range-reduce).  Q31 trig angles are in units
 * of pi by definition of the format (see @ref gd32g553_tmu_format_t).
 *
 * @param[in]  ctx         Initialised driver context.
 * @param[in]  function    One of @ref gd32g553_tmu_function_t.
 * @param[in]  format      One of @ref gd32g553_tmu_format_t.  Must
 *                         match how @p in_a / @p in_b were encoded.
 * @param[in]  in_a        First operand (Q31 or F32 raw bits).
 * @param[in]  in_b        Second operand for two-input functions;
 *                         ignored for one-input functions (caller
 *                         SHOULD pass 0).
 * @param[out] result_out  Result in the same @p format as the inputs.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL (bad function or
 *         format) / ALP_ERR_NOSUPPORT (firmware HAL body not yet
 *         wired) / ALP_ERR_OUT_OF_RANGE (input outside the function's
 *         domain, e.g. sqrt(negative) in Q31) / ALP_ERR_IO.
 */
alp_status_t gd32g553_tmu_compute(gd32g553_t             *ctx,
                                  gd32g553_tmu_function_t function,
                                  gd32g553_tmu_format_t   format,
                                  uint32_t                in_a,
                                  uint32_t                in_b,
                                  uint32_t               *result_out);

/* ------------------------------------------------------------------ */
/* v0.5 (§2B.2 + §2B.3) -- advanced timer extras + power-mode set     */
/*                                                                    */
/* All six opcodes return STATUS_NOSUPPORT today against the current  */
/* firmware (the bridge_hw_* HAL bodies are the gating dep -- see     */
/* the firmware-side comment block at the top of                      */
/* firmware/gd32-bridge/src/protocol.h).  The host helpers below match the     */
/* contract: every call returns ALP_ERR_NOSUPPORT today and ALP_OK    */
/* once the firmware-side HAL ships.  Portable surfaces in            */
/* <alp/pwm.h> + <alp/counter.h> + <alp/power.h> dispatch through     */
/* these on the V2N family.                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Reconfigure a PWM channel as an input-capture source.
 *
 * Mirrors @ref alp_pwm_capture_open in <alp/pwm.h>.  Request
 * payload: `channel:u8 edge:u8`.  Reply: empty + STATUS.
 *
 * @param[in] ctx      Initialised driver context.
 * @param[in] channel  PWM channel (0..7).
 * @param[in] edge     Edge polarity: 0 = rising, 1 = falling,
 *                     2 = both.  Mirrors @ref alp_pwm_capture_edge_t.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_pwm_capture_begin(gd32g553_t *ctx, uint8_t channel, uint8_t edge);

/**
 * @brief Read the latest captured period + pulse-width.
 *
 * Request payload: `channel:u8`.  Reply payload: `period_ns:u32
 * pulse_ns:u32`.
 *
 * @param[in]  ctx          Initialised driver context.
 * @param[in]  channel      PWM channel (0..7) previously bound via
 *                          @ref gd32g553_pwm_capture_begin.
 * @param[out] period_ns    Receives the captured period (ns).
 *                          May be NULL if only the pulse width is needed.
 * @param[out] pulse_ns     Receives the active-level pulse width (ns).
 *                          May be NULL if only the period is needed.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_pwm_capture_read(gd32g553_t *ctx,
                                       uint8_t     channel,
                                       uint32_t   *period_ns,
                                       uint32_t   *pulse_ns);

/**
 * @brief Stop input-capture mode on a PWM channel.
 *
 * Request payload: `channel:u8`.  Reply: empty + STATUS.  After
 * this call the channel can be re-opened as an output via
 * @ref gd32g553_pwm_set.
 *
 * @param[in] ctx      Initialised driver context.
 * @param[in] channel  PWM channel (0..7).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_pwm_capture_end(gd32g553_t *ctx, uint8_t channel);

/**
 * @brief Drive a one-shot pulse on a PWM channel.
 *
 * Request payload: `channel:u8 reserved:u8 reserved:u16
 * pulse_ns:u32`.  Reply: empty + STATUS.  After the pulse the
 * channel returns to inactive level at 0 % duty.
 *
 * @param[in] ctx       Initialised driver context.
 * @param[in] channel   PWM channel (0..7).
 * @param[in] pulse_ns  Pulse width (ns); must be > 0.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_pwm_single_pulse(gd32g553_t *ctx, uint8_t channel, uint32_t pulse_ns);

/**
 * @brief Link two GD32G5 advanced timers in master-slave mode.
 *
 * Request payload: `master:u8 slave:u8 mode:u8`.  Reply: empty
 * + STATUS.  Used to synchronise multi-channel PWM output across
 * TIMER0 / TIMER7 / TIMER19.  Mode semantics are firmware-defined
 * (see the GD32G553 reference manual §17 master-mode control bits).
 *
 * @param[in] ctx     Initialised driver context.
 * @param[in] master  Master timer index (firmware-defined enum).
 * @param[in] slave   Slave timer index.
 * @param[in] mode    Sync mode (firmware-defined: reset / gated /
 *                    trigger / external clock).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_timer_sync(gd32g553_t *ctx, uint8_t master, uint8_t slave, uint8_t mode);

/**
 * @brief Request the GD32 + the bridged Renesas SoC enter a sleep mode.
 *
 * Request payload: `mode:u8 reserved:u8 wake_bitmap:u32
 * wake_after_ms:u32`.  Reply: empty + STATUS.  The supervisor
 * configures the wake sources, signals the Renesas SoC to enter
 * the matching mode, and re-runs the bridge handshake on wakeup
 * (host-side reciprocal: call
 * @c alp_z_v2n_supervisor_invalidate() before this returns so
 * the next bridge acquire re-inits).
 *
 * Mirrors @ref alp_power_request_sleep in <alp/power.h>.
 *
 * @param[in] ctx            Initialised driver context.
 * @param[in] mode           Sleep mode: 0 = run (no-op),
 *                           1 = sleep, 2 = deep-sleep, 3 = standby.
 * @param[in] wake_bitmap    Bitmap of @c ALP_POWER_WAKE_* macros.
 * @param[in] wake_after_ms  Max wall-clock wait, or 0 for "no timer".
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t gd32g553_power_mode_set(gd32g553_t *ctx,
                                     uint8_t     mode,
                                     uint32_t    wake_bitmap,
                                     uint32_t    wake_after_ms);

/* ------------------------------------------------------------------ */
/* v0.5 (§2B wave-2) -- chunked DSP-chain upload                       */
/*                                                                    */
/* Three opcodes assemble a firmware-side DSP chain that processes a  */
/* streaming ADC source in place: CHAIN_OPEN allocates a handle,      */
/* STAGE_PUSH (called once or more per stage) uploads the per-kind    */
/* parameter blob in `<= GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES`     */
/* chunks, and CHAIN_BIND attaches the completed chain to a stream    */
/* opened with `CMD_ADC_STREAM_BEGIN`.  Wire format documented in     */
/* `docs/gd32-bridge-protocol.md` §3.x.  All three opcodes are        */
/* RESERVED at protocol v0.5; firmware default-case dispatch returns  */
/* STATUS_NOSUPPORT until the `bridge_hw_adc_dsp_*` HAL bodies land   */
/* in the GD32 firmware tree -- the host helpers below short-circuit  */
/* to `ALP_ERR_NOSUPPORT` lockstep with the firmware contract.        */
/* ------------------------------------------------------------------ */

/**
 * @brief Allocate a fresh DSP chain handle on the bridge.
 *
 * The bridge keeps up to @ref GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS chains
 * open concurrently; exhausting the pool returns @ref ALP_ERR_NOMEM
 * (firmware: `STATUS_NOMEM`).  The returned `chain_id` is opaque to
 * the host -- pass it back via @ref gd32g553_adc_dsp_stage_push and
 * @ref gd32g553_adc_dsp_chain_bind.  Chains auto-release when the
 * bound stream's `CMD_ADC_STREAM_END` runs; there is no explicit
 * `CHAIN_CLOSE` opcode at v0.5.
 *
 * @param[in]  ctx           Initialised driver context.
 * @param[out] chain_id_out  Receives the assigned chain id on
 *                           @ref ALP_OK.  May not be NULL.
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_NOT_READY (uninitialised
 *         ctx), @ref ALP_ERR_INVAL (NULL out param),
 *         @ref ALP_ERR_NOSUPPORT (firmware HAL absent today), or
 *         a transport error.
 */
alp_status_t gd32g553_adc_dsp_chain_open(gd32g553_t *ctx, uint8_t *chain_id_out);

/**
 * @brief Upload one chunk of one stage's per-kind parameters.
 *
 * The firmware accumulates chunks at `chunk_offset` byte positions
 * within an internal `[chain_id][stage_index]` buffer of size
 * @ref GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES.  A stage's payload is
 * complete once the host has covered `[0, stage_params_len)` -- the
 * firmware then validates the assembled blob against the declared
 * `kind` and either marks the stage ready or rejects the chain at
 * the eventual @ref gd32g553_adc_dsp_chain_bind call.
 *
 * Per-kind reassembled blob layout (what the firmware sees once all
 * chunks for one stage have landed):
 *
 *   - **FIR (kind=0)**:    `format:u8 n_taps:u8 reserved:u16 taps[n_taps * 4]`
 *   - **IIR (kind=1)**:    `format:u8 n_sections:u8 reserved:u16 coeffs[n_sections * 5 * 4]`
 *   - **WINDOW (kind=2)**: `shape:u8 reserved[3]`  (4 bytes)
 *   - **FFT (kind=3)**:    `n_points:u16 output_format:u8 reserved:u8`  (4 bytes)
 *
 * Host-side chunking is handled internally: the helper splits
 * @p stage_params into chunks no larger than
 * @ref GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES bytes and issues one
 * STAGE_PUSH wire transaction per chunk in offset-order.  Any error
 * mid-stream aborts the upload and returns; the chain remains in an
 * undefined-staging state until the host either retries the failed
 * chunk or releases the chain.
 *
 * @param[in] ctx               Initialised driver context.
 * @param[in] chain_id          Chain id returned by
 *                              @ref gd32g553_adc_dsp_chain_open.
 * @param[in] stage_index       Position in the chain
 *                              (`0..GD32G553_BRIDGE_ADC_DSP_MAX_STAGES-1`).
 * @param[in] kind              Stage kind (FIR=0, IIR=1, WINDOW=2, FFT=3).
 * @param[in] stage_params      Reassembled per-kind blob.  May be
 *                              NULL only when @p stage_params_len is 0.
 * @param[in] stage_params_len  Total bytes in @p stage_params.  Must
 *                              be `<= GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES`.
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_NOT_READY (uninitialised
 *         ctx), @ref ALP_ERR_INVAL (bad stage_index / kind, NULL with
 *         non-zero len), @ref ALP_ERR_OUT_OF_RANGE (`stage_params_len`
 *         too large), @ref ALP_ERR_NOSUPPORT (firmware HAL absent),
 *         or a transport error.
 */
alp_status_t gd32g553_adc_dsp_stage_push(gd32g553_t    *ctx,
                                         uint8_t        chain_id,
                                         uint8_t        stage_index,
                                         uint8_t        kind,
                                         const uint8_t *stage_params,
                                         uint16_t       stage_params_len);

/**
 * @brief Attach a fully-populated chain to a streaming ADC source.
 *
 * The bound stream's samples thereafter flow through the chain
 * instead of being delivered raw to `CMD_ADC_STREAM_READ`; the read
 * reply format becomes mode-dependent on the chain's terminal stage
 * (filter samples for FIR / IIR; complex or magnitude bins for FFT).
 * Binding fails (`ALP_ERR_INVAL` / firmware `STATUS_INVAL`) if the
 * chain has any unbound stage or violates the chain-ordering rules
 * documented in `<alp/dsp.h>` (FFT must be terminal, WINDOW must
 * immediately precede FFT).
 *
 * @param[in] ctx         Initialised driver context.
 * @param[in] chain_id    Chain id returned by
 *                        @ref gd32g553_adc_dsp_chain_open.
 * @param[in] stream_id   Stream id previously opened with
 *                        @c CMD_ADC_STREAM_BEGIN
 *                        (`0..GD32G553_BRIDGE_ADC_STREAM_COUNT-1`).
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_NOT_READY (uninitialised
 *         ctx), @ref ALP_ERR_INVAL (bad stream_id or unfinished chain),
 *         @ref ALP_ERR_NOSUPPORT (firmware HAL absent), or a transport
 *         error.
 */
alp_status_t gd32g553_adc_dsp_chain_bind(gd32g553_t *ctx, uint8_t chain_id, uint8_t stream_id);

/* ------------------------------------------------------------------ */
/* OTA -- in-system upgrade of the bridge firmware                    */
/*                                                                    */
/* Opcodes 0xF0..0xF6 implement the Path-A application-bootloader     */
/* upgrade (docs/gd32-bridge-protocol.md §10): BEGIN announces        */
/* size+CRC and erases the inactive slot, WRITE_CHUNK streams the     */
/* image, VERIFY CRC-checks it, COMMIT flips the A/B metadata and     */
/* resets into the new slot, ROLLBACK flips back.  The firmware side  */
/* is SAFE-BY-DEFAULT: only a -DBRIDGE_OTA_PARTITIONED build (paired  */
/* with the bootloader + slot-linked apps) arms the flash path;       */
/* default builds answer STATUS_NOSUPPORT to the whole range, which   */
/* these helpers surface as ALP_ERR_NOSUPPORT.                        */
/*                                                                    */
/* Transaction note: BEGIN (slot erase), VERIFY (full-image CRC) and  */
/* COMMIT/ROLLBACK (reset before the reply is drained) block the      */
/* bridge long enough that the reply transaction can miss -- treat    */
/* ALP_ERR_IO from those as "issued, confirm via GET_STATE" (or, for  */
/* COMMIT/ROLLBACK, by re-initialising against the rebooted bridge).  */
/* ------------------------------------------------------------------ */

/** OTA state-machine snapshot returned by @ref gd32g553_ota_get_state.
 *  Values are the WIRE encoding from the firmware state machine
 *  (firmware/gd32-bridge/src/ota.c) -- keep numerically identical. */
typedef enum {
	GD32G553_OTA_STATE_IDLE     = 0, /**< No upgrade session open. */
	GD32G553_OTA_STATE_READY    = 1, /**< Session open; accepting chunks. */
	GD32G553_OTA_STATE_BUSY     = 2, /**< Transient: FMC erase/program in flight. */
	GD32G553_OTA_STATE_VERIFIED = 3, /**< VERIFY matched; COMMIT allowed. */
	GD32G553_OTA_STATE_ERROR    = 4, /**< Failed; re-BEGIN to restart. */
} gd32g553_ota_state_t;

/** Slot id -- the WIRE encoding from the firmware's A/B metadata
 *  (firmware/gd32-bridge/src/ota_layout.h OTA_SLOT_A/B); 0xFF is the
 *  GET_STATE "no pending slot" sentinel. */
typedef enum {
	GD32G553_OTA_SLOT_A    = 0u,
	GD32G553_OTA_SLOT_B    = 1u,
	GD32G553_OTA_SLOT_NONE = 0xFFu, /**< No slot (e.g. nothing pending). */
} gd32g553_ota_slot_t;

/** Read-only telemetry of the OTA state machine. */
typedef struct {
	gd32g553_ota_state_t state;        /**< @ref gd32g553_ota_state_t. */
	gd32g553_ota_slot_t  active_slot;  /**< Slot the bridge is currently running. */
	gd32g553_ota_slot_t  pending_slot; /**< Staging slot of the in-progress
                                        *   session (not committed until
                                        *   COMMIT); NONE when no session
                                        *   is open. */
	uint16_t             boot_count;   /**< Monotonic boot counter from the metadata page. */
} gd32g553_ota_state_info_t;

/**
 * @brief Announce a payload size + expected CRC32 to the bridge.
 *
 * Erases the inactive slot in preparation for the chunk stream.
 *
 * @param ctx              Driver context.
 * @param size_bytes       Total payload size.
 * @param expected_crc32   CRC32 the bridge will verify after the
 *                         last chunk lands.
 * @param fw_version       Version triple of the INCOMING image; the
 *                         bridge records it in the A/B metadata at
 *                         COMMIT ("0 = unknown").  NULL sends the
 *                         legacy 8-byte BEGIN (version unknown) --
 *                         wire-compatible with pre-v0.7 firmware in
 *                         both pairings (protocol v0.7 additive form).
 * @param chunk_max_bytes  Out: chunk size the firmware accepts in
 *                         a single @ref gd32g553_ota_write_chunk.
 * @param target_slot      Out: slot the bridge will write into.
 *
 * @return ALP_OK / ALP_ERR_NOSUPPORT (firmware lacks the bodies) /
 *         transport error.
 */
alp_status_t gd32g553_ota_begin(gd32g553_t               *ctx,
                                uint32_t                  size_bytes,
                                uint32_t                  expected_crc32,
                                const gd32g553_version_t *fw_version,
                                uint16_t                 *chunk_max_bytes,
                                gd32g553_ota_slot_t      *target_slot);

/**
 * @brief Write one chunk of the payload at @p offset.
 *
 * Wire payload (protocol v0.6): `offset:u32 len:u8 data[len]` -- the
 * explicit length byte lets the firmware reject transaction-merged
 * captures regardless of CRC coincidences (the span CRC alone cannot:
 * a frame whose CRC is byte-palindromic self-validates when
 * zero-extended).
 *
 * Re-sends of an already-written chunk are deduplicated: the firmware
 * compares the bytes against flash and acks without re-programming
 * (the ECC flash cannot program a doubleword twice, even with
 * identical data).  Stream chunks contiguously upward from offset 0;
 * a chunk that PARTIALLY overlaps written data fails with an I/O
 * error and poisons the session (re-BEGIN to recover).
 *
 * @param ctx             GD32G553 bridge context (must be initialised first).
 * @param offset          Absolute byte offset of this chunk within the
 *                        OTA payload (0-based).
 * @param data            Chunk bytes.
 * @param data_len        Chunk byte count.  Must be > 0 and
 *                        <= the `chunk_max_bytes` returned by BEGIN.
 * @param received_bytes  Out: running total the firmware has seen.
 */
alp_status_t gd32g553_ota_write_chunk(gd32g553_t    *ctx,
                                      uint32_t       offset,
                                      const uint8_t *data,
                                      size_t         data_len,
                                      uint32_t      *received_bytes);

/**
 * @brief Ask the bridge to re-compute the CRC32 over the staging
 *        slot and compare against the value passed to BEGIN.
 *
 * @param ctx             GD32G553 bridge context (must be initialised first).
 * @param verified        Out: true iff the recomputed CRC32 matched.
 * @param computed_crc32  Out: the CRC32 the bridge actually saw.
 */
alp_status_t gd32g553_ota_verify(gd32g553_t *ctx, bool *verified, uint32_t *computed_crc32);

/**
 * @brief Stage a metadata-page flip and reset the bridge.
 *
 * On reboot the bootloader sees the pending flip, applies it
 * atomically, and starts the new slot.  The SPI / I2C link drops
 * during the reset; the host MUST re-issue @ref gd32g553_init after
 * a short delay (typically a few hundred milliseconds for the
 * bridge to come back online).
 */
alp_status_t gd32g553_ota_commit(gd32g553_t *ctx);

/**
 * @brief Roll back to the previously-active slot (used after a
 *        committed upgrade bricks the application).
 *
 * Like @ref gd32g553_ota_commit, this resets the bridge and the host
 * must re-init the driver.
 */
alp_status_t gd32g553_ota_rollback(gd32g553_t *ctx);

/**
 * @brief Read-only snapshot of the OTA state machine + active /
 *        pending slot.
 *
 * Safe to call at any time -- doesn't perturb the chip state.
 * Answers concretely today even on scaffold firmware: the firmware
 * handler reads its own in-RAM session struct + the metadata page.
 *
 * @param ctx  GD32G553 bridge context (must be initialised first).
 * @param out  Populated on @ref ALP_OK.  May not be NULL.
 */
alp_status_t gd32g553_ota_get_state(gd32g553_t *ctx, gd32g553_ota_state_info_t *out);

/**
 * @brief Cancel an in-progress OTA session.  Idempotent.
 *
 * Resets the firmware's in-RAM state to IDLE.  The staging slot is
 * left dirty -- the next BEGIN re-erases it before accepting the
 * first chunk.
 */
alp_status_t gd32g553_ota_abort(gd32g553_t *ctx);

/** @brief Release the context.  Idempotent.  Does **not** close the
 *         underlying SPI / I2C bus handles -- the caller owns those. */
void gd32g553_deinit(gd32g553_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GD32G553_H */
