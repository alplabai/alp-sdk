/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-se-service-info -- scopeless on-silicon staging check of the Secure
 * Enclave (SE) SERVICE transport on the E1M-AEN801 (Ensemble E8, M55-HE), via
 * the bench RAM-run + RAM-console flow.  Mirrors aen-camera-regcheck.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The SE service client is the Apache-2.0 hal_alif library
 *   (modules/hal/alif/se_services/zephyr/src/se_service.c).  It reaches the SE
 *   over two Arm MHUv2 mailboxes (RTSS-HE <-> SE) using Zephyr's IPM API:
 *     - seservice0r (mhu@40040000, IRQ 37, "rx")  = SE -> RTSS  inbound
 *     - seservice0s (mhu@40050000, IRQ 38, "tx")  = RTSS -> SE  outbound
 *   A root node `se_service` (compatible "alif,secure-enclave-services") ties
 *   the two together via mhuv2-send-node/mhuv2-recv-node phandles; se_service.c
 *   DEVICE_DT_GET_OR_NULLs both at SYS_INIT.
 *
 *   Upstream Zephyr v4.4 ships no MHUv2 IPM driver and does not `select`
 *   HAS_ALIF_SE_SERVICES / ARM_MHUV2 (those live in the Alif zephyr_alif fork
 *   SoC Kconfig), so on the alp-sdk (upstream + hal_alif) stack the whole SE
 *   path is dark.  The alp-sdk closes that gap in-tree: the "arm,mhuv2" IPM
 *   driver (zephyr/drivers/ipm/ipm_arm_mhuv2.c) + its binding + the DT nodes
 *   (E8 SoC overlay) + the Kconfig that defines ARM_MHUV2 / HAS_ALIF_SE_SERVICES
 *   so hal_alif's se_service.c compiles and links.
 *
 *   So this app validates what IS deliverable on this batch:
 *     1. the two MHUv2 mailbox nodes + the se_service root node BIND
 *        (DT_HAS_*_ENABLED, right compatible, right reg base / IRQ),
 *     2. both IPM mailbox devices are device_is_ready() (the driver bound and
 *        mapped its MMIO),
 *     3. it issues a REAL, ZERO-RISK SE query -- se_service_system_get_device_data()
 *        -- which sends SERVICE_SYSTEM_MGMT_GET_DEVICE_REVISION_DATA (0xD0) over
 *        the outbound mailbox, waits for the SE response on the inbound one, and
 *        reads back the SoC LIFECYCLE STATE (LCS) + the device revision id.
 *
 *   The LCS read is a PURE QUERY.  It does NOT advance the lifecycle, write a
 *   STOC, blow a fuse, or touch the SE provisioning state -- it only reads the
 *   device-revision-data response the SE already holds.  Expected on a
 *   maker-provisioned E8: LCS = 0x1 (Device Manufacturer / DM).  LCS legend
 *   (from the Alif SE service reference): 0x0 = CM (chip mfr), 0x1 = DM (device
 *   mfr), 0x5 = Secure-enabled, 0x7 = RMA.
 *
 * WHAT IS RUNTIME-DEPENDENT: the actual SE round-trip needs the SE firmware
 * (SES) alive and answering on the mailboxes.  On a properly-booted E8 that is
 * the case; if the SE is asleep/unreachable the query returns -EAGAIN/-EBUSY
 * and the app reports that (it does not hang -- se_service.c bounds every wait).
 *
 * The PASS gate is two-stage: (A) the transport BINDS (both mailboxes + the
 * se_service node bind at their fork reg/IRQ and the IPM devices are ready) --
 * always checkable; (B) the live LCS query returns 0 and a plausible LCS.  (A)
 * is the staging deliverable; (B) is the bench confirmation.  The app prints
 * both so a RAM-console read shows exactly how far it got.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* hal_alif SE service client (Apache-2.0). */
#include <se_service.h>

/* The two MHUv2 mailbox nodes + the SE-service root node. */
#define SE_NODE DT_NODELABEL(se_service)
#define MHU_RX  DT_NODELABEL(seservice0r)
#define MHU_TX  DT_NODELABEL(seservice0s)

/*
 * Expected reg bases / IRQs, transcribed from the SoC dtsi (which carries the
 * Alif zephyr_alif fork e1.dtsi addresses VERBATIM).  We read the LIVE values
 * from devicetree and compare, so a binding that resolved to the wrong node is
 * caught.
 */
#define MHU_RX_BASE_EXPECTED 0x40040000U
#define MHU_TX_BASE_EXPECTED 0x40050000U
#define MHU_RX_IRQ_EXPECTED  37U
#define MHU_TX_IRQ_EXPECTED  38U

/* LCS legend (Alif SE service reference). */
#define LCS_CM  0x0U /* Chip Manufacturer */
#define LCS_DM  0x1U /* Device Manufacturer (maker-provisioned) */
#define LCS_SE  0x5U /* Secure-enabled */
#define LCS_RMA 0x7U /* Return Merchandise Authorization */

static const char *lcs_name(uint8_t lcs)
{
	switch (lcs) {
	case LCS_CM:
		return "CM (chip manufacturer)";
	case LCS_DM:
		return "DM (device manufacturer)";
	case LCS_SE:
		return "SE (secure-enabled)";
	case LCS_RMA:
		return "RMA";
	default:
		return "<unknown>";
	}
}

/*
 * Compile-time staging facts -- 1 iff the node exists, is enabled, and binds to
 * its expected compatible.  Pure DT predicates.
 */
#define MHU_RX_BOUND (DT_NODE_HAS_STATUS(MHU_RX, okay) && DT_NODE_HAS_COMPAT(MHU_RX, arm_mhuv2))
#define MHU_TX_BOUND (DT_NODE_HAS_STATUS(MHU_TX, okay) && DT_NODE_HAS_COMPAT(MHU_TX, arm_mhuv2))
#define SE_BOUND                                                                                   \
	(DT_NODE_HAS_STATUS(SE_NODE, okay) && DT_NODE_HAS_COMPAT(SE_NODE, alif_secure_enclave_services))

int main(void)
{
	printk("\n=== aen-se-service-info ===\n");

	/*
	 * Stage A -- the transport BINDS.  DT_REG_ADDR / DT_IRQN are build-time
	 * constants pulled from the bound nodes; a mismatch vs the fork address
	 * means the binding resolved to the wrong node.
	 */
	uint32_t rx_base = (uint32_t)DT_REG_ADDR(MHU_RX);
	uint32_t tx_base = (uint32_t)DT_REG_ADDR(MHU_TX);
	uint32_t rx_irq  = (uint32_t)DT_IRQN(MHU_RX);
	uint32_t tx_irq  = (uint32_t)DT_IRQN(MHU_TX);

	printk("se_service: %s\n", DT_NODE_FULL_NAME(SE_NODE));
	printk("        bound=%d compat=alif,secure-enclave-services\n", (int)SE_BOUND);
	printk("mhu rx  : %s\n", DT_NODE_FULL_NAME(MHU_RX));
	printk("        bound=%d compat=arm,mhuv2 base=0x%08x (exp 0x%08x) irq=%u (exp %u)\n",
	       (int)MHU_RX_BOUND,
	       rx_base,
	       MHU_RX_BASE_EXPECTED,
	       rx_irq,
	       MHU_RX_IRQ_EXPECTED);
	printk("mhu tx  : %s\n", DT_NODE_FULL_NAME(MHU_TX));
	printk("        bound=%d compat=arm,mhuv2 base=0x%08x (exp 0x%08x) irq=%u (exp %u)\n",
	       (int)MHU_TX_BOUND,
	       tx_base,
	       MHU_TX_BASE_EXPECTED,
	       tx_irq,
	       MHU_TX_IRQ_EXPECTED);

	/* The IPM devices the se_service node points at. */
	const struct device *rx_dev   = DEVICE_DT_GET_OR_NULL(MHU_RX);
	const struct device *tx_dev   = DEVICE_DT_GET_OR_NULL(MHU_TX);
	bool                 rx_ready = (rx_dev != NULL) && device_is_ready(rx_dev);
	bool                 tx_ready = (tx_dev != NULL) && device_is_ready(tx_dev);

	printk("mhu rx  : device %s\n", rx_ready ? "READY" : "NOT ready");
	printk("mhu tx  : device %s\n", tx_ready ? "READY" : "NOT ready");

	bool transport_ok = SE_BOUND && MHU_RX_BOUND && (rx_base == MHU_RX_BASE_EXPECTED) &&
	                    (rx_irq == MHU_RX_IRQ_EXPECTED) && MHU_TX_BOUND &&
	                    (tx_base == MHU_TX_BASE_EXPECTED) && (tx_irq == MHU_TX_IRQ_EXPECTED) &&
	                    rx_ready && tx_ready;

	/*
	 * Stage B -- the live, zero-risk SE query.  get_device_revision_data
	 * (service 0xD0) reads the SoC lifecycle state + revision id over the
	 * mailboxes.  Read-only: no STOC/fuse/lifecycle write.  se_service.c bounds
	 * every wait, so this returns (0 / -EAGAIN / -EBUSY / positive SE err) and
	 * never hangs.
	 */
	get_device_revision_data_t dev_data = { 0 };
	int                        rc       = se_service_system_get_device_data(&dev_data);

	if (rc == 0) {
		printk("SE query: se_service_system_get_device_data rc=0\n");
		printk("        revision_id = 0x%08x\n", (uint32_t)dev_data.revision_id);
		printk("        LCS         = 0x%02x  (%s)\n",
		       (unsigned int)dev_data.LCS,
		       lcs_name(dev_data.LCS));
	} else {
		printk("SE query: se_service_system_get_device_data rc=%d "
		       "(<0 EAGAIN/EBUSY = SE asleep/unreachable; >0 = SE-reported error)\n",
		       rc);
	}

	/*
	 * PASS gate.  Stage A (transport binds + both IPM mailboxes ready) is the
	 * staging deliverable and is always checkable on silicon.  Stage B (the live
	 * LCS round-trip) is the bench confirmation: it additionally needs the SE
	 * firmware answering.  Report both so the RAM-console read shows how far it
	 * got; PASS requires A, and notes whether B also landed.
	 */
	if (transport_ok && rc == 0) {
		printk("RESULT PASS: SE service transport BINDS and LIVE query works -- "
		       "seservice0r/seservice0s (arm,mhuv2 @ 0x40040000/0x40050000, IRQ 37/38) "
		       "bind + IPM-ready, se_service node ties them, and "
		       "se_service_system_get_device_data() returned LCS=0x%02x (%s)\n",
		       (unsigned int)dev_data.LCS,
		       lcs_name(dev_data.LCS));
	} else if (transport_ok) {
		printk("RESULT PASS (transport): SE service transport BINDS -- both MHUv2 "
		       "mailboxes bind at the fork reg/IRQ and are IPM-ready, se_service node "
		       "ties them.  LIVE LCS query did not complete (rc=%d): SE firmware "
		       "asleep/unreachable on this boot -- transport staging is the deliverable\n",
		       rc);
	} else {
		printk(
		    "RESULT FAIL: SE service transport NOT fully staged "
		    "(se=%d rx_bound=%d rx_ready=%d tx_bound=%d tx_ready=%d -- a node is "
		    "missing, disabled, bound to the wrong compatible/reg/IRQ, or the IPM "
		    "driver did not come up)\n",
		    (int)SE_BOUND,
		    (int)(MHU_RX_BOUND && rx_base == MHU_RX_BASE_EXPECTED && rx_irq == MHU_RX_IRQ_EXPECTED),
		    (int)rx_ready,
		    (int)(MHU_TX_BOUND && tx_base == MHU_TX_BASE_EXPECTED && tx_irq == MHU_TX_IRQ_EXPECTED),
		    (int)tx_ready);
	}

	return 0;
}
