/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — Path A state machine.  See ota.h + ota_layout.h and
 * the design at alp-sdk-internal/docs/gd32-bridge-ota-path-a-design.md.
 *
 * Safe-by-default: without -DBRIDGE_OTA_PARTITIONED the dispatcher returns
 * STATUS_NOSUPPORT and touches no flash.  Even when partitioned, the real
 * flash ops come from hal/fmc_ota.c (gd32 backend); on the stub backend the
 * weak no-ops below report "unsupported".
 *
 * HIL-REQUIRED: the destructive flash path + the bootloader/slot-relocation
 * it pairs with are NOT validated on silicon.  Do not flash to a field unit
 * (no host-driven SWD reflash this HW rev) until bench-verified.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ota.h"
#include "ota_layout.h"
#include "crc32.h"
#include "fmc_ota.h"
#include "bootloader/bootloader.h"   /* CMD_OTA_* */

/* ---- Weak flash seam (overridden by hal/fmc_ota.c on the gd32 backend) - */
__attribute__((weak)) bool ota_fmc_supported(void) { return false; }
__attribute__((weak)) bool ota_fmc_erase_range(uint32_t base, uint32_t len)
{ (void)base; (void)len; return false; }
__attribute__((weak)) bool ota_fmc_program(uint32_t addr, const uint8_t *data, size_t len)
{ (void)addr; (void)data; (void)len; return false; }
__attribute__((weak)) void ota_system_reset(void) { }

#if defined(BRIDGE_OTA_PARTITIONED)

/* OTA_GET_STATE states (wire-visible). */
enum { OTA_ST_IDLE = 0u, OTA_ST_READY = 1u, OTA_ST_BUSY = 2u,
       OTA_ST_VERIFIED = 3u, OTA_ST_ERROR = 4u };

static uint8_t  s_state    = OTA_ST_IDLE;
static uint8_t  s_inactive = OTA_SLOT_B;
static uint32_t s_img_len;
static uint32_t s_last_off;
static uint32_t s_expected_crc;   /* from OTA_BEGIN (host supplies CRC up front) */
static uint32_t s_img_crc;        /* computed at OTA_VERIFY, reused at COMMIT */
static uint8_t  s_err;

static uint32_t rd_u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void wr_u32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- A/B metadata --------------------------------------------------- */
static bool meta_read(uint32_t addr, ota_meta_record_t *r)
{
    const ota_meta_record_t *p = (const ota_meta_record_t *)addr;
    if (p->magic != OTA_META_MAGIC || p->struct_version != OTA_META_STRUCT_VER) {
        return false;
    }
    const uint32_t c = ota_crc32(0u, (const uint8_t *)p,
                                 offsetof(ota_meta_record_t, rec_crc32));
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
    const bool va = meta_read(OTA_META_REC0, &a);
    const bool vb = meta_read(OTA_META_REC1, &b);
    if (va && vb) {
        if (a.counter >= b.counter) { *out = a; *which_addr = OTA_META_REC0; }
        else                        { *out = b; *which_addr = OTA_META_REC1; }
        return true;
    }
    if (va) { *out = a; *which_addr = OTA_META_REC0; return true; }
    if (vb) { *out = b; *which_addr = OTA_META_REC1; return true; }
    return false;
}

/* Write a fresh record to the *other* meta page (alternating), so a
 * power-fail mid-write leaves the previous record intact. */
static bool meta_commit(uint8_t active_slot, uint32_t fw_ver,
                        uint32_t img_len, uint32_t img_crc)
{
    ota_meta_record_t cur;
    uint32_t which = 0u, counter = 0u;
    uint8_t  valid = 0u;
    uint32_t target = OTA_META_REC0;
    if (meta_current(&cur, &which)) {
        counter = cur.counter;
        valid   = cur.slot_valid;
        target  = (which == OTA_META_REC0) ? OTA_META_REC1 : OTA_META_REC0;
    }
    if (!ota_fmc_erase_range(target, OTA_PAGE_SIZE)) {
        return false;
    }
    ota_meta_record_t rec;
    memset(&rec, 0, sizeof rec);
    rec.magic          = OTA_META_MAGIC;
    rec.struct_version = OTA_META_STRUCT_VER;
    rec.counter        = counter + 1u;
    rec.active_slot    = active_slot;
    rec.slot_valid     = (uint8_t)(valid | (uint8_t)(1u << active_slot));
    rec.fw_version     = fw_ver;
    rec.img_len        = img_len;
    rec.img_crc32      = img_crc;
    rec.rec_crc32      = ota_crc32(0u, (const uint8_t *)&rec,
                                   offsetof(ota_meta_record_t, rec_crc32));
    return ota_fmc_program(target, (const uint8_t *)&rec, sizeof rec);
}

static uint8_t active_slot_now(void)
{
    ota_meta_record_t cur;
    uint32_t which;
    return meta_current(&cur, &which) ? cur.active_slot : OTA_SLOT_A;
}

/* ---- opcode handlers ------------------------------------------------ */
static gd32_bridge_status_t h_begin(const uint8_t *req, size_t len,
                                    uint8_t *reply, size_t cap, size_t *rlen)
{
    /* Host OTA_BEGIN req: size:u32, expected_crc32:u32. */
    if (len < 8u) { return STATUS_INVAL; }
    s_img_len      = rd_u32(&req[0]);
    s_expected_crc = rd_u32(&req[4]);
    if (s_img_len == 0u || s_img_len > OTA_SLOT_SIZE) {
        s_state = OTA_ST_ERROR; s_err = 1u; return STATUS_OUT_OF_RANGE;
    }
    s_inactive = (active_slot_now() == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
    s_state    = OTA_ST_BUSY;
    if (!ota_fmc_erase_range(ota_slot_base(s_inactive), OTA_SLOT_SIZE)) {
        s_state = OTA_ST_ERROR; s_err = 2u; return STATUS_IO;
    }
    s_last_off = 0u;
    s_state    = OTA_ST_READY;
    /* Host OTA_BEGIN reply: chunk_max:u16 (LE), target_slot:u8. */
    if (cap >= 3u) {
        const uint16_t chunk_max = (uint16_t)(GD32_BRIDGE_MAX_PAYLOAD_BYTES - 4u);
        reply[0] = (uint8_t)(chunk_max & 0xFFu);
        reply[1] = (uint8_t)(chunk_max >> 8);
        reply[2] = s_inactive;
        *rlen = 3u;
    }
    return STATUS_OK;
}

static gd32_bridge_status_t h_write(const uint8_t *req, size_t len,
                                    uint8_t *reply, size_t cap, size_t *rlen)
{
    /* Host OTA_WRITE_CHUNK req: offset:u32, data[].  Reply: received_bytes:u32. */
    if (len < 4u) { return STATUS_INVAL; }
    const uint32_t off  = rd_u32(&req[0]);
    const size_t   dlen = len - 4u;
    if ((uint32_t)off + dlen > OTA_SLOT_SIZE) {
        s_state = OTA_ST_ERROR; s_err = 3u; return STATUS_OUT_OF_RANGE;
    }
    s_state = OTA_ST_BUSY;
    if (!ota_fmc_program(ota_slot_base(s_inactive) + off, &req[4], dlen)) {
        s_state = OTA_ST_ERROR; s_err = 4u; return STATUS_IO;
    }
    if (off + (uint32_t)dlen > s_last_off) {
        s_last_off = off + (uint32_t)dlen;   /* cumulative high-water = received bytes */
    }
    s_state = OTA_ST_READY;
    if (cap >= 4u) { wr_u32(&reply[0], s_last_off); *rlen = 4u; }
    return STATUS_OK;
}

static gd32_bridge_status_t h_verify(uint8_t *reply, size_t cap, size_t *rlen)
{
    /* Host OTA_VERIFY req: empty (CRC was supplied at BEGIN).
     * Reply: computed_crc32:u32, verified:u8. */
    s_img_crc = ota_crc32(0u, (const uint8_t *)ota_slot_base(s_inactive), s_img_len);
    const bool ok = (s_img_crc == s_expected_crc);
    s_state = ok ? OTA_ST_VERIFIED : OTA_ST_ERROR;
    if (!ok) { s_err = 5u; }
    if (cap >= 5u) {
        wr_u32(&reply[0], s_img_crc);
        reply[4] = ok ? 1u : 0u;
        *rlen = 5u;
    }
    return STATUS_OK;
}

static gd32_bridge_status_t h_commit(void)
{
    if (s_state != OTA_ST_VERIFIED) { return STATUS_NOT_READY; }
    if (!meta_commit(s_inactive, 0u /* fw_ver: host BEGIN carries no version */,
                     s_img_len, s_img_crc)) {
        s_state = OTA_ST_ERROR; s_err = 6u; return STATUS_IO;
    }
    ota_system_reset();           /* no return on real silicon */
    return STATUS_OK;
}

static gd32_bridge_status_t h_rollback(void)
{
    /* Host OTA_ROLLBACK: no payload either direction (status only). */
    ota_meta_record_t cur;
    uint32_t which;
    if (!meta_current(&cur, &which)) { return STATUS_INVAL; }
    const uint8_t other = (cur.active_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
    if ((cur.slot_valid & (uint8_t)(1u << other)) == 0u) {
        return STATUS_INVAL;                 /* no valid fallback slot */
    }
    /* Flip active to `other`; its slot image is untouched.  The bootloader
     * re-validates the target slot from its own image header, so the
     * metadata img_len/crc carried here are not load-bearing for the flip. */
    if (!meta_commit(other, 0u, 0u, 0u)) {
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
    uint32_t which;
    uint8_t  active = OTA_SLOT_A;
    uint16_t gen    = 0u;
    if (meta_current(&cur, &which)) { active = cur.active_slot; gen = (uint16_t)cur.counter; }
    const bool in_progress = (s_state == OTA_ST_READY || s_state == OTA_ST_BUSY ||
                              s_state == OTA_ST_VERIFIED);
    if (cap >= 5u) {
        reply[0] = s_state;
        reply[1] = active;
        reply[2] = in_progress ? s_inactive : 0xFFu;   /* 0xFF = none pending */
        reply[3] = (uint8_t)(gen & 0xFFu);
        reply[4] = (uint8_t)(gen >> 8);
        *rlen = 5u;
    }
    return STATUS_OK;
}

gd32_bridge_status_t ota_dispatch(uint8_t cmd,
                                  const uint8_t *req_payload,
                                  size_t req_payload_len,
                                  uint8_t *reply_payload,
                                  size_t reply_payload_cap,
                                  size_t *reply_payload_len)
{
    *reply_payload_len = 0u;
    if (!ota_fmc_supported()) {
        return STATUS_NOSUPPORT;
    }
    switch (cmd) {
    case CMD_OTA_BEGIN:
        return h_begin(req_payload, req_payload_len,
                       reply_payload, reply_payload_cap, reply_payload_len);
    case CMD_OTA_WRITE_CHUNK:
        return h_write(req_payload, req_payload_len,
                       reply_payload, reply_payload_cap, reply_payload_len);
    case CMD_OTA_VERIFY:
        return h_verify(reply_payload, reply_payload_cap, reply_payload_len);
    case CMD_OTA_COMMIT:
        return h_commit();
    case CMD_OTA_ROLLBACK:
        return h_rollback();
    case CMD_OTA_GET_STATE:
        return h_get_state(reply_payload, reply_payload_cap, reply_payload_len);
    case CMD_OTA_ABORT:
        s_state = OTA_ST_IDLE; s_err = 0u;
        return STATUS_OK;
    default:
        return STATUS_NOSUPPORT;
    }
}

#else  /* !BRIDGE_OTA_PARTITIONED — OTA inert (safe default) */

gd32_bridge_status_t ota_dispatch(uint8_t cmd,
                                  const uint8_t *req_payload,
                                  size_t req_payload_len,
                                  uint8_t *reply_payload,
                                  size_t reply_payload_cap,
                                  size_t *reply_payload_len)
{
    (void)cmd; (void)req_payload; (void)req_payload_len;
    (void)reply_payload; (void)reply_payload_cap;
    *reply_payload_len = 0u;
    return STATUS_NOSUPPORT;
}

#endif /* BRIDGE_OTA_PARTITIONED */
