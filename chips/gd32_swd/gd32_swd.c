/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bit-banged SWD packet layer for the GD32G553 supervisor MCU on
 * V2N / V2N-M1.  Public surface declared in <alp/chips/gd32_swd.h>.
 *
 * The implementation tracks the Arm ADIv5 / Coresight specification
 * for SW-DPv2 and the GD32G553 FMC flash controller register layout.
 * Algorithm references (the canonical bring-up sequence; the
 * JTAG-to-SWD switch code; the IDR table) are public:
 *
 *   * Arm DDI 0316C "ARM Debug Interface v5 Architecture Specification"
 *   * Arm DUI 0552A "Coresight DAP Bit-bang Algorithms"
 *   * GigaDevice GD32G553 User Manual Rev 1.2 §3 ("Flash memory
 *     controller (FMC)") -- mirrored from the STM32G4 FMC layout the
 *     GD32G553 is register-compatible with.
 *
 * No third-party source is copied; only the wire algorithm.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "alp/chips/gd32_swd.h"

/* ------------------------------------------------------------------ */
/* SW-DP / AP register addresses (4-bit fields A[3:2] combined with    */
/* APnDP into a 6-bit packet header byte).                              */
/* ------------------------------------------------------------------ */

/* SW-DP registers (APnDP=0). */
#define DP_REG_DPIDR        0x0u
#define DP_REG_ABORT        0x0u  /* (write side) */
#define DP_REG_CTRL_STAT    0x4u
#define DP_REG_SELECT       0x8u  /* (write side) */
#define DP_REG_RDBUFF       0xCu  /* (read side) */

/* AHB-AP banks. */
#define AP_REG_CSW          0x00u
#define AP_REG_TAR          0x04u
#define AP_REG_DRW          0x0Cu
#define AP_REG_IDR          0xFCu

/* CTRL/STAT bits. */
#define CTRL_CSYSPWRUPREQ   (1u << 30)
#define CTRL_CSYSPWRUPACK   (1u << 31)
#define CTRL_CDBGPWRUPREQ   (1u << 28)
#define CTRL_CDBGPWRUPACK   (1u << 29)
#define CTRL_STICKYERR      (1u << 5)
#define CTRL_STICKYORUN     (1u << 1)

/* AP CSW field: 32-bit transfer + auto-increment. */
#define CSW_SIZE_WORD       (0x2u)        /* SIZE=Word */
#define CSW_ADDR_INC_SINGLE (0x1u << 4)
#define CSW_BASE_VALUE      (0x23000052u) /* DbgSwEnable | Priv | HPROT */

/* Cortex-M debug register addresses (memory-mapped via the AHB-AP). */
#define CM_DHCSR            0xE000EDF0u
#define CM_DEMCR            0xE000EDFCu
#define CM_AIRCR            0xE000ED0Cu
#define CM_DHCSR_DBGKEY     0xA05F0000u
#define CM_DHCSR_C_DEBUGEN  (1u << 0)
#define CM_DHCSR_C_HALT     (1u << 1)
#define CM_DHCSR_S_HALT     (1u << 17)
#define CM_AIRCR_VECTKEY    0x05FA0000u
#define CM_AIRCR_SYSRESETREQ (1u << 2)

/* GD32G553 FMC -- STM32G4-compatible register block @ 0x40022000. */
#define FMC_BASE            0x40022000u
#define FMC_REG_KEYR        (FMC_BASE + 0x08u)
#define FMC_REG_SR          (FMC_BASE + 0x10u)
#define FMC_REG_CR          (FMC_BASE + 0x14u)
#define FMC_KEY1            0x45670123u
#define FMC_KEY2            0xCDEF89ABu
#define FMC_CR_PG           (1u << 0)
#define FMC_CR_PER          (1u << 1)
#define FMC_CR_PNB_SHIFT    3u
#define FMC_CR_PNB_MASK     (0x7Fu << FMC_CR_PNB_SHIFT)
#define FMC_CR_STRT         (1u << 16)
#define FMC_CR_LOCK         (1u << 31)
#define FMC_SR_BSY          (1u << 16)
#define FMC_SR_PROGERR_MASK 0x000000F8u    /* PROGERR | WRPERR | PGAERR | SIZERR | PGSERR */

/* ------------------------------------------------------------------ */
/* Pacing                                                              */
/* ------------------------------------------------------------------ */

static void swd_clock_delay(const gd32_swd_t *ctx)
{
    /* Tight no-op loop -- the function-call overhead + bounds check
     * gives ~5-10 ns per spin on a Cortex-A55.  The default delay of
     * 4 lands SWCLK around 1 MHz on the V2N. */
    volatile uint32_t spin = ctx->clock_delay;
    while (spin != 0u) --spin;
}

/* ------------------------------------------------------------------ */
/* SWDIO direction switch                                              */
/* ------------------------------------------------------------------ */

static alp_status_t swdio_as_output(gd32_swd_t *ctx)
{
    if (ctx->swdio_is_output) return ALP_OK;
    alp_status_t s = alp_gpio_configure(ctx->swdio, ALP_GPIO_OUTPUT,
                                        ALP_GPIO_PULL_NONE);
    if (s != ALP_OK) return s;
    ctx->swdio_is_output = true;
    return ALP_OK;
}

static alp_status_t swdio_as_input(gd32_swd_t *ctx)
{
    if (!ctx->swdio_is_output) return ALP_OK;
    alp_status_t s = alp_gpio_configure(ctx->swdio, ALP_GPIO_INPUT,
                                        ALP_GPIO_PULL_UP);
    if (s != ALP_OK) return s;
    ctx->swdio_is_output = false;
    return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Bit-bang primitives                                                  */
/* ------------------------------------------------------------------ */

static void swd_clock_cycle(gd32_swd_t *ctx)
{
    (void)alp_gpio_write(ctx->swclk, false);
    swd_clock_delay(ctx);
    (void)alp_gpio_write(ctx->swclk, true);
    swd_clock_delay(ctx);
}

static void swd_write_bit(gd32_swd_t *ctx, bool bit)
{
    /* Target samples SWDIO on the rising edge of SWCLK. */
    (void)alp_gpio_write(ctx->swclk, false);
    (void)alp_gpio_write(ctx->swdio, bit);
    swd_clock_delay(ctx);
    (void)alp_gpio_write(ctx->swclk, true);
    swd_clock_delay(ctx);
}

static bool swd_read_bit(gd32_swd_t *ctx)
{
    bool level = false;
    (void)alp_gpio_write(ctx->swclk, false);
    swd_clock_delay(ctx);
    (void)alp_gpio_read(ctx->swdio, &level);
    (void)alp_gpio_write(ctx->swclk, true);
    swd_clock_delay(ctx);
    return level;
}

static void swd_write_bits(gd32_swd_t *ctx, uint32_t bits, unsigned count)
{
    /* LSB first per the SWD specification. */
    for (unsigned i = 0u; i < count; ++i) {
        swd_write_bit(ctx, (bits >> i) & 0x1u);
    }
}

static uint32_t swd_read_bits(gd32_swd_t *ctx, unsigned count)
{
    uint32_t value = 0u;
    for (unsigned i = 0u; i < count; ++i) {
        if (swd_read_bit(ctx)) value |= (1u << i);
    }
    return value;
}

static void swd_turnaround(gd32_swd_t *ctx)
{
    /* One clock cycle with SWDIO floating gives the target time to
     * switch its driver direction. */
    swd_clock_cycle(ctx);
}

static void swd_line_reset(gd32_swd_t *ctx)
{
    /* >= 50 clock cycles with SWDIO held high.  We do 52 to leave
     * margin against the spec's 50-clock minimum. */
    (void)swdio_as_output(ctx);
    (void)alp_gpio_write(ctx->swdio, true);
    for (unsigned i = 0u; i < 52u; ++i) swd_clock_cycle(ctx);
    /* Two final idle clocks with SWDIO low before the first request. */
    (void)alp_gpio_write(ctx->swdio, false);
    swd_clock_cycle(ctx);
    swd_clock_cycle(ctx);
}

static void swd_jtag_to_swd_switch(gd32_swd_t *ctx)
{
    /* The JTAG-to-SWD switch code is 0xE79E (LSB first).  Documented
     * in Arm DDI 0316C §5.3.1.  Targets in mixed-mode default-on-JTAG
     * transition to SWD on receiving this sequence; targets already
     * in SWD safely ignore it. */
    (void)swdio_as_output(ctx);
    swd_write_bits(ctx, 0xE79Eu, 16u);
}

/* ------------------------------------------------------------------ */
/* Packet header + request / response sequence                          */
/* ------------------------------------------------------------------ */

/* Build the 8-bit SWD request header.
 *   start(1) | APnDP(1) | RnW(1) | A2(1) | A3(1) | parity(1) | stop(1=0) | park(1)
 * (Field order in time / LSB first.)  Park = 1 per ADIv5. */
static uint8_t make_request(bool ap, bool rnw, uint8_t addr_a32)
{
    /* addr_a32 carries the 4-bit DP/AP register address; bits [3:2]
     * are on the wire, [1:0] must be zero. */
    const uint8_t a2 = (addr_a32 >> 2) & 0x1u;
    const uint8_t a3 = (addr_a32 >> 3) & 0x1u;
    const uint8_t parity = (uint8_t)((ap ? 1u : 0u) ^ (rnw ? 1u : 0u) ^ a2 ^ a3);
    uint8_t hdr = 0x81u;                       /* start(1) | park(1) */
    if (ap)        hdr |= (1u << 1);
    if (rnw)       hdr |= (1u << 2);
    hdr |= (uint8_t)(a2 << 3);
    hdr |= (uint8_t)(a3 << 4);
    hdr |= (uint8_t)(parity << 5);
    /* stop bit (bit 6) is always 0. */
    return hdr;
}

static bool parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1u) != 0u;
}

static gd32_swd_ack_t swd_xfer(gd32_swd_t *ctx, bool ap, bool rnw,
                               uint8_t addr_a32, uint32_t *data_io)
{
    if (!ctx->initialised) return GD32_SWD_ACK_PROTO;

    /* 1. Send the 8-bit request header. */
    (void)swdio_as_output(ctx);
    swd_write_bits(ctx, make_request(ap, rnw, addr_a32), 8u);

    /* 2. Turnaround: host releases SWDIO. */
    (void)swdio_as_input(ctx);
    swd_turnaround(ctx);

    /* 3. Read the 3-bit ACK (LSB first). */
    uint32_t ack = swd_read_bits(ctx, 3u);

    if (ack == GD32_SWD_ACK_OK) {
        if (rnw) {
            /* 4. Read 32-bit data + parity.  Host still has SWDIO as input. */
            uint32_t value = swd_read_bits(ctx, 32u);
            bool     par   = swd_read_bit(ctx);
            (void)swdio_as_output(ctx);
            swd_turnaround(ctx);
            (void)alp_gpio_write(ctx->swdio, false);
            if (data_io != NULL) *data_io = value;
            if (par != parity32(value)) return GD32_SWD_ACK_PROTO;
            return GD32_SWD_ACK_OK;
        } else {
            /* 4. Turn around again, then write 32-bit data + parity. */
            swd_turnaround(ctx);
            (void)swdio_as_output(ctx);
            uint32_t value = (data_io != NULL) ? *data_io : 0u;
            swd_write_bits(ctx, value, 32u);
            swd_write_bit(ctx, parity32(value));
            (void)alp_gpio_write(ctx->swdio, false);
            return GD32_SWD_ACK_OK;
        }
    }

    /* WAIT / FAULT / PROTO -- consume the turnaround and return. */
    (void)swdio_as_output(ctx);
    swd_turnaround(ctx);
    (void)alp_gpio_write(ctx->swdio, false);
    return (gd32_swd_ack_t)ack;
}

/* ------------------------------------------------------------------ */
/* Higher-level DP / AP helpers                                         */
/* ------------------------------------------------------------------ */

static alp_status_t swd_read_dp(gd32_swd_t *ctx, uint8_t a, uint32_t *out)
{
    for (unsigned i = 0u; i < GD32_SWD_MAX_WAIT_RETRIES; ++i) {
        gd32_swd_ack_t ack = swd_xfer(ctx, false, true, a, out);
        if (ack == GD32_SWD_ACK_OK)    return ALP_OK;
        if (ack == GD32_SWD_ACK_WAIT)  continue;
        return ALP_ERR_IO;
    }
    return ALP_ERR_TIMEOUT;
}

static alp_status_t swd_write_dp(gd32_swd_t *ctx, uint8_t a, uint32_t v)
{
    for (unsigned i = 0u; i < GD32_SWD_MAX_WAIT_RETRIES; ++i) {
        uint32_t tmp = v;
        gd32_swd_ack_t ack = swd_xfer(ctx, false, false, a, &tmp);
        if (ack == GD32_SWD_ACK_OK)    return ALP_OK;
        if (ack == GD32_SWD_ACK_WAIT)  continue;
        return ALP_ERR_IO;
    }
    return ALP_ERR_TIMEOUT;
}

static alp_status_t swd_select_ap(gd32_swd_t *ctx, uint8_t ap_num, uint8_t bank)
{
    const uint32_t sel = ((uint32_t)ap_num << 24)
                       | ((uint32_t)bank   << 4);
    return swd_write_dp(ctx, DP_REG_SELECT, sel);
}

static alp_status_t swd_write_ap(gd32_swd_t *ctx, uint8_t a, uint32_t v)
{
    for (unsigned i = 0u; i < GD32_SWD_MAX_WAIT_RETRIES; ++i) {
        uint32_t tmp = v;
        gd32_swd_ack_t ack = swd_xfer(ctx, true, false, a, &tmp);
        if (ack == GD32_SWD_ACK_OK)    return ALP_OK;
        if (ack == GD32_SWD_ACK_WAIT)  continue;
        return ALP_ERR_IO;
    }
    return ALP_ERR_TIMEOUT;
}

static alp_status_t swd_read_ap(gd32_swd_t *ctx, uint8_t a, uint32_t *out)
{
    /* AP reads are pipelined: the read request returns the PREVIOUS
     * AP-read value.  Drain the value by reading DP.RDBUFF after the
     * dummy access. */
    uint32_t dummy;
    for (unsigned i = 0u; i < GD32_SWD_MAX_WAIT_RETRIES; ++i) {
        gd32_swd_ack_t ack = swd_xfer(ctx, true, true, a, &dummy);
        if (ack == GD32_SWD_ACK_OK)    break;
        if (ack == GD32_SWD_ACK_WAIT)  continue;
        return ALP_ERR_IO;
    }
    return swd_read_dp(ctx, DP_REG_RDBUFF, out);
}

static alp_status_t swd_power_up_debug(gd32_swd_t *ctx)
{
    alp_status_t s = swd_write_dp(ctx, DP_REG_CTRL_STAT,
                                  CTRL_CSYSPWRUPREQ | CTRL_CDBGPWRUPREQ);
    if (s != ALP_OK) return s;

    for (unsigned i = 0u; i < 256u; ++i) {
        uint32_t stat = 0u;
        s = swd_read_dp(ctx, DP_REG_CTRL_STAT, &stat);
        if (s != ALP_OK) return s;
        if ((stat & (CTRL_CSYSPWRUPACK | CTRL_CDBGPWRUPACK))
            == (CTRL_CSYSPWRUPACK | CTRL_CDBGPWRUPACK)) {
            return ALP_OK;
        }
    }
    return ALP_ERR_TIMEOUT;
}

static alp_status_t swd_mem_write32(gd32_swd_t *ctx, uint32_t addr, uint32_t value)
{
    alp_status_t s = swd_select_ap(ctx, 0u, 0u);
    if (s != ALP_OK) return s;
    s = swd_write_ap(ctx, AP_REG_CSW, CSW_BASE_VALUE | CSW_SIZE_WORD);
    if (s != ALP_OK) return s;
    s = swd_write_ap(ctx, AP_REG_TAR, addr);
    if (s != ALP_OK) return s;
    return swd_write_ap(ctx, AP_REG_DRW, value);
}

static alp_status_t swd_mem_read32(gd32_swd_t *ctx, uint32_t addr, uint32_t *out)
{
    alp_status_t s = swd_select_ap(ctx, 0u, 0u);
    if (s != ALP_OK) return s;
    s = swd_write_ap(ctx, AP_REG_CSW, CSW_BASE_VALUE | CSW_SIZE_WORD);
    if (s != ALP_OK) return s;
    s = swd_write_ap(ctx, AP_REG_TAR, addr);
    if (s != ALP_OK) return s;
    return swd_read_ap(ctx, AP_REG_DRW, out);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

alp_status_t gd32_swd_init(gd32_swd_t *ctx,
                           alp_gpio_t *swdio,
                           alp_gpio_t *swclk,
                           alp_gpio_t *nrst)
{
    if (ctx == NULL || swdio == NULL || swclk == NULL) return ALP_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->swdio       = swdio;
    ctx->swclk       = swclk;
    ctx->nrst        = nrst;
    ctx->clock_delay = GD32_SWD_DEFAULT_CLOCK_DELAY;

    /* SWCLK + SWDIO start as outputs driven high (the SWD idle
     * state per the spec).  SWDIO will toggle between input and
     * output at runtime in the bit-bang routines. */
    alp_status_t s = alp_gpio_configure(swclk, ALP_GPIO_OUTPUT,
                                        ALP_GPIO_PULL_NONE);
    if (s != ALP_OK) return s;
    s = alp_gpio_configure(swdio, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
    if (s != ALP_OK) return s;
    ctx->swdio_is_output = true;

    s = alp_gpio_write(swclk, true);
    if (s != ALP_OK) return s;
    s = alp_gpio_write(swdio, true);
    if (s != ALP_OK) return s;

    /* If NRST is wired, leave it in the released (HiZ on an open-drain
     * net) state.  Driving low + releasing happens later inside
     * reset_and_run. */
    if (nrst != NULL) {
        /* Caller is responsible for the pin being configured open-
         * drain at the SoC level -- on the V2N this is a hard pad
         * property because the NRST line shares a net with the
         * primary PMIC's reset-out (coordinate with the maintainer
         * for rail-level details). */
        (void)alp_gpio_write(nrst, true);
    }

    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t gd32_swd_set_clock_delay(gd32_swd_t *ctx, uint32_t delay_spins)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (delay_spins > 2048u) delay_spins = 2048u;
    ctx->clock_delay = delay_spins;
    return ALP_OK;
}

alp_status_t gd32_swd_connect(gd32_swd_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* 1. Line reset (50+ clocks SWDIO high). */
    swd_line_reset(ctx);

    /* 2. JTAG-to-SWD switch sequence. */
    swd_jtag_to_swd_switch(ctx);

    /* 3. Second line reset is required immediately after the switch. */
    swd_line_reset(ctx);

    /* 4. Read DPIDR. */
    uint32_t idcode = 0u;
    alp_status_t s = swd_read_dp(ctx, DP_REG_DPIDR, &idcode);
    if (s != ALP_OK) return s;
    if (idcode == 0u || idcode == 0xFFFFFFFFu) return ALP_ERR_IO;

    ctx->idcode = idcode;

    /* The GD32G553 carries the Cortex-M33 r0p1 IDCODE.  We don't
     * hard-reject mismatches at this layer -- callers that want a
     * strict match check against GD32_SWD_EXPECTED_IDCODE themselves.
     * The driver's job is to confirm a DP responds. */

    /* 5. Power up the SW-DP. */
    s = swd_power_up_debug(ctx);
    if (s != ALP_OK) return s;

    ctx->connected = true;
    return ALP_OK;
}

alp_status_t gd32_swd_halt(gd32_swd_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (!ctx->connected) return ALP_ERR_NOT_READY;

    /* Write DHCSR with DBGKEY + C_HALT + C_DEBUGEN. */
    alp_status_t s = swd_mem_write32(ctx, CM_DHCSR,
                                     CM_DHCSR_DBGKEY | CM_DHCSR_C_HALT
                                     | CM_DHCSR_C_DEBUGEN);
    if (s != ALP_OK) return s;

    /* Poll DHCSR until S_HALT reads back set.  Bound to a few
     * hundred SWD round-trips to keep the call from livelocking
     * if the target is in a wedged state. */
    for (unsigned i = 0u; i < 1024u; ++i) {
        uint32_t dhcsr = 0u;
        s = swd_mem_read32(ctx, CM_DHCSR, &dhcsr);
        if (s != ALP_OK) return s;
        if (dhcsr & CM_DHCSR_S_HALT) return ALP_OK;
    }
    return ALP_ERR_TIMEOUT;
}

/* ----- flash helpers ------------------------------------------------ */

static alp_status_t fmc_wait_busy(gd32_swd_t *ctx)
{
    for (unsigned i = 0u; i < 16384u; ++i) {
        uint32_t sr = 0u;
        alp_status_t s = swd_mem_read32(ctx, FMC_REG_SR, &sr);
        if (s != ALP_OK) return s;
        if ((sr & FMC_SR_BSY) == 0u) {
            return (sr & FMC_SR_PROGERR_MASK) ? ALP_ERR_IO : ALP_OK;
        }
    }
    return ALP_ERR_TIMEOUT;
}

static alp_status_t fmc_unlock(gd32_swd_t *ctx)
{
    /* Two-step unlock sequence; bad keys latch the FMC into locked
     * state until the next reset. */
    alp_status_t s = swd_mem_write32(ctx, FMC_REG_KEYR, FMC_KEY1);
    if (s != ALP_OK) return s;
    return swd_mem_write32(ctx, FMC_REG_KEYR, FMC_KEY2);
}

static alp_status_t fmc_lock(gd32_swd_t *ctx)
{
    uint32_t cr = 0u;
    alp_status_t s = swd_mem_read32(ctx, FMC_REG_CR, &cr);
    if (s != ALP_OK) return s;
    return swd_mem_write32(ctx, FMC_REG_CR, cr | FMC_CR_LOCK);
}

alp_status_t gd32_swd_flash_erase(gd32_swd_t *ctx,
                                  uint32_t addr,
                                  uint32_t size)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (!ctx->connected) return ALP_ERR_NOT_READY;
    if (size == 0u) return ALP_ERR_INVAL;
    if (addr < GD32_SWD_FMC_FLASH_BASE) return ALP_ERR_INVAL;

    /* Round the start down + the end up to sector boundaries. */
    const uint32_t start_sector = (addr - GD32_SWD_FMC_FLASH_BASE)
                                / GD32_SWD_FMC_SECTOR_BYTES;
    const uint32_t end_byte_excl = addr + size;
    const uint32_t end_sector_excl =
        (end_byte_excl - GD32_SWD_FMC_FLASH_BASE + GD32_SWD_FMC_SECTOR_BYTES - 1u)
        / GD32_SWD_FMC_SECTOR_BYTES;

    alp_status_t s = fmc_unlock(ctx);
    if (s != ALP_OK) return s;

    for (uint32_t sec = start_sector; sec < end_sector_excl; ++sec) {
        s = fmc_wait_busy(ctx);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        uint32_t cr = FMC_CR_PER | ((sec << FMC_CR_PNB_SHIFT) & FMC_CR_PNB_MASK);
        s = swd_mem_write32(ctx, FMC_REG_CR, cr);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        s = swd_mem_write32(ctx, FMC_REG_CR, cr | FMC_CR_STRT);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        s = fmc_wait_busy(ctx);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
    }

    return fmc_lock(ctx);
}

alp_status_t gd32_swd_flash_write(gd32_swd_t *ctx,
                                  uint32_t addr,
                                  const uint8_t *data,
                                  size_t len)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (!ctx->connected) return ALP_ERR_NOT_READY;
    if (data == NULL || len == 0u) return ALP_ERR_INVAL;
    if ((addr & 0x7u) != 0u) return ALP_ERR_INVAL; /* doubleword-aligned */
    if (addr < GD32_SWD_FMC_FLASH_BASE) return ALP_ERR_INVAL;

    alp_status_t s = fmc_unlock(ctx);
    if (s != ALP_OK) return s;
    s = swd_mem_write32(ctx, FMC_REG_CR, FMC_CR_PG);
    if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }

    size_t offset = 0u;
    while (offset < len) {
        /* Pull two little-endian 32-bit halves out of the input
         * buffer.  Pad the tail residue with 0xFF (erased state) so a
         * partial trailing doubleword still writes safely. */
        uint8_t buf[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
        const size_t take = (len - offset) > 8u ? 8u : (len - offset);
        memcpy(buf, data + offset, take);
        const uint32_t lo = (uint32_t)buf[0]
                          | ((uint32_t)buf[1] << 8)
                          | ((uint32_t)buf[2] << 16)
                          | ((uint32_t)buf[3] << 24);
        const uint32_t hi = (uint32_t)buf[4]
                          | ((uint32_t)buf[5] << 8)
                          | ((uint32_t)buf[6] << 16)
                          | ((uint32_t)buf[7] << 24);

        s = swd_mem_write32(ctx, addr + (uint32_t)offset,         lo);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        s = swd_mem_write32(ctx, addr + (uint32_t)offset + 4u,    hi);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        s = fmc_wait_busy(ctx);
        if (s != ALP_OK) { (void)fmc_lock(ctx); return s; }
        offset += 8u;
    }

    /* Clear PG before locking. */
    (void)swd_mem_write32(ctx, FMC_REG_CR, 0u);
    return fmc_lock(ctx);
}

alp_status_t gd32_swd_flash_verify(gd32_swd_t *ctx,
                                   uint32_t addr,
                                   const uint8_t *data,
                                   size_t len)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (!ctx->connected) return ALP_ERR_NOT_READY;
    if (data == NULL || len == 0u) return ALP_ERR_INVAL;
    if ((addr & 0x3u) != 0u) return ALP_ERR_INVAL; /* word-aligned */

    size_t offset = 0u;
    while (offset < len) {
        uint32_t word = 0u;
        alp_status_t s = swd_mem_read32(ctx, addr + (uint32_t)offset, &word);
        if (s != ALP_OK) return s;
        const size_t take = (len - offset) > 4u ? 4u : (len - offset);
        for (size_t i = 0u; i < take; ++i) {
            const uint8_t got      = (uint8_t)(word >> (i * 8u));
            const uint8_t expected = data[offset + i];
            if (got != expected) return ALP_ERR_IO;
        }
        offset += 4u;
    }
    return ALP_OK;
}

alp_status_t gd32_swd_reset_and_run(gd32_swd_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

    /* Prefer the hardware reset line when it's wired -- AIRCR.SYSRESETREQ
     * is gated by the SW-DP power-up status, which the driver may have
     * just torn down. */
    if (ctx->nrst != NULL) {
        (void)alp_gpio_write(ctx->nrst, false); /* drive low (open-drain assert) */
        /* Hold reset for a few thousand clock-delay spins -- the
         * GD32G553 boot ROM honours reset pulses >= 10 us. */
        for (unsigned i = 0u; i < 4000u; ++i) swd_clock_delay(ctx);
        (void)alp_gpio_write(ctx->nrst, true);  /* release (HiZ) */
        ctx->connected = false;
        return ALP_OK;
    }

    /* Software fall-back: clear DHCSR's halt + write AIRCR.SYSRESETREQ. */
    if (!ctx->connected) return ALP_ERR_NOT_READY;
    alp_status_t s = swd_mem_write32(ctx, CM_DEMCR, 0u);
    if (s != ALP_OK) return s;
    s = swd_mem_write32(ctx, CM_DHCSR, CM_DHCSR_DBGKEY); /* clear C_HALT */
    if (s != ALP_OK) return s;
    s = swd_mem_write32(ctx, CM_AIRCR,
                        CM_AIRCR_VECTKEY | CM_AIRCR_SYSRESETREQ);
    /* AIRCR write triggers reset; the SW-DP link drops underneath us.
     * Don't expect the ACK to come back. */
    ctx->connected = false;
    return (s == ALP_OK || s == ALP_ERR_IO) ? ALP_OK : s;
}

void gd32_swd_deinit(gd32_swd_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->initialised && ctx->swclk != NULL) {
        (void)alp_gpio_write(ctx->swclk, true);
    }
    if (ctx->initialised && ctx->swdio != NULL && ctx->swdio_is_output) {
        (void)alp_gpio_write(ctx->swdio, true);
    }
    ctx->initialised = false;
    ctx->connected   = false;
    ctx->swdio       = NULL;
    ctx->swclk       = NULL;
    ctx->nrst        = NULL;
}
