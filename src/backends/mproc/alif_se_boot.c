/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif SE-service peer-core boot backend for alp_mproc_boot_core()
 * (<alp/mproc.h>) on the Alif Ensemble E8 (E1M-AEN801).
 *
 * On the E8 the Secure Enclave (SE) is the boot authority: a
 * dual-core boot package can mark a core's image ["load"]-only (the
 * SES loads it into its ITCM but does not release the core), and the
 * master core starts it at runtime over the SE-service mailbox.  This
 * is the bench-proven "B1 Option B" route the aen-dualcore-* examples
 * validated on silicon -- the same se_service_boot_cpu() call, now
 * behind the portable surface so example/application code carries no
 * vendor include.
 *
 * Core-id mapping (hal_alif services_lib_api.h cpu_id enum):
 *   ALP_CORE_M55_HP -> EXTSYS_0  (M55 HP)
 *   ALP_CORE_M55_HE -> EXTSYS_1  (M55 HE)
 * The A32 cluster boots through a different SE flow (ES0/Linux boot
 * services), so every other core id reports ALP_ERR_NOSUPPORT here.
 *
 * se_service_boot_cpu() is provided by the alp-sdk hal_alif patch
 * zephyr/patches/hal_alif/0001-se-service-add-boot-cpu.patch and
 * bounds its wait inside se_service.c, so the call never hangs.
 *
 * Deferred-TOC path (CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC,
 * default OFF): se_service_boot_cpu() STARTS a peer core but does not
 * PLACE an image there -- it depends on the peer's ATOC entry having
 * already loaded the image via ["load","boot"].  An entry declared
 * ["load"]-only reports Loaded/Verified in the SES table while the
 * peer's ITCM holds no image; releasing that core makes it vector from
 * empty memory and lock up.  The alternative is an ATOC entry flagged
 * ["load","boot","deferred"] (the SES skips its boot-time action) plus
 * a runtime se_service_process_toc_entry() call (service 500), which
 * performs load, verify AND release together -- no separate boot_cpu
 * release needed.  se_service_process_toc_entry() ships unpatched in
 * hal_alif v2.3.0; see the Kconfig help text and docs/aen-bench-bringup.md
 * for the bench evidence.
 */

#include <errno.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>

#include "mproc_ops.h"

#if defined(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE)

/* hal_alif SE service client (Apache-2.0).  Provides
 * se_service_boot_cpu() + the EXTSYS_* cpu_id enum. */
#include <se_service.h>

static alp_status_t se_rc_to_alp(int rc)
{
	if (rc == 0) {
		return ALP_OK;
	}
	switch (rc) {
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EAGAIN:
	case -EBUSY:
		return ALP_ERR_NOT_READY;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t alif_se_boot_core(alp_core_id_t core, uintptr_t entry_addr)
{
	uint32_t cpu_id;

	switch (core) {
	case ALP_CORE_M55_HP:
		cpu_id = (uint32_t)EXTSYS_0;
		break;
	case ALP_CORE_M55_HE:
		cpu_id = (uint32_t)EXTSYS_1;
		break;
	default:
		/* The A32 cluster (and any non-AEN core id) is not bootable
		 * over this SE service. */
		return ALP_ERR_NOSUPPORT;
	}

	if (IS_ENABLED(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC)) {
		/* The peer's ATOC entry is ["load","boot","deferred"]: the
		 * SES skipped the boot-time release, so un-defer + release
		 * happen together here via SERVICE_BOOT_PROCESS_TOC_ENTRY
		 * (service 500).  entry_addr is unused on this path -- the
		 * SES already knows the entry's load address from the ATOC.
		 * See CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC. */
		return se_rc_to_alp(
		    se_service_process_toc_entry(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_ENTRY_ID));
	}

	return se_rc_to_alp(se_service_boot_cpu(cpu_id, (uint32_t)entry_addr));
}

static const alp_mproc_boot_ops_t _ops = {
	.boot_core = alif_se_boot_core,
};

ALP_BACKEND_REGISTER(mproc_boot,
                     alif_se,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE */
