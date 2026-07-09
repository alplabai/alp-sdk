/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include "alp/model.h"
#include <string.h>
#include <zcbor_decode.h>

#include "alp_range.h"

#define HDR_SIZE 24u

static uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool copy_tstr(struct zcbor_string *s, char *dst, size_t cap)
{
	size_t n = s->len < cap - 1 ? s->len : cap - 1;
	memcpy(dst, s->value, n);
	dst[n] = '\0';
	return true;
}

/* Decode the nested "requires" map; pulls sram_kib, skips the rest. */
static bool decode_requires(zcbor_state_t *zs, alp_model_target_t *t)
{
	bool ok = zcbor_map_start_decode(zs);
	while (ok && !zcbor_array_at_end(zs)) {
		struct zcbor_string k;
		if (!zcbor_tstr_decode(zs, &k)) {
			ok = false;
			break;
		}
		if (k.len == 8 && !memcmp(k.value, "sram_kib", 8)) {
			ok = zcbor_uint32_decode(zs, &t->req_sram_kib);
		} else {
			ok = zcbor_any_skip(zs, NULL);
		}
	}
	return ok && zcbor_map_end_decode(zs);
}

/* Decode one target map; the blob *index* is returned via *blob_index. */
static bool decode_target(zcbor_state_t *zs, alp_model_target_t *t, uint32_t *blob_index)
{
	bool ok = zcbor_map_start_decode(zs);
	while (ok && !zcbor_array_at_end(zs)) {
		struct zcbor_string key;
		if (!zcbor_tstr_decode(zs, &key)) {
			ok = false;
			break;
		}
		if (key.len == 7 && !memcmp(key.value, "backend", 7)) {
			struct zcbor_string v;
			ok = zcbor_tstr_decode(zs, &v);
			if (ok) copy_tstr(&v, t->backend, ALP_MODEL_STR_MAX);
		} else if (key.len == 11 && !memcmp(key.value, "silicon_ref", 11)) {
			struct zcbor_string v;
			ok = zcbor_tstr_decode(zs, &v);
			if (ok) copy_tstr(&v, t->silicon_ref, ALP_MODEL_STR_MAX);
		} else if (key.len == 11 && !memcmp(key.value, "blob_format", 11)) {
			struct zcbor_string v;
			ok = zcbor_tstr_decode(zs, &v);
			if (ok) copy_tstr(&v, t->blob_format, ALP_MODEL_STR_MAX);
		} else if (key.len == 12 && !memcmp(key.value, "accel_config", 12)) {
			struct zcbor_string v;
			ok = zcbor_tstr_decode(zs, &v);
			if (ok) copy_tstr(&v, t->accel_config, ALP_MODEL_STR_MAX);
		} else if (key.len == 5 && !memcmp(key.value, "arena", 5)) {
			ok = zcbor_uint32_decode(zs, &t->arena_bytes);
		} else if (key.len == 8 && !memcmp(key.value, "requires", 8)) {
			ok = decode_requires(zs, t);
		} else if (key.len == 4 && !memcmp(key.value, "blob", 4)) {
			ok = zcbor_uint32_decode(zs, blob_index);
		} else {
			ok = zcbor_any_skip(zs, NULL);
		}
	}
	return ok && zcbor_map_end_decode(zs);
}

alp_status_t alp_model_parse(const uint8_t *data, size_t size, alp_model_t *out)
{
	if (!data || !out || size < HDR_SIZE) return ALP_ERR_INVAL;
	if (memcmp(data, ALP_MODEL_MAGIC, 4) != 0) return ALP_ERR_INVAL;
	if (rd_u16(data + 4) != ALP_MODEL_CONTAINER_V) return ALP_ERR_VERSION;

	memset(out, 0, sizeof(*out));
	out->data        = data;
	out->size        = size;
	out->flags       = rd_u16(data + 6);
	uint32_t mft_off = rd_u32(data + 8), mft_len = rd_u32(data + 12);
	uint32_t tbl_off = rd_u32(data + 16), blob_count = rd_u32(data + 20);
	if (!alp_range_ok(mft_off, mft_len, size)) return ALP_ERR_INVAL;

	if (!alp_range_ok(tbl_off, (uint64_t)blob_count * 8u, size)) return ALP_ERR_INVAL;

	uint32_t idx[ALP_MODEL_MAX_TARGETS] = { 0 };
	/* Backup-state budget = n_states - 2.  The depth (8 states ~= 6
     * backups) covers the explicit decode nesting on this path: top map
     * -> targets list -> target map -> requires map = 4 backups, the
     * rest is margin.  zcbor_any_skip() draws nothing from the backup
     * budget: it uses a local state copy plus C recursion, not the
     * zs[] backup array. */
	zcbor_state_t zs[8];
	zcbor_new_decode_state(zs, 8, data + mft_off, mft_len, 1, NULL, 0);

	bool ok = zcbor_map_start_decode(zs);
	while (ok && !zcbor_array_at_end(zs)) {
		struct zcbor_string key;
		if (!zcbor_tstr_decode(zs, &key)) {
			ok = false;
			break;
		}
		if (key.len == 4 && !memcmp(key.value, "name", 4)) {
			struct zcbor_string v;
			ok = zcbor_tstr_decode(zs, &v);
			if (ok) copy_tstr(&v, out->name, ALP_MODEL_STR_MAX);
		} else if (key.len == 7 && !memcmp(key.value, "src_sha", 7)) {
			struct zcbor_string v;
			ok = zcbor_bstr_decode(zs, &v);
			if (ok && v.len == 32) memcpy(out->src_sha, v.value, 32);
		} else if (key.len == 7 && !memcmp(key.value, "targets", 7)) {
			ok = zcbor_list_start_decode(zs);
			while (ok && !zcbor_array_at_end(zs) && out->n_targets < ALP_MODEL_MAX_TARGETS) {
				alp_model_target_t *t = &out->targets[out->n_targets];
				ok                    = decode_target(zs, t, &idx[out->n_targets]);
				if (ok) out->n_targets++;
			}
			if (ok) ok = zcbor_list_end_decode(zs);
		} else {
			ok = zcbor_any_skip(zs, NULL);
		}
	}
	if (!ok || !zcbor_map_end_decode(zs)) return ALP_ERR_INVAL;

	for (uint32_t i = 0; i < out->n_targets; i++) {
		uint32_t bi = idx[i];
		if (bi >= blob_count) return ALP_ERR_INVAL;
		size_t   e    = (size_t)tbl_off + (size_t)bi * 8u; /* in-bounds via the table check above */
		uint32_t boff = rd_u32(data + e);
		uint32_t blen = rd_u32(data + e + 4);
		if (!alp_range_ok(boff, blen, size)) return ALP_ERR_INVAL;
		out->targets[i].blob     = data + boff;
		out->targets[i].blob_len = blen;
	}
	return ALP_OK;
}
