/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI class dispatcher.  Routes the public alp_spi_* API
 * through the .alp_backends_spi registry.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/spi/spi_ops.h"

ALP_BACKEND_DEFINE_CLASS(spi);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_SPI_HANDLES
#define CONFIG_ALP_SDK_MAX_SPI_HANDLES 4
#endif

static struct alp_spi _pool[CONFIG_ALP_SDK_MAX_SPI_HANDLES];

static struct alp_spi *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_SPI_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_spi, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_spi *h)
{
	alp_slot_release(&h->in_use);
}

alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("spi", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_spi_ops_t *ops = (const alp_spi_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_spi *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->bus_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	return h;
}

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (bus == NULL || !bus->in_use) return ALP_ERR_NOT_READY;
	if (len == 0) return ALP_OK;
	return bus->state.ops->transceive(&bus->state, tx, rx, len);
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)
{
	return alp_spi_transceive(bus, tx, NULL, len);
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)
{
	return alp_spi_transceive(bus, NULL, rx, len);
}

void alp_spi_close(alp_spi_t *bus)
{
	if (bus == NULL || !bus->in_use) return;
	if (bus->state.ops != NULL && bus->state.ops->close != NULL) {
		bus->state.ops->close(&bus->state);
	}
	_free(bus);
}

/* ------------------------------------------------------------------ */
/* Target (slave) mode                                                 */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_SPI_TARGET_HANDLES
#define CONFIG_ALP_SDK_MAX_SPI_TARGET_HANDLES 2
#endif

static struct alp_spi_target _tpool[CONFIG_ALP_SDK_MAX_SPI_TARGET_HANDLES];

static struct alp_spi_target *_talloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_SPI_TARGET_HANDLES; ++i) {
		if (alp_slot_try_claim(&_tpool[i].in_use)) {
			/* Zeroing everything before in_use also parks the
			 * lifecycle byte at LC_UNOPENED, so a stale handle
			 * pointer observes NOT_READY until open completes. */
			memset(&_tpool[i], 0, offsetof(struct alp_spi_target, in_use));
			return &_tpool[i];
		}
	}
	return NULL;
}

alp_spi_target_t *alp_spi_target_open(const alp_spi_target_config_t *cfg)
{
	alp_z_clear_last_error();
	/* Portable config validation (covers every backend): only SPI
	 * modes 0..3 exist, and bits_per_word tops out at 32 -- the
	 * widest frame the byte-count reporting (and Zephyr's 16-bit
	 * operation word) supports.  0 means "default to 8". */
	if (cfg == NULL || (unsigned)cfg->mode > (unsigned)ALP_SPI_MODE_3 || cfg->bits_per_word > 32u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("spi", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_spi_ops_t *ops = (const alp_spi_ops_t *)be->ops;
	if (ops == NULL || ops->target_open == NULL || ops->target_transceive == NULL ||
	    ops->target_close == NULL) {
		/* Backend has no slave mode (e.g. the native_sim
		 * sw_fallback) -- degrade gracefully. */
		alp_z_set_last_error(ALP_ERR_NOSUPPORT);
		return NULL;
	}
	struct alp_spi_target *h = _talloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend      = be;
	h->state.ops    = ops;
	alp_status_t rc = ops->target_open(cfg, &h->state);
	if (rc != ALP_OK) {
		alp_slot_release(&h->in_use);
		alp_z_set_last_error(rc);
		return NULL;
	}
	alp_lifecycle_set(&h->lifecycle, ALP_SPI_TARGET_LC_IDLE);
	return h;
}

alp_status_t alp_spi_target_transceive(alp_spi_target_t *bus,
                                       const uint8_t    *tx,
                                       uint8_t          *rx,
                                       size_t            len,
                                       size_t           *rx_len,
                                       uint32_t          timeout_ms)
{
	if (rx_len != NULL) *rx_len = 0;
	if (bus == NULL) return ALP_ERR_NOT_READY;
	/* Claim the handle for the duration of the (possibly blocking)
	 * backend call.  A parallel transceive sees XFER -> BUSY; a
	 * closed / never-opened handle sees UNOPENED/CLOSING -> NOT_READY. */
	if (!alp_lifecycle_cas(&bus->lifecycle, ALP_SPI_TARGET_LC_IDLE, ALP_SPI_TARGET_LC_XFER)) {
		return (alp_lifecycle_get(&bus->lifecycle) == ALP_SPI_TARGET_LC_XFER) ? ALP_ERR_BUSY
		                                                                      : ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (len == 0 || (tx == NULL && rx == NULL)) {
		/* len == 0 is INVAL here (unlike controller-mode
		 * alp_spi_transceive): a target cannot stage a zero-length
		 * transfer for the external controller to clock. */
		rc = ALP_ERR_INVAL;
	} else {
		rc = bus->state.ops->target_transceive(&bus->state, tx, rx, len, rx_len, timeout_ms);
	}
	alp_lifecycle_set(&bus->lifecycle, ALP_SPI_TARGET_LC_IDLE);
	return rc;
}

alp_status_t alp_spi_target_close(alp_spi_target_t *tgt)
{
	if (tgt == NULL) return ALP_OK; /* idempotent, like every alp_*_close */
	if (!alp_lifecycle_cas(&tgt->lifecycle, ALP_SPI_TARGET_LC_IDLE, ALP_SPI_TARGET_LC_CLOSING)) {
		/* A transceive is blocked in the backend: freeing the handle
		 * now would yank state out from under the driver -- refuse. */
		if (alp_lifecycle_get(&tgt->lifecycle) == ALP_SPI_TARGET_LC_XFER) {
			return ALP_ERR_BUSY;
		}
		return ALP_OK; /* already closed / closing elsewhere */
	}
	alp_status_t rc = tgt->state.ops->target_close(&tgt->state);
	if (rc != ALP_OK) {
		/* Backend still owns a timed-out transfer (armed in the
		 * driver).  Keep the handle alive so nothing is freed while
		 * the driver may still complete it. */
		alp_lifecycle_set(&tgt->lifecycle, ALP_SPI_TARGET_LC_IDLE);
		return rc;
	}
	alp_lifecycle_set(&tgt->lifecycle, ALP_SPI_TARGET_LC_UNOPENED);
	alp_slot_release(&tgt->in_use);
	return ALP_OK;
}

const alp_capabilities_t *alp_spi_capabilities(const alp_spi_t *bus)
{
	return (bus != NULL) ? &bus->cached_caps : NULL;
}
