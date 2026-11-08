/* SPDX-License-Identifier: Apache-2.0
 *
 * Host test for the AEN HE/HP update-log mailbox (src/backends/update_log/
 * aen_m55_owner.c), built with gcc and pthreads. The SRAM0 window is a plain
 * buffer that outlives each "core", and a core reset is a thread restart plus
 * a reset of that core's RAM state, so a retained REQ/RSP survives exactly as
 * it does in SRAM0 across a one-core reset.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "alp/backend.h"
#include "alp/update_log.h"
#include "backends/update_log/update_log_ops.h"

static _Alignas(256) unsigned char g_sram[0x200];
static uint32_t g_beacon[2][4];

#define CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_CLIENT         1
#define CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER          1
#define CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_RPC_TIMEOUT_MS 300
#define AEN_ULOG_REQ_ADDR                                ((uintptr_t)&g_sram[0])
#define AEN_ULOG_RSP_ADDR                                ((uintptr_t)&g_sram[0x100])
#define AEN_ULOG_HP_BEACON_ADDR                          ((uintptr_t)g_beacon[0])
#define AEN_ULOG_HE_BEACON_ADDR                          ((uintptr_t)g_beacon[1])
#undef ALP_BACKEND_REGISTER
#define ALP_BACKEND_REGISTER(c, n, ...) typedef int host_unused_##c##_##n

#include "../../../../src/backends/update_log/aen_m55_owner.c"

/* The "log" the owner appends to. */
static struct alp_update_log g_log;
static volatile unsigned     g_appends;
alp_update_log_t            *alp_update_log_open(void)
{
	return &g_log;
}
alp_status_t alp_update_log_append(alp_update_log_t *l, const alp_update_log_entry_t *e)
{
	(void)l;
	(void)e;
	g_appends++;
	return ALP_OK;
}
alp_status_t alp_update_log_verify(alp_update_log_t *l, alp_update_log_verdict_t *v, uint64_t *b)
{
	(void)l;
	*v = ALP_UPDATE_LOG_VERIFY_OK;
	*b = 0;
	return ALP_OK;
}
alp_status_t alp_update_log_count(alp_update_log_t *l, uint64_t *o)
{
	(void)l;
	*o = g_appends;
	return ALP_OK;
}
alp_status_t alp_update_log_get(alp_update_log_t *l, uint64_t s, alp_update_log_entry_t *e)
{
	(void)l;
	(void)s;
	memset(e, 0, sizeof(*e));
	return ALP_OK;
}

static pthread_t g_owner;
static void     *owner_main(void *a)
{
	(void)a;
	update_log_aen_m55_owner_run();
	return NULL;
}

static void start_owner(void)
{
	pthread_create(&g_owner, NULL, owner_main, NULL);
	usleep(50 * 1000); /* let it clear the mailbox and start polling */
}

/* Reset the owner (HP) core: its RAM (last_seq, stack) dies, SRAM0 does not.
 */
static void reset_owner(void)
{
	pthread_cancel(g_owner); /* k_msleep/usleep are cancellation points */
	pthread_join(g_owner, NULL);
	start_owner();
}

/* Reset the client (HE) core: its RAM state dies, SRAM0 does not. */
static void reset_client(void)
{
	g_client_seq       = 0U;
	g_client_mhu_ready = false;
}

static int fails;
#define CHECK(c, msg) \
	do { \
		if (!(c)) { \
			printf("FAIL: %s\n", msg); \
			fails++; \
		} else { \
			printf("ok:   %s\n", msg); \
		} \
	} while (0)

int main(void)
{
	alp_update_log_entry_t e;
	memset(&e, 0, sizeof(e));

	start_owner();
	CHECK(aen_append(&e) == ALP_OK && g_appends == 1, "append served");

	/* HE restarts, HP keeps running with last_seq == 1 and RSP->seq == 1: the
	 * restarted client's first seq collides with both unless it is seeded. */
	reset_client();
	CHECK(aen_append(&e) == ALP_OK && g_appends == 2,
	      "HE restart: first append is served, not skipped or answered by the stale RSP");
	CHECK(aen_append(&e) == ALP_OK && g_appends == 3, "HE restart: next append served");
	reset_client();
	CHECK(aen_append(&e) == ALP_OK && g_appends == 4, "second HE restart: append served");

	/* HP restarts; the served REQ is still in SRAM0. */
	unsigned before = g_appends;
	reset_owner();
	usleep(100 * 1000);
	CHECK(g_appends == before, "HP restart: stale REQ not replayed");
	CHECK(aen_append(&e) == ALP_OK && g_appends == before + 1, "HP restart: new append served");

	/* Both restart: stale REQ and RSP both retained. */
	reset_client();
	reset_owner();
	usleep(100 * 1000);
	CHECK(g_appends == before + 1, "both restart: stale REQ not replayed");
	CHECK(aen_append(&e) == ALP_OK && g_appends == before + 2, "both restart: new append served");

	printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
	return fails ? 1 : 0;
}
