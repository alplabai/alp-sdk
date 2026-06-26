/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file model.h
 * @brief Read-side parser for the .alpmodel package (header + CBOR manifest).
 *
 * Parses an in-memory .alpmodel into a bounded, stack-friendly view: the
 * model identity, its per-backend target table, and blob slices. No malloc;
 * the manifest is decoded once into a caller-provided @ref alp_model_t.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 */
#ifndef ALP_MODEL_H
#define ALP_MODEL_H

#include <stdint.h>
#include <stddef.h>
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALP_MODEL_MAGIC       "ALPM" /**< Container magic at offset 0. */
#define ALP_MODEL_CONTAINER_V 1u     /**< Container version this reader understands. */
#define ALP_MODEL_MAX_TARGETS 8u     /**< Max per-backend blob entries parsed. */
#define ALP_MODEL_STR_MAX     32u    /**< Fixed string field length, incl. NUL. */

/** One compiled-blob target entry from the manifest. */
typedef struct {
	char     backend[ALP_MODEL_STR_MAX];      /**< "cpu" | "ethos_u" | "drpai" | "deepx_dxm1". */
	char     silicon_ref[ALP_MODEL_STR_MAX];  /**< Silicon match, e.g. "alif:ensemble:e8" or "*". */
	char     blob_format[ALP_MODEL_STR_MAX];  /**< Backend-specific blob format tag. */
	char     accel_config[ALP_MODEL_STR_MAX]; /**< Accelerator config tag; "" when N/A. */
	uint32_t arena_bytes;                     /**< Scratch arena the blob needs, in bytes. */
	uint32_t req_sram_kib;                    /**< Required device NPU/SRAM envelope, in KiB. */
	const uint8_t *blob;                      /**< Blob bytes; points into the source buffer. */
	uint32_t       blob_len;                  /**< Blob length in bytes. */
} alp_model_target_t;

/** Parsed view of an .alpmodel (no heap; references the source buffer). */
typedef struct {
	const uint8_t     *data;                    /**< Source buffer; kept and owned by the caller. */
	size_t             size;                    /**< Source buffer length in bytes. */
	char               name[ALP_MODEL_STR_MAX]; /**< Model name from the manifest. */
	uint8_t            src_sha[32];             /**< SHA-256 of the source model. */
	uint16_t           flags;                   /**< Manifest flag bits (reserved). */
	uint32_t           n_targets;               /**< Valid entries in @c targets. */
	alp_model_target_t targets[ALP_MODEL_MAX_TARGETS]; /**< Per-backend blob table. */
} alp_model_t;

/**
 * @brief Parse an in-memory .alpmodel package.
 *
 * @param[in]  data  Package bytes (must outlive @p out — blobs reference it).
 * @param[in]  size  Byte count.
 * @param[out] out   Filled on ALP_OK. Must be non-NULL.
 * @return ALP_OK; ALP_ERR_INVAL (bad magic / truncated / CBOR error);
 *         ALP_ERR_VERSION (container version newer than this reader).
 */
alp_status_t alp_model_parse(const uint8_t *data, size_t size, alp_model_t *out);

#ifdef __cplusplus
}
#endif
#endif /* ALP_MODEL_H */
