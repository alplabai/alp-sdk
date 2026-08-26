/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file i3c.h
 * @brief Alp SDK I3C (MIPI I3C Basic) controller abstraction.
 *
 * Deliberately mirrors the alp_i2c_* surface in <alp/peripheral.h>: open a
 * bus by form-factor ID, then blocking write / read / write_read against a
 * target address.  An I2C caller ports by renaming the prefix.
 *
 * Addressing: @p addr is the target's DYNAMIC address, assigned by dynamic
 * address assignment (DAA) during bus init (or the static address of a
 * legacy I2C device declared on the bus).  Backends resolve the address to
 * the underlying device descriptor; an address no target answers to is
 * ALP_ERR_IO.
 *
 * @warning A target MUST be declared as a child node of the controller in
 *          devicetree.  The Zephyr backend resolves @p addr against the
 *          controller's attached-device list, which the driver builds from
 *          DT children -- a physically present but DT-undeclared target has
 *          no descriptor, so every op returns ALP_ERR_IO WITHOUT toggling a
 *          bus line.  That is indistinguishable from a NACK at this API, so
 *          if a soldered target answers nothing, check the devicetree first.
 *          Timing (SCL rate) is devicetree-owned, not a config field --
 *          the legal rate on a mixed I3C/I2C bus depends on the slowest
 *          device populated, which is a board fact, not a per-open() choice.
 *
 * Legacy I2C devices: a legacy (non-I3C) target sharing this bus is NOT
 * driven through this handle.  It rides the existing alp_i2c_* surface via
 * the board's alp-i2cN alias pointed at the same controller node -- Zephyr's
 * i3c_driver_api mandates an i2c_api as its first member for exactly this
 * reason (i3c_dw.c implements both).  See metadata/boards/e1m-evk.yaml for
 * the established house pattern (E1M_I3C0 doubling as EVK_I2C_BUS_ARDUINO).
 *
 * NOT in this surface yet, deliberately: in-band interrupts (IBI), raw CCC
 * escape hatch, and an explicit re-run-DAA call.  Each would freeze ABI on a
 * controller that has never moved a bit on real E8 silicon.  Planned as
 * additive extensions once the E8 path is bench-verified.
 *
 * Backends:
 *   - Zephyr   : upstream `i3c_*` driver class (Synopsys DesignWare
 *                i3c_dw.c, "snps,designware-i3c") via DT alias alp-i3c0.
 *   - Yocto    : a bus PRESENCE check only (src/backends/i3c/yocto_drv.c,
 *                issue #1147) -- confirms the controller exists under
 *                /sys/bus/i3c/devices/i3c-N (N == bus_id).  write() / read() /
 *                write_read() stay ALP_ERR_NOSUPPORT on every mainline
 *                kernel: unlike I2C's ioctl(I2C_RDWR), Linux has NO
 *                generic userspace raw-transfer ABI for I3C at all --
 *                the subsystem is kernel-driver-bind-only.
 *   - Baremetal: none yet; sw_fallback.
 *
 * Typical usage:
 * @code
 *     alp_i3c_t *bus = alp_i3c_open(&(alp_i3c_config_t){
 *         .bus_id = ALP_E1M_I3C0,
 *     });
 *     uint8_t reg = 0x00u, val = 0u;
 *     alp_i3c_write_read(bus, 0x08u, &reg, 1u, &val, 1u);
 *     alp_i3c_close(bus);
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New class.  Controller init is BENCH-PROVEN on E1M-AEN801 silicon
 *      (Flow C ITCM RAM-run, 2026-07-25):
 *      lpi3c0 binds, `device_is_ready()` passes, and `alp_i3c_open()`
 *      returns a handle -- so the `ALIF_LPI3C_CLK` clock-id and the
 *      P7_6/P7_7 fn3 pinctrl are confirmed correct, which was the risk
 *      this marker was raised for.
 *
 *      A LIVE TRANSFER IS STILL UNPROVEN: no I3C target is populated on
 *      this bench carrier, so DAA finds zero targets and a probe write
 *      returns ALP_ERR_IO by design.  The write/read/write_read paths are
 *      therefore reached but never acknowledged by a real device.
 *      Surface may change until promoted.  Promotion gate: a live transfer
 *      against a populated I3C target.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_I3C_H
#define ALP_I3C_H

#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque I3C bus handle.  Allocate via @ref alp_i3c_open. */
typedef struct alp_i3c alp_i3c_t;

/** Configuration passed to @ref alp_i3c_open. */
typedef struct {
	uint32_t bus_id; /**< Form-factor I3C instance ID: ALP_E1M_I3C0. */
} alp_i3c_config_t;

/**
 * @brief Default-initialize an @ref alp_i3c_config_t for bus @p id.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  On a compiler that
 *       rejects compound literals in C++ (e.g. MSVC), initialize the
 *       config's fields individually.
 */
#define ALP_I3C_CONFIG_DEFAULT(id) ((alp_i3c_config_t){ .bus_id = (id) })

/**
 * @brief Acquire an I3C bus handle.
 *
 * Initializes the bus controller.  Dynamic address assignment for any
 * declared targets is run by the backend at bus init (Zephyr's i3c_dw.c
 * runs DAA during device init when targets are declared in devicetree).
 *
 * @param[in] cfg  Bus configuration.  Must be non-NULL.
 *
 * @return Open handle on success, or NULL on failure with
 *         @ref alp_last_error set to:
 *           @ref ALP_ERR_INVAL (@p cfg is NULL; or @c bus_id out of range,
 *             >= the ACTIVE SoC's I3C count, ALP_SOC_I3C_COUNT -- not the
 *             form-factor count, which may be smaller);
 *           @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC (no I3C backend resolves
 *             for the active SoM);
 *           @ref ALP_ERR_NOT_IMPLEMENTED (the selected backend declares
 *             no open op -- not reachable through any shipped backend
 *             today);
 *           @ref ALP_ERR_NOT_READY (underlying controller not ready);
 *           @ref ALP_ERR_NOMEM (the handle pool is exhausted --
 *             CONFIG_ALP_SDK_MAX_I3C_HANDLES buses already open);
 *           @ref ALP_ERR_NOSUPPORT (Zephyr backend only, and only when
 *             built WITHOUT CONFIG_I3C_CONTROLLER -- either CONFIG_I3C=n,
 *             or CONFIG_I3C=y with CONFIG_I3C_TARGET_ROLE_ONLY selected --
 *             no controller role compiled in, so every op including
 *             open() is unsupported.  This is the DEFAULT open() outcome
 *             on any Zephyr build that doesn't opt into CONFIG_I3C_DUAL_ROLE
 *             or CONFIG_I3C_CONTROLLER_ROLE_ONLY, not a corner case).
 */
alp_i3c_t *alp_i3c_open(const alp_i3c_config_t *cfg);

/**
 * @brief Blocking private write to a target.
 *
 * @param[in] bus   Handle from @ref alp_i3c_open.
 * @param[in] addr  Target's dynamic address (or legacy static address).
 * @param[in] data  Source bytes.
 * @param[in] len   Byte count.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO (NACK, or no DT-declared target at @p addr -- see the
 *         addressing @warning above) / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_i3c_write(alp_i3c_t *bus, uint8_t addr, const uint8_t *data, size_t len);

/**
 * @brief Blocking private read from a target.
 *
 * @param[in]  bus   Handle from @ref alp_i3c_open.
 * @param[in]  addr  Target's dynamic address (or legacy static address).
 * @param[out] data  Destination buffer.
 * @param[in]  len   Byte count to read.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO (NACK, or no DT-declared target at @p addr -- see the
 *         addressing @warning above) / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_i3c_read(alp_i3c_t *bus, uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief Write-then-read (typical register-read idiom).
 *
 * Issues a write phase followed by a repeated START + read phase with no
 * STOP in between -- the canonical "read register N" pattern, byte-
 * identical in shape to @ref alp_i2c_write_read.  Implemented as a single
 * backend transfer (two chained messages), not two separate calls: a
 * two-call write-then-read inserts a STOP many targets do not tolerate.
 *
 * @param[in]  bus    Handle from @ref alp_i3c_open.
 * @param[in]  addr   Target's dynamic address (or legacy static address).
 * @param[in]  wdata  Bytes to write (typically register address).
 * @param[in]  wlen   Write length.
 * @param[out] rdata  Receive buffer.
 * @param[in]  rlen   Read length.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO (NACK, or no DT-declared target at @p addr -- see the
 *         addressing @warning above) / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_i3c_write_read(alp_i3c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *wdata,
                                size_t         wlen,
                                uint8_t       *rdata,
                                size_t         rlen);

/**
 * @brief Release the I3C bus handle.  Idempotent on NULL.
 *
 * Does not power down the controller or revoke assigned dynamic addresses.
 *
 * @param[in] bus  Handle from @ref alp_i3c_open, or NULL.
 */
void alp_i3c_close(alp_i3c_t *bus);

/**
 * @brief Query the capabilities of an opened I3C bus handle.
 *
 * @param bus  Handle from @ref alp_i3c_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p bus is NULL.
 */
const alp_capabilities_t *alp_i3c_capabilities(const alp_i3c_t *bus);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_I3C_H */
