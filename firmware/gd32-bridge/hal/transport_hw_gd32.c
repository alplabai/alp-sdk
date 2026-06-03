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
 * ── SPI model (DMA, 25 MHz) ────────────────────────────────────────
 * SPI1 slave, mode 0, MSB-first, 8-bit frames, hardware NSS.  The link
 * runs at 25 MHz SCK (datasheet slave max 27 MHz): a byte lands every
 * 320 ns, far inside interrupt latency, so the data path is pure DMA --
 * RX DMA captures every clocked byte into a staging buffer and TX DMA
 * streams the staged reply out (the TBE request prefills the TX FIFO
 * the moment the channel is armed, replacing the old CS-falling
 * preload).  The only SPI-side interrupt left is the CS (PA8) edge pair
 * on EXTI8, which frames transactions:
 *
 *   CS falling: reset the portable RX staging (spi_slave_cs_low).
 *   CS rising:  read the RX residue (count = buffer - DMA remaining),
 *               flush + re-init the peripheral (RCU reset -- the only
 *               reliable FIFO flush), feed the received bytes through
 *               the byte seams, decode + stage the reply, then re-arm
 *               RX DMA (full buffer) and TX DMA (exact reply length).
 *
 * Request and reply ride separate CS transactions (../src/transport_spi.c);
 * the portable framing/CRC layer is untouched -- DMA is a drop-in
 * replacement for the per-byte RBNE interrupt at the seam boundary.
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
    /* SCK / MISO / MOSI / NSS all to alternate-function, push-pull.
     * 85 MHz drive class: at 25 MHz SCK the slave's MISO must be valid
     * within tV(SO)=9 ns of the sampling edge -- GigaDevice's own SPI DMA
     * examples use the 85 MHz class for exactly this reason (only MISO is
     * GD32-driven; the inputs' speed class is then a don't-care, set
     * uniformly for consistency).  NSS stays in AF for the SPI
     * hardware-NSS input; EXTI taps the same pin for CS-edge detection. */
    const uint32_t af = BRIDGE_SPI_GPIO_AF;

    gpio_mode_set(BRIDGE_SPI_SCK_PORT,  GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_SCK_PIN);
    gpio_mode_set(BRIDGE_SPI_MISO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_MISO_PIN);
    gpio_mode_set(BRIDGE_SPI_MOSI_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE, BRIDGE_SPI_MOSI_PIN);
    gpio_mode_set(BRIDGE_SPI_NSS_PORT,  GPIO_MODE_AF, GPIO_PUPD_PULLUP, BRIDGE_SPI_NSS_PIN);

    gpio_output_options_set(BRIDGE_SPI_SCK_PORT,  GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, BRIDGE_SPI_SCK_PIN);
    gpio_output_options_set(BRIDGE_SPI_MISO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, BRIDGE_SPI_MISO_PIN);
    gpio_output_options_set(BRIDGE_SPI_MOSI_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, BRIDGE_SPI_MOSI_PIN);
    gpio_output_options_set(BRIDGE_SPI_NSS_PORT,  GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, BRIDGE_SPI_NSS_PIN);

    gpio_af_set(BRIDGE_SPI_SCK_PORT,  af, BRIDGE_SPI_SCK_PIN);
    gpio_af_set(BRIDGE_SPI_MISO_PORT, af, BRIDGE_SPI_MISO_PIN);
    gpio_af_set(BRIDGE_SPI_MOSI_PORT, af, BRIDGE_SPI_MOSI_PIN);
    gpio_af_set(BRIDGE_SPI_NSS_PORT,  af, BRIDGE_SPI_NSS_PIN);
}

/* ── SPI slave DMA plumbing ─────────────────────────────────────────── */

/* HAL-side DMA staging.  RX captures up to one max wire envelope
 * (1 SOF + 1 CMD/STATUS + 65 payload + 2 CRC = 69 B; padded for margin --
 * anything the master over-clocks beyond this simply stops being captured
 * and the CRC check fails loud).  TX holds the staged reply drained from
 * the portable seams at decode time so the DMA has a stable flat buffer. */
#define BRIDGE_SPI_DMA_BUF_LEN 72u
static uint8_t spi_rx_dma_buf[BRIDGE_SPI_DMA_BUF_LEN];
static uint8_t spi_tx_dma_buf[BRIDGE_SPI_DMA_BUF_LEN];

/* One-time channel configuration (clocks, DMAMUX routing, widths).  The
 * per-transaction address/count reloads live in the arm helpers below;
 * everything here survives both the per-transaction SPI RCU reset (DMA and
 * DMAMUX are separate peripherals) and channel disable/enable cycles. */
static void spi_dma_init(void)
{
    dma_parameter_struct d;

    rcu_periph_clock_enable(BRIDGE_SPI_DMA_RCU);
    rcu_periph_clock_enable(RCU_DMAMUX);

    /* RX: SPI1 DATA -> spi_rx_dma_buf, byte-by-byte (BYTEN makes one 8-bit
     * peripheral access == one frame), memory incrementing. */
    dma_deinit(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dma_struct_para_init(&d);
    d.request      = BRIDGE_SPI_RX_DMA_REQ;
    d.direction    = DMA_PERIPHERAL_TO_MEMORY;
    d.periph_addr  = (uint32_t)&SPI_DATA(BRIDGE_SPI_PERIPH);
    d.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    d.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    d.memory_addr  = (uint32_t)spi_rx_dma_buf;
    d.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    d.memory_width = DMA_MEMORY_WIDTH_8BIT;
    d.number       = BRIDGE_SPI_DMA_BUF_LEN;
    d.priority     = DMA_PRIORITY_ULTRA_HIGH;
    dma_init(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH, &d);
    dma_circulation_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dma_memory_to_memory_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dmamux_synchronization_disable(DMAMUX_MULTIPLEXER_CH3);

    /* TX: spi_tx_dma_buf -> SPI1 DATA.  Armed per-reply with the exact
     * staged length; the SPI's TBE request prefills the TX FIFO the moment
     * the channel enables, so the first reply byte is ready before CS. */
    dma_deinit(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    dma_struct_para_init(&d);
    d.request      = BRIDGE_SPI_TX_DMA_REQ;
    d.direction    = DMA_MEMORY_TO_PERIPHERAL;
    d.periph_addr  = (uint32_t)&SPI_DATA(BRIDGE_SPI_PERIPH);
    d.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    d.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    d.memory_addr  = (uint32_t)spi_tx_dma_buf;
    d.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    d.memory_width = DMA_MEMORY_WIDTH_8BIT;
    d.number       = 0;
    d.priority     = DMA_PRIORITY_ULTRA_HIGH;
    dma_init(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH, &d);
    dma_circulation_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    dma_memory_to_memory_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    dmamux_synchronization_disable(DMAMUX_MULTIPLEXER_CH2);
}

/* Re-arm RX for a fresh transaction: full staging buffer.  CHCNT may only
 * be written while the channel is disabled. */
static void spi_dma_arm_rx(void)
{
    dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
    dma_memory_address_config(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH,
                              (uint32_t)spi_rx_dma_buf);
    dma_transfer_number_config(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH,
                               BRIDGE_SPI_DMA_BUF_LEN);
    dma_channel_enable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
}

/* Arm TX with exactly the staged reply (never more: the GD32 SPI has no
 * TX-underrun error and no FIFO flush, so over-queued bytes would stick --
 * the same invariant the old per-byte path enforced via tx_pending()). */
static void spi_dma_arm_tx(uint32_t len)
{
    dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
    if (len == 0u) {
        return;
    }
    dma_memory_address_config(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH,
                              (uint32_t)spi_tx_dma_buf);
    dma_transfer_number_config(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH, len);
    dma_channel_enable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);
}

static void spi_cs_exti_init(void)
{
    rcu_periph_clock_enable(RCU_SYSCFG);
    syscfg_exti_line_config(BRIDGE_SPI_CS_EXTI_PORT, BRIDGE_SPI_CS_EXTI_PIN);
    exti_init(BRIDGE_SPI_CS_EXTI_LINE, EXTI_INTERRUPT, EXTI_TRIG_BOTH);
    exti_interrupt_flag_clear(BRIDGE_SPI_CS_EXTI_LINE);
    nvic_irq_enable(BRIDGE_SPI_CS_EXTI_IRQN, BRIDGE_CS_IRQ_PRIO, BRIDGE_CS_IRQ_SUBPRIO);
}

/* (Re)configure the SPI1 slave peripheral: mode-0, 8-bit, hardware-NSS,
 * full-duplex, RX + error interrupts on.  Called at init AND as the per-
 * transaction FIFO flush (after a peripheral reset -- see the CS-rising
 * handler).  GPIO/EXTI/NVIC are set up once and survive a peripheral reset, so
 * they stay in bridge_transport_spi_hw_init(). */
static void bridge_spi_periph_config(void)
{
    spi_parameter_struct sp;
    spi_struct_para_init(&sp);
    sp.device_mode          = SPI_SLAVE;
    sp.trans_mode           = SPI_TRANSMODE_FULLDUPLEX;
    sp.frame_size           = SPI_FRAMESIZE_8BIT;
    sp.nss                  = SPI_NSS_HARD;
    sp.endian               = SPI_ENDIAN_MSB;
    sp.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;   /* mode 0 */
    spi_init(BRIDGE_SPI_PERIPH, &sp);

    /* Byte-access the FIFO so one CPU access == one 8-bit frame.  At reset the
     * GD32 SPI FIFO is in HALF-WORD access (BYTEN=0): a 16-bit spi_data_receive()
     * then pops TWO frames per call, which the old reply path tried to unpack --
     * but on an odd-length residue (any non-even frame count, e.g. the 4-byte
     * PING split across interrupts, or the 7-byte GET_VERSION reply) the second
     * half was a phantom 0x00 that shifted the request -> CRC fail -> STATUS_IO.
     * GigaDevice's own SPI slave examples (Examples/SPI/.../slave_*) use
     * SPI_BYTE_ACCESS + one spi_data_receive()/spi_data_transmit() per frame;
     * the SPL routes byte access to the 8-bit DATA alias (gd32g5x3_spi.c), so a
     * byte access DOES advance the FIFO -- the stale "8-bit jams / 16-bit
     * required" note was wrong.  Re-applied here on every (re)config because the
     * per-transaction RCU_SPI1RST reset (CS-rising handler) clears BYTEN. */
    spi_fifo_access_size_config(BRIDGE_SPI_PERIPH, SPI_BYTE_ACCESS);

    /* DMA-driven data path: no SPI data interrupts at all (at 25 MHz a byte
     * lands every 320 ns -- interrupt service cannot keep up).  Raise the DMA
     * request lines for both directions; the actual flow is gated by the DMA
     * channel enables (the arm helpers), so a raised TBE request with the TX
     * channel disabled moves nothing.  Both CTL1 bits are cleared by the
     * per-transaction RCU_SPI1RST flush, so they are re-applied here, exactly
     * like BYTEN above. */
    spi_dma_enable(BRIDGE_SPI_PERIPH, SPI_DMA_RECEIVE);
    spi_dma_enable(BRIDGE_SPI_PERIPH, SPI_DMA_TRANSMIT);
    spi_enable(BRIDGE_SPI_PERIPH);
}

void bridge_transport_spi_hw_init(void)
{
    rcu_periph_clock_enable(BRIDGE_SPI_RCU);
    spi_gpio_init();
    spi_dma_init();
    bridge_spi_periph_config();
    spi_dma_arm_rx();
    /* No TX arm yet: nothing is staged until the first request decodes.
     * No SPI NVIC interrupt either -- the DMA channels carry the data and
     * the CS EXTI below carries the framing. */
    spi_cs_exti_init();
}

/* CS edge: PA8 on EXTI8.  Falling = select (reset RX, preload the staged
 * reply); rising = end of transaction (decode + stage the next reply). */
void BRIDGE_SPI_CS_EXTI_HANDLER(void)
{
    if (RESET != exti_interrupt_flag_get(BRIDGE_SPI_CS_EXTI_LINE)) {
        exti_interrupt_flag_clear(BRIDGE_SPI_CS_EXTI_LINE);
        if (RESET == gpio_input_bit_get(BRIDGE_SPI_NSS_PORT, BRIDGE_SPI_NSS_PIN)) {
            /* CS asserted (active-low): reset the portable RX staging.  No
             * preload needed anymore -- the TX DMA armed at the previous
             * CS-rising has already prefilled the SPI TX FIFO with the head
             * of the staged reply (TBE requests are served the moment the
             * channel enables), so the first SCK finds valid data waiting. */
            spi_slave_cs_low();
        } else {
            /* CS released: end of transaction.
             *
             * 1. Snapshot the RX residue FIRST: bytes captured by RX DMA =
             *    buffer length minus the remaining transfer count.
             * 2. Quiesce both DMA channels, then FLUSH + re-init the SPI via
             *    the RCU reset (the only reliable FIFO flush; it also clears
             *    BYTEN/DMAREN/DMATEN, which bridge_spi_periph_config
             *    re-applies) so the peripheral is reception-ready while the
             *    heavier decode below runs.
             * 3. Feed the captured bytes through the byte seams and decode
             *    (spi_slave_cs_high stages the reply; the all-0x00 reply-
             *    drain gate in the portable layer is unchanged).
             * 4. Drain the staged reply into the flat TX DMA buffer and
             *    re-arm: RX for a full buffer, TX for exactly the reply.
             *
             * Budget: steps 1-4 are register writes + CRC over <=69 B at
             * 216 MHz -- single-digit microseconds, well inside the master's
             * inter-transaction gap (its CS setup window alone is 60 us). */
            uint32_t remaining = dma_transfer_number_get(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
            uint32_t received  = (remaining <= BRIDGE_SPI_DMA_BUF_LEN)
                                     ? (BRIDGE_SPI_DMA_BUF_LEN - remaining)
                                     : 0u;

            dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_RX_DMA_CH);
            dma_channel_disable(BRIDGE_SPI_DMA, BRIDGE_SPI_TX_DMA_CH);

            rcu_periph_reset_enable(RCU_SPI1RST);
            rcu_periph_reset_disable(RCU_SPI1RST);
            bridge_spi_periph_config();

            for (uint32_t i = 0; i < received; i++) {
                spi_slave_rx_byte(spi_rx_dma_buf[i]);
            }
            spi_slave_cs_high();

            uint32_t reply_len = 0;
            while (spi_slave_tx_pending() && (reply_len < BRIDGE_SPI_DMA_BUF_LEN)) {
                spi_tx_dma_buf[reply_len++] = spi_slave_tx_next_byte();
            }

            spi_dma_arm_rx();
            spi_dma_arm_tx(reply_len);
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
