/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon HWSEM (hardware-semaphore) validation for the E1M-AEN801 (Alif
 * Ensemble E8) over the alp-sdk in-tree "alif,hwsem" driver (ADR 0017
 * vendor-native custom -- the Alif HWSEM IP has no upstream Zephyr class and no
 * hal_alif library, so the driver is authored clean-room from the Alif DFP
 * HWSEM register header).  HWSEM is the AMP mutual-exclusion primitive the two
 * RTSS cores (M55-HE <-> M55-HP) use to lock a shared resource.
 *
 * What it proves
 * --------------
 * Take then give HWSEM instance 0 through the driver's take/give API and confirm
 * the latch's REL count tracks the operations on silicon, two independent ways:
 *
 *   1. in-firmware:  this app reads the baseline count (free latch -> 0), takes
 *      the latch once (count -> nonzero), gives it back (count -> 0), and prints
 *      every API return code + a single
 *      'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads the HWSEM REL register with mem32
 *      (0x4902E004 = base 0x4902E000 + REL 0x04, see the overlay) across the
 *      same window -- the ground truth on silicon, independent of any printk.
 *
 * SINGLE-CORE smoke, by design.  Acquire/release on ONE core is a valid HWSEM
 * proof: the request register grants ownership, the count increments, and the
 * release frees it.  It does NOT exercise cross-core ARBITRATION (HE owns ->
 * HP's take returns -EBUSY -> HP acquires only after HE gives), which needs a
 * real second-core sender and a dual-core SES boot -- see the README.  The
 * MASTER ID below is the M55-HE CPUID; a dual-core test would run a peer with
 * the M55-HP CPUID against the SAME latch.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>

/* The in-tree alif,hwsem driver's take/give API (no standard Zephyr class). The
 * header dir is put on the include path by zephyr/CMakeLists.txt when
 * CONFIG_HWSEM_ALIF is set. */
#include "hwsem_alif.h"

/* The HWSEM device is the "alif,hwsem" node (hwsem@4902e000); the alif,hwsem
 * driver binds it directly. */
#define HWSEM_NODE DT_NODELABEL(hwsem0)

/*
 * MASTER ID for THIS core.  The HWSEM latch is keyed by the caller's CPUID, and
 * a release only frees the latch for its owner -- so HE and HP MUST use
 * distinct ids.  This value is the M55-HE CPUID, transcribed CLEAN-ROOM from
 * the Alif DFP Device/soc/AE822FA0E5597/include/rtss_he/core_defines.h:
 *   #define HWSEM_MASTERID (0x410FD222)
 * (the M55-HP peer uses 0x410FD221, from .../rtss_hp/core_defines.h -- a
 * dual-core arbitration test would run the peer with that id; see the README).
 */
#define HWSEM_MASTER_ID_HE 0x410FD222U /* DFP rtss_he/core_defines.h HWSEM_MASTERID */

/* Bounded spin budget for the take retry (a peer-owned latch frees on the
 * peer's give; the bound keeps an absent peer from hanging us). */
#define HWSEM_TAKE_SPINS 100000U

static const struct device *const hwsem = DEVICE_DT_GET(HWSEM_NODE);

int main(void)
{
	int      rc;
	uint32_t count_free, count_taken, count_given;

	printk("\n=== AEN801 HWSEM take/give bench "
	       "(alif,hwsem / hwsem@4902e000) ===\n");

	/* 1. device readiness.  If the HWSEM node did not instantiate a device the
	 *    build would have failed at link (undefined __device_dts_ord_*), so
	 *    reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(hwsem)) {
		printk("RESULT FAIL: hwsem device not ready\n");
		return 0;
	}
	printk("hwsem device ready (master_id=0x%08x)\n", HWSEM_MASTER_ID_HE);

	/* 2. baseline count.  A free latch reads REL count == 0.  If it is nonzero
	 *    here a previous owner left it held -- reset to a known-free state so the
	 *    smoke starts clean (reset is the recovery path, not normal flow). */
	rc = hwsem_alif_get_count(hwsem, &count_free);
	printk("hwsem_alif_get_count() #1 rc=%d  count=%u\n", rc, count_free);
	if (rc != 0) {
		printk("RESULT FAIL: get_count #1 rc=%d\n", rc);
		return 0;
	}
	if (count_free != 0U) {
		printk("note: latch not free at start (count=%u); resetting\n", count_free);
		(void)hwsem_alif_reset(hwsem);
		(void)hwsem_alif_get_count(hwsem, &count_free);
		printk("hwsem_alif_get_count() after reset count=%u\n", count_free);
	}

	/* --- J-Link readback window #1: mem32 0x4902E004 (HWSEM REL) -> 0 --- */

	/* 3. TAKE the latch.  On a free latch the request register grants ownership
	 *    immediately, so the bounded spin returns 0 on the first attempt. */
	rc = hwsem_alif_take_busy(hwsem, HWSEM_MASTER_ID_HE, HWSEM_TAKE_SPINS);
	printk("hwsem_alif_take_busy() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: take rc=%d (latch peer-owned or device error)\n", rc);
		return 0;
	}

	/* 4. count after take.  An owned latch reads REL count != 0. */
	rc = hwsem_alif_get_count(hwsem, &count_taken);
	printk("hwsem_alif_get_count() #2 rc=%d  count=%u (expect nonzero)\n", rc, count_taken);
	if (rc != 0) {
		printk("RESULT FAIL: get_count #2 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #2: mem32 0x4902E004 (HWSEM REL) -> nonzero --- */

	/* 5. GIVE the latch back.  A release by the owner decrements the count; one
	 *    take + one give returns it to free (count 0). */
	rc = hwsem_alif_give(hwsem, HWSEM_MASTER_ID_HE);
	printk("hwsem_alif_give() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: give rc=%d\n", rc);
		return 0;
	}

	/* 6. count after give.  A freed latch reads REL count == 0 again. */
	rc = hwsem_alif_get_count(hwsem, &count_given);
	printk("hwsem_alif_get_count() #3 rc=%d  count=%u (expect 0)\n", rc, count_given);
	if (rc != 0) {
		printk("RESULT FAIL: get_count #3 rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window #3: mem32 0x4902E004 (HWSEM REL) -> 0 --- */

	/* 7. verdict: a working HWSEM latch must read free(0) at start, owned
	 *    (nonzero) after take, and free(0) again after give. */
	if (count_taken != 0U && count_given == 0U) {
		printk("RESULT PASS: hwsem take/give works "
		       "(free=%u taken=%u given=%u)\n",
		       count_free,
		       count_taken,
		       count_given);
	} else {
		printk("RESULT FAIL: hwsem count did not track take/give "
		       "(free=%u taken=%u given=%u) -- expected taken!=0 && given==0; "
		       "check the REL register (0x4902E004) over J-Link\n",
		       count_free,
		       count_taken,
		       count_given);
	}

	return 0;
}
