/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the upstream Zephyr `bt`
 * host stack) ======
 * Consumes upstream `bt_gatt_service_register()` / `bt_gatt_read()` /
 * `bt_gatt_write()` / `bt_gatt_notify()` verbatim -- no forked or
 * vendored Bluetooth host code.  The only alp-owned logic is the
 * runtime `alp_ble_service_def_t` -> `struct bt_gatt_attr[]` mapping
 * (register) and the two synchronous k_sem wrappers around the
 * async client read/write callbacks (connect-side).
 *
 * Portable Zephyr BT-host backend for the <alp/ble.h> surface.
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * design spec Section 2 backend matrix (zephyr_drv wins on every
 * SoC unless a more specific backend registers).
 *
 * AEN CC3501E note: AEN does NOT route through Zephyr HCI today.
 * When CONFIG_ALP_SDK_BLE_CC3501E is enabled, the exact-silicon
 * CC3501E backend registers at higher priority and wraps chips/cc3501e
 * directly.  This wildcard backend remains the portable Zephyr path for
 * SoCs that expose a native Zephyr BT controller.
 *
 * Gated on CONFIG_ALP_SDK_BLE -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with BLE in
 * the device tree.
 *
 * @par GATT server (register/read/write/notify) -- issue #480
 * `bt_gatt_service_register()` is a pure host-stack call: it needs
 * neither `bt_enable()` nor a live controller (verified against
 * upstream Zephyr's own `tests/bluetooth/gatt` suite, which never
 * calls `bt_enable()` either).  So registration + the per-characteristic
 * read()/write() attribute callbacks are host-testable under native_sim
 * (see tests/zephyr/ble_gatt_server).  `alp_ble_gatt_read()` /
 * `alp_ble_gatt_write()` (conn-side, GATT *client* reads of a connected
 * peer) build on the real async `bt_gatt_read()`/`bt_gatt_write()` plus
 * a k_sem wrapper -- correct against the upstream API, but DEFERRED for
 * bench proof: native_sim ships no BLE controller (BT_USERCHAN needs a
 * real host Bluetooth adapter, powered off, which this sandbox does not
 * have -- confirmed by hand: bt_enable() blocks indefinitely opening the
 * userchan HCI socket instead of failing fast), so no real over-the-air
 * connection can be established offline to drive them end-to-end.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "ble_ops.h"

#if defined(CONFIG_ALP_SDK_BLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#endif

#ifndef CONFIG_ALP_SDK_BLE_MAX_CONNS
#define CONFIG_ALP_SDK_BLE_MAX_CONNS 2
#endif

/* GATT server attribute-database pool sizing (issue #480).  Services are
 * registered once at boot and never unregistered in v0.3, so a bump
 * allocator over these fixed pools is sufficient -- no free-list needed. */
#ifndef CONFIG_ALP_SDK_BLE_GATT_MAX_SERVICES
#define CONFIG_ALP_SDK_BLE_GATT_MAX_SERVICES 4
#endif
#ifndef CONFIG_ALP_SDK_BLE_GATT_MAX_CHARS
#define CONFIG_ALP_SDK_BLE_GATT_MAX_CHARS 16
#endif
#ifndef CONFIG_ALP_SDK_BLE_GATT_MAX_VALUE_LEN
#define CONFIG_ALP_SDK_BLE_GATT_MAX_VALUE_LEN 64
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

struct ble_radio_be {
	int refcount;
#if defined(CONFIG_ALP_SDK_BLE)
	bool              advertising;
	bool              scanning;
	alp_ble_scan_cb_t scan_cb;
	void             *scan_user;
#endif
};

struct ble_conn_be {
#if defined(CONFIG_ALP_SDK_BLE)
	struct bt_conn *bt;
#else
	int unused;
#endif
};

#if defined(CONFIG_ALP_SDK_BLE)

static struct ble_radio_be _radio_be;
static struct ble_conn_be  _conn_be_pool[CONFIG_ALP_SDK_BLE_MAX_CONNS];
static bool                _conn_be_in_use[CONFIG_ALP_SDK_BLE_MAX_CONNS];

static struct ble_conn_be *_conn_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_conn_be_pool); ++i) {
		if (!_conn_be_in_use[i]) {
			memset(&_conn_be_pool[i], 0, sizeof(_conn_be_pool[i]));
			_conn_be_in_use[i] = true;
			return &_conn_be_pool[i];
		}
	}
	return NULL;
}

static void _conn_be_free(struct ble_conn_be *p)
{
	if (p == NULL) return;
	if (p->bt != NULL) {
		bt_conn_unref(p->bt);
		p->bt = NULL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(_conn_be_pool); ++i) {
		if (&_conn_be_pool[i] == p) {
			_conn_be_in_use[i] = false;
			return;
		}
	}
}

static alp_status_t errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case -ENOMEM:
		return ALP_ERR_NOMEM;
	default:
		return ALP_ERR_IO;
	}
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	if (_radio_be.scan_cb == NULL) return;
	alp_ble_scan_result_t r = { 0 };
	r.rssi_dbm              = info->rssi;
	r.adv_type              = info->adv_type;
	r.adv_data              = buf->data;
	r.adv_len               = buf->len;
	r.addr.type             = (uint8_t)info->addr->type;
	memcpy(r.addr.addr, info->addr->a.val, 6);
	_radio_be.scan_cb(&r, _radio_be.scan_user);
}

static struct bt_le_scan_cb _scan_cb = {
	.recv = scan_recv_cb,
};

/* alp_ble_char_def_t.properties is documented (<alp/ble.h>) as an OR of
 * ALP_BLE_GATT_PROP_* -- those bit values are chosen to match Zephyr's
 * BT_GATT_CHRC_* verbatim, so no translation table is needed below.
 * Guard the assumption so a future edit to either side trips a build
 * error instead of silently mis-mapping properties. */
BUILD_ASSERT(ALP_BLE_GATT_PROP_READ == BT_GATT_CHRC_READ &&
                 ALP_BLE_GATT_PROP_WRITE == BT_GATT_CHRC_WRITE &&
                 ALP_BLE_GATT_PROP_NOTIFY == BT_GATT_CHRC_NOTIFY &&
                 ALP_BLE_GATT_PROP_INDICATE == BT_GATT_CHRC_INDICATE,
             "ALP_BLE_GATT_PROP_* must mirror BT_GATT_CHRC_* bit-for-bit");

/* ------------------------------------------------------------------ */
/* GATT server -- runtime service/characteristic registration (#480)   */
/* ------------------------------------------------------------------ */

/* Per-characteristic backing store: the UUID + bt_gatt_chrc declaration
 * struct both need storage that outlives this call (Zephyr's attribute
 * database keeps pointers into them), and the value buffer + CCC state
 * back this characteristic's read()/write()/notify() callbacks. */
struct ble_char_be {
	struct bt_uuid_128                   uuid;
	struct bt_gatt_chrc                  chrc;
	struct bt_gatt_ccc_managed_user_data ccc;
	uint8_t                              value[CONFIG_ALP_SDK_BLE_GATT_MAX_VALUE_LEN];
	uint16_t                             value_len;
};

/* Worst case: one primary-service-decl attr per service, plus up to 3
 * attrs (chrc-decl + value + CCC) per characteristic across all services. */
static struct bt_gatt_attr
    _gatt_attr_pool[CONFIG_ALP_SDK_BLE_GATT_MAX_SERVICES + CONFIG_ALP_SDK_BLE_GATT_MAX_CHARS * 3];
static struct ble_char_be     _gatt_char_pool[CONFIG_ALP_SDK_BLE_GATT_MAX_CHARS];
static struct bt_uuid_128     _gatt_svc_uuid_pool[CONFIG_ALP_SDK_BLE_GATT_MAX_SERVICES];
static struct bt_gatt_service _gatt_svc_pool[CONFIG_ALP_SDK_BLE_GATT_MAX_SERVICES];
static size_t                 _gatt_attr_used;
static size_t                 _gatt_char_used;
static size_t                 _gatt_svc_used;

static void ble_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	(void)attr;
	(void)value;
}

static ssize_t ble_char_read(struct bt_conn            *conn,
                             const struct bt_gatt_attr *attr,
                             void                      *buf,
                             uint16_t                   len,
                             uint16_t                   offset)
{
	const struct ble_char_be *c = (const struct ble_char_be *)attr->user_data;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, c->value, c->value_len);
}

static ssize_t ble_char_write(struct bt_conn            *conn,
                              const struct bt_gatt_attr *attr,
                              const void                *buf,
                              uint16_t                   len,
                              uint16_t                   offset,
                              uint8_t                    flags)
{
	struct ble_char_be *c = (struct ble_char_be *)attr->user_data;
	(void)conn;
	(void)flags;
	if ((size_t)offset + len > sizeof(c->value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	memcpy(c->value + offset, buf, len);
	c->value_len = (uint16_t)(offset + len);
	return len;
}

/* Synchronous client-side read/write (conn ops, below) block on this
 * semaphore-backed context until the async bt_gatt_read()/bt_gatt_write()
 * completion callback fires -- the ctx must remain valid until then,
 * which the k_sem_take() call below guarantees for an on-stack instance. */
struct ble_read_ctx {
	struct bt_gatt_read_params params; /* MUST be first: cb casts params -> ctx */
	struct k_sem               done;
	alp_status_t               result;
	uint8_t                   *out;
	size_t                     out_cap;
	size_t                    *out_len;
};

struct ble_write_ctx {
	struct bt_gatt_write_params params; /* MUST be first: cb casts params -> ctx */
	struct k_sem                done;
	alp_status_t                result;
};

static uint8_t ble_read_cb(struct bt_conn             *conn,
                           uint8_t                     err,
                           struct bt_gatt_read_params *params,
                           const void                 *data,
                           uint16_t                    length)
{
	struct ble_read_ctx *ctx = (struct ble_read_ctx *)params;
	(void)conn;
	if (data == NULL) {
		/* Read procedure complete (no long-read continuation in v0.3). */
		k_sem_give(&ctx->done);
		return BT_GATT_ITER_STOP;
	}
	if (err != 0) {
		/* err is an ATT protocol error code (BT_ATT_ERR_*), not an
		 * errno -- the peer rejected the read (bad handle, no
		 * permission, ...). Surface as I/O; there is no ALP_ERR_*
		 * finer-grained than that for a remote protocol error. */
		ctx->result = ALP_ERR_IO;
		return BT_GATT_ITER_STOP;
	}
	size_t n = MIN((size_t)length, ctx->out_cap);
	memcpy(ctx->out, data, n);
	if (ctx->out_len != NULL) *ctx->out_len = n;
	ctx->result = ALP_OK;
	return BT_GATT_ITER_STOP;
}

static void ble_write_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	struct ble_write_ctx *ctx = (struct ble_write_ctx *)params;
	(void)conn;
	ctx->result = (err == 0) ? ALP_OK : ALP_ERR_IO;
	k_sem_give(&ctx->done);
}
#endif /* CONFIG_ALP_SDK_BLE */

/* ================================================================== */
/* Radio-side ops                                                      */
/* ================================================================== */

static alp_status_t z_open(alp_ble_radio_state_t *st, alp_capabilities_t *caps_out)
{
#if defined(CONFIG_ALP_SDK_BLE)
	if (_radio_be.refcount == 0) {
		int err = bt_enable(NULL);
		if (err != 0 && err != -EALREADY) {
			caps_out->flags = 0u;
			return errno_to_alp(err);
		}
		bt_le_scan_cb_register(&_scan_cb);
	}
	_radio_be.refcount++;
	st->be_data     = &_radio_be;
	caps_out->flags = 0u;
	return ALP_OK;
#else
	(void)st;
	caps_out->flags = 0u;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
	if (be == NULL) return;
	if (be->refcount > 0) be->refcount--;
	if (be->refcount == 0) {
		if (be->advertising) {
			(void)bt_le_adv_stop();
			be->advertising = false;
		}
		if (be->scanning) {
			(void)bt_le_scan_stop();
			be->scanning = false;
		}
	}
	st->be_data = NULL;
#else
	(void)st;
#endif
}

static alp_status_t z_advertise_start(alp_ble_radio_state_t *st, const alp_ble_adv_config_t *cfg)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (be->advertising) return ALP_ERR_BUSY;

	/* Pack adv data: flags + complete local name (if provided) + service UUIDs.
     * Size budget is 31 bytes total per BT spec; we trust the caller
     * not to exceed that for v0.3.  v0.3.x adds extended-advertising
     * support for larger payloads. */
	struct bt_data ad[3];
	size_t         ad_len = 0;

	static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
	ad[ad_len++]               = (struct bt_data){
		.type     = BT_DATA_FLAGS,
		.data_len = 1,
		.data     = &flags,
	};
	if (cfg->name != NULL && cfg->name[0] != '\0') {
		ad[ad_len++] = (struct bt_data){
			.type     = BT_DATA_NAME_COMPLETE,
			.data_len = (uint8_t)strlen(cfg->name),
			.data     = (const uint8_t *)cfg->name,
		};
	}
	if (cfg->services != NULL && cfg->num_services > 0) {
		ad[ad_len++] = (struct bt_data){
			.type     = BT_DATA_UUID128_ALL,
			.data_len = (uint8_t)(cfg->num_services * sizeof(alp_ble_uuid_t)),
			.data     = (const uint8_t *)cfg->services,
		};
	}

	struct bt_le_adv_param p = {
		.options = (cfg->connectable ? BT_LE_ADV_OPT_CONN : 0) | BT_LE_ADV_OPT_USE_IDENTITY,
		.interval_min =
		    (uint32_t)((cfg->interval_min_ms ? cfg->interval_min_ms : 100) * 1000 / 625),
		.interval_max =
		    (uint32_t)((cfg->interval_max_ms ? cfg->interval_max_ms : 200) * 1000 / 625),
	};

	int err = bt_le_adv_start(&p, ad, ad_len, NULL, 0);
	if (err == 0) be->advertising = true;
	return errno_to_alp(err);
#else
	(void)st;
	(void)cfg;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_advertise_stop(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (!be->advertising) return ALP_OK;
	int err = bt_le_adv_stop();
	if (err == 0) be->advertising = false;
	return errno_to_alp(err);
#else
	(void)st;
	return ALP_ERR_NOSUPPORT;
#endif
}

#if defined(CONFIG_ALP_SDK_BLE)
/* Global (cross-service) lookup -- gatt_notify only gets a handle, and
 * the registered attrs live in one flat bump-allocated pool regardless
 * of which service they belong to. */
static const struct bt_gatt_attr *_gatt_attr_by_handle(alp_ble_attr_handle_t handle)
{
	for (size_t i = 0; i < _gatt_attr_used; ++i) {
		if (_gatt_attr_pool[i].handle == handle) return &_gatt_attr_pool[i];
	}
	return NULL;
}
#endif

static alp_status_t z_gatt_register_service(alp_ble_radio_state_t       *st,
                                            const alp_ble_service_def_t *def,
                                            alp_ble_attr_handle_t       *handles_out)
{
#if defined(CONFIG_ALP_SDK_BLE)
	(void)st;
	if (def->num_chars > 0 && (def->chars == NULL || handles_out == NULL)) {
		return ALP_ERR_INVAL;
	}

	/* Size the attrs this registration needs: 1 primary-service decl +
     * per char (chrc-decl + value [+ CCC iff notify/indicate capable]). */
	size_t attrs_needed = 1;
	for (size_t i = 0; i < def->num_chars; ++i) {
		if (def->chars[i].initial_len > sizeof(_gatt_char_pool[0].value)) {
			return ALP_ERR_INVAL;
		}
		attrs_needed += 2;
		if (def->chars[i].properties & (ALP_BLE_GATT_PROP_NOTIFY | ALP_BLE_GATT_PROP_INDICATE)) {
			attrs_needed += 1;
		}
	}

	if (_gatt_svc_used >= ARRAY_SIZE(_gatt_svc_pool) ||
	    _gatt_char_used + def->num_chars > ARRAY_SIZE(_gatt_char_pool) ||
	    _gatt_attr_used + attrs_needed > ARRAY_SIZE(_gatt_attr_pool)) {
		return ALP_ERR_NOMEM;
	}

	struct bt_gatt_attr *attrs    = &_gatt_attr_pool[_gatt_attr_used];
	struct ble_char_be  *chars    = &_gatt_char_pool[_gatt_char_used];
	struct bt_uuid_128  *svc_uuid = &_gatt_svc_uuid_pool[_gatt_svc_used];

	svc_uuid->uuid.type = BT_UUID_TYPE_128;
	memcpy(svc_uuid->val, def->service_uuid.b, sizeof(svc_uuid->val));

	size_t idx   = 0;
	attrs[idx++] = (struct bt_gatt_attr)BT_GATT_ATTRIBUTE(
	    BT_UUID_GATT_PRIMARY, BT_GATT_PERM_READ, bt_gatt_attr_read_service, NULL, &svc_uuid->uuid);

	for (size_t i = 0; i < def->num_chars; ++i) {
		struct ble_char_be *c = &chars[i];
		memset(c, 0, sizeof(*c));
		c->uuid.uuid.type = BT_UUID_TYPE_128;
		memcpy(c->uuid.val, def->chars[i].uuid.b, sizeof(c->uuid.val));
		c->chrc.uuid         = &c->uuid.uuid;
		c->chrc.value_handle = 0U; /* auto: falls back to (this attr's handle + 1) */
		c->chrc.properties   = def->chars[i].properties;

		if (def->chars[i].initial_value != NULL && def->chars[i].initial_len > 0) {
			memcpy(c->value, def->chars[i].initial_value, def->chars[i].initial_len);
			c->value_len = (uint16_t)def->chars[i].initial_len;
		}

		uint16_t perm = 0;
		if (def->chars[i].properties & ALP_BLE_GATT_PROP_READ) perm |= BT_GATT_PERM_READ;
		if (def->chars[i].properties & ALP_BLE_GATT_PROP_WRITE) perm |= BT_GATT_PERM_WRITE;

		attrs[idx++] = (struct bt_gatt_attr)BT_GATT_ATTRIBUTE(
		    BT_UUID_GATT_CHRC, BT_GATT_PERM_READ, bt_gatt_attr_read_chrc, NULL, &c->chrc);

		/* handles_out[i] temporarily holds this attr's pool index --
         * overwritten with the real, stack-assigned handle once
         * bt_gatt_service_register() below has run. */
		handles_out[i] = (alp_ble_attr_handle_t)idx;
		attrs[idx++]   = (struct bt_gatt_attr)BT_GATT_ATTRIBUTE(
		    &c->uuid.uuid,
		    perm,
		    (perm & BT_GATT_PERM_READ) ? ble_char_read : NULL,
		    (perm & BT_GATT_PERM_WRITE) ? ble_char_write : NULL,
		    c);

		if (def->chars[i].properties & (ALP_BLE_GATT_PROP_NOTIFY | ALP_BLE_GATT_PROP_INDICATE)) {
			c->ccc.cfg_changed = ble_ccc_changed;
			attrs[idx++]       = (struct bt_gatt_attr){
				.uuid      = BT_UUID_GATT_CCC,
				.perm      = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
				.read      = bt_gatt_attr_read_ccc,
				.write     = bt_gatt_attr_write_ccc,
				.user_data = &c->ccc,
			};
		}
	}

	struct bt_gatt_service *svc = &_gatt_svc_pool[_gatt_svc_used];
	svc->attrs                  = attrs;
	svc->attr_count             = idx;

	int err = bt_gatt_service_register(svc);
	if (err != 0) return errno_to_alp(err);

	for (size_t i = 0; i < def->num_chars; ++i) {
		handles_out[i] = (alp_ble_attr_handle_t)attrs[handles_out[i]].handle;
	}

	_gatt_attr_used += attrs_needed;
	_gatt_char_used += def->num_chars;
	_gatt_svc_used += 1;

	return ALP_OK;
#else
	(void)st;
	(void)def;
	(void)handles_out;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_gatt_notify(alp_ble_radio_state_t *radio_st,
                                  alp_ble_conn_state_t  *conn_st,
                                  alp_ble_attr_handle_t  handle,
                                  const uint8_t         *payload,
                                  size_t                 len)
{
#if defined(CONFIG_ALP_SDK_BLE)
	(void)radio_st;
	const struct bt_gatt_attr *attr = _gatt_attr_by_handle(handle);
	if (attr == NULL) return ALP_ERR_INVAL;
	struct ble_conn_be *c   = (conn_st != NULL) ? (struct ble_conn_be *)conn_st->be_data : NULL;
	struct bt_conn     *bt  = (c != NULL) ? c->bt : NULL;
	int                 err = bt_gatt_notify(bt, attr, payload, (uint16_t)len);
	return errno_to_alp(err);
#else
	(void)radio_st;
	(void)conn_st;
	(void)handle;
	(void)payload;
	(void)len;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_scan_start(alp_ble_radio_state_t *st, bool active, alp_ble_scan_cb_t cb, void *user)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (be->scanning) return ALP_ERR_BUSY;
	be->scan_cb               = cb;
	be->scan_user             = user;
	struct bt_le_scan_param p = {
		.type     = active ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = 0x60, /* 60 ms in BT units */
		.window   = 0x30, /* 30 ms */
	};
	int err = bt_le_scan_start(&p, NULL);
	if (err == 0) be->scanning = true;
	return errno_to_alp(err);
#else
	(void)st;
	(void)active;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_scan_stop(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (!be->scanning) return ALP_OK;
	int err = bt_le_scan_stop();
	if (err == 0) be->scanning = false;
	return errno_to_alp(err);
#else
	(void)st;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_connect(alp_ble_radio_state_t *st,
                              const alp_ble_addr_t  *peer,
                              uint32_t               timeout_ms,
                              alp_ble_conn_state_t  *conn_st)
{
#if defined(CONFIG_ALP_SDK_BLE)
	bt_addr_le_t addr = { 0 };
	addr.type         = peer->type;
	memcpy(addr.a.val, peer->addr, 6);

	struct ble_conn_be *c = _conn_be_alloc();
	if (c == NULL) return ALP_ERR_NOMEM;

	int err = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &c->bt);
	if (err != 0) {
		_conn_be_free(c);
		return errno_to_alp(err);
	}
	/* bt_conn_le_create returns immediately; the connection
     * complete event is signalled via callbacks.  v0.3.x adds a
     * connect-completion semaphore here; for v0.3 we stop short of
     * waiting and let the caller poll bt_conn_get_info(). */
	(void)timeout_ms;
	(void)st;
	conn_st->be_data = c;
	return ALP_OK;
#else
	(void)st;
	(void)peer;
	(void)timeout_ms;
	(void)conn_st;
	return ALP_ERR_NOSUPPORT;
#endif
}

/* ================================================================== */
/* Conn-side ops                                                       */
/* ================================================================== */

static alp_status_t z_disconnect(alp_ble_conn_state_t *conn_st)
{
#if defined(CONFIG_ALP_SDK_BLE)
	struct ble_conn_be *c = (struct ble_conn_be *)conn_st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	int err = bt_conn_disconnect(c->bt, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	_conn_be_free(c);
	conn_st->be_data = NULL;
	return errno_to_alp(err);
#else
	(void)conn_st;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_gatt_read(alp_ble_conn_state_t *conn_st,
                                alp_ble_attr_handle_t handle,
                                uint8_t              *out,
                                size_t                out_cap,
                                size_t               *out_len,
                                uint32_t              timeout_ms)
{
#if defined(CONFIG_ALP_SDK_BLE)
	/* GATT *client* read of a connected peer's characteristic (the
     * handle came from that peer's discovery, not our own registered
     * services above).  bt_gatt_read() is async; block on a k_sem until
     * ble_read_cb() completes it or timeout_ms elapses.
     * @par BENCH-UNVERIFIED (issue #480): correct against the upstream
     * async API, but native_sim ships no working BLE controller (see
     * this file's header) so no live peer connection exists to drive
     * this end-to-end offline; needs a real two-device bench proof. */
	struct ble_conn_be *c = (struct ble_conn_be *)conn_st->be_data;
	if (c == NULL || c->bt == NULL) return ALP_ERR_NOT_READY;

	struct ble_read_ctx ctx = {
		.params =
		    {
		        .func         = ble_read_cb,
		        .handle_count = 1,
		        .single       = { .handle = handle, .offset = 0 },
		    },
		.out     = out,
		.out_cap = out_cap,
		.out_len = out_len,
		.result  = ALP_ERR_TIMEOUT,
	};
	k_sem_init(&ctx.done, 0, 1);

	int err = bt_gatt_read(c->bt, &ctx.params);
	if (err != 0) return errno_to_alp(err);

	if (k_sem_take(&ctx.done, K_MSEC(timeout_ms)) != 0) return ALP_ERR_TIMEOUT;
	return ctx.result;
#else
	(void)conn_st;
	(void)handle;
	(void)out;
	(void)out_cap;
	(void)out_len;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_gatt_write(alp_ble_conn_state_t *conn_st,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t        *data,
                                 size_t                len,
                                 uint32_t              timeout_ms)
{
#if defined(CONFIG_ALP_SDK_BLE)
	/* Same story as z_gatt_read() above -- BENCH-UNVERIFIED (#480),
     * correct against bt_gatt_write() but unreachable offline without a
     * live peer connection. */
	struct ble_conn_be *c = (struct ble_conn_be *)conn_st->be_data;
	if (c == NULL || c->bt == NULL) return ALP_ERR_NOT_READY;

	struct ble_write_ctx ctx = {
		.params =
		    {
		        .func   = ble_write_cb,
		        .handle = handle,
		        .offset = 0,
		        .data   = data,
		        .length = (uint16_t)len,
		    },
		.result = ALP_ERR_TIMEOUT,
	};
	k_sem_init(&ctx.done, 0, 1);

	int err = bt_gatt_write(c->bt, &ctx.params);
	if (err != 0) return errno_to_alp(err);

	if (k_sem_take(&ctx.done, K_MSEC(timeout_ms)) != 0) return ALP_ERR_TIMEOUT;
	return ctx.result;
#else
	(void)conn_st;
	(void)handle;
	(void)data;
	(void)len;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_ble_ops_t _ops = {
	.open                  = z_open,
	.advertise_start       = z_advertise_start,
	.advertise_stop        = z_advertise_stop,
	.gatt_register_service = z_gatt_register_service,
	.gatt_notify           = z_gatt_notify,
	.scan_start            = z_scan_start,
	.scan_stop             = z_scan_stop,
	.connect               = z_connect,
	.close                 = z_close,
	.disconnect            = z_disconnect,
	.gatt_read             = z_gatt_read,
	.gatt_write            = z_gatt_write,
};

ALP_BACKEND_REGISTER(ble,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
