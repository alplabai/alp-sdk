/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenAMP resource table for the RZ/V2N M33-SM <-> A55/Linux rpmsg link
 * (alp-sdk #683, Path B Phase 1).
 *
 * Adapted near-verbatim from Renesas's own
 * zephyr/samples/boards/renesas/openamp_linux_zephyr/src/resource_table.h --
 * this header has no RZ/V2L-specific content (only the generic OpenAMP
 * vdev/vring shapes + a CM33<->A55 address helper), so porting to V2N is a
 * straight reuse.  The board-level `vring_ctrl0` / `vring_ctrl1` devicetree
 * nodes this file reads addresses from are declared in this project's board
 * overlay (zephyr/boards/alp/e1m_v2n101_m33_sm/...cm33.dts).
 *
 * Per the commit this comment block in the vendor header references
 * (25ec73986b, "lib: open-amp: add helper to add resource table in
 * project"): vring TX/RX addresses are allocated by the rpmsg MASTER (the
 * A55/Linux side here), so this slave publishes them in its own resource
 * table for the master to read back -- CONFIG_OPENAMP_RSC_TABLE=n in this
 * example's prj.conf (see main.c) so nothing overwrites this table with a
 * Zephyr-generated one.
 */

#ifndef RESOURCE_TABLE_H__
#define RESOURCE_TABLE_H__

#include <openamp/remoteproc.h>
#include <openamp/virtio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDEV_ID   0xFF
#define VRING0_ID 0
#define VRING1_ID 1

#define VRING_COUNT           2
#define RPMSG_IPU_C0_FEATURES 1

#define R_VRING_TX         DT_NODELABEL(vring_ctrl0)
#define R_VRING_RX         DT_NODELABEL(vring_ctrl1)
#define VRING_TX_ADDR_CM33 DT_REG_ADDR(R_VRING_TX)
#define VRING_RX_ADDR_CM33 DT_REG_ADDR(R_VRING_RX)

/*
 * CM33 <-> A55 address-space translation (HW manual Table 5.2): CM33
 * addresses in the non-secure alias window read/write the same physical
 * memory as the A55 sees at (addr - 0x20000000); the secure alias is
 * (addr - 0x30000000).  Only the non-secure helper is used here -- this
 * link runs entirely in the non-secure world on both cores.
 */
#ifndef _ASMLANGUAGE

#define CM33_ADDRESS_OFFSET_SECURE    (0x30000000)
#define CM33_ADDRESS_OFFSET_NONSECURE (0x20000000)
#define CM33_TO_A55_ADDR_S(x)         ((x) - CM33_ADDRESS_OFFSET_SECURE)
#define CM33_TO_A55_ADDR_NS(x)        ((x) - CM33_ADDRESS_OFFSET_NONSECURE)

#endif

#define VRING_TX_ADDR_A55 CM33_TO_A55_ADDR_NS(VRING_TX_ADDR_CM33)
#define VRING_RX_ADDR_A55 CM33_TO_A55_ADDR_NS(VRING_RX_ADDR_CM33)
#define VRING_ALIGNMENT   (0x100U)

#define RSC_TABLE_NUM_RPMSG_BUFF 512

enum rsc_table_entries { RSC_TABLE_VDEV_ENTRY, RSC_TABLE_NUM_ENTRY };

struct fw_resource_table {
	unsigned int ver;
	unsigned int num;
	unsigned int reserved[2];
	unsigned int offset[RSC_TABLE_NUM_ENTRY];

	struct fw_rsc_vdev       vdev;
	struct fw_rsc_vdev_vring vring0;
	struct fw_rsc_vdev_vring vring1;
} METAL_PACKED_END;

void rsc_table_get(void **table_ptr, int *length);

static inline struct fw_rsc_vdev *rsc_table_to_vdev(void *rsc_table)
{
	return &((struct fw_resource_table *)rsc_table)->vdev;
}

static inline struct fw_rsc_vdev_vring *rsc_table_get_vring0(void *rsc_table)
{
	return &((struct fw_resource_table *)rsc_table)->vring0;
}

static inline struct fw_rsc_vdev_vring *rsc_table_get_vring1(void *rsc_table)
{
	return &((struct fw_resource_table *)rsc_table)->vring1;
}

#ifdef __cplusplus
}
#endif

#endif /* RESOURCE_TABLE_H__ */
