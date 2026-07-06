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
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

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
		memcpy(out->serial, dev.SerialN, n);
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
