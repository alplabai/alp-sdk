/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gd32_swd.h
 * @brief Bit-bang SWD controller for flashing the GD32G553 over GPIO.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The companion GD32G553MEY7TR on the E1M-X V2N / V2N-M1 SoMs is
 * a Cortex-M33 with the standard Arm Coresight SWD debug port.
 * Per the 2026-05-12 hardware decision the V2N board routes
 * `GD32_SWDIO` + `GD32_SWCLK` + `GD32_NRST` from the Renesas RZ/V2N
 * host back to the GD32 so the host can reflash the supervisor MCU
 * in the field without an external probe.
 *
 * @par Driver status: PARTIAL
 *
 * Packet layer + DPIDR read + Cortex-M33 halt + FMC erase/write/verify
 * + reset-and-run coded and ready for first-silicon exercise.  Real
 * verification tracked in `docs/test-plan.md`.
 *
 * @par Pin model on V2N
 *
 * | Signal       | Renesas pad | GD32 pad | Notes                                      |
 * |--------------|-------------|----------|--------------------------------------------|
 * | `GD32_SWDIO` | `P70`       | `PA13`   | bidirectional; was GPT0_GTIOC0A / PWM2     |
 * | `GD32_SWCLK` | `P71`       | `PA14`   | host drives; was GPT0_GTIOC0B / PWM3       |
 * | `GD32_NRST`  | `P74`       | `NRST`   | open-drain; shared with PMIC reset out     |
 *
 * The caller opens three `alp_gpio_t` handles, hands them to
 * `gd32_swd_init`, and retains ownership: `gd32_swd_deinit` does not
 * close them.  `SWDIO` switches between input and output transparently
 * inside the driver during data phases.
 *
 * @par Reference
 *
 * Arm DDI 0316C "ARM Debug Interface v5" + Arm DUI 0552A "Coresight
 * DAP Bit-bang Algorithms" specify the wire protocol; both are
 * publicly available from Arm's developer docs.
 */

#ifndef ALP_CHIPS_GD32_SWD_H
#define ALP_CHIPS_GD32_SWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Expected SW-DP IDCODE for the GD32G553 (Cortex-M33 r0p1 SW-DPv2).
 *  Boards can match against this value in production test. */
#define GD32_SWD_EXPECTED_IDCODE          0x6BA02477u

/** Default clock-delay loop count.  Higher = slower SWCLK. */
#define GD32_SWD_DEFAULT_CLOCK_DELAY      4u

/** Maximum number of retry rounds for `ACK_WAIT` from the target. */
#define GD32_SWD_MAX_WAIT_RETRIES         16u

/** GD32G553 flash base address (per the GD32G553 datasheet). */
#define GD32_SWD_FMC_FLASH_BASE           0x08000000u

/** Sector size on the GD32G553 in bytes. */
#define GD32_SWD_FMC_SECTOR_BYTES         2048u

/** SRAM base, used internally to stage operations. */
#define GD32_SWD_FMC_SRAM_BASE            0x20000000u

/** ACK response (3-bit field returned by the target after every
 *  request).  Values match the Arm ADIv5 specification. */
typedef enum {
    GD32_SWD_ACK_OK    = 0x1,
    GD32_SWD_ACK_WAIT  = 0x2,
    GD32_SWD_ACK_FAULT = 0x4,
    GD32_SWD_ACK_PROTO = 0x7,
} gd32_swd_ack_t;

/** Driver context. */
typedef struct {
    bool          initialised;
    bool          connected;
    alp_gpio_t   *swdio;
    alp_gpio_t   *swclk;
    alp_gpio_t   *nrst;
    uint32_t      clock_delay;
    uint32_t      idcode;
    bool          swdio_is_output;
} gd32_swd_t;

/**
 * @brief Bind the controller to caller-supplied GPIO handles.
 *
 * Configures `swdio` + `swclk` as outputs driven to the SWD idle
 * state, and -- when `nrst` is non-NULL -- releases the reset line.
 * Ownership of the handles stays with the caller.
 *
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL on NULL ctx /
 *         swdio / swclk, or the @ref alp_gpio_write error from the
 *         idle-state configuration.
 */
alp_status_t gd32_swd_init(gd32_swd_t *ctx,
                           alp_gpio_t *swdio,
                           alp_gpio_t *swclk,
                           alp_gpio_t *nrst);

/** Override the per-half-bit clock-delay loop count.  Clamped to
 *  [0, 2048]. */
alp_status_t gd32_swd_set_clock_delay(gd32_swd_t *ctx, uint32_t delay_spins);

/**
 * @brief Perform line-reset + JTAG-to-SWD switch + DPIDR read.
 *
 * On success the IDCODE is cached in @ref gd32_swd_t::idcode.
 */
alp_status_t gd32_swd_connect(gd32_swd_t *ctx);

/** Halt the Cortex-M33 cleanly via the Debug Halting Control and
 *  Status Register.  Must be called before any flash op. */
alp_status_t gd32_swd_halt(gd32_swd_t *ctx);

/**
 * @brief Erase the smallest enclosing range of flash sectors that
 *        covers @p addr .. @p addr + @p size - 1.
 *
 * Address + size are rounded out to sector boundaries
 * (@ref GD32_SWD_FMC_SECTOR_BYTES).
 */
alp_status_t gd32_swd_flash_erase(gd32_swd_t *ctx,
                                  uint32_t addr,
                                  uint32_t size);

/** Program @p len bytes from @p data into flash starting at @p addr.
 *  Destination must be erased.  Doubleword-aligned. */
alp_status_t gd32_swd_flash_write(gd32_swd_t *ctx,
                                  uint32_t addr,
                                  const uint8_t *data,
                                  size_t len);

/** Read @p len bytes from flash starting at @p addr and compare
 *  against @p data.  Returns @ref ALP_OK on full match. */
alp_status_t gd32_swd_flash_verify(gd32_swd_t *ctx,
                                   uint32_t addr,
                                   const uint8_t *data,
                                   size_t len);

/** Release the core from debug halt + issue a system reset.  Uses
 *  the hardware @c NRST line when wired, otherwise the standard
 *  AIRCR.SYSRESETREQ + VECTKEY write. */
alp_status_t gd32_swd_reset_and_run(gd32_swd_t *ctx);

/** Release the driver context.  Does NOT close the GPIO handles. */
void         gd32_swd_deinit(gd32_swd_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GD32_SWD_H */
