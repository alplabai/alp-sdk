/* SPDX-License-Identifier: Apache-2.0
 * Pure, dependency-free update-log engine. NOT a public header. */
#ifndef ALP_UPDATE_LOG_ENGINE_H
#define ALP_UPDATE_LOG_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "alp/update_log.h"
#include "update_log/store.h"

#define ULOG_VERSION        1u
#define ULOG_ENTRY_WIRE_LEN 115u
#define ULOG_META_WIRE_LEN  46u
#define ULOG_META_MAGIC     0x554C4F47u /* 'ULOG' */

/* Highest sequence number the engine will ever assign (18 nines). kbuf()
 * renders "ulog.<seq>" into a 24-byte key buffer ("ulog." + up to 18 digits
 * + NUL); bounding seq here keeps every rendered key inside that buffer
 * instead of silently truncating once seq grows past ~10^18. Also leaves
 * headroom below UINT64_MAX so the monotonic counter can never wrap.
 * ulog_engine_append() reports ALP_ERR_NOMEM once the log reaches this
 * bound -- the log stops accepting new entries, but never wraps or
 * truncates a key. */
#define ULOG_SEQ_MAX ((uint64_t)999999999999999999ull)

/* Wire (de)serialisation. prev_hash is the chaining link, kept out of the
 * public entry struct. */
alp_status_t ulog_entry_encode(const alp_update_log_entry_t *e,
                               const uint8_t                 prev_hash[32],
                               uint8_t                       out[ULOG_ENTRY_WIRE_LEN]);
alp_status_t ulog_entry_decode(const uint8_t          *buf,
                               size_t                  len,
                               alp_update_log_entry_t *e_out,
                               uint8_t                 prev_hash_out[32]);

struct ulog_meta {
	uint64_t count;
	uint8_t  head_hash[32];
};
alp_status_t ulog_meta_encode(const struct ulog_meta *m, uint8_t out[ULOG_META_WIRE_LEN]);
alp_status_t ulog_meta_decode(const uint8_t *buf, size_t len, struct ulog_meta *m_out);

/* Engine ops over the two seams (Tasks 2-4). */
alp_status_t ulog_engine_append(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                const alp_update_log_entry_t   *entry);
alp_status_t ulog_engine_verify(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                alp_update_log_verdict_t       *verdict_out,
                                uint64_t                       *bad_seq_out);
alp_status_t ulog_engine_count(const alp_secure_store_if      *store,
                               const alp_monotonic_counter_if *ctr,
                               uint64_t                       *count_out);
alp_status_t
ulog_engine_get(const alp_secure_store_if *store, uint64_t seq, alp_update_log_entry_t *e_out);

#endif /* ALP_UPDATE_LOG_ENGINE_H */
