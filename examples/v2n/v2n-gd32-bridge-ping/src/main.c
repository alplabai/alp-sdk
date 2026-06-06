/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-bridge-ping — open the on-module GD32G553 supervisor MCU
 * bridge, exchange PING + GET_VERSION over both transports.
 *
 * This example is intentionally chatty: it walks through every step
 * of the V2N supervisor handshake so the file doubles as a tutorial
 * for the host driver's API.  Comment density is ~50 % (the example
 * is documentation, not just runnable code).
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/gd32g553.h"

/*
 * Two parallel bus handles are wired into a single gd32g553 driver
 * context.  The driver picks the SPI fast path by default when both
 * are present; per-call `_via` helpers let the caller override.
 *
 * On boards that only wire one of the two transports, pass NULL for
 * the other -- gd32g553_init checks at least one is non-NULL.
 */

int main(void) {
    printf("[gd32-bridge-ping] V2N supervisor MCU smoke test\n");

    /* No fixed AMP gate-sync wait: the FSP opens SCI7 itself (R_BSP_MODULE_START
     * inside R_SCI_B_SPI_Open -- validated on silicon: the link comes up with no
     * A55 SCI7-gate assistance), and the init/PING do-while below already retries
     * every 200 ms until the GD32 answers, so no fixed boot delay is needed. */

    /* SPI-only bring-up: BRD_I2C (RIIC8) is intentionally NOT opened here.
     * The GD32 PING uses the dedicated SPI fast path (gd32g553_init is called
     * with i2c=NULL below), and on this SoM BRD_I2C is held low by the DX-M1
     * (see the bench handoff), so opening RIIC8 first would only add a second
     * CM33-owned peripheral to bring up with no benefit to the SPI test. */

    /* Open the GD32 SPI fast path.  The Renesas SCI7 Simple-SPI master
     * sits at P76/77/96/97 (MOSI/MISO/SCLK/CS) -- alp-studio resolves
     * the bus from the SoM's gd32-io-mcu-map.tsv. */
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id = 1u,
        /*
         * 25 MHz fast path.  The GD32 SLAVE streams through its DMA0 CH2/CH3
         * (RX capture + staged-reply arming framed by the CS EXTI, see
         * transport_hw_gd32.c) -- mandatory at 25 MHz, a slave cannot pace
         * SCK.  The CM33 MASTER uses the driver's zero-interrupt POLLED
         * engine (spi_renesas_rz_sci_b.c): the master paces SCK, so polling
         * is robust by construction and costs only tens of microseconds per
         * <=69-byte transaction; the MCPU-DMAC fast path is preserved in the
         * driver but silicon-blocked (ALP_V2N_SCI7_DMAC notes).  25 MHz is an
         * exact P5CLK/4 divide on the SCI (CCR2 cks=0/bgdm=1/brr=1) and sits
         * inside the GD32 slave's 27 MHz datasheet ceiling (Rev2.0 Table
         * 4-49) with slave timing margins met (tV(SO)=9 ns vs the 20 ns
         * half-period; MISO pad raised to the 85 MHz class).  The wire stays
         * 8-bit frames -- the SCI Simple-SPI IP is 8-bit-only by design
         * (r_sci_b_spi.c:226) -- so the speed-up is the 25x clock plus
         * zero per-byte interrupt work, not wider frames.
         */
        .freq_hz       = 25000000u,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        /* No SDK-driven chip-select: on this SoM the platform SPI driver
         * owns the CS pad itself (P97 direct-latch, framed around every
         * transaction for the GD32's CS-EXTI).  Passing a pin id here
         * would ALSO route CS through the generic GPIO path -- a second,
         * conflicting owner for the same pad. */
        .cs_pin_id = ALP_SPI_NO_CS,
    });
    if (spi == NULL) {
        printf("[gd32-bridge-ping] alp_spi_open failed: err=%d "
               "(continuing with I2C-only)\n", (int)alp_last_error());
    }

    /*
     * SCI7 SPI bring-up + continuous liveness probe.
     *
     * The CM33 system-manager starts very early in the boot chain, so the
     * on-module GD32 may still be in / just leaving reset at that instant
     * (GD32_NRST shares the PMIC reset-out).  A single PING at startup can
     * therefore race the GD32's power-up and be lost.  So: retry init -- which
     * PINGs + reads the firmware version over the SPI fast path -- until the
     * GD32 answers, then keep PINGing so the link stays continuously
     * verifiable.  A serviced PING leaves `A5 00 FF 84` in the GD32's
     * spi_rx_buf @ 0x20000000 (readable over the GD32 SWD with no CM33
     * console), and every step is logged on the CM33 console (sci0) for when a
     * UART is attached.
     *
     * i2c is passed as NULL so the probe exercises ONLY the SPI path -- BRD_I2C
     * is currently held low by the DX-M1 (see the bench handoff), and we do not
     * want a wedged I2C transaction to stall the retry loop.
     */

    gd32g553_t ctx;
    alp_status_t s;
    unsigned     attempt = 0u;

    if (spi == NULL) {
        printf("[gd32-bridge-ping] no SPI bus resolved -- cannot run the "
               "SCI7 probe; check the alp-spi1 alias / board wiring\n");
        goto out;
    }

    do {
        s = gd32g553_init(&ctx, spi, NULL, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
        if (s != ALP_OK) {
            printf("[gd32-bridge-ping] init/PING attempt %u failed: %d "
                   "(GD32 not ready yet?) -- retrying in 200 ms\n",
                   attempt, (int)s);
            attempt++;
            k_msleep(200);
        }
    } while (s != ALP_OK);

    printf("[gd32-bridge-ping] init OK after %u retr%s; firmware v%u.%u.%u\n", attempt,
           (attempt == 1u) ? "y" : "ies", ctx.version.major, ctx.version.minor, ctx.version.patch);

    /* Continuous SPI-path liveness PING -- refreshes the GD32's spi_rx_buf so
     * the link can be confirmed over SWD at any time, and proves the CM33 is
     * alive and clocking SCI7. */
    for (uint32_t i = 0u;; ++i) {
        s = gd32g553_ping_via(&ctx, GD32G553_TRANSPORT_SPI);
        printf("[gd32-bridge-ping] SPI ping #%u -> %d\n", i, (int)s);

        /* Odd-length soak: every 8th cycle also re-read the firmware version.
         * GET_VERSION's reply envelope is 7 bytes (SOF | STATUS | major | minor
         * | patch | CRC16) -- an ODD frame count that stresses the GD32 reply
         * FIFO's residue handling (the failure class the byte-access reply-path
         * fix addresses; a half-entry left in a half-word FIFO desyncs every
         * later transaction).  A desync would corrupt the returned version or
         * wedge the link, so logging it lets a console-attached run confirm the
         * 7-byte path stays byte-aligned alongside the 4-byte PING. */
        if ((i % 8u) == 0u) {
            gd32g553_version_t v  = { 0 };
            alp_status_t       vs = gd32g553_get_version(&ctx, &v);
            printf("[gd32-bridge-ping] SPI get_version #%u -> %d (v%u.%u.%u)\n", i, (int)vs,
                   v.major, v.minor, v.patch);
        }

        k_msleep(500);
    }
    /* not reached */

out:
    if (spi != NULL) alp_spi_close(spi);
    printf("[gd32-bridge-ping] done\n");
    return 0;
}
