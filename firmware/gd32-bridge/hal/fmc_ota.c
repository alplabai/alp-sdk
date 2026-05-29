/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge OTA — flash backend (gd32 HAL backend). Real FMC erase /
 * doubleword-program for the inactive slot + metadata, and the system
 * reset used at commit.  Overrides the weak no-ops in src/ota.c.
 *
 * HIL-REQUIRED: in the partitioned build the erase/program inner loop must
 * execute from RAM — on single-bank flash the FMC stalls the bus, so a
 * routine fetched from the bank being erased faults.  The OTA_RAMFUNC
 * attribute places these in `.ramfunc` (which the partitioned bootloader
 * linker must provide); the default full-flash build leaves the attribute
 * empty (OTA is inert there, so the loop never runs). Not yet validated on
 * silicon — flash externally only.
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

bool ota_fmc_supported(void) { return true; }

OTA_RAMFUNC static fmc_state_enum erase_one_page(uint32_t addr)
{
    const uint32_t page = (addr - OTA_BOOTLOADER_BASE) / OTA_PAGE_SIZE;
    return fmc_page_erase(FMC_BANK0, page);
}

bool ota_fmc_erase_range(uint32_t base, uint32_t len)
{
    if ((base % OTA_PAGE_SIZE) != 0u || (len % OTA_PAGE_SIZE) != 0u) {
        return false;
    }
    bool ok = true;
    fmc_unlock();
    for (uint32_t a = base; a < base + len; a += OTA_PAGE_SIZE) {
        if (erase_one_page(a) != FMC_READY) { ok = false; break; }
    }
    fmc_lock();
    return ok;
}

OTA_RAMFUNC static fmc_state_enum program_one_dword(uint32_t addr, uint64_t dw)
{
    return fmc_doubleword_program(addr, dw);
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
