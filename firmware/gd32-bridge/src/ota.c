/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — Path A state machine.  See ota.h + ota_layout.h for
 * the public wire and flash-layout contract.
 *
 * Safe-by-default: without -DBRIDGE_OTA_PARTITIONED the dispatcher returns
 * STATUS_NOSUPPORT and touches no flash.  Even when partitioned, the real
 * flash ops come from hal/fmc_ota.c (gd32 backend); on the stub backend the
 * weak no-ops below report "unsupported".
 *
 * SILICON-VALIDATED 2026-06-04 (bench, protocol v0.6): full cycle
 * BEGIN -> length-checked chunk stream at 25 MHz -> VERIFY -> COMMIT ->
 * boot into the new slot -> ROLLBACK -> boot back, proven by wire
 * build-id reads + the A/B metadata generation history.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ota.h"
#include "ota_layout.h"
#include "crc32.h"
#include "fmc_ota.h"
#include "bootloader/bootloader.h" /* CMD_OTA_* */

/* ---- Weak flash seam (overridden by hal/fmc_ota.c on the gd32 backend) - */
__attribute__((weak)) bool ota_fmc_supported(void)
{
	return false;
}
__attribute__((weak)) bool ota_fmc_erase_range(uint32_t base, uint32_t len)
{
	(void)base;
	(void)len;
	return false;
}
__attribute__((weak)) bool ota_fmc_program(uint32_t addr, const uint8_t *data, size_t len)
{
	(void)addr;
	(void)data;
	(void)len;
	return false;
}
__attribute__((weak)) void ota_system_reset(void)
{
}
__attribute__((weak)) const void *ota_fmc_flash_ptr(uint32_t addr)
{
	return (const void *)(uintptr_t)addr;
}

#if defined(BRIDGE_OTA_PARTITIONED)

/* OTA_GET_STATE states (wire-visible). */
enum {
	OTA_ST_IDLE     = 0u,
	OTA_ST_READY    = 1u,
	OTA_ST_BUSY     = 2u,
	OTA_ST_VERIFIED = 3u,
	OTA_ST_ERROR    = 4u
};

static uint8_t  s_state    = OTA_ST_IDLE;
static uint8_t  s_inactive = OTA_SLOT_B;
static uint32_t s_img_len;
static uint32_t s_last_off;
static uint32_t s_expected_crc; /* from OTA_BEGIN (host supplies CRC up front) */
static uint32_t s_img_crc;      /* computed at OTA_VERIFY, reused at COMMIT */
static uint32_t s_fw_version;   /* from OTA_BEGIN v0.7 form (packed
                                 * major<<16|minor<<8|patch); 0 = host
                                 * sent the legacy 8-byte form = unknown */
static uint8_t  s_err;

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- A/B metadata --------------------------------------------------- */
static bool meta_read(uint32_t addr, ota_meta_record_t *r)
{
	const ota_meta_record_t *p = (const ota_meta_record_t *)ota_fmc_flash_ptr(addr);
	if (p->magic != OTA_META_MAGIC || p->struct_version != OTA_META_STRUCT_VER) {
		return false;
	}
	const uint32_t c = ota_crc32(0u, (const uint8_t *)p, offsetof(ota_meta_record_t, rec_crc32));
	if (c != p->rec_crc32) {
		return false;
	}
	*r = *p;
	return true;
}

/* Returns true + the winning record (highest valid counter) and which page
 * holds it; false if neither record is valid (factory / corrupt). */
static bool meta_current(ota_meta_record_t *out, uint32_t *which_addr)
{
	ota_meta_record_t a, b;
	const bool        va = meta_read(OTA_META_REC0, &a);
	const bool        vb = meta_read(OTA_META_REC1, &b);
	if (va && vb) {
		if (a.counter >= b.counter) {
			*out        = a;
			*which_addr = OTA_META_REC0;
		} else {
			*out        = b;
			*which_addr = OTA_META_REC1;
		}
		return true;
	}
	if (va) {
		*out        = a;
		*which_addr = OTA_META_REC0;
		return true;
	}
	if (vb) {
		*out        = b;
		*which_addr = OTA_META_REC1;
		return true;
	}
	return false;
}

/* Write a fresh record to the *other* meta page (alternating), so a
 * power-fail mid-write leaves the previous record intact.  The per-slot
 * image descriptors carry forward from the current record; only the new
 * active slot's entry is rewritten (and only when `update_entry` -- a
 * ROLLBACK flips `active_slot` without touching the descriptors, so the
 * rolled-to slot keeps the len/CRC recorded when it was last written). */
static bool meta_commit(uint8_t  active_slot,
                        bool     update_entry,
                        uint32_t fw_ver,
                        uint32_t img_len,
                        uint32_t img_crc)
{
	ota_meta_record_t rec;
	ota_meta_record_t cur;
	uint32_t          which  = 0u;
	uint32_t          target = OTA_META_REC0;
	memset(&rec, 0, sizeof rec);
	if (meta_current(&cur, &which)) {
		rec    = cur; /* counter + slot_valid + per-slot table */
		target = (which == OTA_META_REC0) ? OTA_META_REC1 : OTA_META_REC0;
	}
	if (!ota_fmc_erase_range(target, OTA_PAGE_SIZE)) {
		return false;
	}
	rec.magic          = OTA_META_MAGIC;
	rec.struct_version = OTA_META_STRUCT_VER;
	rec.counter += 1u;
	rec.active_slot = active_slot;
	rec.slot_valid |= (uint8_t)(1u << active_slot);
	if (update_entry) {
		rec.fw_version[active_slot] = fw_ver;
		rec.img_len[active_slot]    = img_len;
		rec.img_crc32[active_slot]  = img_crc;
	}
	rec.rec_crc32 = ota_crc32(0u, (const uint8_t *)&rec, offsetof(ota_meta_record_t, rec_crc32));
	return ota_fmc_program(target, (const uint8_t *)&rec, sizeof rec);
}

static uint8_t active_slot_now(void)
{
	ota_meta_record_t cur;
	uint32_t          which;
	return meta_current(&cur, &which) ? cur.active_slot : OTA_SLOT_A;
}

/* Flash base of the in-flight (inactive) slot.  s_inactive is set to
 * OTA_SLOT_A/B at BEGIN (the trusted internal boundary), so the checked
 * derivation (#741) never fails here; validate anyway for defence. */
static uint32_t ota_inactive_base(void)
{
	uint32_t base = 0u;
	if (!ota_slot_base_checked(s_inactive, &base)) {
		base = OTA_SLOT_A_BASE; /* unreachable: s_inactive is always A/B */
	}
	return base;
}

/* ---- opcode handlers ------------------------------------------------ */
static gd32_bridge_status_t
h_begin(const uint8_t *req, size_t len, uint8_t *reply, size_t cap, size_t *rlen)
{
	/* Host OTA_BEGIN req: size:u32, expected_crc32:u32
     * [, fw_major:u8, fw_minor:u8, fw_patch:u8  -- v0.7 additive form].
     * The version triple is recorded into the meta record at COMMIT
     * (ota_meta_record_t.fw_version[slot], which reserved the field
     * with "0 = unknown" from day one).  Length-tolerant by design:
     * an 8-byte legacy BEGIN means version-unknown, and older firmware
     * ignores the 3 trailing bytes of the v0.7 form -- additive both
     * directions (wire MINOR). */
	if (len < 8u) {
		return STATUS_INVAL;
	}
	s_img_len      = rd_u32(&req[0]);
	s_expected_crc = rd_u32(&req[4]);
	s_fw_version   = (len >= 11u)
	                     ? (((uint32_t)req[8] << 16) | ((uint32_t)req[9] << 8) | (uint32_t)req[10])
	                     : 0u;
	if (s_img_len == 0u || s_img_len > OTA_SLOT_SIZE) {
		s_state = OTA_ST_ERROR;
		s_err   = 1u;
		return STATUS_OUT_OF_RANGE;
	}
	s_inactive = (active_slot_now() == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
	s_state    = OTA_ST_BUSY;
	if (!ota_fmc_erase_range(ota_inactive_base(), OTA_SLOT_SIZE)) {
		s_state = OTA_ST_ERROR;
		s_err   = 2u;
		return STATUS_IO;
	}
	s_last_off = 0u;
	s_state    = OTA_ST_READY;
	/* Host OTA_BEGIN reply: chunk_max:u16 (LE), target_slot:u8.
     * chunk_max accounts for the offset:u32 + len:u8 header (v0.6). */
	if (cap >= 3u) {
		const uint16_t chunk_max = (uint16_t)(GD32_BRIDGE_MAX_PAYLOAD_BYTES - 5u);
		reply[0]                 = (uint8_t)(chunk_max & 0xFFu);
		reply[1]                 = (uint8_t)(chunk_max >> 8);
		reply[2]                 = s_inactive;
		*rlen                    = 3u;
	}
	return STATUS_OK;
}

static gd32_bridge_status_t
h_write(const uint8_t *req, size_t len, uint8_t *reply, size_t cap, size_t *rlen)
{
	/* Host OTA_WRITE_CHUNK req: offset:u32, len:u8, data[len].
     * Reply: received_bytes:u32.
     *
     * The explicit len byte is LOAD-BEARING (protocol v0.6,
     * silicon-caught 2026-06-04): the slave can capture a frame merged
     * with the following transaction's zero filler (the FMC program
     * window swallows CS edges), and for the ~1-in-256 chunk whose
     * frame CRC is byte-palindromic the zero-extended span still
     * passes the span CRC (CRC-CCITT self-consumption: frame + own
     * CRC + zeros hashes to 0x0000).  Cross-checking the embedded
     * length against the span-derived length rejects any such
     * extension regardless of CRC coincidences -- and without
     * poisoning the session (plain STATUS_INVAL, state untouched). */
	if (s_state != OTA_ST_READY) {
		/* No BEGIN-opened session: the inactive slot is not erased and
         * s_img_len/s_expected_crc are unset -- programming here would
         * corrupt the slot.  Host must (re-)issue OTA_BEGIN. */
		return STATUS_NOT_READY;
	}
	if (len < 5u) {
		return STATUS_INVAL;
	}
	const uint32_t off  = rd_u32(&req[0]);
	const size_t   dlen = req[4];
	if (dlen == 0u || dlen != len - 5u) {
		return STATUS_INVAL; /* extended/truncated capture: drop */
	}
	if (off > OTA_SLOT_SIZE || dlen > OTA_SLOT_SIZE - off) {
		s_state = OTA_ST_ERROR;
		s_err   = 3u;
		return STATUS_OUT_OF_RANGE;
	}
	/* The transport is AT-LEAST-ONCE: the slave can decode a request
     * twice (silicon-caught 2026-06-04: chunk #256 replayed
     * deterministically), and re-programming ECC flash hard-faults.
     * Make replays idempotent: a chunk entirely below the high-water
     * mark is compared against what's in flash -- identical bytes ack
     * without programming, different bytes are real corruption.  A
     * PARTIAL overlap still falls through to the program path (and
     * PGERRs) -- fixed-size streaming never produces one. */
	if (off + (uint32_t)dlen <= s_last_off) {
		const uint8_t *flash = (const uint8_t *)ota_fmc_flash_ptr(ota_inactive_base() + off);
		if (memcmp(flash, &req[5], dlen) == 0) {
			s_state = OTA_ST_READY;
			if (cap >= 4u) {
				wr_u32(&reply[0], s_last_off);
				*rlen = 4u;
			}
			return STATUS_OK;
		}
		s_state = OTA_ST_ERROR;
		s_err   = 4u;
		return STATUS_IO;
	}
	s_state = OTA_ST_BUSY;
	if (!ota_fmc_program(ota_inactive_base() + off, &req[5], dlen)) {
		s_state = OTA_ST_ERROR;
		s_err   = 4u;
		return STATUS_IO;
	}
	if (off + (uint32_t)dlen > s_last_off) {
		s_last_off = off + (uint32_t)dlen; /* cumulative high-water = received bytes */
	}
	s_state = OTA_ST_READY;
	if (cap >= 4u) {
		wr_u32(&reply[0], s_last_off);
		*rlen = 4u;
	}
	return STATUS_OK;
}

static gd32_bridge_status_t h_verify(uint8_t *reply, size_t cap, size_t *rlen)
{
	/* Host OTA_VERIFY req: empty (CRC was supplied at BEGIN).
     * Reply: computed_crc32:u32, verified:u8. */
	if (s_state != OTA_ST_READY) {
		/* Without a BEGIN-opened session s_img_len is 0: CRC over zero
         * bytes would "verify" trivially and a subsequent COMMIT would
         * write img_len=0 metadata that no bootloader accepts -- a
         * protocol-misuse brick.  Refuse instead. */
		return STATUS_NOT_READY;
	}
	s_img_crc = ota_crc32(0u, (const uint8_t *)ota_fmc_flash_ptr(ota_inactive_base()), s_img_len);
	const bool ok = (s_img_crc == s_expected_crc);
	s_state       = ok ? OTA_ST_VERIFIED : OTA_ST_ERROR;
	if (!ok) {
		s_err = 5u;
	}
	if (cap >= 5u) {
		wr_u32(&reply[0], s_img_crc);
		reply[4] = ok ? 1u : 0u;
		*rlen    = 5u;
	}
	return STATUS_OK;
}

static gd32_bridge_status_t h_commit(void)
{
	if (s_state != OTA_ST_VERIFIED) {
		return STATUS_NOT_READY;
	}
	/* A verified (CRC-matching) image can still be unbootable -- a
	 * one-byte or truncated image with a matching host CRC (#755).
	 * Refuse to activate metadata that would brick the part on reboot. */
	if (!ota_image_bootable(ota_inactive_base(),
	                        (const uint8_t *)ota_fmc_flash_ptr(ota_inactive_base()),
	                        s_img_len)) {
		s_state = OTA_ST_ERROR;
		s_err   = 6u;
		return STATUS_INVAL;
	}
	if (!meta_commit(
	        s_inactive, true, s_fw_version /* 0 = legacy BEGIN, unknown */, s_img_len, s_img_crc)) {
		s_state = OTA_ST_ERROR;
		s_err   = 6u;
		return STATUS_IO;
	}
	ota_system_reset(); /* no return on real silicon */
	return STATUS_OK;
}

static gd32_bridge_status_t h_rollback(void)
{
	/* Host OTA_ROLLBACK: no payload either direction (status only). */
	ota_meta_record_t cur;
	uint32_t          which;
	if (!meta_current(&cur, &which)) {
		return STATUS_INVAL;
	}
	const uint8_t other = (cur.active_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
	if ((cur.slot_valid & (uint8_t)(1u << other)) == 0u || cur.img_len[other] == 0u ||
	    cur.img_len[other] > OTA_SLOT_SIZE) {
		return STATUS_INVAL; /* no valid fallback slot */
	}
	/* Flip active to `other` WITHOUT touching the per-slot descriptors
     * (update_entry=false): the bootloader validates the rolled-to slot
     * against the len/CRC recorded when that slot was last committed. */
	if (!meta_commit(other, false, 0u, 0u, 0u)) {
		return STATUS_IO;
	}
	ota_system_reset();
	return STATUS_OK;
}

static gd32_bridge_status_t h_get_state(uint8_t *reply, size_t cap, size_t *rlen)
{
	/* Host OTA_GET_STATE reply: state:u8, active:u8, pending:u8, boot_count:u16 (LE).
     * `boot_count` is mapped to the metadata update counter (generation). */
	ota_meta_record_t cur;
	uint32_t          which;
	uint8_t           active = OTA_SLOT_A;
	uint16_t          gen    = 0u;
	if (meta_current(&cur, &which)) {
		active = cur.active_slot;
		gen    = (uint16_t)cur.counter;
	}
	const bool in_progress =
	    (s_state == OTA_ST_READY || s_state == OTA_ST_BUSY || s_state == OTA_ST_VERIFIED);
	if (cap >= 5u) {
		reply[0] = s_state;
		reply[1] = active;
		reply[2] = in_progress ? s_inactive : 0xFFu; /* 0xFF = none pending */
		reply[3] = (uint8_t)(gen & 0xFFu);
		reply[4] = (uint8_t)(gen >> 8);
		*rlen    = 5u;
	}
	return STATUS_OK;
}

gd32_bridge_status_t ota_dispatch(uint8_t        cmd,
                                  const uint8_t *req_payload,
                                  size_t         req_payload_len,
                                  uint8_t       *reply_payload,
                                  size_t         reply_payload_cap,
                                  size_t        *reply_payload_len)
{
	*reply_payload_len = 0u;
	if (!ota_fmc_supported()) {
		return STATUS_NOSUPPORT;
	}
	switch (cmd) {
	case CMD_OTA_BEGIN:
		return h_begin(
		    req_payload, req_payload_len, reply_payload, reply_payload_cap, reply_payload_len);
	case CMD_OTA_WRITE_CHUNK:
		return h_write(
		    req_payload, req_payload_len, reply_payload, reply_payload_cap, reply_payload_len);
	case CMD_OTA_VERIFY:
		return h_verify(reply_payload, reply_payload_cap, reply_payload_len);
	case CMD_OTA_COMMIT:
		return h_commit();
	case CMD_OTA_ROLLBACK:
		return h_rollback();
	case CMD_OTA_GET_STATE:
		return h_get_state(reply_payload, reply_payload_cap, reply_payload_len);
	case CMD_OTA_ABORT:
		s_state = OTA_ST_IDLE;
		s_err   = 0u;
		return STATUS_OK;
	default:
		return STATUS_NOSUPPORT;
	}
}

#else /* !BRIDGE_OTA_PARTITIONED — OTA inert (safe default) */

gd32_bridge_status_t ota_dispatch(uint8_t        cmd,
                                  const uint8_t *req_payload,
                                  size_t         req_payload_len,
                                  uint8_t       *reply_payload,
                                  size_t         reply_payload_cap,
                                  size_t        *reply_payload_len)
{
	(void)cmd;
	(void)req_payload;
	(void)req_payload_len;
	(void)reply_payload;
	(void)reply_payload_cap;
	*reply_payload_len = 0u;
	return STATUS_NOSUPPORT;
}

#endif /* BRIDGE_OTA_PARTITIONED */
