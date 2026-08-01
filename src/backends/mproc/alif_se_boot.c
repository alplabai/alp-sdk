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

/*
 * PRECONDITION, bench-measured on E8 silicon (AE822FA0E5597LS0), 2026-07-31:
 * se_service_boot_cpu() asks the SE to release @p core at @p entry_addr.
 * It does NOT place an image there -- it assumes one is already resident
 * at that address, and nothing in this call's return value says otherwise.
 *
 * On a bare-Zephyr repro, an ATOC entry declared `"flags": ["load"]`
 * reported `uLV` (Loaded, Verified) in the SES boot table while the
 * destination held no image: two independent debug access ports read the
 * peer's ITCM as uninitialized SRAM from t+0.80s to t+60s, never the
 * staged image's first words (`20002200 00002641`).  Releasing that core
 * made it vector from empty memory and lock up immediately:
 * CFSR = 0x00000101 (IACCVIOL + IBUSERR), PC = 0xEFFFFFFE.
 *
 * The working declaration is `"flags": ["load", "boot", "deferred"]`
 * ("deferred" is a member of the flags array -- a sibling `"deferred":
 * true` key is rejected).  It sets TOC_IMAGE_DEFERRED = 0x100 (the
 * entry's flags word goes 0x00000022 -> 0x00000122) and prints `D` in
 * the SES table; the SES then skips the boot-time action entirely
 * (`uLs  D`, Dest Addr blank, Time 0.00 ms), and the host un-defers it
 * at runtime with SERVICES_boot_process_toc_entry (service 500,
 * services_host_boot.c:46-63), which performs load, verify and release
 * together.  With that ATOC shape, se_service_boot_cpu() started the
 * peer and carried an RPMsg link through 495 consecutive PING/PONG
 * round-trips over 4m11s.
 *
 * Left open: whether service 501 (se_service_boot_cpu()) honours
 * @p entry_addr at all was not isolated by this run -- the working
 * entry point came from the ATOC itself, not from this call's
 * argument.  Do not assume @p entry_addr is (or isn't) authoritative
 * until that is bench-checked separately.
 */
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
