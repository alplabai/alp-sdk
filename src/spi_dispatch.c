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

#include "backends/spi/spi_ops.h"

ALP_BACKEND_DEFINE_CLASS(spi);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_SPI_HANDLES
#define CONFIG_ALP_SDK_MAX_SPI_HANDLES 4
#endif

static struct alp_spi  _pool[CONFIG_ALP_SDK_MAX_SPI_HANDLES];

static struct alp_spi *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_SPI_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_spi *h)
{
	h->in_use = false;
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

const alp_capabilities_t *alp_spi_capabilities(const alp_spi_t *bus)
{
	return (bus != NULL) ? &bus->cached_caps : NULL;
}
