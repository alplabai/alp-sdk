/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA bootloader (Path A). Runs from the first 32 KB of flash
 * (0x08000000, never erased by OTA). Picks the active slot from the A/B
 * metadata, validates its image (CRC over the metadata-recorded length),
 * and jumps to it (MSP + VTOR + reset vector). If no slot validates it
 * idles so an external SWD probe can recover the part.
 *
 * Pairs with src/ota.c (the application-side OTA state machine that writes
 * the inactive slot + commits the metadata).
 *
 * HIL-REQUIRED: the boot/validate/jump path is NOT validated on silicon.
 * A bug here bricks the GD32 (no host-driven SWD reflash this HW rev —
 * recover via a bench SWD probe). Bring up incrementally.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gd32g5x3.h"

#include "ota_layout.h"
#include "crc32.h"

static bool meta_read(uint32_t addr, ota_meta_record_t *r)
{
	const ota_meta_record_t *p = (const ota_meta_record_t *)addr;
	if (p->magic != OTA_META_MAGIC || p->struct_version != OTA_META_STRUCT_VER) {
		return false;
	}
	if (ota_crc32(0u, (const uint8_t *)p, offsetof(ota_meta_record_t, rec_crc32)) != p->rec_crc32) {
		return false;
	}
	*r = *p;
	return true;
}

/* Order the two CRC-valid metadata records newest-first into cands[]
 * and return how many are present (0..2).  The bootloader then tries
 * each in order (#754): a newer record that fails SEMANTIC validation
 * must not suppress an older bootable one, or the part drops into
 * permanent recovery with a perfectly good slot sitting unused. */
static int
meta_candidates(ota_meta_record_t *a, ota_meta_record_t *b, const ota_meta_record_t *cands[2])
{
	const bool va = meta_read(OTA_META_REC0, a);
	const bool vb = meta_read(OTA_META_REC1, b);
	int        n  = 0;
	if (va && vb) {
		if (a->counter >= b->counter) {
			cands[n++] = a;
			cands[n++] = b;
		} else {
			cands[n++] = b;
			cands[n++] = a;
		}
	} else if (va) {
		cands[n++] = a;
	} else if (vb) {
		cands[n++] = b;
	}
	return n;
}

static bool active_slot_valid(const ota_meta_record_t *m)
{
	const uint8_t slot = m->active_slot;
	uint32_t      base;
	/* A corrupt active_slot must NOT resolve to a valid flash base
	 * (#741): reject it here, before the slot indexes the [2] arrays. */
	if (!ota_slot_base_checked(slot, &base)) {
		return false;
	}
	const uint32_t len = m->img_len[slot];
	if ((m->slot_valid & (uint8_t)(1u << slot)) == 0u) {
		return false;
	}
	if (len == 0u || len > OTA_SLOT_SIZE) {
		return false;
	}
	if (ota_crc32(0u, (const uint8_t *)base, len) != m->img_crc32[slot]) {
		return false;
	}
	/* CRC integrity is necessary but not sufficient: a CRC-valid but
	 * truncated / vector-less image would boot into garbage MSP/reset
	 * words (#755).  Require a bootable vector head before jumping.
	 * Flash is memory-mapped, so the base doubles as the read pointer. */
	return ota_image_bootable(base, (const uint8_t *)base, len);
}

static void jump_to_slot(uint32_t slot_base)
{
	const uint32_t msp   = *(volatile uint32_t *)slot_base;
	const uint32_t reset = *(volatile uint32_t *)(slot_base + 4u);
	__disable_irq();
	SCB->VTOR = slot_base; /* relocate the vector table to the slot */
	__DSB();
	__set_MSP(msp);
	__ISB();
	((void (*)(void))reset)(); /* no return */
}

int main(void)
{
	ota_meta_record_t        a, b;
	const ota_meta_record_t *cands[2];
	const int                n = meta_candidates(&a, &b, cands);
	/* Newest-first with fallback (#754): the first record whose active
	 * slot passes full semantic + image validation wins. */
	for (int i = 0; i < n; ++i) {
		uint32_t base;
		if (active_slot_valid(cands[i]) && ota_slot_base_checked(cands[i]->active_slot, &base)) {
			jump_to_slot(base);
		}
	}
	/* No valid image: recovery. A later build exposes the OTA opcodes here
     * to accept a reflash over the bridge; today, idle so a bench SWD probe
     * can take over. */
	for (;;) {
		__WFI();
	}
	/* unreachable */
	return 0;
}
