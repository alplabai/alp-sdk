/*
 * Copyright (c) 2025 Alif Semiconductor
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DRIVER_I2S_DW_H
#define __DRIVER_I2S_DW_H
/* Includes -----------------------------------------------------------------*/
#include <stdbool.h>

/*!< Number of bytes for 16/32bit resolution*/
#define I2S_16BIT_BUF_TYPE 2
#define I2S_32BIT_BUF_TYPE 4

/*!< Max Audio Resolution for Tx/Rx Channel */
#define I2S_RX_WORDSIZE 32
#define I2S_TX_WORDSIZE 32

/*!< FIFO Depth for Tx & Rx  */
#define I2S_FIFO_DEPTH 16

/* Register fields and masks */

/* I2S IER.IEN: Global Enable */
#define I2S_IER_IEN_Pos 0U
#define I2S_IER_IEN_Msk (0x1UL << I2S_IER_IEN_Pos)

/* I2S IRER.RXEN: RX block Enable */
#define I2S_IRER_RXEN_Pos 0U
#define I2S_IRER_RXEN_Msk (0x1UL << I2S_IRER_RXEN_Pos)

/* I2S ITER.TXEN: TX block Enable */
#define I2S_ITER_TXEN_Pos 0U
#define I2S_ITER_TXEN_Msk (0x1UL << I2S_ITER_TXEN_Pos)

/* I2S CER.CLKEN: Clock Enable */
#define I2S_CER_CLKEN_Pos 0U
#define I2S_CER_CLKEN_Msk (0x1UL << I2S_CER_CLKEN_Pos)

/* I2S CCR.SCLKG: Clock Gating */
#define I2S_CCR_SCLKG_Pos 0U
#define I2S_CCR_SCLKG_Msk (0x7UL << I2S_CCR_SCLKG_Pos)

/* I2S CCR.WSS: Word Select length */
#define I2S_CCR_WSS_Pos 3U
#define I2S_CCR_WSS_Msk (0x3UL << I2S_CCR_WSS_Pos)

/* I2S RXFFR.RXFFR: Rx Block FIFO Reset */
#define I2S_RXFFR_RXFFR_Pos 0U
#define I2S_RXFFR_RXFFR_Msk (0x1UL << I2S_RXFFR_RXFFR_Pos)

/* I2S TXFFR.TXFFR: Tx Block FIFO Reset */
#define I2S_TXFFR_TXFFR_Pos 0U
#define I2S_TXFFR_TXFFR_Msk (0x1UL << I2S_TXFFR_TXFFR_Pos)

/* I2S RER.RXCHEN: Rx Channel Enable
 *
 * alp-sdk issue #2205: on the Alif E8 this bit is RXCHENX, and it CANNOT be
 * cleared. The E8 SVD (peripheral LPI2S, which i2s0..i2s3 derive from) gives
 * I2S_RER0 a reset value of 0x00FFFF01 with RXCHENX [0:0] read-only, "always
 * enabled" when the block is built with TDM support; bits 8-23 are the
 * per-slot enables RXSLOT0..15_EN. Measured on e1m-aen-evk-03 i2s3
 * (0x49017000) with the block idle (IER 0x00000F00, IRER/ITER/CER 0): RER
 * still read 0x00FFFF01 right after i2s_rx_channel_disable(). The E7 SVD
 * has RER0 reset 0x00000001 with the bit read-write and no slot fields.
 * IRER.RXEN (or IER.IEN) is what actually stops the RX direction on both.
 */
#define I2S_RER_RXCHEN_Pos 0U
#define I2S_RER_RXCHEN_Msk (0x1UL << I2S_RER_RXCHEN_Pos)

/* I2S TER.TXCHEN: Tx Channel Enable
 *
 * alp-sdk issue #2205: TXCHENX on the E8, the same as RER.RXCHEN above --
 * I2S_TER0 resets to 0x00FFFF01, bit 0 is read-only, bits 8-23 are the
 * per-slot enables TXSLOT0..15_EN. Measured on e1m-aen-evk-03 i2s3: TER
 * still read 0x00FFFF01 right after i2s_tx_channel_disable(). ITER.TXEN (or
 * IER.IEN) is what actually stops the TX direction.
 */
#define I2S_TER_TXCHEN_Pos 0U
#define I2S_TER_TXCHEN_Msk (0x1UL << I2S_TER_TXCHEN_Pos)

/* I2S RCR.WLEN: Data Resolution of Rx */
#define I2S_RCR_WLEN_Pos 0U
#define I2S_RCR_WLEN_Msk (0x7UL << I2S_RCR_WLEN_Pos)

/* I2S TCR.WLEN: Data Resolution of Tx */
#define I2S_TCR_WLEN_Pos 0U
#define I2S_TCR_WLEN_Msk (0x7UL << I2S_TCR_WLEN_Pos)

/* I2S ISR.RXDA: Status of Rx Data Available interrupt */
#define I2S_ISR_RXDA_Pos 0U
#define I2S_ISR_RXDA_Msk (0x1UL << I2S_ISR_RXDA_Pos)

/* I2S ISR.RXFO: Status of Data Overrun interrupt of Rx */
#define I2S_ISR_RXFO_Pos 1U
#define I2S_ISR_RXFO_Msk (0x1UL << I2S_ISR_RXFO_Pos)

/* I2S ISR.TXFE: Status of Tx Empty Trigger Interrupt */
#define I2S_ISR_TXFE_Pos 4U
#define I2S_ISR_TXFE_Msk (0x1UL << I2S_ISR_TXFE_Pos)

/* I2S ISR.TXFO: Status of Data Overrun Interrupt for Tx */
#define I2S_ISR_TXFO_Pos 5U
#define I2S_ISR_TXFO_Msk (0x1UL << I2S_ISR_TXFO_Pos)

/* I2S ISR.TXFU: Status of Tx FIFO Underrun Interrupt (alp-sdk issue #2205).
 * E8 SVD only (I2S_ISR0 [6:6]); the E7 SVD defines no bit 6. ISR bits 2-3
 * are undefined in both SVDs. */
#define I2S_ISR_TXFU_Pos 6U
#define I2S_ISR_TXFU_Msk (0x1UL << I2S_ISR_TXFU_Pos)

/* I2S IMR.RXDAM: Mask Rx Data Available interrupt */
#define I2S_IMR_RXDAM_Pos 0U
#define I2S_IMR_RXDAM_Msk (0x1UL << I2S_IMR_RXDAM_Pos)

/* I2S IMR.RXFOM: Mask Data Overrun interrupt of Rx */
#define I2S_IMR_RXFOM_Pos 1U
#define I2S_IMR_RXFOM_Msk (0x1UL << I2S_IMR_RXFOM_Pos)

/* I2S IMR.TXFEM: Mask Tx Empty Interrupt */
#define I2S_IMR_TXFEM_Pos 4U
#define I2S_IMR_TXFEM_Msk (0x1UL << I2S_IMR_TXFEM_Pos)

/* I2S IMR.TXFOM: Mask Data Overrun Interrupt for Tx */
#define I2S_IMR_TXFOM_Pos 5U
#define I2S_IMR_TXFOM_Msk (0x1UL << I2S_IMR_TXFOM_Pos)

/* I2S IMR.TXFUM: Mask Tx FIFO Underrun Interrupt (alp-sdk issue #2205).
 * E8 SVD only (I2S_IMR0 [6:6], reset 0x00000073); the E7 SVD resets IMR0
 * to 0x00000033 and defines no bit 6. */
#define I2S_IMR_TXFUM_Pos 6U
#define I2S_IMR_TXFUM_Msk (0x1UL << I2S_IMR_TXFUM_Pos)

/* Every named IMR bit: RXDAM, RXFOM, TXFEM, TXFOM, TXFUM (0x73). Bits 2-3
 * have no field in either the E7 or the E8 SVD, so no 1 is ever written
 * there. This equals the E8's IMR reset value, i.e. "everything masked". */
#define I2S_IMR_ALL_Msk \
	(I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk | I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk | \
	 I2S_IMR_TXFUM_Msk)

/* I2S ROR.RXCHO: Clear Rx Data Overrun interrupt */
#define I2S_ROR_RXCHO_Pos 0U
#define I2S_ROR_RXCHO_Msk (0x1UL << I2S_ROR_RXCHO_Pos)

/* I2S TOR.TXCHO: Clear Tx Data Overrun interrupt */
#define I2S_TOR_TXCHO_Pos 0U
#define I2S_TOR_TXCHO_Msk (0x1UL << I2S_TOR_TXCHO_Pos)

/* I2S RFCR.RXCHDT: Rx FIFO Trigger Level */
#define I2S_RFCR_RXCHDT_Pos 0U
#define I2S_RFCR_RXCHDT_Msk (0xFUL << I2S_RFCR_RXCHDT_Pos)

/* I2S TFCR.TXCHET: Tx FIFO Trigger Level */
#define I2S_TFCR_TXCHET_Pos 0U
#define I2S_TFCR_TXCHET_Msk (0xFUL << I2S_TFCR_TXCHET_Pos)

/* I2S RFF.RXCHFR: Rx Channel FIFO Reset */
#define I2S_RFF_RXCHFR_Pos 0U
#define I2S_RFF_RXCHFR_Msk (0x1UL << I2S_RFF_RXCHFR_Pos)

/* I2S TFF.TXCHFR: Tx Channel FIFO Reset */
#define I2S_TFF_TXCHFR_Pos 0U
#define I2S_TFF_TXCHFR_Msk (0x1UL << I2S_TFF_TXCHFR_Pos)

/* I2S DMACR.DMAEN_RXBLOCK: DMA Enable for Rx Block */
#define I2S_DMACR_DMAEN_RXBLOCK_Pos 16U
#define I2S_DMACR_DMAEN_RXBLOCK_Msk (0x1UL << I2S_DMACR_DMAEN_RXBLOCK_Pos)

/* I2S DMACR.DMAEN_TXBLOCK: DMA Enable for Tx Block */
#define I2S_DMACR_DMAEN_TXBLOCK_Pos 17U
#define I2S_DMACR_DMAEN_TXBLOCK_Msk (0x1UL << I2S_DMACR_DMAEN_TXBLOCK_Pos)

/* Enums */
typedef enum {
	WSS_CLOCK_CYCLES_16 = 0, /*!< 16 sclk cycles */
	WSS_CLOCK_CYCLES_24,     /*!< 24 sclk cycles */
	WSS_CLOCK_CYCLES_32,     /*!< 32 sclk cycles */

	WSS_CLOCK_CYCLES_MAX
} I2S_WSS_Type;

typedef enum {
	NO_CLOCK_GATING = 0,   /*!< Gating is disabled */
	SCLKG_CLOCK_CYCLES_12, /*!< Gating after 12 sclk cycles */
	SCLKG_CLOCK_CYCLES_16, /*!< Gating after 16 sclk cycles */
	SCLKG_CLOCK_CYCLES_20, /*!< Gating after 20 sclk cycles */
	SCLKG_CLOCK_CYCLES_24, /*!< Gating after 24 sclk cycles */

	SCLKG_CLOCK_CYCLES_MAX
} I2S_SCLKG_Type;

typedef enum {
	I2S_FLAG_CLKSRC_ENABLED = (1U << 0),  /*!< I2S ClockSource Status */
	I2S_FLAG_DRV_INIT_DONE = (1U << 1),   /*!< I2S Driver is Initialized */
	I2S_FLAG_DRV_CONFIG_DONE = (1U << 2), /*!< I2S Driver is Configured */
	I2S_FLAG_DRV_MONO_MODE = (1U << 3),   /*!< I2S Driver in Mono Mode */
} I2S_FLAG_Type;

typedef enum {
	IGNORE_WLEN = 0, /*!< Word length ignored */
	RES_12_BIT,      /*!< 12 bit Data Resolution */
	RES_16_BIT,      /*!< 16 bit Data Resolution */
	RES_20_BIT,      /*!< 20 bit Data Resolution */
	RES_24_BIT,      /*!< 24 bit Data Resolution */
	RES_32_BIT,      /*!< 32 bit Data Resolution */
} I2S_WLEN_Type;

typedef enum {
	I2S_TRANSMITTER,
	I2S_RECEIVER,
} I2S_TRANSFER_Type;

typedef enum {
	I2S_FIFO_TRIGGER_LEVEL_1 = 0, /*!< I2S FIFO Trigger level 1  */
	I2S_FIFO_TRIGGER_LEVEL_2,     /*!< I2S FIFO Trigger level 2  */
	I2S_FIFO_TRIGGER_LEVEL_3,     /*!< I2S FIFO Trigger level 3  */
	I2S_FIFO_TRIGGER_LEVEL_4,     /*!< I2S FIFO Trigger level 4  */
	I2S_FIFO_TRIGGER_LEVEL_5,     /*!< I2S FIFO Trigger level 5  */
	I2S_FIFO_TRIGGER_LEVEL_6,     /*!< I2S FIFO Trigger level 6  */
	I2S_FIFO_TRIGGER_LEVEL_7,     /*!< I2S FIFO Trigger level 7  */
	I2S_FIFO_TRIGGER_LEVEL_8,     /*!< I2S FIFO Trigger level 8  */
	I2S_FIFO_TRIGGER_LEVEL_9,     /*!< I2S FIFO Trigger level 9  */
	I2S_FIFO_TRIGGER_LEVEL_10,    /*!< I2S FIFO Trigger level 10 */
	I2S_FIFO_TRIGGER_LEVEL_11,    /*!< I2S FIFO Trigger level 11 */
	I2S_FIFO_TRIGGER_LEVEL_12,    /*!< I2S FIFO Trigger level 12 */
	I2S_FIFO_TRIGGER_LEVEL_13,    /*!< I2S FIFO Trigger level 13 */
	I2S_FIFO_TRIGGER_LEVEL_14,    /*!< I2S FIFO Trigger level 14 */
	I2S_FIFO_TRIGGER_LEVEL_15,    /*!< I2S FIFO Trigger level 15 */
	I2S_FIFO_TRIGGER_LEVEL_16,    /*!< I2S FIFO Trigger level 16 */

	I2S_FIFO_TRIGGER_LEVEL_MAX
} I2S_FIFO_TRIGGER_LEVEL_Type;

struct I2S_Type {
	__IOM uint32_t IER;  /*!< Offset:0x0 I2S Global Enable */
	__IOM uint32_t IRER; /*!< Offset:0x4 I2S Rx Block Enable */
	__IOM uint32_t ITER; /*!< Offset:0x8 I2S Tx Block Enable */
	__IOM uint32_t CER;  /*!< Offset:0xC I2S Clock Enable */
	__IOM uint32_t CCR;  /*!< Offset:0x10 I2S Clock Configuration */

	__OM uint32_t RXFFR; /*!< Offset:0x14 I2S  Rx Block FIFO Reset */
	__OM uint32_t TXFFR; /*!< Offset:0x18 I2S  Tx Block FIFO Reset */
	uint32_t RESERVED0;
	union {
		__IM uint32_t LRBR; /*!< Offset:0x20, I2S Left Rx Buffer */
		__OM uint32_t LTHR; /*!< Offset:0x20, I2S Left Tx Holding */
	};
	union {
		__IM uint32_t RRBR; /*!< Offset:0x24, I2S Right Rx Buffer */
		__OM uint32_t RTHR; /*!< Offset:0x24, I2S Right Tx Holding */
	};
	__IOM uint32_t RER;  /*!< Offset:0x28, I2S Rx Channel Enable */
	__IOM uint32_t TER;  /*!< Offset:0x2C, I2S Tx Channel Enable */
	__IOM uint32_t RCR;  /*!< Offset:0x30, I2S Rx Configuration */
	__IOM uint32_t TCR;  /*!< Offset:0x34, I2S Tx Configuration */
	__IM uint32_t ISR;   /*!< Offset:0x38, I2S Interrupt Status */
	__IOM uint32_t IMR;  /*!< Offset:0x3C, I2S Interrupt Mask */
	__IM uint32_t ROR;   /*!< Offset:0x40, I2S Rx Overrun */
	/* Offset:0x44 -- TOR0 (TXCHO) on the E7 SVD. On the E8 SVD it is
	 * I2S_TICR0 (TXCHOU), and one read clears BOTH TXFO and TXFU. */
	__IM uint32_t TOR;   /*!< Offset:0x44, I2S Tx Overrun / TICR0 */
	__IOM uint32_t RFCR; /*!< Offset:0x48, I2S Rx FIFO Configuration */
	__IOM uint32_t TFCR; /*!< Offset:0x4C, I2S Tx FIFO Configuration */
	__OM uint32_t RFF;   /*!< Offset:0x50, I2S Rx Channel FIFO Reset */
	__OM uint32_t TFF;   /*!< Offset:0x54, I2S Tx Channel FIFO Reset */
	uint32_t RESERVED1[90];
	__IM uint32_t RXDMA; /*!< Offset:0x1C0, I2S Rx Block DMA */
	uint32_t RESERVED2;
	__OM uint32_t TXDMA; /*!< Offset:0x1C8, I2S Tx Block DMA */
	uint32_t RESERVED4[13];
	__IOM uint32_t DMACR; /*!< Offset:0x200, I2S DMA Control */
};

struct I2S_TX_BUFF {

	/*!< I2S tx buffer: can be 16/32 bit based on databits */
	const void *buf;

	/*!< I2S tx total buffer length in bytes */
	uint32_t total_len;

	/*!< I2S tx points to the current offset in the buffer in bytes */
	uint32_t ofs;
};

struct I2S_RX_BUFF {

	/*!< I2S rx buffer: can be 16/32 bit based on databits */
	void *buf;

	/*!< I2S rx total buffer length in bytes */
	uint32_t total_len;

	/*!< I2S rx points to the current offset in the buffer in bytes */
	uint32_t ofs;
};

struct I2S_CONFIG_INFO {
	uint32_t dir;

	/*!< I2S Audio Sample Rate */
	uint32_t sample_rate;

	/*!< I2S Word Select Size */
	I2S_WSS_Type wss_len;

	/*!< I2S SCLK Gating */
	I2S_SCLKG_Type sclkg;

	/*!< I2S word length */
	/* I2S_WLEN_Type wlen; */
	uint32_t wlen;

	/*!< I2S Rx FIFO Trigger Level */
	I2S_FIFO_TRIGGER_LEVEL_Type rx_fifo_trg_lvl;

	/*!< I2S Tx FIFO Trigger Level */
	I2S_FIFO_TRIGGER_LEVEL_Type tx_fifo_trg_lvl;

	/*!< I2S dma enable */
	const bool dma_enable;

	/*!< DMA IRQ priority number */
	const uint32_t dma_irq_priority;

	/*!< I2S irq priority number */
	const uint32_t irq_priority;

	uint32_t num_channels;

	uint32_t channel_length;
};

/* Device constant configuration parameters */
struct i2s_dw_cfg {
	struct I2S_CONFIG_INFO cfg;
	struct I2S_Type *paddr;
	uint32_t i2s_clk_sel;
	void (*irq_config)(const struct device *dev);
	const struct device *clk_dev;
	clock_control_subsys_t clkid;

#if defined(CONFIG_PINCTRL)
	const struct pinctrl_dev_config *pincfg;
#endif
};

/**
 * \brief             Control I2S Global Enable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_global_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->IER = _VAL2FLD(I2S_IER_IEN, 1U);
}

/**
 * \brief             Control I2S Global Disable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_global_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->IER &= ~I2S_IER_IEN_Msk;
}

/**
 * \brief             Control I2S Receiver Block Enable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_rx_block_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->IRER = _VAL2FLD(I2S_IRER_RXEN, 1U);
}

/**
 * \brief             Control I2S Receiver Block Disable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_rx_block_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->IRER &= ~I2S_IRER_RXEN_Msk;
}

/**
 * \brief             Control I2S Transmitter Block Enable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_tx_block_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->ITER = _VAL2FLD(I2S_ITER_TXEN, 1U);
}

/**
 * \brief             Control I2S Transmitter Block Disable
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_tx_block_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->ITER &= ~I2S_ITER_TXEN_Msk;
}

/**
 * \brief             Control I2S Clock Enable in Master Mode
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_clock_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->CER = _VAL2FLD(I2S_CER_CLKEN, 1U);
}

/**
 * \brief             Control I2S Clock Disable in Master Mode
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_clock_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->CER &= ~I2S_CER_CLKEN_Msk;
}

/**
 * \brief             Query I2S Clock Enable state in Master Mode
 * \param[in]   i2s   Pointer to I2S resources
 * \return            true if CER.CLKEN is set
 *
 * alp-sdk issue #2149 (round 2): paired with struct stream's own one-shot
 * clk_restart_skip_ok flag in tx_stream_start()'s restart-glitch guard --
 * i2s_configure_clock() (CCR) below is documented "Should be called with
 * Clock disabled", so a restart that finds the clock already running for a
 * flag-confirmed reason (the underrun ISR path deliberately keeps
 * CER.CLKEN set, and every path that could invalidate that clears the flag
 * again) skips that write instead of reprogramming CCR live. This query
 * alone is not sufficient -- see the flag's own comment for why.
 */
__STATIC_INLINE bool i2s_clock_is_enabled(const struct i2s_dw_cfg *i2s)
{
	return (i2s->paddr->CER & I2S_CER_CLKEN_Msk) != 0;
}

/**
 * \brief             Control I2S Configure WSS and SCLKG in Master Mode.
 *                    Should be called with Clock disabled.
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_configure_clock(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->CCR =
		_VAL2FLD(I2S_CCR_SCLKG, i2s->cfg.sclkg) | _VAL2FLD(I2S_CCR_WSS, i2s->cfg.wss_len);
}

/**
 * \brief             Read Left Receive Buffer Register
 * \param[in]   i2s   Pointer to I2S resources
 * \return            data
 */
__STATIC_INLINE uint32_t i2s_read_left_rx(const struct i2s_dw_cfg *i2s)
{
	return i2s->paddr->LRBR;
}

/**
 * \fn                void i2s_write_left_tx(uint32_t data,
 *                                           const struct i2s_dw_cfg *i2s)
 * \brief             Write to Left Transmit Holding Register
 * \param[in]   data  data to write
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_write_left_tx(uint32_t data, const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->LTHR = data;
}

/**
 * \fn                uint32_t i2s_read_right_rx(const struct i2s_dw_cfg *i2s)
 * \brief             Read from Right Receive Buffer Register
 * \param[in]   i2s   Pointer to I2S resources
 * \return      data
 */
__STATIC_INLINE uint32_t i2s_read_right_rx(const struct i2s_dw_cfg *i2s)
{
	return i2s->paddr->RRBR;
}

/**
 * \fn                void i2s_write_right_tx(uint32_t data,
 *                                            const struct i2s_dw_cfg *i2s)
 * \brief             Write to Right Transmit Holding Register
 * \param[in]   data  data to write
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_write_right_tx(uint32_t data, const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->RTHR = data;
}

/**
 * \fn                void i2s_rx_channel_enable(const struct i2s_dw_cfg *i2s)
 * \brief             Control I2S Receiver Channel Enable
 * \param[in]   i2s   Pointer to I2S resources
 *
 * alp-sdk issue #2205: read-modify-write. A plain write of 0x1 zeroed the
 * E8's per-slot enables in bits 8-23 (measured: RER 0x00FFFF01 ->
 * 0x00000001), harmless in standard I2S mode and wrong under TDM.
 */
__STATIC_INLINE void i2s_rx_channel_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->RER |= I2S_RER_RXCHEN_Msk;
}

/**
 * \fn                void i2s_rx_channel_disable(const struct i2s_dw_cfg *i2s)
 * \brief             Control I2S Receiver Channel Disable
 * \param[in]   i2s   Pointer to I2S resources
 *
 * alp-sdk issue #2205: a no-op on the E8 (RXCHENX is read-only, see
 * I2S_RER_RXCHEN_Msk); real on the E7. Callers must not rely on it to stop
 * RX -- i2s_rx_block_disable() does that.
 */
__STATIC_INLINE void i2s_rx_channel_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->RER &= ~I2S_RER_RXCHEN_Msk;
}

/**
 * \fn                void i2s_tx_channel_enable(const struct i2s_dw_cfg *i2s)
 * \brief             Control I2S Transmit Channel Enable
 * \param[in]   i2s   Pointer to I2S resources
 *
 * alp-sdk issue #2205: read-modify-write, for the same reason as
 * i2s_rx_channel_enable() -- TER bits 8-23 are the E8's TX slot enables.
 */
__STATIC_INLINE void i2s_tx_channel_enable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->TER |= I2S_TER_TXCHEN_Msk;
}

/**
 * \fn                void i2s_tx_channel_disable(const struct i2s_dw_cfg *i2s)
 * \brief             Control I2S Transmit Channel Disable
 * \param[in]   i2s   Pointer to I2S resources
 *
 * alp-sdk issue #2205: a no-op on the E8 (TXCHENX is read-only, see
 * I2S_TER_TXCHEN_Msk); real on the E7. Callers must not rely on it to stop
 * TX -- i2s_tx_block_disable() does that.
 */
__STATIC_INLINE void i2s_tx_channel_disable(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->TER &= ~I2S_TER_TXCHEN_Msk;
}

/**
 * \fn                void i2s_rx_config_wlen(const struct i2s_dw_cfg *i2s)
 * \brief             Set Wlen in Receive Configuration Register
 *                    Should be called with RXCHEN disabled.
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_rx_config_wlen(const struct i2s_dw_cfg *i2s, uint32_t wlen)
{
	I2S_WLEN_Type tmp = 0;

	switch (wlen) {
	case 12:
		tmp = RES_12_BIT;
		break;
	case 16:
		tmp = RES_16_BIT;
		break;
	case 20:
		tmp = RES_20_BIT;
		break;
	case 24:
		tmp = RES_24_BIT;
		break;
	case 32:
		tmp = RES_32_BIT;
		break;
	default:
		break;
	}
	i2s->paddr->RCR = _VAL2FLD(I2S_RCR_WLEN, tmp);
}

/**
 * \fn                void i2s_tx_config_wlen(const struct i2s_dw_cfg *i2s)
 * \brief             Set Wlen in Transmit Configuration Register
 *                    Should be called with TXCHEN disabled.
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_tx_config_wlen(const struct i2s_dw_cfg *i2s, uint32_t wlen)
{
	I2S_WLEN_Type tmp = 0;

	switch (wlen) {
	case 12:
		tmp = RES_12_BIT;
		break;
	case 16:
		tmp = RES_16_BIT;
		break;
	case 20:
		tmp = RES_20_BIT;
		break;
	case 24:
		tmp = RES_24_BIT;
		break;
	case 32:
		tmp = RES_32_BIT;
		break;
	default:
		break;
	}
	i2s->paddr->TCR = _VAL2FLD(I2S_TCR_WLEN, tmp);
}

/**
 * \fn                void i2s_set_interrupt_mask(uint32_t imr,
 *                                                const struct i2s_dw_cfg *i2s)
 * \brief             Set the Interrupt Mask Register for the channel
 * \param[in]   imr   Mask Value
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_set_interrupt_mask(uint32_t imr, const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->IMR = imr;
}

/**
 * \fn                void i2s_get_interrupt_mask(const struct i2s_dw_cfg *i2s)
 * \brief             Get the Interrupt Mask Register for the channel
 * \param[in]   i2s   Pointer to I2S resources
 * \return            Interrupt Mask value
 */
__STATIC_INLINE uint32_t i2s_get_interrupt_mask(const struct i2s_dw_cfg *i2s)
{
	return i2s->paddr->IMR;
}

/**
 * \fn                void i2s_clear_rx_overrun(const struct i2s_dw_cfg *i2s)
 * \brief             Clear Receiver FIFO Data Overrun Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_clear_rx_overrun(const struct i2s_dw_cfg *i2s)
{
	(void)(i2s->paddr->ROR);
}

/**
 * \fn                void i2s_clear_tx_overrun(const struct i2s_dw_cfg *i2s)
 * \brief             Clear Transmit FIFO Data Overrun Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 *
 * On the E8 this read of offset 0x44 (I2S_TICR0) also clears TXFU.
 */
__STATIC_INLINE void i2s_clear_tx_overrun(const struct i2s_dw_cfg *i2s)
{
	(void)(i2s->paddr->TOR);
}

/**
 * \fn                void i2s_set_rx_trigger_level(const
 *                                                  struct i2s_dw_cfg *i2s)
 * \brief             Program the Trigger Level in RxFIFO
 *                    The channel must be disabled before doing this
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_set_rx_trigger_level(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->RFCR = _VAL2FLD(I2S_RFCR_RXCHDT, i2s->cfg.rx_fifo_trg_lvl);
}

/**
 * \fn                void i2s_set_tx_trigger_level(const
 *                                                  struct i2s_dw_cfg *i2s)
 * \brief             Program the Trigger Level in TxFIFO
 *                    The channel must be disabled before doing this
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_set_tx_trigger_level(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->TFCR = _VAL2FLD(I2S_TFCR_TXCHET, i2s->cfg.tx_fifo_trg_lvl);
}

/**
 * \fn                void i2s_rx_fifo_reset(const struct i2s_dw_cfg *i2s)
 * \brief             Flush the Rx Channel FIFO
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_rx_fifo_reset(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->RFF = _VAL2FLD(I2S_RFF_RXCHFR, 1U);
}

/**
 * \fn                void i2s_tx_fifo_reset(const struct i2s_dw_cfg *i2s)
 * \brief             Flush the Tx Channel FIFO
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_tx_fifo_reset(const struct i2s_dw_cfg *i2s)
{
	i2s->paddr->TFF = _VAL2FLD(I2S_TFF_TXCHFR, 1U);
}

/**
 * \fn                void I2S_EnableTxInterrupt(const struct i2s_dw_cfg *i2s)
 * \brief             Enable I2S Tx Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_enable_tx_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr &= ~(I2S_IMR_TXFEM_Msk | I2S_IMR_TXFOM_Msk);
	i2s_set_interrupt_mask(imr, i2s);
}

/**
 * \fn                void i2s_disable_tx_interrupt(const
 *                                                  struct i2s_dw_cfg *i2s)
 * \brief             Disable I2S Tx Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_disable_tx_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr |= _VAL2FLD(I2S_IMR_TXFEM, 1U) | _VAL2FLD(I2S_IMR_TXFOM, 1U);
	i2s_set_interrupt_mask(imr, i2s);
}

/**
 * \fn                void I2S_EnableRxFOInterrupt(const struct i2s_dw_cfg *i2s)
 * \brief             Enable I2S Rx Overflow Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_enable_rx_fo_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr &= ~(I2S_IMR_RXFOM_Msk);
	i2s_set_interrupt_mask(imr, i2s);
}

/**
 * \fn                void i2s_enable_rx_interrupt(const struct i2s_dw_cfg *i2s)
 * \brief             Enable I2S Rx Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_enable_rx_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr &= ~(I2S_IMR_RXDAM_Msk | I2S_IMR_RXFOM_Msk);
	i2s_set_interrupt_mask(imr, i2s);
}

/**
 * \fn                void i2s_disable_rx_fo_interrupt(const
 *                                                     struct i2s_dw_cfg *i2s)
 * \brief             Disable I2S Rx Overflow Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_disable_rx_fo_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr |= _VAL2FLD(I2S_IMR_RXFOM, 1U);
	i2s_set_interrupt_mask(imr, i2s);
}

/**
 * \fn                void I2S_DisableRxInterrupt(const struct i2s_dw_cfg *i2s)
 * \brief             Disable I2S Rx Interrupt
 * \param[in]   i2s   Pointer to I2S resources
 */
__STATIC_INLINE void i2s_disable_rx_interrupt(const struct i2s_dw_cfg *i2s)
{
	uint32_t imr;

	imr = i2s_get_interrupt_mask(i2s);
	imr |= _VAL2FLD(I2S_IMR_RXDAM, 1U) | _VAL2FLD(I2S_IMR_RXFOM, 1U);
	i2s_set_interrupt_mask(imr, i2s);
}

#endif /* __DRIVER_I2S_DW_H */
