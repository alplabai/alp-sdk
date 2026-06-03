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

    /* RX-driven: TX is fed from the RBNE handler (no TBE interrupt, so
     * the peripheral never spins on an empty TX buffer at idle). */
    spi_interrupt_enable(BRIDGE_SPI_PERIPH, SPI_INT_RBNE);
    spi_interrupt_enable(BRIDGE_SPI_PERIPH, SPI_INT_ERR);
    spi_enable(BRIDGE_SPI_PERIPH);
}

void bridge_transport_spi_hw_init(void)
{
    rcu_periph_clock_enable(BRIDGE_SPI_RCU);
    spi_gpio_init();
    nvic_irq_enable(BRIDGE_SPI_IRQN, BRIDGE_SPI_IRQ_PRIO, BRIDGE_SPI_IRQ_SUBPRIO);
    bridge_spi_periph_config();
    spi_cs_exti_init();
}

/* SPI1 data interrupt: one RX byte per clocked frame; load the next
 * reply byte so it is ready for the byte the master clocks next. */
void BRIDGE_SPI_IRQ_HANDLER(void)
{
    /* Drain the RX FIFO one frame per RBNE.  With byte-access set (see
     * bridge_spi_periph_config) each spi_data_receive() pops exactly one
     * received byte; for every byte taken in, feed exactly one reply byte back
     * so TX stays frame-aligned with RX.  Feed ONLY while the staged reply has
     * bytes left: the GD32 SPI has no TX-underrun error and no FIFO flush, so
     * any byte queued beyond the reply is never clocked out and accumulates
     * until the TX FIFO is FULL (STAT.TXLVL=0b11) -- the reply then sits behind
     * that stale backlog and the host reads garbage -> reply CRC fails. */
    int rbne_guard = 72;
    while ((RESET != spi_interrupt_flag_get(BRIDGE_SPI_PERIPH, SPI_INT_FLAG_RBNE)) &&
           (rbne_guard-- > 0)) {
        spi_slave_rx_byte((uint8_t)spi_data_receive(BRIDGE_SPI_PERIPH));
        if (spi_slave_tx_pending()) {
            spi_data_transmit(BRIDGE_SPI_PERIPH, (uint16_t)spi_slave_tx_next_byte());
        }
    }
    /* Drain any error condition (overrun etc.): reading DATA+STAT in the
     * RBNE path clears RX overrun; nothing else to do for a slave. */
    if (RESET != spi_interrupt_flag_get(BRIDGE_SPI_PERIPH, SPI_INT_FLAG_RXORERR)) {
        (void)spi_data_receive(BRIDGE_SPI_PERIPH);
    }
}

/* CS edge: PA8 on EXTI8.  Falling = select (reset RX, preload the staged
 * reply); rising = end of transaction (decode + stage the next reply). */
void BRIDGE_SPI_CS_EXTI_HANDLER(void)
{
    if (RESET != exti_interrupt_flag_get(BRIDGE_SPI_CS_EXTI_LINE)) {
        exti_interrupt_flag_clear(BRIDGE_SPI_CS_EXTI_LINE);
        if (RESET == gpio_input_bit_get(BRIDGE_SPI_NSS_PORT, BRIDGE_SPI_NSS_PIN)) {
            /* CS asserted (active-low): reset RX and preload the first reply
             * BYTE so it is in the TX shift register before the first SCK.  With
             * byte-access (see bridge_spi_periph_config) a single spi_data_transmit
             * queues exactly one frame -- no phantom second byte to shift the
             * reply.  Guard on spi_slave_tx_pending() so nothing is queued when no
             * reply is staged (the GD32 SPI has no TX-underrun error and no FIFO
             * flush, so an over-queued byte would stick and mis-align later
             * replies).  The rest of the reply is fed one byte per RBNE by the
             * handler above -- which keeps up at the supported SCK (the SCI
             * master, lacking DMA, caps the link well below the refill-lag rate). */
            spi_slave_cs_low();
            if (spi_slave_tx_pending()) {
                spi_data_transmit(BRIDGE_SPI_PERIPH, (uint16_t)spi_slave_tx_next_byte());
            }
        } else {
            /* CS released: end of transaction.  FLUSH + re-arm the SPI peripheral
             * FIRST (RCU reset + re-config), THEN decode -- the order matters: the
             * decode (CRC + protocol_dispatch) is the heavy part, and doing it
             * before the reset leaves the peripheral in its post-transaction state
             * for that whole window; if the host opens the next transaction during
             * it, frames are mishandled.  Resetting first re-arms the slave
             * immediately; the reply is staged in software (spi_tx_buf), survives
             * the reset, and is re-preloaded on the next CS-low.  The reset also
             * drops any residual FIFO bytes from a partially-clocked transaction.
             * spi_slave_cs_high() decodes only a real request (leading SOF); a
             * reply-drain transaction leaves the staged reply untouched. */
            rcu_periph_reset_enable(RCU_SPI1RST);
            rcu_periph_reset_disable(RCU_SPI1RST);
            bridge_spi_periph_config();
            spi_slave_cs_high();
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
