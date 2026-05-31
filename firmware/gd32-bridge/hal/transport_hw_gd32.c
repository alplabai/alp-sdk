/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge firmware: GigaDevice silicon bring-up for the SPI + I2C
 * slave transports (gd32 HAL backend only).
 *
 * The portable transports (src/transport_spi.c, src/transport_i2c.c)
 * own framing/CRC/dispatch and expose byte-level seams.  This file
 * provides:
 *   - the strong bridge_transport_{spi,i2c}_hw_init() implementations
 *     (the stub backend keeps the weak no-op defaults), and
 *   - the SPI1 / EXTI / I2C0 interrupt handlers that override the weak
 *     vector-table symbols and feed the seams.
 *
 * Pin map + the UNCONFIRMED AF/timing facts live in
 * hal/bridge_board_config.h (see the warnings there).  Define
 * BRIDGE_WARN_UNCONFIRMED at compile time to surface them as #warnings.
 *
 * ── SPI model ──────────────────────────────────────────────────────
 * SPI1 slave, mode 0, MSB-first, 8-bit, hardware NSS.  The peripheral
 * gives no CS-edge interrupt, so CS (PA8) is also mirrored onto EXTI8
 * (both edges).  Full-duplex: every byte the master clocks in raises
 * RBNE; we read it into the RX seam and immediately load the next reply
 * byte for the byte that follows.  The first reply byte is preloaded on
 * the CS falling edge.  Request and reply ride separate CS transactions
 * (see ../src/transport_spi.c) — the reply staged at CS-rising is
 * clocked out by the host's next transaction.
 *
 * ── I2C model ──────────────────────────────────────────────────────
 * I2C0 slave at GD32_BRIDGE_DEFAULT_I2C_ADDR.  Write phase accumulates
 * bytes (RBNE); the reply is computed at the repeated-START read
 * (ADDSEND with TR=transmitter) or at STOP after a write, with SCL
 * clock-stretching covering protocol_dispatch().  TI clocks the reply.
 *
 * NOTE: the byte-level interrupt timing (SPI reply preload latency, I2C
 * clock-stretch window) needs validation on real silicon — flash
 * externally (host-driven SWD reflash is not wired in this HW rev).
 */

#include <stdbool.h>
#include <stdint.h>

#include "gd32g5x3.h"

#include "bridge_board_config.h"
#include "protocol.h"   /* GD32_BRIDGE_DEFAULT_I2C_ADDR */
#include "transport.h"  /* the seams we drive */

/* =================================================================== */
/* SPI slave bring-up                                                   */
/* =================================================================== */

static void spi_gpio_init(void)
{
    /* SCK / MISO / MOSI / NSS all to alternate-function, push-pull,
     * high speed.  NSS stays in AF for the SPI hardware-NSS input; EXTI
     * taps the same pin's input level for CS-edge detection. */
    const uint32_t af = BRIDGE_SPI_GPIO_AF;

    gpio_mode_set(BRIDGE_SPI_SCK_PORT,  GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_SCK_PIN);
    gpio_mode_set(BRIDGE_SPI_MISO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_MISO_PIN);
    gpio_mode_set(BRIDGE_SPI_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_MOSI_PIN);
    gpio_mode_set(BRIDGE_SPI_NSS_PORT,  GPIO_MODE_AF, GPIO_PUPD_PULLUP, BRIDGE_SPI_NSS_PIN);

    gpio_output_options_set(BRIDGE_SPI_SCK_PORT,  GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, BRIDGE_SPI_SCK_PIN);
    gpio_output_options_set(BRIDGE_SPI_MISO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, BRIDGE_SPI_MISO_PIN);
    gpio_output_options_set(BRIDGE_SPI_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, BRIDGE_SPI_MOSI_PIN);
    gpio_output_options_set(BRIDGE_SPI_NSS_PORT,  GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, BRIDGE_SPI_NSS_PIN);

    gpio_af_set(BRIDGE_SPI_SCK_PORT,  af, BRIDGE_SPI_SCK_PIN);
    gpio_af_set(BRIDGE_SPI_MISO_PORT, af, BRIDGE_SPI_MISO_PIN);
    gpio_af_set(BRIDGE_SPI_MOSI_PORT, af, BRIDGE_SPI_MOSI_PIN);
    gpio_af_set(BRIDGE_SPI_NSS_PORT,  af, BRIDGE_SPI_NSS_PIN);
}

static void spi_cs_exti_init(void)
{
    rcu_periph_clock_enable(RCU_SYSCFG);
    syscfg_exti_line_config(BRIDGE_SPI_CS_EXTI_PORT, BRIDGE_SPI_CS_EXTI_PIN);
    exti_init(BRIDGE_SPI_CS_EXTI_LINE, EXTI_INTERRUPT, EXTI_TRIG_BOTH);
    exti_interrupt_flag_clear(BRIDGE_SPI_CS_EXTI_LINE);
    nvic_irq_enable(BRIDGE_SPI_CS_EXTI_IRQN, BRIDGE_CS_IRQ_PRIO, BRIDGE_CS_IRQ_SUBPRIO);
}

/* Cached DMA buffers (owned by transport_spi.c). The RX buffer receives the
 * request; the TX buffer is the full frame, pre-padded with 0xFF past the
 * staged reply, so a fixed-length TX DMA clocks the reply then idle bytes. */
static uint8_t *s_spi_rx;
static size_t   s_spi_rx_cap;
static uint8_t *s_spi_tx;
static size_t   s_spi_tx_len;

static void spi_dma_init(void)
{
    rcu_periph_clock_enable(BRIDGE_SPI_DMA_RCU);
    s_spi_rx = spi_slave_rx_dma_buf(&s_spi_rx_cap);
    s_spi_tx = spi_slave_tx_dma_buf(&s_spi_tx_len);

    dma_parameter_struct rx;
    dma_deinit(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    rx.periph_addr  = (uint32_t)(uintptr_t)&SPI_DATA(BRIDGE_SPI_PERIPH);
    rx.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    rx.memory_addr  = (uint32_t)(uintptr_t)s_spi_rx;
    rx.memory_width = DMA_MEMORY_WIDTH_8BIT;
    rx.number       = (uint32_t)s_spi_rx_cap;
    rx.priority     = DMA_PRIORITY_HIGH;
    rx.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    rx.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    rx.direction    = DMA_PERIPHERAL_TO_MEMORY;
    rx.request      = BRIDGE_SPI_RX_DMA_REQ;
    dma_init(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH, &rx);

    dma_parameter_struct tx;
    dma_deinit(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    tx.periph_addr  = (uint32_t)(uintptr_t)&SPI_DATA(BRIDGE_SPI_PERIPH);
    tx.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    tx.memory_addr  = (uint32_t)(uintptr_t)s_spi_tx;
    tx.memory_width = DMA_MEMORY_WIDTH_8BIT;
    tx.number       = (uint32_t)s_spi_tx_len;
    tx.priority     = DMA_PRIORITY_HIGH;
    tx.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    tx.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    tx.direction    = DMA_MEMORY_TO_PERIPHERAL;
    tx.request      = BRIDGE_SPI_TX_DMA_REQ;
    dma_init(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH, &tx);
}

/* Re-arm both channels for one transaction (addresses are fixed; only the
 * transfer count needs resetting + the channels re-enabled). */
static void spi_dma_arm(void)
{
    dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    dma_transfer_number_config(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH, (uint32_t)s_spi_rx_cap);
    dma_transfer_number_config(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH, (uint32_t)s_spi_tx_len);
    dma_channel_enable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dma_channel_enable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
}

void bridge_transport_spi_hw_init(void)
{
    rcu_periph_clock_enable(BRIDGE_SPI_RCU);
    spi_gpio_init();

    spi_parameter_struct sp;
    spi_struct_para_init(&sp);
    sp.device_mode          = SPI_SLAVE;
    sp.trans_mode           = SPI_TRANSMODE_FULLDUPLEX;
    sp.frame_size           = SPI_FRAMESIZE_8BIT;
    sp.nss                  = SPI_NSS_HARD;
    sp.endian               = SPI_ENDIAN_MSB;
    sp.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;   /* mode 0 */
    spi_init(BRIDGE_SPI_PERIPH, &sp);

    /* DMA-driven: RX + TX DMA move the bytes (no per-byte ISR), so the
     * slave can run near the 27 MHz SPI ceiling. The channels are armed
     * per transaction from the CS-EXTI handler below. */
    spi_dma_init();
    spi_dma_enable(BRIDGE_SPI_PERIPH, SPI_DMA_RECEIVE);
    spi_dma_enable(BRIDGE_SPI_PERIPH, SPI_DMA_TRANSMIT);

    spi_enable(BRIDGE_SPI_PERIPH);
    spi_cs_exti_init();
}

/* CS edge: PA8 on EXTI8.  Falling = select: reset framing + arm RX/TX DMA
 * (TX preloads the staged reply via DMA).  Rising = end of transaction:
 * the bytes the master clocked = cap - DMA-remaining; decode + stage reply.
 *
 * Timing: the master's CS-to-first-SCK setup must exceed the EXTI + arm
 * latency, or the first byte(s) clock before DMA is armed.  Validate on
 * silicon (most masters have a programmable CS setup delay). */
void BRIDGE_SPI_CS_EXTI_HANDLER(void)
{
    if (RESET != exti_interrupt_flag_get(BRIDGE_SPI_CS_EXTI_LINE)) {
        exti_interrupt_flag_clear(BRIDGE_SPI_CS_EXTI_LINE);
        if (RESET == gpio_input_bit_get(BRIDGE_SPI_NSS_PORT, BRIDGE_SPI_NSS_PIN)) {
            spi_slave_cs_low();
            spi_dma_arm();
        } else {
            const uint32_t remaining =
                dma_transfer_number_get(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
            dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
            dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
            spi_slave_dma_frame_done(s_spi_rx_cap - (size_t)remaining);
        }
    }
}

/* =================================================================== */
/* I2C slave bring-up                                                   */
/* =================================================================== */

static void i2c_gpio_init(void)
{
    const uint32_t af = BRIDGE_I2C_GPIO_AF;

    /* Open-drain; rely on the BRD_I2C bus pull-ups. */
    gpio_mode_set(BRIDGE_I2C_SCL_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_I2C_SCL_PIN);
    gpio_mode_set(BRIDGE_I2C_SDA_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_I2C_SDA_PIN);
    gpio_output_options_set(BRIDGE_I2C_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, BRIDGE_I2C_SCL_PIN);
    gpio_output_options_set(BRIDGE_I2C_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, BRIDGE_I2C_SDA_PIN);
    gpio_af_set(BRIDGE_I2C_SCL_PORT, af, BRIDGE_I2C_SCL_PIN);
    gpio_af_set(BRIDGE_I2C_SDA_PORT, af, BRIDGE_I2C_SDA_PIN);
}

void bridge_transport_i2c_hw_init(void)
{
    rcu_i2c_clock_config(BRIDGE_I2C_RCU_IDX, BRIDGE_I2C_CK_SRC);
    rcu_periph_clock_enable(BRIDGE_I2C_RCU);
    i2c_gpio_init();

    /* Compute the I2C timing from the live APB1 kernel clock (set as the
     * I2C source just above) so it tracks whatever SystemInit configured
     * -- the GD32G553 core/APB clock has historically been ambiguous.
     * For a SLAVE only the prescaler + data setup (SCLDEL) + data hold
     * (SDADEL) matter; the SCL high/low periods (the 400 kHz bus rate)
     * are driven by the master, not us.  Target a ~8 MHz timing tick;
     * SCLDEL=2 / SDADEL=1 then give Fast-mode setup/hold, with the analog
     * filter on for Fm (400 kHz) spike suppression. */
    uint32_t apb1_hz = rcu_clock_freq_get(CK_APB1);
    uint32_t psc     = apb1_hz / 8000000u;   /* tick ~= (psc+1)/apb1 */
    if (psc > 0u)  { psc -= 1u; }
    if (psc > 15u) { psc = 15u; }            /* TIMING_PSC is a 4-bit field */
    i2c_timing_config(BRIDGE_I2C_PERIPH, psc, 2u /*scl_dely*/, 1u /*sda_dely*/);
    i2c_analog_noise_filter_enable(BRIDGE_I2C_PERIPH);

    i2c_address_config(BRIDGE_I2C_PERIPH,
                       (uint32_t)GD32_BRIDGE_DEFAULT_I2C_ADDR << 1,
                       I2C_ADDFORMAT_7BITS);
    i2c_stretch_scl_low_enable(BRIDGE_I2C_PERIPH);

    /* Address-match, receive, stop, NACK and error always on; the
     * transmit interrupt is enabled only while serving a read. */
    i2c_interrupt_enable(BRIDGE_I2C_PERIPH,
                         I2C_INT_ADDM | I2C_INT_RBNE | I2C_INT_STPDET |
                         I2C_INT_NACK | I2C_INT_ERR);
    nvic_irq_enable(BRIDGE_I2C_EV_IRQN, BRIDGE_I2C_IRQ_PRIO, BRIDGE_I2C_IRQ_SUBPRIO);
    nvic_irq_enable(BRIDGE_I2C_ER_IRQN, BRIDGE_I2C_IRQ_PRIO, BRIDGE_I2C_IRQ_SUBPRIO);

    i2c_enable(BRIDGE_I2C_PERIPH);
}

/* I2C0 event ISR: address match (direction-aware), RX during a write,
 * TX during a read, and STOP. */
void BRIDGE_I2C_EV_HANDLER(void)
{
    if (RESET != i2c_interrupt_flag_get(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_ADDSEND)) {
        const bool is_transmitter =
            (RESET != i2c_flag_get(BRIDGE_I2C_PERIPH, I2C_FLAG_TR));
        i2c_interrupt_flag_clear(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_ADDSEND);
        if (is_transmitter) {
            /* Repeated-START read: make sure the reply for the just-
             * received write is staged, then start clocking it out. */
            (void)i2c_slave_write_end();
            i2c_interrupt_enable(BRIDGE_I2C_PERIPH, I2C_INT_TI);
        } else {
            i2c_slave_write_start();
        }
    } else if (RESET != i2c_interrupt_flag_get(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_RBNE)) {
        i2c_slave_rx_byte((uint8_t)i2c_data_receive(BRIDGE_I2C_PERIPH));
    } else if (RESET != i2c_interrupt_flag_get(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_TI)) {
        i2c_data_transmit(BRIDGE_I2C_PERIPH, i2c_slave_tx_next_byte());
    } else if (RESET != i2c_interrupt_flag_get(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_STPDET)) {
        i2c_interrupt_flag_clear(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_STPDET);
        i2c_interrupt_disable(BRIDGE_I2C_PERIPH, I2C_INT_TI);
        /* STOP after a write with no read: stage the reply so a later
         * separate read transaction can fetch it. */
        (void)i2c_slave_write_end();
    }
}

/* I2C0 error ISR: clear NACK + bus errors so the slave re-syncs. */
void BRIDGE_I2C_ER_HANDLER(void)
{
    if (RESET != i2c_interrupt_flag_get(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_NACK)) {
        i2c_interrupt_flag_clear(BRIDGE_I2C_PERIPH, I2C_INT_FLAG_NACK);
    }
}
