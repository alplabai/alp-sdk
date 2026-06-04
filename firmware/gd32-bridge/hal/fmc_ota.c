/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — flash backend (gd32 HAL backend). Real FMC erase /
 * doubleword-program for the inactive slot + metadata, and the system
 * reset used at commit.  Overrides the weak no-ops in src/ota.c.
 *
 * HIL-REQUIRED: in the partitioned build the erase/program inner loop must
 * execute from RAM — on single-bank flash the FMC stalls the bus, so a
 * routine fetched from the bank being erased faults.  The OTA_RAMFUNC
 * attribute places these in `.ramfunc` (which the partitioned app linker
 * folds into .data); the default full-flash build leaves the attribute
 * empty (OTA is inert there, so the loop never runs). Not yet validated on
 * silicon — flash externally only.
 *
 * The RAMFUNC bodies are deliberately SELF-CONTAINED register-level
 * mirrors of the vendor fmc_page_erase / fmc_doubleword_program
 * (gd32g5x3_fmc.c) rather than wrappers calling them: the vendor
 * routines — including their fmc_ready_wait poll loop — live in flash,
 * so a thin RAM shim around them would re-enter flash for the entire
 * FMC busy window and defeat the .ramfunc guarantee.  Everything
 * fetched between setting START/PG and BUSY clearing (code AND literal
 * pools, which GCC emits into the function's own section) must be
 * RAM-resident.  FMC_STAT/FMC_CTL accesses are peripheral-bus reads,
 * not flash reads — polling them from RAM is safe while FMC is busy.
 */

#include <string.h>

#include "gd32g5x3.h"

#include "fmc_ota.h"
#include "ota_layout.h"

#if defined(BRIDGE_OTA_PARTITIONED)
#define OTA_RAMFUNC __attribute__((section(".ramfunc"), noinline))
#else
#define OTA_RAMFUNC
#endif

/* Vendor-private bit offset of FMC_CTL.PNSEL (gd32g5x3_fmc.c
 * CTL_PNSEL_OFFSET); the public header only exposes the BITS(3,10) mask. */
#define OTA_FMC_CTL_PNSEL_OFFSET 3u

/* Dual-bank geometry (silicon-verified 2026-06-04: the V2N's GD32G553
 * ships with OBCTL.DBS = 1).  In dual-bank mode the page is 1 KB (vendor
 * MAIN_FLASH_PAGE_SIZE_DBANK) and addresses >= bank1 base need
 * FMC_CTL.BKSEL with a bank-relative page index; single-bank mode is
 * 2 KB pages, bank0 only.  Getting this wrong "succeeds" loudly:
 * page indexes silently resolve into bank0 and erase the wrong region
 * (caught on silicon as a PGERR on the first slot-B program). */
#define OTA_FMC_BANK1_BASE      0x08040000u
#define OTA_FMC_PAGE_SIZE_DBANK 0x400u
#define OTA_FMC_PAGE_SIZE_SBANK 0x800u

/* Sticky FMC_STAT error flags (rc_w1).  MUST be cleared before each
 * erase/program: one failed op otherwise poisons every later
 * ota_fmc_wait_ready with the latched error (silicon-verified: a single
 * PGERR short-circuited the whole remaining OTA session). */
#define OTA_FMC_STAT_ERR_MASK (FMC_STAT_WPERR | FMC_STAT_PGERR |   \
                               FMC_STAT_PGSERR | FMC_STAT_PGAERR | \
                               FMC_STAT_RPERR | FMC_STAT_PGMERR |  \
                               FMC_STAT_OBERR)

bool ota_fmc_supported(void) { return true; }

/* RAM-safe FMC state decode (vendor fmc_state_get, inlined so the
 * RAMFUNC callers never branch to a flash-resident helper while the
 * FMC is busy).  Decode order mirrors the vendor exactly. */
__attribute__((always_inline)) static inline fmc_state_enum ota_fmc_state_now(void)
{
    if ((FMC_STAT & FMC_STAT_BUSY) != 0u)   { return FMC_BUSY; }
    if ((FMC_STAT & FMC_STAT_WPERR) != 0u)  { return FMC_WPERR; }
    if ((FMC_STAT & FMC_STAT_PGERR) != 0u)  { return FMC_PGERR; }
    if ((FMC_STAT & FMC_STAT_PGSERR) != 0u) { return FMC_PGSERR; }
    if ((FMC_STAT & FMC_STAT_PGAERR) != 0u) { return FMC_PGAERR; }
    if ((FMC_STAT & FMC_STAT_RPERR) != 0u)  { return FMC_RPERR; }
    if ((FMC_STAT & FMC_STAT_PGMERR) != 0u) { return FMC_PGMERR; }
    if ((FMC_STAT & FMC_STAT_OBERR) != 0u)  { return FMC_OBERR; }
    return FMC_READY;
}

/* RAM-safe fmc_ready_wait (vendor decode + timeout, inlined). */
__attribute__((always_inline)) static inline fmc_state_enum ota_fmc_wait_ready(uint32_t timeout)
{
    fmc_state_enum st;
    do {
        st = ota_fmc_state_now();
        timeout--;
    } while ((st == FMC_BUSY) && (timeout != 0u));
    return (st == FMC_BUSY) ? FMC_TOERR : st;
}

OTA_RAMFUNC static fmc_state_enum erase_one_page(uint32_t addr)
{
    const bool dual = (FMC_OBCTL & FMC_OBCTL_DBS) != 0u;
    uint32_t   page;
    bool       bank1 = false;

    if (dual) {
        bank1 = (addr >= OTA_FMC_BANK1_BASE);
        page  = (addr - (bank1 ? OTA_FMC_BANK1_BASE : OTA_BOOTLOADER_BASE))
                / OTA_FMC_PAGE_SIZE_DBANK;
    } else {
        page = (addr - OTA_BOOTLOADER_BASE) / OTA_FMC_PAGE_SIZE_SBANK;
    }

    fmc_state_enum st = ota_fmc_wait_ready(FMC_TIMEOUT_COUNT);
    if (st != FMC_READY) {
        return st;
    }
    FMC_STAT = OTA_FMC_STAT_ERR_MASK;        /* drop stale sticky errors */
    if (bank1) {
        FMC_CTL |= FMC_CTL_BKSEL;
    } else {
        FMC_CTL &= ~FMC_CTL_BKSEL;
    }
    FMC_CTL &= ~FMC_CTL_PNSEL;
    FMC_CTL |= page << OTA_FMC_CTL_PNSEL_OFFSET;
    FMC_CTL |= FMC_CTL_PER;
    FMC_CTL |= FMC_CTL_START;

    st = ota_fmc_wait_ready(FMC_TIMEOUT_COUNT);

    FMC_CTL &= ~FMC_CTL_PER;
    FMC_CTL &= ~FMC_CTL_PNSEL;
    FMC_CTL &= ~FMC_CTL_BKSEL;
    return st;
}

bool ota_fmc_erase_range(uint32_t base, uint32_t len)
{
    /* Layout regions stay OTA_PAGE_SIZE-granular (2 KB -- a multiple of
     * the real page in both bank modes); the erase loop walks the REAL
     * page size so dual-bank (1 KB pages) erases every page. */
    if ((base % OTA_PAGE_SIZE) != 0u || (len % OTA_PAGE_SIZE) != 0u) {
        return false;
    }
    const uint32_t step = ((FMC_OBCTL & FMC_OBCTL_DBS) != 0u)
                              ? OTA_FMC_PAGE_SIZE_DBANK
                              : OTA_FMC_PAGE_SIZE_SBANK;
    bool ok = true;
    fmc_unlock();
    for (uint32_t a = base; a < base + len; a += step) {
        if (erase_one_page(a) != FMC_READY) { ok = false; break; }
    }
    fmc_lock();
    return ok;
}

OTA_RAMFUNC static fmc_state_enum program_one_dword(uint32_t addr, uint64_t dw)
{
    fmc_state_enum st = ota_fmc_wait_ready(FMC_TIMEOUT_COUNT);
    if (st != FMC_READY) {
        return st;
    }
    FMC_STAT = OTA_FMC_STAT_ERR_MASK;        /* drop stale sticky errors */
    FMC_CTL |= FMC_CTL_PG;
    REG32(addr)      = (uint32_t)(dw & 0xFFFFFFFFu);
    __ISB();
    REG32(addr + 4u) = (uint32_t)(dw >> 32);

    st = ota_fmc_wait_ready(FMC_TIMEOUT_COUNT);

    FMC_CTL &= ~FMC_CTL_PG;
    return st;
}

bool ota_fmc_program(uint32_t addr, const uint8_t *data, size_t len)
{
    /* GD32G5 programs in 64-bit doublewords; `addr` must be 8-byte aligned
     * (the host paces chunk offsets on 8-byte boundaries). Pad a short tail
     * with 0xFF (erased state). */
    if ((addr % 8u) != 0u) {
        return false;
    }
    bool ok = true;
    fmc_unlock();
    for (size_t i = 0u; i < len; i += 8u) {
        uint8_t buf[8];
        const size_t n = (len - i > 8u) ? 8u : (len - i);
        for (size_t k = 0u; k < 8u; k++) {
            buf[k] = (k < n) ? data[i + k] : 0xFFu;
        }
        uint64_t dw;
        memcpy(&dw, buf, sizeof dw);
        if (program_one_dword(addr + (uint32_t)i, dw) != FMC_READY) {
            ok = false;
            break;
        }
    }
    fmc_lock();
    return ok;
}

void ota_system_reset(void)
{
    NVIC_SystemReset();
}
