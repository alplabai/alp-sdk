/*
 * Copyright 2026 ALP Lab AB
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
#include "fmc_ota.h"
#include "bootloader/bootloader.h"   /* CMD_OTA_* */

/* ---- CRC-32 (IEEE, reflected; single-call use, seed 0) ---------------- */
uint32_t ota_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88820u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

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
static uint32_t s_fw_version;
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
    if (len < 8u) { return STATUS_INVAL; }
    s_img_len    = rd_u32(&req[0]);
    s_fw_version = rd_u32(&req[4]);
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
    if (cap >= 2u) {
        reply[0] = s_inactive;
        reply[1] = (uint8_t)(GD32_BRIDGE_MAX_PAYLOAD_BYTES - 4u); /* offset hdr */
        *rlen = 2u;
    }
    return STATUS_OK;
}

static gd32_bridge_status_t h_write(const uint8_t *req, size_t len)
{
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
    s_last_off = off + (uint32_t)dlen;
    s_state    = OTA_ST_READY;
    return STATUS_OK;
}

static gd32_bridge_status_t h_verify(const uint8_t *req, size_t len,
                                     uint8_t *reply, size_t cap, size_t *rlen)
{
    if (len < 4u) { return STATUS_INVAL; }
    const uint32_t want = rd_u32(&req[0]);
    const uint32_t got  = ota_crc32(0u,
                                    (const uint8_t *)ota_slot_base(s_inactive),
                                    s_img_len);
    const bool ok = (got == want);
    s_state = ok ? OTA_ST_VERIFIED : OTA_ST_ERROR;
    if (!ok) { s_err = 5u; }
    if (cap >= 1u) { reply[0] = ok ? 0u : 1u; *rlen = 1u; }
    s_img_len = s_img_len; /* retained for commit's metadata */
    return STATUS_OK;
}

static gd32_bridge_status_t h_commit(void)
{
    if (s_state != OTA_ST_VERIFIED) { return STATUS_NOT_READY; }
    const uint32_t crc = ota_crc32(0u,
                                   (const uint8_t *)ota_slot_base(s_inactive),
                                   s_img_len);
    if (!meta_commit(s_inactive, s_fw_version, s_img_len, crc)) {
        s_state = OTA_ST_ERROR; s_err = 6u; return STATUS_IO;
    }
    ota_system_reset();           /* no return on real silicon */
    return STATUS_OK;
}

static gd32_bridge_status_t h_rollback(uint8_t *reply, size_t cap, size_t *rlen)
{
    ota_meta_record_t cur;
    uint32_t which;
    if (!meta_current(&cur, &which)) { return STATUS_INVAL; }
    const uint8_t other = (cur.active_slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
    if ((cur.slot_valid & (uint8_t)(1u << other)) == 0u) {
        if (cap >= 1u) { reply[0] = 1u; *rlen = 1u; }
        return STATUS_INVAL;                 /* no valid fallback slot */
    }
    if (!meta_commit(other, 0u, 0u, 0u)) {   /* flip active; keep slot data */
        return STATUS_IO;
    }
    if (cap >= 1u) { reply[0] = 0u; *rlen = 1u; }
    ota_system_reset();
    return STATUS_OK;
}

static gd32_bridge_status_t h_get_state(uint8_t *reply, size_t cap, size_t *rlen)
{
    if (cap >= 6u) {
        reply[0] = s_state;
        wr_u32(&reply[1], s_last_off);
        reply[5] = s_err;
        *rlen = 6u;
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
        return h_write(req_payload, req_payload_len);
    case CMD_OTA_VERIFY:
        return h_verify(req_payload, req_payload_len,
                        reply_payload, reply_payload_cap, reply_payload_len);
    case CMD_OTA_COMMIT:
        return h_commit();
    case CMD_OTA_ROLLBACK:
        return h_rollback(reply_payload, reply_payload_cap, reply_payload_len);
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
