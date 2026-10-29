/* SPDX-License-Identifier: Apache-2.0
 *
 * Alif E4/E8 dual-M55 update-log owner.
 *
 * HE/app side:
 *   alp_update_log_open() selects this backend only when the owner replies and
 *   the build/profile explicitly says the MRAM log partition is behind the
 *   SE/device firewall. Normal application firmware then has no direct write
 *   path to old entries; it can only ask the owner to append or verify.
 *
 * HP/owner side:
 *   update_log_aen_m55_owner_run() opens the local update-log store
 *   (normally MRAM NVS through the software tier) and services requests from
 *   the application core over the non-secure RTSS MHU-1 doorbell plus a small
 *   shared-SRAM mailbox.
 *
 * The exact SE/DEVICE firewall provisioning is intentionally outside this
 * public source file. This backend does not report HW_ENFORCED until that
 * board-specific policy has already been provisioned and silicon-proven.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

#include "alp/backend.h"
#include "alp/peripheral.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_CLIENT) || \
    defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)

/* Same CPU-relative MHU-1 aliases as examples/aen/aen-dualcore-ipc. From either
 * M55 core, 0x400B0000 is "my TX frame to the peer" and 0x400A0000 is "my RX
 * frame from the peer". */
#define AEN_ULOG_MHU1_TX_FRAME 0x400B0000U
#define AEN_ULOG_MHU1_RX_FRAME 0x400A0000U
#define AEN_ULOG_TX_CH0_SET    (AEN_ULOG_MHU1_TX_FRAME + 0x0CU)
#define AEN_ULOG_TX_ACC_REQ    (AEN_ULOG_MHU1_TX_FRAME + 0xF88U)
#define AEN_ULOG_TX_ACC_RDY    (AEN_ULOG_MHU1_TX_FRAME + 0xF8CU)
#define AEN_ULOG_RX_CH0_ST     (AEN_ULOG_MHU1_RX_FRAME + 0x00U)
#define AEN_ULOG_RX_CH0_CLR    (AEN_ULOG_MHU1_RX_FRAME + 0x08U)
#define AEN_ULOG_DOORBELL_BIT  0x1U

/* Shared global SRAM0 mailbox. Keep clear of the dualcore example beacons and
 * the OpenAMP vring carve-out used by aen-rpc-pingpong. */
#define AEN_ULOG_REQ ((volatile struct aen_ulog_req *)0x02003000U)
#define AEN_ULOG_RSP ((volatile struct aen_ulog_rsp *)0x02003100U)

#define AEN_ULOG_MAGIC   0x554C4F47U /* "ULOG" */
#define AEN_ULOG_VERSION 1U

/* Bench proof beacons in global SRAM0, away from the request/reply mailboxes.
 * They are deliberately simple so a J-Link `mem32` read can prove owner/client
 * progress even when the UART capture misses early boot text. */
#define AEN_ULOG_HP_BEACON       ((volatile uint32_t *)0x02000060U)
#define AEN_ULOG_HE_BEACON       ((volatile uint32_t *)0x02001060U)
#define AEN_ULOG_HP_BEACON_MAGIC 0x554C4F90U
#define AEN_ULOG_HE_BEACON_MAGIC 0x554C4FE0U

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_RPC_TIMEOUT_MS)
#define AEN_ULOG_MHU_READY_TIMEOUT_MS CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_RPC_TIMEOUT_MS
#else
#define AEN_ULOG_MHU_READY_TIMEOUT_MS 2000
#endif

enum aen_ulog_op {
	AEN_ULOG_OP_READY  = 1,
	AEN_ULOG_OP_APPEND = 2,
	AEN_ULOG_OP_VERIFY = 3,
	AEN_ULOG_OP_COUNT  = 4,
	AEN_ULOG_OP_GET    = 5,
};

struct aen_ulog_req {
	uint32_t               magic;
	uint32_t               version;
	uint32_t               seq;
	uint32_t               op;
	uint64_t               arg;
	alp_update_log_entry_t entry;
};

struct aen_ulog_rsp {
	uint32_t                 magic;
	uint32_t                 version;
	uint32_t                 seq;
	int32_t                  status;
	uint64_t                 value;
	uint64_t                 bad_seq;
	alp_update_log_verdict_t verdict;
	alp_update_log_entry_t   entry;
};

_Static_assert(sizeof(struct aen_ulog_req) <= 0x100U, "request overruns the 0x02003000 slot");
_Static_assert(sizeof(struct aen_ulog_rsp) <= 0x100U, "reply overruns the 0x02003100 slot");

/* Cache maintenance for the SRAM0 mailbox. The two M55 cores do not share a
 * D-cache, so with CONFIG_DCACHE=y a plain volatile store can sit in the
 * writer's cache and a plain load can hit a stale line in the reader's.
 * The writer cleans (flushes) what it wrote; the reader invalidates before
 * it reads. With the D-cache off (or cache management not built) the
 * sys_cache_data_* calls are no-ops, so the same code serves both builds.
 * The mailbox slots are 0x100 apart and line-aligned, so a range never
 * shares a cache line with the other direction's slot. A struct spans
 * several lines and a flush writes them back in no fixed order, so the
 * publisher flushes the payload first, fences, and only then writes and
 * flushes the line that carries seq. */
static inline void aen_mbox_flush(volatile void *p, size_t len)
{
	(void)sys_cache_data_flush_range((void *)p, len);
}

static inline void aen_mbox_invalidate(volatile void *p, size_t len)
{
	(void)sys_cache_data_invd_range((void *)p, len);
}

static void aen_mhu_ring(void)
{
	barrier_dmem_fence_full();
	sys_write32(AEN_ULOG_DOORBELL_BIT, AEN_ULOG_TX_CH0_SET);
}

static void aen_mhu_drain(void)
{
	if ((sys_read32(AEN_ULOG_RX_CH0_ST) & AEN_ULOG_DOORBELL_BIT) != 0U) {
		sys_write32(0xFFFFFFFFU, AEN_ULOG_RX_CH0_CLR);
		barrier_dmem_fence_full();
	}
}

static alp_status_t aen_mhu_sender_ready(void)
{
	sys_write32(1U, AEN_ULOG_TX_ACC_REQ);
	int64_t deadline = k_uptime_get() + AEN_ULOG_MHU_READY_TIMEOUT_MS;
	while (k_uptime_get() < deadline) {
		if (sys_read32(AEN_ULOG_TX_ACC_RDY) != 0U) {
			return ALP_OK;
		}
		k_msleep(1);
	}
	return (sys_read32(AEN_ULOG_TX_ACC_RDY) != 0U) ? ALP_OK : ALP_ERR_TIMEOUT;
}

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_CLIENT)

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN)
#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE)
#error "The app-immutable profile and the firewall probe are mutually exclusive"
#endif
static alp_status_t aen_firewall_proven(void)
{
	return ALP_OK;
}
#else
static alp_status_t aen_firewall_proven(void)
{
	return ALP_ERR_NOSUPPORT;
}
#endif

static bool     g_client_mhu_ready;
static uint32_t g_client_seq;

static void client_beacon(enum aen_ulog_op op, uint32_t seq, alp_status_t status)
{
	AEN_ULOG_HE_BEACON[0] = AEN_ULOG_HE_BEACON_MAGIC;
	AEN_ULOG_HE_BEACON[1] = (uint32_t)op;
	AEN_ULOG_HE_BEACON[2] = seq;
	AEN_ULOG_HE_BEACON[3] = (uint32_t)status;
}

static alp_status_t client_call(enum aen_ulog_op              op,
                                uint64_t                      arg,
                                const alp_update_log_entry_t *entry,
                                struct aen_ulog_rsp          *rsp_out)
{
	if (!g_client_mhu_ready) {
		alp_status_t rc = aen_mhu_sender_ready();
		if (rc != ALP_OK) {
			client_beacon(op, g_client_seq, rc);
			return rc;
		}
		g_client_mhu_ready = true;
	}

	uint32_t seq = ++g_client_seq;
	if (seq == 0U) {
		seq = ++g_client_seq;
	}

	client_beacon(op, seq, ALP_ERR_NOT_READY);

	AEN_ULOG_REQ->magic   = AEN_ULOG_MAGIC;
	AEN_ULOG_REQ->version = AEN_ULOG_VERSION;
	AEN_ULOG_REQ->op      = (uint32_t)op;
	AEN_ULOG_REQ->arg     = arg;
	if (entry != NULL) {
		AEN_ULOG_REQ->entry = *entry;
	} else {
		memset((void *)&AEN_ULOG_REQ->entry, 0, sizeof(AEN_ULOG_REQ->entry));
	}
	aen_mbox_flush(AEN_ULOG_REQ, sizeof(*AEN_ULOG_REQ));
	barrier_dmem_fence_full();
	AEN_ULOG_REQ->seq = seq;
	aen_mbox_flush(&AEN_ULOG_REQ->seq, sizeof(AEN_ULOG_REQ->seq));

	aen_mhu_ring();

	int64_t deadline = k_uptime_get() + CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_RPC_TIMEOUT_MS;
	while (k_uptime_get() < deadline) {
		aen_mhu_drain();
		aen_mbox_invalidate(&AEN_ULOG_RSP->seq, sizeof(AEN_ULOG_RSP->seq));
		if (AEN_ULOG_RSP->seq == seq) {
			barrier_dmem_fence_full();
			aen_mbox_invalidate(AEN_ULOG_RSP, sizeof(*AEN_ULOG_RSP));
			if (AEN_ULOG_RSP->magic != AEN_ULOG_MAGIC ||
			    AEN_ULOG_RSP->version != AEN_ULOG_VERSION) {
				return ALP_ERR_IO;
			}
			if (rsp_out != NULL) {
				*rsp_out = *AEN_ULOG_RSP;
			}
			alp_status_t status = (alp_status_t)AEN_ULOG_RSP->status;
			client_beacon(op, seq, status);
			return status;
		}
		k_msleep(1);
	}

	client_beacon(op, seq, ALP_ERR_TIMEOUT);
	return ALP_ERR_TIMEOUT;
}

static alp_status_t aen_ready(void)
{
	alp_status_t rc = aen_firewall_proven();
	if (rc != ALP_OK) {
		client_beacon(AEN_ULOG_OP_READY, 0U, rc);
		return rc;
	}
	rc = client_call(AEN_ULOG_OP_READY, 0U, NULL, NULL);
	return (rc == ALP_ERR_TIMEOUT) ? ALP_ERR_NOSUPPORT : rc;
}

static alp_status_t aen_append(const alp_update_log_entry_t *entry)
{
	if (entry == NULL) {
		return ALP_ERR_INVAL;
	}
	return client_call(AEN_ULOG_OP_APPEND, 0U, entry, NULL);
}

static alp_status_t aen_verify(alp_update_log_verdict_t *verdict, uint64_t *bad_seq)
{
	if (verdict == NULL) {
		return ALP_ERR_INVAL;
	}

	struct aen_ulog_rsp rsp;
	alp_status_t        rc = client_call(AEN_ULOG_OP_VERIFY, 0U, NULL, &rsp);
	if (rc != ALP_OK) {
		return rc;
	}

	*verdict = rsp.verdict;
	if (bad_seq != NULL) {
		*bad_seq = rsp.bad_seq;
	}
	return ALP_OK;
}

static alp_status_t aen_count(uint64_t *out)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}

	struct aen_ulog_rsp rsp;
	alp_status_t        rc = client_call(AEN_ULOG_OP_COUNT, 0U, NULL, &rsp);
	if (rc == ALP_OK) {
		*out = rsp.value;
	}
	return rc;
}

static alp_status_t aen_get(uint64_t seq, alp_update_log_entry_t *out)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}

	struct aen_ulog_rsp rsp;
	alp_status_t        rc = client_call(AEN_ULOG_OP_GET, seq, NULL, &rsp);
	if (rc == ALP_OK) {
		*out = rsp.entry;
	}
	return rc;
}

static const alp_update_log_ops_t g_aen_client_ops = {
	.assurance = ALP_UPDATE_LOG_HW_ENFORCED,
	.ready     = aen_ready,
	.append    = aen_append,
	.verify    = aen_verify,
	.count     = aen_count,
	.get       = aen_get,
};

ALP_BACKEND_REGISTER(update_log,
                     aen_m55_owner_e4,
                     {
                         .silicon_ref = "alif:ensemble:e4",
                         .vendor      = "alif_aen_m55_owner",
                         .base_caps   = 0u,
                         .priority    = 30,
                         .ops         = &g_aen_client_ops,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(update_log,
                     aen_m55_owner_e8,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif_aen_m55_owner",
                         .base_caps   = 0u,
                         .priority    = 30,
                         .ops         = &g_aen_client_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_CLIENT */

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)

static void owner_reply(uint32_t                      seq,
                        alp_status_t                  status,
                        uint64_t                      value,
                        alp_update_log_verdict_t      verdict,
                        uint64_t                      bad_seq,
                        const alp_update_log_entry_t *entry)
{
	AEN_ULOG_RSP->magic   = AEN_ULOG_MAGIC;
	AEN_ULOG_RSP->version = AEN_ULOG_VERSION;
	AEN_ULOG_RSP->status  = (int32_t)status;
	AEN_ULOG_RSP->value   = value;
	AEN_ULOG_RSP->verdict = verdict;
	AEN_ULOG_RSP->bad_seq = bad_seq;
	if (entry != NULL) {
		AEN_ULOG_RSP->entry = *entry;
	} else {
		memset((void *)&AEN_ULOG_RSP->entry, 0, sizeof(AEN_ULOG_RSP->entry));
	}
	aen_mbox_flush(AEN_ULOG_RSP, sizeof(*AEN_ULOG_RSP));
	barrier_dmem_fence_full();
	AEN_ULOG_RSP->seq = seq;
	aen_mbox_flush(&AEN_ULOG_RSP->seq, sizeof(AEN_ULOG_RSP->seq));
	aen_mhu_ring();
}

void update_log_aen_m55_owner_run(void)
{
	alp_status_t ready_rc = aen_mhu_sender_ready();
	if (ready_rc != ALP_OK) {
		AEN_ULOG_HP_BEACON[0] = AEN_ULOG_HP_BEACON_MAGIC;
		AEN_ULOG_HP_BEACON[1] = (uint32_t)ready_rc;
		AEN_ULOG_HP_BEACON[2] = 0U;
		AEN_ULOG_HP_BEACON[3] = 0U;
		return;
	}

	alp_update_log_t *log      = alp_update_log_open();
	uint32_t          last_seq = 0U;
	uint32_t          served   = 0U;

	/* Clear both directions of the mailbox. RSP->seq alone is not enough: a
	 * request the previous owner already served stays in retained SRAM0
	 * (REQ->seq != 0), and with last_seq reset to 0 it would be run again,
	 * replaying its APPEND into the audit log. A request the client posts
	 * before this point is dropped; the client times out and retries. */
	AEN_ULOG_REQ->seq = 0U;
	AEN_ULOG_RSP->seq = 0U;
	aen_mbox_flush(&AEN_ULOG_REQ->seq, sizeof(AEN_ULOG_REQ->seq));
	aen_mbox_flush(&AEN_ULOG_RSP->seq, sizeof(AEN_ULOG_RSP->seq));
	AEN_ULOG_HP_BEACON[0] = AEN_ULOG_HP_BEACON_MAGIC;
	AEN_ULOG_HP_BEACON[1] = (log != NULL) ? (uint32_t)ALP_OK : (uint32_t)ALP_ERR_NOT_READY;
	AEN_ULOG_HP_BEACON[2] = 0U;
	AEN_ULOG_HP_BEACON[3] = 0U;

	for (;;) {
		aen_mhu_drain();

		aen_mbox_invalidate(&AEN_ULOG_REQ->seq, sizeof(AEN_ULOG_REQ->seq));
		uint32_t seq = AEN_ULOG_REQ->seq;
		if (seq == 0U || seq == last_seq) {
			k_msleep(1);
			continue;
		}
		last_seq = seq;
		barrier_dmem_fence_full();
		aen_mbox_invalidate(AEN_ULOG_REQ, sizeof(*AEN_ULOG_REQ));

		if (AEN_ULOG_REQ->magic != AEN_ULOG_MAGIC || AEN_ULOG_REQ->version != AEN_ULOG_VERSION) {
			owner_reply(seq, ALP_ERR_VERSION, 0U, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN, 0U, NULL);
			continue;
		}

		alp_status_t             rc      = (log != NULL) ? ALP_OK : ALP_ERR_NOT_READY;
		uint64_t                 value   = 0U;
		uint64_t                 bad_seq = 0U;
		alp_update_log_verdict_t verdict = ALP_UPDATE_LOG_VERIFY_OK;
		alp_update_log_entry_t   entry;
		memset(&entry, 0, sizeof(entry));

		if (rc == ALP_OK) {
			switch ((enum aen_ulog_op)AEN_ULOG_REQ->op) {
			case AEN_ULOG_OP_READY:
				break;
			case AEN_ULOG_OP_APPEND:
				rc = alp_update_log_append(log,
				                           (const alp_update_log_entry_t *)&AEN_ULOG_REQ->entry);
				break;
			case AEN_ULOG_OP_VERIFY:
				rc = alp_update_log_verify(log, &verdict, &bad_seq);
				break;
			case AEN_ULOG_OP_COUNT:
				rc = alp_update_log_count(log, &value);
				break;
			case AEN_ULOG_OP_GET:
				rc = alp_update_log_get(log, AEN_ULOG_REQ->arg, &entry);
				break;
			default:
				rc = ALP_ERR_INVAL;
				break;
			}
		}

		AEN_ULOG_HP_BEACON[1] = (uint32_t)rc;
		AEN_ULOG_HP_BEACON[2] = AEN_ULOG_REQ->op;
		AEN_ULOG_HP_BEACON[3] = ++served;
		owner_reply(seq, rc, value, verdict, bad_seq, &entry);
	}
}

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER */

#endif /* CLIENT || OWNER */
