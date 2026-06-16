/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software SPI fallback.  Deterministic in-memory loopback for
 * native_sim builds; not a real bus.
 *
 * transceive(tx, rx, len) -- copies tx into rx when both non-NULL
 *   (echo loopback); tx-only (rx=NULL) is a no-op; rx-only (tx=NULL)
 *   zero-fills the receive buffer.
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_SPI_SW_FALLBACK=y with no Zephyr
 * SPI devices present.
 *
 * @par Cost: ROM ~300 B, RAM 0 bytes (no static frame buffer needed;
 *      loopback operates purely on caller-supplied buffers).
 * @par Performance: O(len) per transceive (memcpy); deterministic
 *      for test assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "spi_ops.h"

static alp_status_t sw_open(const alp_spi_config_t *cfg, alp_spi_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
	(void)cfg;
	st->dev         = NULL;
	st->bus_id      = 0u;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_transceive(alp_spi_backend_state_t *st, const uint8_t *tx, uint8_t *rx,
                                  size_t len)
{
	(void)st;
	if (len == 0u) return ALP_OK;
	if (tx != NULL && rx != NULL) {
		memcpy(rx, tx, len);
	} else if (rx != NULL) {
		/* rx-only: no tx data -- zero-fill receive buffer */
		memset(rx, 0, len);
	}
	/* tx-only (rx == NULL): no-op */
	return ALP_OK;
}

static const alp_spi_ops_t _ops = {
	.open       = sw_open,
	.transceive = sw_transceive,
	.close      = NULL,
};

ALP_BACKEND_REGISTER(spi, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
