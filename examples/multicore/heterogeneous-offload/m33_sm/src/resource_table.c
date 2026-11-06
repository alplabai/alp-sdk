/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Overriding Zephyr's generic OpenAMP resource table with a hand-built one
 * whose vring0/vring1 addresses already match this link's master (the
 * A55/Linux OpenAMP implementation expects those addresses to be resolved
 * by IT, not by us -- see resource_table.h's header comment).  This is
 * placed in the `.resource_table` link section: the Linux OpenAMP
 * remoteproc loader locates a firmware's resource table by scanning the
 * ELF for that exact section name, so any renaming here breaks the A55
 * attach path silently.
 */

#include <zephyr/kernel.h>
#include <resource_table.h>

#define __resource Z_GENERIC_SECTION(.resource_table)

static struct fw_resource_table __resource resource_table = {
	.ver      = 1,
	.num      = RSC_TABLE_NUM_ENTRY,
	.reserved = { 0, 0 },
	/* Offset */
	{
	    offsetof(struct fw_resource_table, vdev),
	},
	/* Virtio device entry */
	{
	    RSC_VDEV,
	    VIRTIO_ID_RPMSG,
	    0,
	    RPMSG_IPU_C0_FEATURES,
	    0,
	    0,
	    0,
	    VRING_COUNT,
	    { 0, 0 },
	},
	/* Vring rsc entry -- part of vdev rsc entry */
	.vring0 = { VRING_TX_ADDR_A55, VRING_ALIGNMENT, RSC_TABLE_NUM_RPMSG_BUFF, VRING0_ID, 0 },
	.vring1 = { VRING_RX_ADDR_A55, VRING_ALIGNMENT, RSC_TABLE_NUM_RPMSG_BUFF, VRING1_ID, 0 },
};

void rsc_table_get(void **table_ptr, int *length)
{
	*table_ptr = (void *)&resource_table;
	*length    = sizeof(resource_table);
}
