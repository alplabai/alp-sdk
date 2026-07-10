/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI test double.  Ordinary alp_spi_ops_t backend registered at the
 * reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_SPI=y it outranks every real/proxy/fallback
 * SPI backend and alp_spi_open() rides on it transparently -- see the
 * priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * This double simulates the DEVICE on the bus, not this MCU's own
 * target (slave) mode: it implements only the CONTROLLER-mode ops
 * (open/transceive/close, which alp_spi_write/alp_spi_read
 * (<alp/peripheral.h>) are thin wrappers over). target_open/
 * target_transceive/target_close are left NULL, so
 * alp_spi_target_open() fails ALP_ERR_NOSUPPORT on this backend,
 * exactly like a backend with no target support at all (see the
 * comment on spi_ops.h's alp_spi_ops).
 *
 * open()        -- ALWAYS succeeds.  Same deliberate ergonomic choice
 *                   as the GPIO/UART/I2C doubles: a "*" silicon_ref
 *                   double has no valid-bus-range knowledge to reject
 *                   an id with, and the injection API
 *                   (<alp/testing/spi.h>) needs inject-before-open to
 *                   work.
 * transceive()  -- full-duplex, per <alp/peripheral.h>'s
 *                   alp_spi_transceive contract: a non-NULL `tx`
 *                   captures up to ALP_TESTING_SPI_MAX_CHUNK bytes into
 *                   the bus's "last MOSI" snapshot (NOT a drained
 *                   queue -- read back any time via
 *                   alp_testing_spi_last_mosi()); a non-NULL `rx` is
 *                   filled from the bus's canned MISO snapshot
 *                   (alp_testing_spi_load_miso(), also persistent, not
 *                   consumed), zero-padding a request longer than the
 *                   canned data. A NULL `tx` (the alp_spi_read()
 *                   wrapper) leaves the MOSI snapshot untouched; a NULL
 *                   `rx` (the alp_spi_write() wrapper) is simply not
 *                   filled. Subject to a one-shot fault if armed.
 * close()       -- detaches the handle from its slot (be_data = NULL);
 *                   the bus's canned MISO / MOSI capture / armed fault
 *                   are NOT cleared by close() (mirrors the UART/I2C
 *                   doubles leaving bus-level state intact across a
 *                   close/re-open on the same id) -- only
 *                   alp_testing_reset_all() clears it.
 *
 * Fault mapping (alp_testing_bus_fault_t, <alp/testing/common.h>),
 * consumed exactly once per armed fault:
 *   - ALP_TESTING_FAULT_NACK    -> ALP_ERR_IO, 0 bytes transferred
 *                                  (models the simulated device never
 *                                  responding at all -- SPI has no
 *                                  wire-level ACK/NACK, so this is a
 *                                  modelling choice, not a literal one).
 *   - ALP_TESTING_FAULT_SHORT   -> ALP_ERR_IO, up to `short_len` bytes
 *                                  actually captured/filled (the same
 *                                  `short_len` bounds both directions
 *                                  of the one full-duplex transfer).
 *   - ALP_TESTING_FAULT_TIMEOUT -> ALP_ERR_TIMEOUT immediately, nothing
 *                                  transferred.  alp_spi_transceive
 *                                  takes no caller timeout (unlike the
 *                                  UART double's read() or this SDK's
 *                                  own alp_spi_target_transceive), so
 *                                  there is no virtual-clock wait to
 *                                  resolve here -- the status is
 *                                  returned synchronously.
 *
 * The injection API (<alp/testing/spi.h>) and the ops table share the
 * same per-bus slot via the generic instance table
 * (src/testing/instance_table.h), keyed by bus_id -- exactly the id
 * the app passes to alp_spi_open() -- so a test can load a MISO
 * response or arm a fault before the app ever opens the bus. Unlike
 * I2C, a bus handle already identifies exactly one device (selected by
 * its own chip-select at open time), so there is no per-address
 * sub-table here.
 *
 * @par Cost: ROM ~1 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per call (linear slot scan); fine for
 *      the handful of buses a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/testing/spi.h>

#include "instance_table.h"
#include "reset_registry.h"
#include "spi_ops.h"

#ifndef ALP_TESTING_SPI_MAX_BUSES
#define ALP_TESTING_SPI_MAX_BUSES 8
#endif

#ifndef ALP_TESTING_SPI_MAX_CHUNK
#define ALP_TESTING_SPI_MAX_CHUNK 64
#endif

typedef struct {
	uint8_t                 miso[ALP_TESTING_SPI_MAX_CHUNK];
	size_t                  miso_len;
	bool                    fail_armed;
	alp_testing_bus_fault_t fail_kind;
	size_t                  fail_short_len;
	uint8_t                 last_mosi[ALP_TESTING_SPI_MAX_CHUNK];
	size_t                  last_mosi_len;
} alp_testing_spi_slot_t;

static alp_testing_spi_slot_t       g_slots[ALP_TESTING_SPI_MAX_BUSES];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_SPI_MAX_BUSES];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state
 * (empty MISO/MOSI snapshots, no armed fault) -- the instance table's
 * memset already produces it, so this has nothing left to set.  Kept
 * explicit (rather than NULL) to mirror the UART/I2C doubles' style. */
static void slot_reset(void *slot_v)
{
	(void)slot_v;
}

/* Reset hook (mirrors src/backends/i2c/testing_drv.c's
 * i2c_reset_hook, itself mirroring src/backends/uart/testing_drv.c):
 * re-clears every bound bus's side-state on alp_testing_reset_all(),
 * in addition to the sweep alp_testing_instance_table_reset_all()
 * already performs on this table -- an explicit, standing, testable
 * reset guarantee (see the reset-frees-side-state regression in
 * tests/zephyr/conformance/src/behavior_spi.c). */
static void spi_reset_hook(void)
{
	alp_testing_instance_table_reset(&g_table);
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(
		    &g_table, g_slots, g_hdrs, sizeof(g_slots[0]), ALP_TESTING_SPI_MAX_BUSES, slot_reset);
		alp_testing_register_reset_hook(spi_reset_hook);
		g_table_ready = true;
	}
	return &g_table;
}

alp_status_t alp_testing_spi_load_miso(uint32_t bus_id, const uint8_t *d, size_t len)
{
	if (d == NULL && len > 0) return ALP_ERR_INVAL;
	if (len > ALP_TESTING_SPI_MAX_CHUNK) return ALP_ERR_NOMEM;

	alp_testing_spi_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	if (len > 0) memcpy(slot->miso, d, len);
	slot->miso_len = len;
	return ALP_OK;
}

alp_status_t alp_testing_spi_fail_next(uint32_t bus_id, alp_testing_bus_fault_t f, size_t short_len)
{
	if (f != ALP_TESTING_FAULT_NACK && f != ALP_TESTING_FAULT_SHORT &&
	    f != ALP_TESTING_FAULT_TIMEOUT) {
		return ALP_ERR_INVAL;
	}

	alp_testing_spi_slot_t *slot = alp_testing_instance_table_touch(table(), bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	slot->fail_armed     = true;
	slot->fail_kind      = f;
	slot->fail_short_len = short_len;
	return ALP_OK;
}

alp_status_t alp_testing_spi_last_mosi(uint32_t bus_id, uint8_t *out, size_t cap, size_t *got)
{
	if (got == NULL) return ALP_ERR_INVAL;
	*got = 0;
	if (out == NULL && cap > 0) return ALP_ERR_INVAL;

	alp_testing_spi_slot_t *slot = alp_testing_instance_table_find(table(), bus_id);
	if (slot == NULL) return ALP_ERR_INVAL;

	size_t n = (cap < slot->last_mosi_len) ? cap : slot->last_mosi_len;
	if (n > 0) memcpy(out, slot->last_mosi, n);
	*got = n;
	return ALP_OK;
}

/* ---- alp_spi_ops_t ---- */

static alp_status_t
t_open(const alp_spi_config_t *cfg, alp_spi_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_spi_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->bus_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = slot;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
t_transceive(alp_spi_backend_state_t *st, const uint8_t *tx, uint8_t *rx, size_t len)
{
	alp_testing_spi_slot_t *slot = (alp_testing_spi_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	if (slot->fail_armed) {
		alp_testing_bus_fault_t kind      = slot->fail_kind;
		size_t                  short_len = slot->fail_short_len;
		slot->fail_armed                  = false;
		if (kind == ALP_TESTING_FAULT_TIMEOUT) return ALP_ERR_TIMEOUT;

		size_t cap = (kind == ALP_TESTING_FAULT_SHORT) ? short_len : 0;
		if (cap > ALP_TESTING_SPI_MAX_CHUNK) cap = ALP_TESTING_SPI_MAX_CHUNK;
		size_t n = (cap < len) ? cap : len;

		if (tx != NULL) {
			if (n > 0) memcpy(slot->last_mosi, tx, n);
			slot->last_mosi_len = n;
		}
		if (rx != NULL) {
			memset(rx, 0, len);
			size_t rn = (n < slot->miso_len) ? n : slot->miso_len;
			if (rn > 0) memcpy(rx, slot->miso, rn);
		}
		return ALP_ERR_IO;
	}

	if (tx != NULL) {
		size_t n = (len < ALP_TESTING_SPI_MAX_CHUNK) ? len : ALP_TESTING_SPI_MAX_CHUNK;
		if (n > 0) memcpy(slot->last_mosi, tx, n);
		slot->last_mosi_len = n;
	}
	if (rx != NULL) {
		memset(rx, 0, len);
		size_t n = (slot->miso_len < len) ? slot->miso_len : len;
		if (n > 0) memcpy(rx, slot->miso, n);
	}
	return ALP_OK;
}

static void t_close(alp_spi_backend_state_t *st)
{
	st->be_data = NULL;
}

static const alp_spi_ops_t _ops = {
	.open              = t_open,
	.transceive        = t_transceive,
	.close             = t_close,
	.target_open       = NULL,
	.target_transceive = NULL,
	.target_close      = NULL,
};

ALP_BACKEND_REGISTER(spi,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
