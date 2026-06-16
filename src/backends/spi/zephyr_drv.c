/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr spi_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/spi_dispatch.c; the
 * backend's open fills state->dev, allocates a per-handle sidecar
 * (spi_config + cs_ctrl + cs_spec + cs_present), and resolves the
 * chip-select GPIO if one is specified.
 *
 * The sidecar is reached via state->be_data so the portable
 * dispatcher TU + struct alp_spi layout never pull in
 * <zephyr/drivers/spi.h> or <zephyr/drivers/gpio.h>.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "spi_ops.h"

#define ALP_SPI_DEV_OR_NULL(idx)                                                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_spi, idx))),                                   \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_spi, idx)))), (NULL))

static const struct device *const _devs[] = {
	ALP_SPI_DEV_OR_NULL(0), ALP_SPI_DEV_OR_NULL(1), ALP_SPI_DEV_OR_NULL(2), ALP_SPI_DEV_OR_NULL(3),
	ALP_SPI_DEV_OR_NULL(4), ALP_SPI_DEV_OR_NULL(5), ALP_SPI_DEV_OR_NULL(6), ALP_SPI_DEV_OR_NULL(7),
};

/* Re-use the GPIO resolution helper from src/backends/gpio/zephyr_drv.c.
 * Weak fallback so the SPI backend still links when the GPIO backend
 * isn't compiled in (CONFIG_GPIO=n -- e.g. the gd32-bridge example
 * that uses SPI without a CS pin).  At runtime, callers must gate on
 * cfg->cs_pin_id != ALP_SPI_NO_CS to avoid touching the stub. */
__attribute__((weak)) bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out)
{
	(void)pin_id;
	(void)out;
	return false;
}

/* ALP_SPI_NO_CS comes from <alp/peripheral.h>. */

#ifndef CONFIG_ALP_SDK_MAX_SPI_HANDLES
#define CONFIG_ALP_SDK_MAX_SPI_HANDLES 4
#endif

/* Per-handle Zephyr sidecar.  One slot per handle in the dispatcher
 * pool; the dispatcher passes the slot via state->be_data (set at
 * open() time). */
typedef struct {
	struct spi_config     zspi_cfg;
	struct spi_cs_control cs_ctrl;
	struct gpio_dt_spec   cs_spec; /* zeroed when no CS gpio resolved */
	bool                  cs_present;
	bool                  in_use;
} alp_z_spi_side_t;

static alp_z_spi_side_t  _sides[CONFIG_ALP_SDK_MAX_SPI_HANDLES];

static alp_z_spi_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
		if (!_sides[i].in_use) {
			_sides[i]        = (alp_z_spi_side_t){ 0 };
			_sides[i].in_use = true;
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(alp_z_spi_side_t *s)
{
	if (s != NULL) s->in_use = false;
}

static uint16_t _to_spi_op(const alp_spi_config_t *cfg)
{
	uint16_t op = SPI_WORD_SET(cfg->bits_per_word ? cfg->bits_per_word : 8);
	op |= SPI_OP_MODE_MASTER;
	op |= SPI_TRANSFER_MSB;
	if (cfg->mode & 0x2u) op |= SPI_MODE_CPOL;
	if (cfg->mode & 0x1u) op |= SPI_MODE_CPHA;
	return op;
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t z_open(const alp_spi_config_t *cfg, alp_spi_backend_state_t *st,
                           alp_capabilities_t *caps_out)
{
	if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->bus_id >= ALP_SOC_SPI_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

	alp_z_spi_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	s->zspi_cfg.frequency = cfg->freq_hz ? cfg->freq_hz : 1000000u;
	s->zspi_cfg.operation = _to_spi_op(cfg);
	s->zspi_cfg.slave     = 0;

	if (cfg->cs_pin_id != ALP_SPI_NO_CS && alp_z_gpio_resolve(cfg->cs_pin_id, &s->cs_spec)) {
		if (!device_is_ready(s->cs_spec.port)) {
			_free_side(s);
			return ALP_ERR_NOT_READY;
		}
		gpio_pin_configure_dt(&s->cs_spec, GPIO_OUTPUT_INACTIVE);
		s->cs_ctrl.gpio = s->cs_spec;
		/* CS setup/hold window: spi_context busy-waits this long after
         * asserting and before deasserting CS.  Slaves that frame
         * transactions on CS edges in software (e.g. the GD32 bridge's
         * NSS->EXTI preload/decode ISRs) need the first/last SCK held
         * off while their edge handler runs.  The GD32 link sizes its
         * own windows in the SCI-B driver's direct-CS shim
         * (ALP_V2N_CS_SETUP_US/HOLD_US, bench-validated 2026-06-04);
         * this generic gpio-CS path only needs to outlast ordinary
         * slave CS-edge latching -- 2 us is ample (the previous 60 us
         * was the GD32 bring-up value stacking on top of the shim's). */
		s->cs_ctrl.delay = 2;
		s->zspi_cfg.cs   = s->cs_ctrl;
		s->cs_present    = true;
	}

	st->dev         = (void *)dev;
	st->bus_id      = cfg->bus_id;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_transceive(alp_spi_backend_state_t *st, const uint8_t *tx, uint8_t *rx,
                                 size_t len)
{
	const struct device *dev = (const struct device *)st->dev;
	alp_z_spi_side_t    *s   = (alp_z_spi_side_t *)st->be_data;
	if (s == NULL) return ALP_ERR_NOT_READY;

	struct spi_buf     tx_buf = { .buf = (void *)tx, .len = (tx != NULL) ? len : 0 };
	struct spi_buf     rx_buf = { .buf = rx, .len = (rx != NULL) ? len : 0 };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	int                err    = spi_transceive(dev, &s->zspi_cfg, (tx != NULL) ? &tx_set : NULL,
                             (rx != NULL) ? &rx_set : NULL);
	return _errno_to_alp(err);
}

static void z_close(alp_spi_backend_state_t *st)
{
	alp_z_spi_side_t *s = (alp_z_spi_side_t *)st->be_data;
	_free_side(s);
	st->be_data = NULL;
}

static const alp_spi_ops_t _ops = {
	.open       = z_open,
	.transceive = z_transceive,
	.close      = z_close,
};

ALP_BACKEND_REGISTER(spi, zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
