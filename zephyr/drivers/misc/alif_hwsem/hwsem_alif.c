// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 Alp Lab AB
 *
 * Zephyr driver for the Alif Ensemble hardware-semaphore (HWSEM) -- an AMP
 * mutual-exclusion latch shared between the two RTSS cores (M55-HE <-> M55-HP).
 * Each of the 16 HWSEM instances is an atomic request/release latch keyed by a
 * per-core MASTER ID; it needs no shared-SRAM spin and survives the absence of
 * cache coherency between the cores. Exposes a tiny take/give API
 * (hwsem_alif.h) -- HWSEM has NO upstream Zephyr device class, so this is a
 * "misc" (no-class) driver, like drivers/misc/* upstream.
 *
 * ============================== STATUS ==============================
 * ADR-0017-ADJACENT (vendor-native custom): the Alif HWSEM IP has NO upstream
 * Zephyr driver, no sdk-alif fork Zephyr driver, and no hal_alif library to
 * consume -- only a bare DFP register header. So it does not fit Tier-1/1.5/2/3
 * cleanly; authored from spec as a last resort per ADR 0017
 * (docs/adr/0017-alp-sdk-over-the-vendor-sdk.md). vendor-ext, BENCH-UNVERIFIED.
 *
 * Every register offset below is transcribed CLEAN-ROOM (value only, no source
 * copied) from the Alif DFP HWSEM register header, struct HWSEM_Type in
 *   alif-dfp/drivers/include/hwsem.h
 * and corroborated by the same struct in
 *   alif-dfp/Device/soc/AE822FA0E5597/include/rtss_he/soc.h (HWSEM0 @0x4902E000)
 * The per-instance bases live in that soc.h too (HWSEM0_BASE 0x4902E000 ..
 * HWSEM15_BASE 0x4902E0F0, stride 0x10) but the driver takes its base from the
 * DT node's reg, so no base is hard-coded here. No offset has been invented.
 * It has NOT been run on real silicon.
 * ====================================================================
 */

#define DT_DRV_COMPAT alif_hwsem

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>

#include "hwsem_alif.h"

/*
 * HWSEM register map -- offsets from the instance base. Transcribed VERBATIM
 * (offset values only) from struct HWSEM_Type in the Alif DFP header
 * alif-dfp/drivers/include/hwsem.h:
 *   __IOM uint32_t HWSEM_REQ_REG;  (@ 0x00000000)  Request register
 *   __IOM uint32_t HWSEM_REL_REG;  (@ 0x00000004)  Release register
 *   __OM  uint32_t HWSEM_RST_REG;  (@ 0x00000008)  Reset register
 * Struct size = 12 (0x0C); instance stride 0x10 (per HWSEMn_BASE in
 * alif-dfp/Device/soc/AE822FA0E5597/include/rtss_he/soc.h). No value invented.
 */
#define HWSEM_REQ_REG 0x00U /* RW request: write master_id to take */
#define HWSEM_REL_REG 0x04U /* RW release: write master_id to give; read = count */
#define HWSEM_RST_REG 0x08U /* WO reset:   write 1 to force-free the latch */

/* Reset register magic -- from hwsem_reset() in the DFP header:
 *   hwsem->HWSEM_RST_REG = 0x1U;
 */
#define HWSEM_RST_MAGIC 0x1U

/** @brief Per-instance immutable configuration. */
struct hwsem_alif_config {
	/** Mapped base address of this HWSEM instance's register window. */
	mm_reg_t base;
};

/*
 * take(): per hwsem_request() in the DFP header --
 *   hwsem->HWSEM_REQ_REG = master_id;       (request ownership)
 *   return hwsem->HWSEM_REQ_REG;            (readback == granted owner id)
 * Ownership is granted iff the readback equals the requested master_id.
 */
int hwsem_alif_take(const struct device *dev, uint32_t master_id)
{
	const struct hwsem_alif_config *cfg;
	uint32_t                        owner;

	if (dev == NULL || !device_is_ready(dev)) {
		return -EINVAL;
	}
	cfg = dev->config;

	/* Request, then read back the current owner id. */
	sys_write32(master_id, cfg->base + HWSEM_REQ_REG);
	owner = sys_read32(cfg->base + HWSEM_REQ_REG);

	return (owner == master_id) ? 0 : -EBUSY;
}

int hwsem_alif_take_busy(const struct device *dev, uint32_t master_id, uint32_t max_spins)
{
	int rc;

	if (dev == NULL || !device_is_ready(dev) || max_spins == 0U) {
		return -EINVAL;
	}

	/* Bounded retry: each failure means the peer core owns the latch. The
	 * bound keeps a dead/absent peer from hanging us. */
	for (uint32_t i = 0U; i < max_spins; i++) {
		rc = hwsem_alif_take(dev, master_id);
		if (rc == 0) {
			return 0;
		}
	}

	return -EBUSY;
}

/*
 * give(): per hwsem_release() in the DFP header --
 *   hwsem->HWSEM_REL_REG = master_id;       (release ownership)
 */
int hwsem_alif_give(const struct device *dev, uint32_t master_id)
{
	const struct hwsem_alif_config *cfg;

	if (dev == NULL || !device_is_ready(dev)) {
		return -EINVAL;
	}
	cfg = dev->config;

	sys_write32(master_id, cfg->base + HWSEM_REL_REG);

	return 0;
}

/*
 * get_count(): per hwsem_getcount() in the DFP header --
 *   return hwsem->HWSEM_REL_REG;            (current latch count: 0 == free)
 */
int hwsem_alif_get_count(const struct device *dev, uint32_t *count)
{
	const struct hwsem_alif_config *cfg;

	if (dev == NULL || !device_is_ready(dev) || count == NULL) {
		return -EINVAL;
	}
	cfg = dev->config;

	*count = sys_read32(cfg->base + HWSEM_REL_REG);

	return 0;
}

/*
 * reset(): per hwsem_reset() in the DFP header --
 *   hwsem->HWSEM_RST_REG = 0x1U;            (force-free regardless of owner)
 */
int hwsem_alif_reset(const struct device *dev)
{
	const struct hwsem_alif_config *cfg;

	if (dev == NULL || !device_is_ready(dev)) {
		return -EINVAL;
	}
	cfg = dev->config;

	sys_write32(HWSEM_RST_MAGIC, cfg->base + HWSEM_RST_REG);

	return 0;
}

/* No init-time register writes: the latch powers up free, and a take/give pair
 * is fully driven by the API above. init returns 0 so the device is ready. */
static int hwsem_alif_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

#define HWSEM_ALIF_INST(inst)                                                                      \
	static const struct hwsem_alif_config hwsem_alif_config_##inst = {                             \
		.base = (mm_reg_t)DT_INST_REG_ADDR(inst),                                                  \
	};                                                                                             \
	DEVICE_DT_INST_DEFINE(inst,                                                                    \
	                      hwsem_alif_init,                                                         \
	                      NULL,                                                                    \
	                      NULL,                                                                    \
	                      &hwsem_alif_config_##inst,                                               \
	                      POST_KERNEL,                                                             \
	                      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                                      \
	                      NULL);

DT_INST_FOREACH_STATUS_OKAY(HWSEM_ALIF_INST)
