/* SPDX-License-Identifier: Apache-2.0
 *
 * Software tamper-evident tier for <alp/update_log.h>. The universal
 * silicon_ref="*" fallback: works on every SoM, reports assurance
 * SW_TAMPER_EVIDENT. Backs the engine with a RAM store + a RAM monotonic
 * counter.
 *
 * SLICE 1 SCOPE: the store is in-RAM, so the log does not survive reboot.
 * Durable persistence via Zephyr Settings/NVS is the named follow-up before
 * this tier is production. The engine + assurance contract are unchanged by
 * that swap -- only the store_if implementation gains persistence.
 */
#include <string.h>

#include "alp/backend.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"
#include "update_log/engine.h"
#include "update_log/store.h"

#define SW_SLOTS 32
#define SW_BLOB ULOG_ENTRY_WIRE_LEN
struct sw_state {
	struct {
		char    key[24];
		uint8_t buf[SW_BLOB];
		size_t  len;
		bool    used;
	} s[SW_SLOTS];
	uint64_t counter;
};
static struct sw_state g_sw;

static int             sw_find(const char *key)
{
	for (int i = 0; i < SW_SLOTS; i++) {
		if (g_sw.s[i].used && strcmp(g_sw.s[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

static alp_status_t sw_put(void *c, const char *k, const uint8_t *b, size_t n)
{
	(void)c;
	if (n > SW_BLOB) {
		return ALP_ERR_NOMEM;
	}
	int i = sw_find(k);
	if (i < 0) {
		for (i = 0; i < SW_SLOTS; i++) {
			if (!g_sw.s[i].used) {
				break;
			}
		}
		if (i == SW_SLOTS) {
			return ALP_ERR_NOMEM;
		}
	}
	g_sw.s[i].used = true;
	strncpy(g_sw.s[i].key, k, sizeof(g_sw.s[i].key) - 1);
	g_sw.s[i].key[sizeof(g_sw.s[i].key) - 1] = 0;
	memcpy(g_sw.s[i].buf, b, n);
	g_sw.s[i].len = n;
	return ALP_OK;
}

static alp_status_t sw_get(void *c, const char *k, uint8_t *b, size_t cap, size_t *o)
{
	(void)c;
	int i = sw_find(k);
	if (i < 0) {
		return ALP_ERR_NOT_FOUND;
	}
	if (g_sw.s[i].len > cap) {
		return ALP_ERR_NOMEM;
	}
	memcpy(b, g_sw.s[i].buf, g_sw.s[i].len);
	if (o) {
		*o = g_sw.s[i].len;
	}
	return ALP_OK;
}

static alp_status_t sw_erase(void *c, const char *k)
{
	(void)c;
	int i = sw_find(k);
	if (i < 0) {
		return ALP_ERR_NOT_FOUND;
	}
	g_sw.s[i].used = false;
	return ALP_OK;
}

static alp_status_t sw_cread(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
	*v = g_sw.counter;
	return ALP_OK;
}

static alp_status_t sw_cinc(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
	g_sw.counter++;
	*v = g_sw.counter;
	return ALP_OK;
}

static const alp_secure_store_if      g_store = { sw_put, sw_get, sw_erase, NULL };
static const alp_monotonic_counter_if g_ctr   = { sw_cread, sw_cinc, NULL };

static alp_status_t                   sw_append(const alp_update_log_entry_t *e)
{
	return ulog_engine_append(&g_store, &g_ctr, e);
}

static alp_status_t sw_verify(alp_update_log_verdict_t *v, uint64_t *bad)
{
	return ulog_engine_verify(&g_store, &g_ctr, v, bad);
}

static alp_status_t sw_count(uint64_t *o)
{
	return ulog_engine_count(&g_store, &g_ctr, o);
}

static alp_status_t sw_get_e(uint64_t seq, alp_update_log_entry_t *o)
{
	return ulog_engine_get(&g_store, seq, o);
}

static const alp_update_log_ops_t _sw_ops = {
	.assurance = ALP_UPDATE_LOG_SW_TAMPER_EVIDENT,
	.append    = sw_append,
	.verify    = sw_verify,
	.count     = sw_count,
	.get       = sw_get_e,
};

ALP_BACKEND_REGISTER(update_log, sw_tier,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_tier",
                         .base_caps   = 0u,
                         .priority    = 10,
                         .ops         = &_sw_ops,
                         .probe       = NULL,
                     });
