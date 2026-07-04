/* SPDX-License-Identifier: Apache-2.0
 *
 * Software tamper-evident tier for <alp/update_log.h>. The universal
 * silicon_ref="*" fallback: works on every SoM, reports assurance
 * SW_TAMPER_EVIDENT.
 *
 * Two store modes, selected once at first use:
 *
 *  - PERSISTENT (CONFIG_ALP_SDK_UPDATE_LOG_PERSIST + an `alp_ulog_partition`
 *    fixed partition in the devicetree): the keyed blob store and the
 *    monotonic counter live in Zephyr NVS on that partition, so the log
 *    survives reboot and firmware update. NVS fits the engine's record
 *    shape exactly -- fixed-size append-mostly blobs keyed by a small id,
 *    with power-fail-safe writes and wear levelling handled upstream
 *    (ADR 0017: consume the Zephyr subsystem, don't reimplement it).
 *
 *  - RAM fallback (no Kconfig opt-in, no partition, or the NVS mount
 *    fails at runtime): the pre-persistence behaviour -- entries vanish
 *    on reboot. Boards opt in by adding the partition label; see the
 *    CONFIG_ALP_SDK_UPDATE_LOG_PERSIST help text.
 *
 * TRUST BOUNDARY: persistence does NOT change the assurance level. This
 * tier stays app-cooperative (SW_TAMPER_EVIDENT): the chain + counter
 * detect mutation/truncation/rollback/reorder, but code with write access
 * to the partition can rebuild both store and counter consistently.
 * App-immutability is the HW_ENFORCED tier's job (issue #111).
 */
#include <string.h>

#include "alp/backend.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"
#include "update_log/engine.h"
#include "update_log/store.h"

#ifdef CONFIG_ALP_SDK_UPDATE_LOG_PERSIST
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#if PARTITION_EXISTS(alp_ulog_partition)
#define SW_TIER_NVS 1
#endif
#endif /* CONFIG_ALP_SDK_UPDATE_LOG_PERSIST */

/* ---- RAM store (fallback mode; always compiled) ------------------- */

#define SW_SLOTS 32
#define SW_BLOB  ULOG_ENTRY_WIRE_LEN
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

static int sw_find(const char *key)
{
	for (int i = 0; i < SW_SLOTS; i++) {
		if (g_sw.s[i].used && strcmp(g_sw.s[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

static alp_status_t sw_ram_put(const char *k, const uint8_t *b, size_t n)
{
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

static alp_status_t sw_ram_get(const char *k, uint8_t *b, size_t cap, size_t *o)
{
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

static alp_status_t sw_ram_erase(const char *k)
{
	int i = sw_find(k);
	if (i < 0) {
		return ALP_ERR_NOT_FOUND;
	}
	g_sw.s[i].used = false;
	return ALP_OK;
}

/* ---- NVS-persistent store (opt-in mode) --------------------------- */

#ifdef SW_TIER_NVS

/* NVS id map. The engine speaks string keys ("ulog.meta", "ulog.<seq>");
 * NVS speaks uint16_t ids. Reserved ids first, entries from a base so the
 * two ranges can never collide. */
#define SW_NVS_ID_COUNTER    0x0000u
#define SW_NVS_ID_META       0x0001u
#define SW_NVS_ID_ENTRY_BASE 0x0010u
#define SW_NVS_ENTRY_MAX     ((uint64_t)(UINT16_MAX - SW_NVS_ID_ENTRY_BASE))

/* One engine append = entry put + meta put + counter increment. Gate the
 * entry put on this much free space so the transaction cannot die halfway
 * (a meta/counter write failing mid-append would make verify() report
 * ROLLED_BACK on an honest log). 8 bytes per record is the NVS allocation
 * table entry (struct nvs_ate); nvs_calc_free_space() reports post-GC
 * capacity but records cannot span sector boundaries, so pad by one
 * entry's worth of boundary slack. */
#define SW_NVS_APPEND_WORST                                                                        \
	(2u * ULOG_ENTRY_WIRE_LEN + ULOG_META_WIRE_LEN + sizeof(uint64_t) + 3u * 8u)

static struct nvs_fs g_nvs;
static bool          g_nvs_ok;
static bool          g_nvs_tried;

/* Mount NVS on the partition once; on any failure fall back to RAM for
 * the rest of this boot (the pre-persistence behaviour). */
static bool sw_nvs_ready(void)
{
	if (g_nvs_tried) {
		return g_nvs_ok;
	}
	g_nvs_tried = true;

	const struct device *dev = PARTITION_DEVICE(alp_ulog_partition);
	if (!device_is_ready(dev)) {
		return false;
	}
	struct flash_pages_info info;
	if (flash_get_page_info_by_offs(dev, PARTITION_OFFSET(alp_ulog_partition), &info) != 0) {
		return false;
	}
	g_nvs.flash_device = dev;
	g_nvs.offset       = PARTITION_OFFSET(alp_ulog_partition);
	g_nvs.sector_size  = (uint32_t)info.size;
	g_nvs.sector_count = (uint16_t)(PARTITION_SIZE(alp_ulog_partition) / info.size);
	g_nvs_ok           = (nvs_mount(&g_nvs) == 0);
	return g_nvs_ok;
}

/* "ulog.meta" -> META id; "ulog.<decimal>" -> ENTRY_BASE + seq. */
static alp_status_t sw_nvs_key_to_id(const char *k, uint16_t *id_out)
{
	if (strcmp(k, "ulog.meta") == 0) {
		*id_out = SW_NVS_ID_META;
		return ALP_OK;
	}
	if (strncmp(k, "ulog.", 5) != 0 || k[5] == '\0') {
		return ALP_ERR_INVAL;
	}
	uint64_t v = 0;
	for (const char *p = k + 5; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			return ALP_ERR_INVAL;
		}
		v = v * 10u + (uint64_t)(*p - '0');
		if (v > SW_NVS_ENTRY_MAX) {
			/* Beyond the id space: the log is full for put(),
			 * trivially absent for get(). */
			return ALP_ERR_NOMEM;
		}
	}
	*id_out = (uint16_t)(SW_NVS_ID_ENTRY_BASE + v);
	return ALP_OK;
}

static alp_status_t sw_nvs_put(const char *k, const uint8_t *b, size_t n)
{
	uint16_t     id;
	alp_status_t rc = sw_nvs_key_to_id(k, &id);
	if (rc != ALP_OK) {
		return rc;
	}
	if (id >= SW_NVS_ID_ENTRY_BASE) {
		/* Head of an append transaction: reserve room for the whole
		 * entry+meta+counter triplet. An audit log must not wrap --
		 * when the partition is full, append fails cleanly and the
		 * existing chain stays verifiable. */
		ssize_t free_b = nvs_calc_free_space(&g_nvs);
		if (free_b < 0) {
			return ALP_ERR_IO;
		}
		if ((size_t)free_b < SW_NVS_APPEND_WORST) {
			return ALP_ERR_NOMEM;
		}
	}
	ssize_t w = nvs_write(&g_nvs, id, b, n);
	if (w < 0) {
		return (w == -ENOSPC) ? ALP_ERR_NOMEM : ALP_ERR_IO;
	}
	/* nvs_write returns 0 (nothing written) on an identical rewrite --
	 * the stored data already matches, so that is success too. */
	return ALP_OK;
}

static alp_status_t sw_nvs_get(const char *k, uint8_t *b, size_t cap, size_t *o)
{
	uint16_t     id;
	alp_status_t rc = sw_nvs_key_to_id(k, &id);
	if (rc == ALP_ERR_NOMEM) {
		return ALP_ERR_NOT_FOUND; /* beyond id space = never written */
	}
	if (rc != ALP_OK) {
		return rc;
	}
	ssize_t r = nvs_read(&g_nvs, id, b, cap);
	if (r == -ENOENT) {
		return ALP_ERR_NOT_FOUND;
	}
	if (r < 0) {
		return ALP_ERR_IO;
	}
	if ((size_t)r > cap) {
		return ALP_ERR_NOMEM; /* record larger than caller's buffer */
	}
	if (o) {
		*o = (size_t)r;
	}
	return ALP_OK;
}

static alp_status_t sw_nvs_erase(const char *k)
{
	uint16_t     id;
	alp_status_t rc = sw_nvs_key_to_id(k, &id);
	if (rc == ALP_ERR_NOMEM) {
		return ALP_ERR_NOT_FOUND;
	}
	if (rc != ALP_OK) {
		return rc;
	}
	int err = nvs_delete(&g_nvs, id);
	if (err == -ENOENT) {
		return ALP_ERR_NOT_FOUND;
	}
	return (err == 0) ? ALP_OK : ALP_ERR_IO;
}

static alp_status_t sw_nvs_counter_read(uint64_t *v)
{
	uint64_t val = 0;
	ssize_t  r   = nvs_read(&g_nvs, SW_NVS_ID_COUNTER, &val, sizeof(val));
	if (r == -ENOENT) {
		*v = 0; /* fresh store: counter starts at zero */
		return ALP_OK;
	}
	if (r < 0 || (size_t)r != sizeof(val)) {
		return ALP_ERR_IO;
	}
	*v = val;
	return ALP_OK;
}

static alp_status_t sw_nvs_counter_increment(uint64_t *v)
{
	uint64_t     val = 0;
	alp_status_t rc  = sw_nvs_counter_read(&val);
	if (rc != ALP_OK) {
		return rc;
	}
	val++;
	ssize_t w = nvs_write(&g_nvs, SW_NVS_ID_COUNTER, &val, sizeof(val));
	if (w < 0) {
		return (w == -ENOSPC) ? ALP_ERR_NOMEM : ALP_ERR_IO;
	}
	*v = val;
	return ALP_OK;
}

#endif /* SW_TIER_NVS */

/* ---- store_if / counter_if seams (mode dispatch) ------------------ */

static alp_status_t sw_put(void *c, const char *k, const uint8_t *b, size_t n)
{
	(void)c;
#ifdef SW_TIER_NVS
	if (sw_nvs_ready()) {
		return sw_nvs_put(k, b, n);
	}
#endif
	return sw_ram_put(k, b, n);
}

static alp_status_t sw_get(void *c, const char *k, uint8_t *b, size_t cap, size_t *o)
{
	(void)c;
#ifdef SW_TIER_NVS
	if (sw_nvs_ready()) {
		return sw_nvs_get(k, b, cap, o);
	}
#endif
	return sw_ram_get(k, b, cap, o);
}

static alp_status_t sw_erase(void *c, const char *k)
{
	(void)c;
#ifdef SW_TIER_NVS
	if (sw_nvs_ready()) {
		return sw_nvs_erase(k);
	}
#endif
	return sw_ram_erase(k);
}

static alp_status_t sw_cread(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
#ifdef SW_TIER_NVS
	if (sw_nvs_ready()) {
		return sw_nvs_counter_read(v);
	}
#endif
	*v = g_sw.counter;
	return ALP_OK;
}

static alp_status_t sw_cinc(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
#ifdef SW_TIER_NVS
	if (sw_nvs_ready()) {
		return sw_nvs_counter_increment(v);
	}
#endif
	g_sw.counter++;
	*v = g_sw.counter;
	return ALP_OK;
}

static const alp_secure_store_if      g_store = { sw_put, sw_get, sw_erase, NULL };
static const alp_monotonic_counter_if g_ctr   = { sw_cread, sw_cinc, NULL };

#ifdef CONFIG_ZTEST
/* Test-only reboot emulation: drop every piece of RAM state this backend
 * holds (store, counter, cached NVS mount) so the next operation re-reads
 * from flash -- proving persisted entries come from NVS, not from RAM.
 * With wipe=true the NVS partition is cleared too (pristine store). */
void alp_ulog_sw_tier_test_reset(bool wipe)
{
	memset(&g_sw, 0, sizeof(g_sw));
#ifdef SW_TIER_NVS
	if (wipe && g_nvs_ok) {
		(void)nvs_clear(&g_nvs); /* leaves fs unmounted */
	}
	g_nvs_ok    = false;
	g_nvs_tried = false; /* force a fresh mount on next use */
#else
	(void)wipe;
#endif
}
#endif /* CONFIG_ZTEST */

static alp_status_t sw_append(const alp_update_log_entry_t *e)
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

ALP_BACKEND_REGISTER(update_log,
                     sw_tier,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_tier",
                         .base_caps   = 0u,
                         .priority    = 10,
                         .ops         = &_sw_ops,
                         .probe       = NULL,
                     });
