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
 * to call sdio_hw_on_block_received() / drain sdio_hw_reply_ptr(),
 * and call sdio_hw_on_reply_block_sent() once the host's read block
 * completes (see below -- the deferred CMD_RESET / OTA swap-reboot
 * depend on it).
 * Until then, prefer the default SPI transport (which is fully wired).
 * ===================================================================
 */

#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include "../../src/transport.h"

#include "../cc3501e_hw.h" /* cc3501e_hw_notify_reply_sent -- arm the deferred reset/OTA-swap reboot */

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

/* Called by the SDIO-device read-block-complete ISR once the staged
 * reply has FULLY clocked back to the host.  Mirrors the SPI
 * transport's PH_REPLY_PAYLOAD completion (transport_hw_ti_spi.c
 * on_transfer): without this drain notification the HAL's
 * reply_drained latch never sets on the SDIO link, so the deferred
 * CMD_RESET reboot and the OTA FINISH swap-reboot (both gated on it
 * in cc3501e_hw_tick) never fire.  Fetching sdio_hw_reply_ptr() is
 * NOT the drain event -- a DMA'd read block finishes later, and
 * notifying early would re-open the reset-races-the-ack window the
 * latch exists to close. */
void sdio_hw_on_reply_block_sent(void)
{
	cc3501e_hw_notify_reply_sent();
}

/* --------------------------------------------------------------- */
/* Peripheral bring-up                                               */
/* --------------------------------------------------------------- */

void bridge_transport_sdio_hw_init(void)
{
	/* Configure the CC3501E SDIO peripheral in DEVICE/function mode on
     * GPIO_3/4/5/6/10/11 and register the block-transfer ISR so it
     * drives sdio_hw_on_block_received() (request block in), clocks
     * sdio_hw_reply_ptr()/sdio_hw_reply_len() back (reply block out)
     * and reports sdio_hw_on_reply_block_sent() when that read block
     * completes (arms the deferred CMD_RESET / OTA swap-reboot).
     *
     * See the BENCH NOTE in this file's header: the device-mode register
     * sequence is SWRU626 §21 + bench work and is intentionally not
     * fabricated.  This hook is reached only when the build selects
     * CC3501E_CONTROL_TRANSPORT=sdio; the default SPI transport never
     * calls it. */
}
