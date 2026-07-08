/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file optiga_trust_m.h
 * @brief Infineon OPTIGA Trust M secure-element driver
 *
 * @par Driver scope: [PROBE-ONLY] -- driver compiles, validates its public
 *   arguments, and probes the chip by reading I2C_STATE.  Product-info
 *   and raw-APDU entry points intentionally return ALP_ERR_NOSUPPORT
 *   until the Infineon host-library transport is integrated.
 *        (SLS32AIA010MLUSON10XTMA2).
 *
 * Hardware security IC providing ECC-256/384/521, RSA-1k/2k,
 * AES-128/192/256, SHA-256, TRNG, and 10 KB user NVM with secure
 * key/object storage.  On the E1M-AEN module the chip sits on
 * Alif's LPI2C bus alongside the TMP112 + RV-3028-C7 RTC.
 *
 * Default I2C address: **0x30** (7-bit, configurable via
 * provisioning).
 *
 * v0.3 driver scope.  This header surfaces the lifecycle bits a
 * developer needs to confirm the part is wired correctly.  `init()`
 * reads I2C_STATE only; it does not send OPEN_APPLICATION.  Product
 * info (`GET_DATA_OBJECT Coprocessor UID`) and raw APDU transport are
 * declared so callers can compile against the planned surface, but the
 * implementation returns ALP_ERR_NOSUPPORT after argument validation.
 *
 * The full APDU command set -- key generation, TLS handshake handler,
 * ECDSA, AES wrapping, SHA, secure NVM read/write -- lands only after
 * Infineon's **OPTIGA Trust M Host Library** is integrated as a Zephyr
 * module.  At that point the cleanest architectural fit is registering
 * OPTIGA's PSA driver with `<alp/security.h>`'s MbedTLS PSA wrapper, so
 * apps that call alp_aead_open / etc. pick up hardware acceleration
 * transparently.
 */

#ifndef ALP_CHIPS_OPTIGA_TRUST_M_H
#define ALP_CHIPS_OPTIGA_TRUST_M_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPTIGA_TRUST_M_I2C_ADDR 0x30u

/** Coprocessor product info as returned by GET_DATA_OBJECT 0xE0C2.
 *  Fields are little-endian on the wire; the parsed shape lives
 *  in this struct.  See SRM table 38. */
typedef struct {
	uint8_t chip_type[6]; /**< Chip type number. */
	uint8_t fw_id[2];     /**< Firmware identifier. */
	uint8_t fw_build[2];  /**< Firmware build number. */
	uint8_t reserved[10];
} optiga_trust_m_product_info_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
} optiga_trust_m_t;

/** @brief Probe the chip's I2C_STATE register.
 *
 *  Returns ALP_ERR_NOT_READY if the chip doesn't ACK on its I2C
 *  address (mis-strap / not populated).  Does not open a Trust M
 *  application session; APDU transport is not implemented yet. */
alp_status_t optiga_trust_m_init(optiga_trust_m_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/** @brief Planned product-info read (GET_DATA_OBJECT 0xE0C2).
 *
 *  Current implementation validates @p ctx and @p out, then returns
 *  ALP_ERR_NOSUPPORT because product info needs the full APDU transport.
 */
alp_status_t optiga_trust_m_read_product_info(optiga_trust_m_t              *ctx,
                                              optiga_trust_m_product_info_t *out);

/**
 * @brief Send a raw APDU command frame and read the response.
 *
 * Planned raw-APDU escape hatch for crypto operations the MbedTLS PSA
 * wrapper has not picked up yet.  Current implementation validates
 * pointers, lengths, and initialisation state, then returns
 * ALP_ERR_NOSUPPORT because the Trust M data-link/info-pack transport
 * is not implemented in-tree.
 *
 * @param[in]  ctx         OPTIGA Trust M context (must be initialised first).
 * @param[in]  apdu        Bytes of the command APDU.
 * @param[in]  apdu_len    APDU length.
 * @param[out] resp        Response buffer.
 * @param[in]  resp_cap    Response buffer capacity.
 * @param[out] resp_len    Receives bytes copied into @p resp.
 * @param[in]  timeout_ms  Max wait for the chip to clock out the response.
 */
alp_status_t optiga_trust_m_send_apdu(optiga_trust_m_t *ctx,
                                      const uint8_t    *apdu,
                                      size_t            apdu_len,
                                      uint8_t          *resp,
                                      size_t            resp_cap,
                                      size_t           *resp_len,
                                      uint32_t          timeout_ms);

/** @brief Close the application context + release I2C resources. */
void optiga_trust_m_deinit(optiga_trust_m_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OPTIGA_TRUST_M_H */
