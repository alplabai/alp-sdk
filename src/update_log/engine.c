/* SPDX-License-Identifier: Apache-2.0
 * Pure update-log engine. No Zephyr/PSA includes -- builds on host. */
#include <string.h>

#include "update_log/engine.h"
#include "update_log/sha256.h"

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}
static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

/* Bounded string length -- strnlen is POSIX, not C99, and the engine must
 * stay libc-variant-portable (Zephyr's minimal libc omits it). */
static size_t bounded_len(const char *s, size_t cap)
{
	size_t n = 0;
	while (n < cap && s[n] != '\0')
		n++;
	return n;
}

alp_status_t ulog_entry_encode(const alp_update_log_entry_t *e, const uint8_t prev_hash[32],
                               uint8_t out[ULOG_ENTRY_WIRE_LEN])
{
	if (e == NULL || prev_hash == NULL || out == NULL) return ALP_ERR_INVAL;
	size_t vlen = bounded_len(e->fw_version, ALP_UPDATE_LOG_FWVER_MAX + 1);
	if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
	memset(out, 0, ULOG_ENTRY_WIRE_LEN);
	put_u16(out + 0, (uint16_t)ULOG_VERSION);
	put_u64(out + 2, e->seq);
	out[10] = (uint8_t)e->status;
	put_u64(out + 11, e->timestamp);
	out[19] = (uint8_t)vlen;
	memcpy(out + 20, e->fw_version, vlen);
	memcpy(out + 51, prev_hash, 32);
	memcpy(out + 83, e->image_hash, 32);
	return ALP_OK;
}

alp_status_t ulog_entry_decode(const uint8_t *buf, size_t len, alp_update_log_entry_t *e_out,
                               uint8_t prev_hash_out[32])
{
	if (buf == NULL || e_out == NULL) return ALP_ERR_INVAL;
	if (len < ULOG_ENTRY_WIRE_LEN) return ALP_ERR_INVAL;
	if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
	uint8_t vlen = buf[19];
	if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
	memset(e_out, 0, sizeof(*e_out));
	e_out->seq       = get_u64(buf + 2);
	e_out->status    = (alp_update_status_t)buf[10];
	e_out->timestamp = get_u64(buf + 11);
	memcpy(e_out->fw_version, buf + 20, vlen); /* zero-padded -> NUL-terminated */
	memcpy(e_out->image_hash, buf + 83, 32);
	if (prev_hash_out != NULL) memcpy(prev_hash_out, buf + 51, 32);
	return ALP_OK;
}

alp_status_t ulog_meta_encode(const struct ulog_meta *m, uint8_t out[ULOG_META_WIRE_LEN])
{
	if (m == NULL || out == NULL) return ALP_ERR_INVAL;
	memset(out, 0, ULOG_META_WIRE_LEN);
	put_u16(out + 0, (uint16_t)ULOG_VERSION);
	put_u32(out + 2, ULOG_META_MAGIC);
	put_u64(out + 6, m->count);
	memcpy(out + 14, m->head_hash, 32);
	return ALP_OK;
}

alp_status_t ulog_meta_decode(const uint8_t *buf, size_t len, struct ulog_meta *m_out)
{
	if (buf == NULL || m_out == NULL) return ALP_ERR_INVAL;
	if (len < ULOG_META_WIRE_LEN) return ALP_ERR_INVAL;
	if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
	if (get_u32(buf + 2) != ULOG_META_MAGIC) return ALP_ERR_INVAL;
	m_out->count = get_u64(buf + 6);
	memcpy(m_out->head_hash, buf + 14, 32);
	return ALP_OK;
}

static void kbuf(char *out, size_t cap, uint64_t seq)
{
	/* "ulog.<seq>" decimal. */
	int      n = 0;
	char     tmp[24];
	uint64_t v = seq;
	do {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v && n < 20);
	size_t      pos = 0;
	const char *pfx = "ulog.";
	while (*pfx && pos < cap - 1)
		out[pos++] = *pfx++;
	while (n > 0 && pos < cap - 1)
		out[pos++] = tmp[--n];
	out[pos] = 0;
}

alp_status_t ulog_engine_append(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                const alp_update_log_entry_t   *entry)
{
	if (store == NULL || ctr == NULL || entry == NULL) return ALP_ERR_INVAL;

	uint64_t     hw = 0;
	alp_status_t rc = ctr->read(ctr->ctx, 0, &hw);
	if (rc != ALP_OK) return rc;

	uint8_t prev[32];
	memset(prev, 0, sizeof(prev));
	if (hw > 0) {
		uint8_t metabuf[ULOG_META_WIRE_LEN];
		size_t  mlen = 0;
		rc           = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
		if (rc != ALP_OK) return rc;
		struct ulog_meta m;
		rc = ulog_meta_decode(metabuf, mlen, &m);
		if (rc != ALP_OK) return rc;
		memcpy(prev, m.head_hash, 32);
	}

	alp_update_log_entry_t e = *entry;
	e.seq                    = hw;
	uint8_t wire[ULOG_ENTRY_WIRE_LEN];
	rc = ulog_entry_encode(&e, prev, wire);
	if (rc != ALP_OK) return rc;

	char key[24];
	kbuf(key, sizeof(key), hw);
	rc = store->put(store->ctx, key, wire, sizeof(wire));
	if (rc != ALP_OK) return rc;

	struct ulog_meta nm;
	nm.count = hw + 1u;
	ulog_sha256(wire, sizeof(wire), nm.head_hash);
	uint8_t metaout[ULOG_META_WIRE_LEN];
	(void)ulog_meta_encode(&nm, metaout);
	rc = store->put(store->ctx, "ulog.meta", metaout, sizeof(metaout));
	if (rc != ALP_OK) return rc;

	uint64_t newhw = 0;
	return ctr->increment(ctr->ctx, 0, &newhw);
}

alp_status_t ulog_engine_verify(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                alp_update_log_verdict_t *verdict_out, uint64_t *bad_seq_out)
{
	if (store == NULL || ctr == NULL || verdict_out == NULL) return ALP_ERR_INVAL;
	if (bad_seq_out) *bad_seq_out = 0;

	uint64_t     hw = 0;
	alp_status_t rc = ctr->read(ctr->ctx, 0, &hw);
	if (rc != ALP_OK) return ALP_ERR_IO;

	struct ulog_meta m;
	m.count = 0;
	memset(m.head_hash, 0, 32);
	if (hw > 0) {
		uint8_t metabuf[ULOG_META_WIRE_LEN];
		size_t  mlen = 0;
		rc           = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
		if (rc == ALP_ERR_NOT_FOUND) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK;
			return ALP_OK;
		}
		if (rc != ALP_OK) return ALP_ERR_IO;
		if (ulog_meta_decode(metabuf, mlen, &m) != ALP_OK) return ALP_ERR_IO;
		/* The counter is the trusted anchor. If the stored meta disagrees, the
         * store (or the counter) was rolled back. */
		if (m.count != hw) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK;
			return ALP_OK;
		}
	}

	uint8_t prev[32];
	memset(prev, 0, sizeof(prev));
	uint8_t cur_hash[32];
	memset(cur_hash, 0, sizeof(cur_hash));
	for (uint64_t i = 0; i < hw; i++) {
		char key[24];
		kbuf(key, sizeof(key), i);
		uint8_t wire[ULOG_ENTRY_WIRE_LEN];
		size_t  n = 0;
		rc        = store->get(store->ctx, key, wire, sizeof(wire), &n);
		if (rc == ALP_ERR_NOT_FOUND) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_TRUNCATED;
			if (bad_seq_out) *bad_seq_out = i;
			return ALP_OK;
		}
		if (rc != ALP_OK) return ALP_ERR_IO;

		alp_update_log_entry_t e;
		uint8_t                got_prev[32];
		if (ulog_entry_decode(wire, n, &e, got_prev) != ALP_OK) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
			if (bad_seq_out) *bad_seq_out = i;
			return ALP_OK;
		}
		if (e.seq != i || memcmp(got_prev, prev, 32) != 0) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
			if (bad_seq_out) *bad_seq_out = i;
			return ALP_OK;
		}
		ulog_sha256(wire, n, cur_hash);
		memcpy(prev, cur_hash, 32);
	}

	if (hw > 0 && memcmp(cur_hash, m.head_hash, 32) != 0) {
		*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
		if (bad_seq_out) *bad_seq_out = hw - 1;
		return ALP_OK;
	}

	*verdict_out = ALP_UPDATE_LOG_VERIFY_OK;
	return ALP_OK;
}

alp_status_t ulog_engine_count(const alp_secure_store_if      *store,
                               const alp_monotonic_counter_if *ctr, uint64_t *count_out)
{
	(void)store;
	if (ctr == NULL || count_out == NULL) return ALP_ERR_INVAL;
	return ctr->read(ctr->ctx, 0, count_out);
}

alp_status_t ulog_engine_get(const alp_secure_store_if *store, uint64_t seq,
                             alp_update_log_entry_t *e_out)
{
	if (store == NULL || e_out == NULL) return ALP_ERR_INVAL;
	char key[24];
	kbuf(key, sizeof(key), seq);
	uint8_t      wire[ULOG_ENTRY_WIRE_LEN];
	size_t       n  = 0;
	alp_status_t rc = store->get(store->ctx, key, wire, sizeof(wire), &n);
	if (rc != ALP_OK) return rc; /* ALP_ERR_NOT_FOUND propagates */
	uint8_t prev[32];
	return ulog_entry_decode(wire, n, e_out, prev);
}
