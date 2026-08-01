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
 * PRECONDITION: se_service_boot_cpu() (SE service 501) asks the SE to
 * release @p core at @p entry_addr.  Per the vendor manual
 * (SE_Host_Services_API_v1.109.0.pdf, p.112, SERVICES_boot_cpu) it
 * "does not perform image loading, verification, etc., it just boots
 * the core" -- placing and keeping the image resident at @p entry_addr
 * is the caller's responsibility, done before this call.
 *
 * DIRECTION-SPECIFIC FAILURE, bench-measured on E8 silicon
 * (AE822FA0E5597LS0), 2026-07-31: a plain `"flags": ["load"]` ATOC
 * entry + se_service_boot_cpu() WORKS in the HP-master -> HE-peer
 * direction -- ALP-HE read `uLV` (Loaded, Verified) with Dest Addr
 * 0x58000000 populated (28.52 ms load time), 0x58000000 held
 * `20006160 00004A85` matching the staged binary on a repeat read
 * (ground-truth control passed first), and the link carried 16/16
 * PING/PONG round-trips.
 *
 * The SAME recipe FAILS in the HE-master -> HP-peer direction: the
 * ATOC still reported `uLV`, but two independent debug access ports
 * read the HP peer's ITCM as uninitialized SRAM from t+0.80s to
 * t+60s, never the staged image's first words.  Releasing that core
 * made it vector from empty memory and lock up immediately:
 * CFSR = 0x00000101 (IACCVIOL + IBUSERR), PC = 0xEFFFFFFE.
 *
 * ROOT CAUSE (vendor-documented, not "Loaded is a lie"): the SES DOES
 * place the bytes at ATOC-processing time in both directions.  The
 * M55-HP core's own TCM is separately invalidated by the reset that
 * SERVICES_boot_cpu() / SERVICES_boot_reset_cpu() issues before
 * release, so by the time HP is released its TCM is empty again even
 * though the SES table still (correctly, as of load time) says `uLV`.
 * M55-HE is not documented as having this behaviour, matching the
 * working direction above.  Two vendor passages describe it and
 * DISAGREE on device scope -- both are quoted verbatim rather than
 * silently picking one:
 *   - p.112, SERVICES_boot_cpu: "For the M55 cores, there are cases
 *     in which this service does not work. The currently known case
 *     is the M55-HP core in FUSION REV_Bx devices, where resetting
 *     the core also invalidates its TCM content."
 *   - p.115, SERVICES_boot_release_cpu: "A known case is the M55-HP
 *     core in Ensemble devices. Because of that, after calling
 *     SERVICES_boot_reset_cpu() to stop the core, the image in the
 *     TCM must be reloaded, before calling SERVICES_boot_release_cpu()."
 * E8 is Ensemble, so the second passage is the one that applies here;
 * the two passages naming different device families for what reads
 * as the same erratum is the vendor doc's inconsistency, not ours.
 *
 * The working fix for the HE-master -> HP-peer direction is deferred
 * ATOC processing: `"flags": ["load", "boot", "deferred"]` (sets
 * TOC_IMAGE_DEFERRED = 0x100; "deferred" is a flags-array member, a
 * sibling `"deferred": true` key is rejected) skips the SES boot-time
 * release, and the host reloads + releases at runtime with
 * SERVICES_boot_process_toc_entry (service 500,
 * services_host_boot.c:46-63) -- the vendor manual (p.112) calls 500
 * the "higher-level ... convenient way to boot a CPU core" and
 * recommends it over 501 for M55 cores generally.  That combination
 * carried an RPMsg link through 495 consecutive PING/PONG round-trips
 * over 4m11s.  See branch feat/aen-mproc-deferred-toc-boot for that
 * work; this backend still rides service 501 as-is because every
 * shipped example uses the working HP-master -> HE-peer direction.
 *
 * Left open: whether service 501 honours @p entry_addr at all was not
 * isolated by either run above -- the entry point came from the ATOC
 * in both cases, not from this call's argument.
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
