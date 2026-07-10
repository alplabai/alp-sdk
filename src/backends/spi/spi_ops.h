/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_spi dispatcher and per-backend
 * implementations.  NOT a public header.
 *
 * Zephyr leakage: state->dev is typed void* and the per-handle
 * Zephyr SPI config (spi_config + spi_cs_control + gpio_dt_spec)
 * lives in a backend-private sidecar reached via state.be_data
 * inside src/backends/spi/zephyr_drv.c.  This keeps the portable
 * dispatcher TU and the struct alp_spi layout free of
 * <zephyr/device.h>, <zephyr/drivers/gpio.h>, and
 * <zephyr/drivers/spi.h>.
 */

#ifndef ALP_BACKENDS_SPI_OPS_H
#define ALP_BACKENDS_SPI_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_spi_ops alp_spi_ops_t;

typedef struct alp_spi_backend_state {
	void                *dev; /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t             bus_id;
	void                *be_data; /* per-handle backend sidecar
                                          * (Zephyr backend stashes spi_config +
                                          * cs_ctrl + cs_spec + cs_present here) */
	const alp_spi_ops_t *ops;
} alp_spi_backend_state_t;

struct alp_spi_ops {
	alp_status_t (*open)(const alp_spi_config_t  *cfg,
	                     alp_spi_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*transceive)(alp_spi_backend_state_t *state,
	                           const uint8_t           *tx,
	                           uint8_t                 *rx,
	                           size_t                   len);
	void (*close)(alp_spi_backend_state_t *state);
	/* Target (slave) mode -- optional.  Backends without slave
	 * support leave all three NULL; the dispatcher then fails
	 * alp_spi_target_open with ALP_ERR_NOSUPPORT.
	 *
	 * target_transceive: rx_len is BYTES (the backend converts from
	 * whatever frame unit its driver reports); timeout_ms bounds the
	 * wait, UINT32_MAX means block until the controller clocks the
	 * transfer.
	 *
	 * target_close returns ALP_ERR_BUSY when a timed-out transfer is
	 * still armed in the controller driver (the dispatcher then keeps
	 * the handle alive instead of freeing state under the driver). */
	alp_status_t (*target_open)(const alp_spi_target_config_t *cfg, alp_spi_backend_state_t *state);
	alp_status_t (*target_transceive)(alp_spi_backend_state_t *state,
	                                  const uint8_t           *tx,
	                                  uint8_t                 *rx,
	                                  size_t                   len,
	                                  size_t                  *rx_len,
	                                  uint32_t                 timeout_ms);
	alp_status_t (*target_close)(alp_spi_backend_state_t *state);
};

/* lifecycle/active_ops (distinct namespace from ALP_SPI_TARGET_LC_*
 * below -- see src/common/alp_slot_claim.h's ALP_HANDLE_LC_*) drive
 * the generic open/op/close guard for CONTROLLER-mode handles:
 * alp_spi_transceive used to gate solely on the atomically-claimed
 * in_use flag with a PLAIN read, so a racing alp_spi_close() could
 * free the slot out from under an in-flight transceive (issue #629).
 * Placed before in_use so the atomic-claim zeroing in
 * src/spi_dispatch.c (memset up to offsetof(..., in_use)) resets both
 * on every fresh claim. */
struct alp_spi {
	alp_spi_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	uint8_t                 lifecycle;
	uint32_t                active_ops;
	bool                    in_use;
};

/* Lifecycle states for struct alp_spi_target.  Driven atomically by
 * src/spi_dispatch.c (see src/common/alp_slot_claim.h): a transceive
 * may only start from IDLE, and close may only proceed from IDLE, so
 * "close while another thread is blocked in transceive" is refused
 * with ALP_ERR_BUSY instead of freeing state under the driver. */
#define ALP_SPI_TARGET_LC_UNOPENED 0u /* slot claimed but open unfinished / closed */
#define ALP_SPI_TARGET_LC_IDLE     1u
#define ALP_SPI_TARGET_LC_XFER     2u /* a transceive is blocked in the backend */
#define ALP_SPI_TARGET_LC_CLOSING  3u

struct alp_spi_target {
	alp_spi_backend_state_t state;
	const alp_backend_t    *backend;
	uint8_t                 lifecycle; /* ALP_SPI_TARGET_LC_*; atomic access only */
	bool                    in_use;
};

#endif /* ALP_BACKENDS_SPI_OPS_H */
