/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Alif Ensemble UART wrapper for <alp/peripheral.h>.
 * See vendors/alif/i2c.c for the gating + scaffolding rationale.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_ALIF_HAL)

#include <stddef.h>
#include <stdint.h>

#include "Driver_USART.h"

extern ARM_DRIVER_USART        Driver_USART0;
extern ARM_DRIVER_USART        Driver_USART1;
extern ARM_DRIVER_USART        Driver_USART2;
extern ARM_DRIVER_USART        Driver_USART3;

static ARM_DRIVER_USART *const alp_alif_uart_drivers[] = {
    &Driver_USART0,
    &Driver_USART1,
    &Driver_USART2,
    &Driver_USART3,
};

static alp_status_t alif_to_alp(int32_t st)
{
    switch (st) {
    case ARM_DRIVER_OK:
        return ALP_OK;
    case ARM_DRIVER_ERROR_BUSY:
        return ALP_ERR_BUSY;
    case ARM_DRIVER_ERROR_TIMEOUT:
        return ALP_ERR_TIMEOUT;
    case ARM_DRIVER_ERROR_PARAMETER:
        return ALP_ERR_INVAL;
    case ARM_DRIVER_ERROR_UNSUPPORTED:
        return ALP_ERR_NOSUPPORT;
    default:
        return ALP_ERR_IO;
    }
}

alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)
{
    if (cfg == NULL) return NULL;
    if (cfg->port_id >= sizeof(alp_alif_uart_drivers) / sizeof(alp_alif_uart_drivers[0])) {
        return NULL;
    }
    ARM_DRIVER_USART *d = alp_alif_uart_drivers[cfg->port_id];
    if (d == NULL) return NULL;
    if (d->Initialize(NULL) != ARM_DRIVER_OK) return NULL;
    if (d->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) return NULL;

    uint32_t parity_bits = ARM_USART_PARITY_NONE;
    switch (cfg->parity) {
    case ALP_UART_PARITY_EVEN:
        parity_bits = ARM_USART_PARITY_EVEN;
        break;
    case ALP_UART_PARITY_ODD:
        parity_bits = ARM_USART_PARITY_ODD;
        break;
    default:
        parity_bits = ARM_USART_PARITY_NONE;
        break;
    }
    uint32_t stop_bits = (cfg->stop_bits == 2) ? ARM_USART_STOP_BITS_2 : ARM_USART_STOP_BITS_1;
    uint32_t data_bits = (cfg->data_bits == 7) ? ARM_USART_DATA_BITS_7 : ARM_USART_DATA_BITS_8;
    uint32_t mode      = ARM_USART_MODE_ASYNCHRONOUS;
    mode |= data_bits;
    mode |= parity_bits;
    mode |= stop_bits;
    mode |= ARM_USART_FLOW_CONTROL_NONE;
    if (d->Control(mode, cfg->baudrate) != ARM_DRIVER_OK) return NULL;
    (void)d->Control(ARM_USART_CONTROL_TX, 1);
    (void)d->Control(ARM_USART_CONTROL_RX, 1);
    return (alp_uart_t *)d;
}

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len)
{
    if (port == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_USART *)port)->Send(data, (uint32_t)len));
}

alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (port == NULL) return ALP_ERR_NOT_READY;
    (void)timeout_ms; /* CMSIS-Driver USART is async; v0.2.x adds a poll-with-timeout helper. */
    return alif_to_alp(((ARM_DRIVER_USART *)port)->Receive(data, (uint32_t)len));
}

void alp_uart_close(alp_uart_t *port)
{
    if (port == NULL) return;
    ARM_DRIVER_USART *d = (ARM_DRIVER_USART *)port;
    (void)d->Control(ARM_USART_CONTROL_TX, 0);
    (void)d->Control(ARM_USART_CONTROL_RX, 0);
    (void)d->PowerControl(ARM_POWER_OFF);
    (void)d->Uninitialize();
}

#endif /* ALP_HAS_ALIF_HAL */
