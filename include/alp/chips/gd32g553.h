/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gd32g553.h
 * @brief Host-side driver for the GD32G553MEY7TR supervisor MCU on
 *        the E1M-X V2N / V2N-M1 SoMs.
 *
 * The GD32G553 is the V2N module's general-purpose supervisor MCU.
 * It owns a fleet of pads that don't fit on the Renesas RZ/V2N pinout
 * (notably two of the eight E1M PWM channels post-2026-05-11 schematic
 * revision, several E1M I2C management lines, and the cached DA9292
 * status forwarder).  Host code reaches the supervisor over a
 * **hybrid** transport:
 *
 *   - **SPI fast path** (Renesas RSPI master / GD32 slave on the GD32's
 *     `PA8`/`PA9`/`PA10`/`PB15` pads) -- low-latency, dedicated link.
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
/* and gd32-bridge/src/protocol.c on the firmware side.              */
/* --------------------------------------------------------------- */

/** Start-of-frame marker carried by every SPI frame. */
#define GD32G553_BRIDGE_SOF              0xA5u

/** Virtual "command register" address on the I2C side. */
#define GD32G553_BRIDGE_I2C_REG_CMD      0x00u

/** Default 7-bit I2C slave address (compile-time configurable on the
 *  firmware side; production builds may relocate to dodge address
 *  collisions on a specific carrier). */
#define GD32G553_BRIDGE_DEFAULT_I2C_ADDR 0x70u

/** Default reply timeout in milliseconds for the SPI path (the GD32
 *  is required to populate its TX FIFO inside this window before the
 *  host clocks the reply bytes). */
#define GD32G553_BRIDGE_REPLY_TIMEOUT_MS 10u

/** Maximum number of ADC samples the bridge accepts per `ADC_READ`. */
#define GD32G553_BRIDGE_ADC_MAX_SAMPLES  8u

/** Length of the truncated SHA-1 build-id (ASCII hex, NUL-terminated). */
#define GD32G553_BUILD_ID_LEN            20u

/** Protocol major version the **host driver** speaks.  Hosts whose
 *  major differs from the GD32 firmware's `GET_VERSION` reply refuse
 *  to operate (see docs/gd32-bridge-protocol.md §8). */
#define GD32G553_HOST_PROTOCOL_MAJOR     0u

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
    /* Reserved range 0xF0..0xFF -- application-bootloader OTA. */
    GD32G553_CMD_OTA_BEGIN             = 0xF0,
    GD32G553_CMD_OTA_WRITE_CHUNK       = 0xF1,
    GD32G553_CMD_OTA_VERIFY            = 0xF2,
    GD32G553_CMD_OTA_COMMIT            = 0xF3,
    GD32G553_CMD_OTA_ROLLBACK          = 0xF4,
    GD32G553_CMD_OTA_GET_STATE         = 0xF5,
    GD32G553_CMD_OTA_ABORT             = 0xF6,
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
    GD32G553_RESET_UNKNOWN  = 0,
    GD32G553_RESET_POWER_ON = 1,
    GD32G553_RESET_NRST_PIN = 2,
    GD32G553_RESET_SOFT     = 3,
    GD32G553_RESET_WDT      = 4,
    GD32G553_RESET_BROWNOUT = 5,
    GD32G553_RESET_LOWPOWER = 6,
} gd32g553_reset_cause_t;

/** Firmware version triple returned by `GET_VERSION`. */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} gd32g553_version_t;

/** Driver context.
 *
 *  At least one of @c spi / @c i2c MUST be non-NULL at @ref gd32g553_init
 *  time.  Passing both attaches the context to **both** transports and
 *  routes commands through @c default_transport unless a `*_via` helper
 *  is used.
 */
typedef struct {
    bool                  initialised;
    alp_spi_t            *spi;              /**< NULL if SPI not wired. */
    alp_i2c_t            *i2c;              /**< NULL if I2C not wired. */
    uint8_t               i2c_addr;         /**< Valid when i2c != NULL.*/
    gd32g553_transport_t  default_transport;/**< Picked at init time.    */
    gd32g553_version_t    version;          /**< Cached after init.      */
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
alp_status_t gd32g553_init(gd32g553_t *ctx, alp_spi_t *spi,
                           alp_i2c_t *i2c, uint8_t i2c_addr_7bit);

/** @brief Pick which transport future commands ride by default. */
alp_status_t gd32g553_set_default_transport(gd32g553_t *ctx,
                                            gd32g553_transport_t t);

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
 *  @param build_id  Destination buffer for the NUL-terminated ASCII
 *                   hex string of length up to @ref GD32G553_BUILD_ID_LEN.
 *                   Buffer size MUST be at least
 *                   @ref GD32G553_BUILD_ID_LEN + 1 bytes. */
alp_status_t gd32g553_get_build_id(gd32g553_t *ctx,
                                   char build_id[GD32G553_BUILD_ID_LEN + 1]);

/** @brief Read the cause of the GD32's most recent reset (cleared on
 *         the firmware side after this call). */
alp_status_t gd32g553_get_reset_reason(gd32g553_t *ctx,
                                       gd32g553_reset_cause_t *out);

/** @brief Read a masked subset of the GD32's pad levels.
 *
 *  @param mask    Logical GD32 pad indices the caller cares about.
 *                 Mapping is documented in gd32-bridge/README.md;
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
alp_status_t gd32g553_pwm_set(gd32g553_t *ctx, uint8_t channel,
                              uint32_t period_ns, uint32_t duty_ns);

/** @brief Read back a PWM channel's currently-programmed setpoint. */
alp_status_t gd32g553_pwm_get(gd32g553_t *ctx, uint8_t channel,
                              uint32_t *period_ns, uint32_t *duty_ns);

/** @brief Read @p samples ADC measurements from @p channel.
 *
 *  @param channel  Firmware-defined logical ADC channel index.
 *  @param samples  Number of consecutive measurements
 *                  (1..@ref GD32G553_BRIDGE_ADC_MAX_SAMPLES).  Values
 *                  beyond the maximum are silently capped by the
 *                  firmware.
 *  @param mv       Caller-supplied buffer for the reply.  Length
 *                  MUST be at least @p samples entries; the firmware
 *                  writes one millivolt reading per slot.  */
alp_status_t gd32g553_adc_read(gd32g553_t *ctx, uint8_t channel,
                               uint8_t samples, uint16_t *mv);

/** @brief Read the GD32's cached snapshot of the DA9292's PMC_STATUS_00
 *         byte.  Latency to the actual silicon state is firmware-
 *         implementation-defined (currently ≤ 20 ms). */
alp_status_t gd32g553_da9292_status_forward(gd32g553_t *ctx, uint8_t *status);

/* ------------------------------------------------------------------ */
/* OTA -- in-system upgrade of the bridge firmware                    */
/*                                                                    */
/* Opcodes 0xF0..0xF6 are reserved by the bridge protocol             */
/* (docs/gd32-bridge-protocol.md §10) for the application-bootloader  */
/* upgrade path.  Scaffolds today: the firmware-side handler set      */
/* compiles + dispatches but every body except CMD_OTA_GET_STATE      */
/* replies with STATUS_NOSUPPORT until the FMC integration lands.     */
/* The host helpers below match that contract: every call returns     */
/* ALP_ERR_NOSUPPORT against current-firmware bridges, and ALP_OK     */
/* once the firmware-side bodies ship.  GET_STATE is the exception -- */
/* it answers concretely today so customer telemetry can already      */
/* read the OTA state machine.                                        */
/* ------------------------------------------------------------------ */

/** OTA state-machine snapshot returned by @ref gd32g553_ota_get_state. */
typedef enum {
    GD32G553_OTA_STATE_IDLE           = 0, /**< No upgrade in progress. */
    GD32G553_OTA_STATE_RECEIVING      = 1, /**< Between BEGIN and last chunk. */
    GD32G553_OTA_STATE_VERIFIED       = 2, /**< VERIFY succeeded. */
    GD32G553_OTA_STATE_PENDING_COMMIT = 3, /**< Metadata flip queued. */
    GD32G553_OTA_STATE_FAULT          = 4, /**< Aborted; staging slot dirty. */
} gd32g553_ota_state_t;

/** Slot id mirrors the firmware's bl_slot_id_t. */
typedef enum {
    GD32G553_OTA_SLOT_INVALID = 0u,
    GD32G553_OTA_SLOT_A       = 1u,
    GD32G553_OTA_SLOT_B       = 2u,
} gd32g553_ota_slot_t;

/** Read-only telemetry of the OTA state machine. */
typedef struct {
    gd32g553_ota_state_t state;        /**< @ref gd32g553_ota_state_t. */
    gd32g553_ota_slot_t  active_slot;  /**< Slot the bridge is currently running. */
    gd32g553_ota_slot_t  pending_slot; /**< Slot queued for next-boot commit; INVALID if none. */
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
 * @param chunk_max_bytes  Out: chunk size the firmware accepts in
 *                         a single @ref gd32g553_ota_write_chunk.
 * @param target_slot      Out: slot the bridge will write into.
 *
 * @return ALP_OK / ALP_ERR_NOSUPPORT (firmware lacks the bodies) /
 *         transport error.
 */
alp_status_t gd32g553_ota_begin(gd32g553_t *ctx,
                                uint32_t size_bytes,
                                uint32_t expected_crc32,
                                uint16_t *chunk_max_bytes,
                                gd32g553_ota_slot_t *target_slot);

/**
 * @brief Write one chunk of the payload at @p offset.
 *
 * Chunks may be re-sent (same offset, same data) safely -- the
 * firmware re-writes the destination region without an erase.  Chunks
 * arriving out of order are also safe because the offset is absolute.
 *
 * @param data            Chunk bytes.
 * @param data_len        Chunk byte count.  Must be > 0 and
 *                        <= the `chunk_max_bytes` returned by BEGIN.
 * @param received_bytes  Out: running total the firmware has seen.
 */
alp_status_t gd32g553_ota_write_chunk(gd32g553_t *ctx,
                                      uint32_t offset,
                                      const uint8_t *data,
                                      size_t data_len,
                                      uint32_t *received_bytes);

/**
 * @brief Ask the bridge to re-compute the CRC32 over the staging
 *        slot and compare against the value passed to BEGIN.
 *
 * @param verified        Out: true iff the recomputed CRC32 matched.
 * @param computed_crc32  Out: the CRC32 the bridge actually saw.
 */
alp_status_t gd32g553_ota_verify(gd32g553_t *ctx,
                                 bool *verified,
                                 uint32_t *computed_crc32);

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
 * @param out  Populated on @ref ALP_OK.  May not be NULL.
 */
alp_status_t gd32g553_ota_get_state(gd32g553_t *ctx,
                                    gd32g553_ota_state_info_t *out);

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
