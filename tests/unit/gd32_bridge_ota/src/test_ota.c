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

/* ---- #733: on-flash layout / byte-representation guard --------------- */

/* The bootloader byte-copies a flash record and CRCs the raw bytes, so
 * ota_meta_record_t's in-memory image IS the on-flash format.  The
 * _Static_asserts in ota_layout.h pin size + offsets at compile time;
 * this test documents the intended LITTLE-ENDIAN byte representation and
 * proves the host toolchain lays it out the same way. */
ZTEST(gd32_bridge_ota, test_meta_record_layout_bytes)
{
	zassert_equal(sizeof(ota_meta_record_t), 44u, "record must be 44 bytes on flash");

	ota_meta_record_t m;
	memset(&m, 0, sizeof(m));
	m.magic          = 0x11223344u;
	m.struct_version = OTA_META_STRUCT_VER;
	m.counter        = 0xA1B2C3D4u;
	m.active_slot    = OTA_SLOT_B;
	m.slot_valid     = 0x03u;
	m.fw_version[0]  = 0x00010203u;
	m.img_len[1]     = 0x0000B000u;
	m.img_crc32[0]   = 0xDEADBEEFu;
	m.rec_crc32      = 0xFEEDFACEu;

	const uint8_t *b = (const uint8_t *)&m;
	zassert_equal(rd_u32(&b[0]), 0x11223344u, "magic @0");
	zassert_equal(rd_u32(&b[4]), OTA_META_STRUCT_VER, "struct_version @4");
	zassert_equal(rd_u32(&b[8]), 0xA1B2C3D4u, "counter @8");
	zassert_equal(b[12], (uint8_t)OTA_SLOT_B, "active_slot @12");
	zassert_equal(b[13], 0x03u, "slot_valid @13");
	zassert_equal(rd_u32(&b[16]), 0x00010203u, "fw_version[0] @16");
	zassert_equal(rd_u32(&b[28]), 0x0000B000u, "img_len[1] @28");
	zassert_equal(rd_u32(&b[32]), 0xDEADBEEFu, "img_crc32[0] @32");
	zassert_equal(rd_u32(&b[40]), 0xFEEDFACEu, "rec_crc32 @40");

	/* The CRC span the bootloader/app compute is everything up to
	 * rec_crc32; the offset is the documented span length. */
	zassert_equal(offsetof(ota_meta_record_t, rec_crc32), 40u, "CRC span = 40 bytes");
}
