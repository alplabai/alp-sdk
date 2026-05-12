/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default HAL implementation: every operation returns
 * BRIDGE_HW_ERR_NOTIMPL.  The protocol layer translates these to
 * STATUS_IO / STATUS_NOSUPPORT on the wire so host code can
 * distinguish "firmware doesn't speak this opcode" (NOSUPPORT, from
 * the dispatcher) from "firmware speaks the opcode but the
 * underlying HW operation failed" (IO).
 *
 * The real implementation against the GigaDevice firmware library
 * lives in bridge_hw_gd32.c; the build picks one or the other via
 * BRIDGE_HAL_BACKEND in CMakeLists.txt.
 */

#include <stdint.h>

#include "bridge_hw.h"

uint8_t bridge_hw_reset_reason(void)
{
    return 0u; /* UNKNOWN */
}

int bridge_hw_gpio_read(uint32_t mask, uint32_t *levels)
{
    (void)mask;
    if (levels != 0) *levels = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_gpio_write(uint32_t mask, uint32_t levels)
{
    (void)mask; (void)levels;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_set(uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
    (void)channel; (void)period_ns; (void)duty_ns;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_pwm_get(uint8_t channel, uint32_t *period_ns, uint32_t *duty_ns)
{
    (void)channel;
    if (period_ns != 0) *period_ns = 0u;
    if (duty_ns != 0)   *duty_ns   = 0u;
    return BRIDGE_HW_ERR_NOTIMPL;
}

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv)
{
    (void)channel;
    for (uint8_t i = 0; i < samples && mv != 0; ++i) {
        mv[i] = 0u;
    }
    return BRIDGE_HW_ERR_NOTIMPL;
}

uint8_t bridge_hw_da9292_status_cached(void)
{
    return 0xFFu; /* "no PMIC poll has populated the cache yet" sentinel */
}
