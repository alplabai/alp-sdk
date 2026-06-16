/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Realtek RTL8211FDI-VD-CG Ethernet PHY driver.
 *
 * Public API: <alp/chips/rtl8211fdi.h>.  Register definitions follow
 * the IEEE 802.3 clause-22 base set + Realtek's paged extensions
 * documented in the RTL8211F(D)(I)-VD-CG datasheet (Realtek doc
 * 105141 rev 1.1).
 *
 * MDIO is callback-driven (see header for rationale) -- the driver
 * never includes a vendor MDIO-controller header.  On Zephyr the
 * caller's callback wraps mdio_read / mdio_write against the
 * Renesas RZ/V2N's MDIO controller; on baremetal it can drive
 * MDC/MDIO via bit-banging or a vendor MAC's MMD register block.
 */

#include <string.h>

#include "alp/chips/rtl8211fdi.h"

/* --------------------------------------------------------------- */
/* Internal helpers                                                  */
/* --------------------------------------------------------------- */

static alp_status_t mdio_read_raw(rtl8211fdi_t *ctx, uint8_t reg, uint16_t *val)
{
	const int rv = ctx->mdio_read(ctx->phy_addr, reg, val, ctx->mdio_user);
	return rv == 0 ? ALP_OK : ALP_ERR_IO;
}

static alp_status_t mdio_write_raw(rtl8211fdi_t *ctx, uint8_t reg, uint16_t val)
{
	const int rv = ctx->mdio_write(ctx->phy_addr, reg, val, ctx->mdio_user);
	return rv == 0 ? ALP_OK : ALP_ERR_IO;
}

static alp_status_t page_select(rtl8211fdi_t *ctx, uint16_t page)
{
	if (page == ctx->current_page) return ALP_OK;
	alp_status_t s = mdio_write_raw(ctx, RTL8211FDI_REG_PAGE_SELECT, page);
	if (s != ALP_OK) return s;
	ctx->current_page = page;
	return ALP_OK;
}

/* Busy-wait helper.  The driver doesn't pull in any OS-level sleep
 * because chip drivers are portable C99; the caller's MDIO callback
 * already imposes natural pacing on each register access, and the
 * IEEE-spec reset window (typically 0.5 s worst-case) is bounded by
 * the timeout argument so the loop terminates deterministically.
 *
 * Each MDIO transaction crosses the bus and back -- on Zephyr that's
 * on the order of microseconds per access -- so the loop naturally
 * paces itself.  Production callers should pass a generous timeout
 * (e.g. 500 ms = 500000 us) for soft reset. */
static bool timeout_expired(uint32_t *spent_us, uint32_t per_iter_us, uint32_t limit_us)
{
	*spent_us += per_iter_us;
	return *spent_us > limit_us;
}

/* --------------------------------------------------------------- */
/* Lifecycle                                                          */
/* --------------------------------------------------------------- */

alp_status_t rtl8211fdi_init(rtl8211fdi_t *ctx, uint8_t phy_addr, rtl8211fdi_mdio_read_t read,
                             rtl8211fdi_mdio_write_t write, void *mdio_user)
{
	if (ctx == NULL || read == NULL || write == NULL) return ALP_ERR_INVAL;
	if (phy_addr > 31u) return ALP_ERR_INVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->phy_addr     = phy_addr;
	ctx->mdio_read    = read;
	ctx->mdio_write   = write;
	ctx->mdio_user    = mdio_user;
	ctx->current_page = 0xFFFFu; /* force first page_select() to actually write */

	/* Park the page register at the default page before probing so
     * the PHYID reads land on the right page even if the previous
     * MDIO user left the chip on a Realtek-extended page. */
	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;

	s = mdio_read_raw(ctx, RTL8211FDI_REG_PHYID1, &ctx->phy_id1);
	if (s != ALP_OK) return s;
	if (ctx->phy_id1 != RTL8211FDI_PHY_OUI_REALTEK) return ALP_ERR_NOT_READY;

	s = mdio_read_raw(ctx, RTL8211FDI_REG_PHYID2, &ctx->phy_id2);
	if (s != ALP_OK) return s;

	ctx->initialised = true;
	return ALP_OK;
}

void rtl8211fdi_deinit(rtl8211fdi_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->mdio_read   = NULL;
	ctx->mdio_write  = NULL;
	ctx->mdio_user   = NULL;
}

/* --------------------------------------------------------------- */
/* Standard PHY operations                                            */
/* --------------------------------------------------------------- */

alp_status_t rtl8211fdi_soft_reset(rtl8211fdi_t *ctx, uint32_t timeout_us)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;

	uint16_t bmcr = 0u;
	s             = mdio_read_raw(ctx, RTL8211FDI_REG_BMCR, &bmcr);
	if (s != ALP_OK) return s;

	bmcr |= RTL8211FDI_BMCR_RESET;
	s = mdio_write_raw(ctx, RTL8211FDI_REG_BMCR, bmcr);
	if (s != ALP_OK) return s;

	/* Poll BMCR.reset until it self-clears.  Each MDIO transaction
     * burns a few microseconds; assume 50 us per iteration as a
     * conservative upper bound. */
	uint32_t spent = 0u;
	do {
		s = mdio_read_raw(ctx, RTL8211FDI_REG_BMCR, &bmcr);
		if (s != ALP_OK) return s;
		if ((bmcr & RTL8211FDI_BMCR_RESET) == 0u) return ALP_OK;
	} while (!timeout_expired(&spent, 50u, timeout_us));

	return ALP_ERR_TIMEOUT;
}

alp_status_t rtl8211fdi_restart_autoneg(rtl8211fdi_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;

	uint16_t bmcr = 0u;
	s             = mdio_read_raw(ctx, RTL8211FDI_REG_BMCR, &bmcr);
	if (s != ALP_OK) return s;
	bmcr |= RTL8211FDI_BMCR_AUTONEG_ENABLE | RTL8211FDI_BMCR_RESTART_AUTONEG;
	return mdio_write_raw(ctx, RTL8211FDI_REG_BMCR, bmcr);
}

alp_status_t rtl8211fdi_get_link(rtl8211fdi_t *ctx, bool *up, rtl8211fdi_speed_t *speed,
                                 bool *full_duplex)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (up == NULL || speed == NULL || full_duplex == NULL) return ALP_ERR_INVAL;

	*up          = false;
	*speed       = RTL8211FDI_SPEED_UNKNOWN;
	*full_duplex = false;

	/* Read BMSR for the basic link status -- this is the standard
     * IEEE field that doesn't require a page change. */
	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;
	uint16_t bmsr = 0u;
	s             = mdio_read_raw(ctx, RTL8211FDI_REG_BMSR, &bmsr);
	if (s != ALP_OK) return s;

	/* BMSR.link_status latches low on link loss; reading it twice
     * gives the live state.  IEEE 802.3 clause 22.2.4.1.6. */
	if ((bmsr & RTL8211FDI_BMSR_LINK_STATUS) == 0u) {
		s = mdio_read_raw(ctx, RTL8211FDI_REG_BMSR, &bmsr);
		if (s != ALP_OK) return s;
	}
	*up = (bmsr & RTL8211FDI_BMSR_LINK_STATUS) != 0u;
	if (!*up) {
		s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
		(void)s; /* best-effort return-to-default; ignore failure */
		return ALP_OK;
	}

	/* Read the Realtek PHY-specific status register at page 0xA43,
     * register 0x1A.  Bit layout (RTL8211F datasheet table "PHY
     * Specific Status"):
     *   bit  3   = Link    (mirror of BMSR)
     *   bits 5:4 = Speed   (00=10M, 01=100M, 10=1000M, 11=reserved)
     *   bit  3-2 layout TBD vs page revisions; this driver uses
     *   bits 5:4 = speed and bit 3 = duplex per public sources.
     * If your silicon revision differs, the page-/reg-level escape
     * hatches (rtl8211fdi_read_page_reg) let the caller decode
     * the layout directly.
     */
	uint16_t physr = 0u;
	s              = rtl8211fdi_read_page_reg(ctx, RTL8211FDI_PAGE_PHYSR, 0x1Au, &physr);
	if (s != ALP_OK) return s;
	/* Make sure we left the page back at default for the next
     * caller -- read_page_reg() already does this, but assert
     * defensively. */
	if (ctx->current_page != RTL8211FDI_PAGE_DEFAULT) {
		(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	}

	const uint16_t spd_bits = (physr >> 4) & 0x3u;
	switch (spd_bits) {
	case 0x0u:
		*speed = RTL8211FDI_SPEED_10M;
		break;
	case 0x1u:
		*speed = RTL8211FDI_SPEED_100M;
		break;
	case 0x2u:
		*speed = RTL8211FDI_SPEED_1000M;
		break;
	default:
		*speed = RTL8211FDI_SPEED_UNKNOWN;
		break;
	}
	*full_duplex = (physr & (1u << 3)) != 0u;
	return ALP_OK;
}

/* --------------------------------------------------------------- */
/* Wake-on-LAN                                                        */
/* --------------------------------------------------------------- */

/* MAC-address scratch register positions in page 0xD8A.  The
 * RTL8211F datasheet documents three 16-bit registers holding the
 * 48-bit MAC in big-endian network byte order with the LSB of each
 * 16-bit word first:
 *   reg 0x10 = mac[1] << 8 | mac[0]
 *   reg 0x11 = mac[3] << 8 | mac[2]
 *   reg 0x12 = mac[5] << 8 | mac[4]
 * The pattern-match engine compares the six bytes against the
 * magic-packet payload in the same order. */
#define RTL8211FDI_WOL_MAC_REG0 0x10u
#define RTL8211FDI_WOL_MAC_REG1 0x11u
#define RTL8211FDI_WOL_MAC_REG2 0x12u
#define RTL8211FDI_WOL_CTRL_REG 0x13u
#define RTL8211FDI_WOL_STAT_REG 0x14u

#define RTL8211FDI_WOL_CTRL_MAGIC_PKT_EN (1u << 6)
#define RTL8211FDI_WOL_STAT_MAGIC_PKT (1u << 15)

alp_status_t rtl8211fdi_wol_set_mac(rtl8211fdi_t *ctx, const uint8_t mac[6])
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (mac == NULL) return ALP_ERR_INVAL;

	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_WOL);
	if (s != ALP_OK) return s;

	s = mdio_write_raw(ctx, RTL8211FDI_WOL_MAC_REG0, (uint16_t)mac[0] | ((uint16_t)mac[1] << 8));
	if (s != ALP_OK) goto out;
	s = mdio_write_raw(ctx, RTL8211FDI_WOL_MAC_REG1, (uint16_t)mac[2] | ((uint16_t)mac[3] << 8));
	if (s != ALP_OK) goto out;
	s = mdio_write_raw(ctx, RTL8211FDI_WOL_MAC_REG2, (uint16_t)mac[4] | ((uint16_t)mac[5] << 8));
out:
	(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	return s;
}

alp_status_t rtl8211fdi_wol_enable(rtl8211fdi_t *ctx, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_WOL);
	if (s != ALP_OK) return s;

	uint16_t ctrl = 0u;
	s             = mdio_read_raw(ctx, RTL8211FDI_WOL_CTRL_REG, &ctrl);
	if (s != ALP_OK) goto out;
	if (enable)
		ctrl |= RTL8211FDI_WOL_CTRL_MAGIC_PKT_EN;
	else
		ctrl &= (uint16_t)~RTL8211FDI_WOL_CTRL_MAGIC_PKT_EN;
	s = mdio_write_raw(ctx, RTL8211FDI_WOL_CTRL_REG, ctrl);
out:
	(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	return s;
}

alp_status_t rtl8211fdi_wol_clear_event(rtl8211fdi_t *ctx, bool *was_set)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (was_set == NULL) return ALP_ERR_INVAL;

	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_WOL);
	if (s != ALP_OK) return s;

	uint16_t stat = 0u;
	s             = mdio_read_raw(ctx, RTL8211FDI_WOL_STAT_REG, &stat);
	if (s != ALP_OK) goto out;
	*was_set = (stat & RTL8211FDI_WOL_STAT_MAGIC_PKT) != 0u;
	/* Write-1-to-clear the latch. */
	s = mdio_write_raw(ctx, RTL8211FDI_WOL_STAT_REG, RTL8211FDI_WOL_STAT_MAGIC_PKT);
out:
	(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	return s;
}

/* --------------------------------------------------------------- */
/* Raw register R/W                                                   */
/* --------------------------------------------------------------- */

alp_status_t rtl8211fdi_read_reg(rtl8211fdi_t *ctx, uint8_t reg, uint16_t *val)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (val == NULL) return ALP_ERR_INVAL;
	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;
	return mdio_read_raw(ctx, reg, val);
}

alp_status_t rtl8211fdi_write_reg(rtl8211fdi_t *ctx, uint8_t reg, uint16_t val)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	if (s != ALP_OK) return s;
	return mdio_write_raw(ctx, reg, val);
}

alp_status_t rtl8211fdi_read_page_reg(rtl8211fdi_t *ctx, uint16_t page, uint8_t reg, uint16_t *val)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (val == NULL) return ALP_ERR_INVAL;
	alp_status_t s = page_select(ctx, page);
	if (s != ALP_OK) return s;
	s = mdio_read_raw(ctx, reg, val);
	/* Always restore to the default page so the next caller doesn't
     * have to know we touched the page register. */
	(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	return s;
}

alp_status_t rtl8211fdi_write_page_reg(rtl8211fdi_t *ctx, uint16_t page, uint8_t reg, uint16_t val)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = page_select(ctx, page);
	if (s != ALP_OK) return s;
	s = mdio_write_raw(ctx, reg, val);
	(void)page_select(ctx, RTL8211FDI_PAGE_DEFAULT);
	return s;
}
