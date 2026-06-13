/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SPI-slave transport wiring.
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti (bench).  Overrides the weak
 * bridge_transport_spi_hw_init() and drives the SILICON-FREE seams in
 * src/transport_spi.c from the CC3501E's SPI-slave peripheral.
 *
 * The seam wiring below is the PROTOCOL contract and is concrete; the
 * "[TI-SDK]" peripheral setup is the bench TODO (confirm against the
 * SimpleLink CC33xx SPI driver -- ti/drivers/SPI.h or the lower-level
 * register driver, depending on the latency budget).
 *
 * Inter-chip pins (metadata/e1m_modules/aen/inter-chip.tsv): the host
 * SPI1 control link lands on CC3501E GPIO_27/28/29 (SCLK/MOSI/MISO);
 * the host (Alif) is master, the CC3501E is slave.
 */

#include <stdint.h>

#include "../../src/transport.h"

/* [TI-SDK] #include the SimpleLink SPI driver + GPIO/interrupt headers. */

/*
 * Bring-up contract for the bench implementer:
 *
 *  1. Configure GPIO_27/28/29 as the SPI-slave SCLK/MOSI/MISO function
 *     and the CS line as an interrupt source (both edges).
 *  2. On CS falling edge  -> call spi_slave_cs_low().
 *  3. On each received byte (RX FIFO / RBNE-equivalent) -> call
 *     spi_slave_rx_byte(b).
 *  4. On CS rising edge    -> call spi_slave_cs_high()  (decodes the
 *     request frame + stages the reply).
 *  5. Raise the firmware's READY GPIO so the host knows the reply is
 *     staged, then on the host's read transaction feed the TX FIFO from
 *     spi_slave_tx_next_byte() while spi_slave_tx_pending() is true.
 *
 * Step 5's READY handshake is the piece the host driver
 * (chips/cc3501e/cc3501e.c) still needs reworked from its current
 * single-transceive placeholder; pin the GPIO choice here when the EVK
 * overlay assigns it.
 */

void bridge_transport_spi_hw_init(void)
{
    /* [TI-SDK] Configure the SPI-slave peripheral + CS interrupt and
     * register the ISRs that call the seams per the contract above. */
}
