/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SDIO-slave transport wiring (the
 * OPTIONAL host-control link).  Built ONLY for CC3501E_HAL_BACKEND=ti.
 *
 * Drives the frame-oriented seams in src/transport_sdio.c: a received
 * command block is handed to sdio_slave_on_request() (which runs the
 * shared protocol_build_reply()), and the staged reply at
 * sdio_slave_reply()/sdio_slave_reply_len() is clocked back on the
 * host's read block.  The framing is byte-identical to the SPI link.
 *
 * Inter-chip pins (metadata/e1m_modules/aen/inter-chip.tsv):
 * SDIO.CLK/CMD/D0..D3 on CC3501E GPIO_3/4/5/6/10/11.  SDIO is available
 * only when the board routes the Alif's single SDIO controller to the
 * CC3501E instead of an SD card (transport.h / docs/cc3501e-bridge.md).
 *
 * ============================ BENCH NOTE ============================
 * The frame glue below is complete and transport-agnostic.  The ONE
 * piece that needs the SDK + datasheet on the bench is binding the
 * CC3501E's SDIO peripheral as a DEVICE/function to the external host:
 * unlike SPI-slave (TI Drivers `SPI_open(..., SPI_SLAVE, ...)`), the
 * SimpleLink CC35xx SDK does NOT expose a public SDIO-device driver
 * (TI Drivers `SD.h` is SD-card *host*; SDIO-as-host-interface is
 * normally TI's companion-mode NWP firmware, not user app code).  The
 * SDIO-device register bring-up must come from SWRU626 §21 (SDIO) on
 * the bench -- it is deliberately NOT fabricated here.  Wire that ISR
 * to call sdio_hw_on_block_received() / drain sdio_hw_reply_ptr().
 * Until then, prefer the default SPI transport (which is fully wired).
 * ===================================================================
 */

#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include "../../src/transport.h"

/* --------------------------------------------------------------- */
/* Frame glue (complete, transport-agnostic)                         */
/* --------------------------------------------------------------- */

/* Called by the SDIO-device block-received ISR with one complete
 * request block.  Runs the shared parser/dispatcher and stages the
 * reply; returns the staged reply length the ISR should clock back on
 * the host's read block. */
uint16_t sdio_hw_on_block_received(const uint8_t *block, uint16_t len)
{
	sdio_slave_on_request(block, len);
	return sdio_slave_reply_len();
}

/* The staged reply buffer the SDIO-device read path clocks back. */
const uint8_t *sdio_hw_reply_ptr(void)
{
	return sdio_slave_reply();
}

uint16_t sdio_hw_reply_len(void)
{
	return sdio_slave_reply_len();
}

/* --------------------------------------------------------------- */
/* Peripheral bring-up                                               */
/* --------------------------------------------------------------- */

void bridge_transport_sdio_hw_init(void)
{
	/* Configure the CC3501E SDIO peripheral in DEVICE/function mode on
     * GPIO_3/4/5/6/10/11 and register the block-transfer ISR so it
     * drives sdio_hw_on_block_received() (request block in) and clocks
     * sdio_hw_reply_ptr()/sdio_hw_reply_len() back (reply block out).
     *
     * See the BENCH NOTE in this file's header: the device-mode register
     * sequence is SWRU626 §21 + bench work and is intentionally not
     * fabricated.  This hook is reached only when the build selects
     * CC3501E_CONTROL_TRANSPORT=sdio; the default SPI transport never
     * calls it. */
}
