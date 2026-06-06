/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi-slave -- demonstrate the SHAPE of SPI slave-mode application
 * code on the Alp SDK.
 *
 * ============================================================
 *   SDK GAP NOTICE -- READ THIS BEFORE COPYING THE EXAMPLE
 * ============================================================
 *
 * As of v0.6 the Alp SDK's <alp/peripheral.h> SPI surface is
 * MASTER-ONLY.  There is no `alp_spi_open_slave()` /
 * `alp_spi_slave_register()` / equivalent yet.  This example
 * exists to:
 *
 *   1. Document the gap so customers don't waste time hunting for
 *      a non-existent header (issue: track at v1.0 ABI freeze).
 *   2. Stake out the proposed API shape so when the slave-mode
 *      surface lands, the example is the migration template.
 *   3. Show the recommended callback-based RX pattern -- the
 *      idiom every embedded engineer expects for "respond to
 *      whatever the master clocks in".
 *
 * The code below uses a small `alp_spi_slave_*` shim defined at
 * the top of this file.  Today every shim function returns
 * ALP_ERR_NOSUPPORT; the example prints the diagnostic and exits.
 * When the real surface lands (planned for v0.7), the shim block
 * deletes and the calls bind to the upstream API unchanged.
 *
 * For master-side SPI (which the SDK DOES support), see:
 *   * examples/peripheral-io/spi-loopback  -- single-chip self-test
 *   * examples/peripheral-io/spi-master    -- discrete master
 *
 * Test setup once the API lands: wire SCK, MOSI, MISO, /CS, and
 * GND between this board and a master-mode board running
 * examples/peripheral-io/spi-master.  Each clock the master generates clocks
 * a byte from our TX buffer onto MISO and a byte from MOSI into
 * our RX callback.
 *
 *
 * Runs on both EVKs: BOARD_SPI_ARDUINO (from <alp/board.h>) resolves
 * to E1M_SPI1 on E1M EVK and E1M_X_SPI1 on E1M-X EVK.
 * ============================================================
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/board.h"

/* ------------------------------------------------------------------
 * Local shim for the not-yet-shipped slave-mode API.
 *
 * Every function returns ALP_ERR_NOSUPPORT so the example runs to
 * completion without hardware effects.  When the real
 * <alp/peripheral.h> surface lands, delete this block and the
 * downstream code keeps compiling against the upstream names.
 * ------------------------------------------------------------------ */

/** Proposed: opaque slave handle, parallel to alp_spi_t. */
typedef struct alp_spi_slave_shim alp_spi_slave_t;

/** Proposed: slave configuration. */
typedef struct {
    uint32_t       bus_id;        /**< BOARD_SPI_ARDUINO (or any SPI bus id) to claim. */
    alp_spi_mode_t mode;          /**< CPOL/CPHA -- must match master. */
    uint8_t        bits_per_word; /**< Usually 8 -- must match master. */
    uint32_t       cs_pin_id;     /**< /CS pin the master will assert. */
} alp_spi_slave_config_t;

/** Proposed: callback fired on each MOSI byte clocked in.  Return the
 *  byte to drive on MISO during the NEXT clock cycle. */
typedef uint8_t (*alp_spi_slave_byte_cb_t)(uint8_t mosi_byte, void *user);

/** Proposed: callback fired when /CS deasserts (end of transfer).
 *  Lets the slave reset its protocol state machine. */
typedef void (*alp_spi_slave_eot_cb_t)(void *user);

/* TODO(api-gap): replace these stubs once <alp/peripheral.h> grows
 * slave-mode support. */
static alp_spi_slave_t *alp_spi_slave_open(const alp_spi_slave_config_t *cfg)
{
    (void)cfg;
    /* Real impl will Zephyr-dispatch to the controller's slave
     * registration call (Zephyr's spi_slave_register, where
     * available).  Note: Zephyr's SPI slave support is itself
     * patchy -- many Zephyr SoC drivers don't implement it. */
    return NULL;
}

static alp_status_t alp_spi_slave_set_callbacks(alp_spi_slave_t        *slave,
                                                alp_spi_slave_byte_cb_t on_byte,
                                                alp_spi_slave_eot_cb_t on_eot, void *user)
{
    (void)slave;
    (void)on_byte;
    (void)on_eot;
    (void)user;
    return ALP_ERR_NOSUPPORT;
}

static void alp_spi_slave_close(alp_spi_slave_t *slave)
{
    (void)slave;
}

/* ------------------------------------------------------------------
 * Slave-side protocol state machine.
 *
 * The canonical SPI slave idiom: implement a tiny request/response
 * protocol where the master sends a 1-byte command followed by N
 * bytes of payload; the slave replies in the same transfer (because
 * SPI is full-duplex).
 *
 * Example protocol:
 *   Master TX: [CMD] [B0] [B1] [B2] [B3]
 *   Slave  TX: [00 ] [R0] [R1] [R2] [R3]
 *
 * The slave's first response byte is always 0x00 (status placeholder)
 * because it didn't know the command was coming until MOSI clocked in.
 * ------------------------------------------------------------------ */

#define CMD_PING 0x01u        /* echo payload back */
#define CMD_GET_VERSION 0x02u /* reply with 4-byte version string */

static volatile uint8_t  g_cmd        = 0u;
static volatile uint32_t g_bytes_seen = 0u;
static volatile uint32_t g_transfers  = 0u;

/* Per-byte callback runs in IRQ context.  Keep work minimal: state
 * machine update + pick the next MISO byte.  Defer heavy work to a
 * thread / workqueue via a flag. */
static uint8_t on_mosi_byte(uint8_t mosi, void *user)
{
    (void)user;
    g_bytes_seen++;

    /* First byte of a transfer is always the command. */
    if (g_bytes_seen == 1u) {
        g_cmd = mosi;
        return 0x00u; /* status placeholder -- master discards */
    }

    /* Subsequent bytes: protocol-specific response. */
    switch (g_cmd) {
    case CMD_PING:
        /* Echo whatever MOSI sent back on MISO -- the simplest
         * possible protocol; useful for connectivity probes. */
        return mosi;
    case CMD_GET_VERSION:
        /* Drive a fixed version byte sequence. */
        switch (g_bytes_seen - 1u) { /* index after the command byte */
        case 1u:
            return 0x00u; /* major */
        case 2u:
            return 0x06u; /* minor (v0.6 today) */
        case 3u:
            return 0x00u; /* patch */
        case 4u:
            return 'A'; /* tag char */
        default:
            return 0x00u; /* extra clocks beyond the
                                       * fixed-length reply: pad */
        }
    default:
        /* Unknown command -- reply with 0xEE so the master can
         * distinguish "bad command" from a 0x00 padding byte. */
        return 0xEEu;
    }
}

/* End-of-transfer callback fires when the master deasserts /CS.
 * Reset the state machine so the next transfer starts cleanly. */
static void on_eot(void *user)
{
    (void)user;
    g_transfers++;
    g_bytes_seen = 0u;
    g_cmd        = 0u;
}

int main(void)
{
    printf("[spi-slave] open as slave on BOARD_SPI_ARDUINO (mode 0, 8 bits)\n");

    alp_spi_slave_t *s = alp_spi_slave_open(&(alp_spi_slave_config_t){
        .bus_id        = BOARD_SPI_ARDUINO,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8,
        .cs_pin_id     = 0u, /* BOARD_SPI_ARDUINO carries its own
                                        * board-routed /CS line; a
                                        * discrete CS GPIO is only
                                        * needed when chaining extra
                                        * slaves. */
    });
    if (s == NULL) {
        /* Today this branch ALWAYS fires because the shim returns
         * NULL.  Customers reading the console see why their build
         * succeeds but the slave doesn't respond. */
        printf("[spi-slave] Alp SDK v0.6 does NOT support SPI slave mode\n");
        printf("[spi-slave]   <alp/peripheral.h> is master-only today\n");
        printf("[spi-slave]   Note: Zephyr's own SPI slave support is patchy too;\n");
        printf("[spi-slave]         some SoC drivers don't implement spi_slave_register.\n");
        printf("[spi-slave]   tracking: v0.7 API surface addition\n");
        printf("[spi-slave] done\n");
        return 0;
    }

    /* Unreachable today; kept so the type-checker exercises the
     * proposed API shape. */
    alp_status_t st = alp_spi_slave_set_callbacks(s, on_mosi_byte, on_eot, NULL);
    if (st != ALP_OK) {
        printf("[spi-slave] set_callbacks -> %d (expected -6 NOSUPPORT today)\n", (int)st);
        alp_spi_slave_close(s);
        printf("[spi-slave] done\n");
        return 0;
    }

    /* Idle loop -- the callbacks do all the work in ISR context.
     * Print transfer counts once a second so an operator running
     * the example can SEE incoming traffic. */
    for (int i = 0; i < 5; i++) {
        printf("[spi-slave] tick %d transfers=%u bytes=%u last_cmd=0x%02x\n", i,
               (unsigned)g_transfers, (unsigned)g_bytes_seen, (unsigned)g_cmd);
        k_msleep(1000);
    }

    alp_spi_slave_close(s);
    printf("[spi-slave] done\n");
    return 0;
}
