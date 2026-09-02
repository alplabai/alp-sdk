/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif SE-service soc_info backend for the SoC-identity half of the
 * <alp/hw_info.h> surface (alp_soc_info_read / alp_soc_secure_fw_ping)
 * on the Alif Ensemble E8 (E1M-AEN801).
 *
 * Registers at silicon_ref="alif:ensemble:e8" priority 100 -- above
 * the priority-0 "*" sw_fallback -- so on the E8 the portable identity
 * surface transparently rides the Secure Enclave (SE) service mailbox:
 * the very transport the bench-proven read-only se_service client
 * drives (examples/aen/aen-se-service-info staged it; the query
 * example pulled every read rc=0 on silicon).  Nothing in
 * <alp/hw_info.h> names the SE -- the vendor stays behind the
 * dispatcher.
 *
 * ONLY READ-ONLY / NON-MUTATING SE services are used here:
 *   se_service_heartbeat                -- the ping op
 *   se_service_get_se_revision          -- secure_fw_version string
 *   se_service_get_device_part_number   -- part_number
 *   se_service_system_get_device_data   -- revision_id + LCS lifecycle
 *                                          + factory-fused serial
 *
 * Every call bounds its wait inside hal_alif's se_service.c (returns
 * 0 / -EAGAIN / -EBUSY / a positive SE error), so the backend never
 * hangs.  A per-field failure does not abort the read: later fields
 * are still attempted and the FIRST failure is reported, per the
 * soc_info_ops contract ("already-filled fields stay valid").
 *
 * DIAGNOSTIC, NOT A FIX (issue #1700 / ADR-0030): a customer AE822
 * running SERAM v106 against a services-library from SETOOLS v109
 * lost HFXTAL + PLL lock on its FIRST SE service request -- M55-HP
 * fell from 400 MHz to 76.8 MHz and stayed there.  Alif confirmed a
 * real API break below v109 for E8 and named v110 as the floor; across
 * a break of that kind any SE behaviour is possible, so this backend
 * cannot safely re-establish a clock the SE itself dropped without
 * knowing what the mismatched pair actually did.  What it CAN do is
 * turn that silent stall into a logged, actionable warning the first
 * time it reads the running SERAM version below the floor -- see
 * alif_se_warn_if_seram_below_floor() and docs/aen-se-services.md
 * section 0.1.
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/hw_info.h>
#include <alp/peripheral.h>

#include "soc_info_ops.h"

#if defined(CONFIG_ALP_SDK_SOC_INFO_ALIF_SE)

/* hal_alif SE service client (Apache-2.0).  Transitively provides
 * get_device_revision_data_t (services_lib_api.h) and
 * VERSION_RESPONSE_LENGTH (services_lib_protocol.h). */
#include <se_service.h>

LOG_MODULE_REGISTER(alp_soc_info_alif_se, CONFIG_LOG_DEFAULT_LEVEL);

/* ADR-0030's floor: E1M-AEN modules must run a SERAM (SE firmware) image
 * that MATCHES the services library this SDK links -- currently
 * hal_alif v2.3.0 -- and that pair's floor is v110, Alif's own
 * recommendation and the version this SDK's reference board runs.  Not a
 * hardware register: a policy number this SDK owns (docs/adr/0030-aen-
 * seram-tracks-alif-latest-as-a-matched-pair.md), re-stated here so the
 * check below has something to compare against. */
#define ALIF_SE_SERAM_FLOOR 110u

static bool alif_se_seram_floor_warned;

/* Alif name a SERAM release by the MIDDLE field of the banner
 * se_service_get_se_revision() returns -- "SES A0 v1.110.0 Mar 4 2026"
 * is SERAM v110, "SES A0 v1.106.2 Jul 14 2025" is v106 (bench captures,
 * docs/aen-se-services.md section 0.1).  This is a free-form vendor
 * banner, not a documented wire format: on any shape this does not
 * recognise, return false and stay silent rather than risk a false
 * warning off a guessed parse. */
static bool alif_se_seram_from_banner(const char *rev, uint32_t *seram_out)
{
	const char *v = strchr(rev, 'v');

	if (v == NULL) {
		return false;
	}

	const char *dot = strchr(v, '.');

	if (dot == NULL || !isdigit((unsigned char)dot[1])) {
		return false;
	}

	uint32_t    seram = 0u;
	const char *p     = dot + 1;

	while (isdigit((unsigned char)*p)) {
		seram = (seram * 10u) + (uint32_t)(*p - '0');
		p++;
	}

	*seram_out = seram;
	return true;
}

/* Fires at most once per boot (a customer polling alp_soc_info_read()
 * should get one warning, not a flood).  Purely diagnostic: never
 * touches the SE, never retries, never blocks -- see the file header
 * for why this backend does not attempt to re-establish the clock
 * itself. */
static void alif_se_warn_if_seram_below_floor(const char *rev)
{
	uint32_t seram;

	if (alif_se_seram_floor_warned) {
		return;
	}
	if (!alif_se_seram_from_banner(rev, &seram) || seram >= ALIF_SE_SERAM_FLOOR) {
		return;
	}

	alif_se_seram_floor_warned = true;
	LOG_WRN("SE firmware (SERAM) reports \"%s\" -- below the v%u floor this SDK "
	        "requires (ADR-0030). A mismatched SERAM/services-library pair is "
	        "untriageable: Alif documented an API break below v109 for E8 that "
	        "can drop HFXTAL/PLL on the FIRST SE service request (alp-sdk#1700). "
	        "Update SERAM over the SE-UART before debugging anything else -- see "
	        "docs/aen-se-services.md section 0.1.",
	        rev,
	        ALIF_SE_SERAM_FLOOR);
}

/* se_service_* return 0 on success, a negative errno for the
 * transport (-EAGAIN timeout, -EBUSY SE busy, -EINVAL bad arg), or a
 * positive SE firmware error for a serviced-but-rejected request. */
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

/* Keep the first failure; later reads still run so the caller gets
 * every field the SE could source on this attempt. */
static void keep_first(alp_status_t *first, alp_status_t rc)
{
	if (*first == ALP_OK && rc != ALP_OK) {
		*first = rc;
	}
}

static alp_status_t alif_se_read(alp_soc_info_t *out)
{
	alp_status_t first = ALP_OK;

	/* SE firmware revision string (up to VERSION_RESPONSE_LENGTH = 80
	 * bytes, not guaranteed NUL-terminated by the SE). */
	uint8_t rev[VERSION_RESPONSE_LENGTH] = { 0 };
	int     rc                           = se_service_get_se_revision(rev);

	keep_first(&first, se_rc_to_alp(rc));
	if (rc == 0) {
		size_t n = MIN(sizeof(rev), sizeof(out->secure_fw_version) - 1u);

		memcpy(out->secure_fw_version, rev, n);
		out->secure_fw_version[n] = '\0';
		alif_se_warn_if_seram_below_floor(out->secure_fw_version);
	}

	/* SoC part-number code. */
	uint32_t part = 0u;

	rc = se_service_get_device_part_number(&part);
	keep_first(&first, se_rc_to_alp(rc));
	if (rc == 0) {
		out->part_number = part;
	}

	/* Die revision + lifecycle state (LCS) + factory-fused serial.
	 * A pure query: reads the device-revision-data response the SE
	 * already holds -- no STOC / fuse / lifecycle write. */
	get_device_revision_data_t dev = { 0 };

	rc = se_service_system_get_device_data(&dev);
	keep_first(&first, se_rc_to_alp(rc));
	if (rc == 0) {
		size_t n = MIN(sizeof(dev.SerialN), sizeof(out->serial));

		out->revision_id = (uint32_t)dev.revision_id;
		out->lifecycle   = (uint32_t)dev.LCS;
		/* dev.SerialN is `volatile uint8_t[8]` (hal_alif wire struct);
		 * dev is a local, single-threaded, already fully populated by
		 * the se_service_system_get_device_data() call above, so the
		 * volatile is stale by the time we read it here -- cast it
		 * away rather than dropping the qualifier implicitly (which
		 * -Werror=discarded-qualifiers correctly flags). */
		memcpy(out->serial, (const void *)dev.SerialN, n);
		out->serial_len = (uint8_t)n;
	}

	return first;
}

static alp_status_t alif_se_ping(void)
{
	return se_rc_to_alp(se_service_heartbeat());
}

static const alp_soc_info_ops_t _ops = {
	.read = alif_se_read,
	.ping = alif_se_ping,
};

ALP_BACKEND_REGISTER(soc_info,
                     alif_se,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_SOC_INFO_ALIF_SE */
