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
 * Deferred-TOC path (CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC):
 * se_service_boot_cpu() (service 501, "boot_cpu") only STARTS a core at
 * an address -- Alif's own SE Host Services API docs say plainly it
 * "does not perform image loading, verification, etc., it just boots
 * the core" (v1.109.0 p.112) -- so it depends on the peer's ATOC entry
 * having already placed the image via ["load","boot"] AT POWER-ON.  For
 * the M55-HP core specifically, Alif documents that this can fail:
 * "resetting the core also invalidates its TCM content" (p.115, "a
 * known case is the M55-HP core in Ensemble devices"; p.113 scopes the
 * same defect to "FUSION REV_Bx devices" -- the vendor text is not
 * fully consistent on scope, but E8 is Ensemble, so the broader p.115
 * statement covers it).  Bench-measured effect on E8 (2026-07-31,
 * HE-master releasing an HP peer via 501): the SES table reports the HP
 * entry Loaded/Verified, but its ITCM read as uninitialized SRAM, and
 * releasing it made the core vector from empty memory and lock up
 * (CFSR=0x00000101 IACCVIOL+IBUSERR, PC=0xEFFFFFFE) -- boot_cpu's
 * release resets the core, which invalidates the TCM the power-on load
 * had already filled: load-then-reset, the ordering p.115 says fails.
 * Releasing an M55-HE peer via 501 is bench-proven fine in two
 * independent sessions (2026-06-17 and 2026-08-01) -- HE isn't named in
 * either vendor passage.
 *
 * The alternative is an ATOC entry flagged ["load","boot","deferred"].
 * The SETOOLS guide (AUGD0005 p.35) defines deferred as "skipped at
 * boot time (i.e., no boot OR LOAD)" -- unlike the plain path, NOTHING
 * is placed in ITCM at power-on.  A runtime se_service_process_toc_entry()
 * call (service 500, "un-defer the TOC entry" -- p.112, "a convenient
 * way to boot a CPU core") then performs the load, verify AND release
 * together, in the order p.115's own documented remedy requires: reset
 * -> reload -> release (reload strictly AFTER any reset, so the image
 * survives it).  Service 500 implements exactly that sequence in one
 * call; the plain path's failure mode is loading BEFORE the reset
 * instead.  Bench-proven on E8 releasing an HE peer this way: a
 * bare-Zephyr repro (2026-07-31, 495 RPMsg PING/PONG round-trips over
 * 4m11s) and the in-tree aen-rpc-pingpong example (2026-08-01, 16/16
 * pongs) -- see the Kconfig help text and docs/aen-bench-bringup.md for
 * the full asymmetry table and both PDF citations.
 * se_service_process_toc_entry() ships unpatched in the west-pinned
 * hal_alif v2.3.0 module (se_services/zephyr/src/se_service.c); it is
 * NOT present in every hal_alif tree (e.g. the separate alif-dfp-ref
 * checkout used for vendor-doc research does not carry it), so its
 * availability here is a property of the pinned module revision.
 *
 * A third recipe exists and is NOT used by this backend: Alif's own
 * DualCore devkit example packages both cores mramAddress + ["boot"]
 * only (XIP straight from MRAM, no ITCM copy at all, so the TCM-reset
 * defect above cannot arise). alp-sdk's AEN examples link for ITCM by
 * default; adopting MRAM-XIP for both cores at once would be a bigger
 * boot-model change than this backend's scope.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h> /* IS_ENABLED() */

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
 * over 4m11s.  This backend now implements that path too, gated by
 * CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC below (default OFF
 * unless the configured peer is HP -- see the Kconfig help): every
 * shipped example still rides service 501 as-is because all six use
 * the working HP-master -> HE-peer direction and none set
 * CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP.
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

	if (IS_ENABLED(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC)) {
		/* CONFIG_..._ENTRY_ID names ONE specific ATOC entry -- the
		 * deferred peer this build ships for -- not a per-call
		 * parameter.  Reject any core id other than the configured
		 * peer instead of silently un-deferring the wrong entry: an
		 * HE-build with this flag on must not report ALP_OK for a
		 * caller that asked to release HP (or vice versa).  cpu_id
		 * above already proved core is HP or HE; this only checks
		 * WHICH one matches the configured peer. entry_addr is
		 * unused on the accepted path -- the SES already knows the
		 * entry's load address from the ATOC. See
		 * CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP.
		 * ALP_ERR_NOSUPPORT, not ALP_ERR_INVAL: <alp/mproc.h> reserves
		 * INVAL for ALP_CORE_SELF and documents NOSUPPORT for "a core
		 * the platform boots by other means" -- this build's boot
		 * authority only knows how to release ONE specific peer, so
		 * any other core is exactly that case (and callers, e.g.
		 * aen-dualcore-master's main.c, already map NOSUPPORT to a
		 * bench SKIP rather than a hard FAIL). */
		alp_core_id_t configured_peer =
		    IS_ENABLED(CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP) ? ALP_CORE_M55_HP
		                                                                          : ALP_CORE_M55_HE;

		if (core != configured_peer) {
			return ALP_ERR_NOSUPPORT;
		}
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
