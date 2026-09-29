/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file optiga_trust_m.h
 * @brief Infineon OPTIGA Trust M secure-element driver
 *
 * @par Driver scope: [PARTIAL] -- probe, Coprocessor UID and a raw APDU
 *   session over Infineon's host library (vendors/optiga-trust-m).  Key,
 *   crypto and NVM commands are reachable through the raw APDU session
 *   only; typed wrappers and the PSA driver hook are not written yet.
 *        (SLS32AIA010MLUSON10XTMA2).
 *
 * Hardware security IC providing ECC-256/384/521, RSA-1k/2k,
 * AES-128/192/256, SHA-256, TRNG, and 10 KB user NVM with secure
 * key/object storage.
 *
 * Populated on E1M-X V2N / V2M at 0x30 on BRD_I2C, with the RZ/V2N as
 * bus master (`metadata/e1m_modules/E1M-V2N101.yaml`) -- that is the
 * target this driver is written for.  E1M-AEN801 carries the same
 * footprint on its own BRD_I2C (SoC I2C0, Alif as bus master -- #1848,
 * corrected from an earlier belief that this bus was the slave-only
 * LPI2C0), but is not a usable target on the current bench batch: the
 * part is DNI there (see `docs/bring-up-aen.md` section 5.1;
 * `examples/aen/aen-secure-element-sign` exercises this driver on AEN).
 *
 * Default I2C address: **0x30** (7-bit, configurable via
 * provisioning).
 *
 * The host library is the transport: this driver does not reimplement
 * the IFX I2C data-link / transport stack.  It runs through a PAL on the
 * portable alp surface (alp_i2c + alp_uptime_ms), so the same code runs
 * on the A55 and an MCU core.  The library holds one IFX I2C instance, so
 * an image drives one Trust M.  Shielded Connection (link encryption) is
 * off until binding-secret provisioning is designed (#1164).
 *
 * A PSA driver registered with `<alp/security.h>`'s MbedTLS wrapper is
 * the intended route for apps to use the chip's crypto transparently.
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

/** Coprocessor UID as returned by GET_DATA_OBJECT 0xE0C2: 27 bytes, in
 *  wire order.  Multi-byte fields are big-endian. */
typedef struct {
	uint8_t cim_id;         /**< CIM identifier. */
	uint8_t platform_id;    /**< Platform identifier. */
	uint8_t model_id;       /**< Model identifier. */
	uint8_t rom_mask_id[2]; /**< ROM mask identifier. */
	uint8_t chip_type[6];   /**< Chip type. */
	uint8_t batch_num[6];   /**< Production batch number. */
	uint8_t x_coord[2];     /**< Die X coordinate on the wafer. */
	uint8_t y_coord[2];     /**< Die Y coordinate on the wafer. */
	uint8_t fw_id[4];       /**< Firmware identifier. */
	uint8_t esw_build[2];   /**< Embedded-software build number. */
} optiga_trust_m_product_info_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
	/* Host-library session state; private to the driver. */
	void             *util;
	void             *comms;
	uint8_t           session;
	volatile uint16_t op_status;
} optiga_trust_m_t;

/** @brief Probe the chip's I2C_STATE register.
 *
 *  Returns ALP_ERR_NOT_READY if the chip doesn't ACK on its I2C
 *  address (mis-strap / not populated).  Does not open a Trust M
 *  application session; the first product-info read or APDU does. */
alp_status_t optiga_trust_m_init(optiga_trust_m_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/** @brief Read the Coprocessor UID (GET_DATA_OBJECT 0xE0C2).
 *
 *  Opens the Trust M application on first use, closing a raw APDU
 *  session if one is open.
 *
 *  @return ALP_OK, ALP_ERR_NOT_READY (not initialised), ALP_ERR_INVAL
 *          (@p out NULL), ALP_ERR_TIMEOUT or ALP_ERR_IO (the chip or
 *          link failed), ALP_ERR_NOMEM (host-library instance).
 */
alp_status_t optiga_trust_m_read_product_info(optiga_trust_m_t              *ctx,
                                              optiga_trust_m_product_info_t *out);

/**
 * @brief Send a raw APDU command frame and read the response.
 *
 * Escape hatch for commands without a typed wrapper.  Runs on the host
 * library's comms layer: the first call opens a fresh link (soft reset,
 * application closed), so the caller sends OpenApplication itself and
 * owns the APDU sequence from there.  The session stays open across calls
 * until a product-info read or deinit() closes it.
 *
 * @param[in]  ctx         OPTIGA Trust M context (must be initialised first).
 * @param[in]  apdu        Bytes of the command APDU.
 * @param[in]  apdu_len    APDU length.
 * @param[out] resp        Response buffer.
 * @param[in]  resp_cap    Response buffer capacity.
 * @param[out] resp_len    Receives bytes copied into @p resp.
 * @param[in]  timeout_ms  Max wait for the chip to clock out the response.
 *
 * @return ALP_OK, ALP_ERR_NOT_READY (not initialised), ALP_ERR_INVAL
 *         (bad pointer or length), ALP_ERR_TIMEOUT, ALP_ERR_IO,
 *         ALP_ERR_NOMEM.
 */
alp_status_t optiga_trust_m_send_apdu(optiga_trust_m_t *ctx,
                                      const uint8_t    *apdu,
                                      size_t            apdu_len,
                                      uint8_t          *resp,
                                      size_t            resp_cap,
                                      size_t           *resp_len,
                                      uint32_t          timeout_ms);

/** @brief Close any open session and free the host-library instances. */
void optiga_trust_m_deinit(optiga_trust_m_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_OPTIGA_TRUST_M_H */
