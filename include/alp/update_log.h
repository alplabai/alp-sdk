/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file update_log.h
 * @brief Portable, tamper-evident firmware-update audit log.
 *
 * One surface across SoMs. The software tier (every target today) gives a
 * hash-chained, monotonic-counter-anchored log that detects mutation,
 * truncation, rollback, and reorder. On SoMs with a secure backend the
 * same API is hardware-enforced (TF-M Protected Storage + a non-decrementable
 * monotonic counter). Query @ref alp_update_log_assurance to learn which.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 new. Surface may change until the hardware backend is
 *      silicon-proven. See docs/abi-markers.md.
 */

#ifndef ALP_UPDATE_LOG_H
#define ALP_UPDATE_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h" /* alp_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 digest length used for image hashes + chaining. */
#define ALP_UPDATE_LOG_HASH_LEN 32
/** Max firmware-version string length stored per entry (excl. NUL). */
#define ALP_UPDATE_LOG_FWVER_MAX 31

/** Outcome of an update, as recorded in an entry. */
typedef enum {
	ALP_UPDATE_STATUS_CONFIRMED       = 0, /**< New image booted + confirmed healthy. */
	ALP_UPDATE_STATUS_VERIFY_FAILED   = 1, /**< Signature/hash verification failed. */
	ALP_UPDATE_STATUS_ROLLED_BACK     = 2, /**< Reverted to the previous slot. */
	ALP_UPDATE_STATUS_PENDING_CONFIRM = 3, /**< Booted, awaiting confirm window. */
} alp_update_status_t;

/** How strongly the log is protected on this SoM. */
typedef enum {
	ALP_UPDATE_LOG_SW_TAMPER_EVIDENT = 0, /**< Hash-chain + counter; app-cooperative. */
	ALP_UPDATE_LOG_HW_ENFORCED       = 1, /**< TF-M-isolated store + HW monotonic counter. */
} alp_update_log_assurance_t;

/** Result of walking the chain in @ref alp_update_log_verify. */
typedef enum {
	ALP_UPDATE_LOG_VERIFY_OK           = 0,
	ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN = 1, /**< An entry was mutated or reordered. */
	ALP_UPDATE_LOG_VERIFY_TRUNCATED    = 2, /**< Tail entries are missing. */
	ALP_UPDATE_LOG_VERIFY_ROLLED_BACK  = 3, /**< Store regressed vs the monotonic anchor. */
} alp_update_log_verdict_t;

/**
 * One audit entry. On @ref alp_update_log_append the caller fills
 * everything except @c seq (the engine assigns it from the monotonic
 * counter). On @ref alp_update_log_get every field is populated.
 */
typedef struct {
	uint64_t            seq; /**< Authoritative order; engine-assigned. */
	char                fw_version[ALP_UPDATE_LOG_FWVER_MAX + 1]; /**< NUL-terminated. */
	uint8_t             image_hash[ALP_UPDATE_LOG_HASH_LEN];      /**< SHA-256 of the image. */
	alp_update_status_t status;
	uint64_t            timestamp; /**< Best-effort epoch; 0 = unset. */
} alp_update_log_entry_t;

/** Opaque log handle. Acquire via @ref alp_update_log_open. */
typedef struct alp_update_log alp_update_log_t;

/**
 * @brief Open the device's update log.
 * @return Handle on success; NULL if no backend is present (sets last error).
 */
alp_update_log_t *alp_update_log_open(void);

/**
 * @brief Append one entry. @c seq is assigned by the engine.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_update_log_append(alp_update_log_t *log, const alp_update_log_entry_t *entry);

/**
 * @brief Walk the chain and report integrity.
 * @param      log          The update log handle (from @ref alp_update_log_open).
 * @param[out] verdict_out  Required.
 * @param[out] bad_seq_out  On CHAIN_BROKEN/TRUNCATED, the offending seq. May be NULL.
 * @return ALP_OK if the walk ran (inspect @p verdict_out for the result);
 *         ALP_ERR_IO if the store was unreadable.
 */
alp_status_t alp_update_log_verify(alp_update_log_t *log, alp_update_log_verdict_t *verdict_out,
                                   uint64_t *bad_seq_out);

/** @brief Number of entries. */
alp_status_t alp_update_log_count(alp_update_log_t *log, uint64_t *count_out);

/** @brief Fetch the entry at @p seq. ALP_ERR_NOT_FOUND if absent. */
alp_status_t alp_update_log_get(alp_update_log_t *log, uint64_t seq,
                                alp_update_log_entry_t *entry_out);

/** @brief Assurance level on this SoM. */
alp_update_log_assurance_t alp_update_log_assurance(const alp_update_log_t *log);

/** @brief Release the handle. */
void alp_update_log_close(alp_update_log_t *log);

#ifdef __cplusplus
}
#endif

#endif /* ALP_UPDATE_LOG_H */
