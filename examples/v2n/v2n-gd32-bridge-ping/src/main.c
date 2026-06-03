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

    /* Open BRD_I2C (Renesas RIIC8 on P07/P06).  Studio-resolved
     * bus_id; replace 0u with the alias your board.yaml resolves
     * to on real hardware. */
    alp_i2c_t *i2c = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,
        .bitrate_hz = 400000u,
    });
    if (i2c == NULL) {
        printf("[gd32-bridge-ping] alp_i2c_open failed: err=%d\n",
               (int)alp_last_error());
        return 0;
    }

    /* Open the GD32 SPI fast path.  The Renesas RSPI master sits at
     * P76/77/96/97 (MOSI/MISO/SCLK/CS) -- alp-studio resolves the
     * bus + CS pin from the SoM's gd32-io-mcu-map.tsv. */
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = 1u,
        .freq_hz       = 10000000u,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8u,
        .cs_pin_id     = 0u, /* studio-resolved */
    });
    if (spi == NULL) {
        printf("[gd32-bridge-ping] alp_spi_open failed: err=%d "
               "(continuing with I2C-only)\n", (int)alp_last_error());
    }

    /* Init the driver against either or both transports.  PING +
     * GET_VERSION happen inside init -- if the firmware reports a
     * mismatched major version, init returns ALP_ERR_NOSUPPORT and
     * the host driver refuses to operate. */
    gd32g553_t ctx;
    alp_status_t s = gd32g553_init(&ctx, spi, i2c,
                                   GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
    if (s != ALP_OK) {
        printf("[gd32-bridge-ping] gd32g553_init failed: %d\n", (int)s);
        goto out;
    }
    printf("[gd32-bridge-ping] init OK; firmware v%u.%u.%u\n",
           ctx.version.major, ctx.version.minor, ctx.version.patch);

    /* Cross-check both transports if both are wired.  PING is
     * idempotent and has no side effects, so issuing it on both
     * transports is a clean liveness probe. */
    if (spi != NULL) {
        s = gd32g553_ping_via(&ctx, GD32G553_TRANSPORT_SPI);
        printf("[gd32-bridge-ping] SPI ping -> %d\n", (int)s);
    }
    s = gd32g553_ping_via(&ctx, GD32G553_TRANSPORT_I2C);
    printf("[gd32-bridge-ping] I2C ping -> %d\n", (int)s);

    /* Read the build identifier (truncated SHA-1 of the firmware
     * ELF) -- useful for production-test logging.  Always rides
     * the default transport (SPI when present). */
    char build_id[GD32G553_BUILD_ID_LEN + 1];
    s = gd32g553_get_build_id(&ctx, build_id);
    if (s == ALP_OK) {
        printf("[gd32-bridge-ping] firmware build-id: %s\n", build_id);
    } else {
        printf("[gd32-bridge-ping] get_build_id -> %d\n", (int)s);
    }

    /* Read the GD32-sampled DA9292 INT/TW fault-pin byte (bit0 = INT
     * asserted, bit1 = TW asserted, 0xFF = not sampled yet).  The
     * GD32 has no I2C path to the DA9292 -- this is pin-state
     * forwarding only.  For register-level PMIC status (PMC_STATUS_00
     * etc.) read the DA9292 over BRD_I2C via da9292_get_status(). */
    uint8_t pmic_status = 0u;
    s = gd32g553_da9292_status_forward(&ctx, &pmic_status);
    if (s == ALP_OK) {
        /* 0xFF = "no sample taken yet" (current firmware). */
        printf("[gd32-bridge-ping] DA9292 fault-pin state: 0x%02X\n",
               pmic_status);
    } else {
        printf("[gd32-bridge-ping] da9292 status forward -> %d "
               "(expected NOSUPPORT until firmware HAL fills it in)\n",
               (int)s);
    }

    gd32g553_deinit(&ctx);

out:
    if (spi != NULL) alp_spi_close(spi);
    alp_i2c_close(i2c);
    printf("[gd32-bridge-ping] done\n");
    return 0;
}
