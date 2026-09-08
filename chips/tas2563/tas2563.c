/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TAS2563 smart Class-D speaker amp driver.  See
 * <alp/chips/tas2563.h> for the public API.
 *
 * Every register address, bit position and reset value below cites
 * TI SLASET3D ("TAS2563 6.1W Boosted Class-D Audio Amplifier With
 * Integrated DSP and IV Sense", April 2019, revised January 2024).
 * Section/table/page numbers are that revision's.  None of it has
 * been read back off silicon.
 */

#include <string.h>

#include "alp/chips/tas2563.h"

/* Register map, book 0 / page 0 -- SLASET3D §7.5.1 "Register Summary
 * Table Page=0x00", p.64-65. */
#define TAS2563_REG_PAGE      0x00u /* Device page          (§7.5.2,  p.65). */
#define TAS2563_REG_PWR_CTL   0x02u /* Power control        (§7.5.4,  p.65). */
#define TAS2563_REG_MISC_CFG1 0x04u /* Misc configuration 1 (§7.5.6,  p.67). */
#define TAS2563_REG_TDM_CFG0  0x06u /* TDM configuration 0  (§7.5.8,  p.68). */
#define TAS2563_REG_TDM_CFG1  0x07u /* TDM configuration 1  (§7.5.9,  p.69). */
#define TAS2563_REG_TDM_CFG2  0x08u /* TDM configuration 2  (§7.5.10, p.69). */
#define TAS2563_REG_TDM_CFG5  0x0Bu /* TDM TX V-sense slot  (§7.5.13, p.71). */
#define TAS2563_REG_TDM_CFG6  0x0Cu /* TDM TX I-sense slot  (§7.5.14, p.71). */
#define TAS2563_REG_INT_LTCH0 0x24u /* Latched interrupts 0 (§7.5.36, p.82). */
#define TAS2563_REG_INT_LTCH1 0x25u /* Latched interrupts 1 (§7.5.37, p.83). */
#define TAS2563_REG_INT_LTCH3 0x26u /* Latched interrupts 2 (§7.5.38, p.84). */
#define TAS2563_REG_INT_LTCH4 0x27u /* Latched interrupts 3 (§7.5.39, p.84). */
#define TAS2563_REG_INT_CLK   0x30u /* INT & CLK CFG        (§7.5.43, p.86). */
#define TAS2563_REG_REVID     0x7Du /* Revision + PG ID, RO (§7.5.60, p.93). */
#define TAS2563_REG_BOOK      0x7Fu /* Device book          (§7.5.62, p.94). */

/* PWR_CTL (0x02) fields -- §7.5.4 Table 7-104, p.66.  MODE is bits
 * 1..0 ONLY; bit 2 is VSNS_PD and bit 3 is ISNS_PD, so a mode change
 * must not reach past 0x03 or it silently powers the IV-sense blocks
 * up or down as a side effect. */
#define TAS2563_PWR_CTL_MODE_MASK 0x03u
#define TAS2563_PWR_CTL_SENSE_PD  0x0Cu /* ISNS_PD (bit 3) | VSNS_PD (bit 2). */

/* MISC_CFG1 (0x04) -- §7.5.6 Table 7-106, p.68.  IRQZ_PU is bit 3,
 * reset 0h (internal 20 kOhm pull-up disabled; §7.3.12 Figure 7-10,
 * p.36 and Table 7-12, p.37). */
#define TAS2563_MISC_CFG1_IRQZ_PU 0x08u

/* TDM_CFG0 (0x06) -- §7.5.8 Table 7-108, p.69.  SAMP_RATE is bits
 * 3..1; AUTO_RATE (bit 4), RAMP_RATE (bit 5) and CLASSD_SYNC (bit 6)
 * are deliberately left at their reset values. */
#define TAS2563_TDM_CFG0_SAMP_RATE_MASK  0x0Eu
#define TAS2563_TDM_CFG0_SAMP_RATE_SHIFT 1u

/* TDM_CFG1 (0x07) -- §7.5.9 Table 7-109, p.69.  RX_JUSTIFY bit 6,
 * RX_OFFSET bits 5..1, RX_EDGE bit 0 (left at reset: rising edge). */
#define TAS2563_TDM_CFG1_RX_JUSTIFY       0x40u
#define TAS2563_TDM_CFG1_RX_OFFSET_MASK   0x3Eu
#define TAS2563_TDM_CFG1_RX_OFFSET_SHIFT  1u
#define TAS2563_TDM_CFG1_RX_FRAMING_FIELD 0x7Eu /* RX_JUSTIFY | RX_OFFSET. */

/* TDM_CFG2 (0x08) -- §7.5.10 Table 7-110, p.70.  RX_SCFG bits 5..4,
 * RX_WLEN bits 3..2, RX_SLEN bits 1..0.  IVMON_LEN (bits 7..6) is
 * left at its reset value 01b = 16 bits. */
#define TAS2563_TDM_CFG2_RX_FIELDS  0x3Fu
#define TAS2563_TDM_CFG2_SCFG_SHIFT 4u
#define TAS2563_TDM_CFG2_WLEN_SHIFT 2u
#define TAS2563_TDM_CFG2_SLEN_SHIFT 0u

/* TDM_CFG5 (0x0B) / TDM_CFG6 (0x0C) -- §7.5.13 Table 7-113 and
 * §7.5.14 Table 7-114, p.71.  Bit 6 is the per-measurement transmit
 * enable, bits 5..0 the transmit time slot. */
#define TAS2563_TDM_SENSE_TX        0x40u
#define TAS2563_TDM_SENSE_SLOT_MASK 0x3Fu
#define TAS2563_TDM_SENSE_FIELD     0x7Fu /* TX enable | slot. */

/* INT & CLK CFG (0x30) -- §7.5.43 Table 7-143, p.86.  CLR_INTP_LTCH
 * is the self-clearing bit 2 (§7.3.12 Table 7-11, p.37);
 * IRQZ_PIN_CFG is bits 1..0, 01b = assert on unmasked LATCHED
 * interrupts (§7.3.12 Table 7-14, p.37). */
#define TAS2563_INT_CLK_CLR_LTCH      0x04u
#define TAS2563_INT_CLK_PIN_CFG_MASK  0x03u
#define TAS2563_INT_CLK_PIN_CFG_LATCH 0x01u

/* A page holds 128 registers and the device map is book/page paged --
 * §7.3.10 "Register Organization", p.34.  0x00 (PAGE) and 0x7F (BOOK)
 * are the paging registers themselves, so a tuning stream may only
 * carry payload registers 0x01..0x7E. */
#define TAS2563_TUNING_REG_MIN 0x01u
#define TAS2563_TUNING_REG_MAX 0x7Eu

static alp_status_t reg_read(tas2563_t *ctx, uint8_t reg, uint8_t *val_out)
{
	return alp_i2c_write_read(ctx->bus, ctx->addr, &reg, 1, val_out, 1);
}

static alp_status_t reg_write(tas2563_t *ctx, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return alp_i2c_write(ctx->bus, ctx->addr, buf, sizeof(buf));
}

/* Read-modify-write of the bits in @p mask.  Every configuration
 * register this driver touches shares a byte with reserved bits or
 * with fields another function owns, so a blind write is never
 * correct here. */
static alp_status_t reg_update(tas2563_t *ctx, uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t      cur = 0;
	alp_status_t s   = reg_read(ctx, reg, &cur);
	if (s != ALP_OK) return s;
	return reg_write(ctx, reg, (uint8_t)((cur & (uint8_t)~mask) | (val & mask)));
}

static alp_status_t select_page(tas2563_t *ctx, uint8_t page)
{
	return reg_write(ctx, TAS2563_REG_PAGE, page);
}

/* Point the device back at book 0 / page 0, the map every other
 * function in this driver addresses.  BOOK lives at page 0
 * (§7.5.62, p.94), so the page write has to come first. */
static alp_status_t select_book0_page0(tas2563_t *ctx)
{
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	return reg_write(ctx, TAS2563_REG_BOOK, 0);
}

/* Table 7-3: only the four AD0/SPICLK strap options identify a real,
 * individually-addressable chip.  TAS2563_I2C_ADDR_BROADCAST (0x48) is
 * deliberately excluded here -- not because these ops only read (they
 * don't: select_page(), called by every function below, WRITEs the page
 * register first, and set_mode() ends on a write too), but because 0x48
 * does not pin down exactly one physical chip the way a strap address
 * does.  Whatever answers a transaction at 0x48 -- every TAS2563 on the
 * bus, an unrelated device strapped there by coincidence (a real EVK
 * pre-respin had an INA236 there, #1846), or nothing -- is undefined
 * from this per-instance context's point of view, for a write as much
 * as for a read.
 *
 * This is the ONLY place addr is validated: tas2563_init() calls it
 * before ctx->addr is ever assigned (below), and every other
 * bus-touching function (read_revision, set_mode) gates on
 * ctx->initialised rather than re-checking the address -- that flag is
 * set true only after this check has already passed, so excluding 0x48
 * here is sufficient for the whole life of the context.
 *
 * A future group-write helper that targets every chip on the bus at
 * once BY DESIGN (e.g. a synchronized MODE_CTRL broadcast across a
 * stereo pair) is free to accept TAS2563_I2C_ADDR_BROADCAST on its own
 * terms -- this per-instance, single-chip context must not. */
static bool addr_is_valid(uint8_t addr)
{
	return addr >= TAS2563_I2C_ADDR_GND_DIRECT && addr <= TAS2563_I2C_ADDR_VDD_DIRECT;
}

alp_status_t tas2563_init(tas2563_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, alp_gpio_t *sd_n)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (!addr_is_valid(addr_7bit)) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus  = bus;
	ctx->addr = addr_7bit;
	ctx->sd_n = sd_n;

	/* If we own the SD_N pin, drive it high to leave HW shutdown.
     * If sd_n == NULL the caller is managing that line elsewhere
     * (or it's tied permanently asserted on the board). */
	if (sd_n != NULL) {
		alp_status_t s = alp_gpio_configure(sd_n, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
		if (s != ALP_OK) return s;
		s = alp_gpio_write(sd_n, true); /* AMP.ENABLE high -> chip out of HW shutdown */
		if (s != ALP_OK) return s;
	}

	/* I2C connectivity probe via REVID register on PAGE 0. */
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;
	uint8_t rev = 0;
	s           = reg_read(ctx, TAS2563_REG_REVID, &rev);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	/* Park the amplifier in software shutdown before handing the
	 * context back.  This is the part's own reset value (PWR_CTL
	 * reset = Eh, MODE = 10b -- §7.5.4 Table 7-104, p.66) and
	 * releasing SD_N also lands there (§7.3.11.1, p.34), but a warm
	 * restart with SD_N board-tied high never went through either:
	 * software shutdown preserves register state (§7.3.11.2, p.34),
	 * so the previous firmware's ACTIVE would survive into this
	 * init.  For a part rated to ~10 W peak into 4 ohm (§1, p.1),
	 * one write is cheap insurance against handing back a context
	 * that is already driving a speaker. */
	s = reg_update(ctx, TAS2563_REG_PWR_CTL, TAS2563_PWR_CTL_MODE_MASK, TAS2563_MODE_SHUTDOWN);
	if (s != ALP_OK) return ALP_ERR_NOT_READY;

	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t tas2563_read_revision(tas2563_t *ctx, uint8_t *rev_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (rev_out == NULL) return ALP_ERR_INVAL;
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	return reg_read(ctx, TAS2563_REG_REVID, rev_out);
}

alp_status_t tas2563_set_mode(tas2563_t *ctx, tas2563_mode_t mode)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* Refuse anything outside the three modelled encodings.  The
	 * fourth documented value, MODE = 11b, runs load diagnostics
	 * into the speaker terminals and then leaves the device ACTIVE
	 * (§7.3.11.5, p.35) -- not somewhere a stray integer should be
	 * able to put a 10 W amplifier. */
	if (mode != TAS2563_MODE_ACTIVE && mode != TAS2563_MODE_MUTE && mode != TAS2563_MODE_SHUTDOWN) {
		return ALP_ERR_INVAL;
	}
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	return reg_update(ctx, TAS2563_REG_PWR_CTL, TAS2563_PWR_CTL_MODE_MASK, (uint8_t)mode);
}

alp_status_t tas2563_set_hw_enable(tas2563_t *ctx, bool enable)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->sd_n == NULL) return ALP_ERR_NOSUPPORT;
	return alp_gpio_write(ctx->sd_n, enable);
}

/* ------------------------------------------------------------------ */
/* I2S / TDM receive configuration                                     */
/* ------------------------------------------------------------------ */

/* SAMP_RATE[2:0] -- §7.5.8 Table 7-108, p.69.  Each encoding covers
 * one 44.1 kHz rate and one 48 kHz rate; which of the pair is running
 * is a clock fact, not a register fact, so both map to the same code. */
static alp_status_t samp_rate_code(uint32_t hz, uint8_t *code_out)
{
	switch (hz) {
	case 7350u:
	case 8000u:
		*code_out = 0u;
		return ALP_OK;
	case 14700u:
	case 16000u:
		*code_out = 1u;
		return ALP_OK;
	case 22050u:
	case 24000u:
		*code_out = 2u;
		return ALP_OK;
	case 29400u:
	case 32000u:
		*code_out = 3u;
		return ALP_OK;
	case 44100u:
	case 48000u:
		*code_out = 4u;
		return ALP_OK;
	case 88200u:
	case 96000u:
		*code_out = 5u;
		return ALP_OK;
	case 176400u:
	case 192000u:
		*code_out = 6u;
		return ALP_OK;
	default:
		return ALP_ERR_OUT_OF_RANGE;
	}
}

/* RX_WLEN[1:0] and RX_SLEN[1:0] -- §7.5.10 Table 7-110, p.70.  Slot
 * length is derived from word length because alp_i2s_config_t has no
 * separate slot field; 20-bit words have no 20-bit slot encoding, so
 * they ride in the next size up (24 bits). */
static alp_status_t word_len_codes(uint8_t bits, uint8_t *wlen_out, uint8_t *slen_out)
{
	switch (bits) {
	case 16u:
		*wlen_out = 0u;
		*slen_out = 0u;
		return ALP_OK;
	case 20u:
		*wlen_out = 1u;
		*slen_out = 1u;
		return ALP_OK;
	case 24u:
		*wlen_out = 2u;
		*slen_out = 1u;
		return ALP_OK;
	case 32u:
		*wlen_out = 3u;
		*slen_out = 2u;
		return ALP_OK;
	default:
		return ALP_ERR_OUT_OF_RANGE;
	}
}

alp_status_t tas2563_configure_i2s(tas2563_t              *ctx,
                                   const alp_i2s_config_t *host_cfg,
                                   tas2563_rx_channel_t    channel)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (host_cfg == NULL) return ALP_ERR_INVAL;
	if (channel != TAS2563_RX_SLOT_FROM_ADDR && channel != TAS2563_RX_LEFT &&
	    channel != TAS2563_RX_RIGHT && channel != TAS2563_RX_DOWNMIX) {
		return ALP_ERR_INVAL;
	}

	/* RX_OFFSET / RX_JUSTIFY -- §7.5.9 Table 7-109, p.69.  I2S
	 * delays the first data bit by one SBCLK after the frame edge;
	 * left-justified does not.  Both are left-justified within the
	 * slot, so RX_JUSTIFY stays 0 in either case. */
	uint8_t rx_offset;
	switch (host_cfg->format) {
	case ALP_I2S_FMT_I2S:
		rx_offset = 1u;
		break;
	case ALP_I2S_FMT_LEFT_JUSTIFIED:
		rx_offset = 0u;
		break;
	default:
		return ALP_ERR_NOSUPPORT;
	}

	uint8_t      rate_code = 0;
	alp_status_t s         = samp_rate_code(host_cfg->sample_rate_hz, &rate_code);
	if (s != ALP_OK) return s;

	uint8_t wlen = 0, slen = 0;
	s = word_len_codes(host_cfg->word_bits, &wlen, &slen);
	if (s != ALP_OK) return s;

	s = select_page(ctx, 0);
	if (s != ALP_OK) return s;

	s = reg_update(ctx,
	               TAS2563_REG_TDM_CFG0,
	               TAS2563_TDM_CFG0_SAMP_RATE_MASK,
	               (uint8_t)(rate_code << TAS2563_TDM_CFG0_SAMP_RATE_SHIFT));
	if (s != ALP_OK) return s;

	s = reg_update(ctx,
	               TAS2563_REG_TDM_CFG1,
	               TAS2563_TDM_CFG1_RX_FRAMING_FIELD,
	               (uint8_t)(rx_offset << TAS2563_TDM_CFG1_RX_OFFSET_SHIFT));
	if (s != ALP_OK) return s;

	return reg_update(ctx,
	                  TAS2563_REG_TDM_CFG2,
	                  TAS2563_TDM_CFG2_RX_FIELDS,
	                  (uint8_t)(((uint8_t)channel << TAS2563_TDM_CFG2_SCFG_SHIFT) |
	                            (uint8_t)(wlen << TAS2563_TDM_CFG2_WLEN_SHIFT) |
	                            (uint8_t)(slen << TAS2563_TDM_CFG2_SLEN_SHIFT)));
}

alp_status_t tas2563_configure_iv_sense(tas2563_t *ctx, bool enable, uint8_t v_slot, uint8_t i_slot)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (enable && (v_slot > TAS2563_TDM_SENSE_SLOT_MASK || i_slot > TAS2563_TDM_SENSE_SLOT_MASK)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;

	if (enable) {
		/* Power the sense blocks up first (ISNS_PD/VSNS_PD are
		 * reset HIGH = powered down, §7.5.4 Table 7-104, p.66),
		 * then hand each measurement a transmit slot -- never a
		 * slot enabled against a powered-down block. */
		s = reg_update(ctx, TAS2563_REG_PWR_CTL, TAS2563_PWR_CTL_SENSE_PD, 0u);
		if (s != ALP_OK) return s;
		s = reg_update(ctx,
		               TAS2563_REG_TDM_CFG5,
		               TAS2563_TDM_SENSE_FIELD,
		               (uint8_t)(TAS2563_TDM_SENSE_TX | v_slot));
		if (s != ALP_OK) return s;
		return reg_update(ctx,
		                  TAS2563_REG_TDM_CFG6,
		                  TAS2563_TDM_SENSE_FIELD,
		                  (uint8_t)(TAS2563_TDM_SENSE_TX | i_slot));
	}

	/* Tearing down runs the other way: stop transmitting, then power
	 * the blocks down.  Slot numbers are left as they were. */
	s = reg_update(ctx, TAS2563_REG_TDM_CFG5, TAS2563_TDM_SENSE_TX, 0u);
	if (s != ALP_OK) return s;
	s = reg_update(ctx, TAS2563_REG_TDM_CFG6, TAS2563_TDM_SENSE_TX, 0u);
	if (s != ALP_OK) return s;
	return reg_update(ctx, TAS2563_REG_PWR_CTL, TAS2563_PWR_CTL_SENSE_PD, TAS2563_PWR_CTL_SENSE_PD);
}

/* ------------------------------------------------------------------ */
/* Fault pin (IRQ_N) + latched fault readback                          */
/* ------------------------------------------------------------------ */

alp_status_t
tas2563_configure_fault_pin(tas2563_t *ctx, alp_gpio_t *irq_n, bool chip_internal_pullup)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (irq_n == NULL) return ALP_ERR_INVAL;

	/* IRQ_N is open drain and only ever pulls low (§7.3.12
	 * Figure 7-10, p.36), so the host side needs a pull-up to see a
	 * released pin as high. */
	alp_status_t s = alp_gpio_configure(irq_n, ALP_GPIO_INPUT, ALP_GPIO_PULL_UP);
	if (s != ALP_OK) return s;

	s = select_page(ctx, 0);
	if (s != ALP_OK) return s;

	s = reg_update(ctx,
	               TAS2563_REG_MISC_CFG1,
	               TAS2563_MISC_CFG1_IRQZ_PU,
	               chip_internal_pullup ? TAS2563_MISC_CFG1_IRQZ_PU : 0u);
	if (s != ALP_OK) return s;

	s = reg_update(
	    ctx, TAS2563_REG_INT_CLK, TAS2563_INT_CLK_PIN_CFG_MASK, TAS2563_INT_CLK_PIN_CFG_LATCH);
	if (s != ALP_OK) return s;

	ctx->irq_n = irq_n;
	return ALP_OK;
}

alp_status_t tas2563_fault_asserted(tas2563_t *ctx, bool *asserted_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (asserted_out == NULL) return ALP_ERR_INVAL;
	if (ctx->irq_n == NULL) return ALP_ERR_NOSUPPORT;

	bool         level = false;
	alp_status_t s     = alp_gpio_read(ctx->irq_n, &level);
	if (s != ALP_OK) return s;
	*asserted_out = !level; /* Active low (§7.3.12 Table 7-13, p.37). */
	return ALP_OK;
}

alp_status_t tas2563_read_faults(tas2563_t *ctx, uint32_t *faults_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (faults_out == NULL) return ALP_ERR_INVAL;

	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;

	/* Byte-aligned pass-through: INT_LTCH0/1/3/4 land in bytes
	 * 0/1/2/3 of the result, so every TAS2563_FAULT_* bit sits where
	 * the datasheet table puts it and nothing is re-encoded. */
	static const uint8_t ltch_regs[4] = {
		TAS2563_REG_INT_LTCH0, TAS2563_REG_INT_LTCH1, TAS2563_REG_INT_LTCH3, TAS2563_REG_INT_LTCH4
	};
	uint32_t faults = 0;
	for (unsigned i = 0; i < 4u; ++i) {
		uint8_t v = 0;
		s         = reg_read(ctx, ltch_regs[i], &v);
		if (s != ALP_OK) return s;
		faults |= (uint32_t)v << (8u * i);
	}
	*faults_out = faults;
	return ALP_OK;
}

alp_status_t tas2563_clear_faults(tas2563_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	alp_status_t s = select_page(ctx, 0);
	if (s != ALP_OK) return s;
	return reg_update(ctx, TAS2563_REG_INT_CLK, TAS2563_INT_CLK_CLR_LTCH, TAS2563_INT_CLK_CLR_LTCH);
}

/* ------------------------------------------------------------------ */
/* Tuning-blob replay                                                  */
/* ------------------------------------------------------------------ */

/* A record is rejected if it would write one of this function's own
 * paging registers, or PWR_CTL -- see tas2563_load_tuning()'s doc
 * comment for why the operating mode is off-limits to a blob. */
static bool tuning_record_is_legal(const tas2563_tuning_reg_t *r)
{
	if (r->reg < TAS2563_TUNING_REG_MIN || r->reg > TAS2563_TUNING_REG_MAX) return false;
	if (r->book == 0u && r->page == 0u && r->reg == TAS2563_REG_PWR_CTL) return false;
	return true;
}

alp_status_t tas2563_load_tuning(tas2563_t                  *ctx,
                                 const tas2563_tuning_reg_t *records,
                                 size_t                      count,
                                 size_t                     *failed_index_out)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (count == 0u) return ALP_OK;
	if (records == NULL) return ALP_ERR_INVAL;

	/* Validate the whole stream before touching the bus: a bad
	 * record 900 entries in must not leave 899 coefficients
	 * half-applied. */
	for (size_t i = 0; i < count; ++i) {
		if (!tuning_record_is_legal(&records[i])) {
			if (failed_index_out != NULL) *failed_index_out = i;
			return ALP_ERR_INVAL;
		}
	}

	alp_status_t s = select_book0_page0(ctx);
	if (s != ALP_OK) return s;

	uint8_t cur_book = 0, cur_page = 0;
	size_t  i = 0;
	for (; i < count; ++i) {
		const tas2563_tuning_reg_t *r = &records[i];

		if (r->book != cur_book) {
			/* BOOK is only reachable from page 0 of the
			 * current book (§7.5.62, p.94). */
			s = select_page(ctx, 0);
			if (s == ALP_OK) s = reg_write(ctx, TAS2563_REG_BOOK, r->book);
			if (s != ALP_OK) break;
			cur_book = r->book;
			cur_page = 0;
		}
		if (r->page != cur_page) {
			s = select_page(ctx, r->page);
			if (s != ALP_OK) break;
			cur_page = r->page;
		}
		s = reg_write(ctx, r->reg, r->val);
		if (s != ALP_OK) break;
	}

	if (s != ALP_OK) {
		if (failed_index_out != NULL) *failed_index_out = i;
		(void)select_book0_page0(ctx); /* Leave the map where callers expect it. */
		return s;
	}
	return select_book0_page0(ctx);
}

void tas2563_deinit(tas2563_t *ctx)
{
	if (ctx == NULL) return;
	if (ctx->initialised) {
		if (ctx->sd_n != NULL) {
			(void)alp_gpio_write(ctx->sd_n, false); /* HW shutdown on close */
		} else {
			/* No hardware line to drop -- fall back to software
			 * shutdown rather than leave the Class-D running. */
			if (select_page(ctx, 0) == ALP_OK) {
				(void)reg_update(
				    ctx, TAS2563_REG_PWR_CTL, TAS2563_PWR_CTL_MODE_MASK, TAS2563_MODE_SHUTDOWN);
			}
		}
	}
	ctx->initialised = false;
	ctx->bus         = NULL;
	ctx->irq_n       = NULL;
}
