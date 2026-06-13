/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SDIO-slave transport wiring
 * (OPTIONAL link; bench item).
 *
 * Built ONLY for CC3501E_HAL_BACKEND=ti.  Overrides the weak
 * bridge_transport_sdio_hw_init() and drives the SILICON-FREE seams in
 * src/transport_sdio.c from the CC3501E's SDIO-slave function.
 *
 * SDIO is available only when the board routes the Alif's single SDIO
 * controller to the CC3501E (no SD card) -- see transport.h /
 * docs/cc3501e-bridge.md.  Inter-chip pins
 * (metadata/e1m_modules/aen/inter-chip.tsv): SDIO.CLK/CMD/D0..D3 on
 * CC3501E GPIO_3/4/5/6/10/11.
 *
 * STATUS: bench skeleton.  The seam contract is concrete; the SDIO-
 * function block-transfer setup ("[TI-SDK]") is the v0.x bench TODO.
 */

#include <stdint.h>

#include "../../src/transport.h"

/* [TI-SDK] #include the SimpleLink SDIO function-driver headers. */

/*
 * Bring-up contract for the bench implementer:
 *
 *  1. Configure the SDIO-slave function on GPIO_3/4/5/6/10/11.
 *  2. On a received command block (one request frame) -> call
 *     sdio_slave_on_request(frame, len).
 *  3. Clock the staged reply back to the host from sdio_slave_reply()
 *     for sdio_slave_reply_len() bytes on the host's read block.
 *
 * Because SDIO is block-oriented, the request/reply correlation is
 * cleaner than SPI's (no per-byte CS framing); the same protocol frame
 * rides inside the data block.
 */

void bridge_transport_sdio_hw_init(void)
{
    /* [TI-SDK] Configure the SDIO-slave function + block-transfer ISRs
     * that call the seams per the contract above. */
}
