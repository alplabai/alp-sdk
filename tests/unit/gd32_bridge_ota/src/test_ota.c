/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the gd32-bridge OTA Path-A handler (ota.c, built with
 * -DBRIDGE_OTA_PARTITIONED).  Focus: the OTA_WRITE_CHUNK offset/length
 * bounds check must reject wrapped ranges (offset near UINT32_MAX) before
 * computing a flash address or calling ota_fmc_program.
 *
 * The firmware addresses flash as raw 32-bit constants cast to pointers,
 * which is not host-safe on native_sim.  The weak ota_fmc_* seams are
 * therefore overridden here to redirect every flash read/program/erase
 * into a host-side buffer, so the REAL state machine runs unmodified.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "ota.h"
#include "ota_layout.h"
#include "fmc_ota.h"
#include "crc32.h"
#include "bootloader/bootloader.h" /* CMD_OTA_* */

/* ---- Host flash model: mirror the whole device flash into a buffer ---- */

#define FL_BASE OTA_BOOTLOADER_BASE
#define FL_SIZE (OTA_FLASH_END - OTA_BOOTLOADER_BASE)

static uint8_t  g_flash[FL_SIZE];
static uint32_t g_program_calls;

static uint8_t *_host_ptr(uint32_t addr)
{
	zassert_true(addr >= FL_BASE && addr < FL_BASE + FL_SIZE,
	             "flash addr 0x%x out of model",
	             (unsigned)addr);
	return &g_flash[addr - FL_BASE];
}

/* ---- weak seam overrides -------------------------------------------- */

bool ota_fmc_supported(void)
{
	return true;
}

bool ota_fmc_erase_range(uint32_t base, uint32_t len)
{
	memset(_host_ptr(base), 0xFF, len);
	return true;
}

bool ota_fmc_program(uint32_t addr, const uint8_t *data, size_t len)
{
	memcpy(_host_ptr(addr), data, len);
	g_program_calls++;
	return true;
}

const void *ota_fmc_flash_ptr(uint32_t addr)
{
	return _host_ptr(addr);
}

void ota_system_reset(void)
{
}

/* ---- helpers -------------------------------------------------------- */

static void wr_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Open a fresh OTA session; leaves the state machine READY. */
static void begin_session(uint32_t img_len)
{
	uint8_t req[8];
	wr_u32(&req[0], img_len);
	wr_u32(&req[4], 0u); /* expected_crc (unused until VERIFY) */
	uint8_t reply[8];
	size_t  rlen = 0u;
	zassert_equal(ota_dispatch(CMD_OTA_BEGIN, req, sizeof(req), reply, sizeof(reply), &rlen),
	              STATUS_OK);
}

static gd32_bridge_status_t
write_chunk(uint32_t off, const uint8_t *data, uint8_t dlen, uint8_t *reply, size_t *rlen)
{
	uint8_t req[5 + 255];
	wr_u32(&req[0], off);
	req[4] = dlen;
	if (dlen > 0u) {
		memcpy(&req[5], data, dlen);
	}
	return ota_dispatch(CMD_OTA_WRITE_CHUNK, req, (size_t)(5u + dlen), reply, 8u, rlen);
}

static void reset_model(void)
{
	memset(g_flash, 0, sizeof(g_flash)); /* zeroed meta -> no valid record */
	g_program_calls = 0u;
}

ZTEST_SUITE(gd32_bridge_ota, NULL, NULL, NULL, NULL, NULL);

ZTEST(gd32_bridge_ota, test_begin_then_normal_chunk_programs)
{
	reset_model();
	begin_session(64u);
	g_program_calls = 0u;

	const uint8_t        data[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t              reply[8];
	size_t               rlen = 0u;
	gd32_bridge_status_t st   = write_chunk(0u, data, sizeof(data), reply, &rlen);
	zassert_equal(st, STATUS_OK, "normal chunk should program, got %d", st);
	zassert_equal(g_program_calls, 1u, "ota_fmc_program must be called once");
	zassert_equal(rlen, 4u);
	zassert_equal(rd_u32(reply), 4u, "high-water = received bytes");
}

ZTEST(gd32_bridge_ota, test_wrapped_offset_rejected_without_program)
{
	reset_model();
	begin_session(64u);
	g_program_calls = 0u;

	const uint8_t data[4] = { 0x5A, 0x5B, 0x5C, 0x5D };
	uint8_t       reply[8];
	size_t        rlen = 0u;
	/* off = UINT32_MAX - 1.  On a 32-bit target the old `off + dlen >
     * OTA_SLOT_SIZE` check wraps and wrongly accepts this; the
     * subtraction-style check rejects any off past the slot outright.  This
     * asserts the OUT_OF_RANGE contract and that no flash program happens. */
	gd32_bridge_status_t st = write_chunk(0xFFFFFFFEu, data, sizeof(data), reply, &rlen);
	zassert_equal(st, STATUS_OUT_OF_RANGE, "wrapped offset must be rejected, got %d", st);
	zassert_equal(g_program_calls, 0u, "ota_fmc_program must NOT be called on OOR");
}

ZTEST(gd32_bridge_ota, test_offset_past_slot_rejected_without_program)
{
	reset_model();
	begin_session(OTA_SLOT_SIZE);
	g_program_calls = 0u;

	const uint8_t data[4] = { 1, 2, 3, 4 };
	uint8_t       reply[8];
	size_t        rlen = 0u;
	/* Non-wrapping but out of range: off within slot, off + dlen past end. */
	gd32_bridge_status_t st = write_chunk(OTA_SLOT_SIZE - 2u, data, sizeof(data), reply, &rlen);
	zassert_equal(st, STATUS_OUT_OF_RANGE, "off+len past slot must be rejected, got %d", st);
	zassert_equal(g_program_calls, 0u);
}

ZTEST(gd32_bridge_ota, test_exact_end_chunk_accepted)
{
	reset_model();
	begin_session(OTA_SLOT_SIZE);
	g_program_calls = 0u;

	const uint8_t data[4] = { 1, 2, 3, 4 };
	uint8_t       reply[8];
	size_t        rlen = 0u;
	/* off + dlen == OTA_SLOT_SIZE is the last legal byte range. */
	gd32_bridge_status_t st = write_chunk(OTA_SLOT_SIZE - 4u, data, sizeof(data), reply, &rlen);
	zassert_equal(st, STATUS_OK, "exact-end chunk must be accepted, got %d", st);
	zassert_equal(g_program_calls, 1u);
}

ZTEST(gd32_bridge_ota, test_replay_is_idempotent)
{
	reset_model();
	begin_session(64u);

	const uint8_t data[4] = { 0x11, 0x22, 0x33, 0x44 };
	uint8_t       reply[8];
	size_t        rlen = 0u;

	g_program_calls = 0u;
	zassert_equal(write_chunk(0u, data, sizeof(data), reply, &rlen), STATUS_OK);
	zassert_equal(g_program_calls, 1u);

	/* Re-issue the identical chunk: below the high-water mark and byte
     * identical -> acked without re-programming (ECC re-write hard-faults). */
	g_program_calls = 0u;
	zassert_equal(write_chunk(0u, data, sizeof(data), reply, &rlen), STATUS_OK);
	zassert_equal(g_program_calls, 0u, "replayed chunk must not re-program");
}

/* #741: ota_slot_base_checked validates the slot instead of silently
 * mapping every non-B value to slot A. */
ZTEST(gd32_bridge_ota, test_slot_base_checked_validates_slot)
{
	uint32_t base = 0xDEADBEEFu;

	zassert_true(ota_slot_base_checked(OTA_SLOT_A, &base));
	zassert_equal(base, OTA_SLOT_A_BASE);
	zassert_true(ota_slot_base_checked(OTA_SLOT_B, &base));
	zassert_equal(base, OTA_SLOT_B_BASE);

	/* Representative invalid slots must be REJECTED, not resolved to a
	 * valid flash address -- the #741 defect. */
	base = 0xDEADBEEFu;
	zassert_false(ota_slot_base_checked(2u, &base), "slot 2 must reject, not map to A");
	zassert_false(ota_slot_base_checked(0xFFu, &base));
	zassert_equal(base, 0xDEADBEEFu, "base must be untouched on reject");
}

/* #755: ota_image_bootable rejects CRC-valid-but-unbootable images. */
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

ZTEST(gd32_bridge_ota, test_image_bootable_validates_vector_head)
{
	const uint32_t base    = OTA_SLOT_A_BASE;
	const uint32_t len     = 0x200u;
	uint8_t        img[16] = { 0 };

	/* Valid: MSP in SRAM + word-aligned, reset inside image + Thumb. */
	put32(&img[0], 0x20010000u);          /* MSP  */
	put32(&img[4], (base + 0x100u) | 1u); /* reset, Thumb */
	zassert_true(ota_image_bootable(base, img, len), "valid vector head must pass");

	/* One-byte / truncated image (below the MSP+reset head). */
	zassert_false(ota_image_bootable(base, img, 1u), "one-byte image must reject (#755)");
	zassert_false(ota_image_bootable(base, img, 7u), "truncated vector head must reject");

	/* MSP not word-aligned / outside SRAM. */
	put32(&img[0], 0x20010001u);
	zassert_false(ota_image_bootable(base, img, len), "unaligned MSP must reject");
	put32(&img[0], 0x08000000u);
	zassert_false(ota_image_bootable(base, img, len), "MSP outside SRAM must reject");
	put32(&img[0], 0x20010000u); /* restore */

	/* Reset vector without the Thumb bit, or outside the image. */
	put32(&img[4], base + 0x100u); /* even -> no Thumb */
	zassert_false(ota_image_bootable(base, img, len), "reset without Thumb bit must reject");
	put32(&img[4], (base + len) | 1u); /* past end */
	zassert_false(ota_image_bootable(base, img, len), "reset past image end must reject");
	put32(&img[4], (base - 4u) | 1u); /* before base */
	zassert_false(ota_image_bootable(base, img, len), "reset before image base must reject");
}
