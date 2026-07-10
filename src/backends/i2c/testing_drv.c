/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C test double.  Ordinary alp_i2c_ops_t backend registered at the
 * reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_I2C=y it outranks every real/proxy/fallback
 * I2C backend and alp_i2c_open() rides on it transparently -- see the
 * priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * This double simulates the DEVICE(S) on the bus, not this MCU's own
 * target (slave) mode: it implements only the CONTROLLER-mode ops
 * (open/write/read/write_read/close). target_open/target_close are
 * left NULL, so alp_i2c_target_open() fails ALP_ERR_NOSUPPORT on this
 * backend, exactly like a backend with no target support at all (see
 * the @note on i2c_ops.h's alp_i2c_ops).
 *
 * open()       -- ALWAYS succeeds.  Same deliberate ergonomic choice
 *                  as the GPIO/UART doubles (src/backends/gpio/,
 *                  src/backends/uart/testing_drv.c): a "*" silicon_ref
 *                  double has no valid-bus-range knowledge to reject
 *                  an id with, and the injection API
 *                  (<alp/testing/i2c.h>) needs inject-before-open to
 *                  work.
 * write()      -- captures up to ALP_TESTING_I2C_MAX_CHUNK bytes into
 *                  the addressed device's "last write" snapshot (NOT a
 *                  drained queue -- read back any time via
 *                  alp_testing_i2c_last_write()), subject to a one-shot
 *                  fault if armed.
 * read()       -- fills the caller's buffer from the addressed
 *                  device's canned response (alp_testing_i2c_target_respond,
 *                  a persistent snapshot, not a consumed queue),
 *                  zero-padding a request longer than the canned data,
 *                  subject to a one-shot fault if armed.
 * write_read() -- both of the above against the SAME one-shot fault
 *                  (a single armed fault covers the whole call, not
 *                  one arm per phase).
 * close()      -- detaches the handle from its slot (be_data = NULL);
 *                  the bus's per-address canned responses / captures /
 *                  armed faults are NOT cleared by close() (mirrors
 *                  the UART double leaving RX/TX state intact across a
 *                  close/re-open on the same id) -- only
 *                  alp_testing_reset_all() clears it.
 *
 * Fault mapping (alp_testing_bus_fault_t, <alp/testing/common.h>),
 * consumed exactly once per armed fault:
 *   - ALP_TESTING_FAULT_NACK    -> ALP_ERR_IO, 0 bytes transferred.
 *   - ALP_TESTING_FAULT_SHORT   -> ALP_ERR_IO, up to `short_len` bytes
 *                                  actually captured/filled.
 *   - ALP_TESTING_FAULT_TIMEOUT -> ALP_ERR_TIMEOUT immediately, nothing
 *                                  transferred.  alp_i2c_write/read/
 *                                  write_read take no caller timeout
 *                                  (unlike the UART double's read()),
 *                                  so there is no virtual-clock wait to
 *                                  resolve here -- the status is
 *                                  returned synchronously.
 *
 * The injection API (<alp/testing/i2c.h>) and the ops table share the
 * same per-bus slot via the generic instance table
 * (src/testing/instance_table.h), keyed by bus_id -- exactly the id
 * the app passes to alp_i2c_open() -- so a test can prime a device's
 * response or arm a fault before the app ever opens the bus.  Each
 * bus slot additionally holds a small fixed table of per-address
 * device entries (ALP_TESTING_I2C_MAX_ADDRS), since one real I2C
 * segment hosts several addressable devices at once.
 *
 * @par Cost: ROM ~1.5 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per bus lookup plus O(addr capacity)
 *      per address lookup (both linear scans); fine for the handful of
 *      buses/devices a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/testing/i2c.h>

#include "i2c_ops.h"
#include "instance_table.h"
#include "reset_registry.h"

#ifndef ALP_TESTING_I2C_MAX_BUSES
#define ALP_TESTING_I2C_MAX_BUSES 8
#endif

#ifndef ALP_TESTING_I2C_MAX_ADDRS
#define ALP_TESTING_I2C_MAX_ADDRS 4
#endif

#ifndef ALP_TESTING_I2C_MAX_CHUNK
#define ALP_TESTING_I2C_MAX_CHUNK 32
#endif

/* One simulated device on the bus: its canned read response, its
 * armed-for-the-next-transfer fault (if any), and the last bytes the
 * app under test wrote to it. */
typedef struct {
	bool                    used;
	uint8_t                 addr;
	uint8_t                 rsp[ALP_TESTING_I2C_MAX_CHUNK];
	size_t                  rsp_len;
	bool                    fail_armed;
	alp_testing_bus_fault_t fail_kind;
	size_t                  fail_short_len;
	uint8_t                 last_write[ALP_TESTING_I2C_MAX_CHUNK];
	size_t                  last_write_len;
} alp_testing_i2c_addr_t;

typedef struct {
	alp_testing_i2c_addr_t addrs[ALP_TESTING_I2C_MAX_ADDRS];
} alp_testing_i2c_slot_t;

static alp_testing_i2c_slot_t       g_slots[ALP_TESTING_I2C_MAX_BUSES];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_I2C_MAX_BUSES];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state
 * (no devices bound, empty responses, no armed faults) -- the
 * instance table's memset already produces it, so this has nothing
 * left to set.  Kept explicit (rather than NULL) to mirror the
 * UART double's style. */
static void slot_reset(void *slot_v)
{
	(void)slot_v;
}

/* Reset hook (mirrors src/backends/uart/testing_drv.c's
 * uart_reset_hook): re-clears every bound bus's per-address device
 * table on alp_testing_reset_all(), in addition to the sweep
 * alp_testing_instance_table_reset_all() already performs on this
 * table -- an explicit, standing, testable reset guarantee rather than
 * an implicit side effect of "this double owns no state outside the
 * table" (see the reset-frees-side-state regression in
 * tests/zephyr/conformance/src/behavior_i2c.c). */
static void i2c_reset_hook(void)
{
	alp_testing_instance_table_reset(&g_table);
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(
		    &g_table, g_slots, g_hdrs, sizeof(g_slots[0]), ALP_TESTING_I2C_MAX_BUSES, slot_reset);
		alp_testing_register_reset_hook(i2c_reset_hook);
		g_table_ready = true;
	}
	return &g_table;
}

static alp_testing_i2c_addr_t *addr_find(alp_testing_i2c_slot_t *slot, uint8_t addr)
{
	for (size_t i = 0; i < ALP_TESTING_I2C_MAX_ADDRS; ++i) {
		if (slot->addrs[i].used && slot->addrs[i].addr == addr) {
			return &slot->addrs[i];
		}
	}
	return NULL;
}

/* Create-on-first-touch within the bus's fixed per-address table.  A
 * newly-claimed entry's fields are already zero (either from the
 * parent bus slot's original memset, or from i2c_reset_hook's sweep),
 * so only `used`/`addr` need setting here. */
static alp_testing_i2c_addr_t *addr_touch(alp_testing_i2c_slot_t *slot, uint8_t addr)
{
	alp_testing_i2c_addr_t *dev = addr_find(slot, addr);
	if (dev != NULL) return dev;

	for (size_t i = 0; i < ALP_TESTING_I2C_MAX_ADDRS; ++i) {
		if (!slot->addrs[i].used) {
			slot->addrs[i].used = true;
			slot->addrs[i].addr = addr;
			return &slot->addrs[i];
		}
	}
	return NULL; /* every entry bound to a different address */
}

alp_status_t
alp_testing_i2c_target_respond(uint32_t bus_id, uint8_t addr, const uint8_t *rsp, size_t len)
{
	if (rsp == NULL && len > 0) return ALP_ERR_INVAL;
	if (len > ALP_TESTING_I2C_MAX_CHUNK) return ALP_ERR_NOMEM;

	alp_testing_i2c_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	alp_testing_i2c_addr_t *dev = addr_touch(slot, addr);
	if (dev == NULL) return ALP_ERR_NOMEM;

	if (len > 0) memcpy(dev->rsp, rsp, len);
	dev->rsp_len = len;
	return ALP_OK;
}

alp_status_t alp_testing_i2c_fail_next(uint32_t                bus_id,
                                       uint8_t                 addr,
                                       alp_testing_bus_fault_t f,
                                       size_t                  short_len)
{
	if (f != ALP_TESTING_FAULT_NACK && f != ALP_TESTING_FAULT_SHORT &&
	    f != ALP_TESTING_FAULT_TIMEOUT) {
		return ALP_ERR_INVAL;
	}

	alp_testing_i2c_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;
	alp_testing_i2c_addr_t *dev = addr_touch(slot, addr);
	if (dev == NULL) return ALP_ERR_NOMEM;

	dev->fail_armed     = true;
	dev->fail_kind      = f;
	dev->fail_short_len = short_len;
	return ALP_OK;
}

alp_status_t
alp_testing_i2c_last_write(uint32_t bus_id, uint8_t addr, uint8_t *out, size_t cap, size_t *got)
{
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0;
	if (out == NULL && cap > 0) return ALP_ERR_INVAL;

	alp_testing_i2c_slot_t *slot = alp_testing_instance_table_find(table(), bus_id);
	if (slot == NULL) return ALP_ERR_INVAL;
	alp_testing_i2c_addr_t *dev = addr_find(slot, addr);
	if (dev == NULL) return ALP_ERR_INVAL;

	size_t n = (cap < dev->last_write_len) ? cap : dev->last_write_len;
	if (n > 0) memcpy(out, dev->last_write, n);
	*got = n;
	return ALP_OK;
}

/* ---- alp_i2c_ops_t ---- */

static alp_status_t
t_open(const alp_i2c_config_t *cfg, alp_i2c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_i2c_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = slot;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
t_write(alp_i2c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	alp_testing_i2c_slot_t *slot = (alp_testing_i2c_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	alp_testing_i2c_addr_t *dev = addr_touch(slot, addr);
	if (dev == NULL) return ALP_ERR_NOMEM;

	if (dev->fail_armed) {
		alp_testing_bus_fault_t kind      = dev->fail_kind;
		size_t                  short_len = dev->fail_short_len;
		dev->fail_armed                   = false;
		if (kind == ALP_TESTING_FAULT_TIMEOUT) return ALP_ERR_TIMEOUT;

		size_t cap = (kind == ALP_TESTING_FAULT_SHORT) ? short_len : 0;
		if (cap > ALP_TESTING_I2C_MAX_CHUNK) cap = ALP_TESTING_I2C_MAX_CHUNK;
		size_t n = (cap < len) ? cap : len;
		if (n > 0) memcpy(dev->last_write, data, n);
		dev->last_write_len = n;
		return ALP_ERR_IO;
	}

	size_t n = (len < ALP_TESTING_I2C_MAX_CHUNK) ? len : ALP_TESTING_I2C_MAX_CHUNK;
	if (n > 0) memcpy(dev->last_write, data, n);
	dev->last_write_len = n;
	return ALP_OK;
}

static alp_status_t t_read(alp_i2c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	alp_testing_i2c_slot_t *slot = (alp_testing_i2c_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	alp_testing_i2c_addr_t *dev = addr_touch(slot, addr);
	if (dev == NULL) return ALP_ERR_NOMEM;

	if (dev->fail_armed) {
		alp_testing_bus_fault_t kind      = dev->fail_kind;
		size_t                  short_len = dev->fail_short_len;
		dev->fail_armed                   = false;
		if (kind == ALP_TESTING_FAULT_TIMEOUT) return ALP_ERR_TIMEOUT;

		if (len > 0) memset(data, 0, len);
		if (kind == ALP_TESTING_FAULT_SHORT) {
			size_t n = short_len;
			if (n > dev->rsp_len) n = dev->rsp_len;
			if (n > len) n = len;
			if (n > 0) memcpy(data, dev->rsp, n);
		}
		return ALP_ERR_IO;
	}

	if (len > 0) memset(data, 0, len);
	size_t n = (dev->rsp_len < len) ? dev->rsp_len : len;
	if (n > 0) memcpy(data, dev->rsp, n);
	return ALP_OK;
}

static alp_status_t t_write_read(alp_i2c_backend_state_t *st,
                                 uint8_t                  addr,
                                 const uint8_t           *wdata,
                                 size_t                   wlen,
                                 uint8_t                 *rdata,
                                 size_t                   rlen)
{
	alp_testing_i2c_slot_t *slot = (alp_testing_i2c_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;
	alp_testing_i2c_addr_t *dev = addr_touch(slot, addr);
	if (dev == NULL) return ALP_ERR_NOMEM;

	if (dev->fail_armed) {
		alp_testing_bus_fault_t kind      = dev->fail_kind;
		size_t                  short_len = dev->fail_short_len;
		dev->fail_armed                   = false;
		if (kind == ALP_TESTING_FAULT_TIMEOUT) return ALP_ERR_TIMEOUT;

		size_t wcap = (kind == ALP_TESTING_FAULT_SHORT) ? short_len : 0;
		if (wcap > ALP_TESTING_I2C_MAX_CHUNK) wcap = ALP_TESTING_I2C_MAX_CHUNK;
		size_t wn = (wcap < wlen) ? wcap : wlen;
		if (wn > 0) memcpy(dev->last_write, wdata, wn);
		dev->last_write_len = wn;

		if (rlen > 0) memset(rdata, 0, rlen);
		if (kind == ALP_TESTING_FAULT_SHORT) {
			size_t rn = short_len;
			if (rn > dev->rsp_len) rn = dev->rsp_len;
			if (rn > rlen) rn = rlen;
			if (rn > 0) memcpy(rdata, dev->rsp, rn);
		}
		return ALP_ERR_IO;
	}

	size_t wn = (wlen < ALP_TESTING_I2C_MAX_CHUNK) ? wlen : ALP_TESTING_I2C_MAX_CHUNK;
	if (wn > 0) memcpy(dev->last_write, wdata, wn);
	dev->last_write_len = wn;

	if (rlen > 0) memset(rdata, 0, rlen);
	size_t rn = (dev->rsp_len < rlen) ? dev->rsp_len : rlen;
	if (rn > 0) memcpy(rdata, dev->rsp, rn);
	return ALP_OK;
}

static void t_close(alp_i2c_backend_state_t *st)
{
	st->be_data = NULL;
}

static const alp_i2c_ops_t _ops = {
	.open         = t_open,
	.write        = t_write,
	.read         = t_read,
	.write_read   = t_write_read,
	.close        = t_close,
	.target_open  = NULL,
	.target_close = NULL,
};

ALP_BACKEND_REGISTER(i2c,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
