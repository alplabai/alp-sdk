/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bodies for <alp/ext/alif/storage.h>.  OSPI SecAES engine on
 * Ensemble E4 / E6 / E8.
 *
 * -------------------------------------------------------------------
 * Issue #224: the deferral was wrong -- this is implementable today
 * -------------------------------------------------------------------
 * This was deferred as "blocked on the vendor HAL pack" on the theory
 * that the host would need to invent the key-load register timing.
 * Two things say that premise is stale:
 *
 *   1. The E8 HWRM's AES_CONTROL description makes LD_KEY the bit the
 *      SE arms once *it* has written the key -- the host never pokes
 *      raw key bytes into an OSPI register (that's why no code in
 *      hal_alif's drivers/ospi/src/ospi.c ever touches
 *      AES_CONTROL_LD_KEY: it isn't the host's job).
 *   2. Alif publish the host-side call for this: AUGD0014 "SE Host
 *      Services API" v1.109.0 documents
 *      SERVICES_application_ospi_write_key(handle, command, key,
 *      error_code) with OSPI_WRITE_OTP_KEY_OSPI0/1 and
 *      OSPI_WRITE_EXTERNAL_KEY_OSPI0/1 command codes
 *      (se_services/include/services_lib_api.h, hal_alif v2.3.0 --
 *      pinned by this SDK's west.yml).  Bench/vendor-confirmed only
 *      for alif:ensemble:e8 so far -- E4/E6 SE firmware exposing
 *      service 105 is untried, not merely unsupported.
 *
 * That said, SERVICES_application_ospi_write_key's *body* is not
 * compiled anywhere in that Apache-2.0 module -- se_services/ ships
 * only se_service.c, and every low-level SERVICES_* prototype in
 * services_lib_api.h (pinmux, pad control, uart_write, ospi_write_key,
 * dmpu, verify_image, ...) is declared but never defined there; only
 * the higher se_service_* wrappers get bodies.  So this backend does
 * not call that symbol -- it builds the identical wire packet
 * (ospi_write_key_svc_t, services_lib_protocol.h;
 * SERVICE_APPLICATION_OSPI_WRITE_KEY_ID, services_lib_ids.h) by hand
 * and hands it to the transport hal_alif *does* export for exactly
 * this shape of caller: the public se_service_send_request(), added
 * by zephyr/patches/hal_alif/0002-se-service-add-public-send-request
 * .patch -- the identical seam src/backends/security/se_cryptocell.c
 * already rides for its CryptoCell AES/SHA/AEAD packets, and the same
 * SE mailbox the read-only se_service client is bench-proven live on
 * (docs/aen-se-services.md).  See docs/aen-se-services.md #2.5 for the
 * service writeup + bench-execution posture.
 *
 * Gated behind CONFIG_ALP_SDK_STORAGE_ALIF_SECAES (default OFF; depends
 * on HAS_ALIF_SE_SERVICES) so this file -- which every board links
 * unconditionally (zephyr/CMakeLists.txt) -- keeps compiling, and this
 * function keeps returning ALP_ERR_NOSUPPORT, on every board that
 * doesn't link hal_alif's SE client.  Default OFF (not ON) because the
 * round-trip is UNVERIFIED ON SILICON (see below) -- CONFIG_ALP_SDK_
 * SECURITY_SE_CRYPTOCELL_SEND_SEAM defaults ON only because that path
 * bench-PASSED (docs/aen-bench-bringup.md); this one hasn't, so it does
 * not get the same default until it does.
 *
 * alp_alif_storage_secaes_get_status() stays ALP_ERR_NOSUPPORT
 * unconditionally: AUGD0014 does not publish an SE service that reads
 * the OSPI SecAES engine's ARMED/ENGAGED/error state back.  The
 * host-side ospi_aes_regs register block (drivers/ospi/include/
 * ospi.h) is the SE's own domain for the LD_KEY sequence -- inventing
 * a register-read here would be exactly the "guess the vendor timing"
 * trap issue #224 was reopened to avoid.  Revisit if Alif publish a
 * status service, or a bench with SETOOLS + a sacrificial board
 * confirms the register is safely host-readable.
 *
 * UNVERIFIED ON SILICON.  No bench unit reachable at implementation
 * time has an OSPI SecAES-relevant part populated: the bench module
 * is board rev r1 (EEPROM manifest "E1M-AEN801 r1", serial
 * 2617-0001); the Macronix MX25UM25645GXDI00 this targets
 * (OSPI_WRITE_EXTERNAL_KEY_OSPI0 -- issue #224 / #915: R2 BOM
 * populates only OSPI0, DNP=0) ships on the R2 BOM, not this one. The
 * SE-transport half of this call -- packet marshalling, send/response,
 * error mapping -- reuses the identical se_service_send_request()
 * path already bench-proven for SE CryptoCell (aen-se-crypto,
 * SHA-256 + AES-128-GCM MATCH); the OSPI write-key round-trip itself
 * has not been run against real SE firmware.  ADR-0030's SES v110
 * floor is the general SE-service floor this SDK tracks; no
 * per-service minimum beyond that is stated in the material available
 * here (AUGD0014 is Alif-confidential -- paraphrased, not quoted),
 * and the bench SES A0 v1.110.0 already exceeds both it and AUGD0014's
 * own v1.109.0 doc revision.
 *
 * NOT COVERED BY tests/unit/storage_registry: that native_sim build has
 * no HAS_ALIF_SE_SERVICES, so CONFIG_ALP_SDK_STORAGE_ALIF_SECAES is off
 * there and the #if branch below never compiles into that test binary.
 * The ZTESTs there exercise only the NULL/vendor-gate/key_bytes
 * validation and the NOSUPPORT fallback; the SE round-trip itself has
 * no automated coverage anywhere in this tree.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/ext/alif/storage.h>
#include <alp/storage.h>

#include "alp_slot_claim.h"
#include "backends/storage/storage_ops.h"

#if defined(CONFIG_ALP_SDK_STORAGE_ALIF_SECAES)
/* hal_alif SE-service client (Apache-2.0).  Transitively provides
 * SERVICE_APPLICATION_OSPI_WRITE_KEY_ID (services_lib_ids.h),
 * ospi_write_key_svc_t / OSPI_WRITE_EXTERNAL_KEY_OSPI0 /
 * OSPI_WRITE_KEY_SUCCESS (services_lib_protocol.h / services_lib_api.h),
 * and SERVICES_REQ_SUCCESS -- the transport-layer header verdict, also
 * services_lib_protocol.h -- all pulled in via services_lib_api.h. */
#include <se_service.h>
#endif

static bool _is_alif_backend(const alp_storage_t *s)
{
	return s != NULL && s->backend != NULL && s->backend->vendor != NULL &&
	       strcmp(s->backend->vendor, "alif") == 0;
}

#if defined(CONFIG_ALP_SDK_STORAGE_ALIF_SECAES)

/* se_service_send_request() returns 0 on a completed transport
 * round-trip, a negative errno for the transport (-EAGAIN timeout,
 * -EBUSY SE busy, -EINVAL bad arg), or never a positive value -- the
 * SE's own verdict lands in the packet's header.hdr_error_code /
 * resp_error_code fields instead, both checked separately by the
 * caller. */
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
 * Build the OSPI write-key request packet, send it, and read back BOTH
 * verdict fields the vendor wire format carries.  send_msg_to_se()
 * (hal_alif se_service.c) returns rc==0 as soon as ANY response frame
 * arrives over the MHUv2 mailbox -- it never itself inspects
 * hdr_error_code or resp_error_code, and no se_service_* wrapper in
 * hal_alif does either (a search for hdr_error_code across that module
 * turns up nothing).  So an SE that NACKs the request at the transport
 * layer (SERVICES_RESP_UNKNOWN_COMMAND if this SE firmware simply
 * doesn't implement service 105) still returns rc==0 here -- checking
 * hdr_error_code is the only thing standing between that NACK and a
 * silent false ALP_OK.
 */
static alp_status_t _do_key_provision(const uint8_t *key)
{
	/* R2 BOM populates only OSPI0 (Macronix MX25UM25645GXDI00, DNP=0);
	 * no SoM in scope wires OSPI1.  Revisit (add a bus selector) if
	 * that changes -- see the file header. */
	ospi_write_key_svc_t pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.hdr_service_id = SERVICE_APPLICATION_OSPI_WRITE_KEY_ID;
	pkt.send_command          = OSPI_WRITE_EXTERNAL_KEY_OSPI0;
	memcpy((void *)pkt.send_key, key, sizeof(pkt.send_key));
	/* Pre-seed a sentinel none of the real OSPI_WRITE_KEY_* codes
	 * (0x0/0x200/0x201) produce.  The memset above already left
	 * resp_error_code == 0, bit-identical to OSPI_WRITE_KEY_SUCCESS --
	 * an SE that never touches this field at all would otherwise read
	 * back as a false ALP_OK on top of the hdr_error_code gap above. */
	pkt.resp_error_code = UINT32_MAX;

	/* send_msg_to_se() flushes this packet to SE-visible memory via
	 * sys_cache_data_flush_range() before it sends.  That is harmless
	 * to the wipe below only because every AEN board configures
	 * zephyr,sram = &dtcm and Cortex-M55 TCM bypasses the D-cache
	 * entirely -- moving that devicetree node to a cached region would
	 * leave a stale copy of the key in the data cache this function's
	 * memset never touches. */
	int rc = se_service_send_request((uint32_t *)&pkt, (uint32_t)sizeof(pkt));

	alp_status_t status;
	if (rc != 0) {
		status = se_rc_to_alp(rc);
	} else if (pkt.header.hdr_error_code != SERVICES_REQ_SUCCESS) {
		status = ALP_ERR_IO;
	} else if (pkt.resp_error_code != OSPI_WRITE_KEY_SUCCESS) {
		status = ALP_ERR_IO;
	} else {
		status = ALP_OK;
	}

	/* pkt.send_key is an INLINE copy of the key (unlike the CryptoCell
	 * AEAD packets in se_cryptocell.c, which only ever carry a DMA
	 * address) -- this packet is the one place this call holds the key
	 * in clear.  Wipe the whole packet, not just send_key.  The
	 * address-taken "r" operand (not a bare clobber) is required: a
	 * bare "" ::: "memory" barrier only stops the compiler eliding the
	 * memset if it cannot already prove &pkt does not escape, which
	 * holds today only because se_service_send_request() lives in a
	 * separate translation unit -- under LTO, or if that call is ever
	 * inlined, a bare clobber lets the compiler prove pkt is dead and
	 * drop the memset. */
	memset((void *)&pkt, 0, sizeof(pkt));
	__asm__ volatile("" : : "r"(&pkt) : "memory");

	return status;
}

#else /* !CONFIG_ALP_SDK_STORAGE_ALIF_SECAES */

static alp_status_t _do_key_provision(const uint8_t *key)
{
	(void)key;
	/* No hal_alif SE client linked in this build -- no transport to
	 * reach. */
	return ALP_ERR_NOSUPPORT;
}

#endif /* CONFIG_ALP_SDK_STORAGE_ALIF_SECAES */

alp_status_t
alp_alif_storage_secaes_key_provision(alp_storage_t *s, const uint8_t *key, uint8_t key_bytes)
{
	if (s == NULL) return ALP_ERR_INVAL;
	/* Brackets the whole call, including the up-to-~35 s SE round-trip
	 * inside _do_key_provision() (se_service_send_request()'s
	 * se_service_ensure_ready() + svc_mutex each carry hal_alif's 15 s
	 * MUTEX_TIMEOUT, plus a 10 s SERVICE_TIMEOUT).  alp_storage_close()
	 * (src/storage_dispatch.c) drains this same counter before it frees
	 * the slot, so a concurrent close cannot free/recycle *s while a
	 * provision is in flight (issue #629's op-vs-close guard,
	 * src/common/alp_slot_claim.h) -- without this, a close racing a
	 * parked call would zero+reissue the slot to a different device out
	 * from under it. */
	if (!alp_handle_op_enter(&s->lifecycle, &s->active_ops)) return ALP_ERR_NOT_READY;

	alp_status_t status;
	if (!_is_alif_backend(s)) {
		status = ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	} else if (key == NULL) {
		status = ALP_ERR_INVAL;
	} else if (key_bytes != 16u) {
		/* ospi_write_key_svc_t.send_key is a fixed uint8_t[16]
		 * (services_lib_protocol.h) and OSPI_KEY_LENGTH_BYTES == 16
		 * (services_lib_api.h) -- the SE OSPI write-key service
		 * takes AES-128 only, unlike the portable
		 * alp_storage_configure_inline_aes surface's 16/24/32. */
		status = ALP_ERR_INVAL;
	} else {
		status = _do_key_provision(key);
	}

	alp_handle_op_leave(&s->active_ops);
	return status;
}

alp_status_t alp_alif_storage_secaes_get_status(alp_storage_t *s, uint32_t *status_out)
{
	if (s == NULL || status_out == NULL) return ALP_ERR_INVAL;
	if (!alp_handle_op_enter(&s->lifecycle, &s->active_ops)) return ALP_ERR_NOT_READY;

	alp_status_t status;
	if (!_is_alif_backend(s)) {
		status = ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	} else {
		*status_out = (uint32_t)ALP_ALIF_STORAGE_SECAES_STATUS_IDLE;
		/* No AUGD0014-published SE service reads the SecAES engine's
		 * ARMED/ENGAGED/error state back -- see the file header. */
		status = ALP_ERR_NOSUPPORT;
	}

	alp_handle_op_leave(&s->active_ops);
	return status;
}
