/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rtl8211fdi.h
 * @brief Realtek RTL8211FDI(-VD)-CG 10/100/1000BASE-T Ethernet PHY driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The V2N module populates **two** RTL8211FDI-VD-CG PHYs (ET0 + ET1)
 * sitting behind the Renesas RZ/V2N RGMII MAC.  Each PHY exposes:
 *
 *   - 30 RGMII data + clock lines (TXC/TXCTL/TXD0..3, RXC/RXCTL/RXD0..3)
 *   - MDC + MDIO management bus (1.8 V level, 1 kΩ pull-up per PHY)
 *   - PHY_INTR open-drain interrupt-out (board nets ENET0_nINT, ENET1_nINT)
 *
 * @par Board wiring (V2N)
 *
 * | Signal       | Renesas pad | Notes                                          |
 * |--------------|-------------|------------------------------------------------|
 * | ET0_MDC      | K27         | management clock (PHY 0)                       |
 * | ET0_MDIO     | J29         | management data (PHY 0)                        |
 * | ET0_PHY_INTR | M27         | open-drain interrupt (PHY 0)                   |
 * | ET1_MDC      | N25         | management clock (PHY 1)                       |
 * | ET1_MDIO     | P29 (BGA)   | management data (PHY 1)                        |
 * | ET1_PHY_INTR | T29         | open-drain interrupt (PHY 1)                   |
 *
 * The two PHYs answer at distinct MDIO addresses set by the
 * `PHYAD[2:0]` straps on the chip (board-rev-specific; see
 * `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` for the
 * authoritative pinout).
 *
 * @par MDIO abstraction
 *
 * The Alp SDK's `<alp/peripheral.h>` surface does **not** declare an
 * MDIO API today.  Rather than couple this driver to Zephyr's
 * `<zephyr/drivers/mdio.h>` (Renesas-style host controller), the
 * driver takes **caller-supplied read/write callbacks**:
 *
 *     int (*read)(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user);
 *     int (*write)(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user);
 *
 * Each callback must return `0` on success and a negative value
 * on bus error.  This keeps the chip driver portable across Zephyr
 * (where the callback wraps `mdio_read`/`mdio_write` against the
 * Renesas RZ/V2N MAC's MDIO controller) and bare-metal boards
 * (where the callback can drive MDC/MDIO via GPIO bit-banging or a
 * vendor MAC's register interface).
 *
 * @par Realtek paged register access
 *
 * The RTL8211F-series uses **paged extended registers**.  Page
 * selection happens via the standard IEEE 802.3 register `0x1F` —
 * the driver's `_page_reg_*` helpers handle the page-select
 * pre-amble + page-restore post-amble so callers can read / write a
 * specific page-N register in one call.
 *
 * @par Datasheet provenance
 * - **RTL8211F(D)(I)-VD-CG Datasheet 1.1 (Realtek doc 105141)** —
 *   register map, RGMII timing, WoL programming.
 * - **Renesas RZ/V2N User Manual** — MDIO controller side.
 * - Public Linux PHY driver `drivers/net/phy/realtek.c` — used as a
 *   cross-reference for the page-0xa43 PHY-specific status decoding.
 */

#ifndef ALP_CHIPS_RTL8211FDI_H
#define ALP_CHIPS_RTL8211FDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- */
/* Wire constants                                                    */
/* --------------------------------------------------------------- */

/** OUI prefix that appears in PHYID1 register for every Realtek
 *  Ethernet PHY.  Reads of register `0x02` MUST equal this value to
 *  be recognised by the driver. */
#define RTL8211FDI_PHY_OUI_REALTEK 0x001Cu

/** Standard IEEE 802.3 clause-22 register addresses. */
#define RTL8211FDI_REG_BMCR        0x00u
#define RTL8211FDI_REG_BMSR        0x01u
#define RTL8211FDI_REG_PHYID1      0x02u
#define RTL8211FDI_REG_PHYID2      0x03u
#define RTL8211FDI_REG_ANAR        0x04u
#define RTL8211FDI_REG_ANLPAR      0x05u
#define RTL8211FDI_REG_GBCR        0x09u
#define RTL8211FDI_REG_GBSR        0x0Au
#define RTL8211FDI_REG_PAGE_SELECT 0x1Fu

/** BMCR bits. */
#define RTL8211FDI_BMCR_RESET           (1u << 15)
#define RTL8211FDI_BMCR_AUTONEG_ENABLE  (1u << 12)
#define RTL8211FDI_BMCR_RESTART_AUTONEG (1u << 9)

/** BMSR bits used by the driver. */
#define RTL8211FDI_BMSR_AUTONEG_COMPLETE (1u << 5)
#define RTL8211FDI_BMSR_LINK_STATUS      (1u << 2)

/** Realtek extended pages relevant to this driver. */
#define RTL8211FDI_PAGE_DEFAULT 0x0000u /**< Standard IEEE registers. */
#define RTL8211FDI_PAGE_PHYSR   0x0A43u /**< PHY-specific status reg 0x1A. */
#define RTL8211FDI_PAGE_WOL     0x0D8Au /**< Wake-on-LAN config. */

/** Negotiated speed enumeration (returned by @ref rtl8211fdi_get_link). */
typedef enum {
	RTL8211FDI_SPEED_UNKNOWN = 0,
	RTL8211FDI_SPEED_10M     = 10,
	RTL8211FDI_SPEED_100M    = 100,
	RTL8211FDI_SPEED_1000M   = 1000,
} rtl8211fdi_speed_t;

/* --------------------------------------------------------------- */
/* MDIO callback signatures                                          */
/* --------------------------------------------------------------- */

/** @brief Read a 16-bit register at @p reg on @p phy_addr via MDIO.
 *  @return 0 on success, negative on bus error. */
typedef int (*rtl8211fdi_mdio_read_t)(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user);

/** @brief Write a 16-bit register at @p reg on @p phy_addr via MDIO. */
typedef int (*rtl8211fdi_mdio_write_t)(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user);

/* --------------------------------------------------------------- */
/* Driver context                                                    */
/* --------------------------------------------------------------- */

typedef struct {
	bool                    initialised;
	uint8_t                 phy_addr; /**< 5-bit MDIO address (0..31). */
	rtl8211fdi_mdio_read_t  mdio_read;
	rtl8211fdi_mdio_write_t mdio_write;
	void                   *mdio_user;
	uint16_t                phy_id1;      /**< Cached PHYID1 (Realtek OUI top). */
	uint16_t                phy_id2;      /**< Cached PHYID2 (model + revision). */
	uint16_t                current_page; /**< Last page written to reg 0x1F. */
} rtl8211fdi_t;

/* --------------------------------------------------------------- */
/* Lifecycle                                                          */
/* --------------------------------------------------------------- */

/**
 * @brief Bind the driver to a callback-driven MDIO bus and probe the PHY.
 *
 * Reads PHYID1 + PHYID2; verifies the Realtek OUI (`0x001C`); caches
 * both IDs for diagnostics.
 *
 * @param ctx        Driver context (output).
 * @param phy_addr   MDIO slave address strapped on the PCB (5-bit).
 * @param read       MDIO read callback (must not be NULL).
 * @param write      MDIO write callback (must not be NULL).
 * @param mdio_user  Opaque pointer passed through to every callback.
 *
 * @return ALP_OK on success.
 * @return ALP_ERR_INVAL on NULL args or @p phy_addr > 31.
 * @return ALP_ERR_IO    on MDIO bus error.
 * @return ALP_ERR_NOT_READY if PHYID1 OUI does not match Realtek.
 */
alp_status_t rtl8211fdi_init(rtl8211fdi_t           *ctx,
                             uint8_t                 phy_addr,
                             rtl8211fdi_mdio_read_t  read,
                             rtl8211fdi_mdio_write_t write,
                             void                   *mdio_user);

/** @brief Release the context.  Idempotent.  Does NOT touch the MDIO bus. */
void rtl8211fdi_deinit(rtl8211fdi_t *ctx);

/* --------------------------------------------------------------- */
/* Standard PHY operations                                            */
/* --------------------------------------------------------------- */

/** @brief Trigger a soft reset (BMCR bit 15) and wait for self-clear.
 *
 *  Spins on BMCR.reset for up to @p timeout_us microseconds.  The
 *  IEEE spec mandates the chip self-clears the bit when the reset
 *  completes.
 *
 *  @return ALP_OK on success, ALP_ERR_TIMEOUT if BMCR.reset stays
 *          asserted past the timeout. */
alp_status_t rtl8211fdi_soft_reset(rtl8211fdi_t *ctx, uint32_t timeout_us);

/** @brief Restart auto-negotiation (BMCR bit 9).  Returns immediately;
 *         the host can poll @ref rtl8211fdi_get_link for completion. */
alp_status_t rtl8211fdi_restart_autoneg(rtl8211fdi_t *ctx);

/** @brief Read current link state + speed + duplex.
 *
 *  Decodes the Realtek-specific PHY-status register at page 0xA43
 *  register 0x1A.  When the link is down, @p *speed reads
 *  @ref RTL8211FDI_SPEED_UNKNOWN and @p *full_duplex is undefined.
 *
 *  @param ctx          RTL8211FDI driver context (must be initialised first).
 *  @param up           [out] true if link is up.
 *  @param speed        [out] negotiated speed.
 *  @param full_duplex  [out] true if full-duplex.
 */
alp_status_t
rtl8211fdi_get_link(rtl8211fdi_t *ctx, bool *up, rtl8211fdi_speed_t *speed, bool *full_duplex);

/* --------------------------------------------------------------- */
/* Wake-on-LAN                                                        */
/* --------------------------------------------------------------- */

/** @brief Program the MAC address used for the WoL magic-packet match. */
alp_status_t rtl8211fdi_wol_set_mac(rtl8211fdi_t *ctx, const uint8_t mac[6]);

/** @brief Enable / disable the WoL magic-packet detector. */
alp_status_t rtl8211fdi_wol_enable(rtl8211fdi_t *ctx, bool enable);

/** @brief Read-and-clear the WoL event flag (page 0xD8A, register 0x14
 *         bit 15 -- the "magic packet seen" latch). */
alp_status_t rtl8211fdi_wol_clear_event(rtl8211fdi_t *ctx, bool *was_set);

/* --------------------------------------------------------------- */
/* Raw register R/W (escape hatch)                                    */
/* --------------------------------------------------------------- */

/** @brief Read a clause-22 register on the default page. */
alp_status_t rtl8211fdi_read_reg(rtl8211fdi_t *ctx, uint8_t reg, uint16_t *val);

/** @brief Write a clause-22 register on the default page. */
alp_status_t rtl8211fdi_write_reg(rtl8211fdi_t *ctx, uint8_t reg, uint16_t val);

/** @brief Read a Realtek-extended page-N register.  Page-select +
 *         page-restore are handled internally; the call leaves the
 *         PHY's page register at @ref RTL8211FDI_PAGE_DEFAULT. */
alp_status_t rtl8211fdi_read_page_reg(rtl8211fdi_t *ctx, uint16_t page, uint8_t reg, uint16_t *val);

/** @brief Write a Realtek-extended page-N register. */
alp_status_t rtl8211fdi_write_page_reg(rtl8211fdi_t *ctx, uint16_t page, uint8_t reg, uint16_t val);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_RTL8211FDI_H */
