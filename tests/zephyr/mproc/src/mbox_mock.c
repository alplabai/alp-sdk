/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mock MBOX controllers + doorbell-slot tests for the Zephyr mproc backend
 * (alp_sdk.mproc.doorbell).  Covers the payload-less (mtu 0) doorbell path,
 * the oversize refusal for mtu > 0, and the alp-mbox-rxN receive alias with
 * its same-device fallback.  The alp-shmem0 node in native_sim_native_64.overlay
 * is the fixed address 0x70000000, which nothing maps on native_sim, so the
 * suite maps one anonymous page there itself (raw Linux x86-64 mmap syscall:
 * the app has no host libc) and skips where that is unavailable.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/ztest.h>

#include <alp/mproc.h>

#define DT_DRV_COMPAT alp_test_mbox

struct mock_data {
	mbox_callback_t cb;
	void           *cb_ctx;
	bool            enabled;
	int             sends;
	bool            last_null;
	size_t          last_size;
};

#define SHM_ADDR 0x70000000UL
#define SHM_SIZE 4096u
static uint8_t *g_shm;

static bool map_shm(void)
{
#if defined(__x86_64__) && defined(__linux__)
	/* mmap(SHM_ADDR, SHM_SIZE, PROT_READ|PROT_WRITE,
	 *      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0) */
	long          ret;
	register long r10 __asm__("r10") = 0x2 | 0x20 | 0x100000;
	register long r8 __asm__("r8")   = -1;
	register long r9 __asm__("r9")   = 0;
	__asm__ volatile(
	    "syscall"
	    : "=a"(ret)
	    : "0"(9L), "D"(SHM_ADDR), "S"((long)SHM_SIZE), "d"(3L), "r"(r10), "r"(r8), "r"(r9)
	    : "rcx", "r11", "memory");
	if (ret != (long)SHM_ADDR) return false;
	g_shm = (uint8_t *)SHM_ADDR;
	return true;
#else
	return false;
#endif
}

static void *suite_setup(void)
{
	if (!map_shm()) ztest_test_skip();
	return NULL;
}

static int mock_send(const struct device *dev, mbox_channel_id_t ch, const struct mbox_msg *msg)
{
	struct mock_data *d = dev->data;
	d->sends++;
	d->last_null = (msg == NULL);
	d->last_size = msg ? msg->size : 0;
	return 0;
}

static int
mock_register(const struct device *dev, mbox_channel_id_t ch, mbox_callback_t cb, void *ctx)
{
	struct mock_data *d = dev->data;
	d->cb               = cb;
	d->cb_ctx           = ctx;
	return 0;
}

static int mock_mtu(const struct device *dev)
{
	return ((const int *)dev->config)[0];
}

static uint32_t mock_max_ch(const struct device *dev)
{
	return 32;
}

static int mock_set_enabled(const struct device *dev, mbox_channel_id_t ch, bool en)
{
	((struct mock_data *)dev->data)->enabled = en;
	return 0;
}

static const struct mbox_driver_api mock_api = {
	.send              = mock_send,
	.register_callback = mock_register,
	.mtu_get           = mock_mtu,
	.max_channels_get  = mock_max_ch,
	.set_enabled       = mock_set_enabled,
};

#define MOCK_INST(n) \
	static struct mock_data mock_data_##n; \
	static const int        mock_cfg_##n = DT_INST_PROP(n, mtu); \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &mock_data_##n, &mock_cfg_##n, POST_KERNEL, 0, &mock_api);
DT_INST_FOREACH_STATUS_OKAY(MOCK_INST)

#define MOCK(label) ((struct mock_data *)DEVICE_DT_GET(DT_NODELABEL(label))->data)

static int     g_rx_count;
static size_t  g_rx_len;
static uint8_t g_rx_bytes[64];

static void on_msg(uint32_t channel, const void *data, size_t len, void *user)
{
	g_rx_count++;
	g_rx_len = len;
	memcpy(g_rx_bytes, data, len < sizeof(g_rx_bytes) ? len : sizeof(g_rx_bytes));
}

ZTEST_SUITE(alp_mproc_doorbell, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(alp_mproc_doorbell, test_doorbell_only_carries_payload_in_shmem_slot)
{
	alp_mbox_config_t cfg = { .channel = 0, .peer = ALP_CORE_M55_HE };
	alp_mbox_t       *mb  = alp_mbox_open(&cfg);
	zassert_not_null(mb, "mtu 0 + alp-shmem0 present must open");

	const uint8_t msg[8]  = { 1, 2, 3, 4, 5, 6, 7, 8 };
	MOCK(mock_tx0)->sends = 0;
	zassert_equal(alp_mbox_send(mb, msg, sizeof(msg), 10), ALP_OK);
	zassert_equal(MOCK(mock_tx0)->sends, 1);
	zassert_true(MOCK(mock_tx0)->last_null, "doorbell-only send must pass a NULL message");
	/* slot = last 64 bytes of the 4 KiB region: u32 len, then data */
	zassert_equal(*(volatile uint32_t *)&g_shm[SHM_SIZE - 64], 8u);
	zassert_mem_equal(&g_shm[SHM_SIZE - 60], msg, sizeof(msg));

	uint8_t big[61] = { 0 };
	zassert_equal(alp_mbox_send(mb, big, sizeof(big), 10), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(MOCK(mock_tx0)->sends, 1, "oversize must not ring the doorbell");

	/* RX: callback goes on the rx device; a NULL-message ring reads the slot. */
	g_rx_count = 0;
	zassert_equal(alp_mbox_set_callback(mb, on_msg, NULL), ALP_OK);
	zassert_true(MOCK(mock_rx0)->enabled, "rx alias must be the enabled device");
	zassert_false(MOCK(mock_tx0)->enabled, "tx device must not be touched for RX");
	MOCK(mock_rx0)->cb(DEVICE_DT_GET(DT_NODELABEL(mock_rx0)), 0, MOCK(mock_rx0)->cb_ctx, NULL);
	zassert_equal(g_rx_count, 1);
	zassert_equal(g_rx_len, sizeof(msg));
	zassert_mem_equal(g_rx_bytes, msg, sizeof(msg));

	alp_mbox_close(mb);
}

ZTEST(alp_mproc_doorbell, test_payload_mbox_refuses_over_mtu_and_rx_falls_back)
{
	alp_mbox_config_t cfg = { .channel = 1, .peer = ALP_CORE_M55_HE };
	alp_mbox_t       *mb  = alp_mbox_open(&cfg);
	zassert_not_null(mb);

	const uint8_t msg[5] = { 1, 2, 3, 4, 5 };
	zassert_equal(alp_mbox_send(mb, msg, 5, 10), ALP_ERR_OUT_OF_RANGE, "5 > mtu 4");
	zassert_equal(MOCK(mock_both1)->sends, 0);
	zassert_equal(alp_mbox_send(mb, msg, 4, 10), ALP_OK);
	zassert_false(MOCK(mock_both1)->last_null, "mtu > 0 keeps the in-message payload path");
	zassert_equal(MOCK(mock_both1)->last_size, 4u);

	/* no alp-mbox-rx1: callback lands on the same device */
	zassert_equal(alp_mbox_set_callback(mb, on_msg, NULL), ALP_OK);
	zassert_true(MOCK(mock_both1)->enabled);
	alp_mbox_close(mb);
}
