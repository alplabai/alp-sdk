/* SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-enforced (TF-M) tier for <alp/update_log.h> -- SEAM + GATED
 * REGISTRATION. This is the tier the SW tamper-evident tier (sw_tier.c)
 * explicitly does NOT provide: an audit log an on-device application
 * cannot forge, because both the backing store and the monotonic anchor
 * live in the Secure Processing Environment, unreachable by the
 * non-secure app.
 *
 * @par What this tier WOULD bind to (the two engine seams, store.h)
 *   - store   -> PSA Protected Storage. The API is groundable in the
 *     pinned Zephyr tree (subsys/secure_storage/include/psa/protected_storage.h):
 *     psa_ps_set()/psa_ps_get()/psa_ps_remove() over a psa_storage_uid_t.
 *     HW isolation is only real under BUILD_WITH_TFM -- with TF-M absent
 *     Zephyr's secure_storage subsystem implements the SAME API inside the
 *     non-secure image (its Kconfig help says data-at-rest protection is
 *     "not a guarantee"), which is the sw_tier trust boundary, not this
 *     one. So this tier is gated on CONFIG_ALP_SDK_UPDATE_LOG_TFM, which
 *     depends on BUILD_WITH_TFM.
 *   - counter -> a hardware, non-decrementable monotonic counter. This is
 *     the ungroundable piece: the pinned workspace exposes NO non-secure-
 *     callable NV-counter service. TF-M's platform NV-counter service
 *     (tfm_platform_nv_counter_read/increment) lives behind the TF-M
 *     build (modules/trusted-firmware-m/Kconfig.tfm.partitions references
 *     it but no trusted-firmware-m module is checked out); hal_alif v2.2.0
 *     SE services (se_services/include/services_lib_api.h) expose only OTP
 *     read/write (SERVICES_system_read_otp / _write_otp), which is
 *     one-time-programmable, not an append-mostly counter. Wiring a real
 *     monotonic anchor is therefore a later, on-silicon slice.
 *
 * @par Why it registers but declines (issues #111, #239)
 *   The backend IS registered into the update_log class section when the
 *   platform advertises TF-M (CONFIG_ALP_SDK_UPDATE_LOG_TFM=y) at a higher
 *   priority than the SW tier, so the dispatcher offers it first. Until
 *   the two seams above are implemented and pass the on-silicon acceptance
 *   criteria, ready() returns ALP_ERR_NOSUPPORT: the dispatcher then falls
 *   through to the SW tamper-evident tier (see update_log_dispatch.c). No
 *   invented secure-service call, no false HW_ENFORCED assurance on a boot
 *   that cannot back it. When the seams land, ready() returns ALP_OK and
 *   this tier serves -- reporting ALP_UPDATE_LOG_HW_ENFORCED.
 *
 * The seam stubs below are compiled unconditionally (they cannot bitrot
 * against store.h) and stand in for the psa_ps / NV-counter bindings the
 * on-silicon slice will replace.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/111
 */
#include "alp/update_log.h"
#include "update_log/store.h"

/* Store seam: reserved shape for the psa_ps_set() binding. */
alp_status_t alp_update_log_alif_tfm_store_put(void *c, const char *k, const uint8_t *b, size_t n)
{
	(void)c;
	(void)k;
	(void)b;
	(void)n;
	return ALP_ERR_NOSUPPORT;
}

/* Counter seam: reserved shape for the HW monotonic-counter binding. */
alp_status_t alp_update_log_alif_tfm_counter_read(void *c, uint32_t id, uint64_t *v)
{
	(void)c;
	(void)id;
	(void)v;
	return ALP_ERR_NOSUPPORT;
}

#ifdef CONFIG_ALP_SDK_UPDATE_LOG_TFM

#include "alp/backend.h"
#include "backends/update_log/update_log_ops.h"

/* Open-time readiness. Returns NOSUPPORT while the secure store +
 * monotonic counter seams are stubs, so the dispatcher degrades to the SW
 * tamper-evident tier (issue #239). Flip to ALP_OK from the on-silicon
 * slice once psa_ps_* + the HW counter are wired and provisioned. */
static alp_status_t tfm_ready(void)
{
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t tfm_append(const alp_update_log_entry_t *e)
{
	(void)e;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t tfm_verify(alp_update_log_verdict_t *v, uint64_t *bad)
{
	(void)v;
	(void)bad;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t tfm_count(uint64_t *out)
{
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t tfm_get(uint64_t seq, alp_update_log_entry_t *out)
{
	(void)seq;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}

static const alp_update_log_ops_t _tfm_ops = {
	.assurance = ALP_UPDATE_LOG_HW_ENFORCED,
	.ready     = tfm_ready,
	.append    = tfm_append,
	.verify    = tfm_verify,
	.count     = tfm_count,
	.get       = tfm_get,
};

/* silicon_ref="*": PSA Protected Storage + the TF-M platform NV-counter
 * service are TF-M platform features, not Alif-specific, so this tier does
 * not pin an (invented) per-SoM ref -- CONFIG_ALP_SDK_UPDATE_LOG_TFM
 * (=> BUILD_WITH_TFM) is the capability gate that scopes it to TF-M builds.
 * priority 20 > the SW tier's 10 so the dispatcher offers it first. */
ALP_BACKEND_REGISTER(update_log,
                     alif_tfm,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alif_tfm",
                         .base_caps   = 0u,
                         .priority    = 20,
                         .ops         = &_tfm_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_TFM */
